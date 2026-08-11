#pragma once
// 无锁单生产者/单消费者（SPSC）环形缓冲。
// 写端（采样线程）：先写槽位，再 release-store 推进 head。
// 读端（渲染线程）：acquire-load 拿到 head，只读取 [0, head) 的槽位。
// release/acquire 配对保证：读端看到的槽位数据一定是写端已完成写入的。
#include <atomic>
#include <cstdint>

template <typename T, uint32_t Capacity>
class SpScRingBuffer {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

  std::atomic<uint32_t> head_{0};
  T data_[Capacity];

 public:
  // 生产者：覆盖最旧数据前先检查消费者游标，避免覆盖读端尚未消费的槽
  // （撕裂读）。消费者落后达到 Capacity-1 时视为缓冲已满，丢弃新点——
  // 帧内尾迹语义下超过一帧的旧点本就会被过滤，丢点无正确性损失。
  // consumer 为消费者的已消费游标（跨线程原子）。
  bool Push(const T& v, const std::atomic<uint32_t>& consumer) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    if (h - consumer.load(std::memory_order_relaxed) >= Capacity - 1) return false;
    data_[h & (Capacity - 1)] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // 消费者
  uint32_t ReadHead() const { return head_.load(std::memory_order_acquire); }
  const T& At(uint32_t index) const { return data_[index & (Capacity - 1)]; }
};
