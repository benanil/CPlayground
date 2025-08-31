
@echo off

echo Shaders compiling...
call "ShaderCompile.bat"
if %ERRORLEVEL% neq 0 (
    echo Failed to compile shaders!
    exit /b %ERRORLEVEL%
)

rem REM <<<  CLANG COMPILER  >>>
rem clang -O3 -mavx2 -mfma -msse4.2 -mf16c -mrdseed -s -fno-rtti -fno-stack-protector -Wimplicit-function-declaration -fno-exceptions -fno-unwind-tables -static-libgcc ^
rem Main.c Extern/rpmalloc.c  -o MainCLANG.exe -I. -ladvapi32 -ld3d11 -ldxgi -ldxguid -luser32 -lshell32 -lgdi32 -lwinmm
rem start "" "MainCLANG.exe"

rem rem REM <<<  MSVC COMPILER  >>>
REM Set up the MSVC environment (adjust the path to your Visual Studio installation)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if %ERRORLEVEL% neq 0 (
    echo Failed to set up MSVC environment!
    exit /b %ERRORLEVEL%
)

if "%1"=="Debug" (
	REM Compile Main.c in debug mode with AVX2, debug info, and link libraries
	cl.exe /Od /Zi /arch:AVX2 /std:c11 /GR- /EHsc- /I. Main.c /Fe:build/MainDebug.exe /link advapi32.lib d3d11.lib dxgi.lib dxguid.lib user32.lib shell32.lib gdi32.lib winmm.lib
	if %ERRORLEVEL% neq 0 (       
		echo Compilation failed!  
		exit /b %ERRORLEVEL%      
	)                             
    start "" "build/MainDebug.exe"
) else (                          
	REM Compile Main.c with optimizations, instruction sets, and link libraries        
	cl.exe /O2 /arch:AVX2 /std:c11 /GR- /GS- /EHsc- /I. Main.c /Fe:build/MainMSVC.exe /link advapi32.lib d3d11.lib dxgi.lib dxguid.lib user32.lib shell32.lib gdi32.lib winmm.lib
	if %ERRORLEVEL% neq 0 (                                                            
		echo Compilation failed!
		exit /b %ERRORLEVEL%
	)
	start "" "build/MainMSVC.exe"
)