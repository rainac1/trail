// 事件驱动的鼠标尾迹全屏透明叠加层（WH_MOUSE_LL + DirectComposition 视觉树）
//
// 单线程：一个窗口 + 消息循环 + 一个全局低级鼠标钩子。没有渲染 tick、没有轮询采样：
// 每个 WM_MOUSEMOVE 钩子回调就是一个采样点，视觉树的更新与 Commit 都在回调里完成。
// DWM 在它自己的合成时刻读取视觉树 —— 我们不做"赶"，也就没有"错过"。
//
// 窗口只覆盖**主显示器**：其它显示器不画。按显示器拆窗口曾让 DWM 的合成路径变复杂
// （实测放视频时会把整个桌面拖慢），不如干脆只服务主屏。
//
// 尾迹不在像素里：每个采样点是一个 visual，全部共享同一张光标贴图 surface；运行期只改
// offset / visible，没有绘制、没有整屏拷贝。"一个采样点显示一帧然后消失"变成"把它设成
// 不可见"，于是擦除这个操作根本不存在。
//
// 采样点过期不能靠"下一次鼠标事件"（鼠标停住就没有事件了），所以主循环等待一个
// 高分辨率可等待定时器：它被 arm 到"最早一个可见采样点的到期时刻"。这是事件驱动的
// 超时，不是 tick —— 没有可见采样点时不会为到期 arm 定时器（它仍服务看门狗和统计）。
//
// 500ms 一次的健康检查只负责看门狗职责：置顶样式、设备有效性，以及在没有鼠标事件时
// 对账光标可见性/位置（ShowCursor / SetCursorPos 不会触发鼠标钩子）。
#include <windows.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "cursor_texture.h"
#include "diag.h"
#include "mouse_sample.h"
#include "overlay_renderer.h"

namespace {

constexpr UINT kQuitHotkeyId = 1;
constexpr UINT kHealthCheckMs = 500;
constexpr UINT kStatsIntervalMs = 2000;
constexpr UINT kCursorRetryMs = 100;
constexpr UINT kRecreateInitialBackoffMs = 500;
constexpr UINT kRecreateMaxBackoffMs = 15000;
const wchar_t* const kWindowClass = L"TrailOverlay";

OverlayRenderer g_renderer;
HCURSOR g_cursorHandle = nullptr;  // 已上传到渲染器的光标句柄（nullptr = 需要重抓）
uint64_t g_nextCursorRetryTicks = 0;
bool g_cursorCaptureFailureLogged = false;

// ---- 输入状态（只由主线程的钩子回调/看门狗访问，无需同步）----
int g_lastCursorX = INT_MIN;  // 上一次喂给视觉树的位置
int g_lastCursorY = INT_MIN;
bool g_haveLastCursor = false;
bool g_cursorVisible = false;  // 上一次观察到的系统光标可见性

HWND g_hwnd = nullptr;
RECT g_windowRect{};  // 主显示器矩形（虚拟屏幕坐标里的物理像素）
UINT g_windowDpi = 96;

HHOOK g_mouseHook = nullptr;
bool g_mouseHookReady = false;  // 钩子已安装且允许回调改渲染状态
bool g_inMouseHook = false;     // 重入保护：回调内部不允许再次进入

// ---- 设备丢失状态（消息、更新都在主线程，无需同步）----
// 本窗口没有任何 GDI 内容，可见像素 100% 来自 DWM 合成的 DirectComposition 视觉树，
// 因此设备一旦丢失叠加层会整体变透明且不会自愈 —— 只能重建整套设备。
bool g_deviceLost = false;
bool g_tearingDownWindow = false;  // 主动销毁窗口时置位，避免 WM_DESTROY 触发退出

uint64_t g_qpcFreq = 0;    // QPC 频率（只读，启动时缓存）
HANDLE g_timer = nullptr;  // 高分辨率可等待定时器（用于采样点到期，不做 tick）
HINSTANCE g_instance = nullptr;

// ---- 每 2 秒一行统计（与钩子同线程，无需同步）----
uint64_t g_statWindowStart = 0;
uint64_t g_statEvents = 0;   // WM_MOUSEMOVE 钩子回调次数
uint64_t g_statSamples = 0;  // 实际新增的采样点（去重后）
uint64_t g_statCommits = 0;
double g_statHookUsSum = 0.0, g_statHookUsMax = 0.0;
double g_statCommitUsSum = 0.0, g_statCommitUsMax = 0.0;

uint64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

double TicksToUs(uint64_t ticks) {
  return static_cast<double>(ticks) * 1000000.0 / static_cast<double>(g_qpcFreq);
}

uint64_t MsToTicks(uint64_t ms) {
  return g_qpcFreq * ms / 1000ULL;
}

void PrintUsage() {
  wprintf(
      L"Trail\n"
      L"Usage: trail.exe [options]\n"
      L"  --trail-ms <ms>    trail lifetime in milliseconds (default 8.0)\n"
      L"  --sprites <n>      max concurrently visible sample sprites (default 64)\n"
      L"  --help             show this help\n"
      L"Quit: Ctrl+Alt+Q\n"
      L"Commits are driven by the WH_MOUSE_LL mouse hook (one per input event); there is no tick rate.\n");
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

  // 新的视觉树没有任何内容，旧的贴图/采样点位置全部作废。
  g_cursorHandle = nullptr;
  g_nextCursorRetryTicks = 0;
  g_cursorCaptureFailureLogged = false;
  g_cursorVisible = false;
  g_haveLastCursor = false;
  g_lastCursorX = INT_MIN;
  g_lastCursorY = INT_MIN;

  if (!g_renderer.Initialize(g_hwnd, g_windowRect, g_windowDpi, maxSprites)) return false;
  g_renderer.SetTrailLifetimeMs(trailMs);
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

void RecordCommit() {
  ++g_statCommits;
  const double us = TicksToUs(g_renderer.lastCommitTicks());
  g_statCommitUsSum += us;
  if (us > g_statCommitUsMax) g_statCommitUsMax = us;
}

// 光标不可见（或被应用隐藏）：叠加层不该留下任何尾迹。
void HideCursorVisuals() {
  if (g_cursorVisible || g_renderer.liveSprites() > 0) g_renderer.HideAll();
  g_cursorVisible = false;
  g_haveLastCursor = false;
  g_lastCursorX = INT_MIN;
  g_lastCursorY = INT_MIN;
}

// 应用一次"光标位于 (x, y)"：维护光标贴图缓存并新增一个采样点。
// 返回本次新增的采样点数（0 或 1）。调用方必须已确认渲染器 ready 且设备未丢。
//
// 位置为什么由调用方给出：低级鼠标钩子在事件投递给目标线程**之前**执行，
// GetCursorInfo 里的位置可能还是上一个事件的位置；WM_MOUSEMOVE 的 MSLLHOOKSTRUCT.pt
// 才是这个事件对应的新位置。看门狗路径则直接使用 GetCursorInfo 的位置。
int ApplyCursorSample(const CURSORINFO& ci, int x, int y) {
  if ((ci.flags & CURSOR_SHOWING) == 0 || !ci.hCursor) {
    HideCursorVisuals();
    return 0;
  }
  g_cursorVisible = true;

  // 光标贴图：仅在句柄变化时重新抓取并上传（形状变化很罕见）。失败时 100ms 后再试。
  if (ci.hCursor != g_cursorHandle && QpcNow() >= g_nextCursorRetryTicks) {
    CursorBitmap bitmap;
    const bool captured = CaptureCursorBitmap(ci.hCursor, bitmap);
    if (captured && g_renderer.SetCursorBitmap(bitmap)) {
      g_cursorHandle = ci.hCursor;
      g_nextCursorRetryTicks = 0;
      g_cursorCaptureFailureLogged = false;
      // SetCursorBitmap 重建了视觉树内容，强制下一次采样重画。
      g_haveLastCursor = false;
    } else {
      if (!g_cursorCaptureFailureLogged) {
        g_cursorCaptureFailureLogged = true;
        DiagLog(L"[cursor] %ls failed for handle %p; retrying every %u ms",
                captured ? L"sprite upload" : L"sprite capture",
                reinterpret_cast<void*>(ci.hCursor), kCursorRetryMs);
      }
      g_cursorHandle = nullptr;
      g_nextCursorRetryTicks = QpcNow() + MsToTicks(kCursorRetryMs);
      if (!g_renderer.DeviceValid()) g_deviceLost = true;
    }
  }

  // 同一像素上的重复事件不占槽位、不 Commit（高报告率鼠标在边缘夹紧时会这样）。
  if (g_haveLastCursor && x == g_lastCursorX && y == g_lastCursorY) return 0;

  const Sample sample{static_cast<int32_t>(x), static_cast<int32_t>(y)};
  bool committed = false;
  const OverlayRenderer::FrameResult fr = g_renderer.Update(&sample, 1, &committed);
  if (committed) RecordCommit();
  if (fr == OverlayRenderer::FrameResult::RecreateDevice) g_deviceLost = true;

  g_lastCursorX = x;
  g_lastCursorY = y;
  g_haveLastCursor = true;
  return 1;
}

// 全局低级鼠标钩子：每个鼠标输入事件都会在主线程（安装钩子的线程）回调一次。
//
// 这里直接做视觉树更新与 Commit —— 这正是本架构的目的：采样与提交同源，没有"轮询间隔"
// 造成的采样丢失。代价是回调必须短：系统对低级钩子有 LowLevelHooksTimeout（默认 300ms），
// 超时会让输入卡住，Windows 7+ 还会静默摘掉钩子。正常的 Update + Commit 是微秒级；
// 若 DWM 反压导致 Commit 阻塞，输入也会跟着被阻塞（见 README 的已知限制）。
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode != HC_ACTION || wParam != WM_MOUSEMOVE || !g_mouseHookReady || g_inMouseHook) {
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
  }

  g_inMouseHook = true;
  const uint64_t t0 = QpcNow();

  const MSLLHOOKSTRUCT* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
  const int x = static_cast<int>(info->pt.x);
  const int y = static_cast<int>(info->pt.y);
  ++g_statEvents;

  if (g_renderer.ready() && !g_deviceLost) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) {
      HideCursorVisuals();
    } else {
      g_statSamples += static_cast<uint64_t>(ApplyCursorSample(ci, x, y));
    }
  }

  const double hookUs = TicksToUs(QpcNow() - t0);
  g_statHookUsSum += hookUs;
  if (hookUs > g_statHookUsMax) g_statHookUsMax = hookUs;

  g_inMouseHook = false;
  return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

bool InstallMouseHook() {
  g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_instance, 0);
  if (!g_mouseHook) {
    DiagLog(L"[main] SetWindowsHookExW(WH_MOUSE_LL) failed (error=%lu)", GetLastError());
    return false;
  }
  g_mouseHookReady = true;
  DiagLog(L"[main] WH_MOUSE_LL installed");
  return true;
}

// 没有鼠标事件时对账光标状态：应用调用 ShowCursor / SetCursorPos 不会产生鼠标钩子。
// 第一次对账（启动后还没有任何鼠标事件）只建立位置基准，不凭空产生采样点。
void ReconcileCursor() {
  if (!g_renderer.ready() || g_deviceLost) return;
  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci)) {
    HideCursorVisuals();
    return;
  }
  if (!g_haveLastCursor) {
    if ((ci.flags & CURSOR_SHOWING) == 0 || !ci.hCursor) {
      HideCursorVisuals();
      return;
    }
    g_cursorVisible = true;
    g_lastCursorX = ci.ptScreenPos.x;
    g_lastCursorY = ci.ptScreenPos.y;
    g_haveLastCursor = true;
    return;
  }
  g_statSamples +=
      static_cast<uint64_t>(ApplyCursorSample(ci, ci.ptScreenPos.x, ci.ptScreenPos.y));
}

// 把高分辨率定时器 arm 到绝对 QPC 时刻 deadline，并给出毫秒级兜底超时。
// 返回 false 表示只能靠 MsgWaitForMultipleObjectsEx 的毫秒超时（没有定时器）。
bool ArmTimer(uint64_t deadline, DWORD* timeoutMs) {
  const uint64_t now = QpcNow();
  const uint64_t remain = deadline > now ? deadline - now : 0;

  uint64_t ms = (remain * 1000ULL + g_qpcFreq - 1) / g_qpcFreq;
  if (ms == 0) ms = 1;
  if (ms > 0xFFFFFFFFULL) ms = 0xFFFFFFFFULL;
  *timeoutMs = static_cast<DWORD>(ms);

  if (!g_timer) return false;

  uint64_t rel100ns = remain * 10000000ULL / g_qpcFreq;
  if (rel100ns < 1) rel100ns = 1;  // 0 表示"绝对 1601 年"，即不触发
  LARGE_INTEGER due;
  due.QuadPart = -static_cast<LONGLONG>(rel100ns);

  static bool loggedFailure = false;
  if (!SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
    if (!loggedFailure) {
      loggedFailure = true;
      DiagLog(L"[main] SetWaitableTimerEx failed (error=%lu), falling back to polling timeout",
              GetLastError());
    }
    return false;
  }
  return true;
}

// 等到 deadline 或输入消息先到。低级鼠标钩子的回调在之后的 PeekMessage 里执行。
void WaitForWork(uint64_t deadline) {
  DWORD timeoutMs = 1;
  const bool timerArmed = ArmTimer(deadline, &timeoutMs);
  HANDLE timer = g_timer;
  const DWORD result =
      timerArmed
          ? MsgWaitForMultipleObjectsEx(1, &timer, timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
          : MsgWaitForMultipleObjectsEx(0, nullptr, timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  if (result == WAIT_FAILED) {
    DiagLog(L"[main] MsgWaitForMultipleObjectsEx failed (error=%lu)", GetLastError());
    Sleep(1);  // 防止错误路径忙转
  }
}

void LogStats(uint64_t now) {
  const double secs = static_cast<double>(now - g_statWindowStart) / static_cast<double>(g_qpcFreq);
  if (secs <= 0.0) return;

  const double events = static_cast<double>(g_statEvents);
  const double commits = static_cast<double>(g_statCommits);
  DiagLog(L"[update] events=%.1f/s samples=%.1f/s commits=%.1f/s live=%d | "
          L"hook mean=%.1f max=%.1f us | commit mean=%.1f max=%.1f us",
          events / secs, static_cast<double>(g_statSamples) / secs, commits / secs,
          g_renderer.liveSprites(), events > 0.0 ? g_statHookUsSum / events : 0.0,
          g_statHookUsMax, commits > 0.0 ? g_statCommitUsSum / commits : 0.0,
          g_statCommitUsMax);

  g_statWindowStart = now;
  g_statEvents = 0;
  g_statSamples = 0;
  g_statCommits = 0;
  g_statHookUsSum = g_statHookUsMax = 0.0;
  g_statCommitUsSum = g_statCommitUsMax = 0.0;
}

// 停止输入驱动与定时器，然后按定义好的顺序拆窗口/视觉树。
void ShutdownApp() {
  g_mouseHookReady = false;
  if (g_mouseHook) {
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
  }
  if (g_timer) {
    CloseHandle(g_timer);
    g_timer = nullptr;
  }
  if (g_hwnd) {
    UnregisterHotKey(g_hwnd, kQuitHotkeyId);
    g_tearingDownWindow = true;
    ShowWindow(g_hwnd, SW_HIDE);
    g_renderer.Shutdown();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
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
  // Per-monitor DPI 感知（v2）：GetCursorInfo / 钩子坐标 / 窗口矩形都使用物理像素，
  // 和视觉树 offset 同一坐标空间。
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
  double trailMs = 8.0;
  int maxSprites = 64;
  for (int i = 1; i < __argc; ++i) {
    const wchar_t* a = __wargv[i];
    const wchar_t* value = (i + 1 < __argc) ? __wargv[i + 1] : nullptr;
    if (wcscmp(a, L"--help") == 0) {
      PrintUsage();
      return 0;
    } else if (wcscmp(a, L"--trail-ms") == 0 && value) {
      trailMs = wcstod(value, nullptr);
      ++i;
    } else if (wcscmp(a, L"--sprites") == 0 && value) {
      maxSprites = static_cast<int>(wcstol(value, nullptr, 10));
      ++i;
    }
  }
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
               L"请查看上方/日志中的具体失败步骤。\n\n诊断日志: %ls",
               DiagLogPath());
    DiagFatal(L"Trail", msg);
    return 1;
  }
  DiagLog(L"[main] renderer ready: trail=%.1f ms, sprites=%d, input=WH_MOUSE_LL", trailMs,
          maxSprites);

  // 采样点到期 / 看门狗用的高分辨率定时器（Windows 10 1803+）。拿不到就退回毫秒超时。
  g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
  if (!g_timer) {
    DiagLog(L"[main] high-resolution timer unavailable (error=%lu), falling back to ms timeout",
            GetLastError());
  }

  // 钩子回调在主线程执行：提高优先级让采样→Commit 更少被抢占。用 HIGHEST 而非
  // TIME_CRITICAL，避免抢占 DWM / 游戏线程。
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

  if (!InstallMouseHook()) {
    wchar_t msg[512];
    swprintf_s(msg,
               L"WH_MOUSE_LL 低级鼠标钩子安装失败 (error=%lu)。\n"
               L"没有钩子就没有采样点，程序无法工作。\n\n诊断日志: %ls",
               GetLastError(), DiagLogPath());
    DiagFatal(L"Trail", msg);
    ShutdownApp();
    DiagClose();
    return 1;
  }

  const uint64_t loopStart = QpcNow();
  g_statWindowStart = loopStart;
  uint64_t nextHealth = loopStart + MsToTicks(kHealthCheckMs);
  uint64_t nextStats = loopStart + MsToTicks(kStatsIntervalMs);
  uint64_t nextRecreate = 0;
  UINT recreateBackoffMs = kRecreateInitialBackoffMs;

  MSG msg{};
  bool running = true;
  while (running) {
    // 泵消息。WH_MOUSE_LL 的回调就在这里被系统调用，视觉树更新也在此时发生。
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        running = false;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!running) break;

    const uint64_t now = QpcNow();

    // ---- 采样点到期：定时器把我们从等待中叫醒，隐藏已经过期的槽位 ----
    if (g_renderer.ready()) {
      const uint64_t expiry = g_renderer.NextExpiryQpc();
      if (expiry != 0 && now >= expiry) {
        bool committed = false;
        const OverlayRenderer::FrameResult fr = g_renderer.Expire(now, &committed);
        if (committed) RecordCommit();
        if (fr == OverlayRenderer::FrameResult::RecreateDevice) g_deviceLost = true;
      }
    }

    // ---- 设备丢失恢复：用现有窗口重建渲染栈 ----
    if (g_deviceLost) {
      if (nextRecreate == 0 || now >= nextRecreate) {
        DiagLog(L"[recover] rebuilding after device loss");
        if (InitRenderer(maxSprites, trailMs)) {
          DiagLog(L"[recover] renderer rebuilt");
          g_deviceLost = false;
          nextRecreate = 0;
          recreateBackoffMs = kRecreateInitialBackoffMs;
          g_haveLastCursor = false;  // 下一次事件重新采样
        } else {
          DiagLog(L"[recover] rebuild failed (will retry in %u ms)", recreateBackoffMs);
          nextRecreate = now + MsToTicks(recreateBackoffMs);
          recreateBackoffMs =
              (recreateBackoffMs >= kRecreateMaxBackoffMs) ? 15000 : recreateBackoffMs * 2;
        }
      }
    }
    if (!g_renderer.ready()) g_deviceLost = true;

    // ---- 健康检查（看门狗，不是渲染节拍）----
    const uint64_t nowHealth = QpcNow();
    if (nowHealth >= nextHealth) {
      nextHealth = nowHealth + MsToTicks(kHealthCheckMs);
      EnsureTopmost(g_hwnd);
      if (g_renderer.ready() && !g_renderer.DeviceValid()) g_deviceLost = true;
      ReconcileCursor();
    }

    // ---- 统计 ----
    const uint64_t nowStats = QpcNow();
    if (nowStats >= nextStats) {
      LogStats(nowStats);
      nextStats = nowStats + MsToTicks(kStatsIntervalMs);
    }

    // ---- 计算下一次唤醒时刻：最早到期 / 健康检查 / 统计 / 重建重试 ----
    uint64_t deadline = nextHealth < nextStats ? nextHealth : nextStats;
    if (g_deviceLost && nextRecreate != 0 && nextRecreate < deadline) deadline = nextRecreate;
    if (g_renderer.ready()) {
      const uint64_t expiry = g_renderer.NextExpiryQpc();
      if (expiry != 0 && expiry < deadline) deadline = expiry;
    }
    WaitForWork(deadline);
  }

  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
  ShutdownApp();
  DiagClose();
  return 0;
}
