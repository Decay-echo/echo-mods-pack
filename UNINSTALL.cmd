@echo off
rem Echo Mods Pack: double-click to put Echo VR back to its own files (close Echo VR first).
setlocal
cd /d "%~dp0"
title Echo Mods Pack - Uninstall
set "PY="
py -3 --version >nul 2>nul && set "PY=py -3"
if not defined PY python --version >nul 2>nul && set "PY=python"
if not defined PY (
  echo Python 3 is needed to uninstall: install it from https://www.python.org/downloads/
  echo.
  pause
  exit /b 1
)
%PY% install_pack.py --uninstall %*
set ERR=%errorlevel%
echo.
pause
exit /b %ERR%
