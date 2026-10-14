@echo off
rem Configure, build and test one preset from a plain cmd/PowerShell prompt.
rem Usage: tools\build.cmd [dev|debug|release] [--no-test]
setlocal
set PRESET=%~1
if "%PRESET%"=="" set PRESET=dev
set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
if not defined VSCMD_VER call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
where ninja >nul 2>nul || set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0.."
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
if /i "%~2"=="--no-test" exit /b 0
ctest --preset %PRESET% || exit /b 1
