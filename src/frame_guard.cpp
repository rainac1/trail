#include "frame_guard.h"

#include <dcomp.h>

#include <algorithm>
#include <vector>

#include "diag.h"

namespace {

constexpr uint64_t kMaxFramesPerPoll = 64;
constexpr int kTimingPauseThreshold = 4;     // VRR/帧周期异常：严重偏离(+2)需 2 帧，轻微(+1)需 4 帧，拒绝单帧毛刺
constexpr int kTimingDecayStreak = 2;        // 连续 2 个正常帧衰减 1 分（仍能抓住 50% 交替抖动）
constexpr int kDropPauseThreshold = 6;       // 持续 DWM 掉帧：连续 6 帧 dRef=2（~100ms）或 3 帧 dRef>=3 才暂停
constexpr int kDropDecayStreak = 2;          // 连续 2 个无掉帧样本衰减 1 分，避免偶发单帧跳过触发暂停
constexpr uint32_t kCleanIdleResetRefs = 12; // 跨越 >=12 次正常刷新后清零陈旧的历史计分
constexpr int kResumeGoodSamples = 6;        // 活跃桌面恢复需连续 6 个正常样本（且过完冷却期）
constexpr int kResumeMinSamplesForSpan = 2;  // 静止桌面帧稀疏时：至少 2 个正常样本且覆盖 >=24 次刷新即可恢复
constexpr uint64_t kResumeGoodRefreshes = 24;
constexpr uint64_t kInitialHoldoffMs = 500;
constexpr uint64_t kMaxHoldoffMs = 4000;
constexpr uint64_t kRapidRetripWindowMs = 2000;
constexpr UINT kStackTargetLimit = 16;

uint64_t AbsDiff(uint64_t a, uint64_t b) {
  return (a >= b) ? (a - b) : (b - a);
}

double TicksToMs(uint64_t ticks, uint64_t qpcFreq) {
  if (qpcFreq == 0) return 0.0;
  return static_cast<double>(ticks) * 1000.0 / static_cast<double>(qpcFreq);
}

bool SameLuid(const LUID& a, const LUID& b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

bool IsSanePeriodTicks(uint64_t ticks, uint64_t qpcFreq) {
  // 合理刷新率范围：10 Hz (100ms) ~ 1000 Hz (1ms)。
  const uint64_t minTicks = qpcFreq / 1000ULL;
  const uint64_t maxTicks = qpcFreq / 10ULL;
  return ticks >= minTicks && ticks <= maxTicks;
}

}  // namespace

double FrameGuard::baselineMs() const {
  return TicksToMs(baselineTicks_, qpcFreq_);
}

uint64_t FrameGuard::QueryPrimaryDisplayConfig(PrimaryTarget& outTarget) const {
  outTarget = PrimaryTarget{};

  for (int attempt = 0; attempt < 3; ++attempt) {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG err = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (err != ERROR_SUCCESS || pathCount == 0) break;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    err = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                             modes.data(), nullptr);
    if (err == ERROR_INSUFFICIENT_BUFFER) continue;
    if (err != ERROR_SUCCESS) break;

    for (UINT32 i = 0; i < pathCount; ++i) {
      const DISPLAYCONFIG_PATH_INFO& p = paths[i];
      if (p.sourceInfo.modeInfoIdx >= modeCount) continue;
      const DISPLAYCONFIG_MODE_INFO& srcMode = modes[p.sourceInfo.modeInfoIdx];
      if (srcMode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
      if (srcMode.sourceMode.position.x != 0 || srcMode.sourceMode.position.y != 0) continue;

      outTarget.adapterLuid = p.targetInfo.adapterId;
      outTarget.vidPnSourceId = p.sourceInfo.id;
      outTarget.vidPnTargetId = p.targetInfo.id;
      outTarget.valid = true;

      const DISPLAYCONFIG_RATIONAL& r = p.targetInfo.refreshRate;
      if (r.Numerator > 0 && r.Denominator > 0) {
        const double ticks = static_cast<double>(qpcFreq_) * static_cast<double>(r.Denominator) /
                             static_cast<double>(r.Numerator);
        const uint64_t period = static_cast<uint64_t>(ticks + 0.5);
        if (IsSanePeriodTicks(period, qpcFreq_)) return period;
      }
      break;
    }
    break;
  }

  // DisplayConfig 不可用时的退路：EnumDisplaySettingsW 仅提供整数 Hz。
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 10 &&
      dm.dmDisplayFrequency <= 1000) {
    return qpcFreq_ / static_cast<uint64_t>(dm.dmDisplayFrequency);
  }
  return 0;
}

bool FrameGuard::ReadCompletedFrame(uint64_t frameId, CompletedFrame& out) const {
  COMPOSITION_FRAME_STATS fs{};
  COMPOSITION_TARGET_ID tids[kStackTargetLimit]{};
  UINT actualTargets = 0;
  HRESULT hr = DCompositionGetStatistics(frameId, &fs, kStackTargetLimit, tids, &actualTargets);
  if (FAILED(hr) || actualTargets == 0) return false;

  const UINT count = std::min(actualTargets, kStackTargetLimit);
  UINT matched = count;
  if (primaryTarget_.valid) {
    for (UINT i = 0; i < count; ++i) {
      if (SameLuid(tids[i].displayAdapterLuid, primaryTarget_.adapterLuid) &&
          tids[i].vidPnSourceId == primaryTarget_.vidPnSourceId &&
          tids[i].vidPnTargetId == primaryTarget_.vidPnTargetId) {
        matched = i;
        break;
      }
    }
  } else {
    matched = 0;
  }
  if (matched >= count) return false;

  COMPOSITION_TARGET_STATS ts{};
  hr = DCompositionGetTargetStatistics(frameId, &tids[matched], &ts);
  if (FAILED(hr)) return false;

  // 部分 DWM 合成帧未在目标显示器触发实际 present，其 completedStats 为全零，需过滤。
  if (ts.completedStats.time == 0 || ts.completedStats.refreshCount == 0) return false;

  out.startTime = fs.startTime;
  out.completedTime = ts.completedStats.time;
  out.vblankDuration = ts.vblankDuration ? ts.vblankDuration : fs.framePeriod;
  out.refreshCount = ts.completedStats.refreshCount;
  return true;
}

uint64_t FrameGuard::ProbeInitialPeriodTicks(uint64_t nominalTicks) {
  uint64_t measuredTicks = 0;

  COMPOSITION_FRAME_ID latest = 0;
  if (SUCCEEDED(DCompositionGetFrameId(COMPOSITION_FRAME_ID_COMPLETED, &latest)) && latest > 0) {
    lastFrameId_ = latest;
    hasLastFrameId_ = true;

    // 从最新完成帧往前最多找 16 帧，取主屏最近一个有效帧作为晶振校准与游标起点。
    const uint64_t minId = (latest > 16) ? (latest - 15) : 1;
    for (uint64_t fid = latest; fid >= minId; --fid) {
      CompletedFrame frame{};
      if (ReadCompletedFrame(fid, frame)) {
        prevFrame_ = frame;
        hasPrevFrame_ = true;
        if (IsSanePeriodTicks(frame.vblankDuration, qpcFreq_)) {
          measuredTicks = frame.vblankDuration;
        }
        break;
      }
      if (fid == 1) break;
    }
  }

  // 晶振校准值与主屏标称周期相差在 ±2% 内时优先用实测值；
  // 否则用标称周期，避免启动瞬间正好处于 DRR 低频/VRR 时把低频误当基准。
  if (nominalTicks > 0 && measuredTicks > 0) {
    if (AbsDiff(measuredTicks, nominalTicks) * 50ULL <= nominalTicks) {
      return measuredTicks;
    }
    return nominalTicks;
  }
  if (nominalTicks > 0) return nominalTicks;
  if (measuredTicks > 0) return measuredTicks;

  // 极端兜底：60 Hz
  return qpcFreq_ / 60ULL;
}

bool FrameGuard::Initialize(uint64_t qpcFreq) {
  if (qpcFreq == 0) return false;
  qpcFreq_ = qpcFreq;

  paused_ = false;
  timingBadScore_ = 0;
  timingCleanStreak_ = 0;
  dropBadScore_ = 0;
  dropCleanStreak_ = 0;
  goodSamples_ = 0;
  goodRefreshes_ = 0;
  pausedAtTime_ = 0;
  lastResumedTime_ = 0;
  holdoffTicks_ = qpcFreq_ * kInitialHoldoffMs / 1000ULL;
  lastFrameId_ = 0;
  hasLastFrameId_ = false;
  prevFrame_ = CompletedFrame{};
  hasPrevFrame_ = false;
  commitHead_ = 0;
  commitCount_ = 0;
  lastMeasuredMs_ = 0.0;
  pauseCount_ = 0;

  const uint64_t nominalTicks = QueryPrimaryDisplayConfig(primaryTarget_);
  baselineTicks_ = ProbeInitialPeriodTicks(nominalTicks);

  // 单侧偏慢检测 + 按 dRef 摊薄端点抖动：
  //   - VRR/DRR 只会拉长帧周期，不会让显示器快于标称刷新率；短于 T_base 的单帧毛刺
  //     通常是 completedTime/refreshCount 相位错位或前一帧延迟后的补偿，不计入异常。
  //   - Active 态容忍偶发 1 帧抖动与小幅桌面波动：
  //       * vblank 轻微偏慢：+12%（至少 0.60ms），严重偏慢：+25%（至少 1.50ms）；
  //       * 周期均长容差：rate(+3%, >=0.15ms) + jitter(+10%, >=0.50ms)/dRef。
  //   - Paused 恢复态仍保持较严的跨帧均长门槛，避免在 VRR 视频播放中途误恢复。
  const uint64_t minVblankMild = std::max<uint64_t>(1, qpcFreq_ * 6ULL / 10000ULL);     // 0.60 ms
  const uint64_t minSevere = std::max<uint64_t>(1, qpcFreq_ * 15ULL / 10000ULL);        // 1.50 ms
  const uint64_t minActiveRate = std::max<uint64_t>(1, qpcFreq_ * 15ULL / 100000ULL);   // 0.15 ms
  const uint64_t minActiveJitter = std::max<uint64_t>(1, qpcFreq_ * 5ULL / 10000ULL);   // 0.50 ms
  const uint64_t minResumeSingle = std::max<uint64_t>(1, qpcFreq_ * 35ULL / 100000ULL); // 0.35 ms
  const uint64_t minResumeMulti = std::max<uint64_t>(1, qpcFreq_ * 8ULL / 100000ULL);   // 0.08 ms
  const uint64_t minResumeJitter = std::max<uint64_t>(1, qpcFreq_ * 16ULL / 100000ULL); // 0.16 ms

  tolVblankMildTicks_ = std::max(baselineTicks_ * 12ULL / 100ULL, minVblankMild);
  tolSevereTicks_ = std::max(baselineTicks_ * 25ULL / 100ULL, minSevere);
  tolActiveRateTicks_ = std::max(baselineTicks_ * 3ULL / 100ULL, minActiveRate);
  tolActiveJitterTicks_ = std::max(baselineTicks_ * 10ULL / 100ULL, minActiveJitter);
  tolResumeSingleTicks_ = std::max(baselineTicks_ * 6ULL / 100ULL, minResumeSingle);
  tolResumeMultiTicks_ = std::max(baselineTicks_ / 100ULL, minResumeMulti);
  tolResumeJitterTicks_ = std::max(baselineTicks_ * 2ULL / 100ULL, minResumeJitter);

  const double baseMs = baselineMs();
  const double baseHz = (baseMs > 0.0) ? (1000.0 / baseMs) : 0.0;
  const double nomHz =
      (nominalTicks > 0) ? (static_cast<double>(qpcFreq_) / static_cast<double>(nominalTicks))
                         : 0.0;
  DiagLog(L"[guard] baseline=%.3f ms (%.2f Hz, nominal=%.2f Hz, target=src%u/tgt%u, "
          L"tol=vblank%.2f/severe%.2f/rate%.2f+%.2f ms)",
          baseMs, baseHz, nomHz, primaryTarget_.vidPnSourceId, primaryTarget_.vidPnTargetId,
          TicksToMs(tolVblankMildTicks_, qpcFreq_), TicksToMs(tolSevereTicks_, qpcFreq_),
          TicksToMs(tolActiveRateTicks_, qpcFreq_), TicksToMs(tolActiveJitterTicks_, qpcFreq_));
  return true;
}

void FrameGuard::ResetSession() {
  hasLastFrameId_ = false;
  hasPrevFrame_ = false;
  commitHead_ = 0;
  commitCount_ = 0;
  timingBadScore_ = 0;
  timingCleanStreak_ = 0;
  dropBadScore_ = 0;
  dropCleanStreak_ = 0;
  goodSamples_ = 0;
  goodRefreshes_ = 0;
  pausedAtTime_ = 0;
  lastResumedTime_ = 0;
  holdoffTicks_ = qpcFreq_ * kInitialHoldoffMs / 1000ULL;
  if (paused_) {
    paused_ = false;
    DiagLog(L"[guard] session reset -> active");
  }
}

void FrameGuard::NotifyCommit(uint64_t commitQpc) {
  if (paused_ || commitQpc == 0) return;
  if (commitCount_ < kCommitRingCapacity) {
    commitRing_[(commitHead_ + commitCount_) % kCommitRingCapacity] = commitQpc;
    ++commitCount_;
  } else {
    commitRing_[commitHead_] = commitQpc;
    commitHead_ = (commitHead_ + 1) % kCommitRingCapacity;
  }
}

void FrameGuard::PurgeCommitsBefore(uint64_t cutoffQpc) {
  while (commitCount_ > 0 && commitRing_[commitHead_] <= cutoffQpc) {
    commitHead_ = (commitHead_ + 1) % kCommitRingCapacity;
    --commitCount_;
  }
}

bool FrameGuard::HasPendingCommitDropped(uint64_t prevStart, uint64_t currStart) {
  if (commitCount_ == 0 || currStart <= prevStart) return false;
  // prevStart + 0.25*T_base 之后的提交肯定来不及合入 prev 帧；如果最早那次提交比
  // currStart 还早超过 1.5*T_base，说明我们的提交排队等待超过一个完整周期才开始合成，
  // 属于真实的 DWM 掉帧，而非鼠标慢移或刚停住时的到期提交。
  const uint64_t afterPrev = prevStart + baselineTicks_ / 4ULL;
  const uint64_t maxWait = (baselineTicks_ * 3ULL) / 2ULL;
  for (uint32_t i = 0; i < commitCount_; ++i) {
    const uint64_t tc = commitRing_[(commitHead_ + i) % kCommitRingCapacity];
    if (tc <= afterPrev) continue;
    if (currStart > tc && (currStart - tc) > maxWait) return true;
    break;
  }
  return false;
}

void FrameGuard::EnterPaused(const wchar_t* reason, uint64_t eventTime, double measMs,
                             double vblankMs, uint32_t dRef) {
  const uint64_t initHoldoff = qpcFreq_ * kInitialHoldoffMs / 1000ULL;
  const uint64_t maxHoldoff = qpcFreq_ * kMaxHoldoffMs / 1000ULL;
  const uint64_t retripWindow = qpcFreq_ * kRapidRetripWindowMs / 1000ULL;
  if (lastResumedTime_ != 0 && eventTime >= lastResumedTime_ &&
      (eventTime - lastResumedTime_) <= retripWindow) {
    // 刚恢复不久又立刻撞上 VRR/掉帧：加倍冷却期，避免反复闪断。
    holdoffTicks_ = std::min(std::max(holdoffTicks_ * 2ULL, initHoldoff), maxHoldoff);
  } else {
    holdoffTicks_ = initHoldoff;
  }

  paused_ = true;
  pausedAtTime_ = eventTime;
  timingBadScore_ = 0;
  timingCleanStreak_ = 0;
  dropBadScore_ = 0;
  dropCleanStreak_ = 0;
  goodSamples_ = 0;
  goodRefreshes_ = 0;
  commitHead_ = 0;
  commitCount_ = 0;
  ++pauseCount_;
  DiagLog(L"[guard] paused (reason=%ls, meas=%.3f ms, vblank=%.3f ms, dRef=%u, base=%.3f ms, "
          L"holdoff=%.1fs)",
          reason, measMs, vblankMs, dRef, baselineMs(),
          TicksToMs(holdoffTicks_, qpcFreq_) / 1000.0);
}

void FrameGuard::EnterActive(uint64_t eventTime, double measMs, double vblankMs) {
  paused_ = false;
  lastResumedTime_ = eventTime;
  timingBadScore_ = 0;
  timingCleanStreak_ = 0;
  dropBadScore_ = 0;
  dropCleanStreak_ = 0;
  DiagLog(L"[guard] resumed (samples=%d, refreshes=%llu, meas=%.3f ms, vblank=%.3f ms, base=%.3f ms)",
          goodSamples_, static_cast<unsigned long long>(goodRefreshes_), measMs, vblankMs,
          baselineMs());
  goodSamples_ = 0;
  goodRefreshes_ = 0;
}

bool FrameGuard::OnFrameSample(const CompletedFrame& curr) {
  if (!hasPrevFrame_) {
    prevFrame_ = curr;
    hasPrevFrame_ = true;
    PurgeCommitsBefore(curr.startTime);
    return false;
  }

  // 忽略时间未前进或同一刷新号内的重复记录。
  const uint32_t dRef = curr.refreshCount - prevFrame_.refreshCount;
  if (curr.completedTime <= prevFrame_.completedTime || dRef == 0 || dRef >= 0x80000000U) {
    if (curr.completedTime > prevFrame_.completedTime) prevFrame_ = curr;
    PurgeCommitsBefore(curr.startTime);
    return false;
  }

  const uint64_t dt = curr.completedTime - prevFrame_.completedTime;
  const uint64_t measTicks = (dt + static_cast<uint64_t>(dRef) / 2ULL) / static_cast<uint64_t>(dRef);
  const uint64_t periodOver = (measTicks > baselineTicks_) ? (measTicks - baselineTicks_) : 0;
  const uint64_t periodUnder = (baselineTicks_ > measTicks) ? (baselineTicks_ - measTicks) : 0;
  const uint64_t vblankOver =
      (curr.vblankDuration > baselineTicks_) ? (curr.vblankDuration - baselineTicks_) : 0;
  const double measMs = TicksToMs(measTicks, qpcFreq_);
  const double vblankMs = TicksToMs(curr.vblankDuration, qpcFreq_);

  const bool commitDrop =
      (!paused_ && dRef >= 2 && HasPendingCommitDropped(prevFrame_.startTime, curr.startTime));

  prevFrame_ = curr;
  PurgeCommitsBefore(curr.startTime);

  // 物理上显示器无法在 VRR/掉帧时跑得远快于标称刷新率；此类超短样本（如 120Hz 下测出 4.3ms）
  // 是 completedTime 与 refreshCount 错位 1 帧导致的测量毛刺：既不判异常，也不计入正常恢复样本。
  const bool fastGlitch = (periodUnder > tolVblankMildTicks_) && (vblankOver <= tolVblankMildTicks_);
  if (fastGlitch) return false;

  lastMeasuredMs_ = measMs;

  if (!paused_) {
    const uint64_t activePeriodTol = tolActiveRateTicks_ + tolActiveJitterTicks_ / dRef;
    const bool vblankMild = (vblankOver > tolVblankMildTicks_);
    const bool periodMild = (periodOver > activePeriodTol);
    const bool timingSevere = (vblankOver > tolSevereTicks_) || (periodOver > tolSevereTicks_);
    const bool timingMild = vblankMild || periodMild;

    if (timingSevere || timingMild) {
      timingCleanStreak_ = 0;
      const int weight = timingSevere ? 2 : 1;
      timingBadScore_ = std::min(timingBadScore_ + weight, kTimingPauseThreshold);
      if (timingBadScore_ >= kTimingPauseThreshold) {
        const wchar_t* reason = vblankMild ? L"vblank" : L"period";
        EnterPaused(reason, curr.completedTime, measMs, vblankMs, dRef);
        return true;
      }
    } else if (timingBadScore_ > 0) {
      if (dRef >= kCleanIdleResetRefs) {
        timingBadScore_ = 0;
        timingCleanStreak_ = 0;
      } else {
        ++timingCleanStreak_;
        if (timingCleanStreak_ >= kTimingDecayStreak) {
          --timingBadScore_;
          timingCleanStreak_ = 0;
        }
      }
    }

    if (commitDrop) {
      dropCleanStreak_ = 0;
      const int weight = (dRef >= 3) ? 2 : 1;
      dropBadScore_ = std::min(dropBadScore_ + weight, kDropPauseThreshold);
      if (dropBadScore_ >= kDropPauseThreshold) {
        EnterPaused(L"drop", curr.completedTime, measMs, vblankMs, dRef);
        return true;
      }
    } else if (dropBadScore_ > 0) {
      if (dRef >= kCleanIdleResetRefs) {
        dropBadScore_ = 0;
        dropCleanStreak_ = 0;
      } else {
        ++dropCleanStreak_;
        if (dropCleanStreak_ >= kDropDecayStreak) {
          --dropBadScore_;
          dropCleanStreak_ = 0;
        }
      }
    }
  } else {
    const uint64_t resumePeriodTol =
        (dRef >= 2) ? (tolResumeMultiTicks_ + tolResumeJitterTicks_ / dRef)
                    : tolResumeSingleTicks_;
    const bool resumeBad =
        (vblankOver > tolResumeSingleTicks_) || (periodOver > resumePeriodTol);

    if (resumeBad) {
      // 暂停期间仍在持续偏慢/抖动：刷新冷却起点，防止中途误恢复。
      pausedAtTime_ = curr.completedTime;
      goodSamples_ = 0;
      goodRefreshes_ = 0;
    } else if (curr.completedTime >= pausedAtTime_ + holdoffTicks_) {
      ++goodSamples_;
      goodRefreshes_ += dRef;
      if (goodSamples_ >= kResumeGoodSamples ||
          (goodSamples_ >= kResumeMinSamplesForSpan && goodRefreshes_ >= kResumeGoodRefreshes)) {
        EnterActive(curr.completedTime, measMs, vblankMs);
        return true;
      }
    }
  }
  return false;
}

bool FrameGuard::Poll() {
  if (qpcFreq_ == 0 || baselineTicks_ == 0) return false;

  COMPOSITION_FRAME_ID latest = 0;
  if (FAILED(DCompositionGetFrameId(COMPOSITION_FRAME_ID_COMPLETED, &latest)) || latest == 0) {
    return false;
  }

  if (!hasLastFrameId_) {
    lastFrameId_ = latest;
    hasLastFrameId_ = true;
    const uint64_t minId = (latest > 16) ? (latest - 15) : 1;
    for (uint64_t fid = latest; fid >= minId; --fid) {
      CompletedFrame frame{};
      if (ReadCompletedFrame(fid, frame)) {
        prevFrame_ = frame;
        hasPrevFrame_ = true;
        break;
      }
      if (fid == 1) break;
    }
    return false;
  }

  // 无新完成帧：Paused 状态下保持静默不恢复，Active 状态下零开销返回。
  if (latest <= lastFrameId_) return false;

  const bool wasPaused = paused_;
  uint64_t startId = lastFrameId_ + 1;
  if (latest - lastFrameId_ > kMaxFramesPerPoll) {
    startId = latest - kMaxFramesPerPoll + 1;
    hasPrevFrame_ = false;
    commitHead_ = 0;
    commitCount_ = 0;
  }

  for (uint64_t fid = startId; fid <= latest; ++fid) {
    CompletedFrame frame{};
    if (!ReadCompletedFrame(fid, frame)) continue;
    OnFrameSample(frame);
  }
  lastFrameId_ = latest;
  return paused_ != wasPaused;
}
