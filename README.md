# Trail

一个 Windows 全屏透明叠加层，用 C++ + Direct2D 渲染**帧内光标尾迹**：取出上一帧
期间记录的每一个光标移动采样点，在下一帧把光标贴图绘制在各自的位置上。每个采样点
只显示一帧便消失，因此尾迹长度 ≈ 光标在一帧内的位移。

## 文档

详细且长期有效的项目知识都放在 `docs/` 下：

| 文档 | 内容 |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | 单线程模型、帧内尾迹语义、光标纹理捕获、DirectComposition 透明渲染路径、低延迟 vblank 前对齐、性能特征 |
| [`docs/device-loss-recovery.md`](docs/device-loss-recovery.md) | “渲染突然消失”这一类故障：叠加层为何会整体变透明、设备丢失如何被检出、进程内自动重建、窗口健康看门狗 |
| [`docs/diagnostics.md`](docs/diagnostics.md) | `trail.log`（程序运行中即可读取）、日志行速查表、“尾迹不见了”的分诊步骤、工具链陷阱 |
| [`docs/limitations.md`](docs/limitations.md) | 已知限制 |
| [`docs/README.en.md`](docs/README.en.md) | 本 README 的英文版 |

## 构建

### 前置要求

- **Windows 10 1803+ / 11**（必须启用桌面合成 / DWM）。1803 是硬性下限：等待机制用的是
  高分辨率可等待定时器（`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`），且本程序没有任何
  降级回退，不满足前提会直接报错退出
- **Visual Studio 2017+**，需安装“使用 C++ 的桌面开发”工作负载（MSVC 编译器、Windows
  SDK、Ninja）
- **CMake 3.16+** 在 `PATH` 上（https://cmake.org 或 `winget install cmake`）
- Ninja 可选（VS2019+ 自带；脚本会自动回退到 MSBuild）

### 方式一：build.bat（推荐）

```bat
build.bat            # Release 构建
build.bat Debug      # Debug 构建
```

脚本用 `vswhere` 定位 Visual Studio，通过 `vcvarsall.bat x64` 准备环境，优先使用
Ninja 生成器（否则回退到 Visual Studio / MSBuild），然后编译。产物路径：

| 生成器 | 产物 |
|---|---|
| Ninja | `build\trail.exe` |
| Visual Studio (MSBuild) | `build\Release\trail.exe`（或 `build\Debug\...`） |

### 方式二：手动（cmd + Ninja）

请按实际安装位置修改 Visual Studio 路径（可用
`vswhere -latest -property installationPath` 查询）：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 方式三：手动（cmd + MSBuild，不用 Ninja/vcvarsall）

```bat
cmake -S . -B build        REM 生成 Visual Studio 工程（多配置）
cmake --build build --config Release
```

### 构建排错

- **`[ERROR] Visual Studio C++ toolchain not found`**：没装 Visual Studio、没装 C++
  工作负载，或者装了但没有在 Visual Studio Installer 中注册（因此 `vswhere` 看不到
  它）——见 [`docs/diagnostics.md`](docs/diagnostics.md#toolchain-pitfalls)。
- **提示 `cmake is not recognized`**：CMake 未安装或不在 `PATH` 上。
- **`d2d1.h` / `d3d11.h` / `dcomp.h` 编译报错**：请在 `vcvarsall x64`（或 VS 开发者
  命令提示符）环境下构建；`build.bat` 会自动完成这一步。
- **`The build directory is incompatible with the generator`**：`build/` 之前是用别的
  生成器配置的——删掉 `build/` 重新构建。
- **请用 MSVC 构建**：MinGW 下可编译运行，但它的 `libdwmapi.a` 把部分 DWM 入口做成
  stub（新旧方案受影响的程度不同），排错时以 MSVC 构建为准——见
  [`docs/diagnostics.md`](docs/diagnostics.md#toolchain-pitfalls)。
- 构建/运行问题会记录到 exe 同目录的 `trail.log`，该文件**在程序运行期间也能读取**
  （见 [`docs/diagnostics.md`](docs/diagnostics.md)）。

## 运行

```bat
build\trail.exe                # 默认
build\trail.exe --hide-cursor  # 同时隐藏系统光标（在叠加层范围内）
```

- **退出**：`Ctrl+Alt+Q`
- 窗口覆盖整个虚拟桌面（多显示器），点击穿透，且从不抢焦点。

## 工作原理（简述）

只有一个线程：它创建覆盖整个虚拟桌面的分层窗口、跑消息循环并逐帧渲染。取轨迹的方式
不是轮询光标位置，而是每帧从系统自己的 64 点鼠标移动历史里（`GetMouseMovePointsEx`）
取出“自上一帧以来新增”的采样点，把缓存的光标贴图逐一画进一张离屏 Direct2D 位图；该
位图再拷贝进 DirectComposition 交换链，由 DWM 按逐像素 alpha 合成。每个采样点恰好被
绘制一次，所以尾迹正好等于光标在一帧内的位移。

有两点值得先知道：

- **它以合成刷新率持续运行，并在每次合成之前一点点提交**：刷新周期与合成时刻直接读
  自 DWM 合成时钟（`DwmGetCompositionTimingInfo`，每帧一次、约 10 µs），用高分辨率
  可等待定时器加 0.5 ms 自旋精确等到截止时刻，使尾迹头部与系统光标只差几毫秒。
  **没有降级回退**：读不到合成时钟或定时器不可用会直接报错并以退出码 1 结束进程——见
  [`docs/architecture.md`](docs/architecture.md) 与 [`docs/limitations.md`](docs/limitations.md)。
- **叠加层的像素完全由 DWM 提供**（窗口本身没有任何 GDI 内容），因此一旦
  DXGI/DirectComposition 设备丢失，它会整体变透明，而不是“看起来坏掉”。这种情况会
  被检出并在进程内自动恢复——见
  [`docs/device-loss-recovery.md`](docs/device-loss-recovery.md)。

## 已知限制

见 [`docs/limitations.md`](docs/limitations.md)。简要来说：没有回退路径（不满足前提即报错
退出）；设备丢失后需要几帧才能重建完成；看门狗无法对抗排在叠加层之上的窗口；单线程设计下
`Present` 阻塞会连带卡住消息循环；混合 DPI 的多显示器环境下尾迹坐标可能偏移，等等。
