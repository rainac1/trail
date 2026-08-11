#pragma once
#include <windows.h>
#include <wrl/client.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>

#include "cursor_sampler.h"

// Direct2D + DirectComposition 全屏透明叠加渲染器（硬件 GPU，无软件降级）。
//
// 呈现路径：
//   1. D2D 渲染到自建 premultiplied 离屏位图（CreateBitmap + D2D1_BITMAP_OPTIONS_TARGET）。
//      不能直接绑定 flip-model swapchain backbuffer：部分显示栈（AMD 核显等）对
//      CreateBitmapFromDxgiSurface + flip backbuffer 一律返回 E_INVALIDARG。
//   2. 每帧通过 GPU CopyResource 把离屏纹理拷贝到 composition swapchain backbuffer
//      （显存内拷贝，硬件加速）。
//   3. IDCompositionVisual::SetContent(swapchain) 由 DWM 按 premultiplied alpha 合成，
//      Present(1,0) vsync 节流。
//
// 选择 DirectComposition 而非 flip+Hwnd 的原因：此类显示栈对 CreateSwapChainForHwnd
// + DXGI_ALPHA_MODE_PREMULTIPLIED 返回 DXGI_ERROR_INVALID_CALL，而
// CreateSwapChainForComposition 完整支持 premultiplied alpha。
class OverlayRenderer {
 public:
  bool Initialize(HWND hwnd, int width, int height);
  void Shutdown();

  // 绘制一帧：清屏为全透明，把 cursorBmp 绘制到每个采样点位置（热点对齐），
  // 然后离屏 -> swapchain 拷贝并 Present。samples 按时间升序，最新点最后绘制。
  // waitForVBlank=false 时 Present(0) 不等待 vsync（由调用方做 vblank 前对齐以
  // 缩短显示延迟）；true 时 Present(1,0) 阻塞等 vsync。
  bool RenderFrame(ID2D1Bitmap* cursorBmp, int texW, int texH, int hotX, int hotY,
                   const Sample* samples, uint32_t count, int originX, int originY,
                   bool waitForVBlank);

  ID2D1DeviceContext* Context() const { return ctx_.Get(); }

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dCtx_;
  Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
  Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
  Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> offscreen_;  // D2D 渲染目标（premultiplied）
  Microsoft::WRL::ComPtr<IDCompositionDevice> dcompDevice_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> dcompVisual_;
};
