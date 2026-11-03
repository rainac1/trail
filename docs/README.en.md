# Trail

> **Language:** this is the English README. The default README is the Chinese
> [`../README.md`](../README.md).

A full-screen, click-through, always-on-top transparent overlay for Windows that draws a
**sub-frame cursor trail** with C++ + DirectComposition: every mouse-move sample the
system recorded during the last moment is laid out behind the cursor, and each sample
disappears after `--trail-ms` milliseconds. The trail length is the cursor's movement over
that time window, and the path resolution is the system mouse report rate (typically
125–1000 Hz, so a fast flick is split into a dozen points).

Unlike the usual approach of *painting* a trail into a texture, the trail here is **not in
the pixels**: every sample is an `IDCompositionVisual` and all visuals share one cursor
sprite. At runtime only a visual's offset and visibility change — no drawing, no
full-screen copy, no `Present`, and therefore no "this frame missed its composition".

## Documentation

Detailed, durable notes live in `docs/`:

| Document | Contents |
|---|---|
| [`architecture.md`](architecture.md) | single-threaded tick model, one sample = one visual, commit cadence and latency model, sprite upload, z-order, performance |
| [`device-loss-recovery.md`](device-loss-recovery.md) | the "rendering suddenly disappeared" failure class: detection, in-process rebuild, window health watchdog |
| [`diagnostics.md`](diagnostics.md) | `trail.log` (readable while the program runs), log-line reference, triage guide, toolchain pitfalls |
| [`limitations.md`](limitations.md) | known limitations |
| [`../README.md`](../README.md) | Chinese README (default) |

## Building

### Prerequisites

- **Windows 10+** (`DCompositionCreateDevice3` and `IDCompositionVisual3`), with desktop
  composition (DWM) enabled.
- **Visual Studio 2017+** with the "Desktop development with C++" workload (MSVC, Windows
  SDK, Ninja).
- **CMake 3.16+** on `PATH` (https://cmake.org or `winget install cmake`).
- Ninja optional (bundled with VS2019+; the script falls back to MSBuild).

### Option 1: build.bat (recommended)

```bat
build.bat            # Release build
build.bat Debug      # Debug build
```

The script locates Visual Studio via `vswhere`, sets up the environment with
`vcvarsall.bat x64`, prefers the Ninja generator (falls back to Visual Studio / MSBuild),
then compiles. Output paths:

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

- **`[ERROR] Visual Studio C++ toolchain not found`**: Visual Studio or the C++ workload is
  missing, or it is installed but not registered with the Visual Studio Installer (so
  `vswhere` cannot see it) — see [`diagnostics.md`](diagnostics.md#toolchain-pitfalls).
- **`cmake is not recognized`**: CMake is not installed or not on `PATH`.
- **Build errors for `d3d11.h` / `dcomp.h`**: build from the `vcvarsall x64` (or VS
  Developer Command Prompt) environment; `build.bat` does this automatically.
- **`The build directory is incompatible with the generator`**: `build/` was configured with
  a different generator before — delete `build/` and rebuild.
- Build with **MSVC**: a MinGW build compiles and runs, but its import libraries stub some
  DWM/DComp entry points, so use the MSVC build when diagnosing — see
  [`diagnostics.md`](diagnostics.md#toolchain-pitfalls).
- Build/runtime issues are logged to `trail.log` next to the exe; the log can be read while
  the program is running ([`diagnostics.md`](diagnostics.md)).

## Running

```bat
build\trail.exe                                        REM defaults: tick 1000 Hz, trail 8 ms, 64 slots
build\trail.exe --commit-hz 250 --trail-ms 12          REM fewer commits, longer trail
build\trail.exe --hide-cursor                          REM also hide the system cursor
```

- `--commit-hz <hz>`: upper bound on the visual-tree tick/commit rate (default 1000). It
  only sets the freshness (average head lag ≈ half a tick); it does not affect correctness.
- `--trail-ms <ms>`: how long a sample stays visible (default 8; at 120 Hz that is about
  "one frame of movement").
- `--sprites <n>`: number of ring slots = max concurrently visible samples (default 64).
- `--hide-cursor`: hide the system cursor (the overlay covers the primary monitor, so this is
  effectively global on that screen).
- **Quit**: `Ctrl+Alt+Q`. The single window covers the **primary monitor** only; it is
  click-through and never steals focus, and nothing is drawn on the other monitors.

## How it works (summary)

There is a single thread: it creates one layered window covering the **primary monitor**, runs
the message loop and ticks at `--commit-hz`.

Every tick:

1. `GetCursorInfo` fetches the cursor handle / visibility / position;
2. only if the position changed, `GetMouseMovePointsEx` pulls the samples added since the
   previous tick out of the system's own 64-point mouse history — the most expensive kernel
   round trip is skipped while the mouse is still;
3. each new sample occupies one ring slot: set its offset, `SetVisible(TRUE)`; slots older
   than `--trail-ms` get `SetVisible(FALSE)`; the head visual is drawn **only in
   `--hide-cursor` mode** (with the system cursor visible it is covered by the hardware
   cursor anyway);
4. **`Commit` only when something actually changed** — DWM reads the visual tree at its own
   composition instants.

That removes the "missing a composition" failure mode entirely: there is no deadline to
chase, so the worst case is a picture one tick old instead of one frame late.

Two behaviours are worth knowing about up front:

- **The overlay's pixels come entirely from DWM** (the window has no GDI content), so a
  lost D3D/DirectComposition device makes it fully transparent instead of visibly broken.
  That is detected and recovered in-process — see
  [`device-loss-recovery.md`](device-loss-recovery.md).
- **Nothing is drawn while the system cursor is hidden** (`CURSOR_SHOWING` clear), which is
  what games and fullscreen video do.

## Performance (reference system: 2560×1600 / 120 Hz / Radeon 780M / Windows 11)

- one `update`: **15–18 µs** mean, 0.1–0.4 ms peak (at ~1000 samples/s);
- **3.6 %** of one core while idle, **2.4 %** while moving; the idle part is almost entirely
  the two kernel calls used for tick pacing, and `--commit-hz 250` brings it to ~1 %;
- GPU work per frame = a handful of 32×32 sprite quads; no full-screen copy, no full-screen
  draw, no swapchain.

## Known limitations

See [`limitations.md`](limitations.md). In short: only the **primary monitor** is drawn on;
display topology / DPI changes need a restart; the commit cadence is set by `--commit-hz` alone
(it is not aligned to DWM's composition phase, so the worst case is a picture one tick old); the
ring size caps how many samples can be visible at once (the oldest are dropped); `--trail-ms` is
a time window, not "one frame of movement"; a lost device needs a few hundred milliseconds to
rebuild.
