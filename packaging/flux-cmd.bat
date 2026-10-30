@echo off
rem ---------------------------------------------------------------------------
rem  Opens a Flux command line: a prompt in which "flux" works, wherever Flux is installed.
rem  The folder that holds this file is added to PATH for THIS window only; PATH is never
rem  changed permanently and nothing else is modified.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "PATH=%~dp0;%PATH%"
title Flux command line
echo Flux command line. Try:  flux help
echo.
cmd /k flux --version
