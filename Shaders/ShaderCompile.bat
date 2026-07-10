@echo off
setlocal enabledelayedexpansion

set DXC=Shaders\Build\dxc.exe
set BIN2C=Shaders\Build\bin2c.exe

set SHADER_DIR=Shaders
set SPV_DIR=Shaders\spv

rem One HLSL file per graphics pass:
rem FileName:VertexEntry:FragmentEntry
set GRAPHICS_SHADERS=SurfaceForward:vert:frag SkinnedForward:vert:frag SurfaceDepthOnly:vert:frag SkinnedDepthOnly:vert:frag LineDebug:vert:frag Outline:vert:frag UI\Slug:vert:frag UI\UIShape:vert:frag UI\UIImage:vert:frag Shadow\SurfaceShadowDepthOnly:vert:frag Shadow\SkinnedShadowDepthOnly:vert:frag Shadow\SurfacePointShadowDepthOnly:vert:frag Shadow\SkinnedPointShadowDepthOnly:vert:frag TerrainForward:vert:frag TerrainDepthOnly:vert:frag TerrainChunk:vert:frag

rem Extra single-stage graphics passes with custom entry/output/target:
rem FileName:OutputName:Entry:Target
set EXTRA_GRAPHICS_SHADERS=UI\Slug:Slug2DVert:vert2d:vs_6_6 TerrainDepthOnly:TerrainWireFrag:wireFrag:ps_6_6 Shadow\SurfaceSpotShadowDepthOnly:Shadow\SurfaceSpotShadowDepthOnlyVert:vert:vs_6_6 Shadow\SkinnedSpotShadowDepthOnly:Shadow\SkinnedSpotShadowDepthOnlyVert:vert:vs_6_6

rem One HLSL file per compute pass:
rem FileName:ComputeEntry
set COMPUTE_SHADERS=TexturePageCopyRGBA:main TexturePageCopyRG:main UI\ColorPickCompute:main ReconstructNormalCompute:main PreProcessing\BuildLightGridCompute:main Animation\AnimationCompute:main Animation\AnimateVertices:main PreProcessing\CullDrawArgsCompute:main PreProcessing\HiZBuildCompute:main PreProcessing\HiZDownscaleCompute:main PostProcessing\TonemapCompute:main PostProcessing\BloomPrefilterDownsampleCompute:main PostProcessing\BloomUpsampleCompute:main PostProcessing\HBAOCompute:main PostProcessing\HBAOBlurCompute:main PostProcessing\ContactShadowsCompute:main PostProcessing\MLAAEdgeMaskCompute:main PostProcessing\MLAALineLengthCompute:main PostProcessing\MLAABlendCompute:main

if not exist %SPV_DIR% mkdir %SPV_DIR%
if not exist %SPV_DIR%\UI mkdir %SPV_DIR%\UI
if not exist %SPV_DIR%\Shadow mkdir %SPV_DIR%\Shadow
if not exist %SPV_DIR%\Animation mkdir %SPV_DIR%\Animation
if not exist %SPV_DIR%\PreProcessing mkdir %SPV_DIR%\PreProcessing
if not exist %SPV_DIR%\PostProcessing mkdir %SPV_DIR%\PostProcessing

echo [1/2] Compiling graphics shaders...

for %%S in (%GRAPHICS_SHADERS%) do (
	for /f "tokens=1,2,3 delims=:" %%A in ("%%S") do (
		echo Compiling %%A.hlsl vertex entry %%B...

		%DXC% -spirv -fspv-target-env=vulkan1.1 -T vs_6_6 -E %%B -enable-16bit-types %SHADER_DIR%\%%A.hlsl -Fo %SHADER_DIR%\%%AVert.spv
		if !ERRORLEVEL! neq 0 (
			echo [ERROR] Failed to compile %%A.hlsl vertex entry %%B
			pause
			exit /b !ERRORLEVEL!
		)

		echo Compiling %%A.hlsl fragment entry %%C...

		%DXC% -spirv -fspv-target-env=vulkan1.1 -T ps_6_6 -E %%C -enable-16bit-types %SHADER_DIR%\%%A.hlsl -Fo %SHADER_DIR%\%%AFrag.spv
		if !ERRORLEVEL! neq 0 (
			echo [ERROR] Failed to compile %%A.hlsl fragment entry %%C
			pause
			exit /b !ERRORLEVEL!
		)
	)
)

echo Compiling extra graphics shaders...

for %%S in (%EXTRA_GRAPHICS_SHADERS%) do (
	for /f "tokens=1,2,3,4 delims=:" %%A in ("%%S") do (
		echo Compiling %%A.hlsl entry %%C -^> %%B...

		%DXC% -spirv -fspv-target-env=vulkan1.1 -T %%D -E %%C -enable-16bit-types %SHADER_DIR%\%%A.hlsl -Fo %SHADER_DIR%\%%B.spv
		if !ERRORLEVEL! neq 0 (
			echo [ERROR] Failed to compile %%A.hlsl entry %%C
			pause
			exit /b !ERRORLEVEL!
		)
	)
)

echo Compiling compute shaders...

for %%S in (%COMPUTE_SHADERS%) do (
	for /f "tokens=1,2 delims=:" %%A in ("%%S") do (
		echo Compiling %%A.hlsl compute entry %%B...

		%DXC% -spirv -fspv-target-env=vulkan1.1 -T cs_6_6 -E %%B -enable-16bit-types %SHADER_DIR%\%%A.hlsl -Fo %SHADER_DIR%\%%A.spv
		if !ERRORLEVEL! neq 0 (
			echo [ERROR] Failed to compile %%A.hlsl compute entry %%B
			pause
			exit /b !ERRORLEVEL!
		)
	)
)

echo [2/2] Generating C headers...

for %%S in (%GRAPHICS_SHADERS%) do (
	for /f "tokens=1,2,3 delims=:" %%A in ("%%S") do (
		%BIN2C% -o %SHADER_DIR%\%%AVert.spv.h %SHADER_DIR%\%%AVert.spv
		if !ERRORLEVEL! neq 0 goto :bin_error
		move /Y %SHADER_DIR%\%%AVert.spv.h %SPV_DIR%\%%AVert.spv.h >NUL

		%BIN2C% -o %SHADER_DIR%\%%AFrag.spv.h %SHADER_DIR%\%%AFrag.spv
		if !ERRORLEVEL! neq 0 goto :bin_error
		move /Y %SHADER_DIR%\%%AFrag.spv.h %SPV_DIR%\%%AFrag.spv.h >NUL
	)
)

for %%S in (%EXTRA_GRAPHICS_SHADERS%) do (
	for /f "tokens=1,2,3,4 delims=:" %%A in ("%%S") do (
		%BIN2C% -o %SHADER_DIR%\%%B.spv.h %SHADER_DIR%\%%B.spv
		if !ERRORLEVEL! neq 0 goto :bin_error
		move /Y %SHADER_DIR%\%%B.spv.h %SPV_DIR%\%%B.spv.h >NUL
	)
)

for %%S in (%COMPUTE_SHADERS%) do (
	for /f "tokens=1,2 delims=:" %%A in ("%%S") do (
		%BIN2C% -o %SHADER_DIR%\%%A.spv.h %SHADER_DIR%\%%A.spv
		if !ERRORLEVEL! neq 0 goto :bin_error
		move /Y %SHADER_DIR%\%%A.spv.h %SPV_DIR%\%%A.spv.h >NUL
	)
)

del %SHADER_DIR%\*.spv

echo.
echo SUCCESS: All shaders compiled and headers generated.
exit /b 0

:bin_error
echo [ERROR] bin2c failed to process files.
pause
exit /b 1
