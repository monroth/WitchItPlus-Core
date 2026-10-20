@echo off
setlocal DisableDelayedExpansion
powershell.exe -NoLogo -NoProfile -NonInteractive -File "%~dp0updater.ps1" -TargetWin64 "%~dp0."
if not "%errorlevel%"=="0" exit /b 20
if "%~1"=="" exit /b 0
%*
