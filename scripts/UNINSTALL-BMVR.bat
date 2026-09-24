@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PS1=%~dp0UNINSTALL-BMVR.ps1"
if not exist "%PS1%" (
  echo Could not find UNINSTALL-BMVR.ps1 next to this bat.
  echo Copy both UNINSTALL-BMVR.bat and UNINSTALL-BMVR.ps1 into the Black Mesa folder.
  pause
  exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
set "ERR=%ERRORLEVEL%"
echo.
pause
exit /b %ERR%
