#include "overlay_renderer.h"

#include <algorithm>
#include <cmath>

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

// 把光标位图缩放到指定像素尺寸。像素是预乘 alpha，所以对四个通道直接做面积平均即可
// （放大时退化为双线性插值）。热点按同一比例缩放。
void ScaleCursor(const CursorBitmap& src, int tw, int th, CursorBitmap& dst) {
  dst = CursorBitmap{};
  if (tw <= 0 || th <= 0) return;
  dst.width = tw;
  dst.height = th;
  dst.hotX = (src.width > 0)
                 ? static_cast<int>(std::lround(static_cast<double>(src.hotX) * tw / src.width))
                 : 0;
  dst.hotY = (src.height > 0)
                 ? static_cast<int>(std::lround(static_cast<double>(src.hotY) * th / src.height))
                 : 0;
  dst.pixels.assign(static_cast<size_t>(tw) * th * 4, 0);
  if (src.width <= 0 || src.height <= 0 ||
      src.pixels.size() < static_cast<size_t>(src.width) * src.height * 4) {
    return;
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
          const uint8_t* p = &src.pixels[(static_cast<size_t>(yy) * src.width + xx) * 4];
          b += p[0] * w;
          g += p[1] * w;
          r += p[2] * w;
          a += p[3] * w;
          wsum += w;
        }
      }
      uint8_t* q = &dst.pixels[(static_cast<size_t>(y) * tw + x) * 4];
      if (wsum > 0.0) {
        q[0] = ClampByte(b / wsum);
        q[1] = ClampByte(g / wsum);
        q[2] = ClampByte(r / wsum);
        q[3] = ClampByte(a / wsum);
      }
    }
  }
}

}  // namespace

bool OverlayRenderer::Initialize(HWND hwnd, const RECT& windowRect, UINT dpi, int maxSprites,
                                 uint64_t qpcFreq) {
  Shutdown();  // 可直接用于重建：先释放上一次创建的全部对象

  if (!hwnd || maxSprites < 1 || qpcFreq == 0) return false;
  rect_ = windowRect;
  qpcFreq_ = qpcFreq;

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
  // visual 只暴露 Visual 基接口，QI 不到 IDCompositionVisual3（SetVisible）。
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

  // 窗口 DPI 固定，光标目标像素尺寸不变：surface 只创建一次并绑定到全部 visual，
  // 后续光标变形仅原位更新像素。
  hr = dcompDevice_->CreateSurface(static_cast<UINT>(spriteW_), static_cast<UINT>(spriteH_),
                                   DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                                   &spriteSurface_);
  if (FAILED(hr)) {
    LogHr(L"CreateSurface(sprite)", hr);
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
    slot.visual->SetContent(spriteSurface_.Get());
    slot.visual->SetVisible(FALSE);
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
  hasCursorBitmap_ = false;
  loggedFailure_ = false;
  initialized_ = true;
  DiagLog(L"[OverlayRenderer] initialized: %d sprite visuals (%dx%d), DirectComposition visual tree",
          maxSprites, spriteW_, spriteH_);
  return true;
}

void OverlayRenderer::Shutdown() {
  // 先解绑视觉树与根，再释放（视觉 -> 根 -> 目标 -> surface -> DComp 设备 -> D3D），
  // 避免 DWM 仍引用已释放的内容。
  if (target_) target_->SetRoot(nullptr);
  slots_.clear();
  root_.Reset();
  target_.Reset();
  spriteSurface_.Reset();
  dcompDevice_.Reset();
  d3dCtx_.Reset();
  d3dDevice_.Reset();

  head_ = 0;
  live_ = 0;
  spriteW_ = 0;
  spriteH_ = 0;
  hotX_ = 0;
  hotY_ = 0;
  hasCursorBitmap_ = false;
  initialized_ = false;
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

void OverlayRenderer::LogFailureOnce(const wchar_t* step, HRESULT hr) {
  if (loggedFailure_) return;
  loggedFailure_ = true;
  DiagLog(L"[OverlayRenderer] %ls failed: 0x%08X (further identical failures suppressed)", step,
          static_cast<unsigned>(hr));
}

bool OverlayRenderer::SetCursorBitmap(const CursorBitmap& bmp) {
  if (!initialized_ || !dcompDevice_ || !spriteSurface_) return false;
  if (bmp.width <= 0 || bmp.height <= 0) return false;

  CursorBitmap scaled;
  const CursorBitmap* src = &bmp;
  if (bmp.width != spriteW_ || bmp.height != spriteH_) {
    ScaleCursor(bmp, spriteW_, spriteH_, scaled);
    src = &scaled;
  }
  if (src->pixels.size() < static_cast<size_t>(src->width) * src->height * 4) return false;

  POINT offset{0, 0};
  Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurface;
  HRESULT hr = spriteSurface_->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
  if (FAILED(hr)) {
    LogHr(L"surface BeginDraw", hr);
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> dst;
  hr = dxgiSurface.As(&dst);
  if (SUCCEEDED(hr)) {
    const D3D11_BOX dstBox{
        static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0,
        static_cast<UINT>(offset.x + src->width), static_cast<UINT>(offset.y + src->height), 1};
    d3dCtx_->UpdateSubresource(dst.Get(), 0, &dstBox, src->pixels.data(),
                               static_cast<UINT>(src->width) * 4, 0);
  }
  const HRESULT endHr = spriteSurface_->EndDraw();
  if (FAILED(hr)) {
    LogHr(L"upload sprite pixels", hr);
    return false;
  }
  if (FAILED(endHr)) {
    LogHr(L"surface EndDraw", endHr);
    return false;
  }

  hotX_ = src->hotX;
  hotY_ = src->hotY;
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

    Slot& slot = slots_[static_cast<size_t>(idx)];
    // offset 用窗口客户区坐标（窗口左上角 = rect_ 左上角）。
    slot.visual->SetOffsetX(static_cast<float>(samples[i].x - rect_.left - hotX_));
    slot.visual->SetOffsetY(static_cast<float>(samples[i].y - rect_.top - hotY_));
    slot.bornQpc = born;
    dirty = true;
  }

  // ---- 过期：从队头弹出时间窗之外的采样点 ----
  if (HideExpired(now)) dirty = true;

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
  if (live_ == 0) head_ = 0;  // 停顿清空后从槽 0 重来，减少环形回绕的 Z 序反转
  return dirty;
}

void OverlayRenderer::HideVisibleSlots() {
  if (live_ == 0) return;
  const int n = static_cast<int>(slots_.size());
  for (int k = 0; k < live_; ++k) {
    Slot& slot = slots_[static_cast<size_t>((head_ + k) % n)];
    slot.visual->SetVisible(FALSE);
    slot.bornQpc = 0;
  }
  head_ = 0;
  live_ = 0;
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
