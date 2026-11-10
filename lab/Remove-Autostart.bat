@echo off
rem Removes the autostart shortcut again.
del "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\BSI-Quest lab runner.lnk"
echo Autostart removed.
pause
