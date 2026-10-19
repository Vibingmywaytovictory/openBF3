@echo off
rem openBF3 M2a: the playable Metro TDM scene (docs/design/m2a-gx.md, README "M2a").
rem   tools\play.cmd [extra user args, e.g. --kit=3 --team=2 --bots=16 --respawn=3]
rem Godot: %OPENBF3_GODOT%, default the 4.8 dev7 console build next to the repository.
setlocal
rem M2b bb [help]: --help prints the options, --tdm included (docs/design/m2b-bb.md).
if /i "%~1"=="--help" goto help
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
:help
rem M2b bb [help]
echo Usage: tools\play.cmd [user args]
echo   --kit=0..3 --team=1^|2 --bots=N ^(default 24^) --respawn=S   M2a: the kit, team, bot count, respawn seconds
echo   --tdm   M2b preview ^(bb^): TDM rules on the M2a scene without the deploy screen; the bots are the TDM bots
echo           ^(config\bot_tuning.cfg^), at most 31; try: tools\play.cmd --tdm --bots=31
echo   --kill-limit=N --preround=S   with --tdm: the kill limit ^(data: 100^) and the pre-round seconds ^(data: 10^)
echo The match state prints as "[m2b] match: ..." console lines ^(no TDM HUD in the M2a scene^).
exit /b 0
