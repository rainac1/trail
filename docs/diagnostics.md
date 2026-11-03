# Diagnostics

How to find out what the program is doing, especially when "the trail disappeared".

## `trail.log`

- Written **next to the exe** (`build\trail.exe` → `build\trail.log`), appended to, with a
  `===== --- session --- =====` header per run.
- Opened for **shared reads** (`_wfsopen` + `_SH_DENYWR`), so it can be inspected *while the
  program is running*. This matters for this project: killing the process to read the log
  destroys the evidence you wanted.
- Read it with PowerShell/cmd, e.g. `Get-Content .\trail.log -Tail 40` (add `-Wait` to
  follow). MSYS/Git-Bash `cat`/`grep` can report `Device or resource busy` on a log that is
  open by the running process — use PowerShell or `cmd` instead. It is UTF-8 with BOM.
- `TRAIL_NO_UI=1` suppresses the startup-failure message box (legacy name
  `SUBFRAME_NO_UI` is still accepted); the log is always written.

## Log line reference

| Line | Meaning |
|---|---|
| `[main] window X,Y WxH dpi=N hwnd=…` | the overlay window: primary monitor, physical pixels, window DPI |
| `[diag] DWM composition enabled/DISABLED` | desktop composition state |
| `[diag] adapter: …` / `[diag] D3D feature level: 0x…` | which GPU/adapter the D3D11 device landed on |
| `[main] renderer ready: commit-hz=N (tick T ms), trail=M ms, sprites=K, head=overlay/system` | the configuration actually in force; `head=overlay` means `--hide-cursor` is on (the head visual is drawn), `head=system` means the head is left to the hardware cursor |
| `[OverlayRenderer] initialized: N sprite visuals + head, DirectComposition visual tree` | visual tree ready |
| `[update] ticks=A/s commits=B/s samples=C/s live=D \| update mean=E max=F us \| commit mean=G max=H us \| tick step=I ms` | health block, every 2 s: achieved tick rate, how many ticks actually committed, samples per second, currently visible sprites, the per-update cost, and how long the `Commit` calls took |
| `[recover] …` | device loss detected / rebuilt / rebuild failed (see [device-loss-recovery.md](device-loss-recovery.md)) |
| `[watch] …` | topmost style re-asserted, or a display topology / DPI change was detected (restart `trail` to adapt — the topology is static) |
| `[OverlayRenderer] <step> failed: 0x…` | a D3D/DComp call failed; device-lost ones are followed by `[recover]` |
| `… failed: 0x… (further identical failures suppressed)` | a non-fatal failure, logged once until an update succeeds again |
| `FATAL: …` | startup failure (also shown in a message box unless `TRAIL_NO_UI=1`) |

Reading the `[update]` fields:

- `ticks` should track `--commit-hz`. Well below it means the tick itself is slow (a stalled
  `Commit`, a preempted thread) — look at `update max`.
- `commits` ≤ `ticks` by construction: a tick commits only when a sample was added, a slot
  expired or the head moved. Idle → near zero commits; moving fast → close to `ticks`.
- `samples` is the cursor report rate actually consumed. Zero while the mouse is moving means
  `GetMouseMovePointsEx` returned nothing — usually the anchor was pushed out of the 64-point
  system history by a stall, or the cursor was hidden (`CURSOR_SHOWING` clear) so nothing is
  drawn at all.
- `live` should be roughly `--trail-ms × report rate` while moving, and drop to 0 shortly
  after the cursor stops. A `live` that stays at `--sprites` means slots are not expiring
  (check `--trail-ms`).
- `update mean` is around 15 µs on the reference system. `commit mean/max` is how long the
  `Commit` calls took; a `commit max` in the millisecond range (with `commits` far below
  `ticks`) means DWM was not consuming commits — a video or a fullscreen app can do that — and
  the loop was blocked inside `Commit`.

## Triaging "the trail disappeared"

1. **Is the log still growing?** (`Get-Content .\trail.log -Wait`)
2. **Growing** → the tick loop is alive. Then:
   - `[recover]` lines → it was a device loss; it is recovered automatically, and
     `[recover] renderer rebuilt` tells you when updates resumed.
   - `[watch] WS_EX_TOPMOST lost` → the window had been pushed below something else.
   - `[watch] display topology changed …` / `[watch] display DPI changed …` → the monitor set,
     resolution or DPI changed while running. The program does **not** follow it (static
     topology); restart `trail`.
   - `live=0` while you move the mouse → the system cursor is hidden (`CURSOR_SHOWING`
     clear — games and fullscreen video do this), the cursor is on a *secondary* monitor (only
     the primary is served), or the sprite failed to capture (look for `[OverlayRenderer]
     CreateSurface` / `surface BeginDraw` failures).
   - **Nothing at all** → nothing is broken on our side; check the by-design cases above, or
     something covering the overlay (a fullscreen-exclusive app or another always-on-top window
     above ours, in which case the trail is drawn but not visible).
3. **Stopped growing** → the loop is blocked. The blocking calls left are
   `WaitForSingleObject` on the tick timer (bounded), `Commit` (DWM stopped consuming updates:
   fullscreen-exclusive app, display asleep, secure desktop) and the cursor capture calls. A
   blocked `Commit` blocks the message loop too, so the quit hotkey stops responding until
   composition resumes.
4. **The process is gone** → look for a `FATAL:` line (startup failures: no hardware D3D11
   device, no DirectComposition, window/class creation). Also check *which* exe you ran: the
   log lives next to that exe.

Useful state to capture while the overlay is misbehaving: the overlay window class is
`TrailOverlay` (title `Trail  (Ctrl+Alt+Q 退出)`). This reports whether it is still visible and
still topmost (`WS_EX_TOPMOST` = 0x8 in `GWL_EXSTYLE`):

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

- **MSVC is the supported toolchain** (`build.bat`). MinGW builds compile and run, but its
  import libraries mix real imports with stubs depending on the entry point, so a MinGW build
  can fail at startup for reasons that do not exist in the MSVC build. If a MinGW build fails
  at `DCompositionCreateDevice3` or the `IDCompositionVisual3` QI, check the import table with
  `objdump -p trail.exe | findstr /i dcomp` before blaming the code.
- **Windows 10+ is required**: the renderer uses `DCompositionCreateDevice3` and
  `IDCompositionVisual3::SetVisible`. On older systems the visual query fails with
  `E_NOINTERFACE (0x80004002)` and the program exits with the corresponding `FATAL:` message.
- **Prefer a Release build when comparing numbers.** Debug builds update more slowly (the
  `update mean` field grows), but correctness and log behaviour are the same.
- **`build.bat` reports "Visual Studio C++ toolchain not found"** when `vswhere` cannot see an
  installation. `vswhere` only reports installs registered with the Visual Studio Installer — a
  VS that was copied/moved to another location (or an unregistered/preview install) is
  invisible to it even though `cl.exe` exists. In that case locate
  `VC\Auxiliary\Build\vcvarsall.bat` and drive CMake manually (option 2 in
  [`README.md`](../README.md) / [`README.en.md`](README.en.md)).
- **`LNK1104: cannot open file 'trail.exe'`** means a previous `trail.exe` is still running
  and holding the file: quit it (`Ctrl+Alt+Q`) or kill it, then rebuild.
