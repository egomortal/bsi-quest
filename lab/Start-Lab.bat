@echo off
title BSI-Quest lab runner
rem Starts the lab runner. Leave this window open (minimise it). Close it or run Stop-Lab.bat to stop.
rem Exit code 3 = the runner was updated by Claude: start the new version.
:again
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0lab_runner.ps1"
if %errorlevel%==3 goto again
echo.
echo Lab runner stopped.
pause
