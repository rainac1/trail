#include "cursor_sampler.h"

SpScRingBuffer<Sample, kRingCapacity> g_ring;
std::atomic<bool> g_stopSampler{false};
std::atomic<uint32_t> g_consumerIndex{0};
int g_sampleMs = 1;

DWORD WINAPI SamplerThreadMain(LPVOID /*param*/) {
  // 高优先级（非 TIME_CRITICAL，避免抢占 DWM/游戏线程造成卡顿）。
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

  // 提高系统计时器分辨率，使 Sleep(1) 接近 1ms。
  timeBeginPeriod(1);

  while (!g_stopSampler.load(std::memory_order_relaxed)) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    POINT pt;
    GetCursorPos(&pt);  // 开销约 0.5~1µs，1000Hz 下占用可忽略
    g_ring.Push(Sample{static_cast<uint64_t>(t.QuadPart), pt.x, pt.y}, g_consumerIndex);
    Sleep(static_cast<DWORD>(g_sampleMs));
  }

  timeEndPeriod(1);
  return 0;
}
