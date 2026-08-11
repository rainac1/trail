#pragma once
#include <atomic>
#include <cstdint>
#include <windows.h>

#include "ring_buffer.h"

// 一次鼠标位置采样。
struct Sample {
  uint64_t t;  // QueryPerformanceCounter 时间戳（与渲染线程同一时钟域）
  int32_t x;   // 屏幕坐标（物理像素，虚拟桌面坐标系）
  int32_t y;
};

// 环形缓冲容量：4096 个采样点 @1000Hz 可容纳约 4 秒的卡顿余量。
constexpr uint32_t kRingCapacity = 4096;

// ---- 跨线程共享状态 ----
extern SpScRingBuffer<Sample, kRingCapacity> g_ring;
extern std::atomic<bool> g_stopSampler;
extern std::atomic<uint32_t> g_consumerIndex;  // 渲染线程已消费到的写入序号
extern int g_sampleMs;                         // 采样间隔（毫秒）

// 采样线程入口：高优先级轮询 GetCursorPos，写入环形缓冲。
DWORD WINAPI SamplerThreadMain(LPVOID param);
