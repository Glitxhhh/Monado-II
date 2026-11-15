@echo off
setlocal EnableExtensions DisableDelayedExpansion
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Monado_Windows\tools\windows_driver.ps1" -Action Play %*
exit /b %ERRORLEVEL%
