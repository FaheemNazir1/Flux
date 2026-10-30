@echo off
rem ---------------------------------------------------------------------------
rem  Flux launcher.  Runs flux.exe from wherever it was built, whatever the
rem  current directory is, and passes every argument through unchanged.
rem
rem    run-flux.bat root add "C:\Users\<you>\Documents"
rem    run-flux.bat sync --all
rem    run-flux.bat search invoice 2024
rem
rem  It does not build anything, download anything, or change PATH.  Build once
rem  with build-flux.bat.  Quote any argument that contains spaces.
rem
rem  Which flux.exe is used (first match wins):
rem    1. the file named by the FLUX_EXE environment variable
rem    2. build-release\flux.exe   next to this script  (optimized; preferred)
rem    3. build\flux.exe           next to this script
rem
rem  The exit code of flux.exe is returned unchanged.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "FLUX_HOME=%~dp0"
set "FLUX_BIN="

if defined FLUX_EXE set "FLUX_BIN=%FLUX_EXE%"
if not defined FLUX_BIN if exist "%FLUX_HOME%build-release\flux.exe" set "FLUX_BIN=%FLUX_HOME%build-release\flux.exe"
if not defined FLUX_BIN if exist "%FLUX_HOME%build\flux.exe" set "FLUX_BIN=%FLUX_HOME%build\flux.exe"

if not defined FLUX_BIN goto :notbuilt
if not exist "%FLUX_BIN%" goto :notbuilt

"%FLUX_BIN%" %*
exit /b %ERRORLEVEL%

:notbuilt
>&2 echo flux: could not find flux.exe.
if defined FLUX_EXE >&2 echo       FLUX_EXE points to "%FLUX_EXE%", which does not exist.
>&2 echo       Flux has not been built yet. Build it once by running:
>&2 echo           "%FLUX_HOME%build-flux.bat"
>&2 echo       or set the FLUX_EXE environment variable to the full path of flux.exe.
exit /b 1
