# Known limitations

- **Device loss is recovered, not prevented.** A lost DXGI/DirectComposition device
  (driver reset, driver update, display-mode change, monitor hot-plug, sleep/resume,
  power-management transition) makes the overlay transparent until the render stack is
  rebuilt. That rebuild is automatic now (a few frames, logged as `[recover]`), but
  anything drawn in that window is gone — see
  [device-loss-recovery.md](device-loss-recovery.md).
- **The watchdog restores the topmost *style*, not z-order against a window above us.**
  If another always-on-top window (or a fullscreen-exclusive app) sits above the overlay,
  the trail is drawn but not visible, and no amount of re-asserting `WS_EX_TOPMOST` fixes
  that.
- **The render loop is single-threaded, so a blocking `DwmFlush` blocks the message
  loop.** If DWM stops composing (fullscreen-exclusive app, display asleep, secure
  desktop), `DwmFlush` calls in the low-latency path can stall the loop, and the quit
  hotkey stops responding until composition resumes. This is a deliberate trade: no
  second thread, no locks, no cross-thread latency.
- **Mixed-DPI multi-monitor**: the cross-screen window is scaled by DWM, so trail
  coordinates can be offset; single-DPI is recommended.
- **`--hide-cursor`** hides the cursor only inside the overlay window — which covers the
  whole desktop, so it is effectively global.
- **Cursor textures are cached by handle.** When the system reuses a handle with the same
  shape the cache hits and the texture stays correct; an animated cursor shows whichever
  frame was current when it was captured.
- **Hardware only**: requires a D3D11 hardware-accelerated GPU and enabled desktop
  composition (DWM). The program refuses to start without a hardware device (no WARP
  downgrade).
- **Initialization failures** are reported by a message box and `trail.log` next to the
  exe, with the failing step and HRESULT; set `TRAIL_NO_UI=1` to suppress the dialog (the
  legacy `SUBFRAME_NO_UI` name is still accepted).
- **The trail head still lags the system cursor** by roughly one render budget (a few
  milliseconds in the low-latency path, about one frame in the `Present(1,0)` fallback) —
  the physical floor of sample → DWM composite.
- **Trail path resolution is bounded by the system mouse report rate** (typically
  125–1000 Hz) rather than by an explicit sampling interval; extremely fast flicks can
  still be under-sampled.
- **The system mouse history stores coordinates in 16-bit form**, so virtual-desktop
  coordinates are limited to ±32767; layouts wider/taller than 32768 px can mis-track (a
  rare, multi-8K-monitor edge case).
