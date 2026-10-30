@echo off
setlocal EnableDelayedExpansion
if /i "%~1"=="--help" goto help
set "ROOT=%~dp0.."
if "%OPENBF3_GODOT%"=="" for %%G in ("%ROOT%\..\Godot_v4.8-dev7_win64_console.exe" "%ROOT%\..\..\Godot_v4.8-dev7_win64_console.exe") do if not defined OPENBF3_GODOT if exist "%%~G" set "OPENBF3_GODOT=%%~G"
if "%OPENBF3_GODOT%"=="" set "OPENBF3_GODOT=%ROOT%\..\Godot_v4.8-dev7_win64_console.exe"
if not exist "%OPENBF3_GODOT%" (
    echo server.cmd: Godot not found at "%OPENBF3_GODOT%" ^(set OPENBF3_GODOT^)
    exit /b 2
)
set "ALL= %*"
set "ARGS="
set "A1=%~1"
if not defined A1 goto run
if "%A1:~0,2%"=="--" goto run
set "ARGS=--level=%A1%"
set "ALL=!ALL:* %A1%=!"
set "A2=%~2"
if not defined A2 goto run
if "%A2:~0,2%"=="--" goto run
set "ARGS=%ARGS% --mode=%A2%"
set "ALL=!ALL:* %A2%=!"
:run
"%OPENBF3_GODOT%" --headless --path "%ROOT%\bf3-opensource" --main-loop DedicatedMainLoop -- %ARGS% !ALL! --bf3-cache=user://bf3cache-server
exit /b %ERRORLEVEL%
:help
echo Usage: tools\server.cmd [level [mode]] [user args]
echo A headless dedicated server: TDM rules and bots, no window, its own cache user://bf3cache-server.
echo   level mode   one level, e.g. MP_Subway TeamDeathMatch0; without them the rotation in config\maplist.cfg
echo   --maplist=PATH   another rotation file ^(one "level mode rounds" line per entry^)
echo   --bots=0..64 ^(default 31^) --max-players=1..64 ^(default 32^) --kill-limit=N ^(data: 100^) --preround=S ^(data: 10^) --min-players=N
echo   --port=N ^(default 0: any free port, printed^) --port-file=PATH --bind=IP ^(default 127.0.0.1^) --bf3-cache=PATH --seed=N --dev
echo   --dedicated-seconds=S --stop-after-kills=N   stop after S seconds or N kills
echo   --intermission-s=S --load-stall-s=S   dev overrides: S seconds from round over to the next round ^(data: 60^), and of no load progress before a LoadStalled kick ^(30^)
echo Join it with: tools\play.cmd --connect=127.0.0.1:PORT
exit /b 0
