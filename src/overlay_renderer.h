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
// 尾迹不在像素里：每个采样点是一个 visual，按级别共享预烘焙的光标贴图 surface；运行期只改
// offset / visible / level，没有绘制、没有整屏拷贝。一个采样点"显示一帧然后消失"变成"把它设成
// 不可见"，于是擦除这个操作根本不存在。
//
// 分级亮度（尾随零标尺分布，方案 B 零平移）：
//   - 每个采样点出生时获得单调递增序号 seq，永久级别为 ctz(seq & seqMask_) + 1（ctz(0) = -1 -> 0 级），
//     几何级数线性不透明度 alpha(level) = base^(-level)（默认 base = 1.618，0 级为 1.0）。
//   - 每次更新保持最新点为 0 级、仅将上一个最新点降级回其永久出生级别，O(1) 无平移。
//   - 普通 SDR 屏：按总级别数预烘焙黑白双端点逆 gamma 补偿的预乘 Surface 表，热路径零像素运算；
//   - HDR / Windows 11 ACM 屏：DWM 已在 FP16 线性 scRGB 空间合成，共享 0 级贴图并直接用
//     IDCompositionVisual3::SetOpacity(alpha)（MULTIPLY 模式）实现原生线性混合，避免双重补偿。
//
// 槽位按 FIFO 环形队列管理：新采样点按单调递增的时间戳写入队尾，满时覆盖队头；可见槽位
// 始终是连续区间 [head_, head_ + live_) mod N，因此到期查询 O(1)、过期回收 O(expired)。
//
// 多屏与多 DPI：始终只维护一个叠加层窗口并跟随光标所在显示器移动；当目标显示器 DPI 对应的
// 光标像素尺寸变化时，仅原位重建分级 surface 表并重绑视觉，无需重建 D3D/DComp 设备。
//
// 设备丢失：本窗口没有 GDI 内容，可见像素 100% 来自 DWM 合成的视觉树，设备一旦丢失叠加层
// 会整体变透明且不会自愈 —— 调用方必须重建整套设备与内容（RecreateDevice / DeviceValid）。
class OverlayRenderer {
 public:
  static constexpr double kDefaultOpacityBase = 1.618;

  enum class FrameResult {
    Ok,              // 正常（本次调用可能没有任何变化，也就没有 Commit）
    Failed,          // 普通失败（本次状态没提交完整，设备仍可用）
    RecreateDevice,  // 设备丢失：调用方必须重建整套设备与内容
  };

  // windowRect：窗口在虚拟屏幕坐标里的矩形（物理像素）；dpi：窗口的 DPI；
  // advancedColor：当前屏是否处于 HDR/ACM 线性合成模式；opacityBase：几何分级底数（>= 1.0）。
  bool Initialize(HWND hwnd, const RECT& windowRect, UINT dpi, int maxSprites, uint64_t qpcFreq,
                  bool advancedColor = false, double opacityBase = kDefaultOpacityBase);
  void Shutdown();

  bool ready() const { return initialized_; }

  // 设备是否仍然有效。DirectComposition 在设备丢失时会向合成其内容的窗口发送 WM_PAINT。
  bool DeviceValid();

  // 尾迹寿命（毫秒）：采样点从可见到隐藏的时间窗。
  void SetTrailLifetimeMs(double ms);

  // 切换叠加层所在显示器的几何、DPI、尾迹寿命与色彩模式（隐藏旧尾迹；若目标 DPI 导致光标
  // 像素尺寸变化或 HDR/ACM 状态切换，则重建/重置 surface 并将 *needCursorReupload 置为 true）。
  FrameResult SetMonitorGeometry(const RECT& windowRect, UINT dpi, double trailMs,
                                 bool advancedColor, bool* needCursorReupload);

  // 当前可见的采样点视觉数量（诊断用）。
  int liveSprites() const { return live_; }

  // 上一次 Commit 的耗时（QPC ticks；本次没有提交时为 0）。诊断 DWM 是否在反压。
  uint64_t lastCommitTicks() const { return lastCommitTicks_; }

  // 最早一个可见槽位的到期 QPC 时刻（O(1)）；没有可见槽位时返回 0。
  // 调用方按它 arm 一次性高分辨率定时器，而不是按固定 tick 轮询。
  uint64_t NextExpiryQpc() const;

  // 上传光标贴图（形状变化时调用），在线性光空间按窗口 DPI 缩放并烘焙各级贴图。
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
    uint32_t seq = 0;  // 采样点出生序号（方案 B 永久定级依据）
    int level = -1;    // 当前 visual 已绑定的亮度级别（避免重复 SetContent/SetOpacity）
  };

  HRESULT CreateLevelSurfaces(int width, int height,
                              std::vector<Microsoft::WRL::ComPtr<IDCompositionSurface>>& out);
  bool UploadSurfacePixels(IDCompositionSurface* surface, const uint8_t* pixels, int width,
                           int height);
  int LevelForSeq(uint32_t seq) const;
  void ApplySlotLevel(Slot& slot, int level);
  void ResetSlotBindings();

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
  std::vector<Microsoft::WRL::ComPtr<IDCompositionSurface>> levelSurfaces_;
  std::vector<float> levelAlphas_;
  std::vector<Slot> slots_;  // FIFO 环形槽位：可见区间为 [head_, head_ + live_) mod N

  RECT rect_{};  // 窗口矩形（虚拟屏幕坐标）
  uint64_t qpcFreq_ = 0;
  uint64_t trailTicks_ = 0;
  uint64_t lastCommitTicks_ = 0;
  double opacityBase_ = kDefaultOpacityBase;
  uint32_t seqMask_ = 0;
  uint32_t seq_ = 0;
  int levelCount_ = 1;
  int head_ = 0;
  int live_ = 0;
  int spriteW_ = 0;
  int spriteH_ = 0;
  int hotX_ = 0;
  int hotY_ = 0;
  bool advancedColor_ = false;
  bool hasCursorBitmap_ = false;
  bool initialized_ = false;
  bool loggedFailure_ = false;
};
