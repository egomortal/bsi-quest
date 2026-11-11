@echo off
rem Uninstall.bat - double-click starter for Uninstall-BSIQuest.ps1: removes the app cleanly so the next install starts fresh.
setlocal
title BSI-Quest uninstall
cd /d "%~dp0"
echo.
echo  BSI-Quest clean uninstall
echo  -------------------------
echo  Removes the app and its folder on the headset (with the game data copied there).
echo  Keeps your saves and settings in /sdcard/BSI. Nothing on this PC is changed.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-BSIQuest.ps1" -DryRun
if errorlevel 1 goto end
echo.
choice /c YN /n /m " Remove the app now?  [Y/N]: "
if errorlevel 2 goto end
choice /c NY /n /m " Also remove game data in /sdcard/BSI/BioShock Infinite, if there is any? Saves are kept either way.  [N/Y]: "
if errorlevel 2 (set "BSI_GD=-GameData") else (set "BSI_GD=")
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-BSIQuest.ps1" %BSI_GD%
:end
echo.
pause
