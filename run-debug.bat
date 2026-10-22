@echo off
rem Like run-game.bat, with renderer debugging on: F10 in the game window dumps every render pass of the next 2 frames
rem to build\shots\ (takes a few seconds), shaders are saved to build\build\glsl\ and the log is build\log_debug.txt.
set MHGU_DUMP_VERBOSE=1
set MHGU_SAVE_GLSL=1
set MHGU_SAMPLER_LOG=1
cd /d "%~dp0build"
"%~dp0build\mhgurecomp.exe" 2> log_debug.txt
if errorlevel 1 (
    echo The game exited with an error. See build\log_debug.txt
    pause
)
