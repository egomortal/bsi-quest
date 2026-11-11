@echo off
rem Send-Report.bat - double-click starter for Send-Report.ps1. It asks before it does anything and uploads nothing:
rem it makes a zip on this PC that you can attach to an issue yourself. See REPORTS.md for what is in it and why.
setlocal
title BSI-Quest play report
cd /d "%~dp0"
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Send-Report.ps1"
echo.
pause
