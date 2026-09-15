# Trail

> **Language:** this is the English README. The default README is the Chinese
> [`../README.md`](../README.md).

A Windows full-screen transparent overlay that renders a **sub-frame cursor trail**
with C++ + Direct2D: it retrieves every cursor movement sample within the past
frame and draws the pointer texture at each position on the next frame. Each
sample is shown for exactly one frame before disappearing, so the trail length ≈
the cursor's movement over one frame.

## Documentation

Detailed, durable notes live in `docs/`:

| Document | Contents |
|---|---|
| [`architecture.md`](architecture.md) | Thread model, sub-frame trail semantics, cursor texture capture, the DirectComposition transparent render path, low-latency vblank-front alignment, performance characteristics |
| [`device-loss-recovery.md`](device-loss-recovery.md) | The "rendering suddenly disappeared" failure mode: why the overlay goes transparent, how device loss is detected, the automatic in-process rebuild, and the overlay health watchdog |
| [`diagnostics.md`](diagnostics.md) | `trail.log` (readable while the program runs), log-line reference, a triage guide for a missing trail, toolchain pitfalls |
| [`limitations.md`](limitations.md) | Known limitations |

## Building

### Prerequisites

- **Windows 10 1803+ / 11** (desktop composition / DWM must be enabled). 1803 is a hard
  floor: the waiting mechanism is a high-resolution waitable timer
  (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) and there is no fallback path, so a missing
  prerequisite is reported as an error and exits
- **Visual Studio 2017+** with the "Desktop development with C++" workload
  (MSVC compiler, Windows SDK, Ninja)
- **CMake 3.16+** on `PATH` (https://cmake.org or `winget install cmake`)
- Ninja is optional (bundled with VS2019+; the script falls back to MSBuild)

### Option 1: build.bat (recommended)

```bat
build.bat            # Release build
build.bat Debug      # Debug build
```

The script locates Visual Studio via `vswhere`, sets up the environment with
`vcvarsall.bat x64`, prefers the Ninja generator (falls back to Visual Studio /
MSBuild), then compiles. Output paths:

| Generator | Output |
|---|---|
| Ninja | `build\trail.exe` |
| Visual Studio (MSBuild) | `build\Release\trail.exe` (or `build\Debug\...`) |

### Option 2: manual (cmd, Ninja)

Adjust the Visual Studio path to match your install (query it with
`vswhere -latest -property installationPath`):

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Option 3: manual (cmd, MSBuild, no Ninja/vcvarsall)

```bat
cmake -S . -B build        REM generates a Visual Studio project (multi-config)
cmake --build build --config Release
```

### Build troubleshooting

- **`[ERROR] Visual Studio C++ toolchain not found`**: Visual Studio or the C++
  workload is missing, or it is installed but not registered with the Visual Studio
  Installer (so `vswhere` cannot see it) — see
  [`diagnostics.md`](diagnostics.md#toolchain-pitfalls).
- **`cmake is not recognized`**: CMake is not installed or not on `PATH`.
- **Build errors for `d2d1.h` / `d3d11.h` / `dcomp.h`**: build from the `vcvarsall x64`
  (or VS Developer Command Prompt) environment; `build.bat` does this automatically.
- **`The build directory is incompatible with the generator`**: `build/` was
  configured with a different generator before — delete `build/` and rebuild.
- Build with **MSVC**: a MinGW build compiles and runs, but MinGW's `libdwmapi.a` stubs
  some of the DWM entry points, so use the MSVC build when diagnosing — see
  [`diagnostics.md`](diagnostics.md#toolchain-pitfalls).
- Build/runtime issues are logged to `trail.log` next to the exe; the log can be read
  while the program is running ([`diagnostics.md`](diagnostics.md)).

## Running

```bat
build\trail.exe                # default
build\trail.exe --hide-cursor  # also hide the system cursor (inside the overlay)
```

- **Quit**: `Ctrl+Alt+Q`
- The window covers the entire virtual desktop (multi-monitor), is click-through,
  and never steals focus.

## How it works (summary)

One thread creates the full-virtual-desktop layered window, runs the message loop and
renders every frame. Instead of polling the cursor, it pulls the movement samples
recorded since the previous frame out of the system's own 64-point mouse-move history
(`GetMouseMovePointsEx`) and draws the cached pointer texture at each of them into an
offscreen Direct2D bitmap; that bitmap is blitted into a DirectComposition swapchain
which DWM composites with per-pixel alpha. Every sample is drawn exactly once, so the
trail is exactly the cursor's movement over one frame.

Two behaviours are worth knowing about up front:

- **It runs at the composition refresh rate and submits each frame just before the DWM
  composition that will display it**: the refresh period and the composition timestamp
  are read straight from the DWM composition clock (`DwmGetCompositionTimingInfo`, once
  per frame, a few µs), and a high-resolution waitable timer plus a 0.5 ms spin waits out
  the deadline, keeping the trail head within a few milliseconds of the system cursor.
  **There is no fallback**: if the composition clock or the timer is unavailable the
  program reports the error and exits with code 1 — see [`architecture.md`](architecture.md)
  and [`limitations.md`](limitations.md).
- **The overlay's pixels come entirely from DWM** (the window has no GDI content), so a
  lost DXGI/DirectComposition device makes it fully transparent instead of visibly
  broken. That is detected and recovered in-process — see
  [`device-loss-recovery.md`](device-loss-recovery.md).

## Known limitations

See [`limitations.md`](limitations.md) — there is no fallback path (a missing
prerequisite is a hard error at startup), device loss costs a few frames before the
rebuild finishes, the watchdog cannot win against a window stacked above the overlay, a
blocking `Present` blocks the single-threaded loop, mixed-DPI multi-monitor setups can
offset trail coordinates, and a few more.
