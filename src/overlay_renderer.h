#pragma once
#include <windows.h>
#include <wrl/client.h>

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>

#include <vector>

#include "cursor_texture.h"
#include "mouse_history.h"

// 一个覆盖整个虚拟桌面的窗口 + 一棵 DirectComposition 视觉树。
//
// 尾迹不在像素里：每个采样点是一个 visual，全部共享同一张光标贴图 surface；运行期只改
// offset / visible，没有绘制、没有拷贝、没有 Present。一个采样点"显示一帧然后消失"变成
// "把它设成不可见"，于是擦除这个操作根本不存在。
//
// 多显示器：只有一个窗口，覆盖整个虚拟桌面（虚拟屏幕坐标 = 物理像素）。混合 DPI 时窗口
// 会被 DWM 按它的 DPI 上下文位图缩放，非该 DPI 的显示器上尾迹可能偏移/发虚 —— 这是刻意
// 接受的取舍：按显示器拆窗口会让 DWM 的合成路径变复杂，实测会拖慢全桌面。
//
// 光标贴图尺寸：GetCursorInfo/CopyIcon 给的是原生位图，系统按 DPI 缩放绘制，所以这里把
// 它缩放到窗口 DPI 对应的光标像素尺寸（SM_CXCURSOR 随 DPI 变化），避免高 DPI 下尾迹比
// 真实光标小一圈。
//
// 设备丢失：本窗口没有 GDI 内容，可见像素 100% 来自 DWM 合成的视觉树，设备一旦丢失叠加层
// 会整体变透明且不会自愈 —— 调用方必须重建整套设备与内容（RecreateDevice / DeviceValid）。
class OverlayRenderer {
 public:
  enum class FrameResult {
    Ok,              // 正常（本次调用可能没有任何变化，也就没有 Commit）
    Failed,          // 普通失败（本次状态没提交完整，设备仍可用）
    RecreateDevice,  // 设备丢失：调用方必须重建整套设备与内容
  };

  // windowRect：窗口在虚拟屏幕坐标里的矩形（物理像素）；dpi：窗口的 DPI。
  bool Initialize(HWND hwnd, const RECT& windowRect, UINT dpi, int maxSprites);
  void Shutdown();

  bool ready() const { return initialized_; }

  // 设备是否仍然有效。DirectComposition 在设备丢失时会向合成其内容的窗口发送 WM_PAINT。
  bool DeviceValid();

  // 尾迹寿命（毫秒）：采样点从可见到隐藏的时间窗。
  void SetTrailLifetimeMs(double ms);

  // 当前可见的采样点视觉数量（诊断用）。
  int liveSprites() const { return live_; }

  // 现在有没有任何可见内容（头部可见或尾迹点存活）。调用方用它决定要不要显示窗口：
  // 没有内容的叠加层窗口会挡住下层全屏视频的独立翻转/硬件平面提升。
  bool visible() const { return headVisible_ || live_ > 0; }

  // 上一次 Commit 的耗时（QPC ticks；本次没有提交时为 0）。诊断 DWM 是否在反压。
  uint64_t lastCommitTicks() const { return lastCommitTicks_; }

  // 上传光标贴图（形状变化时调用），并按窗口 DPI 缩放到系统光标的像素尺寸。
  bool SetCursorBitmap(const CursorBitmap& bmp);

  // 把本 tick 新增的采样点分配到环形槽位、隐藏过期槽位、更新头部位置。
  // samples 可为空（鼠标没动）。只有确有变化时才 Commit；*committed 返回是否提交。
  FrameResult Update(const Sample* samples, uint32_t count, int headX, int headY, bool showHead,
                     bool* committed);

  // 立即隐藏所有视觉并提交（光标不可见 / 设备重建后）。
  void HideAll();

 private:
  struct Slot {
    Microsoft::WRL::ComPtr<IDCompositionVisual2> visual;
    Microsoft::WRL::ComPtr<IDCompositionVisual3> visual3;  // SetVisible（Windows 10+）
    uint64_t bornQpc = 0;
    bool visible = false;
  };

  void LogFailureOnce(const wchar_t* step, HRESULT hr);

  Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dCtx_;
  // 用 DCompositionCreateDevice3 创建：只有高版本设备创建出来的 visual 才会暴露
  // IDCompositionVisual3（基础版 DCompositionCreateDevice 只给得到 Visual 基接口）。
  Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> dcompDevice_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual2> root_;
  Microsoft::WRL::ComPtr<IDCompositionVisual2> head_;
  Microsoft::WRL::ComPtr<IDCompositionVisual3> head3_;
  Microsoft::WRL::ComPtr<IDCompositionSurface> spriteSurface_;
  std::vector<Slot> slots_;  // 环形槽位：新采样点覆盖最老的槽位

  RECT rect_{};  // 窗口矩形（虚拟屏幕坐标）
  UINT dpi_ = 96;
  uint64_t qpcFreq_ = 0;
  uint64_t trailTicks_ = 0;
  uint64_t lastCommitTicks_ = 0;
  int nextSlot_ = 0;
  int live_ = 0;
  int spriteW_ = 0;
  int spriteH_ = 0;
  int hotX_ = 0;
  int hotY_ = 0;
  int lastHeadX_ = 0;
  int lastHeadY_ = 0;
  bool headVisible_ = false;
  bool initialized_ = false;
  bool loggedFailure_ = false;
};
