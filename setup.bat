@echo off
title The Sims: Bustin' Out - One-Click Setup Wizard
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup.ps1" %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo [Setup exited with code %RC%]
    pause
)
exit /b %RC%
