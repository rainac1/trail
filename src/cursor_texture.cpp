#include "cursor_texture.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace {

// 经典 mask-only 光标（hbmColor 为空）：用 AND/XOR 两张 1bpp 位图合成黑白形状。
// hbmMask 高度为 2*h：上半为 AND mask，下半为 XOR mask。
bool BuildFromMonoMask(HBITMAP mask, CursorBitmap& out) {
  BITMAP bm{};
  if (!GetObjectW(mask, sizeof(bm), &bm)) return false;
  if (bm.bmWidth <= 0 || bm.bmHeight <= 0 || (bm.bmHeight & 1) != 0) return false;

  const int w = bm.bmWidth;
  const int h = bm.bmHeight / 2;
  const uint32_t rowBytes = static_cast<uint32_t>(((w + 31) / 32) * 4);

  std::vector<uint8_t> mono(static_cast<size_t>(rowBytes) * bm.bmHeight);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -bm.bmHeight;  // top-down：AND 在上半，XOR 在下半
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 1;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC mem = CreateCompatibleDC(nullptr);
  if (!mem) return false;
  HGDIOBJ old = SelectObject(mem, mask);
  const int lines = static_cast<int>(GetDIBits(mem, mask, 0, static_cast<UINT>(bm.bmHeight),
                                               mono.data(), &bi, DIB_RGB_COLORS));
  SelectObject(mem, old);
  DeleteDC(mem);
  if (lines != bm.bmHeight) return false;

  out.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t* andRow = mono.data() + static_cast<size_t>(y) * rowBytes;
    const uint8_t* xorRow = mono.data() + static_cast<size_t>(y + h) * rowBytes;
    for (int x = 0; x < w; ++x) {
      const bool andBit = (andRow[x >> 3] >> (7 - (x & 7))) & 1;
      if (andBit) continue;  // 透明
      const bool xorBit = (xorRow[x >> 3] >> (7 - (x & 7))) & 1;
      uint8_t* p = &out.pixels[(static_cast<size_t>(y) * w + x) * 4];
      const uint8_t v = xorBit ? 255 : 0;
      p[0] = p[1] = p[2] = v;  // 已预乘（alpha=255）
      p[3] = 255;
    }
  }
  out.width = w;
  out.height = h;
  return true;
}

// 抓取 hCursor 的像素到 out（premultiplied BGRA）。宽高写入 out.width/out.height。
bool FetchCursorPixels(HCURSOR hCursor, CursorBitmap& out) {
  HICON icon = CopyIcon(hCursor);
  if (!icon) return false;

  ICONINFO ii{};
  if (!GetIconInfo(icon, &ii)) {
    DestroyIcon(icon);
    return false;
  }

  bool ok = false;

  // 主路径：32bpp 彩色光标（现代 Windows 光标的普遍形式，含 alpha 通道）。
  if (ii.hbmColor) {
    BITMAP bm{};
    GetObjectW(ii.hbmColor, sizeof(bm), &bm);
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    if (bm.bmBitsPixel == 32 && w > 0 && h > 0 && w <= 1024 && h <= 1024) {
      out.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = w;
      bi.bmiHeader.biHeight = -h;  // top-down
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;

      HDC mem = CreateCompatibleDC(nullptr);
      if (mem) {
        HGDIOBJ old = SelectObject(mem, ii.hbmColor);
        const int lines = static_cast<int>(GetDIBits(mem, ii.hbmColor, 0, static_cast<UINT>(h),
                                                     out.pixels.data(), &bi, DIB_RGB_COLORS));
        SelectObject(mem, old);
        DeleteDC(mem);
        if (lines == h) {
          // 32bpp DIB 内存字节序为 B,G,R,A。把 straight alpha 预乘为 premultiplied。
          for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            uint8_t* p = &out.pixels[i * 4];
            const uint32_t a = p[3];
            p[0] = static_cast<uint8_t>((uint32_t)p[0] * a / 255);
            p[1] = static_cast<uint8_t>((uint32_t)p[1] * a / 255);
            p[2] = static_cast<uint8_t>((uint32_t)p[2] * a / 255);
          }
          out.width = w;
          out.height = h;
          ok = true;
        }
      }
    }
  }

  // fallback：旧式 mask-only 光标（以及少数低色深彩色光标），仅保证形状正确。
  if (!ok && ii.hbmMask) ok = BuildFromMonoMask(ii.hbmMask, out);

  if (ok) {
    out.hotX = ii.xHotspot;
    out.hotY = ii.yHotspot;
  }

  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  DestroyIcon(icon);
  return ok;
}

}  // namespace

bool CaptureCursorBitmap(HCURSOR hCursor, CursorBitmap& out) {
  CursorBitmap tmp;
  if (!FetchCursorPixels(hCursor, tmp)) return false;
  out = std::move(tmp);
  return true;
}
