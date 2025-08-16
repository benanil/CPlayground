@echo off
echo Compiling shaders with sokol-shdc...
rem sokol-shdc --input Triangle.glsl --output Triangle.h --slang hlsl5
sokol-shdc --input Shaders/Cube.glsl --output Shaders/Cube.glsl.h --slang hlsl5
exit /b 0