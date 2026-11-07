@echo off
rem Monado: set up the tools (tools\setup_windows.ps1), point the project at your own game files
rem (tools\setup_game_files.ps1) and build everything (scripts\build_all.sh). Instructions on how to obtain the game
rem files are NOT provided: you need the exefs and romfs folders of your own copy, version 2.0.0.
setlocal EnableExtensions
cd /d "%~dp0"
title Monado - build
echo.
echo  Monado build
echo  ============

rem ---- Git Bash runs the build scripts
set "BASH=%ProgramFiles%\Git\bin\bash.exe"
if not exist "%BASH%" set "BASH=%LocalAppData%\Programs\Git\bin\bash.exe"
if not exist "%BASH%" (
    echo.
    echo  Git for Windows is needed ^(https://git-scm.com/download/win^). Install it and run this again.
    goto :fail
)

rem ---- 1. tools: MSYS2 + libraries, Python + .venv
set "SETUP="
if not exist "C:\msys64\ucrt64\lib\libSDL3.dll.a" set "SETUP=1"
if not exist "C:\msys64\ucrt64\lib\libshaderc_shared.dll.a" set "SETUP=1"
if not exist ".venv\Scripts\python.exe" set "SETUP=1"
if defined SETUP (
    echo.
    echo  1. The build tools are not set up yet ^(MSYS2 with SDL3, FFmpeg, Vulkan, shaderc, Opus; Python 3.12^).
    choice /m "    Install them now (winget + pacman + pip, a few GB)"
    if errorlevel 2 goto :fail
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\setup_windows.ps1
    if errorlevel 1 goto :fail
) else (
    echo  1. Build tools: ready
)
set "PATH=C:\msys64\ucrt64\bin;%PATH%"

rem ---- 2. your game files
".venv\Scripts\python.exe" -X utf8 tools\extract\check_dump.py targets >nul 2>nul
if errorlevel 1 goto :pick
echo  2. Game files: targets\ already holds version 2.0.0
choice /m "    Use them"
if not errorlevel 2 goto :build
:pick
echo.
echo  2. Choose the folder that has the game's exefs and romfs folders in it.
echo     ^(Xenoblade Chronicles: Definitive Edition, version 2.0.0: the base game with the 2.0.0 update applied^)
powershell -NoProfile -ExecutionPolicy Bypass -File tools\setup_game_files.ps1
if errorlevel 2 goto :fail
if errorlevel 1 (
    choice /m "    Choose another folder"
    if errorlevel 2 goto :fail
    goto :pick
)

rem ---- 3. build
:build
echo.
echo  3. Building ^(10-20 minutes the first time; log: build\build_all.log^)
if not exist build mkdir build
"%BASH%" -c "scripts/build_all.sh 2>&1 | tee build/build_all.log; exit ${PIPESTATUS[0]}"
if errorlevel 1 (
    echo.
    echo  The build failed: see build\build_all.log.
    goto :fail
)
echo.
echo  Optional: FSR 3.1 and DLSS need vendor DLLs that are not included ^(README, "Building", step 5^).
choice /m " Start the launcher now"
if not errorlevel 2 start "" "build\xcde_launcher.exe"
endlocal
exit /b 0

:fail
echo.
pause
endlocal
exit /b 1
