@echo off
rem Switch-Mode.bat - double-click starter for Set-Mode.ps1: choose VR or Flat. Close the game on the headset first.
setlocal
title BSI-Quest display mode
cd /d "%~dp0"
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Set-Mode.ps1" -Mode Show
if errorlevel 1 goto end
echo.
choice /c VFK /n /m " V = VR (stereo 3D), F = Flat (big virtual screen), K = keep as it is  [V/F/K]: "
if errorlevel 3 goto end
if errorlevel 2 (set "BSI_MODE=Flat") else (set "BSI_MODE=VR")
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Set-Mode.ps1" -Mode %BSI_MODE%
:end
echo.
pause
