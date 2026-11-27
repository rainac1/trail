#include "cursor_texture.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace {

// 1bpp DIB 需要 2 项调色板（标准 BITMAPINFO 的 bmiColors 仅 1 项，直接传给 GetDIBits 会栈越界）。
struct MonoBitmapInfo {
  BITMAPINFOHEADER bmiHeader;
  RGBQUAD bmiColors[2];
};

// 读取 1bpp 位图（top-down）。GetDIBits 要求目标位图未被选入 DC。
bool ReadMonoBits(HBITMAP mask, int w, int h, std::vector<uint8_t>& mono, uint32_t& rowBytes) {
  rowBytes = static_cast<uint32_t>(((w + 31) / 32) * 4);
  mono.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(h));

  MonoBitmapInfo bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 1;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC mem = CreateCompatibleDC(nullptr);
  if (!mem) return false;
  const int lines = static_cast<int>(GetDIBits(mem, mask, 0, static_cast<UINT>(h), mono.data(),
                                               reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS));
  DeleteDC(mem);
  return lines == h;
}

// 经典 mask-only 光标（hbmColor 为空）：用 AND/XOR 两张 1bpp 位图合成形状。
// hbmMask 高度为 2*h：上半为 AND mask，下半为 XOR mask。
bool BuildFromMonoMask(HBITMAP mask, CursorBitmap& out) {
  BITMAP bm{};
  if (!GetObjectW(mask, sizeof(bm), &bm)) return false;
  if (bm.bmWidth <= 0 || bm.bmHeight <= 0 || (bm.bmHeight & 1) != 0) return false;

  const int w = bm.bmWidth;
  const int h = bm.bmHeight / 2;
  std::vector<uint8_t> mono;
  uint32_t rowBytes = 0;
  if (!ReadMonoBits(mask, w, bm.bmHeight, mono, rowBytes)) return false;

  out.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t* andRow = mono.data() + static_cast<size_t>(y) * rowBytes;
    const uint8_t* xorRow = mono.data() + static_cast<size_t>(y + h) * rowBytes;
    for (int x = 0; x < w; ++x) {
      const bool andBit = (andRow[x >> 3] >> (7 - (x & 7))) & 1;
      const bool xorBit = (xorRow[x >> 3] >> (7 - (x & 7))) & 1;
      if (andBit && !xorBit) continue;  // 透明
      uint8_t* p = &out.pixels[(static_cast<size_t>(y) * w + x) * 4];
      // AND=0,XOR=0 -> 黑；AND=0,XOR=1 -> 白；AND=1,XOR=1 -> 反色（如工字光标），
      // 叠加层不支持 XOR 混合，用中灰保证在深/浅背景上均可见。
      const uint8_t v = andBit ? 128 : (xorBit ? 255 : 0);
      p[0] = p[1] = p[2] = v;  // alpha=255 时 straight 与 premultiplied 相同
      p[3] = 255;
    }
  }
  out.width = w;
  out.height = h;
  return true;
}

// 全零 alpha 的旧式 32bpp 彩色光标：用 1bpp AND mask 决定透明与不透明。
bool ApplyAndMaskAlpha(HBITMAP mask, int w, int h, std::vector<uint8_t>& pixels) {
  BITMAP bm{};
  if (!GetObjectW(mask, sizeof(bm), &bm)) return false;
  if (bm.bmWidth != w || bm.bmHeight != h) return false;

  std::vector<uint8_t> mono;
  uint32_t rowBytes = 0;
  if (!ReadMonoBits(mask, w, h, mono, rowBytes)) return false;

  for (int y = 0; y < h; ++y) {
    const uint8_t* andRow = mono.data() + static_cast<size_t>(y) * rowBytes;
    for (int x = 0; x < w; ++x) {
      const bool transparent = (andRow[x >> 3] >> (7 - (x & 7))) & 1;
      uint8_t* p = &pixels[(static_cast<size_t>(y) * w + x) * 4];
      if (transparent) {
        p[0] = p[1] = p[2] = p[3] = 0;
      } else {
        p[3] = 255;  // alpha=255 时 straight 与 premultiplied 相同
      }
    }
  }
  return true;
}

// 抓取 hCursor 的像素到 out（straight alpha sRGB BGRA）。宽高写入 out.width/out.height。
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
    if (GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmBitsPixel == 32 && bm.bmWidth > 0 &&
        bm.bmHeight > 0 && bm.bmWidth <= 1024 && bm.bmHeight <= 1024) {
      const int w = bm.bmWidth;
      const int h = bm.bmHeight;
      const size_t pixelCount = static_cast<size_t>(w) * h;
      out.pixels.assign(pixelCount * 4, 0);

      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = w;
      bi.bmiHeader.biHeight = -h;  // top-down
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;

      HDC mem = CreateCompatibleDC(nullptr);
      if (mem) {
        const int lines = static_cast<int>(GetDIBits(mem, ii.hbmColor, 0, static_cast<UINT>(h),
                                                     out.pixels.data(), &bi, DIB_RGB_COLORS));
        DeleteDC(mem);
        if (lines == h) {
          bool hasAlpha = false;
          for (size_t i = 0; i < pixelCount; ++i) {
            if (out.pixels[i * 4 + 3] != 0) {
              hasAlpha = true;
              break;
            }
          }
          if (hasAlpha) {
            // 32bpp DIB 字节序为 B,G,R,A（straight alpha）。保留直通通道，仅清理全透明像素，
            // 让渲染器在线性光空间完成缩放与分级预乘烘焙。
            for (size_t i = 0; i < pixelCount; ++i) {
              uint8_t* p = &out.pixels[i * 4];
              if (p[3] == 0) p[0] = p[1] = p[2] = 0;
            }
            ok = true;
          } else if (ii.hbmMask) {
            ok = ApplyAndMaskAlpha(ii.hbmMask, w, h, out.pixels);
          }
          if (ok) {
            out.width = w;
            out.height = h;
          }
        }
      }
    }
  }

  // fallback：旧式 mask-only 光标（以及少数低色深彩色光标），仅保证形状正确。
  if (!ok && ii.hbmMask) ok = BuildFromMonoMask(ii.hbmMask, out);

  if (ok) {
    out.hotX = static_cast<int>(ii.xHotspot);
    out.hotY = static_cast<int>(ii.yHotspot);
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
