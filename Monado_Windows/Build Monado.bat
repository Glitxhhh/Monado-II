@echo off
setlocal EnableExtensions DisableDelayedExpansion
call "%~dp0..\Build_Monado_II.bat" %*
exit /b %ERRORLEVEL%
