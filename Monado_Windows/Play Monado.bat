@echo off
setlocal EnableExtensions DisableDelayedExpansion
call "%~dp0..\Play_Monado_II.bat" %*
exit /b %ERRORLEVEL%
