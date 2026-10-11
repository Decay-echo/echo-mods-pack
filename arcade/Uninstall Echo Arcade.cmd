@echo off
rem Echo Arcade: the UNINSTALL button. Puts the original game files back and removes everything
rem Echo Arcade added, whichever copy of this folder installed it. Close Echo VR first.
setlocal
cd /d "%~dp0"
title Echo Arcade - Uninstall
echo.
echo   ECHO ARCADE - UNINSTALL
echo   from: %CD%
echo.
if not exist .venv\Scripts\python.exe (
  echo The Python environment is missing: .venv\Scripts\python.exe
  echo See README.md, section Install.
  goto :fail
)
tasklist /FI "IMAGENAME eq echovr.exe" | find /I "echovr.exe" >nul
if not errorlevel 1 (
  echo Echo VR is running. Close it, then run this again.
  goto :fail
)
.venv\Scripts\python tools\install.py uninstall
if errorlevel 1 goto :fail
echo.
echo Done. Echo VR is back to its original files.
echo.
pause
exit /b 0

:fail
echo.
echo Stopped. Scroll up for the reason; nothing more was changed.
echo.
pause
exit /b 1
