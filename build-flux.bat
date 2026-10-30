@echo off
rem ---------------------------------------------------------------------------
rem  Builds Flux once (optimized Release build) into build-release\ using CMake
rem  and MinGW Makefiles.  Run it yourself when you want to build or rebuild;
rem  run-flux.bat never builds anything.
rem
rem  If cmake or g++ are not on PATH it looks in the usual places
rem  (C:\Program Files\CMake\bin and C:\msys64\ucrt64\bin) for THIS run only;
rem  your PATH is not changed.  It only writes inside build-release\.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "FLUX_HOME=%~dp0"

where cmake >nul 2>nul || if exist "C:\Program Files\CMake\bin\cmake.exe" set "PATH=C:\Program Files\CMake\bin;%PATH%"
where g++ >nul 2>nul || if exist "C:\msys64\ucrt64\bin\g++.exe" set "PATH=C:\msys64\ucrt64\bin;%PATH%"

where cmake >nul 2>nul
if errorlevel 1 (
    >&2 echo build-flux: cmake was not found. Install CMake, or add its bin folder to PATH.
    exit /b 1
)
where g++ >nul 2>nul
if errorlevel 1 (
    >&2 echo build-flux: g++ was not found. Install MSYS2 with the UCRT64 toolchain, or add it to PATH.
    exit /b 1
)

cmake -S "%FLUX_HOME%." -B "%FLUX_HOME%build-release" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build "%FLUX_HOME%build-release" -j 8
if errorlevel 1 exit /b 1

echo.
echo Built: "%FLUX_HOME%build-release\flux.exe"
echo Next:  "%FLUX_HOME%run-flux.bat" help
exit /b 0
