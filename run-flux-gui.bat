@echo off
rem ---------------------------------------------------------------------------
rem  Starts the Flux desktop app (flux-gui.exe) and returns immediately.
rem
rem  It does not build anything or change PATH.  Build once with build-flux.bat.
rem
rem  Which flux-gui.exe is used (first match wins):
rem    1. the file named by the FLUX_GUI_EXE environment variable
rem    2. build-release\flux-gui.exe   next to this script  (optimized; preferred)
rem    3. build\flux-gui.exe           next to this script
rem
rem  The command-line tool is unaffected: use run-flux.bat for that.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "FLUX_HOME=%~dp0"
set "FLUX_GUI_BIN="

if defined FLUX_GUI_EXE set "FLUX_GUI_BIN=%FLUX_GUI_EXE%"
if not defined FLUX_GUI_BIN if exist "%FLUX_HOME%build-release\flux-gui.exe" set "FLUX_GUI_BIN=%FLUX_HOME%build-release\flux-gui.exe"
if not defined FLUX_GUI_BIN if exist "%FLUX_HOME%build\flux-gui.exe" set "FLUX_GUI_BIN=%FLUX_HOME%build\flux-gui.exe"

if not defined FLUX_GUI_BIN goto :notbuilt
if not exist "%FLUX_GUI_BIN%" goto :notbuilt

start "" "%FLUX_GUI_BIN%" %*
exit /b 0

:notbuilt
>&2 echo flux: could not find flux-gui.exe.
if defined FLUX_GUI_EXE >&2 echo       FLUX_GUI_EXE points to "%FLUX_GUI_EXE%", which does not exist.
>&2 echo       The desktop app has not been built yet. Build it once by running:
>&2 echo           "%FLUX_HOME%build-flux.bat"
>&2 echo       or set the FLUX_GUI_EXE environment variable to the full path of flux-gui.exe.
exit /b 1
