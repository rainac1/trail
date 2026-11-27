#include "overlay_renderer.h"

#include <algorithm>
#include <cmath>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "diag.h"

namespace {

// 设备丢失类错误：D3D/DComp 设备被移除/复位/挂起。出现这些错误时旧的设备、视觉树
// 全部失效，只能整栈重建（见头文件说明）。
bool IsDeviceLost(HRESULT hr) {
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
         hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

uint64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

void LogHr(const wchar_t* step, HRESULT hr) {
  DiagLog(L"[OverlayRenderer] %ls failed: 0x%08X", step, static_cast<unsigned>(hr));
}

// 记录 GPU 诊断信息（仅在启用日志时查询）。
void LogAdapterDiagnostics(ID3D11Device* device) {
  if (!DiagLogEnabled() || !device) return;
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
      SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
      DiagLog(L"[diag] adapter: %ls (vendor 0x%04X, device 0x%04X, VRAM %llu MB)", desc.Description,
              desc.VendorId, desc.DeviceId,
              static_cast<unsigned long long>(desc.DedicatedVideoMemory) / (1024 * 1024));
    }
  }
}

// 系统光标在给定 DPI 下的像素尺寸（物理像素）。系统按 DPI 缩放绘制光标，但
// GetCursorInfo/CopyIcon 给的是原生位图，所以这就是我们要缩放到的目标尺寸。
int CursorMetricForDpi(int index, UINT dpi) {
  return GetSystemMetricsForDpi(index, dpi);
}

uint8_t ClampByte(double v) {
  if (v <= 0.0) return 0;
  if (v >= 255.0) return 255;
  return static_cast<uint8_t>(v + 0.5);
}

// 尾随零计数，按约定 ctz(0) = -1。
int CountTrailingZeros(uint32_t v) {
  if (v == 0) return -1;
#if defined(_MSC_VER)
  unsigned long idx = 0;
  _BitScanForward(&idx, v);
  return static_cast<int>(idx);
#else
  return __builtin_ctz(v);
#endif
}

// 标准 IEC 61966-2-1 sRGB EOTF / OETF（逆 gamma 与正 gamma）。
double SrgbToLinear(double c) {
  if (c <= 0.0) return 0.0;
  if (c >= 1.0) return 1.0;
  return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

double LinearToSrgb(double l) {
  if (l <= 0.0) return 0.0;
  if (l >= 1.0) return 1.0;
  return (l <= 0.0031308) ? (l * 12.92) : (1.055 * std::pow(l, 1.0 / 2.4) - 0.055);
}

// 线性光直通 BGRA 光标位图（每通道 [0, 1]，用于缩放与各亮度级别预烘焙）。
struct LinearCursor {
  int width = 0;
  int height = 0;
  int hotX = 0;
  int hotY = 0;
  std::vector<float> pixels;  // width*height*4，B_lin, G_lin, R_lin, A
};

// 将 straight sRGB 光标转入线性光空间，并按需在线性光预乘空间做面积平均缩放到 (tw, th)。
void BuildLinearCursor(const CursorBitmap& src, int tw, int th, LinearCursor& dst) {
  dst = LinearCursor{};
  if (tw <= 0 || th <= 0) return;
  dst.width = tw;
  dst.height = th;
  dst.hotX = (src.width > 0)
                 ? static_cast<int>(std::lround(static_cast<double>(src.hotX) * tw / src.width))
                 : 0;
  dst.hotY = (src.height > 0)
                 ? static_cast<int>(std::lround(static_cast<double>(src.hotY) * th / src.height))
                 : 0;
  dst.pixels.assign(static_cast<size_t>(tw) * th * 4, 0.0f);
  if (src.width <= 0 || src.height <= 0 ||
      src.pixels.size() < static_cast<size_t>(src.width) * src.height * 4) {
    return;
  }

  if (src.width == tw && src.height == th) {
    const size_t n = static_cast<size_t>(tw) * th;
    for (size_t i = 0; i < n; ++i) {
      const uint8_t* p = &src.pixels[i * 4];
      const double a = static_cast<double>(p[3]) / 255.0;
      if (a <= 0.0) continue;
      float* q = &dst.pixels[i * 4];
      q[0] = static_cast<float>(SrgbToLinear(static_cast<double>(p[0]) / 255.0));
      q[1] = static_cast<float>(SrgbToLinear(static_cast<double>(p[1]) / 255.0));
      q[2] = static_cast<float>(SrgbToLinear(static_cast<double>(p[2]) / 255.0));
      q[3] = static_cast<float>(a);
    }
    return;
  }

  // 先把源图一次性转成线性预乘，避免在缩放核内层循环重复调用 pow。
  const size_t srcCount = static_cast<size_t>(src.width) * src.height;
  std::vector<float> srcPremul(srcCount * 4, 0.0f);
  for (size_t i = 0; i < srcCount; ++i) {
    const uint8_t* p = &src.pixels[i * 4];
    const double a = static_cast<double>(p[3]) / 255.0;
    if (a <= 0.0) continue;
    float* q = &srcPremul[i * 4];
    q[0] = static_cast<float>(SrgbToLinear(static_cast<double>(p[0]) / 255.0) * a);
    q[1] = static_cast<float>(SrgbToLinear(static_cast<double>(p[1]) / 255.0) * a);
    q[2] = static_cast<float>(SrgbToLinear(static_cast<double>(p[2]) / 255.0) * a);
    q[3] = static_cast<float>(a);
  }

  const double sx = static_cast<double>(src.width) / tw;
  const double sy = static_cast<double>(src.height) / th;
  for (int y = 0; y < th; ++y) {
    const double y0 = y * sy;
    const double y1 = y0 + sy;
    const int yyBegin = static_cast<int>(std::floor(y0));
    const int yyEnd = std::min(src.height, static_cast<int>(std::ceil(y1)));
    for (int x = 0; x < tw; ++x) {
      const double x0 = x * sx;
      const double x1 = x0 + sx;
      const int xxBegin = static_cast<int>(std::floor(x0));
      const int xxEnd = std::min(src.width, static_cast<int>(std::ceil(x1)));
      double b = 0.0, g = 0.0, r = 0.0, a = 0.0, wsum = 0.0;
      for (int yy = yyBegin; yy < yyEnd; ++yy) {
        const double wy =
            std::min(y1, static_cast<double>(yy + 1)) - std::max(y0, static_cast<double>(yy));
        if (wy <= 0.0) continue;
        for (int xx = xxBegin; xx < xxEnd; ++xx) {
          const double wx =
              std::min(x1, static_cast<double>(xx + 1)) - std::max(x0, static_cast<double>(xx));
          if (wx <= 0.0) continue;
          const double w = wx * wy;
          const float* p = &srcPremul[(static_cast<size_t>(yy) * src.width + xx) * 4];
          b += static_cast<double>(p[0]) * w;
          g += static_cast<double>(p[1]) * w;
          r += static_cast<double>(p[2]) * w;
          a += static_cast<double>(p[3]) * w;
          wsum += w;
        }
      }
      if (wsum > 0.0) {
        const double aOut = std::clamp(a / wsum, 0.0, 1.0);
        if (aOut > 1e-6) {
          float* q = &dst.pixels[(static_cast<size_t>(y) * tw + x) * 4];
          q[0] = static_cast<float>(std::clamp((b / wsum) / aOut, 0.0, 1.0));
          q[1] = static_cast<float>(std::clamp((g / wsum) / aOut, 0.0, 1.0));
          q[2] = static_cast<float>(std::clamp((r / wsum) / aOut, 0.0, 1.0));
          q[3] = static_cast<float>(aOut);
        }
      }
    }
  }
}

// 从线性光直通光标烘焙一张 premultiplied BGRA 贴图。
//   - advancedColor == true（HDR/ACM）：DWM 本身在 FP16 线性 scRGB 空间混合，直接编码为标准
//     sRGB 预乘贴图（由 visual SetOpacity 在线性空间施加 alphaLevel，避免双重补偿）。
//   - advancedColor == false（普通 SDR）：DWM 在非线性 sRGB 缓冲做仿射混合
//     C_out = C_premul + (1 - A_eff) * B，此处对每个像素做黑白双端点精确线性光补偿：
//       C0 = LinearToSrgb(a * F_lin)           （黑背景 B=0 严格精确）
//       C1 = LinearToSrgb(a * F_lin + 1 - a)   （白背景 B=1 严格精确）
//       C_premul = C0，A_eff = 1 - luma(C1 - C0)
//     纯黑描边在任意背景 B 上严格精确，纯白主体在黑/白背景严格精确。
void BakeLevelPixels(const LinearCursor& src, double alphaLevel, bool advancedColor,
                     std::vector<uint8_t>& out) {
  const size_t n = static_cast<size_t>(src.width) * src.height;
  out.assign(n * 4, 0);

  if (advancedColor) {
    for (size_t i = 0; i < n; ++i) {
      const float* p = &src.pixels[i * 4];
      const double a = std::clamp(static_cast<double>(p[3]) * alphaLevel, 0.0, 1.0);
      if (a <= 0.0) continue;
      const uint8_t aByte = ClampByte(a * 255.0);
      if (aByte == 0) continue;
      uint8_t* q = &out[i * 4];
      q[0] = std::min(aByte, ClampByte(LinearToSrgb(p[0]) * a * 255.0));
      q[1] = std::min(aByte, ClampByte(LinearToSrgb(p[1]) * a * 255.0));
      q[2] = std::min(aByte, ClampByte(LinearToSrgb(p[2]) * a * 255.0));
      q[3] = aByte;
    }
    return;
  }

  for (size_t i = 0; i < n; ++i) {
    const float* p = &src.pixels[i * 4];
    const double a = std::clamp(static_cast<double>(p[3]) * alphaLevel, 0.0, 1.0);
    if (a <= 0.0) continue;

    const double b0 = LinearToSrgb(a * static_cast<double>(p[0]));
    const double g0 = LinearToSrgb(a * static_cast<double>(p[1]));
    const double r0 = LinearToSrgb(a * static_cast<double>(p[2]));

    const double b1 = LinearToSrgb(a * static_cast<double>(p[0]) + (1.0 - a));
    const double g1 = LinearToSrgb(a * static_cast<double>(p[1]) + (1.0 - a));
    const double r1 = LinearToSrgb(a * static_cast<double>(p[2]) + (1.0 - a));

    const double trans = 0.0722 * (b1 - b0) + 0.7152 * (g1 - g0) + 0.2126 * (r1 - r0);
    double aEff = std::clamp(1.0 - trans, 0.0, 1.0);
    // 保证 premultiplied alpha 不变量 C_premul[c] <= A_eff（饱和彩色光标也安全）。
    aEff = std::clamp(std::max({aEff, b0, g0, r0}), 0.0, 1.0);

    const uint8_t aByte = ClampByte(aEff * 255.0);
    if (aByte == 0) continue;
    uint8_t* q = &out[i * 4];
    q[0] = std::min(aByte, ClampByte(b0 * 255.0));
    q[1] = std::min(aByte, ClampByte(g0 * 255.0));
    q[2] = std::min(aByte, ClampByte(r0 * 255.0));
    q[3] = aByte;
  }
}

}  // namespace

bool OverlayRenderer::Initialize(HWND hwnd, const RECT& windowRect, UINT dpi, int maxSprites,
                                 uint64_t qpcFreq, bool advancedColor, double opacityBase) {
  Shutdown();  // 可直接用于重建：先释放上一次创建的全部对象

  if (!hwnd || maxSprites < 1 || qpcFreq == 0) return false;
  rect_ = windowRect;
  qpcFreq_ = qpcFreq;
  advancedColor_ = advancedColor;
  opacityBase_ = (std::isfinite(opacityBase) && opacityBase > 1.0)
                     ? std::min(opacityBase, 100.0)
                     : 1.0;

  if (opacityBase_ <= 1.0 || maxSprites <= 1) {
    levelCount_ = 1;
    seqMask_ = 0;
  } else {
    uint32_t m = 1;
    int bits = 0;
    while (m < static_cast<uint32_t>(maxSprites)) {
      m <<= 1;
      ++bits;
    }
    seqMask_ = m - 1;
    levelCount_ = bits + 1;
  }
  levelAlphas_.resize(static_cast<size_t>(levelCount_));
  for (int l = 0; l < levelCount_; ++l) {
    levelAlphas_[static_cast<size_t>(l)] =
        (l == 0) ? 1.0f : static_cast<float>(std::pow(opacityBase_, -static_cast<double>(l)));
  }

  const UINT effectiveDpi = dpi ? dpi : 96;
  spriteW_ = CursorMetricForDpi(SM_CXCURSOR, effectiveDpi);
  spriteH_ = CursorMetricForDpi(SM_CYCURSOR, effectiveDpi);
  if (spriteW_ <= 0 || spriteH_ <= 0) return false;

  auto fail = [&]() {
    Shutdown();
    return false;
  };

  // 仅使用硬件加速设备。
  const UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, nullptr,
                                 0, D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, nullptr);
  if (FAILED(hr)) {
    LogHr(L"D3D11 hardware device", hr);
    return fail();
  }
  LogAdapterDiagnostics(d3dDevice_.Get());
  DiagLog(L"[diag] D3D feature level: 0x%04X", static_cast<unsigned>(featureLevel));
  d3dDevice_->GetImmediateContext(&d3dCtx_);

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  hr = d3dDevice_.As(&dxgiDevice);
  if (FAILED(hr)) {
    LogHr(L"device->IDXGIDevice QI", hr);
    return fail();
  }

  // 必须用 DCompositionCreateDevice3：基础版 DCompositionCreateDevice 创建出来的
  // visual 只暴露 Visual 基接口，QI 不到 IDCompositionVisual3（SetVisible / SetOpacity）。
  hr = DCompositionCreateDevice3(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice_));
  if (FAILED(hr)) {
    LogHr(L"DCompositionCreateDevice3", hr);
    return fail();
  }
  hr = dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, &target_);
  if (FAILED(hr)) {
    LogHr(L"CreateTargetForHwnd", hr);
    return fail();
  }
  hr = dcompDevice_->CreateVisual(&root_);
  if (FAILED(hr)) {
    LogHr(L"CreateVisual(root)", hr);
    return fail();
  }
  hr = target_->SetRoot(root_.Get());
  if (FAILED(hr)) {
    LogHr(L"SetRoot", hr);
    return fail();
  }

  // 按总级别数一次性创建预烘焙 Surface 表（默认 64 槽仅 7 张 32x32 贴图，总计 ~28KB）。
  hr = CreateLevelSurfaces(spriteW_, spriteH_, levelSurfaces_);
  if (FAILED(hr)) {
    LogHr(L"CreateSurface(levels)", hr);
    return fail();
  }

  // 环形槽位：每个槽位一个 visual，初始不可见。新采样点覆盖最老的槽位，
  // 因此槽位数就是"同时可见的采样点上限"。
  //
  // 初始树序（零成本，只在初始化时排一次）：AddVisual 在 referenceVisual 为 NULL 时
  // insertAbove=FALSE 表示"插到最上层"，于是下标越大 = 越新 = 越靠上。
  slots_.reserve(static_cast<size_t>(maxSprites));
  for (int i = 0; i < maxSprites; ++i) {
    Microsoft::WRL::ComPtr<IDCompositionVisual2> baseVisual;
    hr = dcompDevice_->CreateVisual(&baseVisual);
    if (FAILED(hr)) {
      LogHr(L"CreateVisual(sprite)", hr);
      return fail();
    }
    Slot slot;
    hr = baseVisual.As(&slot.visual);
    if (FAILED(hr)) {
      LogHr(L"visual->IDCompositionVisual3 QI", hr);
      return fail();
    }
    // 叶子节点显式设 MULTIPLY，确保 HDR/ACM 下使用 SetOpacity 不分配离屏中间层。
    slot.visual->SetOpacityMode(DCOMPOSITION_OPACITY_MODE_MULTIPLY);
    slot.visual->SetContent(levelSurfaces_[0].Get());
    slot.visual->SetOpacity(1.0f);
    slot.visual->SetVisible(FALSE);
    slot.level = 0;
    hr = root_->AddVisual(slot.visual.Get(), FALSE, nullptr);
    if (FAILED(hr)) {
      LogHr(L"AddVisual(sprite)", hr);
      return fail();
    }
    slots_.push_back(std::move(slot));
  }

  hr = dcompDevice_->Commit();
  if (FAILED(hr)) {
    LogHr(L"Commit (init)", hr);
    return fail();
  }

  head_ = 0;
  live_ = 0;
  seq_ = 0;
  hasCursorBitmap_ = false;
  loggedFailure_ = false;
  initialized_ = true;
  DiagLog(L"[OverlayRenderer] initialized: %d sprite visuals (%dx%d), levels=%d base=%.3f "
          L"color=%ls",
          maxSprites, spriteW_, spriteH_, levelCount_, opacityBase_,
          advancedColor_ ? L"linear(HDR/ACM)" : L"sRGB(SDR)");
  return true;
}

void OverlayRenderer::Shutdown() {
  // 先解绑视觉树与根，再释放（视觉 -> 根 -> 目标 -> surface -> DComp 设备 -> D3D），
  // 避免 DWM 仍引用已释放的内容。
  if (target_) target_->SetRoot(nullptr);
  slots_.clear();
  root_.Reset();
  target_.Reset();
  levelSurfaces_.clear();
  levelAlphas_.clear();
  dcompDevice_.Reset();
  d3dCtx_.Reset();
  d3dDevice_.Reset();

  seqMask_ = 0;
  seq_ = 0;
  levelCount_ = 1;
  head_ = 0;
  live_ = 0;
  spriteW_ = 0;
  spriteH_ = 0;
  hotX_ = 0;
  hotY_ = 0;
  advancedColor_ = false;
  hasCursorBitmap_ = false;
  initialized_ = false;
}

HRESULT OverlayRenderer::CreateLevelSurfaces(
    int width, int height, std::vector<Microsoft::WRL::ComPtr<IDCompositionSurface>>& out) {
  std::vector<Microsoft::WRL::ComPtr<IDCompositionSurface>> created;
  created.reserve(static_cast<size_t>(levelCount_));
  for (int l = 0; l < levelCount_; ++l) {
    Microsoft::WRL::ComPtr<IDCompositionSurface> surf;
    const HRESULT hr = dcompDevice_->CreateSurface(
        static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED, &surf);
    if (FAILED(hr)) return hr;
    created.push_back(std::move(surf));
  }
  out = std::move(created);
  return S_OK;
}

bool OverlayRenderer::UploadSurfacePixels(IDCompositionSurface* surface, const uint8_t* pixels,
                                          int width, int height) {
  POINT offset{0, 0};
  Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurface;
  HRESULT hr = surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
  if (FAILED(hr)) {
    LogHr(L"surface BeginDraw", hr);
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> dst;
  hr = dxgiSurface.As(&dst);
  if (SUCCEEDED(hr)) {
    const D3D11_BOX dstBox{
        static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0,
        static_cast<UINT>(offset.x + width), static_cast<UINT>(offset.y + height), 1};
    d3dCtx_->UpdateSubresource(dst.Get(), 0, &dstBox, pixels, static_cast<UINT>(width) * 4, 0);
  }
  const HRESULT endHr = surface->EndDraw();
  if (FAILED(hr)) {
    LogHr(L"upload sprite pixels", hr);
    return false;
  }
  if (FAILED(endHr)) {
    LogHr(L"surface EndDraw", endHr);
    return false;
  }
  return true;
}

int OverlayRenderer::LevelForSeq(uint32_t seq) const {
  if (levelCount_ <= 1) return 0;
  const uint32_t k = seq & seqMask_;
  const int level = CountTrailingZeros(k) + 1;  // ctz(0) = -1 -> 0 级
  return std::clamp(level, 0, levelCount_ - 1);
}

void OverlayRenderer::ApplySlotLevel(Slot& slot, int level) {
  level = std::clamp(level, 0, levelCount_ - 1);
  if (slot.level == level) return;
  if (advancedColor_) {
    slot.visual->SetOpacity(levelAlphas_[static_cast<size_t>(level)]);
  } else {
    slot.visual->SetContent(levelSurfaces_[static_cast<size_t>(level)].Get());
  }
  slot.level = level;
}

void OverlayRenderer::ResetSlotBindings() {
  if (levelSurfaces_.empty()) return;
  IDCompositionSurface* baseSurface = levelSurfaces_[0].Get();
  for (Slot& slot : slots_) {
    slot.visual->SetContent(baseSurface);
    slot.visual->SetOpacity(1.0f);
    slot.level = 0;
  }
}

bool OverlayRenderer::DeviceValid() {
  // 用 D3D 设备的移除原因判断：DCompositionCreateDevice3 返回的
  // IDCompositionDesktopDevice 没有 CheckDeviceState，而 DComp 设备由该 D3D 设备支撑，
  // D3D 被移除/复位时 DComp 内容一并失效。
  if (!initialized_ || !d3dDevice_) return false;
  const HRESULT reason = d3dDevice_->GetDeviceRemovedReason();
  if (FAILED(reason)) {
    DiagLog(L"[OverlayRenderer] device removed: 0x%08X", static_cast<unsigned>(reason));
    return false;
  }
  return true;
}

void OverlayRenderer::SetTrailLifetimeMs(double ms) {
  if (ms < 0.0) ms = 0.0;
  trailTicks_ = static_cast<uint64_t>(ms * static_cast<double>(qpcFreq_) / 1000.0);
}

OverlayRenderer::FrameResult OverlayRenderer::SetMonitorGeometry(const RECT& windowRect, UINT dpi,
                                                                 double trailMs,
                                                                 bool advancedColor,
                                                                 bool* needCursorReupload) {
  if (needCursorReupload) *needCursorReupload = false;
  if (!initialized_ || !dcompDevice_) return FrameResult::RecreateDevice;

  const bool hadLive = (live_ > 0);
  if (hadLive) HideVisibleSlots();

  rect_ = windowRect;
  SetTrailLifetimeMs(trailMs);

  const UINT effectiveDpi = dpi ? dpi : 96;
  const int newW = CursorMetricForDpi(SM_CXCURSOR, effectiveDpi);
  const int newH = CursorMetricForDpi(SM_CYCURSOR, effectiveDpi);
  if (newW <= 0 || newH <= 0) return FrameResult::Failed;

  const bool colorChanged = (advancedColor != advancedColor_);
  advancedColor_ = advancedColor;

  if (newW != spriteW_ || newH != spriteH_) {
    std::vector<Microsoft::WRL::ComPtr<IDCompositionSurface>> newSurfaces;
    const HRESULT hr = CreateLevelSurfaces(newW, newH, newSurfaces);
    if (FAILED(hr)) {
      LogHr(L"CreateSurface(levels resize)", hr);
      return IsDeviceLost(hr) ? FrameResult::RecreateDevice : FrameResult::Failed;
    }

    levelSurfaces_ = std::move(newSurfaces);
    ResetSlotBindings();
    spriteW_ = newW;
    spriteH_ = newH;
    hotX_ = 0;
    hotY_ = 0;
    hasCursorBitmap_ = false;
    if (needCursorReupload) *needCursorReupload = true;
    return CommitNow(nullptr);
  }

  if (colorChanged) {
    ResetSlotBindings();
    hasCursorBitmap_ = false;
    if (needCursorReupload) *needCursorReupload = true;
    return CommitNow(nullptr);
  }

  return hadLive ? CommitNow(nullptr) : FrameResult::Ok;
}

void OverlayRenderer::LogFailureOnce(const wchar_t* step, HRESULT hr) {
  if (loggedFailure_) return;
  loggedFailure_ = true;
  DiagLog(L"[OverlayRenderer] %ls failed: 0x%08X (further identical failures suppressed)", step,
          static_cast<unsigned>(hr));
}

bool OverlayRenderer::SetCursorBitmap(const CursorBitmap& bmp) {
  if (!initialized_ || !dcompDevice_ || levelSurfaces_.empty()) return false;
  if (bmp.width <= 0 || bmp.height <= 0) return false;

  LinearCursor linear;
  BuildLinearCursor(bmp, spriteW_, spriteH_, linear);
  if (linear.pixels.size() < static_cast<size_t>(linear.width) * linear.height * 4) return false;

  std::vector<uint8_t> baked;
  const int uploadCount = advancedColor_ ? 1 : levelCount_;
  for (int l = 0; l < uploadCount; ++l) {
    const double alpha = advancedColor_ ? 1.0 : static_cast<double>(levelAlphas_[static_cast<size_t>(l)]);
    BakeLevelPixels(linear, alpha, advancedColor_, baked);
    if (!UploadSurfacePixels(levelSurfaces_[static_cast<size_t>(l)].Get(), baked.data(),
                             linear.width, linear.height)) {
      return false;
    }
  }

  hotX_ = linear.hotX;
  hotY_ = linear.hotY;
  hasCursorBitmap_ = true;

  // 光标形状/热点已变：只隐藏当前活跃槽位，避免旧尾迹用新图形错位闪现。
  HideVisibleSlots();
  return CommitNow(nullptr) == FrameResult::Ok;
}

OverlayRenderer::FrameResult OverlayRenderer::Update(const Sample* samples, uint32_t count,
                                                     uint64_t now, bool* committed) {
  if (committed) *committed = false;
  if (!initialized_) return FrameResult::RecreateDevice;
  if (!hasCursorBitmap_ || slots_.empty()) return FrameResult::Ok;

  const int n = static_cast<int>(slots_.size());
  bool dirty = false;
  lastCommitTicks_ = 0;

  // 方案 B（零平移）：每个采样点按出生序号 seq 永久定级，仅保持队头（最新点）为 0 级、
  // 把上一个队头降级回其永久级别；每来 1 个点最多改 2 个视觉节点。
  const int oldNewest = (live_ > 0) ? ((head_ + live_ - 1) % n) : -1;
  int lastInserted = -1;

  // ---- 新采样点写入 FIFO 环形队列：满时覆盖队头最老的槽位 ----
  for (uint32_t i = 0; i < count; ++i) {
    if (samples[i].x < rect_.left || samples[i].x >= rect_.right || samples[i].y < rect_.top ||
        samples[i].y >= rect_.bottom) {
      continue;
    }

    uint64_t born = samples[i].qpc ? samples[i].qpc : now;
    if (trailTicks_ > 0 && now >= born + trailTicks_) continue;  // 入队前已过期
    if (live_ > 0) {
      const int newest = (head_ + live_ - 1) % n;
      if (born < slots_[static_cast<size_t>(newest)].bornQpc) {
        born = slots_[static_cast<size_t>(newest)].bornQpc;
      }
    }

    int idx = 0;
    if (live_ < n) {
      idx = (head_ + live_) % n;
      ++live_;
      slots_[static_cast<size_t>(idx)].visual->SetVisible(TRUE);
    } else {
      idx = head_;
      head_ = (head_ + 1) % n;
    }

    // 把前一个最新点定格到它的永久出生级别（批次内同一槽位最多更新一次）。
    if (lastInserted >= 0 && lastInserted != idx) {
      Slot& prev = slots_[static_cast<size_t>(lastInserted)];
      ApplySlotLevel(prev, LevelForSeq(prev.seq));
    } else if (lastInserted < 0 && oldNewest >= 0 && oldNewest != idx) {
      Slot& prev = slots_[static_cast<size_t>(oldNewest)];
      ApplySlotLevel(prev, LevelForSeq(prev.seq));
    }

    Slot& slot = slots_[static_cast<size_t>(idx)];
    // offset 用窗口客户区坐标（窗口左上角 = rect_ 左上角）。
    slot.visual->SetOffsetX(static_cast<float>(samples[i].x - rect_.left - hotX_));
    slot.visual->SetOffsetY(static_cast<float>(samples[i].y - rect_.top - hotY_));
    slot.bornQpc = born;
    slot.seq = seq_++;
    lastInserted = idx;
    dirty = true;
  }

  // ---- 过期：从队头弹出时间窗之外的采样点 ----
  if (HideExpired(now)) dirty = true;

  // 保证当前最新可见采样点为 0 级（不透明度 1.0）。
  if (live_ > 0 && lastInserted >= 0) {
    const int newest = (head_ + live_ - 1) % n;
    ApplySlotLevel(slots_[static_cast<size_t>(newest)], 0);
  }

  if (!dirty) return FrameResult::Ok;
  return CommitNow(committed);
}

OverlayRenderer::FrameResult OverlayRenderer::CommitNow(bool* committed) {
  if (committed) *committed = false;
  if (!initialized_ || !dcompDevice_) return FrameResult::RecreateDevice;

  const uint64_t commitStart = QpcNow();
  const HRESULT hr = dcompDevice_->Commit();
  lastCommitTicks_ = QpcNow() - commitStart;

  if (IsDeviceLost(hr)) {
    LogHr(L"Commit (device lost)", hr);
    return FrameResult::RecreateDevice;
  }
  if (FAILED(hr)) {
    LogFailureOnce(L"Commit", hr);
    return FrameResult::Failed;
  }
  loggedFailure_ = false;
  if (committed) *committed = true;
  return FrameResult::Ok;
}

bool OverlayRenderer::HideExpired(uint64_t now) {
  if (trailTicks_ == 0 || live_ == 0) return false;
  const int n = static_cast<int>(slots_.size());
  bool dirty = false;
  while (live_ > 0) {
    Slot& slot = slots_[static_cast<size_t>(head_)];
    if (now < slot.bornQpc + trailTicks_) break;
    slot.visual->SetVisible(FALSE);
    slot.bornQpc = 0;
    head_ = (head_ + 1) % n;
    --live_;
    dirty = true;
  }
  if (live_ == 0) {
    head_ = 0;  // 停顿清空后从槽 0 与序号 0 重来，减少回绕并让新一笔从 0 级起步
    seq_ = 0;
  }
  return dirty;
}

void OverlayRenderer::HideVisibleSlots() {
  if (live_ == 0) {
    seq_ = 0;
    return;
  }
  const int n = static_cast<int>(slots_.size());
  for (int k = 0; k < live_; ++k) {
    Slot& slot = slots_[static_cast<size_t>((head_ + k) % n)];
    slot.visual->SetVisible(FALSE);
    slot.bornQpc = 0;
    slot.seq = 0;
  }
  head_ = 0;
  live_ = 0;
  seq_ = 0;
}

uint64_t OverlayRenderer::NextExpiryQpc() const {
  if (!initialized_ || trailTicks_ == 0 || live_ == 0) return 0;
  return slots_[static_cast<size_t>(head_)].bornQpc + trailTicks_;
}

OverlayRenderer::FrameResult OverlayRenderer::Expire(uint64_t now, bool* committed) {
  if (committed) *committed = false;
  if (!initialized_ || !dcompDevice_) return FrameResult::RecreateDevice;
  if (!HideExpired(now)) return FrameResult::Ok;
  return CommitNow(committed);
}

void OverlayRenderer::HideAll() {
  if (!initialized_ || !dcompDevice_ || live_ == 0) return;
  HideVisibleSlots();
  CommitNow(nullptr);  // 尽力而为：失败由看门狗/下次 Update 兜住
}
