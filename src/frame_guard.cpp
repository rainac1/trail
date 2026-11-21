#include "frame_guard.h"

#include <dcomp.h>
#include <dwmapi.h>

#include <algorithm>
#include <vector>

#include "diag.h"

namespace {

// 兼容较旧的 Windows 10 SDK（未定义 Win11 Compositor Statistics 结构体时提供同 ABI 定义）。
#ifndef COMPOSITION_STATS_MAX_TARGETS
enum COMPOSITION_FRAME_ID_TYPE {
  COMPOSITION_FRAME_ID_CREATED = 0,
  COMPOSITION_FRAME_ID_CONFIRMED = 1,
  COMPOSITION_FRAME_ID_COMPLETED = 2,
};

using COMPOSITION_FRAME_ID = ULONG64;

struct COMPOSITION_FRAME_STATS {
  UINT64 startTime;
  UINT64 targetTime;
  UINT64 framePeriod;
};

struct COMPOSITION_TARGET_ID {
  LUID displayAdapterLuid;
  LUID renderAdapterLuid;
  UINT vidPnSourceId;
  UINT vidPnTargetId;
  UINT uniqueId;
};

struct COMPOSITION_STATS {
  UINT presentCount;
  UINT refreshCount;
  UINT virtualRefreshCount;
  UINT64 time;
};

struct COMPOSITION_TARGET_STATS {
  UINT outstandingPresents;
  UINT64 presentTime;
  UINT64 vblankDuration;
  COMPOSITION_STATS presentedStats;
  COMPOSITION_STATS completedStats;
};
#endif

using GetFrameIdFn = HRESULT(STDAPICALLTYPE*)(COMPOSITION_FRAME_ID_TYPE, COMPOSITION_FRAME_ID*);
using GetStatisticsFn = HRESULT(STDAPICALLTYPE*)(COMPOSITION_FRAME_ID, COMPOSITION_FRAME_STATS*,
                                                 UINT, COMPOSITION_TARGET_ID*, UINT*);
using GetTargetStatisticsFn = HRESULT(STDAPICALLTYPE*)(COMPOSITION_FRAME_ID,
                                                       const COMPOSITION_TARGET_ID*,
                                                       COMPOSITION_TARGET_STATS*);

constexpr uint64_t kMaxFramesPerPoll = 64;
constexpr int kPauseMildThreshold = 2;      // 轻微偏离：2 帧即停（且需连续 4 帧正常才衰减 1）
constexpr int kBadDecayStreak = 4;          // 防止 50% 交替掉帧把 badScore 来回清零
constexpr int kResumeGoodSamples = 6;       // 恢复需连续 6 个正常样本（且过完冷却期）
constexpr uint64_t kInitialHoldoffMs = 500;
constexpr uint64_t kMaxHoldoffMs = 8000;
constexpr uint64_t kRapidRetripWindowMs = 3000;
constexpr UINT kStackTargetLimit = 16;

uint64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

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

void FrameGuard::LoadDcompStatsApi() {
  fnGetFrameId_ = nullptr;
  fnGetStatistics_ = nullptr;
  fnGetTargetStatistics_ = nullptr;
  hasDcompStats_ = false;

  // dcomp.lib 已静态链接，进程启动后 dcomp.dll 就在地址空间内；用 GetModuleHandleW 优先取句柄。
  HMODULE dcomp = GetModuleHandleW(L"dcomp.dll");
  if (!dcomp) dcomp = LoadLibraryW(L"dcomp.dll");
  if (!dcomp) return;

  fnGetFrameId_ = reinterpret_cast<void*>(GetProcAddress(dcomp, "DCompositionGetFrameId"));
  fnGetStatistics_ = reinterpret_cast<void*>(GetProcAddress(dcomp, "DCompositionGetStatistics"));
  fnGetTargetStatistics_ =
      reinterpret_cast<void*>(GetProcAddress(dcomp, "DCompositionGetTargetStatistics"));
  hasDcompStats_ = (fnGetFrameId_ && fnGetStatistics_ && fnGetTargetStatistics_);
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
  if (!hasDcompStats_) return false;
  const auto fnGetStatistics = reinterpret_cast<GetStatisticsFn>(fnGetStatistics_);
  const auto fnGetTargetStatistics =
      reinterpret_cast<GetTargetStatisticsFn>(fnGetTargetStatistics_);

  COMPOSITION_FRAME_STATS fs{};
  COMPOSITION_TARGET_ID tids[kStackTargetLimit]{};
  UINT actualTargets = 0;
  HRESULT hr = fnGetStatistics(frameId, &fs, kStackTargetLimit, tids, &actualTargets);
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
  hr = fnGetTargetStatistics(frameId, &tids[matched], &ts);
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

  if (hasDcompStats_) {
    const auto fnGetFrameId = reinterpret_cast<GetFrameIdFn>(fnGetFrameId_);
    COMPOSITION_FRAME_ID latest = 0;
    if (SUCCEEDED(fnGetFrameId(COMPOSITION_FRAME_ID_COMPLETED, &latest)) && latest > 0) {
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
  }

  // Win10 或启动时 DComp 尚无主屏历史帧：退回 DwmGetCompositionTimingInfo::qpcRefreshPeriod。
  if (measuredTicks == 0) {
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof(ti);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) &&
        IsSanePeriodTicks(ti.qpcRefreshPeriod, qpcFreq_)) {
      measuredTicks = ti.qpcRefreshPeriod;
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
  badScore_ = 0;
  activeCleanStreak_ = 0;
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

  LoadDcompStatsApi();
  const uint64_t nominalTicks = QueryPrimaryDisplayConfig(primaryTarget_);
  baselineTicks_ = ProbeInitialPeriodTicks(nominalTicks);

  // 非对称灵敏度：
  //   - 单帧轻微偏离：±8%（至少 0.50ms），2 帧触发；
  //   - 单帧严重偏离：±20%（至少 1.50ms），1 帧立即停（抓住 12~19ms 的 VRR 跳变）；
  //   - 跨帧均长（dRef >= 2）：±1%（至少 0.08ms），健康桌面误差通常 <0.02ms，
  //     VRR 视频下常见 0.07~0.17ms 抖动会被挡住，防止暂停期间误恢复。
  const uint64_t minSingle = std::max<uint64_t>(1, qpcFreq_ / 2000ULL);   // 0.50 ms
  const uint64_t minSevere = std::max<uint64_t>(1, qpcFreq_ * 3ULL / 2000ULL);  // 1.50 ms
  const uint64_t minMulti = std::max<uint64_t>(1, qpcFreq_ / 12500ULL);   // 0.08 ms
  tolSingleTicks_ = std::max(baselineTicks_ * 8ULL / 100ULL, minSingle);
  tolSevereTicks_ = std::max(baselineTicks_ * 20ULL / 100ULL, minSevere);
  tolMultiTicks_ = std::max(baselineTicks_ / 100ULL, minMulti);

  const double baseMs = baselineMs();
  const double baseHz = (baseMs > 0.0) ? (1000.0 / baseMs) : 0.0;
  const double nomHz =
      (nominalTicks > 0) ? (static_cast<double>(qpcFreq_) / static_cast<double>(nominalTicks))
                         : 0.0;
  DiagLog(L"[guard] baseline=%.3f ms (%.2f Hz, nominal=%.2f Hz, target=src%u/tgt%u, "
          L"tol=%.2f/%.2f/%.2f ms, dcompStats=%ls)",
          baseMs, baseHz, nomHz, primaryTarget_.vidPnSourceId, primaryTarget_.vidPnTargetId,
          TicksToMs(tolSingleTicks_, qpcFreq_), TicksToMs(tolSevereTicks_, qpcFreq_),
          TicksToMs(tolMultiTicks_, qpcFreq_),
          hasDcompStats_ ? L"on" : L"off(win10-fallback)");
  return true;
}

void FrameGuard::ResetSession() {
  hasLastFrameId_ = false;
  hasPrevFrame_ = false;
  commitHead_ = 0;
  commitCount_ = 0;
  badScore_ = 0;
  activeCleanStreak_ = 0;
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
  badScore_ = 0;
  activeCleanStreak_ = 0;
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
  badScore_ = 0;
  activeCleanStreak_ = 0;
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
  const uint64_t periodErr = AbsDiff(measTicks, baselineTicks_);
  const double measMs = TicksToMs(measTicks, qpcFreq_);
  const double vblankMs = TicksToMs(curr.vblankDuration, qpcFreq_);
  lastMeasuredMs_ = measMs;

  const uint64_t periodTol = (dRef >= 2) ? tolMultiTicks_ : tolSingleTicks_;
  const bool vblankBad =
      (curr.vblankDuration > 0 && AbsDiff(curr.vblankDuration, baselineTicks_) > tolSingleTicks_);
  const bool periodBad = (periodErr > periodTol);
  const bool severeBad = vblankBad || (periodErr > tolSevereTicks_);
  const bool commitDrop =
      (!paused_ && dRef >= 2 && HasPendingCommitDropped(prevFrame_.startTime, curr.startTime));

  prevFrame_ = curr;
  PurgeCommitsBefore(curr.startTime);

  const bool isBad = vblankBad || periodBad || commitDrop;

  if (!paused_) {
    if (severeBad) {
      const wchar_t* reason = vblankBad ? L"vblank" : L"period-severe";
      EnterPaused(reason, curr.completedTime, measMs, vblankMs, dRef);
      return true;
    }
    if (isBad) {
      activeCleanStreak_ = 0;
      if (badScore_ < kPauseMildThreshold) ++badScore_;
      if (badScore_ >= kPauseMildThreshold) {
        const wchar_t* reason = periodBad ? L"period" : L"drop";
        EnterPaused(reason, curr.completedTime, measMs, vblankMs, dRef);
        return true;
      }
    } else if (badScore_ > 0) {
      ++activeCleanStreak_;
      if (activeCleanStreak_ >= kBadDecayStreak) {
        --badScore_;
        activeCleanStreak_ = 0;
      }
    }
  } else {
    if (isBad) {
      // 暂停期间仍在持续抖动：刷新冷却起点，防止中途误恢复。
      pausedAtTime_ = curr.completedTime;
      goodSamples_ = 0;
      goodRefreshes_ = 0;
    } else if (curr.completedTime >= pausedAtTime_ + holdoffTicks_) {
      ++goodSamples_;
      goodRefreshes_ += dRef;
      if (goodSamples_ >= kResumeGoodSamples) {
        EnterActive(curr.completedTime, measMs, vblankMs);
        return true;
      }
    }
  }
  return false;
}

bool FrameGuard::PollDcompStats() {
  const auto fnGetFrameId = reinterpret_cast<GetFrameIdFn>(fnGetFrameId_);
  COMPOSITION_FRAME_ID latest = 0;
  if (FAILED(fnGetFrameId(COMPOSITION_FRAME_ID_COMPLETED, &latest)) || latest == 0) {
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

bool FrameGuard::PollDwmFallback() {
  // Windows 10 降级路径：没有 Win11 历史帧统计，仅监测 DWM 上报的刷新周期是否偏离基准。
  DWM_TIMING_INFO ti{};
  ti.cbSize = sizeof(ti);
  if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) || ti.qpcRefreshPeriod == 0) {
    return false;
  }
  const double vblankMs = TicksToMs(ti.qpcRefreshPeriod, qpcFreq_);
  lastMeasuredMs_ = vblankMs;
  const bool isBad = (AbsDiff(ti.qpcRefreshPeriod, baselineTicks_) > tolSingleTicks_);

  const uint64_t now = QpcNow();
  const bool wasPaused = paused_;
  if (!paused_) {
    if (isBad) {
      activeCleanStreak_ = 0;
      if (badScore_ < kPauseMildThreshold) ++badScore_;
      if (badScore_ >= kPauseMildThreshold) {
        EnterPaused(L"dwm-period", now, vblankMs, vblankMs, 1);
      }
    } else if (badScore_ > 0) {
      ++activeCleanStreak_;
      if (activeCleanStreak_ >= kBadDecayStreak) {
        --badScore_;
        activeCleanStreak_ = 0;
      }
    }
  } else {
    if (isBad) {
      pausedAtTime_ = now;
      goodSamples_ = 0;
      goodRefreshes_ = 0;
    } else if (now >= pausedAtTime_ + holdoffTicks_) {
      ++goodSamples_;
      goodRefreshes_ += 1;
      if (goodSamples_ >= kResumeGoodSamples) {
        EnterActive(now, vblankMs, vblankMs);
      }
    }
  }
  return paused_ != wasPaused;
}

bool FrameGuard::Poll() {
  if (qpcFreq_ == 0 || baselineTicks_ == 0) return false;
  if (hasDcompStats_) return PollDcompStats();
  return PollDwmFallback();
}
