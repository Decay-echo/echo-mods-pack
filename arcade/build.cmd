@echo off
rem Builds EchoArcade.dll (plugin), ArcadeHost.exe and the tests into dist\.
setlocal
if not defined VCVARS (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VCVARS%" (echo ERROR: Visual Studio C++ tools not found & exit /b 1)
call "%VCVARS%" >nul 2>nul
cd /d "%~dp0"
rem Lobby poster table (DOCK) comes from your game data; build it once if missing.
if not exist native\generated\posters.h (
  .venv\Scripts\python tools\build_poster_table.py
  if errorlevel 1 exit /b 1
)
if not exist build\obj mkdir build\obj
if not exist dist\EchoArcade mkdir dist\EchoArcade
set CFLAGS=/nologo /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE /wd4100 /wd4201

cl /nologo /O2 /MT /c /TC native\vendor\minhook\src\buffer.c native\vendor\minhook\src\hook.c native\vendor\minhook\src\trampoline.c native\vendor\minhook\src\hde\hde64.c /Fo:build\obj\
if errorlevel 1 exit /b 1

cl %CFLAGS% /LD native\runtime\runtime.cpp native\runtime\d3d12_stream.cpp native\runtime\posters.cpp native\runtime\controllers.cpp native\runtime\ovr_tweaks.cpp native\runtime\tablet_scale.cpp build\obj\buffer.obj build\obj\hook.obj build\obj\trampoline.obj build\obj\hde64.obj /Fo:build\obj\ /Fe:dist\EchoArcade.dll /link /IMPLIB:build\obj\EchoArcade.lib user32.lib
if errorlevel 1 exit /b 1

cl /nologo /O2 /MT /W4 /EHsc /std:c++20 /DUNICODE /D_UNICODE /wd4100 /wd4201 host\main.cpp host\canvas.cpp host\capture.cpp host\apps.cpp host\playlists.cpp host\mpv.cpp host\plex.cpp host\youtube.cpp host\screen.cpp host\audio.cpp host\lightgun.cpp /Fo:build\obj\ /Fe:dist\EchoArcade\ArcadeHost.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib d3d11.lib dxgi.lib ws2_32.lib windowsapp.lib dwmapi.lib shell32.lib ole32.lib oleaut32.lib runtimeobject.lib
if errorlevel 1 exit /b 1

cl %CFLAGS% tests\ipc_test.cpp /Fo:build\obj\ /Fe:build\ipc_test.exe
if errorlevel 1 exit /b 1
cl %CFLAGS% tests\settings_test.cpp /Fo:build\obj\ /Fe:build\settings_test.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /EHsc /std:c++20 /DUNICODE /D_UNICODE /wd4100 /wd4201 tests\freeze_test.cpp /Fo:build\obj\ /Fe:build\freeze_test.exe /link ws2_32.lib
if errorlevel 1 exit /b 1
cl %CFLAGS% tests\tablet_scan_test.cpp native\runtime\tablet_scale.cpp /Fo:build\obj\ /Fe:build\tablet_scan_test.exe
if errorlevel 1 exit /b 1
cl %CFLAGS% tests\poster_math_test.cpp build\obj\buffer.obj build\obj\hook.obj build\obj\trampoline.obj build\obj\hde64.obj /Fo:build\obj\ /Fe:build\poster_math_test.exe
if errorlevel 1 exit /b 1
rem tests log under build\ instead of the real %LOCALAPPDATA%\EchoArcade
set "LOCALAPPDATA=%~dp0build"
build\ipc_test.exe
if errorlevel 1 exit /b 1
build\freeze_test.exe
if errorlevel 1 exit /b 1
build\settings_test.exe
if errorlevel 1 exit /b 1
build\tablet_scan_test.exe
if errorlevel 1 exit /b 1
build\poster_math_test.exe
exit /b %errorlevel%
