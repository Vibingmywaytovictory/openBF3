@echo off
rem openBF3: the playable Metro TDM scene. M2b gx: the default is the M2b scene (docs/design/m2b-gx.md, README "M2b"):
rem a Metro TDM round against 31 bots with the spawn screen; --m2a keeps M2a's scene (docs/design/m2a-gx.md).
rem   tools\play.cmd [extra user args, e.g. --kill-limit=30 --preround=0 --bots=15]
rem Godot: %OPENBF3_GODOT%, default the 4.8 dev7 console build next to the repository.
setlocal
rem M2b bb [help]: --help prints the options, --tdm included (docs/design/m2b-bb.md).
if /i "%~1"=="--help" goto help
set "ROOT=%~dp0.."
if "%OPENBF3_GODOT%"=="" set "OPENBF3_GODOT=%ROOT%\..\Godot_v4.8-dev7_win64_console.exe"
if not exist "%OPENBF3_GODOT%" (
    echo play.cmd: Godot not found at "%OPENBF3_GODOT%" ^(set OPENBF3_GODOT^)
    exit /b 2
)
rem M2b gx [scene]: --m2a (M2a's scene) and bb's --tdm preview (TDM rules on the M2a scene) keep the M2a scene and its
rem 24 bots; otherwise the M2b scene with 31 TDM bots. --m2a itself is passed on and ignored by the session.
set "SCENE=res://scenes/m2b_metro.tscn"
set "BOTS=--bots=31"
echo  %* | findstr /c:" --m2a " /c:" --tdm " >nul && (
    set "SCENE=res://scenes/m2a_metro.tscn"
    set "BOTS=--bots=24"
)
echo %* | findstr /c:"--bots=" >nul && set "BOTS="
"%OPENBF3_GODOT%" --path "%ROOT%\bf3-opensource" %SCENE% -- %BOTS% %*
exit /b %ERRORLEVEL%
:help
rem M2b bb [help]
echo Usage: tools\play.cmd [user args]
rem M2b gx [help]
echo The M2b scene ^(default^): a Metro TDM round, you and 31 bots ^(16 v 16^), teams by the game.
echo   Spawn screen: 1-4 pick the kit ^(assault, engineer, support, recon^) or click it, Enter or Deploy spawns.
echo   In game: Tab scoreboard, 3 the gadget ^(assault: the defibrillator^), the M2a weapon keys otherwise.
echo   --bots=0..31 ^(default 31^) --kill-limit=N ^(data: 100^) --preround=S ^(data: 10^) --deploy-screen=0 ^(automatic respawn^)
echo   --m2a   M2a's scene instead: --kit=0..3 --team=1^|2 --bots=N ^(default 24^) --respawn=S
echo   --tdm   M2b preview ^(bb^): TDM rules on the M2a scene without the deploy screen; the bots are the TDM bots
echo           ^(config\bot_tuning.cfg^), at most 31; try: tools\play.cmd --tdm --bots=31
echo   --kill-limit=N --preround=S   with --tdm: the kill limit ^(data: 100^) and the pre-round seconds ^(data: 10^)
echo With --tdm the match state prints as "[m2b] match: ..." console lines ^(no TDM HUD in the M2a scene^).
exit /b 0
