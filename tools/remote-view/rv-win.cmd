@echo off
rem L540 remote view launcher for Windows (see docs/remote-view.md).
rem Usage: rv-win.cmd user@L540-IP [rv-server args, e.g. --fps 15 --size 1440x960]
rem Opens an SSH tunnel, starts ~/remote-view/run.sh on the L540, then opens the browser.
rem Close this window or press Ctrl+C to stop. Port: set RV_PORT (default 8765).
rem Keep this file ASCII-only: cmd.exe misparses UTF-8 batch files.
setlocal
if "%~1"=="" (
	echo Usage: %~nx0 user@L540-IP [rv-server args]
	exit /b 1
)
if not defined RV_PORT set RV_PORT=8765
set "RV_HOST=%~1"
set "RV_ARGS="
:collect
shift
if "%~1"=="" goto run
set "RV_ARGS=%RV_ARGS% %1"
goto collect
:run
rem Give the tunnel and server a few seconds; the first run compiles rv-cap, refresh if the page fails.
start "" /min cmd /c "timeout /t 4 /nobreak >nul & start http://localhost:%RV_PORT%"
ssh -t -o ExitOnForwardFailure=yes -L %RV_PORT%:127.0.0.1:%RV_PORT% %RV_HOST% "~/remote-view/run.sh --port %RV_PORT%%RV_ARGS%"
