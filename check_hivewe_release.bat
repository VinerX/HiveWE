@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\check_hivewe_release.ps1" %*
echo.
echo See hivewe_diagnose.txt in the checked release folder.
pause
