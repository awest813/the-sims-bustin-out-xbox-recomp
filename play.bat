@echo off
cd /d "%~dp0"
set "EXE=.\build\Release\sims_bustin_out_recomp.exe"
if not exist "%EXE%" (
    echo.
    echo The Sims: Bustin' Out has not been set up / built yet.
    echo.
    choice /C YN /M "Run setup.bat now to extract your ISO and build automatically?"
    if errorlevel 2 exit /b 0
    call "%~dp0setup.bat"
    exit /b %ERRORLEVEL%
)
echo Launching The Sims: Bustin' Out (Native Windows x64 Launcher ^& Port)...
"%EXE%" %*
