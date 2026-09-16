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
| `[clock] DWM composition clock: period=N ms (rateRefresh=a/b), qpcVBlank-qpcCompose=X ms, qpcVBlank-now=Y ms` | the composition clock that everything is timed against; `period` is the refresh period, and the last field says whether the reported timestamp is the upcoming (+) or the last (−) composition |
| `[clock] lead policy: start=… margin=… baseline tau ~ … warmup … seed … follower …` | the active `lead` policy and its tuning constants, logged once at startup |
| `[clock] frames=N fps=F missed=M (P%), lead=X ms (baseline=Y), need mean/max, wake mean/max, slack mean/min, clockRead mean/max` | timing health, every 3000 frames. `fps` should equal the refresh rate; `missed` is the share of frames that finished after their target composition (cumulative); `lead` is the wake-up budget currently in force and `baseline` is the slow requirement mean it is built from (`lead = baseline + kLeadMarginMs`, so `baseline` reads 0.00 during the startup warm-up); `need` is `t1 − target`, the work that has to fit inside `lead` (wake-up jitter + render time) — its `max` is what shows how heavy the tail is; `wake` is how late the thread woke relative to its target; `slack` is how much room was left before the target composition when the frame was submitted (negative = missed), so its mean is the average head latency; `clockRead` is the cost of the `DwmGetCompositionTimingInfo` call. The `mean` fields cover one 3000-frame block and reset with each line; the same line is emitted once more at exit to cover the final, incomplete block, so a run shorter than 3000 frames still yields a usable row |
| `[clock] margin probe over N frames (need-baseline > x): 0.30=…% 0.45=…% …` | the miss rate each candidate `kLeadMarginMs` would produce, measured directly: a miss is exactly `needMs − baseline > margin`. Emitted with each stats block (and at exit); use it to pick the margin instead of re-running with one value at a time |
| `[recover] …` | device loss detected / rebuilt / rebuild failed (see [device-loss-recovery.md](device-loss-recovery.md)) |
| `[watch] …` | topmost style re-asserted, or virtual-screen geometry changed |
| `[OverlayRenderer] <step> failed: 0x…` | D3D/D2D/DComp call failure (device-lost ones are followed by `[recover]`) |
| `… failed: 0x… (further identical failures suppressed)` | a non-fatal per-frame failure, logged once until a frame succeeds again |
| `FATAL: 无法读取 DWM 合成时钟…` | startup or runtime clock failure → the process exits with code 1 (no fallback) |
| `FATAL: 高分辨率可等待定时器创建失败…` | waiting mechanism unavailable → the process exits with code 1 (no fallback) |
| `FATAL: …` | any other startup failure (also shown in a message box unless `TRAIL_NO_UI=1`) |

Reading the timing fields:

- `fps` below the refresh rate → compositions are being skipped (each skip also makes one
  displayed frame carry two slots' worth of trail).
- `missed` above ~1 % → frames are being submitted after their composition; look at
  `wake max` (a wake-up tail) and `slack min` (how far past the deadline).
- `clockRead max` in the millisecond range → the DWM query is stalling; measured ≈ 10 µs
  mean, with worst cases of a few hundred µs.
- `lead` well above `need max` → the policy is wasting head latency. `lead` sitting right on
  `need max` while `missed` climbs → it is too tight. The one knob that trades head latency
  against `missed` is `kLeadMarginMs` in `src/main.cpp`: the operating point is
  `baseline + margin`, and `baseline` is measured (≈ 0.55 ms on the reference system), so
  lowering the margin lowers `lead` one-for-one. Read the `margin probe` line to pick it — it
  lists the miss rate each candidate margin would produce, from the same run.
  `kNeedSlowAlpha` sets how fast the baseline follows a real change in render cost; `kNeedWarmupFrames` / `kNeedSeedSamples` keep the
  cold-start transient out of it; `kMaxAttackStepMs` and `kReleaseAlpha` only shape the
  follower and no longer affect stability, now that the target is smooth.

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
   where. There is no longer a `DwmFlush`-based stall to blame: the loop only blocks in
   `WaitForSingleObject` (bounded by design), in `Present`, or in a `DwmGetCompositionTimingInfo`
   query (measured at µs). A blocked `Present` means DWM stopped consuming frames
   (fullscreen-exclusive app, display asleep, secure desktop); the message loop is then
   blocked too and the quit hotkey does not respond until composition resumes.
4. **The process is gone** → look for a `FATAL:` line: since there is no fallback, an
   unavailable composition clock or waiting mechanism exits the process with code 1
   instead of degrading. Also check *which* exe you ran: the log lives next to that exe.

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

- **MSVC is the supported toolchain** (`build.bat`); MinGW builds are possible but have
  one caveat worth knowing: MinGW's `libdwmapi.a` mixes real imports with **stubs**
  depending on the function. `DwmGetCompositionTimingInfo` is a real import (checked with
  `objdump -p trail.exe | findstr /i dwmapi`), so a MinGW build of the current code runs
  normally, whereas `DwmFlush` is a stub that returns immediately — which is exactly why
  the old `DwmFlush`-based scheme could never be evaluated with a MinGW build. If a MinGW
  build ever fails at startup with a clock error, check that import table first.
  MinGW also needs `#include <dxgi1_3.h>` for `IDXGISwapChain2`, which MSVC declares in
  `dxgi1_2.h`.
- **Windows 10 1803+ is required** for `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`. On older
  builds `CreateWaitableTimerExW` fails, and since there is no fallback the program exits
  with the corresponding `FATAL:` message.
- **Prefer a Release build when comparing latency.** `lead` follows the measured `need`
  (`t1 − target`), so a slower (Debug) build legitimately raises it and shows a larger head
  latency. Correctness and log behaviour are the same in both.
- **`build.bat` reports "Visual Studio C++ toolchain not found"** when `vswhere` cannot
  see an installation. `vswhere` only reports installs registered with the Visual Studio
  Installer — a VS that was copied/moved to another location (or an unregistered/preview
  install) is invisible to it even though `cl.exe` exists. In that case locate
  `VC\Auxiliary\Build\vcvarsall.bat` and drive CMake manually (option 2 in
  [`README.md`](../README.md) / [`README.en.md`](README.en.md)).
