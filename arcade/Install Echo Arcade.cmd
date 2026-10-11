@echo off
rem Echo Arcade: the INSTALL button. Builds (when Visual Studio's C++ tools are here), patches
rem RetroArch, removes any earlier Echo Arcade install and installs this copy. Close Echo VR first.
setlocal
cd /d "%~dp0"
title Echo Arcade - Install
echo.
echo   ECHO ARCADE - INSTALL
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
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "HAVE_VS="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "HAVE_VS=%%i"
if defined HAVE_VS (
  echo Building EchoArcade.dll and ArcadeHost.exe...
  call build.cmd
  if errorlevel 1 (
    echo.
    echo BUILD FAILED - nothing was installed.
    goto :fail
  )
) else (
  if not exist dist\EchoArcade\ArcadeHost.exe (
    echo Visual Studio C++ tools were not found and dist\ has no build to install.
    goto :fail
  )
  echo Visual Studio C++ tools not found; installing the binaries already in dist\.
)
echo.
.venv\Scripts\python tools\install.py setup
if errorlevel 1 goto :fail
echo.
echo Done. Start Echo VR, open the hand tablet and press the gamepad tab (far left).
echo.
pause
exit /b 0

:fail
echo.
echo Stopped. Scroll up for the reason; nothing more was changed.
echo.
pause
exit /b 1
