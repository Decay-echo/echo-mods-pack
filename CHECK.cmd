@echo off
rem Echo Mods Pack: what is installed and whether it ran (writes check.txt to paste when asking for help).
setlocal
cd /d "%~dp0"
title Echo Mods Pack - Check
set "PY="
py -3 --version >nul 2>nul && set "PY=py -3"
if not defined PY python --version >nul 2>nul && set "PY=python"
if not defined PY (echo Python 3 is not installed: get it from python.org and tick "Add python.exe to PATH". & pause & exit /b 1)
%PY% install_pack.py --check
echo.
pause
