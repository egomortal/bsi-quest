@echo off
rem Optional: start the lab runner automatically (minimised) when you log in to Windows.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Startup')+'\BSI-Quest lab runner.lnk'); $s.TargetPath='%~dp0Start-Lab.bat'; $s.WorkingDirectory='%~dp0'; $s.WindowStyle=7; $s.Save(); Write-Host 'Autostart shortcut created in your Startup folder.'"
pause
