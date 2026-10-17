@echo off
rem Configure, build and test one preset from a plain cmd/PowerShell prompt (docs/testing.md).
rem Usage: tools\build.cmd [dev|debug|release|asan] [--no-test | --timing | --phase P | --all-tests | --asan-full] [-j N]
rem        [--strict]
rem   (default)    the branch pass: ctest --preset <preset> (16 jobs), which excludes the timing, realtime and window
rem                rows (docs/testing.md "Scheduling labels"); for asan the default asan selection (16 jobs, no
rem                asan_sweep rows), then the asan-realtime phase (4 jobs), never mixed
rem   --asan-full  asan only: the two asan phases, then the full tier's sweeps (asan-sweep, 8 jobs; docs/testing.md
rem                "Asan")
rem   --timing     the merge pass's phases, always all three, in order: <preset>-realtime (8 jobs), <preset>-timing
rem                (serial), <preset>-window (serial; the windowed rows skip unless OPENBF3_GODOT_WINDOWED=1)
rem   --phase P    only that merge-pass phase (realtime, timing or window); repeat it for several, run in that order
rem   --all-tests  the branch pass, then the three --timing phases (never one mixed ctest call)
rem The merge-pass phases run through tools\timing_pass.cmake: every phase runs even when an earlier one failed, and
rem the realtime phase applies the rerun rule (tests\realtime_retry.cmake); OPENBF3_RERUN_LOG names its rerun log.
rem   -j N         jobs for the build and the branch pass (CTEST_PARALLEL_LEVEL does the same for the branch pass)
rem The build and the branch pass run at below-normal priority. While the file named by OPENBF3_QUIET_LOCK exists (the
rem workflows set the variable; this script never creates, waits on or deletes that file) they use 4 jobs. The timing
rem phases keep normal priority and their preset's jobs: they are what the quiet lock is held for.
rem After each ctest phase tools\test_budget.cmake prints the slowest rows, the skipped rows and every budget or label
rem violation; --strict makes a violation fail the run. Junit files: b\<preset>\ctest-<preset>-<phase>.xml.
setlocal
rem shift below also shifts %0: take the checkout root first.
set "ROOT=%~dp0.."
set PRESET=
set MODE=branch
set JOBS=
set STRICT=0
set PHASES=
set ASAN_FULL=0
:args
if "%~1"=="" goto args_done
if /i "%~1"=="--no-test" (
    set MODE=none
) else if /i "%~1"=="--timing" (
    set MODE=timing
) else if /i "%~1"=="--phase" (
    set MODE=timing
    if defined PHASES (set "PHASES=%PHASES%;%~2") else (set "PHASES=%~2")
    shift
) else if /i "%~1"=="--all-tests" (
    set MODE=all
) else if /i "%~1"=="--asan-full" (
    set ASAN_FULL=1
) else if /i "%~1"=="--strict" (
    set STRICT=1
) else if /i "%~1"=="-j" (
    set "JOBS=%~2"
    shift
) else (
    set "PRESET=%~1"
)
shift
goto args
:args_done
if "%PRESET%"=="" set PRESET=dev
if not defined PHASES set "PHASES=realtime;timing;window"
if "%JOBS%"=="" if defined CTEST_PARALLEL_LEVEL set "JOBS=%CTEST_PARALLEL_LEVEL%"
if defined OPENBF3_QUIET_LOCK if exist "%OPENBF3_QUIET_LOCK%" (
    echo build.cmd: %OPENBF3_QUIET_LOCK% exists: the build and the branch pass use 4 jobs
    set JOBS=4
)
set "BUILD_JOBS="
set "TEST_JOBS="
if not "%JOBS%"=="" (
    set "BUILD_JOBS=-j %JOBS%"
    set "TEST_JOBS=-j %JOBS%"
)
set ASAN=0
if /i "%PRESET%"=="asan" set ASAN=1
set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
if not defined VSCMD_VER call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
where ninja >nul 2>nul || set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%ROOT%"
set "BDIR=%CD%\b\%PRESET%"
rem start /wait hands the child's exit code to ERRORLEVEL (|| would only see start itself).
start "" /b /wait /belownormal cmake --preset %PRESET%
if errorlevel 1 exit /b 1
start "" /b /wait /belownormal cmake --build --preset %PRESET% %BUILD_JOBS%
if errorlevel 1 exit /b 1
if "%MODE%"=="none" exit /b 0
set RC=0
if "%MODE%"=="timing" goto timing
call :phase %PRESET% branch belownormal "%TEST_JOBS%"
if "%ASAN%"=="1" call :phase asan-realtime realtime belownormal ""
if "%ASAN%"=="1" if "%ASAN_FULL%"=="1" call :phase asan-sweep sweep belownormal ""
if "%MODE%"=="branch" exit /b %RC%
:timing
echo build.cmd: merge-pass phases %PHASES% (normal priority)
cmake "-DPRESET=%PRESET%" "-DBDIR=%BDIR%" "-DPHASES=%PHASES%" -DSTRICT=%STRICT% "-DRERUN_LOG=%OPENBF3_RERUN_LOG%" -P tools\timing_pass.cmake || set RC=1
exit /b %RC%

rem :phase <test preset> <phase name> <priority> "<extra ctest args>"
:phase
set "JUNIT=%BDIR%\ctest-%PRESET%-%~2.xml"
echo build.cmd: ctest --preset %~1 %~4 (%~3 priority)
start "" /b /wait /%~3 ctest --preset %~1 %~4 --output-junit "%JUNIT%"
if errorlevel 1 set RC=1
if exist "%JUNIT%" (
    cmake "-DJUNIT=%JUNIT%" "-DTESTDIR=%BDIR%" -DSTRICT=%STRICT% -DASAN=%ASAN% -DPHASE=%PRESET%-%~2 -P tools\test_budget.cmake || set RC=1
)
exit /b 0
