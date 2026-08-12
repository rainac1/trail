# Trail

A Windows full-screen transparent overlay that renders a **sub-frame cursor trail**
with C++ + Direct2D: it captures every cursor sample within the past frame and draws
the pointer texture at each position on the next frame. Each sample is shown for
exactly one frame before disappearing, so the trail length ≈ the cursor's movement
over one frame.

## Building

### Prerequisites

- Windows 10/11 (desktop composition / DWM must be enabled)
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

### Troubleshooting

- **`[ERROR] Visual Studio C++ toolchain not found`**: Visual Studio or the C++
  workload is missing. Open the Visual Studio Installer, add "Desktop development
  with C++", and retry.
- **`cmake is not recognized`**: CMake is not installed or not on `PATH`.
- **Build errors for `d2d1.h` / `d3d11.h` / `dcomp.h`**: build from the `vcvarsall x64`
  (or VS Developer Command Prompt) environment; `build.bat` does this automatically.
- **`The build directory is incompatible with the generator`**: `build/` was
  configured with a different generator before — delete `build/` and rebuild.
- Build/runtime issues are logged to `trail.log` next to the exe (see "Known
  limitations").

## Running

```bat
build\trail.exe                # default 1000 Hz sampling
build\trail.exe --sample-ms 2  # 500 Hz sampling (lower CPU)
build\trail.exe --hide-cursor  # also hide the system cursor (inside the overlay)
```

- **Quit**: `Ctrl+Alt+Q`
- The window covers the entire virtual desktop (multi-monitor), is click-through,
  and never steals focus.

## How it works

### Thread model (two threads)

| Thread | Responsibility |
|---|---|
| Main thread | Creates the window, runs the message loop, renders. `Present(1, 0)` blocks on vsync; nearly zero CPU when idle. |
| Sampler thread | `THREAD_PRIORITY_HIGHEST`; polls `GetCursorPos` every `--sample-ms` (default 1 ms ≈ 1000 Hz) and pushes `(QPC timestamp, x, y)` into a lock-free SPSC ring buffer. |

The sampler and render threads synchronize through a **lock-free single-producer /
single-consumer ring buffer** (`src/ring_buffer.h`) using release/acquire atomic
pairs for visibility — no locks, no dynamic allocation on the hot path.

### Sub-frame trail semantics

The render thread records the previous frame's timestamp `T_prev`, and each frame
draws only the samples in `t ∈ [T_prev, T_now)`. Because the frame windows tile the
timeline seamlessly, **each sample is drawn exactly once**: the newest sample appears
on the frame after it was sampled, then slides out of the window and disappears. The
trail consists of the positions the cursor passed through during the last frame —
render frame rate caps the trail "age", sampling rate determines path accuracy
(`RenderOneFrame` in `src/main.cpp`).

As a safety net, the window start is clamped to `now - one refresh period`, so the
trail never stretches beyond one frame even if a frame misses its vsync (older
samples are dropped instead of being drawn all at once).

### Pointer texture (Windows API)

1. `GetCursorInfo` fetches the current cursor handle `hCursor` and visibility;
2. `CopyIcon` + `GetIconInfo` yield the cursor bitmap and hotspot;
3. `GetDIBits` reads 32 bpp BGRA pixels, which are alpha-premultiplied;
4. cached as an `ID2D1Bitmap` (premultiplied) keyed by `hCursor` — **zero per-frame
   cost while the handle is unchanged** (one `GetCursorInfo` per frame, ≈ 1 µs).
   Animated cursors grab the current frame; legacy mask-only cursors fall back to
   AND/XOR compositing.

### Low-latency rendering (vblank-front alignment)

Enabled by default; compresses the head-vs-system-cursor latency from about one frame
down to a few milliseconds:

- **`DwmFlush` bootstrap calibration**: measures the composition refresh period and
  vsync phase (≈ 8.3 ms @ 120 Hz / 16.7 ms @ 60 Hz)
- **vblank-front alignment**: sleeps (`Sleep(1)`) until ~1 ms before
  `next_vsync - budget`, then busy-spins (`YieldProcessor`) for the final 1 ms so the
  render thread wakes *just before* the vsync deadline, then `Present(0)` lands the
  frame on the *current* vsync instead of the *next* one (the old `Present(1,0)`
  waited half a frame or more on average)
- **No catch-up stall after a miss**: if rendering starts after its target vsync, the
  loop does *not* idle-wait for the following vsync — it renders immediately and
  re-anchors the phase, so one missed vsync doesn't stretch the next frame's interval
  into two periods (which would otherwise pile two frames of trail into one frame)
- **Live head point, sampled late**: `GetCursorPos` is called at the last moment —
  after the historical trail is drawn and `EndDraw`'d, just before `CopyResource` —
  and the head point is drawn in a second `BeginDraw/EndDraw` pass. Head latency ≈
  the render budget (copy + present), not the whole draw
- **Adaptive budget**: render-time EMA + 1.0 ms margin, clamped to [1, 8] ms,
  tightening automatically when rendering is fast
- On calibration failure (no DWM composition) it falls back to `Present(1,0)`; the
  log prints `missed` (fraction of frames whose render overran the target vsync,
  ~0.2 % measured) and budget stats every 3000 frames
- `IDXGISwapChain2::SetMaximumFrameLatency(1)` caps the DWM composition queue depth

### Transparent rendering (DirectComposition + hardware GPU single path)

- D2D renders into a self-owned premultiplied offscreen bitmap → per-frame GPU
  `CopyResource` into a flip-model composition swapchain → `IDCompositionVisual::SetContent`
  composites it in DWM with premultiplied alpha, throttled by vsync
- Offscreen bitmap content persists across frames, so each frame **clears only the
  bounding box of the previous frame's trail** (dirty-rect clear) instead of a full-
  screen clear — a large GPU saving at high resolutions
- Window styles: `WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW
  | WS_EX_LAYERED`, plus `SetLayeredWindowAttributes(alpha=255)`
- **Click-through**: `WS_EX_LAYERED | WS_EX_TRANSPARENT` makes the whole window
  transparent to mouse hit-testing (per Microsoft's window-features docs: layered
  window hit-testing is shape/alpha based, and `WS_EX_TRANSPARENT` then ignores the
  shape and forwards mouse events to windows below; DComp permits a layered target
  window). Note that `WM_NCHITTEST → HTTRANSPARENT` only forwards to same-thread
  sibling windows, and `WS_EX_TRANSPARENT` alone has no hit-test effect — neither is
  a correct cross-process click-through mechanism
- Hardware D3D11 device only, **no WARP software fallback**: on failure it shows an
  error and writes to the log
- Why DirectComposition instead of flip+Hwnd: some display stacks (e.g. AMD Radeon
  780M iGPU) return `DXGI_ERROR_INVALID_CALL`/`E_INVALIDARG` from
  `CreateSwapChainForHwnd`/`CreateBitmapFromDxgiSurface` with premultiplied alpha,
  whereas `CreateSwapChainForComposition` + offscreen blit works everywhere and is
  the only reliable hardware transparent path in this scenario
- Every frame clears to transparent (dirty-rect) and draws each sample with
  `DrawBitmap` (`NEAREST_NEIGHBOR` — 1:1 sharp and cheapest)

## Performance notes

- Zero-allocation render hot path: samples are collected into a fixed stack array,
  and the cursor texture is captured only when its shape changes
- Lock-free SPSC ring buffer: `Capacity = 4096`, tolerating ~4 s of render stall at
  1000 Hz without dropping order
- Sampler thread uses `timeBeginPeriod(1)` for timer precision; high priority but not
  `TIME_CRITICAL`, to avoid preempting DWM / game threads
- The main loop is naturally throttled by vsync; idle CPU usage is very low

## Known limitations

- D3D device loss is not handled (`D2DERR_RECREATE_TARGET` is skipped; rare)
- With mixed-DPI multi-monitor setups the cross-screen window is scaled by DWM and
  trail coordinates can be offset; single-DPI is recommended
- `--hide-cursor` hides the cursor only inside the overlay (which covers the whole
  desktop, so effectively global)
- When a cursor handle is reused by the system (same shape), the cache hits and the
  texture stays correct
- Requires a D3D11-hardware-accelerated GPU and enabled desktop composition (DWM);
  the program refuses to start without a hardware device (no WARP downgrade)
- On initialization failure, a `MessageBox` and `trail.log` (next to the exe) show the
  failing step and HRESULT; set `TRAIL_NO_UI=1` to suppress the dialog (the legacy
  `SUBFRAME_NO_UI` name is still accepted)
- The trail head still lags the system cursor by roughly one render budget (a few
  milliseconds — the physical floor of sample → DWM composite), plus the sampler
  thread's quantization (1 ms by default)
