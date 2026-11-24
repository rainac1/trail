// 事件驱动的鼠标尾迹全屏透明叠加层（WH_MOUSE_LL + DirectComposition 视觉树）
//
// 单线程：一个窗口 + 消息循环 + 一个全局低级鼠标钩子。没有渲染 tick、没有轮询采样：
// WH_MOUSE_LL 回调只把 (x, y, qpc) 写入定长环形缓冲并立刻放行，不在钩子内做任何
// GDI / DComp / Commit 调用；紧随 PeekMessageW 之后主循环立即批量更新视觉树并做一次
// Commit。DWM 在它自己的合成时刻读取视觉树 —— 我们不做"赶"，也就没有"错过"。
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
// 超时，不是 tick —— 仅在 deadline 变化或触发后才重新编程定时器。
//
// 500ms 一次的健康检查只负责看门狗职责：置顶样式、设备有效性，以及在没有鼠标事件时
// 对账光标可见性/位置（ShowCursor / SetCursorPos 不会触发鼠标钩子）。
#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "cursor_texture.h"
#include "diag.h"
#include "frame_guard.h"
#include "mouse_sample.h"
#include "overlay_renderer.h"

namespace {

constexpr UINT kQuitHotkeyId = 1;
constexpr UINT kHealthCheckMs = 500;
constexpr UINT kStatsIntervalMs = 2000;
constexpr UINT kCursorRetryMs = 100;
constexpr UINT kRecreateInitialBackoffMs = 500;
constexpr UINT kRecreateMaxBackoffMs = 15000;
constexpr uint32_t kPendingCapacity = 128;
const wchar_t* const kWindowClass = L"TrailOverlay";
const wchar_t* const kInstanceMutex = L"Local\\TrailOverlaySingleInstance";

enum class CreateOverlayStatus {
  Ok,
  HotkeyBusy,
  Failed,
};

OverlayRenderer g_renderer;
FrameGuard g_frameGuard;
HCURSOR g_cursorHandle = nullptr;  // 已上传到渲染器的光标句柄（nullptr = 需要重抓）
uint64_t g_nextCursorRetryTicks = 0;
bool g_cursorCaptureFailureLogged = false;

// ---- 输入状态（钩子与主循环同线程，无需锁）----
Sample g_pendingSamples[kPendingCapacity]{};
uint32_t g_pendingHead = 0;
uint32_t g_pendingCount = 0;

int g_lastCursorX = INT_MIN;  // 上一次喂给视觉树的位置
int g_lastCursorY = INT_MIN;
bool g_haveLastCursor = false;
bool g_cursorVisible = false;  // 上一次观察到的系统光标可见性

HWND g_hwnd = nullptr;
RECT g_windowRect{};  // 主显示器矩形（虚拟屏幕坐标里的物理像素）
UINT g_windowDpi = 96;

HHOOK g_mouseHook = nullptr;
bool g_mouseHookReady = false;  // 钩子已安装且允许写入待处理采样缓冲

// ---- 设备丢失状态（消息、更新都在主线程，无需同步）----
// 本窗口没有任何 GDI 内容，可见像素 100% 来自 DWM 合成的 DirectComposition 视觉树，
// 因此设备一旦丢失叠加层会整体变透明且不会自愈 —— 只能重建整套设备。
bool g_deviceLost = false;
bool g_tearingDownWindow = false;  // 主动销毁窗口时置位，避免 WM_DESTROY 触发退出

uint64_t g_qpcFreq = 0;        // QPC 频率（只读，启动时缓存）
HANDLE g_timer = nullptr;      // 高分辨率可等待定时器（用于采样点到期，不做 tick）
uint64_t g_armedDeadline = 0;  // 当前已在 g_timer 上设置且尚未触发的 deadline（0 = 未设置）
HINSTANCE g_instance = nullptr;

// ---- 可选统计（仅 --log 启用且有活动时每 2 秒输出一行）----
uint64_t g_statWindowStart = 0;
uint64_t g_statEvents = 0;   // WM_MOUSEMOVE 钩子回调次数
uint64_t g_statSamples = 0;  // 去重后的有效采样点数
uint64_t g_statCommits = 0;
double g_statCommitUsSum = 0.0;
double g_statCommitUsMax = 0.0;

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
  // GUI 子系统进程默认未绑定父控制台；交互式终端传入 --help 时附着以便输出可见。
  if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN &&
      AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
  }
  wprintf(
      L"Trail\n"
      L"Usage: trail.exe [options]\n"
      L"  --sprites <n>      max concurrently visible sample sprites (default 64)\n"
      L"  --log              write diagnostic log to trail.log next to trail.exe\n"
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
  const UINT dpi = GetDpiForWindow(hwnd);
  return dpi ? dpi : 96;
}

// 创建覆盖主显示器矩形的窗口。**先不显示** —— 等渲染栈把视觉树挂上去之后再 ShowWindow。
HWND CreateOverlayWindow(const RECT& rect) {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return nullptr;

  // WS_EX_NOREDIRECTIONBITMAP：纯 DirectComposition 窗口，不分配无用的全屏 GDI 重定向位图。
  // WS_EX_LAYERED | WS_EX_TRANSPARENT：跨进程点击穿透（命中测试完全忽略本窗口）。
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                  WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
                              kWindowClass, L"Trail  (Ctrl+Alt+Q 退出)", WS_POPUP, rect.left,
                              rect.top, width, height, nullptr, nullptr, g_instance, nullptr);
  if (!hwnd) return nullptr;

  // 带 WS_EX_LAYERED 时仍需初始化 layered 属性，否则 DWM 会视窗口为未初始化而隐藏内容。
  SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
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

  if (!g_renderer.Initialize(g_hwnd, g_windowRect, g_windowDpi, maxSprites, g_qpcFreq)) {
    return false;
  }
  g_renderer.SetTrailLifetimeMs(trailMs);
  return true;
}

// 启动路径：建窗口 -> 抢占退出热键 -> 建渲染栈 -> 最后才让窗口可见。
CreateOverlayStatus CreateOverlay(int maxSprites, double trailMs) {
  g_windowRect = PrimaryMonitorRect();
  g_hwnd = CreateOverlayWindow(g_windowRect);
  if (!g_hwnd) {
    DiagLog(L"[main] CreateWindowExW failed (error=%lu)", GetLastError());
    return CreateOverlayStatus::Failed;
  }

  // 先注册全局退出热键：被占用时直接失败退出，避免出现无法按快捷键退出的无界面常驻进程。
  if (!RegisterHotKey(g_hwnd, kQuitHotkeyId, MOD_CONTROL | MOD_ALT, 'Q')) {
    DiagLog(L"[main] RegisterHotKey(Ctrl+Alt+Q) failed (error=%lu)", GetLastError());
    g_tearingDownWindow = true;
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    g_tearingDownWindow = false;
    return CreateOverlayStatus::HotkeyBusy;
  }

  g_windowDpi = WindowDpi(g_hwnd);
  if (!InitRenderer(maxSprites, trailMs)) {
    UnregisterHotKey(g_hwnd, kQuitHotkeyId);
    g_tearingDownWindow = true;
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    g_tearingDownWindow = false;
    return CreateOverlayStatus::Failed;
  }

  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
  DiagLog(L"[main] window %d,%d %dx%d dpi=%u hwnd=%p", static_cast<int>(g_windowRect.left),
          static_cast<int>(g_windowRect.top),
          static_cast<int>(g_windowRect.right - g_windowRect.left),
          static_cast<int>(g_windowRect.bottom - g_windowRect.top), g_windowDpi,
          static_cast<void*>(g_hwnd));
  return CreateOverlayStatus::Ok;
}

// 顶层样式是叠加层可见的前提：窗口被压到其他窗口之下就等于渲染消失。仅在样式确实
// 丢失时才重新置顶，避免周期性抢 z-order。
void EnsureTopmost(HWND hwnd) {
  if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) return;
  DiagLog(L"[watch] WS_EX_TOPMOST lost -> re-asserting");
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void RecordCommit(uint64_t now) {
  ++g_statCommits;
  g_frameGuard.NotifyCommit(now);
  if (!DiagLogEnabled()) return;
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

// 光标贴图：仅在句柄变化时重新抓取并上传（形状变化罕见）。失败时 100ms 后再试。
void EnsureCursorBitmap(HCURSOR hCursor, uint64_t now) {
  if (hCursor == g_cursorHandle || now < g_nextCursorRetryTicks) return;

  CursorBitmap bitmap;
  const bool captured = CaptureCursorBitmap(hCursor, bitmap);
  if (captured && g_renderer.SetCursorBitmap(bitmap)) {
    g_cursorHandle = hCursor;
    g_nextCursorRetryTicks = 0;
    g_cursorCaptureFailureLogged = false;
    // SetCursorBitmap 隐藏了旧尾迹，允许当前位置重新采样。
    g_haveLastCursor = false;
  } else {
    if (!g_cursorCaptureFailureLogged) {
      g_cursorCaptureFailureLogged = true;
      DiagLog(L"[cursor] %ls failed for handle %p; retrying every %u ms",
              captured ? L"sprite upload" : L"sprite capture",
              reinterpret_cast<void*>(hCursor), kCursorRetryMs);
    }
    g_cursorHandle = nullptr;
    g_nextCursorRetryTicks = now + MsToTicks(kCursorRetryMs);
    if (!g_renderer.DeviceValid()) g_deviceLost = true;
  }
}

// 批量提交钩子攒下的采样点（单事件到达时就是 1 点，高回报率突发时自动合并为 1 次 Commit）。
void FlushPendingSamples(uint64_t now) {
  if (g_pendingCount == 0) return;

  Sample batch[kPendingCapacity];
  uint32_t rawCount = 0;
  while (g_pendingCount > 0) {
    batch[rawCount++] = g_pendingSamples[g_pendingHead];
    g_pendingHead = (g_pendingHead + 1) % kPendingCapacity;
    --g_pendingCount;
  }

  if (!g_renderer.ready() || g_deviceLost || g_frameGuard.paused()) return;

  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || (ci.flags & CURSOR_SHOWING) == 0 || !ci.hCursor) {
    HideCursorVisuals();
    return;
  }
  g_cursorVisible = true;
  EnsureCursorBitmap(ci.hCursor, now);

  // 同一像素上的连续重复事件不占槽位、不 Commit（高回报率鼠标在屏幕边缘夹紧时会如此）。
  Sample filtered[kPendingCapacity];
  uint32_t count = 0;
  for (uint32_t i = 0; i < rawCount; ++i) {
    if (g_haveLastCursor && batch[i].x == g_lastCursorX && batch[i].y == g_lastCursorY) {
      continue;
    }
    g_lastCursorX = batch[i].x;
    g_lastCursorY = batch[i].y;
    g_haveLastCursor = true;
    filtered[count++] = batch[i];
  }
  if (count == 0) return;

  bool committed = false;
  const OverlayRenderer::FrameResult fr = g_renderer.Update(filtered, count, now, &committed);
  if (committed) RecordCommit(now);
  if (fr == OverlayRenderer::FrameResult::RecreateDevice) g_deviceLost = true;
  g_statSamples += count;
}

// 全局低级鼠标钩子：只在主线程把坐标和时间戳入队并立刻放行，绝不在回调内做
// GDI / DComp / Commit，避免给系统输入叠加延迟或触发 LowLevelHooksTimeout。
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE && g_mouseHookReady) {
    const MSLLHOOKSTRUCT* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    const Sample s{static_cast<int32_t>(info->pt.x), static_cast<int32_t>(info->pt.y), QpcNow()};
    if (g_pendingCount < kPendingCapacity) {
      g_pendingSamples[(g_pendingHead + g_pendingCount) % kPendingCapacity] = s;
      ++g_pendingCount;
    } else {
      // 缓冲满时覆盖最老的未处理点，保留最新轨迹。
      g_pendingSamples[g_pendingHead] = s;
      g_pendingHead = (g_pendingHead + 1) % kPendingCapacity;
    }
    ++g_statEvents;
  }
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
void ReconcileCursor(uint64_t now) {
  if (!g_renderer.ready() || g_deviceLost || g_frameGuard.paused()) return;
  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || (ci.flags & CURSOR_SHOWING) == 0 || !ci.hCursor) {
    HideCursorVisuals();
    return;
  }
  g_cursorVisible = true;
  if (!g_haveLastCursor) {
    g_lastCursorX = ci.ptScreenPos.x;
    g_lastCursorY = ci.ptScreenPos.y;
    g_haveLastCursor = true;
    return;
  }

  EnsureCursorBitmap(ci.hCursor, now);
  const int x = ci.ptScreenPos.x;
  const int y = ci.ptScreenPos.y;
  if (g_haveLastCursor && x == g_lastCursorX && y == g_lastCursorY) return;

  g_lastCursorX = x;
  g_lastCursorY = y;
  g_haveLastCursor = true;

  const Sample sample{static_cast<int32_t>(x), static_cast<int32_t>(y), now};
  bool committed = false;
  const OverlayRenderer::FrameResult fr = g_renderer.Update(&sample, 1, now, &committed);
  if (committed) RecordCommit(now);
  if (fr == OverlayRenderer::FrameResult::RecreateDevice) g_deviceLost = true;
  ++g_statSamples;
}

// 等到 deadline 或输入消息先到。低级鼠标钩子的回调在之后的 PeekMessage 里执行。
// 仅在 deadline 变化或定时器触发后才重设 SetWaitableTimerEx，避免每次鼠标事件重复系统调用。
void WaitForWork(uint64_t deadline) {
  const uint64_t now = QpcNow();
  if (deadline <= now) return;
  const uint64_t remain = deadline - now;

  bool timerArmed = false;
  if (g_timer) {
    if (g_armedDeadline == deadline) {
      timerArmed = true;
    } else {
      uint64_t rel100ns = remain * 10000000ULL / g_qpcFreq;
      if (rel100ns < 1) rel100ns = 1;  // 0 表示绝对 1601 年，不触发
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(rel100ns);
      if (SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
        g_armedDeadline = deadline;
        timerArmed = true;
      } else {
        g_armedDeadline = 0;
        static bool loggedFailure = false;
        if (!loggedFailure) {
          loggedFailure = true;
          DiagLog(L"[main] SetWaitableTimerEx failed (error=%lu), falling back to ms timeout",
                  GetLastError());
        }
      }
    }
  }

  DWORD result = 0;
  if (timerArmed) {
    HANDLE timer = g_timer;
    // 定时器已精确定时，等待超时直接用 INFINITE，避免再挂第二个毫秒级内核定时器。
    result = MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0) g_armedDeadline = 0;  // 同步定时器已触发并自动复位
  } else {
    uint64_t ms = (remain * 1000ULL + g_qpcFreq - 1) / g_qpcFreq;
    if (ms == 0) ms = 1;
    if (ms > 0xFFFFFFFFULL) ms = 0xFFFFFFFFULL;
    result = MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(ms), QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE);
  }

  if (result == WAIT_FAILED) {
    g_armedDeadline = 0;
    DiagLog(L"[main] MsgWaitForMultipleObjectsEx failed (error=%lu)", GetLastError());
    Sleep(1);  // 防止错误路径忙转
  }
}

void LogStats(uint64_t now) {
  if (g_statEvents == 0 && g_statCommits == 0) {
    g_statWindowStart = now;
    return;
  }
  const double secs = static_cast<double>(now - g_statWindowStart) / static_cast<double>(g_qpcFreq);
  if (secs <= 0.0) return;

  const double events = static_cast<double>(g_statEvents);
  const double commits = static_cast<double>(g_statCommits);
  DiagLog(L"[update] events=%.1f/s samples=%.1f/s commits=%.1f/s live=%d | "
          L"commit mean=%.1f max=%.1f us | frame=%.2f ms paused=%d",
          events / secs, static_cast<double>(g_statSamples) / secs, commits / secs,
          g_renderer.liveSprites(), commits > 0.0 ? g_statCommitUsSum / commits : 0.0,
          g_statCommitUsMax, g_frameGuard.lastMeasuredMs(), g_frameGuard.paused() ? 1 : 0);

  g_statWindowStart = now;
  g_statEvents = 0;
  g_statSamples = 0;
  g_statCommits = 0;
  g_statCommitUsSum = 0.0;
  g_statCommitUsMax = 0.0;
}

// 停止输入驱动与定时器，然后按定义好的顺序拆窗口/视觉树。
void ShutdownApp() {
  g_mouseHookReady = false;
  if (g_mouseHook) {
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
  }
  g_pendingHead = 0;
  g_pendingCount = 0;
  if (g_timer) {
    CloseHandle(g_timer);
    g_timer = nullptr;
  }
  g_armedDeadline = 0;
  if (g_hwnd) {
    UnregisterHotKey(g_hwnd, kQuitHotkeyId);
    g_tearingDownWindow = true;
    ShowWindow(g_hwnd, SW_HIDE);
    g_renderer.Shutdown();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    g_tearingDownWindow = false;
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
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // ---- 命令行参数 ----
  int maxSprites = 64;
  bool enableLog = false;
  for (int i = 1; i < __argc; ++i) {
    const wchar_t* a = __wargv[i];
    const wchar_t* value = (i + 1 < __argc) ? __wargv[i + 1] : nullptr;
    if (wcscmp(a, L"--help") == 0) {
      PrintUsage();
      return 0;
    } else if (wcscmp(a, L"--sprites") == 0 && value) {
      maxSprites = static_cast<int>(wcstol(value, nullptr, 10));
      ++i;
    } else if (wcscmp(a, L"--log") == 0) {
      enableLog = true;
    }
  }
  if (maxSprites < 1) maxSprites = 1;
  if (maxSprites > 512) maxSprites = 512;

  // ---- 单实例保护（在打开日志之前检查，避免干扰运行中实例）----
  HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, kInstanceMutex);
  if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(instanceMutex);
    DiagInit(false);
    DiagFatal(L"Trail", L"Trail 已在运行中，请勿重复启动。\n如需退出已有实例，请按 Ctrl+Alt+Q。");
    DiagClose();
    return 1;
  }

  DiagInit(enableLog);

  auto cleanupExit = [&](int code) {
    ShutdownApp();
    DiagClose();
    if (instanceMutex) CloseHandle(instanceMutex);
    return code;
  };

  LARGE_INTEGER freq{};
  if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
    DiagFatal(L"Trail", L"QueryPerformanceFrequency 失败。");
    return cleanupExit(1);
  }
  g_qpcFreq = static_cast<uint64_t>(freq.QuadPart);
  g_instance = hInstance;

  g_frameGuard.Initialize(g_qpcFreq);
  const double trailMs = g_frameGuard.baselineMs();

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hCursor = nullptr;
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&wc)) {
    wchar_t msg[256];
    swprintf_s(msg, L"RegisterClassExW 失败 (error=%lu)。", GetLastError());
    DiagFatal(L"Trail", msg);
    return cleanupExit(1);
  }

  const CreateOverlayStatus status = CreateOverlay(maxSprites, trailMs);
  if (status == CreateOverlayStatus::HotkeyBusy) {
    DiagFatal(L"Trail", L"退出热键 Ctrl+Alt+Q 已被其他程序占用，程序退出。");
    return cleanupExit(1);
  }
  if (status != CreateOverlayStatus::Ok) {
    DiagFatal(L"Trail",
              L"叠加层初始化失败：需要硬件 D3D11 设备与可用的 DirectComposition。\n"
              L"可加 --log 参数记录具体失败步骤。");
    return cleanupExit(1);
  }
  DiagLog(L"[main] renderer ready: trail=%.3f ms (auto 1-frame), sprites=%d, input=WH_MOUSE_LL",
          trailMs, maxSprites);

  // 采样点到期 / 看门狗用的高分辨率定时器；创建失败时退回毫秒超时。
  g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
  if (!g_timer) {
    DiagLog(L"[main] high-resolution timer unavailable (error=%lu), falling back to ms timeout",
            GetLastError());
  }

  // 钩子回调在主线程执行：提高优先级让采样入队与提交更少被抢占。用 HIGHEST 而非
  // TIME_CRITICAL，避免抢占 DWM / 游戏线程。
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

  if (!InstallMouseHook()) {
    wchar_t msg[256];
    swprintf_s(msg,
               L"WH_MOUSE_LL 低级鼠标钩子安装失败 (error=%lu)。\n"
               L"没有钩子就没有采样点，程序无法工作。",
               GetLastError());
    DiagFatal(L"Trail", msg);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    return cleanupExit(1);
  }

  // 跳过建窗与显示叠加层期间的 DWM 过渡帧，从主循环进入后开始统计。
  g_frameGuard.ResetSession();

  const uint64_t loopStart = QpcNow();
  g_statWindowStart = loopStart;
  uint64_t nextHealth = loopStart + MsToTicks(kHealthCheckMs);
  uint64_t nextStats = loopStart + MsToTicks(kStatsIntervalMs);
  uint64_t nextRecreate = 0;
  UINT recreateBackoffMs = kRecreateInitialBackoffMs;

  MSG msg{};
  bool running = true;
  while (running) {
    // 泵消息。WH_MOUSE_LL 回调在此期间将采样点入队并立刻返回。
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

    // ---- 帧时间守卫：被动消费 DWM 已完成帧，偏离启动基准时清空尾迹并停 Commit ----
    if (g_frameGuard.Poll() && g_frameGuard.paused()) {
      HideCursorVisuals();
    }

    // ---- 批量提交本轮钩子采集的采样点 ----
    FlushPendingSamples(now);

    // ---- 采样点到期：定时器把我们从等待中叫醒，隐藏已经过期的槽位 ----
    if (g_renderer.ready() && !g_deviceLost && !g_frameGuard.paused()) {
      const uint64_t expiry = g_renderer.NextExpiryQpc();
      if (expiry != 0 && now >= expiry) {
        bool committed = false;
        const OverlayRenderer::FrameResult fr = g_renderer.Expire(now, &committed);
        if (committed) RecordCommit(now);
        if (fr == OverlayRenderer::FrameResult::RecreateDevice) g_deviceLost = true;
      }
    }

    // ---- 设备丢失恢复：用现有窗口重建渲染栈 ----
    if (g_deviceLost) {
      if (nextRecreate == 0 || now >= nextRecreate) {
        DiagLog(L"[recover] rebuilding after device loss");
        if (InitRenderer(maxSprites, trailMs)) {
          DiagLog(L"[recover] renderer rebuilt");
          g_frameGuard.ResetSession();
          g_deviceLost = false;
          nextRecreate = 0;
          recreateBackoffMs = kRecreateInitialBackoffMs;
        } else {
          DiagLog(L"[recover] rebuild failed (will retry in %u ms)", recreateBackoffMs);
          nextRecreate = now + MsToTicks(recreateBackoffMs);
          recreateBackoffMs = std::min(recreateBackoffMs * 2, kRecreateMaxBackoffMs);
        }
      }
    }
    if (!g_renderer.ready()) g_deviceLost = true;

    // ---- 健康检查（看门狗，不是渲染节拍）----
    if (now >= nextHealth) {
      nextHealth = now + MsToTicks(kHealthCheckMs);
      EnsureTopmost(g_hwnd);
      if (g_renderer.ready() && !g_renderer.DeviceValid()) g_deviceLost = true;
      ReconcileCursor(now);
    }

    // ---- 统计（仅 --log 启用时才调度和输出）----
    if (DiagLogEnabled() && now >= nextStats) {
      LogStats(now);
      nextStats = now + MsToTicks(kStatsIntervalMs);
    }

    // ---- 计算下一次唤醒时刻：最早到期 / 健康检查 / 统计 / 重建重试 ----
    uint64_t deadline = nextHealth;
    if (DiagLogEnabled() && nextStats < deadline) deadline = nextStats;
    if (g_deviceLost) {
      // nextRecreate == 0 表示刚检测到设备丢失，立即进入下一轮重建而不空等看门狗周期。
      if (nextRecreate == 0) {
        deadline = now;
      } else if (nextRecreate < deadline) {
        deadline = nextRecreate;
      }
    }
    if (g_renderer.ready()) {
      const uint64_t expiry = g_renderer.NextExpiryQpc();
      if (expiry != 0 && expiry < deadline) deadline = expiry;
    }
    WaitForWork(deadline);
  }

  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
  return cleanupExit(0);
}
