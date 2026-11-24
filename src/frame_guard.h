#pragma once
#include <windows.h>

#include <cstdint>
#include <vector>

// 单个显示器的 DWM 帧时间基准检测与稳定性守卫（Windows 11+，单线程、纯被动、零阻塞）。
//
// 职责：
//   1. 保存该显示器的标称与实测单帧周期 T_base，用作该屏的自适应一帧尾迹寿命。
//   2. 消费该显示器在 DWM 已完成帧中的独立目标统计（COMPOSITION_TARGET_STATS）：
//        - Active 状态：持续偏慢（VRR 降频/抖动、DRR 切频）或活跃 Commit 期间 DWM 持续掉帧时
//          切入 Paused，调用方立即隐藏该屏尾迹并停止提交。
//        - Paused 状态：完全不 Commit，仅被动观察该屏 DWM 新帧；看到持续贴合 T_base 的正常帧后
//          自动恢复 Active。
class FrameGuard {
 public:
  struct TargetId {
    LUID adapterLuid{};
    UINT vidPnSourceId = 0;
    UINT vidPnTargetId = 0;
    bool valid = false;
  };

  struct CompletedFrame {
    uint64_t startTime = 0;
    uint64_t completedTime = 0;
    uint64_t vblankDuration = 0;
    uint32_t refreshCount = 0;
  };

  bool Initialize(uint64_t qpcFreq, const wchar_t* tag, uint64_t nominalTicks,
                  uint64_t measuredTicks);

  // 清空旧的帧/提交游标与计分，保留基准 T_base。
  void ResetSession();

  // 丢弃 prevFrame_ 锚点与提交缓冲（帧序号跳跃较大时调用，不重置暂停态）。
  void ResetFrameAnchor();

  // 用指定已完成帧作为游标起点（不触发判分）。
  void SeedPrevFrame(const CompletedFrame& frame);
  bool hasPrevFrame() const { return hasPrevFrame_; }

  // 本程序在该屏成功 Commit 时调用（用于区分"鼠标慢移"与"真掉帧"）。
  void NotifyCommit(uint64_t commitQpc);

  // 消费该屏的一帧完成统计。仅当本次调用导致 paused 状态变化时返回 true。
  bool OnFrameSample(const CompletedFrame& curr);

  uint64_t baselineTicks() const { return baselineTicks_; }
  double baselineMs() const;
  bool paused() const { return paused_; }

  // 最近一次有效观测到的单刷新周期（毫秒，0 表示尚未观测到有效间隔；仅诊断日志用）。
  double lastMeasuredMs() const { return lastMeasuredMs_; }
  uint64_t pauseCount() const { return pauseCount_; }

 private:
  static constexpr uint32_t kCommitRingCapacity = 64;

  bool HasPendingCommitDropped(uint64_t prevStart, uint64_t currStart);
  void PurgeCommitsBefore(uint64_t cutoffQpc);
  void EnterPaused(const wchar_t* reason, uint64_t eventTime, double measMs, double vblankMs,
                   uint32_t dRef);
  void EnterActive(uint64_t eventTime, double measMs, double vblankMs);

  wchar_t tag_[32]{};
  uint64_t qpcFreq_ = 0;
  uint64_t baselineTicks_ = 0;
  uint64_t tolVblankMildTicks_ = 0;    // vblank 轻微偏慢容差：max(12% * T_base, 0.60ms)
  uint64_t tolSevereTicks_ = 0;        // 严重偏慢阈值（+2 分，仍需多帧确认）：max(25% * T_base, 1.50ms)
  uint64_t tolActiveRateTicks_ = 0;    // Active 跨帧持续偏慢容差：max(3% * T_base, 0.15ms)
  uint64_t tolActiveJitterTicks_ = 0;  // Active 端点抖动摊薄项（除以 dRef）：max(10% * T_base, 0.50ms)
  uint64_t tolResumeSingleTicks_ = 0;  // Paused 恢复单帧偏慢容差：max(6% * T_base, 0.35ms)
  uint64_t tolResumeMultiTicks_ = 0;   // Paused 恢复跨帧均长容差：max(1% * T_base, 0.08ms)
  uint64_t tolResumeJitterTicks_ = 0;  // Paused 恢复跨帧端点抖动摊薄项：max(2% * T_base, 0.16ms)

  CompletedFrame prevFrame_{};
  bool hasPrevFrame_ = false;

  // 本程序最近在该屏的提交时间戳环形缓冲（单调递增）。
  uint64_t commitRing_[kCommitRingCapacity]{};
  uint32_t commitHead_ = 0;
  uint32_t commitCount_ = 0;

  // 双态机状态（需多帧持续偏离才暂停，区分时序/VRR 与 DWM 掉帧，带指数退避恢复）
  bool paused_ = false;
  int timingBadScore_ = 0;
  int timingCleanStreak_ = 0;
  int dropBadScore_ = 0;
  int dropCleanStreak_ = 0;
  int goodSamples_ = 0;
  uint64_t goodRefreshes_ = 0;
  uint64_t pausedAtTime_ = 0;
  uint64_t lastResumedTime_ = 0;
  uint64_t holdoffTicks_ = 0;

  // 诊断统计
  double lastMeasuredMs_ = 0.0;
  uint64_t pauseCount_ = 0;
};

// 多显示器拓扑与分屏 FrameGuard 管理器。
//
// 一次 DCompositionGetFrameId / DCompositionGetStatistics 轮询即可获取所有显示器的
// 已完成帧目标统计，按 (adapterLuid, vidPnSourceId, vidPnTargetId) 分发到对应屏幕的
// 独立 FrameGuard，实现每屏独立的基准刷新率测定与 VRR/掉帧守卫。
class DisplayGuardManager {
 public:
  struct MonitorDesc {
    HMONITOR hmon = nullptr;
    RECT rect{};
    UINT dpi = 96;
    bool primary = false;
    wchar_t deviceName[CCHDEVICENAME]{};
    FrameGuard::TargetId target{};
    uint64_t nominalPeriodTicks = 0;
  };

  struct MonitorSlot {
    MonitorDesc desc{};
    FrameGuard guard{};
  };

  bool Initialize(uint64_t qpcFreq);

  // 重新枚举所有活动显示器（分辨率 / DPI / 刷新率 / 增删屏变化后调用）。
  bool RefreshTopology();

  // 轻量检查显示器列表（数量、句柄、矩形、DPI、主屏标记）是否发生变化。
  bool TopologyChanged() const;

  // 清空所有屏幕的帧游标与计分（启动建窗完成后或设备丢失重建后调用）。
  void ResetSession();

  // 通知指定屏幕发生了一次成功 Commit。
  void NotifyCommit(int monitorIndex, uint64_t commitQpc);

  // 统一轮询 DWM 新完成的帧并分发给各个屏幕的 FrameGuard。
  // 任意屏幕的 paused 状态发生变化时返回 true。
  bool Poll();

  int monitorCount() const { return static_cast<int>(monitors_.size()); }
  const MonitorSlot& monitor(int index) const { return monitors_[static_cast<size_t>(index)]; }
  int primaryMonitorIndex() const;

  // 查找包含物理像素坐标 (x, y) 的显示器下标；若坐标落在所有屏幕外返回 -1。
  int FindMonitorAt(int x, int y) const;
  // 若坐标不在任何屏幕内，退回离该点最近的显示器（没有显示器时返回 -1）。
  int FindMonitorOrNearest(int x, int y) const;

 private:
  int FindMonitorByTarget(LUID adapterLuid, UINT vidPnSourceId, UINT vidPnTargetId) const;
  void SeedPrevFrames(uint64_t latestFrameId);

  uint64_t qpcFreq_ = 0;
  std::vector<MonitorSlot> monitors_;
  uint64_t lastFrameId_ = 0;
  bool hasLastFrameId_ = false;
};
