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

// 收集会话/合成器/GPU 诊断信息，帮助远程定位初始化失败原因。
void LogSystemDiagnostics(ID3D11Device* device) {
  BOOL dwm = FALSE;
  HMODULE dwmapi = GetModuleHandleW(L"dwmapi.dll");
  if (dwmapi) {
    using DwmIsCompFn = HRESULT(WINAPI*)(BOOL*);
    auto fn = reinterpret_cast<DwmIsCompFn>(GetProcAddress(dwmapi, "DwmIsCompositionEnabled"));
    if (fn) fn(&dwm);
  }
  DiagLog(L"[diag] DWM composition %ls", dwm ? L"enabled" : L"DISABLED");

  if (device) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
      DXGI_ADAPTER_DESC desc{};
      if (SUCCEEDED(adapter->GetDesc(&desc))) {
        DiagLog(L"[diag] adapter: %ls (vendor 0x%04X, device 0x%04X, VRAM %llu MB)",
                desc.Description, desc.VendorId, desc.DeviceId,
                static_cast<unsigned long long>(desc.DedicatedVideoMemory) / (1024 * 1024));
      }
    }
  }
}

// 系统光标在给定 DPI 下的像素尺寸（物理像素）。系统按 DPI 缩放绘制光标，但
// GetCursorInfo/CopyIcon 给的是原生位图，所以这就是我们要缩放到的目标尺寸。
int CursorMetricForDpi(int index, UINT dpi) {
  using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
  static const auto fn = reinterpret_cast<GetSystemMetricsForDpiFn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
  if (fn) return fn(index, dpi);
  return MulDiv(GetSystemMetrics(index), static_cast<int>(dpi), 96);
}

uint8_t ClampByte(double v) {
  if (v <= 0.0) return 0;
  if (v >= 255.0) return 255;
  return static_cast<uint8_t>(v + 0.5);
}

// 把光标位图缩放到指定像素尺寸。像素是预乘 alpha，所以对四个通道直接做面积平均即可
// （放大时它退化为双线性插值）。热点按同一比例缩放。
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

bool OverlayRenderer::Initialize(HWND hwnd, const RECT& windowRect, UINT dpi, int maxSprites) {
  Shutdown();  // 可直接用于重建：先释放上一次创建的全部对象

  if (!hwnd || maxSprites < 1) return false;
  rect_ = windowRect;
  dpi_ = dpi ? dpi : 96;

  LARGE_INTEGER freq{};
  if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
    DiagLog(L"[OverlayRenderer] QueryPerformanceFrequency failed");
    return false;
  }
  qpcFreq_ = static_cast<uint64_t>(freq.QuadPart);

  // 仅使用硬件加速设备（WARP 软件渲染不满足延迟目标）。
  const UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, nullptr,
                                 0, D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, nullptr);
  if (FAILED(hr)) {
    LogHr(L"D3D11 hardware device", hr);
    return false;
  }
  LogSystemDiagnostics(d3dDevice_.Get());
  DiagLog(L"[diag] D3D feature level: 0x%04X", static_cast<unsigned>(featureLevel));
  d3dDevice_->GetImmediateContext(&d3dCtx_);

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  hr = d3dDevice_.As(&dxgiDevice);
  if (FAILED(hr)) {
    LogHr(L"device->IDXGIDevice QI", hr);
    return false;
  }

  // 必须用 DCompositionCreateDevice3：基础版 DCompositionCreateDevice 创建出来的
  // visual 只暴露 Visual 基接口，QI 不到 IDCompositionVisual3（SetVisible）。
  hr = DCompositionCreateDevice3(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice_));
  if (FAILED(hr)) {
    LogHr(L"DCompositionCreateDevice3", hr);
    return false;
  }
  hr = dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, &target_);
  if (FAILED(hr)) {
    LogHr(L"CreateTargetForHwnd", hr);
    return false;
  }
  hr = dcompDevice_->CreateVisual(&root_);
  if (FAILED(hr)) {
    LogHr(L"CreateVisual(root)", hr);
    return false;
  }
  hr = target_->SetRoot(root_.Get());
  if (FAILED(hr)) {
    LogHr(L"SetRoot", hr);
    return false;
  }

  // 环形槽位：每个槽位一个 visual，初始不可见。新采样点覆盖最老的槽位，
  // 因此槽位数就是"同时可见的采样点上限"。
  //
  // 初始树序（零成本，只在初始化时排一次）：AddVisual 在 referenceVisual 为 NULL 时
  // 语义是反的 —— insertAbove=TRUE 表示"插到最底层"，FALSE 表示"插到最上层"。这里用
  // FALSE，让后加入的槽位在上层；环形槽位按下标递增分配新点，于是"活着的连续一段槽位"
  // 里下标越大 = 越新 = 越靠上，深度顺序在绝大多数时刻是对的。
  slots_.clear();
  slots_.reserve(static_cast<size_t>(maxSprites));
  for (int i = 0; i < maxSprites; ++i) {
    Slot slot;
    hr = dcompDevice_->CreateVisual(&slot.visual);
    if (FAILED(hr)) {
      LogHr(L"CreateVisual(sprite)", hr);
      return false;
    }
    hr = slot.visual.As(&slot.visual3);
    if (FAILED(hr)) {
      LogHr(L"visual->IDCompositionVisual3 QI (Windows 10+ required)", hr);
      return false;
    }
    slot.visual3->SetVisible(FALSE);
    hr = root_->AddVisual(slot.visual.Get(), FALSE, nullptr);
    if (FAILED(hr)) {
      LogHr(L"AddVisual(sprite)", hr);
      return false;
    }
    slots_.push_back(std::move(slot));
  }

  // 头部视觉最后加入 => 位于所有尾迹点之上。
  hr = dcompDevice_->CreateVisual(&head_);
  if (FAILED(hr)) {
    LogHr(L"CreateVisual(head)", hr);
    return false;
  }
  hr = head_.As(&head3_);
  if (FAILED(hr)) {
    LogHr(L"head->IDCompositionVisual3 QI (Windows 10+ required)", hr);
    return false;
  }
  head3_->SetVisible(FALSE);
  hr = root_->AddVisual(head_.Get(), FALSE, nullptr);
  if (FAILED(hr)) {
    LogHr(L"AddVisual(head)", hr);
    return false;
  }

  hr = dcompDevice_->Commit();
  if (FAILED(hr)) {
    LogHr(L"Commit (init)", hr);
    return false;
  }

  nextSlot_ = 0;
  live_ = 0;
  headVisible_ = false;
  loggedFailure_ = false;
  initialized_ = true;
  DiagLog(L"[OverlayRenderer] initialized: %d sprite visuals + head, DirectComposition visual tree",
          maxSprites);
  return true;
}

void OverlayRenderer::Shutdown() {
  // 先解绑视觉树与根，再释放（视觉 -> 根 -> 目标 -> surface -> DComp 设备 -> D3D），
  // 避免 DWM 仍引用已释放的内容。
  if (target_) target_->SetRoot(nullptr);
  slots_.clear();
  head3_.Reset();
  head_.Reset();
  root_.Reset();
  target_.Reset();
  spriteSurface_.Reset();
  dcompDevice_.Reset();
  d3dCtx_.Reset();
  d3dDevice_.Reset();

  live_ = 0;
  nextSlot_ = 0;
  headVisible_ = false;
  spriteW_ = 0;
  spriteH_ = 0;
  hotX_ = 0;
  hotY_ = 0;
  initialized_ = false;
}

bool OverlayRenderer::DeviceValid() {
  // 用 D3D 设备的移除原因判断：DCompositionCreateDevice3 返回的
  // IDCompositionDesktopDevice 没有 CheckDeviceState（那在基础版 IDCompositionDevice 上），
  // 而 DComp 设备本身由这个 D3D 设备支撑，D3D 被移除/复位时 DComp 内容也一并失效。
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
  if (!initialized_ || !dcompDevice_) return false;
  if (bmp.width <= 0 || bmp.height <= 0) return false;

  // 缩放到窗口 DPI 对应的系统光标像素尺寸（系统按 DPI 缩放绘制光标，原生位图偏小）。
  const int targetW = CursorMetricForDpi(SM_CXCURSOR, dpi_);
  const int targetH = CursorMetricForDpi(SM_CYCURSOR, dpi_);
  if (targetW <= 0 || targetH <= 0) return false;
  CursorBitmap scaled;
  ScaleCursor(bmp, targetW, targetH, scaled);

  Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
  HRESULT hr = dcompDevice_->CreateSurface(static_cast<UINT>(scaled.width),
                                           static_cast<UINT>(scaled.height),
                                           DXGI_FORMAT_B8G8R8A8_UNORM,
                                           DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
  if (FAILED(hr)) {
    LogHr(L"CreateSurface", hr);
    return false;
  }

  POINT offset{0, 0};
  Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurface;
  hr = surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
  if (FAILED(hr)) {
    LogHr(L"surface BeginDraw", hr);
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> dst;
  hr = dxgiSurface.As(&dst);
  if (SUCCEEDED(hr)) {
    // 先把像素上传到一张普通纹理，再用 CopySubresourceRegion 写进 surface
    // （surface 的更新区可能带 offset，UpdateSubresource 无法写子矩形）。
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(scaled.width);
    td.Height = static_cast<UINT>(scaled.height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> src;
    hr = d3dDevice_->CreateTexture2D(&td, nullptr, &src);
    if (SUCCEEDED(hr)) {
      d3dCtx_->UpdateSubresource(src.Get(), 0, nullptr, scaled.pixels.data(),
                                 static_cast<UINT>(scaled.width) * 4, 0);
      const D3D11_BOX box{0, 0, 0, static_cast<UINT>(scaled.width),
                          static_cast<UINT>(scaled.height), 1};
      d3dCtx_->CopySubresourceRegion(dst.Get(), 0, static_cast<UINT>(offset.x),
                                     static_cast<UINT>(offset.y), 0, src.Get(), 0, &box);
    }
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

  spriteSurface_ = surface;
  spriteW_ = scaled.width;
  spriteH_ = scaled.height;
  hotX_ = scaled.hotX;
  hotY_ = scaled.hotY;

  // 把新内容挂到所有视觉上，并让他们不可见（位置在下次 Update 时才确定）。
  for (auto& slot : slots_) {
    slot.visual->SetContent(spriteSurface_.Get());
    slot.visual3->SetVisible(FALSE);
    slot.visible = false;
    slot.bornQpc = 0;
  }
  head_->SetContent(spriteSurface_.Get());
  head3_->SetVisible(FALSE);
  headVisible_ = false;
  live_ = 0;
  nextSlot_ = 0;
  lastHeadX_ = 0;
  lastHeadY_ = 0;

  hr = dcompDevice_->Commit();
  if (FAILED(hr)) {
    LogHr(L"Commit (sprite)", hr);
    return false;
  }
  return true;
}

OverlayRenderer::FrameResult OverlayRenderer::Update(const Sample* samples, uint32_t count,
                                                     int headX, int headY, bool showHead,
                                                     bool* committed) {
  if (committed) *committed = false;
  if (!initialized_) return FrameResult::RecreateDevice;

  // 还没有可用的光标贴图：保证不残留任何可见内容。
  if (!spriteSurface_ || slots_.empty()) {
    HideAll();
    return FrameResult::Ok;
  }

  const uint64_t now = QpcNow();
  bool dirty = false;
  lastCommitTicks_ = 0;

  // ---- 新采样点：占用环形槽位。新点覆盖最老的槽位，所以槽位数就是可见点上限。----
  for (uint32_t i = 0; i < count; ++i) {
    // 只服务窗口覆盖的那块显示器：别的显示器上的采样点直接丢弃（否则它们会占着槽位，
    // 让主屏上能看见的点变少）。
    if (samples[i].x < rect_.left || samples[i].x >= rect_.right || samples[i].y < rect_.top ||
        samples[i].y >= rect_.bottom) {
      continue;
    }
    Slot& slot = slots_[static_cast<size_t>(nextSlot_)];
    nextSlot_ = (nextSlot_ + 1) % static_cast<int>(slots_.size());
    // offset 用窗口客户区坐标（窗口左上角 = 虚拟屏幕原点）。
    slot.visual->SetOffsetX(static_cast<float>(samples[i].x - rect_.left - hotX_));
    slot.visual->SetOffsetY(static_cast<float>(samples[i].y - rect_.top - hotY_));
    if (!slot.visible) {
      slot.visible = true;
      slot.visual3->SetVisible(TRUE);
      ++live_;
    }
    slot.bornQpc = now;
    dirty = true;
  }

  // ---- 过期：时间窗之外的采样点隐藏。这就是"擦除"，它只是一次 SetVisible(FALSE)。----
  if (trailTicks_ > 0 && live_ > 0) {
    for (auto& slot : slots_) {
      if (slot.visible && (now - slot.bornQpc) > trailTicks_) {
        slot.visible = false;
        --live_;
        slot.visual3->SetVisible(FALSE);
        dirty = true;
      }
    }
  }

  // ---- 头部：只在 --hide-cursor 模式下显示（系统光标可见时它被硬件光标完全遮住）。----
  const bool headInside = showHead && headX >= rect_.left && headX < rect_.right &&
                          headY >= rect_.top && headY < rect_.bottom;
  if (headInside) {
    if (!headVisible_ || headX != lastHeadX_ || headY != lastHeadY_) {
      head_->SetOffsetX(static_cast<float>(headX - rect_.left - hotX_));
      head_->SetOffsetY(static_cast<float>(headY - rect_.top - hotY_));
      lastHeadX_ = headX;
      lastHeadY_ = headY;
      if (!headVisible_) {
        headVisible_ = true;
        head3_->SetVisible(TRUE);
      }
      dirty = true;
    }
  } else if (headVisible_) {
    headVisible_ = false;
    head3_->SetVisible(FALSE);
    dirty = true;
  }

  if (!dirty) return FrameResult::Ok;  // 静止且无过期：一个 COM 调用都不做

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

void OverlayRenderer::HideAll() {
  if (!initialized_ || !dcompDevice_) return;
  bool dirty = false;
  if (headVisible_) {
    headVisible_ = false;
    head3_->SetVisible(FALSE);
    dirty = true;
  }
  for (auto& slot : slots_) {
    if (slot.visible) {
      slot.visible = false;
      slot.visual3->SetVisible(FALSE);
      dirty = true;
    }
  }
  live_ = 0;
  nextSlot_ = 0;
  if (dirty) dcompDevice_->Commit();  // 尽力而为：失败由看门狗/下次 Update 兜住
}
