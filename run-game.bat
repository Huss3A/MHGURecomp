@echo off
rem Starts MHGU Recomp: the game files are expected in ..\mhgu\game (exefs\ and romfs\), saves go to run\save\.
rem Needs MSYS2's UCRT64 DLLs (SDL3, FFmpeg, shaderc...) from C:\msys64\ucrt64\bin.
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
set MHGU_VSYNC=0
if not exist "%~dp0run" mkdir "%~dp0run"
cd /d "%~dp0run"
"..\build\mhgurecomp.exe" "..\..\mhgu\game" 2> log.txt
if errorlevel 1 (
    echo The game exited with an error. See run\log.txt
    pause
)
