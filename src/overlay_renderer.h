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

// 一个覆盖当前活动显示器的窗口 + 一棵 DirectComposition 视觉树。
//
// 尾迹不在像素里：每个采样点是一个 visual，全部共享同一张光标贴图 surface；运行期只改
// offset / visible，没有绘制、没有整屏拷贝。一个采样点"显示一帧然后消失"变成"把它设成
// 不可见"，于是擦除这个操作根本不存在。
//
// 槽位按 FIFO 环形队列管理：新采样点按单调递增的时间戳写入队尾，满时覆盖队头；可见槽位
// 始终是连续区间 [head_, head_ + live_) mod N，因此到期查询 O(1)、过期回收 O(expired)。
//
// 多屏与多 DPI：始终只维护一个叠加层窗口并跟随光标所在显示器移动；当目标显示器 DPI 对应的
// 光标像素尺寸变化时，仅原位重建共享 surface 并重绑视觉，无需重建 D3D/DComp 设备。
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
  bool Initialize(HWND hwnd, const RECT& windowRect, UINT dpi, int maxSprites, uint64_t qpcFreq);
  void Shutdown();

  bool ready() const { return initialized_; }

  // 设备是否仍然有效。DirectComposition 在设备丢失时会向合成其内容的窗口发送 WM_PAINT。
  bool DeviceValid();

  // 尾迹寿命（毫秒）：采样点从可见到隐藏的时间窗。
  void SetTrailLifetimeMs(double ms);

  // 切换叠加层所在显示器的几何、DPI 与尾迹寿命（隐藏旧尾迹；若目标 DPI 导致光标像素尺寸变化，
  // 仅原位重建共享 surface 并重绑视觉，同时将 *needCursorReupload 置为 true）。
  FrameResult SetMonitorGeometry(const RECT& windowRect, UINT dpi, double trailMs,
                                 bool* needCursorReupload);

  // 当前可见的采样点视觉数量（诊断用）。
  int liveSprites() const { return live_; }

  // 上一次 Commit 的耗时（QPC ticks；本次没有提交时为 0）。诊断 DWM 是否在反压。
  uint64_t lastCommitTicks() const { return lastCommitTicks_; }

  // 最早一个可见槽位的到期 QPC 时刻（O(1)）；没有可见槽位时返回 0。
  // 调用方按它 arm 一次性高分辨率定时器，而不是按固定 tick 轮询。
  uint64_t NextExpiryQpc() const;

  // 上传光标贴图（形状变化时调用），必要时按窗口 DPI 缩放到系统光标的像素尺寸。
  bool SetCursorBitmap(const CursorBitmap& bmp);

  // 批量处理新增采样点并隐藏已过期槽位；只有确有变化时才 Commit。
  FrameResult Update(const Sample* samples, uint32_t count, uint64_t now, bool* committed);

  // 隐藏所有已到期的槽位；有变化时 Commit。由到期定时器调用。
  FrameResult Expire(uint64_t now, bool* committed);

  // 立即隐藏所有可见视觉并提交（光标不可见时调用）。
  void HideAll();

 private:
  struct Slot {
    Microsoft::WRL::ComPtr<IDCompositionVisual3> visual;  // 继承自 IDCompositionVisual2
    uint64_t bornQpc = 0;
  };

  FrameResult CommitNow(bool* committed);
  bool HideExpired(uint64_t now);
  void HideVisibleSlots();
  void LogFailureOnce(const wchar_t* step, HRESULT hr);

  Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dCtx_;
  // 用 DCompositionCreateDevice3 创建：只有高版本设备创建出来的 visual 才会暴露
  // IDCompositionVisual3（基础版 DCompositionCreateDevice 只给得到 Visual 基接口）。
  Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> dcompDevice_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual2> root_;
  Microsoft::WRL::ComPtr<IDCompositionSurface> spriteSurface_;
  std::vector<Slot> slots_;  // FIFO 环形槽位：可见区间为 [head_, head_ + live_) mod N

  RECT rect_{};  // 窗口矩形（虚拟屏幕坐标）
  uint64_t qpcFreq_ = 0;
  uint64_t trailTicks_ = 0;
  uint64_t lastCommitTicks_ = 0;
  int head_ = 0;
  int live_ = 0;
  int spriteW_ = 0;
  int spriteH_ = 0;
  int hotX_ = 0;
  int hotY_ = 0;
  bool hasCursorBitmap_ = false;
  bool initialized_ = false;
  bool loggedFailure_ = false;
};
