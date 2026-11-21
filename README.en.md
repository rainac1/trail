# Trail

English | [中文](README.md)

A full-screen, transparent Windows overlay that leaves a trail of afterimages behind the cursor.
Every time the mouse reports, one more trail point is added; each point stays visible for
`--trail-ms` milliseconds before it disappears. The default 8 ms matches one frame at 125 Hz,
so each trail point is shown for exactly one frame. The trail length depends on how far the
cursor moved during that time.

## Build

Requirements: Windows 10+, Visual Studio 2017+ ("Desktop development with C++"), CMake 3.16+.

```bat
build.bat            REM Release
build.bat Debug      REM Debug
```

Output: `build\trail.exe` (Ninja) or `build\Release\trail.exe` (MSBuild).

## Run

```bat
build\trail.exe
build\trail.exe --trail-ms 12 --sprites 96
```

| Option | Meaning |
|---|---|
| `--trail-ms <ms>` | how long each trail point is shown; default 8 ms, one frame at 125 Hz |
| `--sprites <n>` | max number of trail points shown at once (default 64) |
| `--log` | enable diagnostic logging to `trail.log` next to the executable (off by default) |

Quit with `Ctrl+Alt+Q` (exits immediately if the hotkey is already in use; single-instance only).

## Known limitations

- **Primary monitor only**; nothing is drawn on other monitors. A change in resolution, display
  scaling, or the number of monitors requires a restart.
- **At most 64 trail points at once.** With a long trail, the oldest points are dropped early;
  if the trail looks shorter than the cursor's actual movement, raise `--sprites`.
- **During continuous movement past `--sprites` samples, the newest part of the trail can
  occasionally be covered by older points**; pausing resets the order, and at the default 8 ms
  (one frame) it is barely visible.
- **A GPU driver reset, sleep/resume, or a similar event can make the whole trail disappear**;
  the program recovers automatically, with a short blank period.
- **A DirectX 11-capable GPU is required, and Windows desktop composition must stay enabled**
  (it is on by default).

## Log

File logging is off by default. Pass `--log` to write `trail.log` next to the executable (readable
while the program is running) for startup failures, automatic recovery after a GPU problem,
display changes, and active-window stats. Set `TRAIL_NO_UI=1` to suppress the message box on
startup failure.
