@echo off
rem Asks the lab runner to stop: the current job is aborted, its cleanup steps run, then the runner exits.
echo stop> "%~dp0STOP"
echo Stop requested. The runner window closes its job and stops within a few seconds.
timeout /t 5
