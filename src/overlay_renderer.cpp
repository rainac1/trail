#include "overlay_renderer.h"

#include <cfloat>
#include <cstdio>
#include <vector>

#include <d2d1helper.h>

#include "diag.h"

namespace {
void LogHr(const wchar_t* step, HRESULT hr) {
  DiagLog(L"[OverlayRenderer] %ls failed: 0x%08X", step, static_cast<unsigned>(hr));
}

// 设备丢失类错误：D3D 设备被移除/复位/挂起、D2D 目标需要重建。这些错误下旧的
// 设备、swapchain 与 DComp 视觉树全部失效，只能整栈重建（见头文件说明）。
bool IsDeviceLost(HRESULT hr) {
  return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
         hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
         hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// 收集会话/合成器/GPU 诊断信息，帮助远程定位初始化失败原因。
void LogSystemDiagnostics(ID3D11Device* device) {
  BOOL dwm = FALSE;
  // 先 GetModuleHandleW（dwmapi 常驻时零开销）；未加载则临时 LoadLibraryW，
  // 用完 FreeLibrary，避免句柄泄漏。
  HMODULE dwmapi = GetModuleHandleW(L"dwmapi.dll");
  bool ownDwmapi = false;
  if (!dwmapi) {
    dwmapi = LoadLibraryW(L"dwmapi.dll");
    ownDwmapi = true;
  }
  if (dwmapi) {
    using DwmIsCompFn = HRESULT(WINAPI*)(BOOL*);
    auto fn = reinterpret_cast<DwmIsCompFn>(GetProcAddress(dwmapi, "DwmIsCompositionEnabled"));
    if (fn) fn(&dwm);
    if (ownDwmapi) FreeLibrary(dwmapi);
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
}  // namespace

bool OverlayRenderer::Initialize(HWND hwnd, int width, int height) {
  Shutdown();  // 可直接用于重建：先释放上一次创建的全部对象
  LogSystemDiagnostics(nullptr);

  HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
  if (FAILED(hr)) {
    LogHr(L"D2D1CreateFactory", hr);
    return false;
  }

  // 仅使用硬件加速设备（WARP 软件渲染不满足高性能目标）。
  const UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
  hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, nullptr, 0,
                         D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, nullptr);
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
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  hr = dxgiDevice->GetAdapter(&adapter);
  if (FAILED(hr)) {
    LogHr(L"GetAdapter", hr);
    return false;
  }
  hr = adapter->GetParent(IID_PPV_ARGS(&dxgiFactory_));
  if (FAILED(hr)) {
    LogHr(L"GetParent(IDXGIFactory2)", hr);
    return false;
  }

  hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
  if (FAILED(hr)) {
    LogHr(L"CreateDevice", hr);
    return false;
  }
  hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx_);
  if (FAILED(hr)) {
    LogHr(L"CreateDeviceContext", hr);
    return false;
  }

  // composition swapchain：premultiplied alpha 由 DWM 视觉树合成。
  // 注意：FLIP_DISCARD 每次 Present 轮换 backbuffer，因此每帧都必须重新
  // GetBuffer(0) 获取当前后缓冲，不能缓存纹理（见 RenderFrame）。
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  hr = dxgiFactory_->CreateSwapChainForComposition(d3dDevice_.Get(), &desc, nullptr, &swapChain_);
  if (FAILED(hr)) {
    LogHr(L"CreateSwapChainForComposition", hr);
    return false;
  }
  // 限制 DWM 合成队列深度为 1：避免极端情况下 Present(0) 提交的帧在合成队列中
  // 堆积，导致 DWM 显示的是较旧的帧。对 composition swapchain 是最低成本的保险
  // （不支持时静默忽略）。
  Microsoft::WRL::ComPtr<IDXGISwapChain2> sc2;
  if (SUCCEEDED(swapChain_.As(&sc2))) sc2->SetMaximumFrameLatency(1);

  // D2D 渲染目标：自建 premultiplied 离屏位图（不直接绑定 swapchain backbuffer，
  // 部分显示栈上 CreateBitmapFromDxgiSurface + flip backbuffer 会返回 E_INVALIDARG）。
  std::vector<uint8_t> zeros(static_cast<size_t>(width) * height * 4, 0);
  D2D1_BITMAP_PROPERTIES1 bp{};
  bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
  bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
  bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
  hr = ctx_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
                          zeros.data(), static_cast<UINT32>(width) * 4, bp, &offscreen_);
  if (FAILED(hr)) {
    LogHr(L"CreateBitmap (offscreen target)", hr);
    return false;
  }
  ctx_->SetTarget(offscreen_.Get());

  // DirectComposition：把 swapchain 挂到窗口的 DWM 视觉树。
  hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice_));
  if (FAILED(hr)) {
    LogHr(L"DCompositionCreateDevice", hr);
    return false;
  }
  hr = dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_);
  if (FAILED(hr)) {
    LogHr(L"CreateTargetForHwnd", hr);
    return false;
  }
  hr = dcompDevice_->CreateVisual(&dcompVisual_);
  if (FAILED(hr)) {
    LogHr(L"CreateVisual", hr);
    return false;
  }
  hr = dcompVisual_->SetContent(swapChain_.Get());
  if (FAILED(hr)) {
    LogHr(L"SetContent", hr);
    return false;
  }
  hr = dcompTarget_->SetRoot(dcompVisual_.Get());
  if (FAILED(hr)) {
    LogHr(L"SetRoot", hr);
    return false;
  }
  hr = dcompDevice_->Commit();
  if (FAILED(hr)) {
    LogHr(L"Commit", hr);
    return false;
  }

  lastFrameBox_ = D2D1_RECT_F{};
  hasLastFrameBox_ = false;
  loggedFailure_ = false;
  initialized_ = true;

  DiagLog(L"[OverlayRenderer] initialized: %d x %d, DirectComposition + offscreen blit", width,
          height);
  return true;
}

void OverlayRenderer::Shutdown() {
  // 释放顺序：先解绑渲染目标与视觉树内容，再自内向外销毁（内容 -> 目标 ->
  // DComp 设备 -> swapchain -> D2D -> DXGI/D3D），避免 DWM 仍引用已释放的交换链。
  if (ctx_) {
    ctx_->SetTarget(nullptr);
    ctx_->Flush();
  }
  offscreen_.Reset();
  if (dcompVisual_) dcompVisual_->SetContent(nullptr);
  if (dcompTarget_) dcompTarget_->SetRoot(nullptr);
  if (dcompDevice_) dcompDevice_->Commit();  // 尽力而为：失败无需处理
  dcompVisual_.Reset();
  dcompTarget_.Reset();
  dcompDevice_.Reset();
  swapChain_.Reset();
  d2dDevice_.Reset();
  ctx_.Reset();
  d2dFactory_.Reset();
  dxgiFactory_.Reset();
  d3dCtx_.Reset();
  d3dDevice_.Reset();

  hasLastFrameBox_ = false;
  initialized_ = false;
}

void OverlayRenderer::LogFailureOnce(const wchar_t* step, HRESULT hr) {
  if (loggedFailure_) return;
  loggedFailure_ = true;
  DiagLog(L"[OverlayRenderer] %ls failed: 0x%08X (further identical failures suppressed)", step,
          static_cast<unsigned>(hr));
}

bool OverlayRenderer::DeviceValid() {
  if (!initialized_ || !dcompDevice_) return false;
  BOOL valid = FALSE;
  const HRESULT hr = dcompDevice_->CheckDeviceState(&valid);
  if (FAILED(hr) || !valid) {
    DiagLog(L"[OverlayRenderer] CheckDeviceState: invalid (hr=0x%08X, valid=%d)",
            static_cast<unsigned>(hr), static_cast<int>(valid));
    return false;
  }
  return true;
}

OverlayRenderer::FrameResult OverlayRenderer::RenderFrame(
    ID2D1Bitmap* cursorBmp, int texW, int texH, int hotX, int hotY, const Sample* samples,
    uint32_t count, int originX, int originY, bool drawLiveHead) {
  if (!initialized_ || !ctx_) return FrameResult::RecreateDevice;

  const D2D1_RECT_F src{0.0f, 0.0f, static_cast<FLOAT>(texW), static_cast<FLOAT>(texH)};
  // 屏幕坐标 -> 窗口客户区坐标（窗口左上角 = 虚拟屏幕原点）。
  const auto dstFor = [&](int x, int y) {
    const FLOAT left = static_cast<FLOAT>(x - hotX - originX);
    const FLOAT top = static_cast<FLOAT>(y - hotY - originY);
    return D2D1_RECT_F{left, top, left + static_cast<FLOAT>(texW), top + static_cast<FLOAT>(texH)};
  };

  // 本帧绘制内容 bbox（用于下一帧脏矩形清除）。
  D2D1_RECT_F curBox{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
  const auto extend = [&](const D2D1_RECT_F& r) {
    if (r.left < curBox.left) curBox.left = r.left;
    if (r.top < curBox.top) curBox.top = r.top;
    if (r.right > curBox.right) curBox.right = r.right;
    if (r.bottom > curBox.bottom) curBox.bottom = r.bottom;
  };

  // ---- pass 1：清除上一帧残留 + 绘制历史尾迹 ----
  ctx_->BeginDraw();
  if (hasLastFrameBox_) {
    // 只清除上一帧绘制内容覆盖的区域，替代全屏 Clear（高分辨率下大幅降低开销）。
    ctx_->PushAxisAlignedClip(lastFrameBox_, D2D1_ANTIALIAS_MODE_ALIASED);
    ctx_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    ctx_->PopAxisAlignedClip();
  }
  if (cursorBmp) {
    for (uint32_t i = 0; i < count; ++i) {
      const D2D1_RECT_F dst = dstFor(samples[i].x, samples[i].y);
      // NEAREST_NEIGHBOR：光标像素 1:1 锐利，且是开销最低的插值模式。
      ctx_->DrawBitmap(cursorBmp, dst, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src);
      extend(dst);
    }
  }
  HRESULT hr = ctx_->EndDraw();
  if (IsDeviceLost(hr)) {
    LogHr(L"EndDraw (device lost)", hr);
    return FrameResult::RecreateDevice;
  }
  if (FAILED(hr)) {
    LogFailureOnce(L"EndDraw", hr);
    return FrameResult::Failed;
  }

  // ---- pass 2：实时头部点（提交前最后一刻采样，最小化头部延迟）----
  // 用 GetCursorInfo（替代 GetCursorPos）在历史点 EndDraw 之后、CopyResource 之前
  // 采样当前光标位置并绘制，把头部采样推迟到提交前最后一刻。
  if (drawLiveHead && cursorBmp) {
    CURSORINFO ci{sizeof(ci)};
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
      const D2D1_RECT_F dst = dstFor(ci.ptScreenPos.x, ci.ptScreenPos.y);
      ctx_->BeginDraw();
      ctx_->DrawBitmap(cursorBmp, dst, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src);
      hr = ctx_->EndDraw();
      if (IsDeviceLost(hr)) {
        LogHr(L"EndDraw (live head, device lost)", hr);
        return FrameResult::RecreateDevice;
      }
      if (FAILED(hr)) {
        LogFailureOnce(L"EndDraw (live head)", hr);
        return FrameResult::Failed;
      }
      extend(dst);
    }
  }

  // 记录本帧 bbox 供下一帧清除（本帧无内容则下一帧无需清除）。
  if (curBox.left <= curBox.right && curBox.top <= curBox.bottom) {
    lastFrameBox_ = curBox;
    hasLastFrameBox_ = true;
  } else {
    hasLastFrameBox_ = false;
  }

  // 离屏渲染结果 -> 当前 swapchain backbuffer（GPU 显存拷贝），再 Present。
  // FLIP_DISCARD 每帧轮换 buffer，故必须每帧重新 GetBuffer(0)。
  if (d3dCtx_ && offscreen_) {
    Microsoft::WRL::ComPtr<IDXGISurface> backSurface;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backTex;
    Microsoft::WRL::ComPtr<IDXGISurface> offSurface;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> offTex;
    HRESULT cr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backSurface));
    if (SUCCEEDED(cr)) cr = backSurface.As(&backTex);
    if (SUCCEEDED(cr)) cr = offscreen_->GetSurface(&offSurface);
    if (SUCCEEDED(cr)) cr = offSurface.As(&offTex);
    if (SUCCEEDED(cr)) {
      d3dCtx_->CopyResource(backTex.Get(), offTex.Get());
    } else {
      // 拷贝路径失败时才向驱动查询设备移除原因（成功路径不做任何额外调用，热路径
      // 每帧开销保持不变）；设备确实被移除则升级为整栈重建。
      if (IsDeviceLost(cr) || IsDeviceLost(d3dDevice_->GetDeviceRemovedReason())) {
        LogHr(L"copy offscreen -> backbuffer (device lost)", cr);
        return FrameResult::RecreateDevice;
      }
      // 拷贝失败：本帧 backbuffer 内容是旧的（画面可能短暂停滞），设备仍可用，
      // 不升级为重建，仅记录一次。
      LogFailureOnce(L"copy offscreen -> backbuffer", cr);
      return FrameResult::Failed;
    }
  }

  // Present(0)：不等待 vsync，提交时机由调用方按 DWM 合成时钟对齐（见 main.cpp），
  // 使帧赶在当前这次合成之前交上去，而不是由 DWM 等到下一次 vsync。
  const HRESULT pr = swapChain_->Present(0, 0);
  if (IsDeviceLost(pr)) {
    LogHr(L"Present (device lost)", pr);
    return FrameResult::RecreateDevice;
  }
  if (FAILED(pr)) {  // DXGI_STATUS_OCCLUDED 是成功码，不在此列
    LogFailureOnce(L"Present", pr);
    return FrameResult::Failed;
  }

  loggedFailure_ = false;  // 连续多帧成功后重新武装失败日志
  return FrameResult::Ok;
}
