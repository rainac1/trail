// 帧内鼠标尾迹全屏透明叠加层（视觉树版本）
//
// 单线程：一个窗口 + 消息循环 + 状态更新。
//
// 窗口只覆盖**主显示器**：其它显示器不画。按显示器拆窗口曾让 DWM 的合成路径变复杂
// （实测放视频时会把整个桌面拖慢），不如干脆只服务主屏。
//
// 尾迹不在像素里：每个采样点是一个 visual，全部共享同一张光标贴图 surface；运行期只改
// offset / visible，没有绘制、没有整屏拷贝、没有 Present。"一个采样点显示一帧然后消失"
// 变成"把它设成不可见"，于是擦除这个操作根本不存在。
//
// 时间基准：不需要 DWM 合成时钟，也不把帧对齐到某个 deadline。循环以固定频率
// （--commit-hz）tick 一次：取光标位置/新增历史、更新视觉树状态、需要时 Commit。
// DWM 在它自己的合成时刻读取视觉树 —— 我们不做"赶"，也就没有"错过"。
//
// 头部视觉只在 --hide-cursor 模式下绘制：系统光标可见时，它被硬件光标完全遮住，纯属多余。
#include <windows.h>
#include <wrl/client.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "cursor_texture.h"
#include "diag.h"
#include "mouse_history.h"
#include "overlay_renderer.h"

namespace {

constexpr UINT kQuitHotkeyId = 1;
constexpr int kMaxSamplesPerTick = 512;
const wchar_t* const kWindowClass = L"TrailOverlay";

OverlayRenderer g_renderer;
HCURSOR g_cursorHandle = nullptr;  // 已上传到渲染器的光标句柄（nullptr = 需要重抓）
bool g_hideCursor = false;
MouseHistoryTracker g_mouseHistory;  // 系统鼠标历史的增量读取状态
uint64_t g_nextCursorRetryTicks = 0;

HWND g_hwnd = nullptr;
RECT g_windowRect{};  // 主显示器矩形（虚拟屏幕坐标里的物理像素）
UINT g_windowDpi = 96;

// ---- 设备丢失状态（消息、更新都在主线程，无需同步）----
// 本窗口没有任何 GDI 内容，可见像素 100% 来自 DWM 合成的 DirectComposition 视觉树，
// 因此设备一旦丢失叠加层会整体变透明且不会自愈 —— 只能重建整套设备。
bool g_deviceLost = false;
bool g_tearingDownWindow = false;  // 主动销毁窗口时置位，避免 WM_DESTROY 触发退出

uint64_t g_qpcFreq = 0;    // QPC 频率（只读，启动时缓存）
HANDLE g_timer = nullptr;  // 高分辨率可等待定时器（用于 tick 节流）
HINSTANCE g_instance = nullptr;

uint64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

double TicksToMs(uint64_t ticks) {
  return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_qpcFreq);
}

double TicksToUs(uint64_t ticks) {
  return static_cast<double>(ticks) * 1000000.0 / static_cast<double>(g_qpcFreq);
}

// 等到 QPC 时刻 target。tick 节流不需要微秒级精度，所以这里不忙等：
// 只用高分辨率可等待定时器（Windows 10 1803+），拿不到就退回 Sleep。
void SleepUntil(uint64_t target) {
  for (;;) {
    const uint64_t now = QpcNow();
    if (now >= target) return;
    if (!g_timer) {
      const uint64_t remainMs = (target - now) * 1000ULL / g_qpcFreq;
      Sleep(static_cast<DWORD>(remainMs > 0 ? remainMs : 1));
      continue;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>((target - now) * 10000000ULL / g_qpcFreq);
    if (due.QuadPart >= 0) due.QuadPart = -1;  // 负数 = 相对时间（100ns 单位）
    if (!SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
      Sleep(1);
      continue;
    }
    WaitForSingleObject(g_timer, INFINITE);
  }
}

void PrintUsage() {
  wprintf(
      L"Trail\n"
      L"Usage: trail.exe [options]\n"
      L"  --commit-hz <hz>   tick rate cap (visual-tree commits per second, default 1000)\n"
      L"  --trail-ms <ms>    trail lifetime in milliseconds (default 8.0)\n"
      L"  --sprites <n>      max concurrently visible sample sprites (default 64)\n"
      L"  --hide-cursor      hide the system cursor (within this overlay window)\n"
      L"  --help             show this help\n"
      L"Quit: Ctrl+Alt+Q\n");
}

// 主显示器的物理像素矩形（per-monitor DPI 感知下即真实像素）。
RECT PrimaryMonitorRect() {
  const POINT origin{0, 0};
  const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info)) return info.rcMonitor;
  const RECT fallback{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  return fallback;
}

UINT WindowDpi(HWND hwnd) {
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  static const auto fn = reinterpret_cast<GetDpiForWindowFn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (fn) {
    const UINT dpi = fn(hwnd);
    if (dpi) return dpi;
  }
  return 96;
}

// 创建覆盖主显示器矩形的窗口。**先不显示** —— 没有内容的 layered 窗口会露出黑底，
// 必须等渲染栈把视觉树挂上去之后再 ShowWindow。
HWND CreateOverlayWindow(const RECT& rect) {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return nullptr;

  // 点击穿透：WS_EX_LAYERED | WS_EX_TRANSPARENT 组合使整个窗口对鼠标命中测试透明。
  // layered 窗口的命中基于形状/透明度，加 WS_EX_TRANSPARENT 后形状被忽略、鼠标事件传给
  // 下层窗口——这是唯一可靠的跨进程穿透机制（HTTRANSPARENT 只对同线程兄弟窗口生效）。
  HWND hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
      kWindowClass, L"Trail  (Ctrl+Alt+Q 退出)", WS_POPUP, rect.left, rect.top, width, height,
      nullptr, nullptr, g_instance, nullptr);
  if (!hwnd) return nullptr;

  // alpha=255：窗口内容不透明度不变（视觉树自带逐像素 alpha），仅让 layered 样式
  // 生效以启用命中穿透。
  SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
  SetWindowPos(hwnd, HWND_TOPMOST, rect.left, rect.top, width, height,
               SWP_NOACTIVATE | SWP_NOREDRAW);
  return hwnd;
}

// 用现有窗口重建渲染栈（设备丢失恢复）。窗口/几何/DPI 都不变。
bool InitRenderer(int maxSprites, double trailMs) {
  g_renderer.Shutdown();
  g_cursorHandle = nullptr;
  g_nextCursorRetryTicks = 0;
  if (!g_renderer.Initialize(g_hwnd, g_windowRect, g_windowDpi, maxSprites)) return false;
  g_renderer.SetTrailLifetimeMs(trailMs);
  // 新的视觉树没有任何内容，历史水印也作废：下一次 tick 只建立水印。
  g_mouseHistory.haveWatermark = false;
  return true;
}

// 启动路径：建窗口 -> 建渲染栈 -> 最后才让窗口可见。
bool CreateOverlay(int maxSprites, double trailMs) {
  g_windowRect = PrimaryMonitorRect();
  g_hwnd = CreateOverlayWindow(g_windowRect);
  if (!g_hwnd) {
    DiagLog(L"[main] CreateWindowExW failed (error=%lu)", GetLastError());
    return false;
  }
  g_windowDpi = WindowDpi(g_hwnd);
  if (!InitRenderer(maxSprites, trailMs)) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    return false;
  }
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
  RegisterHotKey(g_hwnd, kQuitHotkeyId, MOD_CONTROL | MOD_ALT, 'Q');
  DiagLog(L"[main] window %d,%d %dx%d dpi=%u hwnd=%p", static_cast<int>(g_windowRect.left),
          static_cast<int>(g_windowRect.top),
          static_cast<int>(g_windowRect.right - g_windowRect.left),
          static_cast<int>(g_windowRect.bottom - g_windowRect.top), g_windowDpi,
          static_cast<void*>(g_hwnd));
  return true;
}

// 顶层样式是叠加层可见的前提：窗口被压到其他窗口之下就等于渲染消失。仅在样式确实
// 丢失时才重新置顶，避免周期性抢 z-order。
void EnsureTopmost(HWND hwnd) {
  if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) return;
  DiagLog(L"[watch] WS_EX_TOPMOST lost -> re-asserting");
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // 点击穿透到下层窗口
    case WM_PAINT:
      // DirectComposition 在底层 DXGI 设备丢失时会向合成其内容的窗口发送 WM_PAINT。
      // 此处确认设备状态，失效则交由主循环重建整套设备与内容。
      ValidateRect(hwnd, nullptr);
      if (g_renderer.ready() && !g_renderer.DeviceValid()) g_deviceLost = true;
      return 0;
    case WM_DISPLAYCHANGE:
      DiagLog(L"[watch] display topology changed (restart trail to adapt)");
      return 0;
    case WM_DPICHANGED:
      DiagLog(L"[watch] display DPI changed (restart trail to adapt)");
      return 0;
    case WM_SETCURSOR:
      if (g_hideCursor) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    case WM_HOTKEY:
      PostQuitMessage(0);
      return 0;
    case WM_ERASEBKGND:
      return 1;  // 无背景，避免闪烁
    case WM_DESTROY:
      if (!g_tearingDownWindow) PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrev*/, PWSTR /*cmdLine*/, int /*show*/) {
  // Per-monitor DPI 感知（v2）：GetCursorInfo / GetMouseMovePointsEx / 窗口矩形都使用
  // 物理像素，和视觉树 offset 同一坐标空间。
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  auto setDpiAwarenessContext =
      reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(GetProcAddress(
          user32, "SetProcessDpiAwarenessContext"));
  if (setDpiAwarenessContext) {
    setDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  } else {
    SetProcessDPIAware();
  }

  DiagInit();

  LARGE_INTEGER freq{};
  if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
    wchar_t msg[256];
    swprintf_s(msg, L"QueryPerformanceFrequency 失败。\n\n诊断日志: %ls", DiagLogPath());
    DiagFatal(L"Trail", msg);
    return 1;
  }
  g_qpcFreq = static_cast<uint64_t>(freq.QuadPart);
  g_instance = hInstance;

  // ---- 命令行参数 ----
  double commitHz = 1000.0;
  double trailMs = 8.0;
  int maxSprites = 64;
  for (int i = 1; i < __argc; ++i) {
    const wchar_t* a = __wargv[i];
    const wchar_t* value = (i + 1 < __argc) ? __wargv[i + 1] : nullptr;
    if (wcscmp(a, L"--hide-cursor") == 0) {
      g_hideCursor = true;
    } else if (wcscmp(a, L"--help") == 0) {
      PrintUsage();
      return 0;
    } else if (wcscmp(a, L"--commit-hz") == 0 && value) {
      commitHz = wcstod(value, nullptr);
      ++i;
    } else if (wcscmp(a, L"--trail-ms") == 0 && value) {
      trailMs = wcstod(value, nullptr);
      ++i;
    } else if (wcscmp(a, L"--sprites") == 0 && value) {
      maxSprites = static_cast<int>(wcstol(value, nullptr, 10));
      ++i;
    }
  }
  if (!(commitHz >= 1.0)) commitHz = 1.0;
  if (commitHz > 10000.0) commitHz = 10000.0;
  if (!(trailMs >= 0.5)) trailMs = 0.5;
  if (trailMs > 5000.0) trailMs = 5000.0;
  if (maxSprites < 1) maxSprites = 1;
  if (maxSprites > 512) maxSprites = 512;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hCursor = nullptr;
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&wc)) {
    const DWORD err = GetLastError();
    wchar_t msg[256];
    swprintf_s(msg, L"RegisterClassExW 失败 (error=%lu)。\n\n诊断日志: %ls", err, DiagLogPath());
    DiagFatal(L"Trail", msg);
    return 1;
  }

  if (!CreateOverlay(maxSprites, trailMs)) {
    wchar_t msg[512];
    swprintf_s(msg,
               L"叠加层初始化失败：需要硬件 D3D11 设备、可用的 DirectComposition 与 DWM。\n"
               L"（不降级 WARP）。请查看上方/日志中的具体失败步骤。\n\n诊断日志: %ls",
               DiagLogPath());
    DiagFatal(L"Trail", msg);
    return 1;
  }
  DiagLog(L"[main] renderer ready: commit-hz=%.0f (tick %.3f ms), trail=%.1f ms, sprites=%d, "
          L"head=%ls",
          commitHz, 1000.0 / commitHz, trailMs, maxSprites,
          g_hideCursor ? L"overlay" : L"system");

  // tick 节流用的高分辨率定时器（Windows 10 1803+）。拿不到就退回 Sleep。
  g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
  if (!g_timer) {
    DiagLog(L"[main] high-resolution timer unavailable (error=%lu), falling back to Sleep",
            GetLastError());
  }

  // 提升更新线程优先级：Commit 若被抢占到合成之后，画面会旧一拍。用 HIGHEST 而非
  // TIME_CRITICAL，避免抢占 DWM / 游戏线程。
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

  const uint64_t tickStep = [&]() {
    const double step = static_cast<double>(g_qpcFreq) / commitHz;
    const uint64_t v = static_cast<uint64_t>(step);
    return v > 0 ? v : 1;
  }();

  MSG msg{};
  bool running = true;
  int lastX = INT_MIN, lastY = INT_MIN;  // 首次 tick / 光标重新出现时强制取一次历史
  int recreateBackoffMs = 500;
  uint64_t nextRecreateTicks = 0;
  uint64_t lastWatchTicks = QpcNow();

  // 统计窗口（每 2 秒一行）：实际 tick 率、真实提交率、update / Commit 的耗时。
  uint64_t lastStatTicks = QpcNow();
  uint64_t statTicks = 0, statCommits = 0, statSamples = 0;
  double statUpdateUsSum = 0.0, statUpdateUsMax = 0.0;
  double statCommitUsSum = 0.0, statCommitUsMax = 0.0;

  uint64_t nextTick = QpcNow();

  while (running) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        running = false;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!running) break;

    // ---- 周期性健康检查（≈每 500ms）：置顶样式 + 设备有效性 ----
    const uint64_t nowWatch = QpcNow();
    if (nowWatch - lastWatchTicks >= g_qpcFreq / 2) {
      lastWatchTicks = nowWatch;
      EnsureTopmost(g_hwnd);
      if (g_renderer.ready() && !g_renderer.DeviceValid()) g_deviceLost = true;
    }

    // ---- 设备丢失恢复：用现有窗口重建渲染栈 ----
    if (g_deviceLost) {
      const uint64_t nowTicks = QpcNow();
      if (nextRecreateTicks != 0 && nowTicks < nextRecreateTicks) {
        const uint64_t remainTicks = nextRecreateTicks - nowTicks;
        DWORD ms = static_cast<DWORD>(remainTicks * 1000ULL / g_qpcFreq);
        if (ms > 50) ms = 50;
        Sleep(ms > 0 ? ms : 1);
        continue;
      }
      DiagLog(L"[recover] rebuilding after device loss");
      if (InitRenderer(maxSprites, trailMs)) {
        DiagLog(L"[recover] renderer rebuilt");
        g_deviceLost = false;
        nextRecreateTicks = 0;
        recreateBackoffMs = 500;
        lastX = lastY = INT_MIN;
      } else {
        DiagLog(L"[recover] rebuild failed (will retry in %d ms)", recreateBackoffMs);
        nextRecreateTicks =
            nowTicks + static_cast<uint64_t>(recreateBackoffMs) * g_qpcFreq / 1000ULL;
        recreateBackoffMs = (recreateBackoffMs >= 8000) ? 15000 : recreateBackoffMs * 2;
        continue;
      }
    }
    if (!g_renderer.ready()) {
      g_deviceLost = true;
      continue;
    }

    // ---- 本 tick：采样并更新视觉树状态 ----
    CURSORINFO ci{sizeof(ci)};
    const bool gotCursor = GetCursorInfo(&ci) != FALSE;
    if (!gotCursor || (ci.flags & CURSOR_SHOWING) == 0) {
      // 光标不可见（全屏视频、锁屏等）：叠加层不该留下任何尾迹。
      g_renderer.HideAll();
      lastX = lastY = INT_MIN;
      g_mouseHistory.haveWatermark = false;
    } else {
      // 光标贴图：仅在句柄变化时重新抓取并上传（形状变化很罕见）。失败时 100ms 后再试。
      if (ci.hCursor != g_cursorHandle && QpcNow() >= g_nextCursorRetryTicks) {
        CursorBitmap bitmap;
        if (CaptureCursorBitmap(ci.hCursor, bitmap) && g_renderer.SetCursorBitmap(bitmap)) {
          g_cursorHandle = ci.hCursor;
        } else {
          g_cursorHandle = nullptr;
          g_nextCursorRetryTicks = QpcNow() + g_qpcFreq / 10;
        }
      }

      // 只在光标位置变化时查历史：鼠标静止时 GetMouseMovePointsEx 不会返回新点，
      // 这一调用是热路径上最贵的一次内核往返。
      Sample samples[kMaxSamplesPerTick];
      uint32_t sampleCount = 0;
      if (ci.ptScreenPos.x != lastX || ci.ptScreenPos.y != lastY) {
        sampleCount = static_cast<uint32_t>(CollectMouseHistory(
            samples, kMaxSamplesPerTick, g_mouseHistory, ci.ptScreenPos.x, ci.ptScreenPos.y));
        lastX = ci.ptScreenPos.x;
        lastY = ci.ptScreenPos.y;
      }

      bool committed = false;
      const bool showHead = g_hideCursor;
      const uint64_t t0 = QpcNow();
      const OverlayRenderer::FrameResult fr =
          g_renderer.Update(samples, sampleCount, ci.ptScreenPos.x, ci.ptScreenPos.y, showHead,
                            &committed);
      const uint64_t t1 = QpcNow();
      const double updateUs = TicksToUs(t1 - t0);
      statUpdateUsSum += updateUs;
      if (updateUs > statUpdateUsMax) statUpdateUsMax = updateUs;
      statSamples += sampleCount;
      if (committed) {
        ++statCommits;
        const double commitUs = TicksToUs(g_renderer.lastCommitTicks());
        statCommitUsSum += commitUs;
        if (commitUs > statCommitUsMax) statCommitUsMax = commitUs;
      }
      if (fr == OverlayRenderer::FrameResult::RecreateDevice) g_deviceLost = true;
    }
    ++statTicks;

    // ---- 统计 ----
    const uint64_t statNow = QpcNow();
    if (statNow - lastStatTicks >= g_qpcFreq * 2) {
      const double secs = TicksToMs(statNow - lastStatTicks) / 1000.0;
      if (secs > 0.0 && statTicks > 0) {
        const double commits = static_cast<double>(statCommits);
        DiagLog(L"[update] ticks=%.1f/s commits=%.1f/s samples=%.1f/s live=%d | "
                L"update mean=%.1f max=%.1f us | commit mean=%.1f max=%.1f us | "
                L"tick step=%.3f ms",
                static_cast<double>(statTicks) / secs, commits / secs,
                static_cast<double>(statSamples) / secs, g_renderer.liveSprites(),
                statUpdateUsSum / static_cast<double>(statTicks), statUpdateUsMax,
                commits > 0.0 ? statCommitUsSum / commits : 0.0, statCommitUsMax,
                TicksToMs(tickStep));
      }
      lastStatTicks = statNow;
      statTicks = statCommits = statSamples = 0;
      statUpdateUsSum = statUpdateUsMax = 0.0;
      statCommitUsSum = statCommitUsMax = 0.0;
    }

    // ---- 节流到 commitHz ----
    nextTick += tickStep;
    const uint64_t nowT = QpcNow();
    if (nextTick > nowT) {
      SleepUntil(nextTick);
    } else {
      nextTick = nowT;
    }
  }

  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
  if (g_timer) {
    CloseHandle(g_timer);
    g_timer = nullptr;
  }

  // 清理
  if (g_hwnd) {
    UnregisterHotKey(g_hwnd, kQuitHotkeyId);
    g_tearingDownWindow = true;
    ShowWindow(g_hwnd, SW_HIDE);
    g_renderer.Shutdown();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  DiagClose();
  return 0;
}
