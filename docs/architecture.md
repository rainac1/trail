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

## Low-latency rendering (absolute composition-clock deadline)

The goal is to submit each frame **just before the DWM composition that will display it**,
so the head of the trail is only the render budget (a fraction of a millisecond) behind
the system cursor instead of a full refresh period. Two mechanisms do all the work, and
there is **no fallback path** — if either is unavailable the program reports the error and
exits (see [limitations.md](limitations.md)).

**A. Read the composition clock instead of sampling it.** `DwmGetCompositionTimingInfo(NULL, &ti)`
returns the authoritative values directly, in QPC units:

- `qpcRefreshPeriod` — the composition refresh period (e.g. ≈ 8.33 ms @ 120 Hz);
- `qpcCompose` / `qpcVBlank` — the composition / vertical-blank timestamp on that lattice
  (identical on the tested system, and reported as the *upcoming* one);
- `rateRefresh` — the refresh rate as a ratio (diagnostics only).

The deadline is therefore an **absolute lattice**: `next = qpcCompose + k·qpcRefreshPeriod`,
advanced until `next − lead > now`, and the loop waits for `next − lead`. Because the
lattice is re-derived from the clock every frame, phase error cannot accumulate. That
replaces the whole earlier machinery — 11 blocking `DwmFlush` calls at startup, 5 more
every 1500 frames (which dropped a handful of frames each time), a period EMA, and a
free-running phase prediction that could drift for up to 1500 frames between re-anchors.

The `lead` is a **slow requirement baseline plus a fixed margin**. What has to fit inside
`lead` is `needMs = t1 − target` — wake-up jitter **plus** render time, not render time alone.
`needMs` is heavy-tailed, but its two halves behave very differently: measured across five
sessions the **mean is stable at 0.51–0.58 ms**, while single-frame peaks reach 1.4–5.7 ms (OS
scheduling stalls). The render cost is therefore the part that needs adapting, and the stalls
are the part that needs a fixed allowance:

    lead = slow baseline (τ ≈ 2000 frames) + kLeadMarginMs (0.6 ms)

The margin is not a fudge factor but a property of the scheduler, which does not change with
the resolution — so it is fixed and is the one knob traded against the miss rate. The baseline
is deliberately slow: a single 4 ms stall moves it by only `0.0005 × 3.5 ≈ 0.002 ms`, while a
genuinely sustained change (a resolution or device change) is still followed within seconds.

Choosing the margin does not have to be iterative. Since a miss is exactly the event
`needMs > baseline + margin`, the `[clock] margin probe` line counts how often
`needMs − baseline` exceeds a set of candidate margins, so **each number on that line is the
miss rate that margin would produce**. One run yields the whole curve.

The earlier version chased the *instantaneous* `needMs` instead. That put the downward target
around 0.7 ms — far below the ~1.5 ms needed to keep the miss rate low — and turned the scheme
into a relaxation oscillator: `lead` sawed between 1.16 ms and 2.15 ms, gliding slowly down and
then being knocked back up by two spikes. A deadband stopped the sawing but the operating point
drifted up to ~2.1 ms, because the margin was doing double duty as the attack threshold. With a
smooth target both problems go away, and the up/down rates stop mattering for stability.

Startup is handled separately. The first few hundred frames of a session are a cold-start
transient — the largest stall of a session consistently lands there, at 4–6 ms, versus
1.3–3.1 ms later — so the first `600` frames are not sampled at all (`lead` stays at the
measured-good 2.0 ms default), the next `120` samples seed the baseline with an exact running
mean so that one spike is diluted, and only then does the slow EMA take over. An earlier version
seeded the baseline from its very first sample, and a single 4.19 ms cold-start spike then held
the operating point 0.8 ms too high for about 5000 frames (measured `baseline` = 1.33 ms at
frame 3000, decaying exactly as `0.53 + 3.66·e^(−3000/2000)` predicts).

Measured on the reference system (120 Hz, 2560×1600, AMD Radeon 780M), against the policies it
replaced:

| `lead` policy | head latency (`slack` mean) | miss rate |
|---|---|---|
| fixed 2.0 ms | 1.47 ms | 0.050 % (9/18 000) |
| instant-`need` envelope | 0.87 ms | 0.125 % (15/12 000) |
| the same envelope plus a deadband | 1.41 ms | 0.010 % (1/9940) † |
| one-shot 5 s calibration | 1.0–4.1 ms across runs | 0.035–0.05 % |

† Not statistically distinguishable from the 0.050 % row — the 95 % CI of 1/9940 is
0.0003–0.056 %. That, plus the deadband failing to actually bind, is why the scheme was
simplified to "baseline + margin" instead of being tuned further.

A one-shot calibration is not used because its window sits on the cold-start transient: three
runs of the same 5 s window produced `lead` = 1.43 / 1.72 / 4.90 ms on unchanged hardware, and a
"second largest" trim survives only one such sample.

### Choosing the margin

Because the miss condition is exactly `needMs > baseline + margin`, the margin can be chosen from
measurement instead of by trial. The `[clock] margin probe` line counts how often
`needMs − baseline` exceeds each candidate margin, so **every number on that line is the miss
rate that margin would produce**. Across 12 849 measured frames:

| margin (= mean head latency) | miss rate | worst 25 s window |
|---|---|---|
| 1.20 ms | 0.062 % | — |
| **0.90 ms** (shipped) | **0.23 %** | 0.43 % |
| 0.75 ms | 0.40 % | ~0.9 % |
| 0.60 ms | 0.69 % | 1.70 % |
| 0.45 ms | 1.35 % | ~3 % |
| 0.30 ms | 3.23 % | — |

Note the identity **mean head latency ≈ margin**: `slack = lead − need`, and the baseline is the
mean of `need`, so the two cancel. The left column is therefore directly readable as the average
latency the trail head carries.

Two properties of this curve matter when picking a value. It is **steep** — every 0.15 ms costs
roughly a doubling of the miss rate, so there is no flat region in which "a little less" is free.
And the tail is **non-stationary**: at margin 0.60 one session's five 25 s windows measured
0.83 / 1.70 / 0.37 / 0.10 / 0.32 % — a 17× spread, with the bad windows correlating with machine
activity (`wake max` 1083 µs versus ~500 µs in the good ones). For a metric about visible
glitches the worst window matters more than the mean, which is why the shipped value sits at
0.90 ms rather than at the aggressive end of the curve.

`lead` is clamped to [1, 8] ms and to 60 % of the period, computed per frame because the
period is re-read.

**B. Wait with a high-resolution timer plus a short spin.** The coarse wait is
`SetWaitableTimerEx` on a timer created with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`
(Windows 10 1803+), in ≤ 2 ms slices, and the final 0.5 ms is a `YieldProcessor` spin.
This replaces `timeBeginPeriod(1)` + `Sleep(1)` polling + a 2 ms spin:

- no global timer-resolution change (which affects the whole system, not just this
  process);
- the spin window shrinks from 2 ms to 0.5 ms, roughly halving the render thread's CPU;
- the ≤ 2 ms slices matter: a single long sleep (e.g. 7 ms) lets the CPU enter a deeper
  idle state, and the observed wake-up latency then occasionally reached 4–7 ms — enough
  to blow through the lead margin and miss a composition. Slicing removed those tail
  events (measured; see [diagnostics.md](diagnostics.md) for the log fields).

Other latency-relevant decisions:

- **Live head point, sampled late**: `GetCursorInfo` is called a second time as late as
  possible — after the historical trail has been drawn and `EndDraw`'d, just before the
  `CopyResource` — and the head point is drawn in a second `BeginDraw`/`EndDraw` pass.
  Head latency is therefore ≈ the render budget (copy + present), not the whole draw.
- **`Present(0)`, never `Present(1,0)`**: blocking present would hand the timing back to
  DWM and add about a frame.
- **High-priority render thread**: the main thread is raised to `THREAD_PRIORITY_HIGHEST`
  (deliberately not `TIME_CRITICAL`, which would preempt DWM/game threads) so the render
  is less likely to be preempted past its deadline.
- **One render per composition slot**: the deadline must be strictly in the future
  (`next − lead > now`). Requiring only `next > now` makes the target fall into the past
  when `now` lands inside the last `lead` of a slot, and the loop then renders several
  times per displayed frame — which, given that each render consumes one mouse-history
  watermark, silently shortens the visible trail to a fraction of a frame's movement.

## Performance characteristics

- Zero-allocation render hot path: samples go into a fixed stack array
  (`GetMouseMovePointsEx` writes into a stack `MOUSEMOVEPOINT[64]`), and the cursor
  texture is captured only when its shape changes.
- One `GetCursorInfo` + one `GetMouseMovePointsEx` + one `DwmGetCompositionTimingInfo`
  per frame; no background thread, no locks, no dynamic allocation in the frame loop.
  The clock query is cheap: measured ≈ 10 µs mean, with worst cases of a few hundred µs.
- The system mouse-move history is a single fixed 64-point buffer shared across all
  threads and processes; movement between two frames is bounded by the mouse report rate
  (typically ≤ 1000 Hz), well below 64 points per frame at normal refresh rates.
- Health checks (topmost style, virtual-screen geometry, DirectComposition device
  validity) run every 240 frames — a handful of `Get*` calls, no measurable cost.
- Measured on the reference system at 120 Hz: 119.9 fps, ~8 % of one core (versus ~15 % for
  the `DwmFlush` + `timeBeginPeriod` scheme it replaced, at the same frame rate and the same
  rendering time). Frames finishing after their target composition: 0.0–0.1 % (an
  18 000-frame session at a fixed 2.0 ms lead recorded 9 misses, i.e. 0.05 %).
