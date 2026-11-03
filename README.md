# Trail

一个 Windows 全屏透明叠加层，用 C++ + DirectComposition 渲染**帧内光标尾迹**：把系统在
一帧内记录下的每一个鼠标移动采样点摊在光标身后，每个采样点存活 `--trail-ms` 毫秒后
消失。尾迹长度 ≈ 光标在这段时间内的位移，路径分辨率由系统鼠标报告率决定（典型
125–1000 Hz，所以一次快速甩动会被拆成十几个点）。

与"把尾迹画进一张纹理"的常见做法不同，这里**尾迹不在像素里**：每个采样点是一个
`IDCompositionVisual`，所有视觉共享同一张光标贴图。运行期只改 visual 的 offset 和可见
性 —— 没有绘制、没有整屏拷贝、没有 `Present`，也就没有"这一帧赶不上合成"这回事。

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | 单线程 tick 模型、采样点 = visual、提交节奏与延迟模型、光标贴图上传、深度顺序、性能特征 |
| [`docs/device-loss-recovery.md`](docs/device-loss-recovery.md) | “渲染突然消失”这一类故障：设备丢失如何检出、进程内如何重建、窗口健康看门狗 |
| [`docs/diagnostics.md`](docs/diagnostics.md) | `trail.log`（程序运行中即可读取）、日志行速查表、“尾迹不见了”的分诊步骤、工具链陷阱 |
| [`docs/limitations.md`](docs/limitations.md) | 已知限制 |
| [`docs/README.en.md`](docs/README.en.md) | 本 README 的英文版 |

## 构建

### 前置要求

- **Windows 10+**（用到了 `DCompositionCreateDevice3` 与 `IDCompositionVisual3`），且桌面
  合成（DWM）开启
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
- **`d3d11.h` / `dcomp.h` 编译报错**：请在 `vcvarsall x64`（或 VS 开发者命令提示符）
  环境下构建；`build.bat` 会自动完成这一步。
- **`The build directory is incompatible with the generator`**：`build/` 之前是用别的
  生成器配置的——删掉 `build/` 重新构建。
- **请用 MSVC 构建**：MinGW 下能编译运行，但它的导入库把部分 DWM/DComp 入口做成 stub，
  排错时以 MSVC 构建为准——见
  [`docs/diagnostics.md`](docs/diagnostics.md#toolchain-pitfalls)。
- 构建/运行问题会记录到 exe 同目录的 `trail.log`，该文件**在程序运行期间也能读取**
  （见 [`docs/diagnostics.md`](docs/diagnostics.md)）。

## 运行

```bat
build\trail.exe                                        REM 默认：tick 1000 Hz、尾迹 8 ms、64 个槽位
build\trail.exe --commit-hz 250 --trail-ms 12          REM 少提交、更长尾迹
build\trail.exe --hide-cursor                          REM 同时隐藏系统光标（在叠加层范围内）
```

- `--commit-hz <hz>`：视觉树 tick/提交频率上限（默认 1000）。它只决定"新鲜度"
  （平均头部滞后 ≈ 半个 tick），不影响正确性。
- `--trail-ms <ms>`：采样点存活时间（默认 8；120 Hz 下约等于“一帧的位移”）。
- `--sprites <n>`：环形槽位数 = 同时可见的采样点上限（默认 64）。
- `--hide-cursor`：隐藏系统光标（叠加层覆盖主显示器，因此在该屏上等价于全局隐藏）。
- **退出**：`Ctrl+Alt+Q`。窗口只覆盖**主显示器**，点击穿透、从不抢焦点；其它显示器上不画
  尾迹。

## 工作原理（简述）

只有一个线程：它创建一个覆盖**主显示器**的分层窗口、跑消息循环，并按 `--commit-hz` 的
节奏 tick。

每个 tick：

1. `GetCursorInfo` 取当前光标句柄 / 可见性 / 位置；
2. 位置变了才从系统自己的 64 点鼠标历史（`GetMouseMovePointsEx`）里取回“自上次以来
   新增”的采样点——鼠标静止时省掉这次最贵的内核往返；
3. 每个新采样点占用环形槽位里的一个 visual：写 offset、`SetVisible(TRUE)`；超过
   `--trail-ms` 的槽位 `SetVisible(FALSE)`；头部 visual 只在 `--hide-cursor` 模式下绘制
   （系统光标可见时它被硬件光标完全遮住，纯属多余）；
4. **只有真的发生了变化才 `Commit`** —— DWM 在它自己的合成时刻读取视觉树。

于是“赶不上合成”这件事被消掉了：没有需要追赶的截止时刻，最坏情况只是“画面旧一个
tick”，而不是“漏帧 → 尾迹陈旧一整个刷新周期”。

## 性能特征（参考机：2560×1600 / 120 Hz / Radeon 780M / Windows 11）

- `update` 一次平均 **15–18 µs**、峰值 0.1–0.4 ms（约 1000 采样点/s 时）；
- 静止 **3.6%** 单核、移动 **2.4%** 单核；静止那部分几乎全是 tick 节流的两次内核调用，
  `--commit-hz 250` 会降到 ~1%；
- 每帧的 GPU 工作量 = 几个 32×32 的贴图四边形；没有整屏拷贝、没有全屏绘制、没有交换链。

## 已知限制

见 [`docs/limitations.md`](docs/limitations.md)。简要来说：**只画主显示器**；显示器拓扑 /
DPI 变化需要重启；提交节奏只由 `--commit-hz` 决定（不与 DWM 合成相位对齐，最坏情况是画面
旧一个 tick）；环形槽位数限制同时可见的采样点数量（超了就丢最老的）；`--trail-ms` 是时间窗
而不是“一帧的位移”；设备丢失后需要几百毫秒重建，等等。
