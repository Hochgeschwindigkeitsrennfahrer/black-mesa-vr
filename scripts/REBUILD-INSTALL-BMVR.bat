@echo off
setlocal EnableExtensions
cd /d "%~dp0"

REM Point this at the Black Mesa VR source repo if you move it.
set "REPO=C:\Users\amien\Documents\cursor\black-mesa-vr"
set "PS1=%REPO%\scripts\rebuild-install.ps1"

if not exist "%PS1%" (
  echo Rebuild script not found:
  echo   %PS1%
  echo Edit REPO= in this bat if the source folder moved.
  pause
  exit /b 1
)

echo Building and reinstalling Black Mesa VR from:
echo   %REPO%
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
set "ERR=%ERRORLEVEL%"
echo.
pause
exit /b %ERR%
