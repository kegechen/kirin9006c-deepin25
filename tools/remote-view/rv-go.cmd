@echo off
rem Double-click / cmd.exe wrapper for rv-go.ps1 (same arguments). See rv-go.ps1 for usage.
rem Keep this file ASCII-only: cmd.exe misparses UTF-8 batch files.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0rv-go.ps1" %*
if errorlevel 1 pause
