@echo off
cd /d "%~dp0"
set "EXE=.\build\Release\sims_bustin_out_recomp.exe"
if not exist "%EXE%" (
    echo [ERROR] Executable not found: %EXE%
    echo Build the project first with:
    echo   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    echo   cmake --build build --config Release --parallel
    pause
    exit /b 1
)
echo Launching The Sims: Bustin' Out (Native Windows x64 Launcher ^& Port)...
"%EXE%" %*
