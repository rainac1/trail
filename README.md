# Subframe Cursor Trail Overlay

Windows 全屏透明叠加层：用 C++ + Direct2D 绘制**帧内鼠标尾迹** —— 取过去一帧时间内
鼠标的所有采样点，在下一帧把指针纹理绘制到对应位置。每个采样点恰好显示一帧后消失，
尾迹长度 ≈ 一帧内的鼠标位移。

## 构建

```bat
build.bat
```

脚本通过 `vswhere` 自动定位 Visual Studio（需安装"使用 C++ 的桌面开发"），
生成 `build\subframe_cursor_trail.exe`。手动方式：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 运行

```bat
build\subframe_cursor_trail.exe                # 默认 1000Hz 采样
build\subframe_cursor_trail.exe --sample-ms 2  # 500Hz 采样（省 CPU）
build\subframe_cursor_trail.exe --hide-cursor  # 顺带隐藏系统光标（仅覆盖窗口内）
```

- **退出**：`Ctrl+Alt+Q`
- 窗口覆盖整个虚拟桌面（多显示器），点击穿透，不抢焦点。

## 工作原理

### 线程模型（两个线程）

| 线程 | 职责 |
|---|---|
| 主线程 | 创建窗口、消息循环、渲染。`Present(1, 0)` 阻塞到 vsync，空闲时几乎不占 CPU |
| 采样线程 | `THREAD_PRIORITY_HIGHEST`，按 `--sample-ms`（默认 1ms ≈ 1000Hz）轮询 `GetCursorPos`，把 `(QPC 时间戳, x, y)` 推入无锁 SPSC 环形缓冲 |

采样线程与渲染线程之间用**无锁单生产者/单消费者环形缓冲**（`src/ring_buffer.h`）同步，
通过 release/acquire 原子配对保证可见性，全程无锁、无动态分配。

### 帧内尾迹语义

渲染线程记录上一帧时刻 `T_prev`，每帧只绘制 `t ∈ [T_prev, T_now)` 内的采样点。
由于帧窗口无缝连续覆盖整个时间线，**每个采样点恰好被绘制一帧**：最新采样点在采样后
的下一帧出现，随后滑出窗口消失。尾迹由最近一帧时间内鼠标经过的位置组成，且渲染帧率
决定尾迹"年龄"上限、采样率决定路径的精确度（`src/main.cpp` 的 `RenderOneFrame`）。

### 指针纹理（Windows API）

1. `GetCursorInfo` 取当前光标句柄 `hCursor` 与显示状态；
2. `CopyIcon` + `GetIconInfo` 取得光标位图与热点；
3. `GetDIBits` 读出 32bpp BGRA 像素，做 alpha 预乘；
4. 以 `hCursor` 为 key 缓存为 `ID2D1Bitmap`（premultiplied），**句柄不变则每帧零开销**
   （每帧仅一次 `GetCursorInfo`，约 1µs）。动画光标取当前帧，旧式 mask-only 光标走
   AND/XOR 合成 fallback。

### 低延迟渲染（vblank 前对齐）

默认启用，将尾迹与系统光标的延迟从约 1 帧压缩到数毫秒：

- **`DwmFlush` 自举校准**：实测合成刷新周期与 vsync 相位（约 8.3ms@120Hz / 16.7ms@60Hz）
- **vblank 前对齐**：忙等（分层等待：远睡/近让出/极近忙等）到 `next_vsync - budget` 再渲染，
  `Present(0)` 让帧赶上*当前* vsync 显示，而不是等*下一个*（原来 `Present(1,0)` 平均多等半帧以上）
- **实时头部点**：渲染提交前最后一刻 `GetCursorPos` 注入尾迹头，头部延迟 ≈ 渲染预算（2-3ms）
- **自适应预算**：渲染耗时 EMA + 1.5ms 余量，限幅 [2, 8]ms，渲染快时自动收紧
- 校准失败（无 DWM 合成）时自动回退 `Present(1,0)`；运行日志每 300 帧输出
  `missed`（渲染超时错过 vsync 的帧占比，实测约 0.2%）与预算统计

### 透明渲染（DirectComposition + 硬件 GPU 单路径）

- D2D 渲染到自建 premultiplied 离屏位图 → 每帧 GPU `CopyResource` 拷贝到 flip-model
  composition swapchain → `IDCompositionVisual::SetContent` 由 DWM 按 premultiplied alpha
  合成，`Present(1,0)` vsync 节流
- 窗口样式：`WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED`，
  并调用 `SetLayeredWindowAttributes(alpha=255)`
- **点击穿透**：`WS_EX_LAYERED | WS_EX_TRANSPARENT` 组合使整个窗口对鼠标命中测试透明
  （微软 window-features 文档：layered 窗口命中基于形状/透明度，加 `WS_EX_TRANSPARENT`
  后形状被忽略、鼠标事件传给下层窗口；DComp 允许 layered target 窗口）。注意
  `WM_NCHITTEST → HTTRANSPARENT` 只转发给同线程窗口、`WS_EX_TRANSPARENT` 单独使用对
  命中无效——两者都不是跨进程穿透的正确机制
- 仅使用硬件 D3D11 设备，**不做 WARP 软件降级**：初始化失败时弹窗报错并写日志
- 为什么用 DirectComposition 而非 flip+Hwnd：部分显示栈（实测 AMD Radeon 780M 核显）
  对 `CreateSwapChainForHwnd`/`CreateBitmapFromDxgiSurface` + premultiplied alpha 返回
  `DXGI_ERROR_INVALID_CALL`/`E_INVALIDARG`，而 `CreateSwapChainForComposition` +
  离屏 blit 完整可用，是该场景下唯一可靠的硬件透明路径
- 每帧 `Clear` 为全透明后逐个 `DrawBitmap`（`NEAREST_NEIGHBOR` 插值，1:1 锐利且开销最低）

## 性能设计要点

- 渲染热路径零分配：采样点收集用栈上定长数组，光标纹理仅在形状变化时抓取一次；
- 无锁 SPSC 环形缓冲：`Capacity = 4096`，1000Hz 下可容忍约 4 秒渲染卡顿而不丢序；
- 采样线程 `timeBeginPeriod(1)` 提升定时器精度；高优先级但非 `TIME_CRITICAL`，
  避免抢占 DWM / 游戏线程；
- 主循环由 vsync 自然节流，空闲 CPU 占用极低。

## 已知限制

- 未处理 D3D 设备丢失（`D2DERR_RECREATE_TARGET` 时直接跳过，罕见场景）；
- 多显示器且各屏 DPI 不同时，跨屏窗口存在 DWM 缩放，尾迹坐标可能偏移，推荐单 DPI 环境；
- `--hide-cursor` 仅在覆盖窗口内隐藏光标（窗口覆盖整个桌面，等效全局）；
- 光标句柄被系统复用（形状相同）时缓存直接命中，纹理依然正确；
- 需要支持 D3D11 硬件加速的 GPU 及启用的桌面合成（DWM）：无硬件设备时程序拒绝启动（不降级 WARP）；
- 初始化失败时通过 MessageBox 与 exe 同目录的 `subframe_cursor_trail.log` 输出具体失败步骤与
  HRESULT（环境变量 `SUBFRAME_NO_UI=1` 可禁用弹窗）；
- 尾迹头与系统光标仍有约一个渲染预算（2-3ms）的固有差距（采样→DWM 合成物理下限），
  以及采样线程的量化误差（默认 1ms）。
