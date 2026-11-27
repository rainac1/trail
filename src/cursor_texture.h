#pragma once
#include <windows.h>

#include <cstdint>
#include <vector>

// 光标位图：straight alpha BGRA（32bpp，sRGB 编码）。
//
// 保持直通 alpha 以便渲染器在线性光空间完成 DPI 缩放与分级不透明度预烘焙；
// 运行期热路径上没有任何像素操作，只有 visual 的 offset / visible / level 切换。
struct CursorBitmap {
  int width = 0;
  int height = 0;
  int hotX = 0;  // 热点（相对位图左上角）
  int hotY = 0;
  std::vector<uint8_t> pixels;  // width*height*4，B,G,R,A，straight alpha（sRGB）
};

// 通过 Windows API（CopyIcon + GetIconInfo + GetDIBits）抓取 hCursor 的像素与热点。
// 失败返回 false（out 不变）。只在光标句柄变化时调用。
bool CaptureCursorBitmap(HCURSOR hCursor, CursorBitmap& out);
