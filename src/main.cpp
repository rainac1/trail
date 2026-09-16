// 帧内鼠标尾迹全屏透明叠加层
//
// 线程模型：
// 线程模型：
//   单线程 —— 窗口 + 消息循环 + 渲染（Present(0)，由调用方按 DWM 合成时钟对齐）。
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
// ---- 低延迟渲染：以 DWM 合成时钟为绝对时间基准（A+B，无采样、无回退）----
//
// 时间基准是 QueryPerformanceCounter；显示时钟不再靠 DwmFlush 采样，而是直接读
// DwmGetCompositionTimingInfo：
//   qpcRefreshPeriod = 刷新周期（QPC ticks）
//   qpcCompose       = 最近一次合成时刻（QPC）—— 以它作为相位栅格，因为要"赶在
//                      某次合成之前把帧交上去"；vblank 时刻比合成时刻晚一个固定
//                      相位（qpcVBlank - qpcCompose），该偏移由唤醒提前量 lead 覆盖
//   qpcVBlank / gpcCompose 栅格每帧重读，因此不存在自由推进的累积漂移
//
// 由此可以删掉：启动 11 次阻塞采样、每 1500 帧 5 次阻塞重测（会周期性掉帧）、
// 周期 EMA、相位预测，以及 timeBeginPeriod(1) + Sleep(1) 轮询 + 2ms 忙等。
// 读不到合成时钟或高分辨率定时器不可用即报错退出：本程序不做降级回退。
//
// ---- lead（唤醒提前量）策略：慢速需求基线 + 固定余量 ----
// lead 必须容纳的是"从唤醒目标到提交完成"的全部工作，即 t1 - target（唤醒抖动 +
// 渲染耗时），下面记作 needMs。
//
// needMs 是**重尾**的：实测均值稳定在 0.51~0.58 ms（跨会话、跨分辨率都如此），但单帧
// 峰值达 1.4~5.7 ms（OS 调度停顿）。也就是说"渲染成本"很稳，需要用余量兜的是"停顿"。
// 于是操作点就是两者之和：
//     lead = needMs 的慢速均值 + kLeadMarginMs
// 均值自适应机器 / 分辨率 / 驱动；余量留给调度停顿 —— 后者是调度器的属性，与渲染成本
// 无关，所以它不该"自适应"。
//
// 关键是**均值取得足够慢**：基线 τ ≈ 2000 帧（≈17s），单次 4 ms 尖峰只把它抬高
// 0.0005 × 3.5 ≈ 0.002 ms，而真正持续的变化（换分辨率 / 设备）仍在几秒内跟上。
//
// 之所以不做"按瞬时 need 追高"（初版 S3）：那会让下行目标远低于维持低漏帧率所需的
// 水平，系统变成限环 —— 实测 lead 在 1.16 ↔ 2.15 ms 之间来回锯。目标平滑之后跟随器
// 就不会振荡，上下行速率也就退化成无关紧要的细节（这里只用来平滑启动台阶）。
//
// 启动期单独处理：会话开头几百帧是冷启动瞬态（实测会话最大的停顿总是落在这里，达
// 4~6 ms），所以前 kNeedWarmupFrames 帧不采样、lead 保持 kLeadStartMs；随后
// kNeedSeedSamples 个样本用精确 running mean 稀释单个尖峰，再转入慢速 EMA。
// 早期版本直接用第一个样本初始化基线，结果一个 4.19 ms 的冷启动尖峰把操作点抬高了
// 0.8 ms 并持续约 5000 帧（实测第 3000 帧时 baseline=1.33 ms）。
// 实测的"margin（≈ 平均头部延迟）→ 漏帧率"曲线（12849 帧，参考机 120Hz，见
// docs/architecture.md）：
//     1.20ms→0.06%   0.90→0.23%   0.75→0.40%   0.60→0.69%   0.45→1.35%   0.30→3.2%
// 每降 0.15 ms 漏帧率约翻一倍 —— 重尾分布没有平坦区，所以不存在"再压一点也没事"。
// 而且尾部不平稳：同一 margin 在不同 25 秒窗口里能差一个数量级，选值时看最差窗口
// 而不是均值。取 0.9 是这里的折中（平均头部延迟 0.9 ms、平均漏帧 0.23%、最差窗口 0.43%）。
// 调这个值不必逐次试跑：[clock] margin probe 行每次运行都会打出上面这条曲线。
constexpr double kLeadStartMs = 2.0;       // 预热期使用的 lead（实测该值漏帧率 ~0.05%）
constexpr double kLeadMarginMs = 0.9;      // 操作点余量：lead = 基线 + 它（唯一旋钮）
constexpr double kNeedSlowAlpha = 0.0005;  // 基线 EMA 系数（τ ≈ 2000 帧 ≈ 16.7s @120Hz）
constexpr int kNeedWarmupFrames = 600;     // 预热帧数（≈5s @120Hz）：冷启动瞬态不采样
constexpr int kNeedSeedSamples = 120;      // 播种样本数（≈1s @120Hz）：精确 running mean
constexpr double kMaxAttackStepMs = 0.25;  // 上行单帧步长上限
constexpr double kReleaseAlpha = 0.02;     // 下行释放系数（τ ≈ 50 帧 ≈ 0.4s @120Hz）
constexpr double kLeadMinMs = 1.0;         // lead 下限
constexpr double kLeadMaxMs = 8.0;         // lead 上限（另受周期 60% 限制）

// margin 探针：统计 (needMs - 基线) 超过各候选 margin 的帧占比。因为漏帧条件正是
// needMs > lead = 基线 + margin，所以**每个数字就是"把 kLeadMarginMs 取成该值会得到
// 多少漏帧率"**。这样一次运行就能读出整条"margin → 漏帧率"曲线，不必逐次改参数试跑。
constexpr double kMarginProbeMs[] = {0.30, 0.45, 0.60, 0.75, 0.90, 1.20};
constexpr int kMarginProbeCount = 6;

struct VsyncState {
  uint64_t period = 0;             // 合成刷新周期（QPC ticks）
  uint64_t deadline = 0;           // 本帧瞄准的合成时刻（QPC）
  double budgetMs = kLeadStartMs;  // 当前生效的 lead
  double needSlowMs = 0.0;         // needMs 的慢速基线（操作点的自适应部分）
  int needWarmupLeft = kNeedWarmupFrames;  // 预热剩余帧数
  int needSeedCount = 0;                   // 已播种的样本数
  uint64_t frameCount = 0;
  uint64_t missed = 0;  // 渲染在目标合成时刻之后才完成的次数
  // 每 3000 帧汇总一次的诊断量（用于核对 lead：唤醒是否偏晚、完成后余量是否够）
  // 注意：*Sum 每个汇总块都会被清零，所以均值必须除以 statFrames（本块帧数），不能
  // 除以累计的 frameCount —— 否则第 2 块之后报出的均值会成倍偏小（老代码的 bug）。
  uint64_t statFrames = 0;
  uint64_t wakeSum = 0;  // t0 - target 之和（唤醒落后于 target 的量）
  uint64_t wakeMax = 0;
  int64_t slackSum = 0;  // deadline - t1 之和（正 = 赶在目标合成之前完成）
  int64_t slackMin = 0;
  uint64_t needSum = 0;  // t1 - target 之和（= 唤醒抖动 + 渲染耗时）
  double needMaxMs = 0.0;
  // margin 探针（见 kMarginProbeMs）：播种完成后才开始计数
  uint64_t probeFrames = 0;
  uint64_t probeOver[kMarginProbeCount] = {};
  uint64_t clockReadSum = 0;  // DwmGetCompositionTimingInfo 调用耗时（每帧一次查询）
  uint64_t clockReadMax = 0;
};
static VsyncState g_vsync;
static uint64_t g_qpcFreq = 0;    // 缓存的 QPC 频率（每帧换算都要用）
static HANDLE g_timer = nullptr;  // 高分辨率可等待定时器（B）
static HRESULT g_clockError = S_OK;  // 时钟/定时器失败原因（用于退出时报告）

static uint64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

static uint64_t MsToTicks(double ms) {
  return static_cast<uint64_t>(ms * static_cast<double>(g_qpcFreq) / 1000.0);
}

static double TicksToMs(uint64_t ticks) {
  return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_qpcFreq);
}

static double TicksToUs(uint64_t ticks) {
  return static_cast<double>(ticks) * 1000000.0 / static_cast<double>(g_qpcFreq);
}

static double TicksToUsI(int64_t ticks) {
  return static_cast<double>(ticks) * 1000000.0 / static_cast<double>(g_qpcFreq);
}

// A：读一次 DWM 合成时钟。字段不合法（含未初始化）一律视为失败，由调用方报错。
static bool ReadCompositionClock(DWM_TIMING_INFO& out) {
  out = DWM_TIMING_INFO{};
  out.cbSize = sizeof(out);
  const HRESULT hr = DwmGetCompositionTimingInfo(nullptr, &out);
  if (FAILED(hr)) {
    g_clockError = hr;
    return false;
  }
  if (out.qpcRefreshPeriod == 0 || out.qpcCompose == 0) {
    g_clockError = E_UNEXPECTED;
    return false;
  }
  return true;
}

// 建立时间基础设施：缓存 QPC 频率、校验合成时钟、创建高分辨率可等待定时器。
// 失败返回 false 并把退出原因写入 message。
static bool InitTiming(wchar_t* message, size_t messageCount) {
  LARGE_INTEGER freq{};
  if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
    swprintf_s(message, messageCount, L"QueryPerformanceFrequency 失败。\n\n诊断日志: %ls",
               DiagLogPath());
    return false;
  }
  g_qpcFreq = static_cast<uint64_t>(freq.QuadPart);

  DWM_TIMING_INFO ti{};
  if (!ReadCompositionClock(ti)) {
    swprintf_s(message, messageCount,
               L"无法读取 DWM 合成时钟（DwmGetCompositionTimingInfo 失败，HRESULT=0x%08X）。\n"
               L"本程序以该时钟为唯一时间基准，不做降级回退。\n"
               L"（桌面合成 / DWM 未启用时会出现这种情况。）\n\n诊断日志: %ls",
               static_cast<unsigned>(g_clockError), DiagLogPath());
    return false;
  }
  const double periodMs = TicksToMs(ti.qpcRefreshPeriod);
  if (periodMs < 0.5 || periodMs > 500.0) {  // 2Hz~2000Hz 之外视为异常值
    g_clockError = E_UNEXPECTED;
    swprintf_s(message, messageCount,
               L"DWM 合成时钟给出的刷新周期异常（%.3f ms），拒绝作为时间基准。\n"
               L"本程序不做降级回退。\n\n诊断日志: %ls",
               periodMs, DiagLogPath());
    return false;
  }
  g_vsync.period = ti.qpcRefreshPeriod;
  // qpcVBlank-now 的正负可判断栅格语义：为正说明是"即将到来的"那次合成（本机实测
  // qpcVBlank == qpcCompose 且为正值），为负则是"刚过去的"那次。
  DiagLog(L"[clock] DWM composition clock: period=%.3f ms (rateRefresh=%u/%u), "
          L"qpcVBlank-qpcCompose=%.3f ms, qpcVBlank-now=%+.3f ms",
          periodMs, ti.rateRefresh.uiNumerator, ti.rateRefresh.uiDenominator,
          TicksToMs(ti.qpcVBlank - ti.qpcCompose),
          static_cast<double>(static_cast<int64_t>(ti.qpcVBlank) - static_cast<int64_t>(QpcNow())) *
              1000.0 / static_cast<double>(g_qpcFreq));

  // B：高分辨率可等待定时器（Windows 10 1803+）。它替代了 timeBeginPeriod(1)：
  // 后者改的是全系统定时器分辨率，且 Sleep 精度只有 ~1ms、过冲可达 1.5ms。
  g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
  if (!g_timer) {
    const DWORD err = GetLastError();
    g_clockError = HRESULT_FROM_WIN32(err);
    swprintf_s(message, messageCount,
               L"高分辨率可等待定时器创建失败（error=%lu），需要 Windows 10 1803+。\n"
               L"本程序不做降级回退（不会退回 Sleep 轮询）。\n\n诊断日志: %ls",
               err, DiagLogPath());
    return false;
  }
  return true;
}

// lead 的统一限幅：上界 min(kLeadMaxMs, 60% 周期) 随刷新率变化，所以每帧按当前
// period 现算；原先分散在帧循环里的两次 clamp 合并到这一处。
static double EffectiveLeadMs() {
  const double periodMs = g_vsync.period ? TicksToMs(g_vsync.period) : kLeadMaxMs;
  double hi = periodMs * 0.6;
  if (hi > kLeadMaxMs) hi = kLeadMaxMs;
  if (hi < kLeadMinMs) hi = kLeadMinMs;  // 极高刷新率下保证下限不高于上限
  double lead = g_vsync.budgetMs;
  if (lead < kLeadMinMs) lead = kLeadMinMs;
  if (lead > hi) lead = hi;
  return lead;
}

// 推出本帧的等待目标：把绝对合成栅格 qpcCompose + k*period 推进到"提前 lead 之后
// 仍在未来"的最近一格，使渲染恰好在这次合成之前完成提交。
//
// 必须要求 next - lead > now（而不是 next > now）：DWM 报出的合成/vblank 时刻可能是
// *即将到来* 的那一次（实测 qpcVBlank == qpcCompose），若只要求 next > now，那么当
// now 落在 (next-lead, next) 区间时目标会落在过去，于是同一格里连续空转 —— 实测会
// 退化到约 4.7 次渲染/显示帧。按尾迹语义（每次渲染消费一次鼠标历史水印），这会让
// 屏幕上的尾迹只有应有的约 1/4 长，所以必须保证每格恰好渲染一次。
// 起步已晚（目标在过去）时本函数仍会给出未来目标，不再"立即追赶"：错过一格后立即
// 渲染并不会让画面提前，只会白丢掉一段采样。
static bool NextFrameTarget(uint64_t& outTarget) {
  DWM_TIMING_INFO ti{};
  if (!ReadCompositionClock(ti)) return false;
  // period 每帧都重读，不做"是否变化"的判断：qpcRefreshPeriod 由刷新率比例换算而来，
  // 有 tick 级抖动，精确相等会每帧都判为变化。刷新率/显示模式的变化会自动反映到
  // 60% 周期的 lead 上限与尾迹语义里。
  g_vsync.period = ti.qpcRefreshPeriod;
  const uint64_t period = ti.qpcRefreshPeriod;
  const uint64_t now = QpcNow();
  const uint64_t lead = MsToTicks(EffectiveLeadMs());
  uint64_t next = ti.qpcCompose;
  const uint64_t earliest = now + lead;  // 目标(target)必须严格晚于 now
  if (next <= earliest) next += period * ((earliest - next) / period + 1);
  g_vsync.deadline = next;
  outTarget = next - lead;
  return true;
}

// B：等到 target。粗睡用高分辨率可等待定时器（相对时间），最后 spinMs 交给自旋，
// 用极短的忙等吸收定时器唤醒抖动。自旋窗口从原先的 2ms 缩到 0.5ms。
//
// 单片上限 sliceMs：单次长睡（例如 7ms）会让 CPU 进入较深空闲状态，唤醒延迟偶发
// 达到 4~7ms（实测 wake max），足以吃掉 lead 的余量而错过一次合成。切成 ≤2ms 的片
// 可显著降低这种长尾（旧方案靠 timeBeginPeriod(1) 每毫秒醒一次恰好避开了它）。
static bool WaitUntil(uint64_t target) {
  const uint64_t spinTicks = MsToTicks(0.5);
  const uint64_t sliceTicks = MsToTicks(2.0);
  for (;;) {
    const uint64_t now = QpcNow();
    if (now >= target) return true;
    const uint64_t remain = target - now;
    if (remain <= spinTicks) {
      YieldProcessor();
      continue;
    }
    uint64_t sleepTicks = remain - spinTicks;
    if (sleepTicks > sliceTicks) sleepTicks = sliceTicks;
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(sleepTicks * 10000000ULL / g_qpcFreq);
    if (due.QuadPart >= 0) due.QuadPart = -1;  // 负数 = 相对时间（100ns 单位）
    if (!SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
      g_clockError = HRESULT_FROM_WIN32(GetLastError());
      return false;
    }
    WaitForSingleObject(g_timer, INFINITE);
  }
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

OverlayRenderer::FrameResult RenderOneFrame() {
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
                                /*drawLiveHead=*/cursorBmp != nullptr);
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
  // 时间基准 = QPC，显示时钟直接读 DWM 合成时钟（A），粗睡用高分辨率可等待定时器
  // 加短自旋（B）。二者缺一即报错退出 —— 本程序不做降级回退（没有 Present(1,0)
  // 之类的备用呈现方式，也没有 Sleep 轮询备用等待方式）。
  wchar_t timingError[640] = {};
  if (!InitTiming(timingError, ARRAYSIZE(timingError))) {
    DiagFatal(L"Trail", timingError);
    UnregisterHotKey(hwnd, kQuitHotkeyId);
    g_renderer.Shutdown();
    DestroyWindow(hwnd);
    DiagClose();
    return 1;
  }
  // lead 从 VsyncState::budgetMs 起始，之后按"慢速需求基线 + 固定余量"自适应
  // （见文件头策略说明）。
  DiagLog(L"[clock] lead policy: start=%.2f ms, margin=%.2f ms (operating point), "
          L"baseline tau ~ %.0f frames, warmup %d frames, seed %d samples, "
          L"follower attack<=%.2f ms/frame, release alpha=%.3f",
          kLeadStartMs, kLeadMarginMs, 1.0 / kNeedSlowAlpha, kNeedWarmupFrames,
          kNeedSeedSamples, kMaxAttackStepMs, kReleaseAlpha);
  // 提升渲染线程（主线程）优先级：截止时刻对齐对调度抖动敏感，普通优先级下渲染易被
  // 抢占而错过目标合成。用 HIGHEST 而非 TIME_CRITICAL，避免抢占 DWM / 游戏线程。
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

  MSG msg{};
  bool running = true;
  int exitCode = 0;
  wchar_t fatalError[640] = {};
  uint64_t loopCount = 0;
  // 重建失败的退避：以 QPC 计时而非帧计数（丢失期间不渲染，循环节奏与帧率无关），
  // 从 500ms 起逐次加倍，成功后复位。
  int recreateBackoffMs = 500;
  uint64_t nextRecreateTicks = 0;  // 下次尝试重建的时刻（0 = 立即尝试）
  uint64_t lastStatTicks = QpcNow();  // 上次打统计行的时刻（用于算 fps）

  // 汇总并打印自上次调用以来的统计块。每 3000 帧调用一次；退出时再补一次不完整的
  // 块 —— 否则试跑不到 3000 帧（25 秒）就一行统计都没有，短测拿不到任何数据。
  // fps 用本块帧数折算，因此整块与残块都正确。
  const auto logStats = [&]() {
    if (g_vsync.statFrames == 0) return;
    const double n = static_cast<double>(g_vsync.frameCount);  // 累计帧数：missed% 用
    // 均值必须除以本块帧数，见 VsyncState 里的说明。
    const double bn = static_cast<double>(g_vsync.statFrames);
    const uint64_t statNow = QpcNow();
    const double statMs = TicksToMs(statNow - lastStatTicks);
    lastStatTicks = statNow;
    DiagLog(L"[clock] frames=%llu fps=%.1f missed=%llu (%.1f%%), lead=%.2f ms "
            L"(baseline=%.2f), need mean=%.0f max=%.0f us | wake mean=%.0f max=%.0f us, "
            L"slack mean=%.0f min=%.0f us, clockRead mean=%.0f max=%.0f us",
            static_cast<unsigned long long>(g_vsync.frameCount), bn * 1000.0 / statMs,
            static_cast<unsigned long long>(g_vsync.missed),
            100.0 * static_cast<double>(g_vsync.missed) / n, EffectiveLeadMs(),
            g_vsync.needSlowMs,
            TicksToUs(g_vsync.needSum) / bn, g_vsync.needMaxMs * 1000.0,
            TicksToUs(g_vsync.wakeSum) / bn, TicksToUs(g_vsync.wakeMax),
            TicksToUsI(g_vsync.slackSum) / bn, TicksToUsI(g_vsync.slackMin),
            TicksToUs(g_vsync.clockReadSum) / bn, TicksToUs(g_vsync.clockReadMax));
    if (g_vsync.probeFrames > 0) {
      const double pn = static_cast<double>(g_vsync.probeFrames);
      DiagLog(L"[clock] margin probe over %llu frames (need-baseline > x): "
              L"0.30=%.2f%% 0.45=%.2f%% 0.60=%.2f%% 0.75=%.2f%% 0.90=%.2f%% 1.20=%.2f%%",
              static_cast<unsigned long long>(g_vsync.probeFrames),
              100.0 * static_cast<double>(g_vsync.probeOver[0]) / pn,
              100.0 * static_cast<double>(g_vsync.probeOver[1]) / pn,
              100.0 * static_cast<double>(g_vsync.probeOver[2]) / pn,
              100.0 * static_cast<double>(g_vsync.probeOver[3]) / pn,
              100.0 * static_cast<double>(g_vsync.probeOver[4]) / pn,
              100.0 * static_cast<double>(g_vsync.probeOver[5]) / pn);
    }
    g_vsync.probeFrames = 0;
    for (int i = 0; i < kMarginProbeCount; ++i) g_vsync.probeOver[i] = 0;
    g_vsync.statFrames = 0;
    g_vsync.wakeSum = 0;
    g_vsync.wakeMax = 0;
    g_vsync.needSum = 0;
    g_vsync.needMaxMs = 0.0;
    g_vsync.slackSum = 0;
    g_vsync.slackMin = 0;
    g_vsync.clockReadSum = 0;
    g_vsync.clockReadMax = 0;
  };

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
        DWORD ms = static_cast<DWORD>(remainTicks * 1000ULL / g_qpcFreq);
        if (ms > 50) ms = 50;
        Sleep(ms > 0 ? ms : 1);
        continue;
      }
      if (RecreateRenderer(hwnd, vw, vh)) {
        g_deviceLost = false;
        nextRecreateTicks = 0;
        recreateBackoffMs = 500;
        // 无需重测 vsync：合成时钟每帧都会重读（NextFrameTarget），显示模式/刷新率
        // 变化会在下一帧自动反映出来。
      } else {
        nextRecreateTicks = nowTicks + static_cast<uint64_t>(recreateBackoffMs) * g_qpcFreq / 1000ULL;
        recreateBackoffMs = (recreateBackoffMs >= 8000) ? 15000 : recreateBackoffMs * 2;
        continue;
      }
    }

    // ---- 算出本帧的截止时刻并等到它 ----
    // 时钟读不到、或定时器等待失败：不降级、不空转，直接报错结束进程。
    uint64_t target = 0;
    const uint64_t tClock0 = QpcNow();
    const bool haveTarget = NextFrameTarget(target);
    const uint64_t tClock1 = QpcNow();
    g_vsync.clockReadSum += tClock1 - tClock0;
    if (tClock1 - tClock0 > g_vsync.clockReadMax) g_vsync.clockReadMax = tClock1 - tClock0;
    if (!haveTarget) {
      DiagLog(L"[clock] DwmGetCompositionTimingInfo failed at runtime: 0x%08X",
              static_cast<unsigned>(g_clockError));
      swprintf_s(fatalError, ARRAYSIZE(fatalError),
                 L"运行时无法读取 DWM 合成时钟（HRESULT=0x%08X）。\n"
                 L"本程序不做降级回退，已结束进程。\n\n诊断日志: %ls",
                 static_cast<unsigned>(g_clockError), DiagLogPath());
      exitCode = 1;
      break;
    }
    if (!WaitUntil(target)) {
      DiagLog(L"[clock] SetWaitableTimerEx failed at runtime: 0x%08X",
              static_cast<unsigned>(g_clockError));
      swprintf_s(fatalError, ARRAYSIZE(fatalError),
                 L"高分辨率定时器等待失败（HRESULT=0x%08X）。\n"
                 L"本程序不做降级回退，已结束进程。\n\n诊断日志: %ls",
                 static_cast<unsigned>(g_clockError), DiagLogPath());
      exitCode = 1;
      break;
    }

    const uint64_t t0 = QpcNow();
    const OverlayRenderer::FrameResult fr = RenderOneFrame();
    const uint64_t t1 = QpcNow();
    if (fr == OverlayRenderer::FrameResult::RecreateDevice) g_deviceLost = true;

    // ---- lead：慢速需求基线 + 固定余量（见文件头策略说明）----
    // 需求信号 = t1 - target = 唤醒抖动 + 渲染耗时，也就是 lead 必须容纳的全部工作。
    const double needMs = TicksToMs(t1 - target);
    g_vsync.needSum += t1 - target;
    if (needMs > g_vsync.needMaxMs) g_vsync.needMaxMs = needMs;
    // 只采信真正提交成功的帧：Failed / RecreateDevice 会提前返回，耗时系统性偏低。
    if (fr == OverlayRenderer::FrameResult::Ok) {
      if (g_vsync.needWarmupLeft > 0) {
        --g_vsync.needWarmupLeft;  // 预热期：不采样，lead 保持 kLeadStartMs
      } else {
        if (g_vsync.needSeedCount < kNeedSeedSamples) {
          // 播种期：精确 running mean，单个尖峰被样本数稀释。
          ++g_vsync.needSeedCount;
          g_vsync.needSlowMs += (needMs - g_vsync.needSlowMs) / g_vsync.needSeedCount;
        } else {
          g_vsync.needSlowMs += kNeedSlowAlpha * (needMs - g_vsync.needSlowMs);
        }
        // 操作点 = 基线 + 余量。目标本身平滑，所以上下行速率不再影响稳定性。
        const double targetMs = g_vsync.needSlowMs + kLeadMarginMs;
        const double diff = targetMs - g_vsync.budgetMs;
        if (diff > 0) {
          g_vsync.budgetMs += (diff > kMaxAttackStepMs) ? kMaxAttackStepMs : diff;
        } else {
          g_vsync.budgetMs += kReleaseAlpha * diff;
        }
        // margin 探针：播种完成后才有意义（播种期基线尚未收敛）。
        if (g_vsync.needSeedCount >= kNeedSeedSamples) {
          const double excess = needMs - g_vsync.needSlowMs;
          ++g_vsync.probeFrames;
          for (int i = 0; i < kMarginProbeCount; ++i) {
            if (excess > kMarginProbeMs[i]) ++g_vsync.probeOver[i];
          }
        }
      }
    }
    ++g_vsync.frameCount;

    // 错过：渲染在目标合成时刻之后才完成（即这次合成没赶上）。唤醒比 target 晚几十
    // 微秒不算错过 —— lead 里本来就留了余量，只要余量没被吃完就仍然赶得上。
    if (t1 > g_vsync.deadline) ++g_vsync.missed;

    // 诊断累计：唤醒落后 target 多少、渲染完成后距目标合成时刻还有多少余量。
    {
      ++g_vsync.statFrames;
      const uint64_t wakeLate = t0 - target;
      g_vsync.wakeSum += wakeLate;
      if (wakeLate > g_vsync.wakeMax) g_vsync.wakeMax = wakeLate;
      const int64_t slack = static_cast<int64_t>(g_vsync.deadline) - static_cast<int64_t>(t1);
      g_vsync.slackSum += slack;
      if (slack < g_vsync.slackMin) g_vsync.slackMin = slack;
    }

    if (g_vsync.frameCount % 3000 == 0) logStats();
  }
  logStats();  // 收尾：把退出前那个不完整的块也打出来
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
  if (g_timer) {
    CloseHandle(g_timer);
    g_timer = nullptr;
  }

  // 清理
  UnregisterHotKey(hwnd, kQuitHotkeyId);
  g_cursorTex.bitmap.Reset();
  g_renderer.Shutdown();
  DestroyWindow(hwnd);
  // 运行时失败在窗口销毁之后再报错，避免弹窗被顶层叠加层盖住。
  if (exitCode != 0) DiagFatal(L"Trail", fatalError);
  DiagClose();
  return exitCode;
}
