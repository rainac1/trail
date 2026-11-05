#pragma once
#include <cstdint>

// 一个鼠标轨迹采样点（虚拟桌面物理像素坐标，与 GetCursorInfo().ptScreenPos、
// MSLLHOOKSTRUCT.pt 同一坐标空间）。
struct Sample {
  int32_t x;
  int32_t y;
};
