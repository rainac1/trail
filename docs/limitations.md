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
- **There is no fallback path by design.** The composition clock
  (`DwmGetCompositionTimingInfo`) and the high-resolution waitable timer are the only
  timing mechanisms: if either is unavailable — DWM composition off, an implausible
  refresh period, or a Windows build older than 10 1803 for
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` — the program logs the reason and exits with
  code 1 instead of degrading to a slower (but working) presentation mode. `Present(1,0)`
  and `Sleep`-based polling are deliberately absent.
- **One composition clock for the whole desktop.** Timing is derived from the single
  global DWM composition clock, while the window spans every monitor. With outputs at
  different refresh rates, or with VRR (where the period is not constant), no single
  period/phase fits all outputs; the deadline follows the composition cadence the OS
  reports.
- **A small fraction of frames still misses its composition** (~0.1 % on the reference
  system at 120 Hz), each time by a few milliseconds of OS scheduling stall — a tail no
  estimator can predict, because what causes it is the scheduler, not the render cost. A
  missed frame is displayed one refresh later. Ordinary jitter is absorbed automatically
  (see [architecture.md](architecture.md)); the `[clock]` log line exposes `need max`,
  `wake max` and `slack min` to check it.
- **The baseline follows a real change in render cost slowly (τ ≈ 2000 frames ≈ 17 s).** That
  is what makes it ignore individual stalls, and the cost is the adaptation time: after a
  resolution or GPU-load change, `lead` is briefly too low (a few missed frames) or briefly
  generous. The former is bounded because `lead` never drops below `baseline + margin` and the
  latter is the safe direction.
- **One `kLeadMarginMs` sets both the head latency and the miss rate**, and there is no feedback
  loop that tunes it automatically. `lead = baseline + margin` and the mean head latency equals
  the margin, so the two move together one-for-one; picking it is a manual trade read off the
  `margin probe` line (see [architecture.md](architecture.md) for the measured curve). It is
  deliberately not adaptive: the stalls it covers are a property of the OS scheduler, not of the
  render cost, so there is nothing for it to adapt to.
- **The `need` tail is non-stationary, so a miss rate is only an average.** At margin 0.60 one
  session's five 25 s windows spanned 0.10–1.70 % — a 17× spread driven by whatever else the
  machine was doing (`wake max` 1083 µs in the bad windows versus ~500 µs in the good ones). A
  margin tuned on one session's average can therefore be several times worse in a bad window;
  size it against the worst window you are willing to accept, not the mean.
- **The startup warm-up spends ~5 s at the 2.0 ms default** before any of the measurements
  above apply, and the following ~1 s seeds the baseline. A short session therefore never
  leaves that default; the `baseline=0.00` field in the `[clock]` line tells you the warm-up is
  still running.
- **A session's largest stall is a cold-start artifact.** Every session measured so far peaked
  at 4–6 ms within its first 600 frames, against 1.3–3.1 ms later. Anything that tries to size
  `lead` from early measurements will over-estimate it, which is why the warm-up exists.
- **A skipped composition lengthens the trail for one frame**: if a slot is missed, the
  next displayed frame carries the mouse movement of two refresh periods.
- **The render loop is single-threaded**, so anything that blocks it (a blocking `Present`
  when DWM stops consuming frames, a display asleep, the secure desktop) also stops the
  message loop and the quit hotkey until composition resumes. This is a deliberate trade:
  no second thread, no locks, no cross-thread latency.
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
