@echo off
rem openBF3 M2a: the playable Metro TDM scene (docs/design/m2a-gx.md, README "M2a").
rem   tools\play.cmd [extra user args, e.g. --kit=3 --team=2 --bots=16 --respawn=3]
rem Godot: %OPENBF3_GODOT%, default the 4.8 dev7 console build next to the repository.
setlocal
set "ROOT=%~dp0.."
if "%OPENBF3_GODOT%"=="" set "OPENBF3_GODOT=C:\Users\olku\BF3 reimplementation\Godot_v4.8-dev7_win64_console.exe"
if not exist "%OPENBF3_GODOT%" (
    echo play.cmd: Godot not found at "%OPENBF3_GODOT%" ^(set OPENBF3_GODOT^)
    exit /b 2
)
set "BOTS=--bots=24"
echo %* | findstr /c:"--bots=" >nul && set "BOTS="
"%OPENBF3_GODOT%" --path "%ROOT%\bf3-opensource" res://scenes/m2a_metro.tscn -- %BOTS% %*
exit /b %ERRORLEVEL%
