#pragma once
#include <windows.h>

#include <cstdint>

// 一个鼠标轨迹采样点（虚拟桌面物理像素坐标，与 GetCursorPos / GetCursorInfo
// 的 ptScreenPos 同一坐标空间）。
struct Sample {
  int32_t x;
  int32_t y;
};

// 系统鼠标移动历史的增量读取状态（渲染线程独占，无需原子/锁）。
struct MouseHistoryTracker {
  bool haveWatermark = false;
  MOUSEMOVEPOINT wm{};  // 上次已消费的最新点 (x, y, time)
};

// 每帧调用一次：以 (curX, curY) 为 anchor，从系统 64 点鼠标移动历史中取回
// "自上次调用以来新增"的轨迹点，按时间从旧到新写入 out，返回点数。
//
// 返回 0 表示本帧无新增轨迹（鼠标静止、anchor 未命中被 64 点上限冲掉、或首帧
// 仅建立水印）。outCap 建议 >= 64（系统历史上限）。
//
// 原理：GetMouseMovePointsEx 不是"增量读取"接口——它返回 anchor 点及其之前
// （更旧）的 ≤64 个点，且读取后历史不被消费。因此用 (x,y,time) 水印记录上次
// 已消费的最新点，每帧取回整条历史后只保留"比水印新"的前缀，即本帧增量。
int CollectMouseHistory(Sample* out, int outCap, MouseHistoryTracker& tracker,
                        int curX, int curY);
