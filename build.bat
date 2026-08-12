@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM  SubframeCursorTrailOverlay build script
REM  Usage: build.bat [Debug|Release]     (default: Release)
REM ============================================================

set "BUILD_TYPE=Release"
if /I "%~1"=="Debug"   set "BUILD_TYPE=Debug"
if /I "%~1"=="Release" set "BUILD_TYPE=Release"
if not "%~1"=="" if /I not "%~1"=="Debug" if /I not "%~1"=="Release" (
  echo [ERROR] Unknown argument "%~1". Usage: build.bat [Debug^|Release]
  exit /b 1
)

REM ---- cmake ----
where cmake >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cmake not found in PATH. Install CMake 3.16+ from https://cmake.org and add it to PATH.
  exit /b 1
)

REM ---- Visual Studio C++ toolchain via vswhere ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe not found. Install Visual Studio 2017+ with "Desktop development with C++".
  exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo [ERROR] Visual Studio C++ toolchain not found. Open Visual Studio Installer and add the "Desktop development with C++" workload.
  exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 (
  echo [ERROR] vcvarsall.bat failed for x64.
  exit /b 1
)

REM ---- generator: prefer Ninja (bundled with VS2019+), fall back to MSBuild ----
set "USING_NINJA=0"
where ninja >nul 2>nul
if not errorlevel 1 set "USING_NINJA=1"

REM ---- recreate 'build' if it was configured with a different generator ----
if exist build\CMakeCache.txt (
  set "CACHE_NINJA=0"
  findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja" build\CMakeCache.txt >nul 2>nul && set "CACHE_NINJA=1"
  set "CACHE_VS=0"
  findstr /C:"CMAKE_GENERATOR:INTERNAL=Visual Studio" build\CMakeCache.txt >nul 2>nul && set "CACHE_VS=1"
  set "MATCH=0"
  if "!USING_NINJA!"=="1" if "!CACHE_NINJA!"=="1" set "MATCH=1"
  if "!USING_NINJA!"=="0" if "!CACHE_VS!"=="1" set "MATCH=1"
  if not "!MATCH!"=="1" (
    echo [build] existing build/ uses a different generator, removing it...
    rmdir /s /q build
  )
)

if "%USING_NINJA%"=="1" (
  echo [build] generator=Ninja  configuration=%BUILD_TYPE%
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
) else (
  echo [build] generator=MSBuild  configuration=%BUILD_TYPE%
  REM CMake picks the newest installed Visual Studio generator by default.
  cmake -S . -B build
)
if errorlevel 1 exit /b 1

cmake --build build --config %BUILD_TYPE%
if errorlevel 1 exit /b 1

if "%USING_NINJA%"=="1" (
  set "OUT_EXE=build\subframe_cursor_trail.exe"
) else (
  set "OUT_EXE=build\%BUILD_TYPE%\subframe_cursor_trail.exe"
)
echo.
echo Build OK: %OUT_EXE%
echo Run:     %OUT_EXE% [--sample-ms N] [--hide-cursor]
echo Quit:    Ctrl+Alt+Q
endlocal
