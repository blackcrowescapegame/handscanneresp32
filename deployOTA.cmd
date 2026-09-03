@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\deploy_ota.ps1" %*
exit /b %ERRORLEVEL%
