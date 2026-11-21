#pragma once
#include <windows.h>

#include <cstdint>

// DWM 帧时间基准检测与稳定性守卫（单线程、纯被动、零阻塞）。
//
// 职责：
//   1. 启动时测定主显示器的基准单帧周期 T_base（结合 QueryDisplayConfig 标称刷新率与
//      DCompositionGetTargetStatistics / DwmGetCompositionTimingInfo 晶振实测周期），
//      用作自适应的一帧尾迹寿命。
//   2. 运行期在主循环已唤醒时顺手消费 DWM 已完成帧的历史环形缓冲（Win11
//      DCompositionGetFrameId / DCompositionGetStatistics / DCompositionGetTargetStatistics，
//      动态加载，Win10 退回 DwmGetCompositionTimingInfo）：
//        - Active 状态：连续偏离 T_base（VRR 抖动/锁低频、DRR 切频、活跃 Commit 期间 DWM
//          掉帧）时切入 Paused，调用方立即 HideAll 并停止一切 Commit。
//        - Paused 状态：完全不 Commit，仅被动观察 DWM 新帧；无新帧则保持 Paused，看到连续
//          贴合 T_base 的正常帧后自动恢复 Active。
class FrameGuard {
 public:
  bool Initialize(uint64_t qpcFreq);

  // 设备丢失重建后清空旧的帧/提交游标，保留启动基准 T_base。
  void ResetSession();

  // 主循环每次成功向 DComp 提交视觉树变化时调用（Active 状态下用于区分"鼠标慢移"和"真掉帧"）。
  void NotifyCommit(uint64_t commitQpc);

  // 被动消费 DWM 新完成的帧并更新状态机。仅当本次调用导致 paused 状态变化时返回 true。
  bool Poll();

  uint64_t baselineTicks() const { return baselineTicks_; }
  double baselineMs() const;
  bool paused() const { return paused_; }
  bool usingDcompStats() const { return hasDcompStats_; }

  // 最近一次观测到的单刷新周期（毫秒，0 表示尚未观测到有效间隔；仅诊断日志用）。
  double lastMeasuredMs() const { return lastMeasuredMs_; }
  uint64_t pauseCount() const { return pauseCount_; }

 private:
  static constexpr uint32_t kCommitRingCapacity = 64;

  struct PrimaryTarget {
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

  void LoadDcompStatsApi();
  uint64_t QueryPrimaryDisplayConfig(PrimaryTarget& outTarget) const;
  uint64_t ProbeInitialPeriodTicks(uint64_t nominalTicks);
  bool ReadCompletedFrame(uint64_t frameId, CompletedFrame& out) const;
  bool HasPendingCommitDropped(uint64_t prevStart, uint64_t currStart);
  void PurgeCommitsBefore(uint64_t cutoffQpc);
  bool PollDcompStats();
  bool PollDwmFallback();
  bool OnFrameSample(const CompletedFrame& curr);
  void EnterPaused(const wchar_t* reason, uint64_t eventTime, double measMs, double vblankMs,
                   uint32_t dRef);
  void EnterActive(uint64_t eventTime, double measMs, double vblankMs);

  // 动态加载的 Win11 DirectComposition 统计 API（避免 Win10 启动缺导出符号）。
  void* fnGetFrameId_ = nullptr;
  void* fnGetStatistics_ = nullptr;
  void* fnGetTargetStatistics_ = nullptr;
  bool hasDcompStats_ = false;

  uint64_t qpcFreq_ = 0;
  uint64_t baselineTicks_ = 0;
  uint64_t tolSingleTicks_ = 0;  // 单帧轻微偏离容差：max(8% * T_base, 0.5ms)
  uint64_t tolSevereTicks_ = 0;  // 单帧严重偏离阈值（1 帧即停）：max(20% * T_base, 1.5ms)
  uint64_t tolMultiTicks_ = 0;   // 跨帧均长容差（dRef >= 2）：max(1% * T_base, 0.08ms)

  PrimaryTarget primaryTarget_{};

  uint64_t lastFrameId_ = 0;
  bool hasLastFrameId_ = false;
  CompletedFrame prevFrame_{};
  bool hasPrevFrame_ = false;

  // 本程序最近提交时间戳环形缓冲（单调递增），用于判断 dRef >= 2 时是否确有未合成提交。
  uint64_t commitRing_[kCommitRingCapacity]{};
  uint32_t commitHead_ = 0;
  uint32_t commitCount_ = 0;

  // 双态机状态（非对称迟滞：快停、稳恢复 + 快速复发指数退避）
  bool paused_ = false;
  int badScore_ = 0;
  int activeCleanStreak_ = 0;
  int goodSamples_ = 0;
  uint64_t goodRefreshes_ = 0;
  uint64_t pausedAtTime_ = 0;
  uint64_t lastResumedTime_ = 0;
  uint64_t holdoffTicks_ = 0;

  // 诊断统计
  double lastMeasuredMs_ = 0.0;
  uint64_t pauseCount_ = 0;
};
