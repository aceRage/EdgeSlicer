@echo off
rem EdgeSlicer printer emulator: double-click to start it and open its control page.
rem Fake printers on 127.0.0.x only. Never shipped in the EdgeSlicer installer.
setlocal
cd /d "%~dp0"
set "PYCMD="
where py >nul 2>nul && set "PYCMD=py -3"
if not defined PYCMD (
  where python >nul 2>nul && set "PYCMD=python"
)
if not defined PYCMD (
  echo Python 3 was not found.
  echo Install it from https://www.python.org/downloads/ ^(tick "Add python.exe to PATH"^), then double-click this file again.
  pause
  exit /b 1
)
echo Starting the EdgeSlicer printer emulator. Close this window to stop it.
%PYCMD% "%~dp0emu.py" serve %*
if errorlevel 1 pause
endlocal
