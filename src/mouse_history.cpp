#include "mouse_history.h"

namespace {
constexpr int kSystemHistory = 64;  // 系统鼠标移动历史缓冲区的固定容量
}

int CollectMouseHistory(Sample* out, int outCap, MouseHistoryTracker& tracker,
                        int curX, int curY) {
  if (outCap <= 0) return 0;

  // anchor 必须是 16-bit 掩码后的当前坐标：多显示器下左侧/上方显示器坐标为负，
  // 系统按 16-bit 无符号回绕存储，不掩码则匹配不上（见 MSDN Remarks 的变换示例）。
  MOUSEMOVEPOINT anchor{};
  anchor.x = curX & 0xFFFF;
  anchor.y = curY & 0xFFFF;

  MOUSEMOVEPOINT buf[kSystemHistory];
  const int n = GetMouseMovePointsEx(sizeof(MOUSEMOVEPOINT), &anchor, buf, kSystemHistory,
                                     GMMP_USE_DISPLAY_POINTS);
  if (n <= 0) return 0;  // anchor 未命中（-1，被 64 点上限冲掉）或历史为空

  // 解 16-bit 回绕，恢复负坐标（物理像素）。
  for (int i = 0; i < n; ++i) {
    if (buf[i].x > 32767) buf[i].x -= 65536;
    if (buf[i].y > 32767) buf[i].y -= 65536;
  }

  // 返回数组"最新在前"（buf[0] 是 anchor 匹配到的最新点）。校验 buf[0] 确为当前
  // 坐标：若当前像素与历史中某旧点重合且系统匹配到旧副本，buf[0] 会偏离 cur，
  // 本帧放弃历史轨迹（仅画实时头部点），下一帧自愈。
  if (buf[0].x != curX || buf[0].y != curY) return 0;

  // 首帧只建立水印、不输出历史，避免启动瞬间画出整条 ≤64 点旧轨迹。
  if (!tracker.haveWatermark) {
    tracker.wm = buf[0];
    tracker.haveWatermark = true;
    return 0;
  }

  // 找"比水印新"的前缀：buf[0..newCount-1] 都是新增点，遇到水印即止。
  int newCount = 0;
  while (newCount < n) {
    const MOUSEMOVEPOINT& p = buf[newCount];
    if (p.x == tracker.wm.x && p.y == tracker.wm.y && p.time == tracker.wm.time) break;
    ++newCount;
  }
  // newCount == n 且未命中：两帧间新增超过 64 点，历史被截断，丢弃中间段即可。

  // 反转 buf[0..newCount-1]（最新在前）为 out（最旧在前）。若超过 outCap，
  // 只保留最新的 outCap 个点（丢弃更旧的），尾迹的"最新"部分更重要。
  const int keep = newCount < outCap ? newCount : outCap;
  int produced = 0;
  for (int k = keep - 1; k >= 0; --k) {
    out[produced].x = buf[k].x;
    out[produced].y = buf[k].y;
    ++produced;
  }

  // 水印推进到本帧最新点（含 time，供下次 (x,y,time) 精确匹配，区分同像素不同时刻）。
  tracker.wm = buf[0];
  return produced;
}
