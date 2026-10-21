@echo off
setlocal
if /i "%~1"=="--help" goto help
set "ROOT=%~dp0.."
if "%OPENBF3_GODOT%"=="" set "OPENBF3_GODOT=%ROOT%\..\Godot_v4.8-dev7_win64_console.exe"
if not exist "%OPENBF3_GODOT%" (
    echo play.cmd: Godot not found at "%OPENBF3_GODOT%" ^(set OPENBF3_GODOT^)
    exit /b 2
)
set "SCENE=res://scenes/metro_tdm.tscn"
set "BOTS=--bots=31"
echo  %* | findstr /c:" --firefight " /c:" --tdm " >nul && (
    set "SCENE=res://scenes/metro_firefight.tscn"
    set "BOTS=--bots=24"
)
echo %* | findstr /c:"--bots=" >nul && set "BOTS="
"%OPENBF3_GODOT%" --path "%ROOT%\bf3-opensource" %SCENE% -- %BOTS% %*
exit /b %ERRORLEVEL%
:help
echo Usage: tools\play.cmd [user args]
echo The TDM scene ^(default^): a Metro TDM round, you and 31 bots ^(16 v 16^), teams by the game.
echo   Spawn screen: 1-4 pick the kit ^(assault, engineer, support, recon^) or click it, Enter or Deploy spawns.
echo   In game: Tab scoreboard, 3 the gadget ^(assault: the defibrillator^), the weapon keys otherwise.
echo   --bots=0..31 ^(default 31^) --kill-limit=N ^(data: 100^) --preround=S ^(data: 10^) --deploy-screen=0 ^(automatic respawn^)
echo   --firefight   the firefight scene instead: --kit=0..3 --team=1^|2 --bots=N ^(default 24^) --respawn=S
echo   --tdm   TDM rules on the firefight scene without the spawn screen; the bots are the TDM bots
echo           ^(config\bot_tuning.cfg^), at most 31; try: tools\play.cmd --tdm --bots=31
echo   --kill-limit=0..100000 --preround=0..600   with --tdm: the kill limit ^(data: 100^) and the pre-round seconds ^(data: 10^)
echo With --tdm the match state prints as "[tdm] match: ..." console lines ^(no TDM HUD in the firefight scene^).
exit /b 0
