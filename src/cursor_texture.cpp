#include "cursor_texture.h"

#include <cstdint>
#include <vector>

#include <d2d1helper.h>

namespace {

// 经典 mask-only 光标（hbmColor 为空）：用 AND/XOR 两张 1bpp 位图合成黑白形状。
// hbmMask 高度为 2*h：上半为 AND mask，下半为 XOR mask。
bool BuildFromMonoMask(HBITMAP mask, int& outW, int& outH, std::vector<uint8_t>& outPx) {
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
  HGDIOBJ old = SelectObject(mem, mask);
  const int lines = static_cast<int>(GetDIBits(mem, mask, 0, static_cast<UINT>(bm.bmHeight),
                                               mono.data(), &bi, DIB_RGB_COLORS));
  SelectObject(mem, old);
  DeleteDC(mem);
  if (lines != bm.bmHeight) return false;

  outPx.assign(static_cast<size_t>(w) * h * 4, 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t* andRow = mono.data() + static_cast<size_t>(y) * rowBytes;
    const uint8_t* xorRow = mono.data() + static_cast<size_t>(y + h) * rowBytes;
    for (int x = 0; x < w; ++x) {
      const bool andBit = (andRow[x >> 3] >> (7 - (x & 7))) & 1;
      if (andBit) continue;  // 透明
      const bool xorBit = (xorRow[x >> 3] >> (7 - (x & 7))) & 1;
      uint8_t* p = &outPx[(static_cast<size_t>(y) * w + x) * 4];
      const uint8_t v = xorBit ? 255 : 0;
      p[0] = p[1] = p[2] = v;  // 已预乘（alpha=255）
      p[3] = 255;
    }
  }
  outW = w;
  outH = h;
  return true;
}

// 抓取 hCursor 的像素（premultiplied BGRA）。
bool FetchCursorPixels(HCURSOR hCursor, std::vector<uint8_t>& outPx, int& outW, int& outH,
                       int& outHotX, int& outHotY) {
  HICON icon = CopyIcon(hCursor);
  if (!icon) return false;

  ICONINFO ii{};
  const BOOL got = GetIconInfo(icon, &ii);
  if (!got) {
    DestroyIcon(icon);
    return false;
  }

  bool ok = false;
  outW = outH = 0;

  // 主路径：32bpp 彩色光标（现代 Windows 光标的普遍形式，含 alpha 通道）。
  if (ii.hbmColor) {
    BITMAP bm{};
    GetObjectW(ii.hbmColor, sizeof(bm), &bm);
    outW = bm.bmWidth;
    outH = bm.bmHeight;
    if (bm.bmBitsPixel == 32 && outW > 0 && outH > 0 && outW <= 1024 && outH <= 1024) {
      outPx.assign(static_cast<size_t>(outW) * outH * 4, 0);
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = outW;
      bi.bmiHeader.biHeight = -outH;  // top-down
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;

      HDC mem = CreateCompatibleDC(nullptr);
      HGDIOBJ old = SelectObject(mem, ii.hbmColor);
      const int lines = static_cast<int>(GetDIBits(mem, ii.hbmColor, 0, static_cast<UINT>(outH),
                                                   outPx.data(), &bi, DIB_RGB_COLORS));
      SelectObject(mem, old);
      DeleteDC(mem);
      if (lines == outH) {
        // 32bpp DIB 内存字节序为 B,G,R,A。把 straight alpha 预乘为 premultiplied。
        for (size_t i = 0; i < static_cast<size_t>(outW) * outH; ++i) {
          uint8_t* p = &outPx[i * 4];
          const uint32_t a = p[3];
          p[0] = static_cast<uint8_t>((uint32_t)p[0] * a / 255);
          p[1] = static_cast<uint8_t>((uint32_t)p[1] * a / 255);
          p[2] = static_cast<uint8_t>((uint32_t)p[2] * a / 255);
        }
        ok = true;
      }
    }
  }

  // fallback：旧式 mask-only 光标（以及少数低色深彩色光标），仅保证形状正确。
  if (!ok && ii.hbmMask) {
    ok = BuildFromMonoMask(ii.hbmMask, outW, outH, outPx);
  }

  if (ok) {
    outHotX = ii.xHotspot;
    outHotY = ii.yHotspot;
  }

  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  DestroyIcon(icon);
  return ok;
}

}  // namespace

bool CaptureCursorTexture(ID2D1DeviceContext* dc, HCURSOR hCursor, CursorTexture& out) {
  std::vector<uint8_t> px;
  int w = 0, h = 0, hx = 0, hy = 0;
  if (!FetchCursorPixels(hCursor, px, w, h, hx, hy)) return false;

  D2D1_BITMAP_PROPERTIES1 props{};
  props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
  props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

  Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp;
  const HRESULT hr = dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)),
                                      px.data(), static_cast<UINT32>(w * 4), props, &bmp);
  if (FAILED(hr)) return false;

  out.handle = hCursor;
  out.width = w;
  out.height = h;
  out.hotX = hx;
  out.hotY = hy;
  out.bitmap = bmp;  // ComPtr<ID2D1Bitmap1> -> ComPtr<ID2D1Bitmap>
  return true;
}
