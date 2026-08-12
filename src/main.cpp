// 帧内鼠标尾迹全屏透明叠加层
//
// 线程模型：
//   主线程    —— 窗口 + 消息循环 + 渲染（Present(1,0) 由 vsync 节流）
//   采样线程  —— THREAD_PRIORITY_HIGHEST，~1000Hz 轮询 GetCursorPos，
//                把 (QPC 时间戳, 坐标) 写入无锁 SPSC 环形缓冲
//
// 帧内尾迹语义：渲染线程记录上一帧时刻 T_prev，每帧只绘制 t∈[T_prev, T_now)
// 的采样点。帧窗口无缝连续覆盖整个时间线，因此每个采样点恰好被绘制一帧后
// 消失 —— 尾迹长度 = 一帧内的鼠标位移，采样率越高路径越精确。
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dwmapi.h>

#include "cursor_sampler.h"
#include "cursor_texture.h"
#include "diag.h"
#include "overlay_renderer.h"

namespace {

constexpr UINT kQuitHotkeyId = 1;

OverlayRenderer g_renderer;
CursorTexture g_cursorTex;  // 光标纹理缓存（形状变化时才重建）
uint64_t g_lastFrameQpc = 0;
int g_originX = 0, g_originY = 0;  // 虚拟屏幕原点（窗口左上角）
bool g_hideCursor = false;
HANDLE g_samplerThread = nullptr;

// ---- 低延迟渲染：vblank 前对齐（Present(0) 赶上当前 vsync 显示）----
struct VsyncState {
  bool ok = false;        // 校准成功（否则回退 Present(1,0)）
  uint64_t period = 0;    // 合成刷新周期（QPC ticks）
  uint64_t anchor = 0;    // 目标 vsync 相位（QPC 域）
  double emaRenderMs = 3.0;  // 渲染耗时 EMA，用于自适应预算
  uint64_t frameCount = 0;
  uint64_t missed = 0;    // 渲染超时错过目标 vsync 的次数
};

static uint64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

// 用 DwmFlush（阻塞到合成刷新）自举校准刷新周期与相位。
static bool CalibrateVsync(VsyncState& s) {
  if (FAILED(DwmFlush())) return false;
  uint64_t t0 = QpcNow();
  if (FAILED(DwmFlush())) return false;
  uint64_t t1 = QpcNow();
  uint64_t total = t1 - t0;
  for (int i = 0; i < 9; ++i) {
    if (FAILED(DwmFlush())) return false;
    const uint64_t t = QpcNow();
    total += t - t1;
    t1 = t;
  }
  const uint64_t period = total / 10;
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const double ms = static_cast<double>(period) * 1000.0 / static_cast<double>(freq.QuadPart);
  if (ms < 3.0 || ms > 70.0) return false;  // 刷新率约 15Hz~333Hz 之外视为异常
  s.period = period;
  s.anchor = t1;  // 最近一次合成刷新 ≈ vsync 相位
  s.ok = true;
  DiagLog(L"[vsync] calibrated refresh period: %.2f ms", ms);
  return true;
}

// 运行中重校准刷新周期（DwmFlush 实测，EMA 更新），应对 VRR/显示器切换等
// 刷新率变化。每次约阻塞 4 个刷新周期（掉几帧），每 1500 帧一次可接受。
static void RefreshVsyncPeriod(VsyncState& s) {
  if (FAILED(DwmFlush())) return;
  uint64_t t0 = QpcNow();
  if (FAILED(DwmFlush())) return;
  uint64_t t1 = QpcNow();
  uint64_t total = t1 - t0;
  for (int i = 0; i < 3; ++i) {
    if (FAILED(DwmFlush())) return;
    const uint64_t t = QpcNow();
    total += t - t1;
    t1 = t;
  }
  const uint64_t newPeriod = total / 4;
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
  s.anchor = t1;
}

// 忙等（分层等待）到 下一 vsync - budgetMs，返回时渲染可赶上当前 vsync。
// 分层：剩余 >2/3 预算睡眠；剩余 >0.1ms 让出时间片；极近则纯忙等，
// 避免 Sleep(1) 睡过头的过冲导致错过 vblank。
static void WaitForVsyncAligned(VsyncState& s, double budgetMs) {
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const uint64_t budget =
      static_cast<uint64_t>(budgetMs * static_cast<double>(freq.QuadPart) / 1000.0);
  const uint64_t now = QpcNow();
  // 本帧内容将在"下一个 vsync"显示：
  //  - 锚点已过（上一帧超时）：推进到未来最近的同相位 vsync；
  //  - 锚点未到（上一帧提前完成）：本帧只能排到 anchor + period（DWM 一帧
  //    占一个 vsync 槽），若复用当前 anchor 会导致渲染逐帧逼近 vsync 直至错过。
  uint64_t next;
  if (now >= s.anchor) {
    next = s.anchor + s.period * ((now - s.anchor) / s.period + 1);
  } else {
    next = s.anchor + s.period;
  }
  s.anchor = next;
  const uint64_t target = next - budget;
  const uint64_t spinThreshold = static_cast<uint64_t>(0.1 * static_cast<double>(freq.QuadPart) / 1000.0);
  for (;;) {
    const uint64_t t = QpcNow();
    if (t >= target) break;
    const uint64_t remain = target - t;
    if (remain > (budget * 2) / 3) {
      Sleep(1);
    } else if (remain > spinThreshold) {
      Sleep(0);
    }
    // 剩余 <=0.1ms：直接忙等（循环继续）
  }
}

void PrintUsage() {
  wprintf(
      L"Trail\n"
      L"Usage: trail.exe [options]\n"
      L"  --sample-ms <N>   sample interval in ms, default 1 (~1000 Hz)\n"
      L"  --hide-cursor     hide the system cursor (within this overlay window)\n"
      L"  --help            show this help\n"
      L"Quit: Ctrl+Alt+Q\n");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // 点击穿透到下层窗口
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

void RenderOneFrame(bool waitForVBlank) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const uint64_t nowQ = static_cast<uint64_t>(now.QuadPart);
  const uint64_t windowStart = g_lastFrameQpc;  // 上帧时刻 T_prev

  // 消费自上次以来的新增采样点，只保留落在 [T_prev, T_now) 内的。
  Sample pts[512];
  uint32_t n = 0;
  const uint32_t head = g_ring.ReadHead();
  uint32_t from = g_consumerIndex.load(std::memory_order_relaxed);
  for (uint32_t i = from; i < head && n < 512; ++i) {
    const Sample s = g_ring.At(i);
    // 严格属于本帧窗口 [T_prev, T_now) 才绘制；其余已过期，丢弃。
    if (s.t >= windowStart && s.t < nowQ) pts[n++] = s;
  }
  g_consumerIndex.store(head, std::memory_order_relaxed);

  // 实时头部点改为在 RenderFrame 内部、历史点 EndDraw 之后（CopyResource 之前）
  // 延迟采样，见 overlay_renderer.cpp —— 把头部点采样从渲染管线最前端推迟到
  // 提交前最后一刻，头部延迟从约一个渲染预算压缩到 CopyResource+Present 量级。

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

  g_renderer.RenderFrame(cursorBmp, texW, texH, hotX, hotY, pts, n, g_originX, g_originY,
                         waitForVBlank, /*drawLiveHead=*/true);
  g_lastFrameQpc = nowQ;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrev*/, PWSTR /*cmdLine*/, int /*show*/) {
  // Per-monitor DPI 感知（v2）：GetCursorPos / GetSystemMetrics 均返回物理像素，
  // 与 D2D 渲染坐标一致。
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
    } else if (wcscmp(a, L"--sample-ms") == 0 && i + 1 < __argc) {
      g_sampleMs = _wtoi(__wargv[++i]);
    } else if (wcscmp(a, L"--help") == 0) {
      PrintUsage();
      return 0;
    }
  }
  if (g_sampleMs < 1) g_sampleMs = 1;

  // 覆盖整个虚拟桌面（含多显示器）。
  g_originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
  g_originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

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

  // 启动高优先级采样线程。
  g_samplerThread = CreateThread(nullptr, 0, SamplerThreadMain, nullptr, 0, nullptr);

  RegisterHotKey(hwnd, kQuitHotkeyId, MOD_CONTROL | MOD_ALT, 'Q');

  // 主循环：消息 + 低延迟渲染。
  // 默认模式：DwmFlush 校准 vsync 相位，忙等到 vblank 前 budget 毫秒开始渲染，
  // Present(0) 让帧赶上当前 vsync 显示 —— 尾迹头延迟从约 1 帧压缩到渲染预算量级。
  // 校准失败（无 DWM 合成）时回退 Present(1,0) 阻塞等 vsync。
  VsyncState vsync;
  const bool lowLatency = CalibrateVsync(vsync);
  if (!lowLatency) DiagLog(L"[main] vsync calibration failed, falling back to Present(1,0)");
  double budgetMs = 2.0;
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  // 主线程自己提升定时器分辨率（不依赖采样线程的副作用），退出时恢复。
  timeBeginPeriod(1);

  MSG msg{};
  bool running = true;
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

    if (lowLatency) {
      WaitForVsyncAligned(vsync, budgetMs);
      const uint64_t t0 = QpcNow();
      RenderOneFrame(false);
      const uint64_t t1 = QpcNow();
      // 渲染耗时 EMA -> 自适应预算：EMA + 1.0ms 余量。
      // 上限受刷新周期约束（预算必须小于一个周期，否则 target 越过当前
      // vsync 导致对齐失效），并限幅 [1, 8]ms。
      const double renderMs =
          static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(freq.QuadPart);
      vsync.emaRenderMs = vsync.emaRenderMs * 0.9 + renderMs * 0.1;
      const double periodMs =
          static_cast<double>(vsync.period) * 1000.0 / static_cast<double>(freq.QuadPart);
      // 自适应预算：EMA + 1.0ms 余量，限幅 [1, 8]ms（配合脏矩形清除与延迟采样，
      // 渲染更快，故下限/余量较旧值收紧，让 Present 更贴近 vsync）。
      budgetMs = vsync.emaRenderMs + 1.0;
      if (budgetMs < 1.0) budgetMs = 1.0;
      if (budgetMs > 8.0) budgetMs = 8.0;
      const double budgetMax = periodMs * 0.6;
      if (budgetMs > budgetMax) budgetMs = budgetMax;
      ++vsync.frameCount;
      if (t1 > vsync.anchor) ++vsync.missed;  // 渲染在目标 vsync 后才完成 -> 错过
      if (vsync.frameCount % 3000 == 0) {
        DiagLog(L"[vsync] frames=%llu missed=%llu (%.1f%%), render EMA=%.2f ms, budget=%.2f ms",
                static_cast<unsigned long long>(vsync.frameCount),
                static_cast<unsigned long long>(vsync.missed),
                100.0 * static_cast<double>(vsync.missed) / vsync.frameCount,
                vsync.emaRenderMs, budgetMs);
      }
      if (vsync.frameCount % 1500 == 0) RefreshVsyncPeriod(vsync);
    } else {
      RenderOneFrame(true);  // Present(1,0)，vsync 阻塞节流
    }
  }
  timeEndPeriod(1);

  // 清理
  UnregisterHotKey(hwnd, kQuitHotkeyId);
  g_stopSampler.store(true, std::memory_order_relaxed);
  if (g_samplerThread) {
    WaitForSingleObject(g_samplerThread, 2000);
    CloseHandle(g_samplerThread);
  }
  g_cursorTex.bitmap.Reset();
  g_renderer.Shutdown();
  DestroyWindow(hwnd);
  DiagClose();
  return 0;
}
