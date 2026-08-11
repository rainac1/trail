#pragma once
#include <windows.h>
#include <wrl/client.h>

#include <d2d1_1.h>
#include <dxgi1_2.h>

// 缓存的光标纹理：以 hCursor 句柄为 key，形状变化时才重新抓取。
struct CursorTexture {
  HCURSOR handle = nullptr;  // 缓存对应的句柄
  int width = 0;
  int height = 0;
  int hotX = 0;  // 热点（相对位图左上角）
  int hotY = 0;
  Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;  // premultiplied BGRA
};

// 通过 Windows API（CopyIcon + GetIconInfo + GetDIBits）抓取光标像素，
// 生成 premultiplied 的 ID2D1Bitmap 存入 out。失败返回 false（out 不变）。
bool CaptureCursorTexture(ID2D1DeviceContext* dc, HCURSOR hCursor, CursorTexture& out);
