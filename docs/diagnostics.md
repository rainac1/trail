# Diagnostics

How to find out what the program is doing, especially when "the rendering disappeared".

## `trail.log`

- Written **next to the exe** (`build\trail.exe` → `build\trail.log`), appended to, with a
  `===== --- session --- =====` header per run.
- Opened for **shared reads** (`_wfsopen` + `_SH_DENYWR`), so it can be inspected *while
  the program is running*. This matters for this project: killing the process to read the
  log destroys the evidence you wanted.
- Read it with PowerShell/cmd, e.g. `Get-Content .\trail.log -Tail 40` (add `-Wait` to
  follow). MSYS/Git-Bash `cat`/`grep` can report `Device or resource busy` on a log that
  is open by the running process — use PowerShell or `cmd` instead. It is UTF-8 with BOM.
- `TRAIL_NO_UI=1` suppresses the startup-failure message box (legacy name
  `SUBFRAME_NO_UI` is still accepted); the log is always written.

## Log line reference

| Line | Meaning |
|---|---|
| `[main] virtual screen X,Y WxH, hwnd=…` | window created, covering the whole virtual desktop |
| `[diag] DWM composition enabled/DISABLED` | desktop composition state |
| `[diag] adapter: …` / `[diag] D3D feature level: 0x…` | which GPU/adapter the D3D11 device landed on |
| `[OverlayRenderer] initialized: W x H, DirectComposition + offscreen blit` | render stack ready |
| `[vsync] calibrated refresh period: N ms` | low-latency path active, phase aligned to the composition refresh |
| `[vsync] calibration rejected: N ms per flush (DwmFlush not vsync-throttled?)` | `DwmFlush` did not block as expected → fallback (see toolchain pitfall below) |
| `[vsync] DwmFlush failed: 0x…` | `DwmFlush` returned an error (composition unavailable) |
| `[main] vsync calibration failed, falling back to Present(1,0)` | running in the high-latency fallback (~1 frame more head latency) |
| `[vsync] refresh period changed: A ms -> B ms` | periodic re-measurement saw VRR / mode / refresh-rate change |
| `[vsync] frames=N missed=M (P%), render EMA=X ms, budget=Y ms` | low-latency path health: `missed` is the share of frames whose render overran the target vsync, `budget` is the adaptive wake-up lead |
| `[frame] N frames rendered (Present(1,0) fallback)` | fallback-path heartbeat |
| `[recover] …` | device loss detected / rebuilt / rebuild failed (see [device-loss-recovery.md](device-loss-recovery.md)) |
| `[watch] …` | topmost style re-asserted, or virtual-screen geometry changed |
| `[OverlayRenderer] <step> failed: 0x…` | D3D/D2D/DComp call failure (device-lost ones are followed by `[recover]`) |
| `… failed: 0x… (further identical failures suppressed)` | a non-fatal per-frame failure, logged once until a frame succeeds again |
| `FATAL: …` | startup failure (also shown in a message box unless `TRAIL_NO_UI=1`) |

## Triaging "the rendering disappeared"

1. **Is the log still growing?** (`Get-Content .\trail.log -Wait`)
2. **Growing** → the render loop is alive and presenting. Then:
   - `[recover]` lines → it was a device loss; it is now recovered automatically, and the
     `[recover] renderer re-initialized` line tells you when rendering resumed.
   - `[watch] WS_EX_TOPMOST lost` → the window had been pushed below something else.
   - `[watch] virtual screen … -> …` → the desktop geometry changed.
   - **Nothing at all** → the device is valid and the window is topmost, so nothing is
     broken on our side. Check the two by-design cases: the **system cursor is hidden**
     (`CURSOR_SHOWING` clear — games do this; the overlay deliberately draws nothing
     then), or something is covering the overlay (a fullscreen-exclusive app or another
     always-on-top window above ours, in which case our trail is drawn but not visible).
3. **Stopped growing** → the loop is blocked, and the last line before the gap says
   where. A stall that begins around a `[vsync]` line points at `DwmFlush` blocking
   because DWM stopped composing (fullscreen-exclusive app, display asleep, secure
   desktop). The message loop is then blocked too, so the quit hotkey does not respond
   until composition resumes.
4. If the session did not log anything at all, check *which* exe you ran: the log lives
   next to that exe.

Useful state to capture while the overlay is misbehaving: the overlay window class is
`TrailOverlay` (title `Trail  (Ctrl+Alt+Q 退出)`). This reports whether it is still
visible and still topmost (`WS_EX_TOPMOST` = 0x8 in `GWL_EXSTYLE`):

```powershell
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="GetClassNameW")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GL(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public static string Report() {
    string r = "not found";
    EnumWindows((h,l) => {
      var sb = new StringBuilder(256); GetClassName(h, sb, 256);
      if (sb.ToString() == "TrailOverlay") {
        r = string.Format("hwnd={0} visible={1} exstyle=0x{2:X} topmost={3}",
                          h, IsWindowVisible(h), (long)GL(h,-20), ((long)GL(h,-20) & 0x8L) != 0);
        return false;
      }
      return true;
    }, IntPtr.Zero);
    return r;
  }
}
"@
[W]::Report()
```

## Toolchain pitfalls

- **Never judge latency from a MinGW build.** MinGW's `libdwmapi.a` resolves
  `DwmFlush` and `DwmIsCompositionEnabled` to **stubs**: the resulting exe imports no
  `dwmapi.dll` at all, so `DwmFlush` returns immediately, calibration is rejected
  (`0.00 ms per flush`) and the program always runs in the high-latency
  `Present(1,0)` fallback. Verify with
  `objdump -p trail.exe | findstr /i dwmapi` (an MSVC build lists `DwmFlush`; a MinGW
  build lists nothing). The MSVC build of the same source tree behaves normally.
  MinGW also needs `#include <dxgi1_3.h>` for `IDXGISwapChain2`, which MSVC declares in
  `dxgi1_2.h`.
- **Prefer a Release build when comparing latency.** The adaptive budget is derived from
  the measured render time, so a slower (Debug) build legitimately wakes the render loop
  earlier and shows a slightly larger head latency. Correctness and log behaviour are the
  same in both.
- **`build.bat` reports "Visual Studio C++ toolchain not found"** when `vswhere` cannot
  see an installation. `vswhere` only reports installs registered with the Visual Studio
  Installer — a VS that was copied/moved to another location (or an unregistered/preview
  install) is invisible to it even though `cl.exe` exists. In that case locate
  `VC\Auxiliary\Build\vcvarsall.bat` and drive CMake manually (option 2 in
  [`README.md`](../README.md) / [`README.en.md`](README.en.md)).
