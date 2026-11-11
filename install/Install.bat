@echo off
rem Install.bat - double-click starter for Install-BSIQuest.ps1 (the script does the work; this only asks two questions).
rem Game not found automatically? Drag your "BioShock Infinite" folder onto this file instead of double-clicking it.
setlocal
title BSI-Quest installer
cd /d "%~dp0"

echo.
echo  BSI-Quest installer
echo  -------------------
echo  Puts BioShock Infinite (your own Steam copy) on a Meta Quest 3.
echo.
echo  Before you continue:
echo    - BioShock Infinite is installed from Steam on this PC
echo    - the Quest is connected by USB, developer mode is on,
echo      and you accepted "Allow USB debugging" inside the headset
echo    - SideQuest (or adb) is installed
echo.
echo  Nothing in your Steam folder is changed. The first copy is about 42 GB
echo  with DLC (18 GB without) and takes a long time; it is safe to run again
echo  if it is interrupted.
echo.

if not exist "%~dp0Install-BSIQuest.ps1" (
  echo  STOPPED: Install-BSIQuest.ps1 is not next to this file.
  echo  Unzip the whole download first, then start Install.bat from the unzipped folder.
  echo.
  pause
  exit /b 1
)

choice /c VF /n /m " Play mode - V = VR (stereo 3D), F = Flat (big virtual screen, faster)  [V/F]: "
if errorlevel 2 (set "BSI_MODE=Flat") else (set "BSI_MODE=VR")

choice /c YN /n /m " Copy the DLC too (Burial at Sea, Clash in the Clouds)?  [Y/N]: "
if errorlevel 2 (set "BSI_DLC=-NoDLC") else (set "BSI_DLC=")

choice /c IT /n /m " I = install now, T = test only (shows what would happen, changes nothing)  [I/T]: "
if errorlevel 2 (set "BSI_DRY=-DryRun") else (set "BSI_DRY=")

echo.
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-BSIQuest.ps1" -Mode %BSI_MODE% %BSI_DLC% %BSI_DRY%
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-BSIQuest.ps1" -Game "%~1" -Mode %BSI_MODE% %BSI_DLC% %BSI_DRY%
)
set "BSI_RC=%ERRORLEVEL%"

echo.
if not "%BSI_RC%"=="0" (
  echo  The installer stopped. Read the red STOPPED line above for the reason.
  echo  If it could not find the game: drag your "BioShock Infinite" folder onto Install.bat.
  echo  Still stuck? Open an issue and paste the text of this window:
  echo    https://github.com/egomortal/bsi-quest/issues
)
echo.
pause
exit /b %BSI_RC%
