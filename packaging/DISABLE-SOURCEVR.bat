@echo off
cd /d "%~dp0"
if not exist "bms.exe" (
  echo Run this from the Black Mesa folder ^(the one that contains bms.exe^).
  pause
  exit /b 1
)
if exist "bin\sourcevr.dll" (
  if not exist "bin\sourcevr.dll.disabled" (
    move "bin\sourcevr.dll" "bin\sourcevr.dll.disabled"
  ) else (
    del "bin\sourcevr.dll"
  )
  echo Disabled bin\sourcevr.dll ^(renamed to sourcevr.dll.disabled^).
) else (
  echo bin\sourcevr.dll is already absent.
)
pause
