# Trail

English | [中文](README.md)

A full-screen, transparent Windows overlay that leaves a trail of afterimages behind the cursor.
Every time the mouse reports, one more trail point is added; at startup the program automatically
measures the primary monitor's single-frame refresh period and uses it as the trail lifetime, so
each trail point is shown for one frame before disappearing. The trail length depends on how far
the cursor moved during that time.

While running, it passively monitors DWM's actual frame timing on the primary monitor: when frame
time deviates from the startup baseline (for example, video playback triggering VRR or DWM frame
drops), it hides the trail and pauses commits until timing returns to the baseline.

## Build

Requirements: Windows 11 (21H2+), Visual Studio 2022+ ("Desktop development with C++" with Windows 11 SDK 10.0.22000+), CMake 3.16+.

```bat
build.bat            REM Release
build.bat Debug      REM Debug
```

Output: `build\trail.exe` (Ninja) or `build\Release\trail.exe` (MSBuild).

## Run

```bat
build\trail.exe
build\trail.exe --sprites 96
```

| Option | Meaning |
|---|---|
| `--sprites <n>` | max number of trail points shown at once (default 64) |
| `--log` | enable diagnostic logging to `trail.log` next to the executable (off by default) |

Quit with `Ctrl+Alt+Q` (exits immediately if the hotkey is already in use; single-instance only).

## Known limitations

- **Primary monitor only**; nothing is drawn on other monitors. A change in resolution, refresh
  rate, display scaling, or the number of monitors requires a restart.
- **At most 64 trail points at once.** With a high-polling-rate mouse on a lower refresh rate, the
  oldest points may be dropped early; if the trail looks shorter than the cursor's actual movement,
  raise `--sprites`.
- **During continuous movement past `--sprites` samples, the newest part of the trail can
  occasionally be covered by older points**; pausing resets the order, and at a one-frame lifetime
  it is barely visible.
- **A GPU driver reset, sleep/resume, or a similar event can make the whole trail disappear**;
  the program recovers automatically, with a short blank period.
- **A DirectX 11-capable GPU is required, and Windows desktop composition must stay enabled**
  (it is on by default).

## Log

File logging is off by default. Pass `--log` to write `trail.log` next to the executable (readable
while the program is running) for startup failures, automatic recovery after a GPU problem,
display changes, and active-window stats. Set `TRAIL_NO_UI=1` to suppress the message box on
startup failure.
