# Architecture

How Trail renders and why it is built this way.

- The failure mode where the overlay goes fully transparent ("rendering suddenly
  disappeared"): [device-loss-recovery.md](device-loss-recovery.md)
- Log lines and triage: [diagnostics.md](diagnostics.md)
- Known limitations: [limitations.md](limitations.md)

## Overview

Trail is a full-screen, click-through, always-on-top transparent overlay that draws a
**sub-frame cursor trail**: it draws every cursor movement sample recorded during the
previous frame at its own position, for exactly one frame. The trail length therefore
equals the cursor's movement over one frame, and the path resolution equals the system
mouse report rate.

One thread does everything (window, message loop, rendering). There is no sampler
thread, no shared buffer, no lock and no per-frame allocation on the hot path.

## Thread model (single thread)

The main thread creates the window, runs the message loop and renders. Instead of
polling `GetCursorPos` on a high-priority background thread, once per frame the render
thread calls `GetMouseMovePointsEx` on demand and pulls the movement points it has not
drawn yet out of the system's own 64-point mouse-move history — the OS records the raw
movement history for us.

## Sub-frame trail semantics

`GetMouseMovePointsEx` returns the anchor point and up to 63 points *before* it (newest
first) and does **not** consume the history, so the render thread keeps an
`(x, y, time)` watermark of the newest point it has already drawn. Each frame it reads
the whole history, keeps only the prefix newer than the watermark, then advances the
watermark. Every movement point is therefore drawn exactly once — on the frame after it
happened — and disappears on the next frame.

`CollectMouseHistory` (`src/mouse_history.cpp`) handles the API's quirks:

- the 16-bit wrap-around of negative multi-monitor coordinates (the anchor must be
  masked to 16 bits or it never matches on displays left of / above the primary);
- the `-1` "anchor not found" case (the watermark was pushed out of the 64-point window
  after a long stall) and the `buf[0] != cur` mismatch case after which the frame simply
  drops the historical part of the trail and self-heals on the next frame;
- the newest-first output order (reversed into oldest-first for drawing);
- the first frame only establishes the watermark, so a startup does not draw up to 64
  stale points.

If more than 64 raw moves occur between two frames, the middle of the trail is dropped:
the system only keeps 64 points.

## Pointer texture capture

1. `GetCursorInfo` fetches the current cursor handle `hCursor` and its visibility;
2. `CopyIcon` + `GetIconInfo` yield the cursor bitmap and the hotspot;
3. `GetDIBits` reads 32 bpp BGRA pixels, which are then alpha-premultiplied;
4. the result is cached as an `ID2D1Bitmap` keyed by `hCursor` — **zero per-frame cost
   while the handle is unchanged** (one `GetCursorInfo` per frame, ≈ 1 µs).

Animated cursors grab the current frame. Legacy mask-only cursors (no colour bitmap)
fall back to AND/XOR compositing of the two halves of the mono mask.

The cache is keyed by handle only, so a handle reused by the system with the same shape
keeps hitting the cache (correct), and any handle change forces a re-capture. The cache
is invalidated whenever the render stack is rebuilt — see
[device-loss-recovery.md](device-loss-recovery.md).

## Transparent rendering path

The window's pixels come **entirely** from DirectComposition; the window itself has no
GDI content (which is why device loss is fatal to visibility — see
[device-loss-recovery.md](device-loss-recovery.md)).

1. D2D renders into a self-owned premultiplied offscreen bitmap
   (`CreateBitmap` + `D2D1_BITMAP_OPTIONS_TARGET`).
2. Each frame blits that bitmap into the current back buffer of a **composition**
   swapchain with a GPU `CopyResource`. With `DXGI_SWAP_EFFECT_FLIP_DISCARD` the back
   buffers rotate, so `GetBuffer(0)` is re-fetched every frame (never cached).
3. `IDCompositionVisual::SetContent(swapChain)` hands the swapchain to DWM, which
   composites it with premultiplied alpha and throttles the queue.
4. `IDXGISwapChain2::SetMaximumFrameLatency(1)` caps the DWM composition queue depth so
   a submitted frame cannot pile up behind older ones.

Details that matter:

- **Dirty-rectangle clear**: the offscreen bitmap is a persistent target, so instead of a
  full-screen `Clear` each frame only the bounding box of the previous frame's trail is
  cleared (clipped). This is a large GPU saving at high resolutions.
- **Why DirectComposition instead of flip + `HWND`**: some display stacks return
  `DXGI_ERROR_INVALID_CALL` from `CreateSwapChainForHwnd` with
  `DXGI_ALPHA_MODE_PREMULTIPLIED`, and `CreateBitmapFromDxgiSurface` on a flip back
  buffer can return `E_INVALIDARG`; `CreateSwapChainForComposition` plus the offscreen
  blit works there and is the reliable hardware transparent path. The extra
  `CopyResource` is the price paid for that reliability.
- **Hardware D3D11 device only, no WARP fallback**: software rendering does not meet the
  latency goal, so initialization fails loudly (log + message box) instead of silently
  degrading.
- **Window styles**: `WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
  WS_EX_TOOLWINDOW | WS_EX_LAYERED`, plus `SetLayeredWindowAttributes(alpha=255)`.
- **Click-through**: `WS_EX_LAYERED | WS_EX_TRANSPARENT` is what makes the whole window
  transparent to mouse hit-testing: layered-window hit-testing is shape/alpha based, and
  `WS_EX_TRANSPARENT` makes the shape be ignored so events go to the windows below.
  Neither `WM_NCHITTEST → HTTRANSPARENT` (forwards only to same-thread siblings) nor
  `WS_EX_TRANSPARENT` alone is a correct cross-process click-through mechanism.
- **Bounding box bookkeeping**: the bbox of the current frame (historical points + live
  head) is remembered for the next frame's clear; a frame that draws nothing clears the
  previous bbox and remembers "nothing to clear".

## Low-latency rendering (vblank-front alignment)

Enabled by default. It compresses the head-vs-system-cursor latency from about one frame
down to a few milliseconds:

- **`DwmFlush` bootstrap calibration** measures the composition refresh period and the
  vsync phase (≈ 8.3 ms @ 120 Hz, 16.7 ms @ 60 Hz). The measurement is rejected when it
  is outside 3–70 ms, which is what happens when `DwmFlush` does not block at all
  (see [diagnostics.md](diagnostics.md) for the toolchain pitfall that causes this).
- **vblank-front alignment**: the loop sleeps (`Sleep(1)`) until ~2 ms before
  `next_vsync - budget`, then busy-spins (`YieldProcessor`) for the last 2 ms so the
  thread wakes *just before* the vsync deadline; `Present(0)` then lands the frame on
  the *current* vsync instead of the next one (`Present(1,0)` waits half a frame or more
  on average).
- **No catch-up stall after a miss**: if the render starts after its target vsync the
  loop renders immediately and re-anchors the phase instead of idling to the following
  vsync, so a single miss does not stretch the next frame's interval into two periods
  (which would pile two frames of trail into one frame).
- **Live head point, sampled late**: `GetCursorInfo` is called a second time as late as
  possible — after the historical trail has been drawn and `EndDraw`'d, just before the
  `CopyResource` — and the head point is drawn in a second `BeginDraw`/`EndDraw` pass.
  Head latency is therefore ≈ the render budget (copy + present), not the whole draw.
- **Adaptive budget**: render-time EMA + 1.0 ms margin, clamped to [1, 8] ms and to 60 %
  of the refresh period, so it tightens automatically when rendering is fast.
- **High-priority render thread**: the main thread is raised to `THREAD_PRIORITY_HIGHEST`
  (deliberately not `TIME_CRITICAL`, which would preempt DWM/game threads) so the
  busy-wait and the render are less likely to be preempted into a miss.
- **Periodic re-measurement**: every 1500 frames the refresh period is re-measured
  (`DwmFlush`, slow EMA) and the phase re-anchored, to follow VRR / display-mode /
  refresh-rate changes. A failed re-measure keeps the previous period and phase.
- **Fallback**: if calibration fails (no DWM composition, or `DwmFlush` not throttling)
  the loop uses blocking `Present(1,0)` with vsync as the throttle — correctness and
  trail semantics are unaffected, only the head latency grows by roughly one frame. The
  fallback path logs a `[frame] … frames rendered` heartbeat every 3000 frames.

## Performance characteristics

- Zero-allocation render hot path: samples go into a fixed stack array
  (`GetMouseMovePointsEx` writes into a stack `MOUSEMOVEPOINT[64]`), and the cursor
  texture is captured only when its shape changes.
- One `GetCursorInfo` + one `GetMouseMovePointsEx` per frame; no background thread, no
  locks, no dynamic allocation in the frame loop.
- The system mouse-move history is a single fixed 64-point buffer shared across all
  threads and processes; movement between two frames is bounded by the mouse report rate
  (typically ≤ 1000 Hz), well below 64 points per frame at normal refresh rates.
- Health checks (topmost style, virtual-screen geometry, DirectComposition device
  validity) run every 240 frames — a handful of `Get*` calls, no measurable cost.
