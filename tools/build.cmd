@echo off
rem Configure, build and test one preset from a plain cmd/PowerShell prompt.
rem Usage: tools\build.cmd [dev|debug|release] [--no-test | --all-tests] [--strict]
rem The default test step skips the wall-clock and GPU timing rows (TIMING below): several worktrees build and test on
rem one machine, and those rows both disturb and are disturbed by parallel work. Run them on purpose, under the quiet
rem lock, with ctest --preset <preset> -R "<row>" (or pass --all-tests here).
rem After the ctest call tools\test_budget.cmake prints the slowest rows, the skipped rows and every budget or label
rem violation (docs/testing.md); --strict makes a violation fail the run. The junit file is b\<preset>\ctest-<preset>-<phase>.xml.
setlocal
set PRESET=%~1
if "%PRESET%"=="" set PRESET=dev
set MODE=%~2
if /i "%MODE%"=="--strict" set MODE=
set STRICT=0
for %%a in (%*) do if /i "%%~a"=="--strict" set STRICT=1
set "TIMING=^(godot\.|host\.scale$|host\.session|host\.server|host\.gunplay\.perf|sim\.nav\.bake|sim\.weapons\.perf|tools\.fx_bench|audio\.decode\.perf|audio\.bank|logic\.perf|tools\.ebx_memory|tools\.m1_prepare|render\.scene$)"
set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
if not defined VSCMD_VER call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
where ninja >nul 2>nul || set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0.."
set "BDIR=%CD%\b\%PRESET%"
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
if /i "%MODE%"=="--no-test" exit /b 0
if /i "%MODE%"=="--all-tests" (
    set PHASE=all
    set "FILTER="
) else (
    echo build.cmd: the timing rows are skipped; run them under the quiet lock or pass --all-tests
    set PHASE=dev
    set "FILTER=-E"
)
set "JUNIT=%BDIR%\ctest-%PRESET%-%PHASE%.xml"
if defined FILTER (
    ctest --preset %PRESET% -E "%TIMING%" --output-junit "%JUNIT%"
) else (
    ctest --preset %PRESET% --output-junit "%JUNIT%"
)
set RC=%ERRORLEVEL%
set ASAN=0
if /i "%PRESET%"=="asan" set ASAN=1
cmake "-DJUNIT=%JUNIT%" "-DTESTDIR=%BDIR%" -DSTRICT=%STRICT% -DASAN=%ASAN% -DPHASE=%PRESET%-%PHASE% -P tools\test_budget.cmake || set RC=1
exit /b %RC%
