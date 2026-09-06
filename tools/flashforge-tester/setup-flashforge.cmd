@echo off
setlocal
rem Snapmaker Orca Ultra - Flashforge tester setup
rem Run this from the EdgeSlicer folder (next to EdgeSlicer.exe).
rem It copies the two files the Flashforge connection needs from your
rem Flash Studio installation and turns on debug logging.

cd /d "%~dp0"
if not exist "EdgeSlicer.exe" (
    echo This script must sit in the EdgeSlicer folder, next to EdgeSlicer.exe.
    echo Move it there and run it again.
    pause
    exit /b 1
)

set "FS="
if exist "%ProgramFiles%\Flashforge\Flash Studio Desktop\FlashNetwork.dll" set "FS=%ProgramFiles%\Flashforge\Flash Studio Desktop"
if not defined FS if exist "%ProgramFiles(x86)%\Flashforge\Flash Studio Desktop\FlashNetwork.dll" set "FS=%ProgramFiles(x86)%\Flashforge\Flash Studio Desktop"
if not defined FS if exist "%LOCALAPPDATA%\Orca-Flashforge\FlashNetwork.dll" set "FS=%LOCALAPPDATA%\Orca-Flashforge"

if not defined FS (
    echo Could not find Flash Studio Desktop on this computer.
    echo Please install it first from flashforge.com ^(Downloads ^> Flash Studio^),
    echo then run this script again.
    pause
    exit /b 1
)

echo Found Flash Studio at: %FS%
copy /y "%FS%\FlashNetwork.dll" "FlashNetwork.dll" >nul || goto :copyfail
if not exist "resources\data" mkdir "resources\data"
copy /y "%FS%\resources\data\FLASHNETWORK9.DAT" "resources\data\FLASHNETWORK9.DAT" >nul || goto :copyfail
type nul > "FLASHNETWORK_DEBUG"

echo.
echo Done! Flashforge support is set up with debug logging enabled.
echo Now start EdgeSlicer.exe and follow TESTER_README.md.
pause
exit /b 0

:copyfail
echo Copying failed - try running this script as administrator.
pause
exit /b 1
