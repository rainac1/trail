#pragma once
#include <windows.h>
#include <wrl/client.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>  // IDXGISwapChain2（SetMaximumFrameLatency）：MSVC 在 dxgi1_2.h 中
                      // 声明，MinGW 放在 dxgi1_3.h；显式包含以兼容两套工具链。

#include "mouse_history.h"

// Direct2D + DirectComposition 全屏透明叠加渲染器（硬件 GPU，无软件降级）。
//
// 呈现路径：
//   1. D2D 渲染到自建 premultiplied 离屏位图（CreateBitmap + D2D1_BITMAP_OPTIONS_TARGET）。
//      不能直接绑定 flip-model swapchain backbuffer：部分显示栈上
//      CreateBitmapFromDxgiSurface + flip backbuffer 会返回 E_INVALIDARG。
//      离屏位图帧间内容保留，故每帧只清除上一帧绘制内容的 bbox（脏矩形清除），
//      而非全屏 Clear，以降低高分辨率下的 GPU 开销。
//   2. 每帧通过 GPU CopyResource 把离屏纹理拷贝到 composition swapchain backbuffer
//      （显存内拷贝，硬件加速）。
//   3. IDCompositionVisual::SetContent(swapchain) 由 DWM 按 premultiplied alpha 合成；
//      每帧 Present(0) 不等待 vsync，提交时机由调用方按 DWM 合成时钟对齐（无降级路径）。
//
// 选择 DirectComposition 而非 flip+Hwnd 的原因：部分显示栈对 CreateSwapChainForHwnd
// + DXGI_ALPHA_MODE_PREMULTIPLIED 返回 DXGI_ERROR_INVALID_CALL，而
// CreateSwapChainForComposition + 离屏拷贝在同样条件下可用，是更可靠的硬件透明路径。
//
// 设备丢失（DXGI_ERROR_DEVICE_REMOVED / D2DERR_RECREATE_TARGET 等）：本窗口没有
// 任何 GDI 内容，可见像素 100% 来自 DWM 合成的 DComp 视觉树，因此设备一旦丢失，
// 叠加层会整体变透明（"渲染突然消失"），且不会自愈。RenderFrame/DeviceValid 会把
// 这种情况报为 FrameResult::RecreateDevice / false，调用方必须重建整套设备与内容
// （官方 CheckDeviceState 文档要求：新的 DXGI + DirectComposition 设备，内容全部重建）。
class OverlayRenderer {
 public:
  // 单帧结果。
  enum class FrameResult {
    Ok,              // 正常
    Failed,          // 普通失败（本帧内容可能不完整，但设备仍可用，可继续渲染）
    RecreateDevice,  // 设备丢失：必须重建整套设备与内容才能恢复显示
  };

  bool Initialize(HWND hwnd, int width, int height);
  void Shutdown();

  // 绘制一帧：把 cursorBmp 绘制到每个采样点位置（热点对齐），然后离屏 ->
  // swapchain 拷贝并 Present(0)（不等待 vsync）。samples 为历史尾迹点（按时间升序）。
  // drawLiveHead 为 true 时，在历史点渲染完成、EndDraw 之后（CopyResource 之前）用
  // GetCursorInfo 最后一刻采样当前光标位置并单独绘制头部点 —— 替代 GetCursorPos，且
  // 把头部采样推迟到提交前最后一刻以压缩头部延迟。
  // 提交时机由调用方按 DWM 合成时钟对齐（见 main.cpp）；本函数不做任何等待或降级。
  FrameResult RenderFrame(ID2D1Bitmap* cursorBmp, int texW, int texH, int hotX, int hotY,
                          const Sample* samples, uint32_t count, int originX, int originY,
                          bool drawLiveHead);

  // DirectComposition 设备是否仍然有效。DirectComposition 在设备丢失时会向合成
  // 其内容的窗口发送 WM_PAINT，应用应在 WM_PAINT 中调用本函数确认设备状态；
  // 返回 false 表示必须重建（见类注释）。
  bool DeviceValid();

  ID2D1DeviceContext* Context() const { return ctx_.Get(); }

  // 初始化是否全部完成（未初始化或已 Shutdown 时为 false）。设备不可用时调用方
  // 不得渲染：Context() 为 null，光标纹理抓取会解引用空指针。
  bool ready() const { return initialized_; }

 private:
  // 非设备丢失类的失败只记一次日志（否则会逐帧刷屏），成功一帧后重新武装。
  void LogFailureOnce(const wchar_t* step, HRESULT hr);

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

  bool initialized_ = false;
  bool loggedFailure_ = false;

  // 脏矩形清除状态：offscreen_ 为 D2D1_BITMAP_OPTIONS_TARGET，帧间内容保留，
  // 故每帧只需清除上一帧绘制内容覆盖的区域（bbox），无需全屏 Clear。
  D2D1_RECT_F lastFrameBox_{};  // 上一帧绘制内容（历史点 + 头部点）的 bbox
  bool hasLastFrameBox_ = false;
};
