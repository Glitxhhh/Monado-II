@echo off
rem Monado: launcher (graphics, scaling, controls, language), then the game.
cd /d "%~dp0"
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
start "" "build\xcde_launcher.exe"
