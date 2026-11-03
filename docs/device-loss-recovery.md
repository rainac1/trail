# Device-loss recovery and overlay health watchdog

**Symptom**: the overlay's rendering suddenly disappears — the window is still there
(click-through, on top, invisible) but nothing is drawn any more, and the trail never comes
back until the process is restarted.

## Why it is invisible instead of broken

The window has **no GDI content**: every visible pixel comes from the DWM-composited
DirectComposition visual tree (see [architecture.md](architecture.md)). When the underlying
D3D/DirectComposition device is lost, there is nothing left for DWM to composite — the overlay
becomes fully transparent rather than showing a frozen or garbled image. The visual tree, the
sprite surface and the D3D device are all tied to that device, so every later call fails and
the process cannot recover on its own.

## Trigger conditions

Any event that removes or resets the D3D device, or invalidates the associated
DirectComposition device:

- GPU driver reset / hang recovery (TDR), driver update or reinstall;
- display-mode change (resolution, refresh rate), monitor hot-plug or power-off/on;
- system sleep/resume;
- driver power-management transitions;
- any other device removal.

## Detection

Three independent sources, all on the main thread:

1. **`WM_PAINT`** — per Microsoft's documentation for `IDCompositionDevice::CheckDeviceState`,
   DirectComposition sends `WM_PAINT` to all windows composing content with a lost device.
   `WndProc` validates the update region and calls `OverlayRenderer::DeviceValid()`; an invalid
   device flags a rebuild.
2. **Commit error classification** — `OverlayRenderer::Update` inspects every `Commit` result
   and returns `FrameResult::RecreateDevice` for `DXGI_ERROR_DEVICE_REMOVED`,
   `DXGI_ERROR_DEVICE_RESET`, `DXGI_ERROR_DEVICE_HUNG` and
   `DXGI_ERROR_DRIVER_INTERNAL_ERROR`. Non-fatal failures are logged once (not once per tick)
   and do not trigger a rebuild.
3. **Periodic health check** (every 500 ms) — re-checks `DeviceValid()` and the window's
   topmost style. This covers losses whose `WM_PAINT` never reaches us (e.g. a window that is
   not being composited at that moment).

### Why `GetDeviceRemovedReason` and not `CheckDeviceState`

`CheckDeviceState` lives on the base `IDCompositionDevice`. The renderer needs
`IDCompositionVisual3` (for `SetVisible`), which requires creating the composition device with
`DCompositionCreateDevice3`; that returns `IDCompositionDesktopDevice` (`IDCompositionDevice2`
lineage), which does **not** expose `CheckDeviceState`. The D3D device is what backs the
composition device, so `ID3D11Device::GetDeviceRemovedReason()` is used as the validity signal,
and a `Commit` that reports a device-lost error is the second signal.

## Recovery

`InitRenderer` rebuilds the **entire** render stack in-process on the existing window — new
D3D11 device, new DirectComposition device (`DCompositionCreateDevice3`), new target, root,
sprite visuals and sprite surface — then lets the next tick re-capture the cursor sprite (the
old surface belongs to the destroyed device) and resumes updating. Logged as `[recover]`.

- The old stack is torn down in a defined order (root → target → surface → DComp device → D3D)
  so DWM never keeps a reference to a released surface.
- While the renderer is down nothing is drawn: the tick loop sees `ready() == false` and the
  recovery path owns the loop.
- A rebuild can fail — a GPU is often unusable for a moment after a reset — so it is retried
  with an exponential backoff (0.5 s → 1 → 2 → 4 → 8 → 15 s, reset to 0.5 s after a success).
  The loop keeps pumping messages between attempts and sleeps at most 50 ms at a time, so the
  quit hotkey stays responsive.
- Nothing needs re-calibrating after a rebuild: there is no timing state to restore, and the
  next tick re-captures the cursor sprite and re-establishes the mouse-history watermark.

## Health watchdog

The overlay is only visible while it is both composited and on top, so the same 500 ms check
also fixes the non-device way it can "disappear":

- **topmost style lost** (`WS_EX_TOPMOST` cleared by anything) → re-assert it; a window buried
  under other windows looks identical to a rendering failure. It acts only when the style is
  actually missing, so it does not fight other topmost windows for z-order.

Display topology and DPI are deliberately **not** followed (the window is built once for the
primary monitor at startup); a change only logs `restart trail to adapt`.

## Observability

Everything above is logged (see [diagnostics.md](diagnostics.md)):

| Line | Meaning |
|---|---|
| `[recover] rebuilding after device loss` | device loss detected, rebuild starting |
| `[recover] renderer rebuilt` | rebuild succeeded, updates resumed |
| `[recover] rebuild failed (will retry in N ms)` | rebuild failed, backing off |
| `[watch] WS_EX_TOPMOST lost -> re-asserting` | window was no longer topmost |
| `[watch] display topology changed …` / `[watch] display DPI changed …` | display changed while running; restart to adapt |
| `[OverlayRenderer] device removed: 0x…` | `GetDeviceRemovedReason()` reported a failure |
| `[OverlayRenderer] Commit (device lost) failed: 0x…` | a commit reported device removal/reset/hang |
