// 帧内鼠标尾迹全屏透明叠加层
//
// 线程模型：
//   单线程 —— 窗口 + 消息循环 + 渲染（Present(1,0) 由 vsync 节流）。
//   无独立采样线程：渲染线程每帧唤醒时按需调用 GetMouseMovePointsEx，从系统
//   自维护的 64 点鼠标移动历史中增量取回本帧轨迹。
//
// 帧内尾迹语义：渲染线程用 (x,y,time) 水印追踪系统历史的消费进度，每帧只绘制
// 自上次调用以来新增的移动点。每个采样点恰好被绘制一帧后消失 —— 尾迹长度 =
// 一帧内的鼠标位移，路径精度由系统鼠标报告率决定。
#include <windows.h>
#include <wrl/client.h>

#include <intrin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dwmapi.h>

#include "cursor_texture.h"
#include "diag.h"
#include "mouse_history.h"
#include "overlay_renderer.h"

namespace {

constexpr UINT kQuitHotkeyId = 1;

OverlayRenderer g_renderer;
CursorTexture g_cursorTex;  // 光标纹理缓存（形状变化时才重建）
int g_originX = 0, g_originY = 0;  // 虚拟屏幕原点（窗口左上角）
bool g_hideCursor = false;
MouseHistoryTracker g_mouseHistory;  // 系统鼠标历史的增量读取状态
// ---- 设备丢失 / 窗口健康状态（消息、渲染都在主线程，无需同步）----
// 本窗口没有任何 GDI 内容，可见像素 100% 来自 DWM 合成的 DirectComposition 视觉树，
// 因此 DXGI/DComp 设备一旦丢失，叠加层会整体变透明且不会自愈 —— 只能重建整套设备。
bool g_deviceLost = false;      // 设备丢失（WM_PAINT 通知 / 帧循环检出 / 几何变化）
bool g_displayChanged = false;  // 收到 WM_DISPLAYCHANGE
// ---- 低延迟渲染：vblank 前对齐（Present(0) 赶上当前 vsync 显示）----
struct VsyncState {
  uint64_t period = 0;    // 合成刷新周期（QPC ticks）
  uint64_t anchor = 0;    // 目标 vsync 相位（QPC 域）
  double emaRenderMs = 3.0;  // 渲染耗时 EMA，用于自适应预算
  uint64_t frameCount = 0;
  uint64_t missed = 0;    // 渲染超时错过目标 vsync 的次数
};
// 主线程独占的 vsync 校准/对齐状态（含刷新周期，供低延迟渲染对齐）。
static VsyncState g_vsync;

static uint64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

// 用 DwmFlush（阻塞到合成刷新）实测刷新周期与相位。失败返回 false 并记录原因
// （DWM 不合成时 DwmFlush 会立即返回错误码，此时不能以它作为 vsync 基准）。
static bool MeasureVsyncPeriod(int samples, uint64_t& outPeriod, uint64_t& outAnchor) {
  HRESULT hr = DwmFlush();
  if (FAILED(hr)) {
    DiagLog(L"[vsync] DwmFlush failed: 0x%08X", static_cast<unsigned>(hr));
    return false;
  }
  uint64_t prev = QpcNow();
  uint64_t total = 0;
  for (int i = 0; i < samples; ++i) {
    hr = DwmFlush();
    if (FAILED(hr)) {
      DiagLog(L"[vsync] DwmFlush failed: 0x%08X", static_cast<unsigned>(hr));
      return false;
    }
    const uint64_t t = QpcNow();
    total += t - prev;
    prev = t;
  }
  outPeriod = total / static_cast<uint64_t>(samples);
  outAnchor = prev;  // 最近一次合成刷新 ≈ vsync 相位
  return true;
}

// 用 DwmFlush 自举校准刷新周期与相位。
static bool CalibrateVsync(VsyncState& s) {
  uint64_t period = 0, anchor = 0;
  if (!MeasureVsyncPeriod(10, period, anchor)) return false;
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const double ms = static_cast<double>(period) * 1000.0 / static_cast<double>(freq.QuadPart);
  if (ms < 3.0 || ms > 70.0) {  // 刷新率约 15Hz~333Hz 之外视为异常
    DiagLog(L"[vsync] calibration rejected: %.2f ms per flush (DwmFlush not vsync-throttled?)",
            ms);
    return false;
  }
  s.period = period;
  s.anchor = anchor;
  DiagLog(L"[vsync] calibrated refresh period: %.2f ms", ms);
  return true;
}

// 运行中重校准刷新周期（DwmFlush 实测，EMA 更新），应对 VRR/显示器切换等
// 刷新率变化。每次约阻塞 4 个刷新周期（掉几帧），每 1500 帧一次可接受。
// 实测失败时保留旧周期与旧相位（不清零），避免把对齐基准一次打坏。
static void RefreshVsyncPeriod(VsyncState& s) {
  uint64_t newPeriod = 0, anchor = 0;
  if (!MeasureVsyncPeriod(4, newPeriod, anchor)) return;
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const double ms = static_cast<double>(newPeriod) * 1000.0 / static_cast<double>(freq.QuadPart);
  if (ms < 3.0 || ms > 70.0) return;
  if (newPeriod != s.period) {
    const double oldMs = static_cast<double>(s.period) * 1000.0 / static_cast<double>(freq.QuadPart);
    if (oldMs > 0 && (ms / oldMs > 1.05 || ms / oldMs < 0.95)) {
      DiagLog(L"[vsync] refresh period changed: %.2f ms -> %.2f ms", oldMs, ms);
    }
  }
  s.period = (s.period * 7 + newPeriod) / 8;  // 慢 EMA，抑制抖动
  // 重新锚定相位到实测合成刷新（最后一次 DwmFlush 返回 ≈ 实际 vsync 相位）。
  // 仅更新周期而不重锚定的话，刷新率真实变化后旧相位基准会让显示持续晚一帧。
  s.anchor = anchor;
}

// 忙等（分层等待）到 下一 vsync - leadMs，返回时渲染可赶上当前 vsync。
// leadMs 是唤醒提前量（渲染预算）。返回 true 表示起步时已
// 错过目标 vsync（上一帧渲染超时，或首帧），本帧不等待、立即渲染追赶。唤醒分
// 两段：远离 target 用 Sleep(1) 粗睡省 CPU，进入最后 spinMargin（2ms）纯忙等
// （YieldProcessor）精确对齐到 target —— 忙等缓冲足够吸收 Sleep(1) 在
// timeBeginPeriod(1) 下的过冲（约 ≤1.5ms），保证唤醒点精确落在 target、既不睡
// 过头（睡过头会压缩渲染预算、增加错过 vsync 的概率）也不提前太多。
static bool WaitForVsyncAligned(VsyncState& s, double leadMs) {
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const uint64_t lead =
      static_cast<uint64_t>(leadMs * static_cast<double>(freq.QuadPart) / 1000.0);
  const uint64_t now = QpcNow();

  // 已错过目标 vsync：不空等、立即渲染追赶，避免空等把"漏一帧"放大成
  // "帧间隔翻倍"——否则下一帧会一次性画出更长时间窗口内积累的轨迹，尾迹被
  // 拉长。仍把相位推进到未来最近的同相位 vsync，供下一帧重新对齐节奏。
  if (now >= s.anchor) {
    s.anchor = s.anchor + s.period * ((now - s.anchor) / s.period + 1);
    return true;
  }

  // 锚点未到（上一帧提前完成）：本帧只能排到 anchor + period（DWM 一帧
  // 占一个 vsync 槽），若复用当前 anchor 会导致渲染逐帧逼近 vsync 直至错过。
  s.anchor = s.anchor + s.period;
  const uint64_t target = s.anchor - lead;
  const uint64_t spinMargin =
      static_cast<uint64_t>(2.0 * static_cast<double>(freq.QuadPart) / 1000.0);
  for (;;) {
    const uint64_t t = QpcNow();
    if (t >= target) break;
    const uint64_t remain = target - t;
    if (remain > spinMargin) {
      Sleep(1);  // 还远：粗睡省 CPU
    } else {
      YieldProcessor();  // 最后 2ms：纯忙等，降低自旋功耗与总线争用
    }
  }
  return false;
}

void PrintUsage() {
  wprintf(
      L"Trail\n"
      L"Usage: trail.exe [options]\n"
      L"  --hide-cursor     hide the system cursor (within this overlay window)\n"
      L"  --help            show this help\n"
      L"Quit: Ctrl+Alt+Q\n");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // 点击穿透到下层窗口
    case WM_PAINT:
      // DirectComposition 在底层 DXGI 设备丢失时会向合成其内容的窗口发送 WM_PAINT
      // （见 IDCompositionDevice::CheckDeviceState 文档）。此处确认设备状态，失效则
      // 交由主循环重建整套设备与内容 —— 否则内容永久消失，只能重启进程。
      ValidateRect(hwnd, nullptr);
      if (g_renderer.ready() && !g_renderer.DeviceValid()) g_deviceLost = true;
      return 0;
    case WM_DISPLAYCHANGE:
      // 分辨率/显示器拓扑变化：窗口几何与离屏位图尺寸都会失配，主循环里重新同步
      // 并重建交换链。
      g_displayChanged = true;
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
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

OverlayRenderer::FrameResult RenderOneFrame(bool waitForVBlank) {
  // 设备丢失/重建期间渲染器已释放，Context() 为空，此时不得进入渲染路径
  // （光标纹理抓取会解引用空上下文）。
  if (!g_renderer.ready()) return OverlayRenderer::FrameResult::RecreateDevice;

  // 光标纹理：仅当 hCursor 句柄变化时才重新抓取（游戏中光标形状几乎不变，
  // 该路径每帧仅一次 GetCursorInfo，开销约 1µs）。
  ID2D1Bitmap* cursorBmp = nullptr;
  int texW = 0, texH = 0, hotX = 0, hotY = 0;
  CURSORINFO ci{sizeof(ci)};
  if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
    if (ci.hCursor != g_cursorTex.handle) {
      if (!CaptureCursorTexture(g_renderer.Context(), ci.hCursor, g_cursorTex)) {
        g_cursorTex.bitmap.Reset();
        g_cursorTex.handle = nullptr;
      }
    }
    if (g_cursorTex.bitmap) {
      cursorBmp = g_cursorTex.bitmap.Get();
      texW = g_cursorTex.width;
      texH = g_cursorTex.height;
      hotX = g_cursorTex.hotX;
      hotY = g_cursorTex.hotY;
    }
  }

  // 从系统鼠标移动历史按需取回本帧轨迹（替代独立采样线程的 GetCursorPos 轮询）。
  // GetCursorInfo 已返回当前位置 ci.ptScreenPos，作 GetMouseMovePointsEx 的 anchor。
  // 实时头部点由 RenderFrame 在提交前最后一刻用 GetCursorInfo 重新采样。
  Sample pts[512];
  const uint32_t n = static_cast<uint32_t>(
      CollectMouseHistory(pts, 512, g_mouseHistory, ci.ptScreenPos.x, ci.ptScreenPos.y));

  return g_renderer.RenderFrame(cursorBmp, texW, texH, hotX, hotY, pts, n, g_originX, g_originY,
                                waitForVBlank, /*drawLiveHead=*/cursorBmp != nullptr);
}

// 把窗口几何同步到当前虚拟屏幕。显示器热插拔 / 分辨率变化后窗口与离屏位图尺寸
// 都会失配（新区域无内容、坐标错位）。返回 true 表示几何已变化（窗口已同步，调用
// 方需按新尺寸重建渲染器）。
bool SyncWindowGeometry(HWND hwnd, int& originX, int& originY, int& vw, int& vh) {
  const int nx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int ny = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int nw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int nh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (nw <= 0 || nh <= 0) return false;
  if (nx == originX && ny == originY && nw == vw && nh == vh) return false;
  DiagLog(L"[watch] virtual screen %d,%d %dx%d -> %d,%d %dx%d, resizing overlay", originX, originY,
          vw, vh, nx, ny, nw, nh);
  originX = nx;
  originY = ny;
  vw = nw;
  vh = nh;
  SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, nw, nh, SWP_NOACTIVATE | SWP_NOREDRAW);
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

// 重建整套渲染栈（设备丢失后进程内唯一的恢复途径）。成功返回 true。
bool RecreateRenderer(HWND hwnd, int vw, int vh) {
  DiagLog(L"[recover] recreating D3D/D2D/DComp stack (%d x %d)", vw, vh);
  g_cursorTex.bitmap.Reset();  // 旧纹理由已销毁的 D2D 设备创建，必须重新抓取
  g_cursorTex.handle = nullptr;
  g_renderer.Shutdown();
  if (!g_renderer.Initialize(hwnd, vw, vh)) {
    DiagLog(L"[recover] re-initialize failed (will retry)");
    return false;
  }
  DiagLog(L"[recover] renderer re-initialized");
  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrev*/, PWSTR /*cmdLine*/, int /*show*/) {
  // Per-monitor DPI 感知（v2）：GetCursorInfo / GetMouseMovePointsEx / GetSystemMetrics
  // 均返回物理像素，与 D2D 渲染坐标一致。
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  auto setDpiAwarenessContext =
      reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(GetProcAddress(
          user32, "SetProcessDpiAwarenessContext"));
  if (setDpiAwarenessContext) {
    setDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  } else {
    SetProcessDPIAware();
  }

  // 诊断输出：日志文件 + stderr + MessageBox（任何启动方式下可见）。
  DiagInit();

  // 命令行参数
  for (int i = 1; i < __argc; ++i) {
    const wchar_t* a = __wargv[i];
    if (wcscmp(a, L"--hide-cursor") == 0) {
      g_hideCursor = true;
    } else if (wcscmp(a, L"--help") == 0) {
      PrintUsage();
      return 0;
    }
  }

  // 覆盖整个虚拟桌面（含多显示器）。
  g_originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
  g_originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);  // 非 const：运行中可能变化（见看门狗）
  int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hCursor = nullptr;
  wc.lpszClassName = L"TrailOverlay";
  if (!RegisterClassExW(&wc)) {
    const DWORD err = GetLastError();
    wchar_t msg[256];
    swprintf_s(msg, L"RegisterClassExW 失败 (error=%lu)。\n\n诊断日志: %ls", err, DiagLogPath());
    DiagFatal(L"Trail", msg);
    return 1;
  }

  // 点击穿透：WS_EX_LAYERED | WS_EX_TRANSPARENT 组合使整个窗口对鼠标命中测试
  // 透明（微软 window-features 文档：layered 窗口命中基于形状/透明度，加
  // WS_EX_TRANSPARENT 后形状被忽略、鼠标事件传给下层窗口）。这是唯一可靠的
  // 跨进程穿透机制——HTTRANSPARENT 只转发给同线程兄弟窗口，WS_EX_TRANSPARENT
  // 单独使用对命中测试无效。DComp 允许 layered target 窗口。
  // 有意不加 WS_EX_NOREDIRECTIONBITMAP：本窗口内容完全由 DComp swapchain 视觉树
  // 提供，若加该样式 DWM 仍会为窗口维护重定向表面，白白多一次拷贝。
  HWND hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
      wc.lpszClassName, L"Trail  (Ctrl+Alt+Q 退出)",
      WS_POPUP, g_originX, g_originY, vw, vh, nullptr, nullptr, hInstance, nullptr);
  if (!hwnd) {
    const DWORD err = GetLastError();
    wchar_t msg[256];
    swprintf_s(msg, L"CreateWindowExW 失败 (error=%lu)。\n\n诊断日志: %ls", err, DiagLogPath());
    DiagFatal(L"Trail", msg);
    return 1;
  }
  // alpha=255：窗口内容不透明度不变（DComp 视觉树自带逐像素 alpha），
  // 仅让 layered 样式生效以启用命中穿透。
  SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
  DiagLog(L"[main] virtual screen %d,%d %dx%d, hwnd=%p", g_originX, g_originY, vw, vh,
          static_cast<void*>(hwnd));

  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(hwnd, HWND_TOPMOST, g_originX, g_originY, vw, vh, SWP_NOACTIVATE | SWP_NOREDRAW);

  if (!g_renderer.Initialize(hwnd, vw, vh)) {
    wchar_t msg[512];
    swprintf_s(msg,
               L"渲染器初始化失败：需要硬件 D3D11 设备与 flip-model 交换链\n"
               L"（不降级 WARP）。请查看上方/日志中的具体失败步骤。\n\n诊断日志: %ls",
               DiagLogPath());
    DiagFatal(L"Trail", msg);
    DestroyWindow(hwnd);
    return 1;
  }
  DiagLog(L"[main] renderer ready, %d x %d", vw, vh);

  RegisterHotKey(hwnd, kQuitHotkeyId, MOD_CONTROL | MOD_ALT, 'Q');

  // 主循环：消息 + 低延迟渲染。
  // 默认模式：DwmFlush 校准 vsync 相位，忙等到 vblank 前 budget 毫秒开始渲染，
  // Present(0) 让帧赶上当前 vsync 显示 —— 尾迹头延迟从约 1 帧压缩到渲染预算量级。
  // 校准失败（无 DWM 合成）时回退 Present(1,0) 阻塞等 vsync。
  const bool lowLatency = CalibrateVsync(g_vsync);
  if (!lowLatency) DiagLog(L"[main] vsync calibration failed, falling back to Present(1,0)");
  double budgetMs = 2.0;
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  // 提升定时器分辨率，使低延迟等待里的 Sleep(1) 接近 1ms；退出时恢复。
  timeBeginPeriod(1);
  // 提升渲染线程（主线程）优先级：低延迟 vsync 对齐对调度抖动敏感，普通优先级
  // 下忙等/渲染易被抢占导致错过 vsync。用 HIGHEST 而非 TIME_CRITICAL，避免抢占
  // DWM / 游戏线程。退出时恢复。
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

  MSG msg{};
  bool running = true;
  uint64_t loopCount = 0;
  // 重建失败的退避：以 QPC 计时而非帧计数（丢失期间不渲染，循环节奏与帧率无关），
  // 从 500ms 起逐次加倍，成功后复位。
  int recreateBackoffMs = 500;
  uint64_t nextRecreateTicks = 0;  // 下次尝试重建的时刻（0 = 立即尝试）
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

    ++loopCount;

    // ---- 周期性健康检查（≈每 2 秒 @120Hz）----
    // 叠加层"渲染突然消失"的三条已知途径：窗口被压到非顶层、几何与虚拟屏幕失配、
    // DComp 设备失效。这里逐项确认并纠正（几个 Get* 调用，开销可忽略）。
    if (loopCount % 240 == 0) {
      EnsureTopmost(hwnd);
      if (SyncWindowGeometry(hwnd, g_originX, g_originY, vw, vh)) {
        g_deviceLost = true;  // 尺寸变化：交换链与离屏位图必须按新尺寸重建
      } else if (g_renderer.ready() && !g_renderer.DeviceValid()) {
        g_deviceLost = true;
      }
    }
    if (g_displayChanged) {
      g_displayChanged = false;
      if (SyncWindowGeometry(hwnd, g_originX, g_originY, vw, vh)) g_deviceLost = true;
    }

    // ---- 设备丢失恢复 ----
    // 重建整套 D3D/D2D/DComp 设备与内容。重建失败时按 0.5s 起、逐次加倍（上限
    // 15s）的时间退避重试 —— 驱动复位后设备可能短暂不可用；期间不渲染（渲染器已
    // 释放，Context() 为空），也不让日志被重试失败刷屏。
    if (g_deviceLost) {
      const uint64_t nowTicks = QpcNow();
      if (nextRecreateTicks != 0 && nowTicks < nextRecreateTicks) {
        // 睡到下次重试（单片上限 50ms：既不做无谓空转，也保持消息循环/退出热键响应）
        const uint64_t remainTicks = nextRecreateTicks - nowTicks;
        DWORD ms = static_cast<DWORD>(remainTicks * 1000ULL / static_cast<uint64_t>(freq.QuadPart));
        if (ms > 50) ms = 50;
        Sleep(ms > 0 ? ms : 1);
        continue;
      }
      if (RecreateRenderer(hwnd, vw, vh)) {
        g_deviceLost = false;
        nextRecreateTicks = 0;
        recreateBackoffMs = 500;
        // 设备丢失常伴随显示模式/刷新率变化，重新实测刷新周期并锚定 vsync 相位。
        if (lowLatency) RefreshVsyncPeriod(g_vsync);
      } else {
        nextRecreateTicks =
            nowTicks + static_cast<uint64_t>(recreateBackoffMs) *
                           static_cast<uint64_t>(freq.QuadPart) / 1000ULL;
        recreateBackoffMs = (recreateBackoffMs >= 8000) ? 15000 : recreateBackoffMs * 2;
        continue;
      }
    }

    if (lowLatency) {
      const double periodMs =
          static_cast<double>(g_vsync.period) * 1000.0 / static_cast<double>(freq.QuadPart);

      const bool startedLate = WaitForVsyncAligned(g_vsync, budgetMs);
      const uint64_t t0 = QpcNow();
      const OverlayRenderer::FrameResult fr = RenderOneFrame(false);
      const uint64_t t1 = QpcNow();
      if (fr == OverlayRenderer::FrameResult::RecreateDevice) g_deviceLost = true;
      // 渲染耗时 EMA -> 自适应预算：EMA + 1.0ms 余量，限幅 [1, 8]ms（配合脏矩形
      // 清除与延迟采样，渲染更快，故下限/余量较旧值收紧，让 Present 更贴近 vsync）。
      const double renderMs =
          static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(freq.QuadPart);
      g_vsync.emaRenderMs = g_vsync.emaRenderMs * 0.9 + renderMs * 0.1;
      budgetMs = g_vsync.emaRenderMs + 1.0;
      if (budgetMs < 1.0) budgetMs = 1.0;
      if (budgetMs > 8.0) budgetMs = 8.0;
      const double budgetMax = periodMs * 0.6;
      if (budgetMs > budgetMax) budgetMs = budgetMax;
      ++g_vsync.frameCount;

      // 错过：起步就晚（startedLate），或渲染在目标 vsync 后才完成（t1 > anchor）。
      if (startedLate || t1 > g_vsync.anchor) {
        ++g_vsync.missed;
      }

      if (g_vsync.frameCount % 3000 == 0) {
        DiagLog(L"[vsync] frames=%llu missed=%llu (%.1f%%), render EMA=%.2f ms, budget=%.2f ms",
                static_cast<unsigned long long>(g_vsync.frameCount),
                static_cast<unsigned long long>(g_vsync.missed),
                100.0 * static_cast<double>(g_vsync.missed) / g_vsync.frameCount,
                g_vsync.emaRenderMs, budgetMs);
      }
      if (g_vsync.frameCount % 1500 == 0) RefreshVsyncPeriod(g_vsync);
    } else {
      // Present(1,0)，vsync 阻塞节流。回退路径同样打存活心跳：日志停止增长 = 主
      // 循环被阻塞（例如 DwmFlush 卡住），日志继续增长 = 循环正常、问题在合成/分层
      // 侧 —— 这是"渲染消失"最直接的区分依据。
      if (RenderOneFrame(true) == OverlayRenderer::FrameResult::RecreateDevice) {
        g_deviceLost = true;
      }
      if (++g_vsync.frameCount % 3000 == 0) {
        DiagLog(L"[frame] %llu frames rendered (Present(1,0) fallback)",
                static_cast<unsigned long long>(g_vsync.frameCount));
      }
    }
  }
  timeEndPeriod(1);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

  // 清理
  UnregisterHotKey(hwnd, kQuitHotkeyId);
  g_cursorTex.bitmap.Reset();
  g_renderer.Shutdown();
  DestroyWindow(hwnd);
  DiagClose();
  return 0;
}
