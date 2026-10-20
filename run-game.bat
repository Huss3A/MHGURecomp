@echo off
rem Starts MHGU Recomp from the build folder, like double-clicking build\mhgurecomp.exe (its DLLs are beside it, the
rem game files in build\data\exefs and build\data\romfs; saves go to %APPDATA%\MHGURecomp\save\), with classic vsync and the log in
rem build\log.txt.
set MHGU_VSYNC=1
cd /d "%~dp0build"
"%~dp0build\mhgurecomp.exe" 2> log.txt
if errorlevel 1 (
    echo The game exited with an error. See build\log.txt
    pause
)
