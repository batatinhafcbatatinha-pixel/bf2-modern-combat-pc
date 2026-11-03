@echo off
rem One-click setup: installs the build tools, asks for your game and builds the port. Safe to run again.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
echo.
pause
