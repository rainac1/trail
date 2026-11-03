#pragma once
#include <windows.h>

#include <cstdint>
#include <vector>

// 光标位图：premultiplied BGRA（32bpp）。
//
// 视觉树方案里这份位图不是"每帧画的纹理"，而是**唯一一份**内容：上传到一张小的
// DirectComposition surface 后被所有采样点视觉共享。于是运行期的热路径上没有任何
// 像素操作，只有 visual 的 offset/opacity 变化 —— 见 overlay_renderer.h。
struct CursorBitmap {
  int width = 0;
  int height = 0;
  int hotX = 0;  // 热点（相对位图左上角）
  int hotY = 0;
  std::vector<uint8_t> pixels;  // width*height*4，B,G,R,A，已预乘
};

// 通过 Windows API（CopyIcon + GetIconInfo + GetDIBits）抓取 hCursor 的像素与热点。
// 失败返回 false（out 不变）。只在光标句柄变化时调用。
bool CaptureCursorBitmap(HCURSOR hCursor, CursorBitmap& out);
