#pragma once
#include <windows.h>
#include <wrl/client.h>

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <vector>

#include "cursor_texture.h"
#include "mouse_sample.h"

// 一个覆盖主显示器的窗口 + 一棵 DirectComposition 视觉树。
//
// 尾迹不在像素里：每个采样点是一个 visual，全部共享同一张光标贴图 surface；运行期只改
// offset / visible，没有绘制、没有整屏拷贝。一个采样点"显示一帧然后消失"变成"把它设成
// 不可见"，于是擦除这个操作根本不存在。
//
// 更新由 WH_MOUSE_LL 回调驱动：每个鼠标事件调用一次 Update，没有 tick，也没有轮询。
// 采样点到期不能等下一次鼠标事件，调用方按 NextExpiryQpc() 设置一次性定时器，到点调用
// Expire() 隐藏过期槽位。
//
// 多显示器：只有一个窗口，只覆盖主显示器。按显示器拆窗口会让 DWM 的合成路径变复杂，
// 实测放视频时会拖慢整个桌面，因此其它显示器上不画。
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

  // 上一次 Commit 的耗时（QPC ticks；本次没有提交时为 0）。诊断 DWM 是否在反压。
  uint64_t lastCommitTicks() const { return lastCommitTicks_; }

  // 最早一个可见槽位的到期 QPC 时刻；没有可见槽位时返回 0。
  // 调用方按它 arm 一次性高分辨率定时器，而不是按固定 tick 轮询。
  uint64_t NextExpiryQpc() const;

  // 上传光标贴图（形状变化时调用），并按窗口 DPI 缩放到系统光标的像素尺寸。
  bool SetCursorBitmap(const CursorBitmap& bmp);

  // 处理一次鼠标事件新增的采样点：分配环形槽位、隐藏过期槽位。
  // samples 可为空（鼠标没动）。只有确有变化时才 Commit；*committed 返回是否提交。
  FrameResult Update(const Sample* samples, uint32_t count, bool* committed);

  // 隐藏所有已到期的槽位；有变化时 Commit。由到期定时器调用。
  FrameResult Expire(uint64_t now, bool* committed);

  // 立即隐藏所有视觉并提交（光标不可见 / 设备重建后）。
  void HideAll();

 private:
  struct Slot {
    Microsoft::WRL::ComPtr<IDCompositionVisual2> visual;
    Microsoft::WRL::ComPtr<IDCompositionVisual3> visual3;  // SetVisible（Windows 10+）
    uint64_t bornQpc = 0;
    bool visible = false;
  };

  FrameResult CommitNow(bool* committed);
  bool HideExpired(uint64_t now);
  void LogFailureOnce(const wchar_t* step, HRESULT hr);

  Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dCtx_;
  // 用 DCompositionCreateDevice3 创建：只有高版本设备创建出来的 visual 才会暴露
  // IDCompositionVisual3（基础版 DCompositionCreateDevice 只给得到 Visual 基接口）。
  Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> dcompDevice_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual2> root_;
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
  bool initialized_ = false;
  bool loggedFailure_ = false;
};
