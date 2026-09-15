# Device-loss recovery and overlay health watchdog

**Symptom**: the overlay's rendering suddenly disappears — the window is still there
(click-through, on top, invisible) but nothing is drawn any more, and the trail never
comes back until the process is restarted.

## Why it is invisible instead of broken

The window has **no GDI content**: every visible pixel comes from the
DWM-composited DirectComposition visual tree (see
[architecture.md](architecture.md)). When the underlying DXGI / DirectComposition device
is lost, there is nothing left for DWM to composite — the overlay becomes fully
transparent rather than showing a frozen or garbled image. Because the swapchain, the
visual tree and the D2D context are all tied to that device, drawing calls and presents
keep failing and the process can never recover on its own. Restarting the process is the
only thing that used to help, because restarting re-creates the whole device stack.

That is exactly the failure mode the code used to leave unhandled: `D2DERR_RECREATE_TARGET`
was detected in `EndDraw` and then discarded (the `RenderFrame` result was not even
consulted), `IDCompositionDevice::CheckDeviceState` was never called, and no `Present` /
`CopyResource` result was checked.

## Trigger conditions

Any event that removes or resets the DXGI device, or invalidates the associated
DirectComposition device, produces this symptom:

- GPU driver reset / hang recovery (TDR), driver update or reinstall;
- display-mode change (resolution, refresh rate), monitor hot-plug or power-off/on;
- system sleep/resume;
- driver power-management transitions;
- any other DXGI device removal.

**This is handled generically.** The failure mode was not pinned to a specific GPU,
driver or platform, and no platform-specific workaround is applied: any device loss is
detected and recovered the same way, whatever caused it. If a platform-specific trigger
is ever identified, record it here.

## Detection

Three independent sources, all on the main thread:

1. **`WM_PAINT`** — per Microsoft's documentation for
   [`IDCompositionDevice::CheckDeviceState`](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-checkdevicestate):
   *"If the DXGI device is lost, the DirectComposition device associated with the DXGI
   device is also lost. When it detects a lost device, DirectComposition sends the
   WM_PAINT message to all windows that are composing DirectComposition content using the
   lost device. An application should call CheckDeviceState in response to each WM_PAINT
   message … The application must take steps to recover content if the device object
   becomes invalid. Steps include creating new DXGI and DirectComposition devices, and
   recreating all content."*
   `WndProc` therefore validates the update region and calls
   `OverlayRenderer::DeviceValid()` (→ `CheckDeviceState`); an invalid device flags a
   rebuild.
2. **Frame-loop error classification** — `OverlayRenderer::RenderFrame` checks every
   `EndDraw` / `CopyResource` / `Present` result and returns
   `FrameResult::RecreateDevice` for `D2DERR_RECREATE_TARGET`,
   `DXGI_ERROR_DEVICE_REMOVED`, `DXGI_ERROR_DEVICE_RESET`, `DXGI_ERROR_DEVICE_HUNG` and
   `DXGI_ERROR_DRIVER_INTERNAL_ERROR`. `DXGI_STATUS_OCCLUDED` is a success code and is
   not treated as a failure. Non-fatal failures are logged once (not once per frame) and
   do not trigger a rebuild.
3. **Periodic health check** (~2 s) — re-checks `CheckDeviceState` and the window's
   topmost style and geometry. This covers losses whose `WM_PAINT` never reaches us
   (e.g. a window that is not being composited at that moment).

## Recovery

`RecreateRenderer` rebuilds the **entire** render stack in-process — new D3D11 device,
new DXGI factory and composition swapchain, new D2D device/context and offscreen bitmap,
new DirectComposition device/target/visual — then re-captures the cursor texture (the old
one belongs to the destroyed D2D device) and resumes rendering. Logged as `[recover]`.

- The old stack is torn down in a defined order (`SetTarget(nullptr)` → visual content →
  root → DComp device → swapchain → D2D → D3D) so DWM never keeps a reference to a
  released swapchain.
- While the renderer is down, nothing is rendered: `RenderOneFrame` returns early because
  `OverlayRenderer::ready()` is false (the D2D context would be null and the cursor
  texture capture would dereference it).
- A rebuild can fail — a GPU is often unusable for a moment after a reset — so it is
  retried with an exponential backoff (0.5 s → 1 → 2 → 4 → 8 → 15 s, reset to 0.5 s after
  a success). The loop keeps pumping messages between attempts and sleeps up to 50 ms at a
  time, so the quit hotkey stays responsive.
- After a successful rebuild the vsync period is re-measured and the phase re-anchored
  (device loss often accompanies a display-mode or refresh-rate change).

## Health watchdog

The overlay is only visible while it is both composited and on top, so a ~2 s health
check also fixes the two non-device ways it can "disappear":

- **topmost style lost** (`WS_EX_TOPMOST` cleared by anything) → re-assert it; a window
  buried under other windows looks identical to a rendering failure. It acts only when
  the style is actually missing, so it does not fight other topmost windows for z-order.
- **virtual-screen geometry changed** (`WM_DISPLAYCHANGE`, monitor hot-plug, resolution
  change) → move/resize the window to the new virtual screen and rebuild the swapchain
  and offscreen bitmap at the new size. Without this the overlay keeps its old size and
  origin, so parts of the desktop have no overlay at all.

## Observability

Everything above is logged (see [diagnostics.md](diagnostics.md)):

| Line | Meaning |
|---|---|
| `[recover] recreating D3D/D2D/DComp stack (W x H)` | device loss detected, rebuild starting |
| `[recover] renderer re-initialized` | rebuild succeeded, rendering resumed |
| `[recover] re-initialize failed (will retry)` | rebuild failed, backing off |
| `[watch] WS_EX_TOPMOST lost -> re-asserting` | window was no longer topmost |
| `[watch] virtual screen A,B WxH -> C,D WxH, resizing overlay` | desktop geometry changed |
| `[OverlayRenderer] CheckDeviceState: invalid (…)` | DComp device reported invalid |
| `[OverlayRenderer] Present (device lost) failed: 0x…` | device removed/reset/hung |
