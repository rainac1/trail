# Trail

[English](README.en.md) | 中文

Windows 全屏透明叠加层：在光标身后显示一串残影。鼠标每回报一次，就多一个尾迹点；每个
尾迹点显示 `--trail-ms` 毫秒后消失。默认 8ms 对应 125Hz 刷新率的一帧，也就是每个尾迹点
只显示一帧就消失。尾迹长度取决于光标在这段时间内移动了多远。

## 构建

前置：Windows 10+、Visual Studio 2017+（“使用 C++ 的桌面开发”工作负载）、CMake 3.16+。

```bat
build.bat            REM Release
build.bat Debug      REM Debug
```

产物：`build\trail.exe`（Ninja）或 `build\Release\trail.exe`（MSBuild）。

## 运行

```bat
build\trail.exe
build\trail.exe --trail-ms 12 --sprites 96
```

| 参数 | 含义 |
|---|---|
| `--trail-ms <ms>` | 每个尾迹点显示多久；默认 8 毫秒，对应 125Hz 的一帧 |
| `--sprites <n>` | 最多同时显示多少个尾迹点，默认 64 |

`Ctrl+Alt+Q` 退出。

## 已知限制

- **只画主显示器**；其它显示器上没有尾迹。显示器分辨率、缩放比例或数量变化后需要重启。
- **同时最多 64 个尾迹点**。尾迹设得很长时，最早的点会被提前挤掉；如果尾迹看起来比实际
  移动短，把 `--sprites` 调大。
- **偶尔，最新的一小段尾迹会被较旧的点盖住，最长约一个 `--trail-ms`**；默认 8ms
  （一帧）下几乎看不出来。
- **显卡驱动重置、睡眠唤醒等情况可能让尾迹整体消失**；程序会自动恢复，恢复前有一小段空白。
- **需要支持 DirectX 11 的显卡，并且 Windows 的桌面合成保持开启**（默认开启）。

## 日志

运行日志写在 exe 同目录的 `trail.log`，程序运行时也能查看。启动失败、显卡异常后的自动
恢复、显示器变化等事件都会记录在这里；设置 `TRAIL_NO_UI=1` 可以关闭启动失败时的弹窗。
