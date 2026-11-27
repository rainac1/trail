#include "frame_guard.h"

#include <dcomp.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cwchar>
#include <vector>

#include "diag.h"

namespace {

constexpr uint64_t kMaxFramesPerPoll = 64;
constexpr int kTimingPauseThreshold = 4;      // VRR/帧周期异常：严重偏离(+2)需 2 帧，轻微(+1)需 4 帧，拒绝单帧毛刺
constexpr int kTimingDecayStreak = 2;         // 连续 2 个正常帧衰减 1 分（仍能抓住 50% 交替抖动）
constexpr int kDropPauseThreshold = 6;        // 持续 DWM 掉帧：连续 6 帧 dRef=2（~100ms）或 3 帧 dRef>=3 才暂停
constexpr int kDropDecayStreak = 2;           // 连续 2 个无掉帧样本衰减 1 分，避免偶发单帧跳过触发暂停
constexpr uint32_t kCleanIdleResetRefs = 12;  // 跨越 >=12 次正常刷新后清零陈旧的历史计分
constexpr int kResumeGoodSamples = 6;         // 活跃桌面恢复需连续 6 个正常样本（且过完冷却期）
constexpr int kResumeMinSamplesForSpan = 2;   // 静止桌面帧稀疏时：至少 2 个正常样本且覆盖 >=24 次刷新即可恢复
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

bool SameRect(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool IsSanePeriodTicks(uint64_t ticks, uint64_t qpcFreq) {
  // 合理刷新率范围：10 Hz (100ms) ~ 1000 Hz (1ms)。
  const uint64_t minTicks = qpcFreq / 1000ULL;
  const uint64_t maxTicks = qpcFreq / 10ULL;
  return ticks >= minTicks && ticks <= maxTicks;
}

UINT QueryMonitorDpi(HMONITOR hmon) {
  UINT dpiX = 96;
  UINT dpiY = 96;
  if (SUCCEEDED(GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
    return dpiX;
  }
  return 96;
}

// 轻量快照：仅枚举所有活动显示器的句柄、物理矩形、DPI 与主屏标记。
struct MonitorSnapshot {
  HMONITOR hmon = nullptr;
  RECT rect{};
  UINT dpi = 96;
  bool primary = false;
  wchar_t deviceName[CCHDEVICENAME]{};
};

BOOL CALLBACK EnumMonitorsProc(HMONITOR hmon, HDC /*hdc*/, LPRECT /*clip*/, LPARAM lParam) {
  auto* list = reinterpret_cast<std::vector<MonitorSnapshot>*>(lParam);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(hmon, &mi)) return TRUE;

  MonitorSnapshot snap{};
  snap.hmon = hmon;
  snap.rect = mi.rcMonitor;
  snap.dpi = QueryMonitorDpi(hmon);
  snap.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  wcsncpy_s(snap.deviceName, mi.szDevice, _TRUNCATE);
  list->push_back(snap);
  return TRUE;
}

std::vector<MonitorSnapshot> EnumerateMonitorSnapshots() {
  std::vector<MonitorSnapshot> list;
  EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, reinterpret_cast<LPARAM>(&list));
  // 稳定排序：主屏在前，其余按物理位置 (left, top) 排序，便于日志对照与 TopologyChanged 比较。
  std::sort(list.begin(), list.end(), [](const MonitorSnapshot& a, const MonitorSnapshot& b) {
    if (a.primary != b.primary) return a.primary;
    if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
    if (a.rect.top != b.rect.top) return a.rect.top < b.rect.top;
    return wcscmp(a.deviceName, b.deviceName) < 0;
  });
  return list;
}

struct DisplayPathEntry {
  wchar_t gdiDeviceName[CCHDEVICENAME]{};
  FrameGuard::TargetId target{};
  uint64_t nominalPeriodTicks = 0;
  bool advancedColor = false;
};

// 查询显示目标是否处于高级色彩模式（HDR 或 Windows 11 自动色彩管理 ACM）。
// 开启时 DWM 内部使用 FP16 线性 scRGB 合成，混合天然处于线性光空间。
bool QueryTargetAdvancedColor(LUID adapterId, UINT32 targetId) {
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
  info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  info.header.size = sizeof(info);
  info.header.adapterId = adapterId;
  info.header.id = targetId;
  if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS) return false;
  return (info.advancedColorEnabled != 0) || (info.wideColorEnforced != 0);
}

std::vector<DisplayPathEntry> QueryActiveDisplayPaths(uint64_t qpcFreq) {
  std::vector<DisplayPathEntry> result;

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
      DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
      srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      srcName.header.size = sizeof(srcName);
      srcName.header.adapterId = p.sourceInfo.adapterId;
      srcName.header.id = p.sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS) continue;

      DisplayPathEntry entry{};
      wcsncpy_s(entry.gdiDeviceName, srcName.viewGdiDeviceName, _TRUNCATE);
      entry.target.adapterLuid = p.targetInfo.adapterId;
      entry.target.vidPnSourceId = p.sourceInfo.id;
      entry.target.vidPnTargetId = p.targetInfo.id;
      entry.target.valid = true;
      entry.advancedColor = QueryTargetAdvancedColor(p.targetInfo.adapterId, p.targetInfo.id);

      const DISPLAYCONFIG_RATIONAL& r = p.targetInfo.refreshRate;
      if (r.Numerator > 0 && r.Denominator > 0) {
        const double ticks = static_cast<double>(qpcFreq) * static_cast<double>(r.Denominator) /
                             static_cast<double>(r.Numerator);
        const uint64_t period = static_cast<uint64_t>(ticks + 0.5);
        if (IsSanePeriodTicks(period, qpcFreq)) entry.nominalPeriodTicks = period;
      }
      result.push_back(entry);
    }
    break;
  }
  return result;
}

uint64_t QueryFallbackNominalTicks(const wchar_t* deviceName, uint64_t qpcFreq) {
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &dm) &&
      dm.dmDisplayFrequency >= 10 && dm.dmDisplayFrequency <= 1000) {
    return qpcFreq / static_cast<uint64_t>(dm.dmDisplayFrequency);
  }
  return 0;
}

// 简化显示设备名用于日志标签（如 "\\.\DISPLAY1" -> "DISPLAY1"）。
const wchar_t* ShortDeviceName(const wchar_t* name) {
  if (!name) return L"?";
  if (wcsncmp(name, L"\\\\.\\", 4) == 0) return name + 4;
  return name;
}

}  // namespace

// ============================================================================
// FrameGuard（单屏独立状态机）
// ============================================================================

double FrameGuard::baselineMs() const {
  return TicksToMs(baselineTicks_, qpcFreq_);
}

bool FrameGuard::Initialize(uint64_t qpcFreq, const wchar_t* tag, uint64_t nominalTicks,
                            uint64_t measuredTicks) {
  if (qpcFreq == 0) return false;
  qpcFreq_ = qpcFreq;
  wcsncpy_s(tag_, tag ? tag : L"?", _TRUNCATE);

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
  prevFrame_ = CompletedFrame{};
  hasPrevFrame_ = false;
  commitHead_ = 0;
  commitCount_ = 0;
  ResetActiveWindow();
  lastMeasuredMs_ = 0.0;
  pauseCount_ = 0;

  // 晶振校准值与该屏标称周期相差在 ±2% 内时优先用实测值；
  // 否则用标称周期，避免启动瞬间正好处于 DRR 低频/VRR 时把低频误当基准。
  if (nominalTicks > 0 && measuredTicks > 0 && IsSanePeriodTicks(measuredTicks, qpcFreq_)) {
    if (AbsDiff(measuredTicks, nominalTicks) * 50ULL <= nominalTicks) {
      baselineTicks_ = measuredTicks;
    } else {
      baselineTicks_ = nominalTicks;
    }
  } else if (nominalTicks > 0) {
    baselineTicks_ = nominalTicks;
  } else if (measuredTicks > 0 && IsSanePeriodTicks(measuredTicks, qpcFreq_)) {
    baselineTicks_ = measuredTicks;
  } else {
    baselineTicks_ = qpcFreq_ / 60ULL;
  }

  // 单侧偏慢检测 + 10% 帧率偏离容忍底线 + 按 dRef 摊薄端点抖动：
  //   - 帧率未低于基准的 90%（即 F >= 0.90 * F_base <=> T <= 10/9 * T_base <=> 周期增量 <= T_base / 9
  //     ≈ +11.11%）均视为正常波动，不判为 VRR 降频或卡顿。
  //   - Active 态：
  //       * vblank 轻微偏慢：max(T_base / 9, 0.60ms)，严重偏慢：max(25% * T_base, 1.50ms)；
  //       * 周期均长容差：rate(max(T_base / 9, 0.15ms)) + jitter(max(10% * T_base, 0.50ms)) / dRef。
  //   - Paused 恢复态相对 10% 留轻微回滞（跨帧均长 +9% ≈ 帧率恢复到基准 91.7% 以上），避免在临界点抖动。
  const uint64_t minVblankMild = std::max<uint64_t>(1, qpcFreq_ * 6ULL / 10000ULL);      // 0.60 ms
  const uint64_t minSevere = std::max<uint64_t>(1, qpcFreq_ * 15ULL / 10000ULL);         // 1.50 ms
  const uint64_t minActiveRate = std::max<uint64_t>(1, qpcFreq_ * 15ULL / 100000ULL);    // 0.15 ms
  const uint64_t minActiveJitter = std::max<uint64_t>(1, qpcFreq_ * 5ULL / 10000ULL);    // 0.50 ms
  const uint64_t minResumeSingle = std::max<uint64_t>(1, qpcFreq_ * 35ULL / 100000ULL);  // 0.35 ms
  const uint64_t minResumeMulti = std::max<uint64_t>(1, qpcFreq_ * 8ULL / 100000ULL);    // 0.08 ms
  const uint64_t minResumeJitter = std::max<uint64_t>(1, qpcFreq_ * 16ULL / 100000ULL);  // 0.16 ms

  tolFps10Ticks_ = baselineTicks_ / 9ULL;
  tolVblankMildTicks_ = std::max(tolFps10Ticks_, minVblankMild);
  tolSevereTicks_ = std::max(baselineTicks_ * 25ULL / 100ULL, minSevere);
  tolActiveRateTicks_ = std::max(tolFps10Ticks_, minActiveRate);
  tolActiveJitterTicks_ = std::max(baselineTicks_ * 10ULL / 100ULL, minActiveJitter);
  tolResumeSingleTicks_ = std::max(tolFps10Ticks_, minResumeSingle);
  tolResumeMultiTicks_ = std::max(baselineTicks_ * 9ULL / 100ULL, minResumeMulti);
  tolResumeJitterTicks_ = std::max(baselineTicks_ * 2ULL / 100ULL, minResumeJitter);
  return true;
}

void FrameGuard::ResetSession() {
  hasPrevFrame_ = false;
  commitHead_ = 0;
  commitCount_ = 0;
  ResetActiveWindow();
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
    DiagLog(L"[guard][%ls] session reset -> active", tag_);
  }
}

void FrameGuard::ResetFrameAnchor() {
  hasPrevFrame_ = false;
  commitHead_ = 0;
  commitCount_ = 0;
  ResetActiveWindow();
}

void FrameGuard::SeedPrevFrame(const CompletedFrame& frame) {
  prevFrame_ = frame;
  hasPrevFrame_ = true;
  PurgeCommitsBefore(frame.startTime);
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

void FrameGuard::ResetActiveWindow() {
  activeDtHead_ = 0;
  activeDtCount_ = 0;
  activeDtSum_ = 0;
}

void FrameGuard::RecordActiveInterval(uint64_t dtTicks) {
  if (activeDtCount_ < kActiveWindowCapacity) {
    activeDtRing_[(activeDtHead_ + activeDtCount_) % kActiveWindowCapacity] = dtTicks;
    ++activeDtCount_;
    activeDtSum_ += dtTicks;
  } else {
    activeDtSum_ -= activeDtRing_[activeDtHead_];
    activeDtRing_[activeDtHead_] = dtTicks;
    activeDtSum_ += dtTicks;
    activeDtHead_ = (activeDtHead_ + 1) % kActiveWindowCapacity;
  }
}

bool FrameGuard::ActiveFpsDeviatedOver10Pct() const {
  if (activeDtCount_ == 0 || baselineTicks_ == 0) return false;
  // 有效帧率 F_eff = count / sum(dt) < 0.90 * F_base  <=>  9 * sum(dt) > 10 * count * T_base
  return activeDtSum_ * 9ULL > static_cast<uint64_t>(activeDtCount_) * baselineTicks_ * 10ULL;
}

bool FrameGuard::HasPendingCommitDropped(uint64_t prevStart, uint64_t currStart) {
  if (commitCount_ == 0 || currStart <= prevStart) return false;
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
  ResetActiveWindow();
  ++pauseCount_;
  DiagLog(L"[guard][%ls] paused (reason=%ls, meas=%.3f ms, vblank=%.3f ms, dRef=%u, base=%.3f ms, "
          L"holdoff=%.1fs)",
          tag_, reason, measMs, vblankMs, dRef, baselineMs(),
          TicksToMs(holdoffTicks_, qpcFreq_) / 1000.0);
}

void FrameGuard::EnterActive(uint64_t eventTime, double measMs, double vblankMs) {
  paused_ = false;
  lastResumedTime_ = eventTime;
  timingBadScore_ = 0;
  timingCleanStreak_ = 0;
  dropBadScore_ = 0;
  dropCleanStreak_ = 0;
  DiagLog(L"[guard][%ls] resumed (samples=%d, refreshes=%llu, meas=%.3f ms, vblank=%.3f ms, "
          L"base=%.3f ms)",
          tag_, goodSamples_, static_cast<unsigned long long>(goodRefreshes_), measMs, vblankMs,
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
    if (dRef == 1 || commitDrop) {
      RecordActiveInterval(dt);
    } else if (dRef >= kCleanIdleResetRefs) {
      ResetActiveWindow();
    }

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
      // 仅当活跃窗口内有效帧率偏离基准超过 10% 时才因掉帧暂停，零星偶发跳帧不算卡顿。
      if (dropBadScore_ >= kDropPauseThreshold && ActiveFpsDeviatedOver10Pct()) {
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

// ============================================================================
// DisplayGuardManager（多屏拓扑与统一 DComp 统计分发）
// ============================================================================

bool DisplayGuardManager::Initialize(uint64_t qpcFreq) {
  if (qpcFreq == 0) return false;
  qpcFreq_ = qpcFreq;
  return RefreshTopology();
}

bool DisplayGuardManager::RefreshTopology() {
  if (qpcFreq_ == 0) return false;

  const std::vector<MonitorSnapshot> snaps = EnumerateMonitorSnapshots();
  const std::vector<DisplayPathEntry> paths = QueryActiveDisplayPaths(qpcFreq_);

  monitors_.clear();
  monitors_.reserve(snaps.size());
  lastFrameId_ = 0;
  hasLastFrameId_ = false;

  for (const MonitorSnapshot& snap : snaps) {
    MonitorSlot slot{};
    slot.desc.hmon = snap.hmon;
    slot.desc.rect = snap.rect;
    slot.desc.dpi = snap.dpi;
    slot.desc.primary = snap.primary;
    wcsncpy_s(slot.desc.deviceName, snap.deviceName, _TRUNCATE);

    for (const DisplayPathEntry& entry : paths) {
      if (_wcsicmp(entry.gdiDeviceName, snap.deviceName) == 0) {
        slot.desc.target = entry.target;
        slot.desc.nominalPeriodTicks = entry.nominalPeriodTicks;
        slot.desc.advancedColor = entry.advancedColor;
        break;
      }
    }
    if (slot.desc.nominalPeriodTicks == 0) {
      slot.desc.nominalPeriodTicks = QueryFallbackNominalTicks(snap.deviceName, qpcFreq_);
    }
    monitors_.push_back(std::move(slot));
  }

  // 从最近 16 个 DWM 已完成帧中为每个显示器探测实测 vblankDuration（晶振校准）与初始帧游标。
  std::vector<uint64_t> measured(monitors_.size(), 0);
  std::vector<FrameGuard::CompletedFrame> initialFrames(monitors_.size());
  std::vector<bool> haveInitial(monitors_.size(), false);

  COMPOSITION_FRAME_ID latest = 0;
  if (!monitors_.empty() &&
      SUCCEEDED(DCompositionGetFrameId(COMPOSITION_FRAME_ID_COMPLETED, &latest)) && latest > 0) {
    lastFrameId_ = latest;
    hasLastFrameId_ = true;

    const uint64_t minId = (latest > 16) ? (latest - 15) : 1;
    size_t remaining = monitors_.size();
    for (uint64_t fid = latest; fid >= minId && remaining > 0; --fid) {
      COMPOSITION_FRAME_STATS fs{};
      COMPOSITION_TARGET_ID tids[kStackTargetLimit]{};
      UINT actualTargets = 0;
      if (FAILED(DCompositionGetStatistics(fid, &fs, kStackTargetLimit, tids, &actualTargets)) ||
          actualTargets == 0) {
        if (fid == 1) break;
        continue;
      }
      const UINT count = std::min(actualTargets, kStackTargetLimit);
      for (UINT i = 0; i < count; ++i) {
        const int idx =
            FindMonitorByTarget(tids[i].displayAdapterLuid, tids[i].vidPnSourceId, tids[i].vidPnTargetId);
        if (idx < 0 || haveInitial[static_cast<size_t>(idx)]) continue;

        COMPOSITION_TARGET_STATS ts{};
        if (FAILED(DCompositionGetTargetStatistics(fid, &tids[i], &ts))) continue;
        if (ts.completedStats.time == 0 || ts.completedStats.refreshCount == 0) continue;

        const uint64_t vblank = ts.vblankDuration ? ts.vblankDuration : fs.framePeriod;
        initialFrames[static_cast<size_t>(idx)] =
            FrameGuard::CompletedFrame{fs.startTime, ts.completedStats.time, vblank,
                                       ts.completedStats.refreshCount};
        measured[static_cast<size_t>(idx)] = vblank;
        haveInitial[static_cast<size_t>(idx)] = true;
        --remaining;
      }
      if (fid == 1) break;
    }
  }

  for (size_t i = 0; i < monitors_.size(); ++i) {
    MonitorSlot& slot = monitors_[i];
    const wchar_t* tag = ShortDeviceName(slot.desc.deviceName);
    slot.guard.Initialize(qpcFreq_, tag, slot.desc.nominalPeriodTicks, measured[i]);
    if (haveInitial[i]) slot.guard.SeedPrevFrame(initialFrames[i]);

    const double baseMs = slot.guard.baselineMs();
    const double baseHz = (baseMs > 0.0) ? (1000.0 / baseMs) : 0.0;
    const double nomHz =
        (slot.desc.nominalPeriodTicks > 0)
            ? (static_cast<double>(qpcFreq_) / static_cast<double>(slot.desc.nominalPeriodTicks))
            : 0.0;
    DiagLog(L"[guard][%ls] #%zu %ls %d,%d %dx%d dpi=%u color=%ls | baseline=%.3f ms (%.2f Hz, "
            L"nominal=%.2f Hz, target=src%u/tgt%u%ls)",
            tag, i, slot.desc.primary ? L"primary" : L"secondary",
            static_cast<int>(slot.desc.rect.left), static_cast<int>(slot.desc.rect.top),
            static_cast<int>(slot.desc.rect.right - slot.desc.rect.left),
            static_cast<int>(slot.desc.rect.bottom - slot.desc.rect.top), slot.desc.dpi,
            slot.desc.advancedColor ? L"linear(HDR/ACM)" : L"sRGB(SDR)", baseMs, baseHz, nomHz,
            slot.desc.target.vidPnSourceId, slot.desc.target.vidPnTargetId,
            slot.desc.target.valid ? L"" : L"(unmatched)");
  }
  return !monitors_.empty();
}

bool DisplayGuardManager::TopologyChanged() const {
  const std::vector<MonitorSnapshot> snaps = EnumerateMonitorSnapshots();
  if (snaps.size() != monitors_.size()) return true;
  for (size_t i = 0; i < snaps.size(); ++i) {
    const MonitorDesc& cur = monitors_[i].desc;
    const MonitorSnapshot& s = snaps[i];
    if (cur.hmon != s.hmon || cur.dpi != s.dpi || cur.primary != s.primary ||
        !SameRect(cur.rect, s.rect) || _wcsicmp(cur.deviceName, s.deviceName) != 0) {
      return true;
    }
    if (cur.target.valid &&
        QueryTargetAdvancedColor(cur.target.adapterLuid, cur.target.vidPnTargetId) !=
            cur.advancedColor) {
      return true;
    }
  }
  return false;
}

void DisplayGuardManager::ResetSession() {
  hasLastFrameId_ = false;
  for (MonitorSlot& slot : monitors_) {
    slot.guard.ResetSession();
  }
}

void DisplayGuardManager::NotifyCommit(int monitorIndex, uint64_t commitQpc) {
  if (monitorIndex < 0 || monitorIndex >= monitorCount()) return;
  monitors_[static_cast<size_t>(monitorIndex)].guard.NotifyCommit(commitQpc);
}

int DisplayGuardManager::primaryMonitorIndex() const {
  for (size_t i = 0; i < monitors_.size(); ++i) {
    if (monitors_[i].desc.primary) return static_cast<int>(i);
  }
  return monitors_.empty() ? -1 : 0;
}

int DisplayGuardManager::FindMonitorAt(int x, int y) const {
  for (size_t i = 0; i < monitors_.size(); ++i) {
    const RECT& r = monitors_[i].desc.rect;
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int DisplayGuardManager::FindMonitorOrNearest(int x, int y) const {
  const int exact = FindMonitorAt(x, y);
  if (exact >= 0 || monitors_.empty()) return exact;

  int bestIdx = 0;
  int64_t bestDistSq = LLONG_MAX;
  for (size_t i = 0; i < monitors_.size(); ++i) {
    const RECT& r = monitors_[i].desc.rect;
    const int64_t dx = (x < r.left) ? (r.left - x) : ((x >= r.right) ? (x - (r.right - 1)) : 0);
    const int64_t dy = (y < r.top) ? (r.top - y) : ((y >= r.bottom) ? (y - (r.bottom - 1)) : 0);
    const int64_t distSq = dx * dx + dy * dy;
    if (distSq < bestDistSq) {
      bestDistSq = distSq;
      bestIdx = static_cast<int>(i);
    }
  }
  return bestIdx;
}

int DisplayGuardManager::FindMonitorByTarget(LUID adapterLuid, UINT vidPnSourceId,
                                             UINT vidPnTargetId) const {
  for (size_t i = 0; i < monitors_.size(); ++i) {
    const FrameGuard::TargetId& t = monitors_[i].desc.target;
    if (t.valid && SameLuid(t.adapterLuid, adapterLuid) && t.vidPnSourceId == vidPnSourceId &&
        t.vidPnTargetId == vidPnTargetId) {
      return static_cast<int>(i);
    }
  }
  if (monitors_.size() == 1 && !monitors_[0].desc.target.valid) return 0;
  return -1;
}

void DisplayGuardManager::SeedPrevFrames(uint64_t latestFrameId) {
  if (monitors_.empty() || latestFrameId == 0) return;

  size_t remaining = 0;
  for (const MonitorSlot& slot : monitors_) {
    if (!slot.guard.hasPrevFrame()) ++remaining;
  }
  if (remaining == 0) return;

  const uint64_t minId = (latestFrameId > 16) ? (latestFrameId - 15) : 1;
  for (uint64_t fid = latestFrameId; fid >= minId && remaining > 0; --fid) {
    COMPOSITION_FRAME_STATS fs{};
    COMPOSITION_TARGET_ID tids[kStackTargetLimit]{};
    UINT actualTargets = 0;
    if (FAILED(DCompositionGetStatistics(fid, &fs, kStackTargetLimit, tids, &actualTargets)) ||
        actualTargets == 0) {
      if (fid == 1) break;
      continue;
    }
    const UINT count = std::min(actualTargets, kStackTargetLimit);
    for (UINT i = 0; i < count; ++i) {
      const int idx =
          FindMonitorByTarget(tids[i].displayAdapterLuid, tids[i].vidPnSourceId, tids[i].vidPnTargetId);
      if (idx < 0 || monitors_[static_cast<size_t>(idx)].guard.hasPrevFrame()) continue;

      COMPOSITION_TARGET_STATS ts{};
      if (FAILED(DCompositionGetTargetStatistics(fid, &tids[i], &ts))) continue;
      if (ts.completedStats.time == 0 || ts.completedStats.refreshCount == 0) continue;

      monitors_[static_cast<size_t>(idx)].guard.SeedPrevFrame(FrameGuard::CompletedFrame{
          fs.startTime,
          ts.completedStats.time,
          ts.vblankDuration ? ts.vblankDuration : fs.framePeriod,
          ts.completedStats.refreshCount,
      });
      --remaining;
    }
    if (fid == 1) break;
  }
}

bool DisplayGuardManager::Poll() {
  if (qpcFreq_ == 0 || monitors_.empty()) return false;

  COMPOSITION_FRAME_ID latest = 0;
  if (FAILED(DCompositionGetFrameId(COMPOSITION_FRAME_ID_COMPLETED, &latest)) || latest == 0) {
    return false;
  }

  if (!hasLastFrameId_) {
    lastFrameId_ = latest;
    hasLastFrameId_ = true;
    SeedPrevFrames(latest);
    return false;
  }

  if (latest <= lastFrameId_) return false;

  uint64_t startId = lastFrameId_ + 1;
  if (latest - lastFrameId_ > kMaxFramesPerPoll) {
    startId = latest - kMaxFramesPerPoll + 1;
    for (MonitorSlot& slot : monitors_) {
      slot.guard.ResetFrameAnchor();
    }
  }

  bool anyChanged = false;
  for (uint64_t fid = startId; fid <= latest; ++fid) {
    COMPOSITION_FRAME_STATS fs{};
    COMPOSITION_TARGET_ID tids[kStackTargetLimit]{};
    UINT actualTargets = 0;
    if (FAILED(DCompositionGetStatistics(fid, &fs, kStackTargetLimit, tids, &actualTargets)) ||
        actualTargets == 0) {
      continue;
    }
    const UINT count = std::min(actualTargets, kStackTargetLimit);
    for (UINT i = 0; i < count; ++i) {
      const int idx =
          FindMonitorByTarget(tids[i].displayAdapterLuid, tids[i].vidPnSourceId, tids[i].vidPnTargetId);
      if (idx < 0) continue;

      COMPOSITION_TARGET_STATS ts{};
      if (FAILED(DCompositionGetTargetStatistics(fid, &tids[i], &ts))) continue;
      if (ts.completedStats.time == 0 || ts.completedStats.refreshCount == 0) continue;

      const FrameGuard::CompletedFrame frame{
          fs.startTime,
          ts.completedStats.time,
          ts.vblankDuration ? ts.vblankDuration : fs.framePeriod,
          ts.completedStats.refreshCount,
      };
      if (monitors_[static_cast<size_t>(idx)].guard.OnFrameSample(frame)) {
        anyChanged = true;
      }
    }
  }

  lastFrameId_ = latest;
  return anyChanged;
}
