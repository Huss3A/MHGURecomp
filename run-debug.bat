@echo off
rem Like run-game.bat, with renderer debugging on: F10 in the game window dumps every render pass of the next 2 frames
rem to run\shots\ (takes a few seconds), shaders are saved to run\build\glsl\ and the log is run\log_debug.txt.
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
set MHGU_VSYNC=1
set MHGU_DUMP_VERBOSE=1
set MHGU_SAVE_GLSL=1
set MHGU_SAMPLER_LOG=1
if not exist "%~dp0run" mkdir "%~dp0run"
cd /d "%~dp0run"
"..\build\mhgurecomp.exe" "..\..\mhgu\game" 2> log_debug.txt
if errorlevel 1 (
    echo The game exited with an error. See run\log_debug.txt
    pause
)
