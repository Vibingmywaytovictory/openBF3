@echo off
rem Configure, build and test one preset from a plain cmd/PowerShell prompt.
rem Usage: tools\build.cmd [dev|debug|release] [--no-test | --all-tests]
rem The default test step skips the wall-clock and GPU timing rows (TIMING below): several worktrees build and test on
rem one machine, and those rows both disturb and are disturbed by parallel work. Run them on purpose, under the quiet
rem lock, with ctest --preset <preset> -R "<row>" (or pass --all-tests here).
setlocal
set PRESET=%~1
if "%PRESET%"=="" set PRESET=dev
set "TIMING=^(godot\.|host\.scale$|host\.session|host\.server|host\.gunplay\.perf|host\.gunplay\.client$|sim\.nav\.bake|sim\.weapons\.perf|tools\.fx_bench|audio\.decode\.perf|audio\.bank|logic\.perf|tools\.ebx_memory|tools\.m1_prepare|render\.scene$)"
set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
if not defined VSCMD_VER call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
where ninja >nul 2>nul || set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0.."
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
if /i "%~2"=="--no-test" exit /b 0
if /i "%~2"=="--all-tests" (
    ctest --preset %PRESET% || exit /b 1
    exit /b 0
)
echo build.cmd: the timing rows are skipped; run them under the quiet lock or pass --all-tests
ctest --preset %PRESET% -E "%TIMING%" || exit /b 1
