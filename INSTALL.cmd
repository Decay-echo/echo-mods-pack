@echo off
rem Echo Mods Pack: double-click to install (close Echo VR first).
setlocal
cd /d "%~dp0"
title Echo Mods Pack - Install
set "PY="
py -3 --version >nul 2>nul && set "PY=py -3"
if not defined PY python --version >nul 2>nul && set "PY=python"
if not defined PY (
  echo Python 3 is needed: install it from https://www.python.org/downloads/
  echo ^(tick "Add python.exe to PATH" in the installer^), then run this again.
  echo.
  pause
  exit /b 1
)
%PY% install_pack.py %*
set ERR=%errorlevel%
echo.
pause
exit /b %ERR%
