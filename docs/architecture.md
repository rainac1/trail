# Architecture

How Trail works and why it is built this way.

- The failure mode where the overlay goes fully transparent ("rendering suddenly
  disappeared"): [device-loss-recovery.md](device-loss-recovery.md)
- Log lines and triage: [diagnostics.md](diagnostics.md)
- Known limitations: [limitations.md](limitations.md)

## Overview

Trail is a click-through, always-on-top transparent overlay that draws a **sub-frame cursor
trail** on the **primary monitor**: every cursor movement sample recorded during the previous
moment is drawn at its own position, and each sample survives `--trail-ms` milliseconds. The
trail length is the cursor's movement over that time window, and the path resolution is the
system mouse report rate.

The implementation is deliberately **not** a renderer. The trail is not painted into a
texture; it lives in the DirectComposition **visual tree** as a set of small sprite visuals
that all share one cursor sprite. A sample becomes visible by setting its offset and
`SetVisible(TRUE)`; it disappears by `SetVisible(FALSE)`. There is no clear, no blit, no
swapchain and no `Present` on the hot path.

## One window, primary monitor only

There is exactly one window, covering the primary monitor's physical-pixel rect. It is created
hidden, the visual tree is attached, and only then is it shown (an empty layered window can
show a black surface).

Only the primary monitor is served: samples outside its rect are dropped, so they cannot
occupy ring slots. A per-monitor design (one window per monitor) was implemented and then
removed — with several full-screen layered windows DWM's composition got measurably slower for
the whole desktop, especially while a video played. If the display topology or DPI changes,
restart the program.

## Thread model (single thread)

One thread creates the window, runs the message loop and ticks at `--commit-hz`. There is no
sampler thread, no shared buffer and no lock.

## Trail semantics

- Every tick, `GetCursorInfo` returns the cursor position and `CollectMouseHistory`
  (`src/mouse_history.cpp`) pulls the movement samples added since the previous tick out of the
  system's own 64-point mouse-move history (`GetMouseMovePointsEx`). The OS records that
  history for us; the tick loop only keeps a watermark of the newest consumed point. The
  history query is skipped entirely while the cursor has not moved, because it is the most
  expensive kernel round trip on the path.
- Each new sample is assigned to one **ring slot**, i.e. one visual. New samples reuse the
  oldest slot, so `--sprites` is both the ring size and the cap on concurrently visible
  samples.
- A slot is hidden when `now − born > --trail-ms`. A sample therefore lives in a time window
  rather than "for one frame"; the visible trail length is `trail-ms` of movement at any
  refresh rate.
- The **head** visual follows the cursor position and is refreshed on every tick, but it is
  drawn only in `--hide-cursor` mode: with the system cursor visible it would be covered by the
  hardware cursor anyway.
- If the system cursor is hidden (`CURSOR_SHOWING` clear), everything is hidden.

## Drawing order (z-order)

`IDCompositionVisual::AddVisual` has a counter-intuitive special case: when `referenceVisual` is
`NULL`, `insertAbove = TRUE` places the new child at the **bottom** ("above no sibling,
therefore rendered below all of its siblings"), while `FALSE` places it at the **top**. The
slots and the head are therefore created with `FALSE`, so a higher-numbered slot is higher in
the stack; the head is added last and stays on top.

Because the ring assigns new samples to increasing slot indices, a contiguous run of live slots
has "larger index = newer = higher", which is the wanted order. Only when the live run crosses
the ring wrap point does the order invert for that moment; at real sample densities (1–3 px
apart) that is not visible. Keeping the order exact at every instant would require moving the
assigned visual in the tree on every sample (measured +3–5 µs per sample) and was not worth it.

## Sprite (cursor texture) upload

1. `GetCursorInfo` supplies the cursor handle; the texture is re-captured only when it changes.
2. `CaptureCursorBitmap` (`src/cursor_texture.cpp`) uses `CopyIcon` + `GetIconInfo` +
   `GetDIBits` to read the 32 bpp BGRA pixels, premultiplies the alpha, and falls back to
   AND/XOR compositing of the two halves of the mono mask for legacy mask-only cursors.
3. The bitmap is scaled to the cursor's physical size at the window's DPI
   (`GetSystemMetricsForDpi(SM_CXCURSOR, dpi)`): `GetCursorInfo` returns the resource's native
   image while the system draws the cursor at the DPI-scaled size, so an unscaled sprite would
   be too small on a high-DPI monitor.
4. The result is uploaded into one small `IDCompositionSurface` (`CreateSurface`,
   `DXGI_FORMAT_B8G8R8A8_UNORM`, `DXGI_ALPHA_MODE_PREMULTIPLIED`) via `BeginDraw` →
   `UpdateSubresource` into a scratch texture → `CopySubresourceRegion` → `EndDraw`. Every
   sprite visual and the head reference that one surface, so a shape change costs one upload,
   not one per sprite.

## Commit cadence and the latency model

There is no composition-clock read and no deadline on the hot path. The loop ticks at
`--commit-hz`, and a tick that produced no change does not `Commit` at all. DWM reads the
visual tree at its own composition instants, so:

- **head freshness at display ≈ half a tick** (a uniformly distributed phase between the last
  commit and the composition that displays it), plus the commit's own latency;
- **being late is not a "missed frame"**: no `Present` is rejected, so a commit that lands
  after the composition simply leaves the previous state on screen for one more tick. The
  failure mode is "the picture is one tick old", not "the trail is a whole refresh stale".

`--trail-ms` and `--commit-hz` are independent: the former is the look (how much motion the
trail spans), the latter is the freshness. A long trail can still have a fresh head.
`[update]` reports the `Commit` time separately, which is the first number to look at if the
trail starts lagging — a video or a full-screen app can make DWM stop consuming commits, and
`Commit` then blocks the loop.

## Performance characteristics

- Zero pixel work per tick: a tick with one new sample is two `SetOffset` calls, one
  `SetVisible`, one head update and one `Commit`.
- Measured on the reference system: one `update` averages **~15 µs** at ~1000 samples/s; the
  idle CPU cost is dominated by the two kernel calls used for tick pacing
  (`SetWaitableTimerEx` + `WaitForSingleObject`), not by the update.
- One `GetCursorInfo` per tick plus one `GetMouseMovePointsEx` per movement.
- Health checks (topmost style, device validity) run every 500 ms.
