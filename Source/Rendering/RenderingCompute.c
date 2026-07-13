#include "RenderingInternal.h"
#include "bend_sss_cpu.h"
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_time.h>

void DispatchCullDrawArgsCompute(SDL_GPUCommandBuffer* cmd, RenderSet* renderSet,
                                 RenderSetBuffers* buffers,
                                 FrustumPlanes     frustumPlanes,
                                 mat4x4            viewProj,
                                 CullDrawFlags     flags,
                                 u32               forcedLOD,
                                 u32               instanceMultiplier,
                                 const f32         cullSphere[4])
{
    if (renderSet->numGroups == 0) return;
    CHECK_CREATE(g_CullDrawArgsComputePipeline, "Cull Draw Args Compute Pipeline");
    struct {
        FrustumPlanes planes;
        u32 numEntities;
        u32 numPrimitiveGroups;
        u32 mode;
        u32 flags;
        mat4x4 viewProjection;
        u32 hiZSize[2];
        u32 hiZMipCount;
        f32 hiZDepthBias;
        u32 lodCount;
        u32 sparseIndexLODStride;
        u32 forcedLOD;
        f32 lodDistanceModifier;
        u32 instanceMultiplier;
        u32 padding[3];
        f32 cullSphere[4];
    } params;

    WindowState* winstate = &g_WindowState;
    SDL_GPUTexture* hiZTexture = winstate->tex_hiz;
    u32 hiZWidth = winstate->hiz_width;
    u32 hiZHeight = winstate->hiz_height;
    u32 hiZMipCount = winstate->hiz_mip_count;
    MemCopy(&params.planes, frustumPlanes.planes, sizeof(FrustumPlanes));
    params.numEntities = renderSet->numEntities;
    params.numPrimitiveGroups = renderSet->numGroups;
    params.mode = 0;
    params.flags = flags;
    params.viewProjection = viewProj;
    params.hiZSize[0] = hiZWidth;
    params.hiZSize[1] = hiZHeight;
    params.hiZMipCount = hiZMipCount;
    params.hiZDepthBias = 0.02f;
    params.lodCount = ((flags & CullDrawFlag_EnableLODSelection) != 0u || forcedLOD < MESH_LOD_COUNT) ? MESH_LOD_COUNT : 1u;
    params.sparseIndexLODStride = renderSet->maxEntities;
    params.forcedLOD = forcedLOD;
    params.lodDistanceModifier = Maxf32(g_RenderSettings.lodDistanceModifier, 0.001f);
    params.instanceMultiplier = Maxu32(instanceMultiplier, 1u);
    params.padding[0] = params.padding[1] = params.padding[2] = 0u;
    if (cullSphere) MemCopy(params.cullSphere, cullSphere, sizeof(params.cullSphere));
    else SDL_zero(params.cullSphere);

    SDL_GPUBuffer* ro_buffers[3] = {
        buffers->entity,
        buffers->primitiveGroup,
        buffers->primitiveGroupLOD
    };
    SDL_GPUStorageBufferReadWriteBinding rw_bindings[8] = {
        { buffers->drawSparseIndices },
        { buffers->drawArgs },
        { g_RenderState.lineBuffer },
        { g_RenderState.lineDrawArgsBuffer },
        { buffers->visibleSparseIndices },
        { buffers->visibilityMask },
        { buffers->visibleCount },
        { buffers->dispatchArgs }
    };

    // The reset (mode 0) and cull (mode 1) dispatches share RW buffers: mode 0 plain-writes
    // drawArgs[].numInstances/visibilityMask/visibleCount/dispatchArgs to zero, while mode 1
    // atomically accumulates into the same locations. SDL_GPU does NOT synchronize dispatches
    // within a single compute pass (see SDL_DispatchGPUCompute docs), so running them in one
    // pass lets the reset's zeroing store land after mode 1's atomics, clobbering a group's
    // instance count to 0 for the frame -> intermittent flicker (worst for high-LOD draw slots,
    // i.e. small/thin/distant objects). End the reset pass before beginning the cull pass.
    u32 resetCount = renderSet->numGroups * params.lodCount;
    if ((flags & CullDrawFlag_VisibilityOutput) != 0u && renderSet->numEntities > resetCount) resetCount = renderSet->numEntities;

    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, NULL, 0, rw_bindings, SDL_arraysize(rw_bindings));
    SDL_BindGPUComputePipeline(pass, g_CullDrawArgsComputePipeline);
    SDL_BindGPUComputeStorageBuffers(pass, 0, ro_buffers, SDL_arraysize(ro_buffers));
    if (hiZTexture)
        SDL_BindGPUComputeStorageTextures(pass, 0, &hiZTexture, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, (resetCount + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(pass);

    if (renderSet->numEntities > 0)
    {
        params.mode = 1;
        pass = SDL_BeginGPUComputePass(cmd, NULL, 0, rw_bindings, SDL_arraysize(rw_bindings));
        SDL_BindGPUComputePipeline(pass, g_CullDrawArgsComputePipeline);
        SDL_BindGPUComputeStorageBuffers(pass, 0, ro_buffers, SDL_arraysize(ro_buffers));
        if (hiZTexture)
            SDL_BindGPUComputeStorageTextures(pass, 0, &hiZTexture, 1);
        SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
        SDL_DispatchGPUCompute(pass, (renderSet->numEntities + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
}

void DispatchHiZBuildCompute(SDL_GPUCommandBuffer* cmd)
{
    WindowState* winstate = &g_WindowState;
    SDL_GPUTexture* depthTexture = winstate->tex_hiz_depth; 
    SDL_GPUTexture* hiZTexture = winstate->tex_hiz;
    u32 width    = winstate->hiz_width;
    u32 height   = winstate->hiz_height;
    u32 mipCount = winstate->hiz_mip_count;
    if (!depthTexture || !hiZTexture || mipCount == 0) return;
    for (u32 mip = 0; mip < mipCount; mip++)
    {
        u32 outputWidth  = Maxu32(width >> mip, 1);
        u32 outputHeight = Maxu32(height >> mip, 1);
        u32 sourceWidth  = Maxu32((mip == 0) ? width  : (width >> (mip - 1)), 1);
        u32 sourceHeight = Maxu32((mip == 0) ? height : (height >> (mip - 1)), 1);

        SDL_GPUStorageTextureReadWriteBinding rwTexture = {
            .texture = hiZTexture,
            .mip_level = mip,
            .layer = 0,
            .cycle = false
        };
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &rwTexture, 1, NULL, 0);
        struct { u32 sourceSize[2]; u32 outputSize[2]; u32 sourceMip; u32 isBaseLevel; u32 padding[2]; } params = {
            { sourceWidth, sourceHeight },
            { outputWidth, outputHeight },
            mip == 0 ? 0u : mip - 1u,
            mip == 0 ? 1u : 0u,
            { 0u, 0u }
        };

        SDL_GPUTextureSamplerBinding depthBinding = { .texture = depthTexture, .sampler = g_RenderState.hiZSampler };
        SDL_BindGPUComputeSamplers(pass, 0, &depthBinding, 1);
        if (mip == 0)
        {
            CHECK_CREATE(g_HiZBuildComputePipeline, "Hi-Z Build Compute Pipeline")
            SDL_BindGPUComputePipeline(pass, g_HiZBuildComputePipeline);
        }
        else
        {
            CHECK_CREATE(g_HiZDownscaleComputePipeline, "Hi-Z Downscale Compute Pipeline");
            SDL_BindGPUComputePipeline(pass, g_HiZDownscaleComputePipeline);
            SDL_BindGPUComputeStorageTextures(pass, 0, &hiZTexture, 1);
        }
        SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
        SDL_DispatchGPUCompute(pass, (outputWidth + 7u) / 8u, (outputHeight + 7u) / 8u, 1);
        SDL_EndGPUComputePass(pass);
    }
}

// Forward+ AO: reconstruct half-res world normals from the prepass depth into
// tex_hbao_normal so the existing HBAO/blur passes can run from the depth prepass.
void DispatchReconstructNormalCompute(SDL_GPUCommandBuffer* cmd, mat4x4 viewProj, u32 width, u32 height)
{
    WindowState* winstate = &g_WindowState;
    if (!winstate->tex_hiz_depth || !winstate->tex_hbao_normal) return;
    CHECK_CREATE(g_ReconstructNormalComputePipeline, "Reconstruct Normal Compute Pipeline");

    u32 aoWidth = Maxu32(width / 2u, 1u);
    u32 aoHeight = Maxu32(height / 2u, 1u);

    struct {
        mat4x4 invViewProj;
        f32 cameraPosition[4];
        u32 fullSize[2];
        u32 aoSize[2];
    } params = {0};
    params.invViewProj = M44Inverse(viewProj);
    params.cameraPosition[0] = g_Camera.position.x;
    params.cameraPosition[1] = g_Camera.position.y;
    params.cameraPosition[2] = g_Camera.position.z;
    params.fullSize[0] = width;
    params.fullSize[1] = height;
    params.aoSize[0] = aoWidth;
    params.aoSize[1] = aoHeight;

    SDL_GPUStorageTextureReadWriteBinding output = {
        .texture = winstate->tex_hbao_normal, .mip_level = 0, .layer = 0, .cycle = true
    };
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &output, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_ReconstructNormalComputePipeline);
    SDL_BindGPUComputeSamplers(pass, 0, &(SDL_GPUTextureSamplerBinding){
        .texture = winstate->tex_hiz_depth, .sampler = g_RenderState.hiZSampler }, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, (aoWidth + 7u) / 8u, (aoHeight + 7u) / 8u, 1);
    SDL_EndGPUComputePass(pass);
}

// Forward+ tiled light culling. First a tiny reset dispatch zeroes the global index
// allocator and per-light visibility, then one group per 16x16 screen tile bins the
// visible local lights into a flat per-tile index list.
void DispatchBuildLightGridCompute(SDL_GPUCommandBuffer* cmd, u32 width, u32 height, u32 tilesX, u32 tilesY)
{
    WindowState* winstate = &g_WindowState;
    if (!winstate->tex_hiz_depth || !g_RenderState.lightBuffer || !g_RenderState.lightGridBuffer ||
        !g_RenderState.lightIndexBuffer || !g_RenderState.lightIndexCounter || !g_RenderState.lightVisibilityBuffer)
        return;
    CHECK_CREATE(g_BuildLightGridComputePipeline, "Build Light Grid Compute Pipeline");

    struct {
        mat4x4 invProjection;
        mat4x4 view;
        u32 screenSize[2];
        u32 tileSize;
        u32 tilesX;
        u32 tilesY;
        u32 numLights;
        u32 resetMode;
        u32 padding0;
    } params = {0};
    params.invProjection = g_Camera.inverseProjection;
    params.view = g_Camera.view;
    params.screenSize[0] = width;
    params.screenSize[1] = height;
    params.tileSize = FORWARD_TILE_SIZE;
    params.tilesX = tilesX;
    params.tilesY = tilesY;
    params.numLights = g_RenderState.numLights;

    SDL_GPUStorageBufferReadWriteBinding rw[4] = {
        { g_RenderState.lightGridBuffer },
        { g_RenderState.lightIndexBuffer },
        { g_RenderState.lightIndexCounter },
        { g_RenderState.lightVisibilityBuffer }
    };
    SDL_GPUTextureSamplerBinding depthBinding = { .texture = winstate->tex_hiz_depth, .sampler = g_RenderState.hiZSampler };
    SDL_GPUBuffer* roBuffers[1] = { g_RenderState.lightBuffer };

    // reset pass
    params.resetMode = 1u;
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, NULL, 0, rw, SDL_arraysize(rw));
    SDL_BindGPUComputePipeline(pass, g_BuildLightGridComputePipeline);
    SDL_BindGPUComputeSamplers(pass, 0, &depthBinding, 1);
    SDL_BindGPUComputeStorageBuffers(pass, 0, roBuffers, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, Maxu32((g_RenderState.numLights + 255u) / 256u, 1u), 1, 1);
    SDL_EndGPUComputePass(pass);

    // main per-tile pass
    params.resetMode = 0u;
    pass = SDL_BeginGPUComputePass(cmd, NULL, 0, rw, SDL_arraysize(rw));
    SDL_BindGPUComputePipeline(pass, g_BuildLightGridComputePipeline);
    SDL_BindGPUComputeSamplers(pass, 0, &depthBinding, 1);
    SDL_BindGPUComputeStorageBuffers(pass, 0, roBuffers, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, tilesX, tilesY, 1);
    SDL_EndGPUComputePass(pass);
}

void DispatchHBAOCompute(SDL_GPUCommandBuffer* cmd, bool enabled, u32 width, u32 height)
{
    WindowState* winstate = &g_WindowState;
    if (!winstate->tex_hiz_depth || !winstate->tex_hbao || !winstate->tex_hbao_blur || !winstate->tex_hbao_normal) return;
    CHECK_CREATE(g_HBAOComputePipeline, "HBAO Compute Pipeline");
    CHECK_CREATE(g_HBAOBlurComputePipeline, "HBAO Blur Compute Pipeline");

    u32 aoWidth = Maxu32(width / 2u, 1u);
    u32 aoHeight = Maxu32(height / 2u, 1u);

    struct {
        mat4x4 invProjection;
        mat4x4 view;
        u32 fullSize[2];
        u32 aoSize[2];
        f32 radius;
        f32 projectionScale;
        f32 bias;
        f32 intensity;
        f32 power;
        u32 enabled;
        u32 frameIndex;
        u32 numDirections;
    } params;

    params.invProjection = g_Camera.inverseProjection;
    params.view = g_Camera.view;
    params.fullSize[0] = width;
    params.fullSize[1] = height;
    params.aoSize[0] = aoWidth;
    params.aoSize[1] = aoHeight;
    params.radius = Maxf32(g_RenderSettings.hbaoRadius, 0.001f);
    params.projectionScale = g_Camera.projection.m[1][1] * (f32)height * 0.5f;
    params.bias = g_RenderSettings.hbaoBias;
    params.intensity = g_RenderSettings.hbaoIntensity;
    params.power = Maxf32(g_RenderSettings.hbaoPower, 0.001f);
    params.enabled = enabled ? 1u : 0u;
    params.frameIndex = (u32)SDL_GetTicks();
    params.numDirections = (u32)Clampf32(g_RenderSettings.hbaoDirections, 2.0f, 16.0f);

    SDL_GPUStorageTextureReadWriteBinding hbaoOutput = {
        .texture = winstate->tex_hbao,
        .mip_level = 0,
        .layer = 0,
        .cycle = true
    };
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &hbaoOutput, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_HBAOComputePipeline);
    SDL_GPUTextureSamplerBinding hbaoInputs[2] = {
        { .texture = winstate->tex_hiz_depth, .sampler = g_RenderState.hiZSampler },
        { .texture = winstate->tex_hbao_normal, .sampler = g_RenderState.hiZSampler }
    };
    SDL_BindGPUComputeSamplers(pass, 0, hbaoInputs, SDL_arraysize(hbaoInputs));
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, (aoWidth + 7u) / 8u, (aoHeight + 7u) / 8u, 1);
    SDL_EndGPUComputePass(pass);

    SDL_GPUStorageTextureReadWriteBinding blurOutput = {
        .texture = winstate->tex_hbao_blur,
        .mip_level = 0,
        .layer = 0,
        .cycle = true
    };
    pass = SDL_BeginGPUComputePass(cmd, &blurOutput, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_HBAOBlurComputePipeline);
    SDL_GPUTextureSamplerBinding blurInputs[2] = {
        { .texture = winstate->tex_hbao, .sampler = g_RenderState.hiZSampler },
        { .texture = winstate->tex_hiz_depth, .sampler = g_RenderState.hiZSampler }
    };
    SDL_BindGPUComputeSamplers(pass, 0, blurInputs, SDL_arraysize(blurInputs));
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, (aoWidth + 7u) / 8u, (aoHeight + 7u) / 8u, 1);
    SDL_EndGPUComputePass(pass);
}

void DispatchContactShadowsCompute(SDL_GPUCommandBuffer* cmd, bool enabled, mat4x4 viewProj, u32 width, u32 height)
{
    WindowState* winstate = &g_WindowState;
    if (!winstate->tex_hiz_depth || !winstate->tex_contact_shadow) return;
    CHECK_CREATE(g_ContactShadowsComputePipeline, "Contact Shadows Compute Pipeline");

    u32 shadowWidth = Maxu32(width / 2u, 1u);
    u32 shadowHeight = Maxu32(height / 2u, 1u);
    float3 sunDirection = GetRenderSunDirection();

    struct {
        f32 lightCoordinate[4];
        s32 waveOffset[2];
        u32 fullSize[2];
        u32 shadowSize[2];
        f32 surfaceThickness;
        f32 bilinearThreshold;
        f32 shadowContrast;
        f32 intensity;
        u32 enabled;
        u32 padding;
    } params = {0};

    params.fullSize[0] = width;
    params.fullSize[1] = height;
    params.shadowSize[0] = shadowWidth;
    params.shadowSize[1] = shadowHeight;
    params.surfaceThickness = Maxf32(g_RenderSettings.SSSThickness, 0.0001f);
    params.bilinearThreshold = Maxf32(g_RenderSettings.SSSBilinearThreshold, 0.0001f);
    params.shadowContrast = Maxf32(g_RenderSettings.SSSContrast, 1.0f);
    params.intensity = Saturatef32(g_RenderSettings.SSSIntensity);

    SDL_GPUStorageTextureReadWriteBinding output = {
        .texture = winstate->tex_contact_shadow,
        .mip_level = 0,
        .layer = 0,
        .cycle = true
    };
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &output, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_ContactShadowsComputePipeline);
    SDL_BindGPUComputeSamplers(pass, 0, &(SDL_GPUTextureSamplerBinding){
        .texture = winstate->tex_hiz_depth, .sampler = g_RenderState.hiZSampler }, 1);

    if (!enabled)
    {
        params.enabled = 0u;
        SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
        SDL_DispatchGPUCompute(pass, (shadowWidth + 63u) / 64u, shadowHeight, 1u);
        SDL_EndGPUComputePass(pass);
        return;
    }

    v128f lightClip = Vec4Transform(VecSetR(sunDirection.x, sunDirection.y, sunDirection.z, 0.0f), viewProj.r);
	f32 lightProjection[4]; 
	VecStore(lightProjection, lightClip);
    
	s32 viewportSize[2] = { (s32)shadowWidth, (s32)shadowHeight };
    s32 minBounds[2] = { 0, 0 };
    s32 maxBounds[2] = { (s32)shadowWidth, (s32)shadowHeight };
    BendDispatchList list = SSSBuildDispatchList(lightProjection, viewportSize, minBounds, maxBounds, false, 64);
    MemCopy(params.lightCoordinate, list.LightCoordinate_Shader, sizeof(params.lightCoordinate));
    params.enabled = 1u;

    if (list.DispatchCount <= 0)
    {
        params.enabled = 0u;
        SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
        SDL_DispatchGPUCompute(pass, (shadowWidth + 63u) / 64u, shadowHeight, 1u);
        SDL_EndGPUComputePass(pass);
        return;
    }

    for (s32 i = 0; i < list.DispatchCount; i++)
    {
        params.waveOffset[0] = list.Dispatch[i].WaveOffset_Shader[0];
        params.waveOffset[1] = list.Dispatch[i].WaveOffset_Shader[1];
        SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
        SDL_DispatchGPUCompute(pass,
                               (u32)Maxs32(list.Dispatch[i].WaveCount[0], 1),
                               (u32)Maxs32(list.Dispatch[i].WaveCount[1], 1),
                               (u32)Maxs32(list.Dispatch[i].WaveCount[2], 1));
    }
    SDL_EndGPUComputePass(pass);
}

static float2 GetGodRaySunPos(mat4x4 viewProj, float* intensity)
{
    float3 dir = GetRenderSunDirection();
    float3 sunWorld = F3Add(g_Camera.position, F3MulF(dir, 100.0f));
    v128f clip = Vec3Transform(Vec3Load(&sunWorld.x), viewProj.r);
    float w = VecGetW(clip);
    float facing = F3Dot(g_Camera.Front, dir);
    if (facing <= -0.2f)
    {
        *intensity = 0.0f;
        return (float2){ -10.0f, -10.0f };
    }

    if (w <= 0.0001f) w = 0.0001f;
    v128f ndcV = VecDiv(clip, VecSet1(w));
    float ndcX = VecGetX(ndcV);
    float ndcY = VecGetY(ndcV);
    *intensity *= Saturatef32((facing + 0.2f) / 0.7f);
    return (float2){ Clampf32(ndcX * 0.5f + 0.5f, -0.5f, 1.5f), Clampf32(0.5f - ndcY * 0.5f, -0.5f, 1.5f) };
}

static f32 GetSkyPhaseTime(void)
{
    static double startupPhase = 0.0;
    if (startupPhase == 0.0)
    {
        SDL_Time wallTime = 0;
        if (SDL_GetCurrentTime(&wallTime))
        {
            const SDL_Time dayNS = (SDL_Time)86400 * 1000000000;
            startupPhase = (double)(wallTime % dayNS) / 1000000000.0;
        }
    }
    return (f32)(startupPhase * 0.01);
}

static u32 BloomMipSize(u32 size, u32 mip)
{
    return Maxu32(size >> mip, 1u);
}

static SDL_GPUTexture* BloomDownsampleTexture(WindowState* winstate, u32 mip)
{
    return (mip < winstate->bloom_mip_count && mip < BLOOM_MAX_MIPS) ? winstate->tex_bloom_downsample[mip] : NULL;
}

static SDL_GPUTexture* BloomUpsampleTexture(WindowState* winstate, u32 mip)
{
    return (mip < winstate->bloom_mip_count && mip < BLOOM_MAX_MIPS) ? winstate->tex_bloom_upsample[mip] : NULL;
}

void DispatchBloomCompute(SDL_GPUCommandBuffer* cmd, u32 width, u32 height)
{
    WindowState* winstate = &g_WindowState;
    if (!g_RenderSettings.enableBloom) return;
    if (!winstate->tex_color || !BloomDownsampleTexture(winstate, 0u) || winstate->bloom_mip_count == 0u)
    {
        AX_WARN("Bloom resources are not ready");
        return;
    }
    CHECK_CREATE(g_BloomPrefilterDownsampleComputePipeline, "Bloom Prefilter Downsample Compute Pipeline");
    CHECK_CREATE(g_BloomUpsampleComputePipeline, "Bloom Upsample Compute Pipeline");

    struct {
        u32 outputSize[2];
        f32 sourceTexelSize[2];
        u32 sourceMip;
        u32 prefilter;
        f32 threshold;
        f32 knee;
        f32 clampValue;
        f32 padding0;
    } downParams = {0};

    downParams.outputSize[0] = winstate->bloom_width;
    downParams.outputSize[1] = winstate->bloom_height;
    downParams.sourceTexelSize[0] = 1.0f / Maxf32((f32)width, 1.0f);
    downParams.sourceTexelSize[1] = 1.0f / Maxf32((f32)height, 1.0f);
    downParams.sourceMip = 0u;
    downParams.prefilter = 1u;
    downParams.threshold = Maxf32(g_RenderSettings.bloomThreshold, 0.0f);
    downParams.knee = Maxf32(g_RenderSettings.bloomKnee, 0.0001f);
    downParams.clampValue = Maxf32(g_RenderSettings.bloomClamp, 1.0f);

    SDL_GPUStorageTextureReadWriteBinding output = {
        .texture = BloomDownsampleTexture(winstate, 0u),
        .mip_level = 0,
        .layer = 0,
        .cycle = true
    };
    SDL_GPUTextureSamplerBinding sourceBinding = { .texture = winstate->tex_color, .sampler = g_RenderState.sampler };
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &output, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_BloomPrefilterDownsampleComputePipeline);
    SDL_BindGPUComputeSamplers(pass, 0, &sourceBinding, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, &downParams, sizeof(downParams));
    SDL_DispatchGPUCompute(pass, (downParams.outputSize[0] + 7u) / 8u, (downParams.outputSize[1] + 7u) / 8u, 1);
    SDL_EndGPUComputePass(pass);

    for (u32 mip = 1u; mip < winstate->bloom_mip_count; mip++)
    {
        u32 sourceWidth  = BloomMipSize(winstate->bloom_width, mip - 1u);
        u32 sourceHeight = BloomMipSize(winstate->bloom_height, mip - 1u);
        downParams.outputSize[0] = BloomMipSize(winstate->bloom_width, mip);
        downParams.outputSize[1] = BloomMipSize(winstate->bloom_height, mip);
        downParams.sourceTexelSize[0] = 1.0f / (f32)sourceWidth;
        downParams.sourceTexelSize[1] = 1.0f / (f32)sourceHeight;
        downParams.sourceMip = 0u;
        downParams.prefilter = 0u;

        output.texture = BloomDownsampleTexture(winstate, mip);
        if (!output.texture)
        {
            AX_WARN("Bloom downsample mip texture is not ready");
            return;
        }
        output.mip_level = 0;
        output.cycle = false;
        sourceBinding.texture = BloomDownsampleTexture(winstate, mip - 1u);
        if (!sourceBinding.texture)
        {
            AX_WARN("Bloom downsample source texture is not ready");
            return;
        }
        pass = SDL_BeginGPUComputePass(cmd, &output, 1, NULL, 0);
        SDL_BindGPUComputePipeline(pass, g_BloomPrefilterDownsampleComputePipeline);
        SDL_BindGPUComputeSamplers(pass, 0, &sourceBinding, 1);
        SDL_PushGPUComputeUniformData(cmd, 0, &downParams, sizeof(downParams));
        SDL_DispatchGPUCompute(pass, (downParams.outputSize[0] + 7u) / 8u, (downParams.outputSize[1] + 7u) / 8u, 1);
        SDL_EndGPUComputePass(pass);
    }

    if (winstate->bloom_mip_count <= 1u) return;

    struct {
        u32 outputSize[2];
        f32 lowTexelSize[2];
        u32 lowMip;
        u32 highMip;
        f32 sampleScale;
        f32 padding0;
    } upParams = {0};

    upParams.sampleScale = Clampf32(g_RenderSettings.bloomRadius, 0.25f, 4.0f);
    for (u32 mip = winstate->bloom_mip_count - 1u; mip > 0u; mip--)
    {
        u32 outMip = mip - 1u;
        u32 lowWidth  = BloomMipSize(winstate->bloom_width, mip);
        u32 lowHeight = BloomMipSize(winstate->bloom_height, mip);
        upParams.outputSize[0] = BloomMipSize(winstate->bloom_width, outMip);
        upParams.outputSize[1] = BloomMipSize(winstate->bloom_height, outMip);
        upParams.lowTexelSize[0] = 1.0f / (f32)lowWidth;
        upParams.lowTexelSize[1] = 1.0f / (f32)lowHeight;
        upParams.lowMip = 0u;
        upParams.highMip = 0u;

        output.texture = BloomUpsampleTexture(winstate, outMip);
        if (!output.texture)
        {
            AX_WARN("Bloom upsample mip texture is not ready");
            return;
        }
        output.mip_level = 0;
        output.cycle = false;
        SDL_GPUTextureSamplerBinding inputs[2] = {
            { .texture = (mip == winstate->bloom_mip_count - 1u) ? BloomDownsampleTexture(winstate, mip) : BloomUpsampleTexture(winstate, mip), .sampler = g_RenderState.sampler },
            { .texture = BloomDownsampleTexture(winstate, outMip), .sampler = g_RenderState.sampler }
        };
        if (!inputs[0].texture || !inputs[1].texture)
        {
            AX_WARN("Bloom upsample source texture is not ready");
            return;
        }
        pass = SDL_BeginGPUComputePass(cmd, &output, 1, NULL, 0);
        SDL_BindGPUComputePipeline(pass, g_BloomUpsampleComputePipeline);
        SDL_BindGPUComputeSamplers(pass, 0, inputs, SDL_arraysize(inputs));
        SDL_PushGPUComputeUniformData(cmd, 0, &upParams, sizeof(upParams));
        SDL_DispatchGPUCompute(pass, (upParams.outputSize[0] + 7u) / 8u, (upParams.outputSize[1] + 7u) / 8u, 1);
        SDL_EndGPUComputePass(pass);
    }
}

void DispatchTonemapCompute(SDL_GPUCommandBuffer* cmd, u32 width, u32 height, mat4x4 viewProj, u32 tilesX, bool tileHeatEnabled)
{
    WindowState* winstate       = &g_WindowState;
    SDL_GPUTexture* source      = winstate->tex_color; 
    SDL_GPUTexture* depth       = winstate->tex_hiz_depth; 
    SDL_GPUTexture* bloom       = (winstate->bloom_mip_count > 1u) ? winstate->tex_bloom_pong : winstate->tex_bloom_ping;
    bool bloomReady             = bloom != NULL;
    if (!bloom) bloom = source;
    SDL_GPUTexture* destination = winstate->tex_post;
    CHECK_CREATE(g_TonemapComputePipeline, "Tonemap Compute Pipeline");
    SDL_GPUStorageTextureReadWriteBinding rwTexture = {
        .texture = destination,
        .mip_level = 0,
        .layer = 0,
        .cycle = true
    };
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &rwTexture, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_TonemapComputePipeline);
    SDL_GPUTextureSamplerBinding inputs[4] = {
        { .texture = source, .sampler = g_RenderState.sampler },
        { .texture = depth, .sampler = g_RenderState.hiZSampler },
        { .texture = g_RenderState.skyNoise3D, .sampler = g_RenderState.sampler },
        { .texture = bloom, .sampler = g_RenderState.sampler }
    };
    SDL_BindGPUComputeSamplers(pass, 0, inputs, SDL_arraysize(inputs));
    SDL_GPUBuffer* storageBuffers[1] = { g_RenderState.lightGridBuffer };
    SDL_BindGPUComputeStorageBuffers(pass, 0, storageBuffers, SDL_arraysize(storageBuffers));

    float godRayIntensity = g_RenderSettings.godRayIntensity;
    float2 sunPos = GetGodRaySunPos(viewProj, &godRayIntensity);
    struct {
        u32 outputSize[2];
        f32 exposure;
        f32 gamma;
        f32 sunPos[2];
        f32 godRayIntensity;
        f32 time;
        f32 cloudTime;
        f32 godRaySamples;
        f32 bloomIntensity;
        u32 bloomEnabled;
        u32 tilesX;
        u32 tileHeatEnabled;
        u32 bloomPadding[2];
        mat4x4 invViewProj;
        f32 cameraPosition[4];
        f32 sunDirection[4];
        f32 fogColorDensity[4];
        f32 fogParams[4];
    } params = {0};
    float3 sunDir = GetRenderSunDirection();
    params.outputSize[0] = width;
    params.outputSize[1] = height;
    params.exposure = g_RenderSettings.exposure;
    params.gamma = g_RenderSettings.gamma;
    params.sunPos[0] = sunPos.x;
    params.sunPos[1] = sunPos.y;
    params.godRayIntensity = godRayIntensity;
    params.godRaySamples = Clampf32(g_RenderSettings.godRaySamples, 0.0f, 128.0f);
    params.bloomIntensity = g_RenderSettings.bloomIntensity;
    params.bloomEnabled = (g_RenderSettings.enableBloom && bloomReady) ? 1u : 0u;
    params.tilesX = tilesX;
    params.tileHeatEnabled = tileHeatEnabled ? 1u : 0u;
    params.cloudTime = TimeSinceStartup();
    params.time = GetSkyPhaseTime() + params.cloudTime;
    params.invViewProj = M44Inverse(viewProj);
    params.cameraPosition[0] = g_Camera.position.x;
    params.cameraPosition[1] = g_Camera.position.y;
    params.cameraPosition[2] = g_Camera.position.z;
    params.sunDirection[0] = sunDir.x;
    params.sunDirection[1] = sunDir.y;
    params.sunDirection[2] = sunDir.z;
    params.fogColorDensity[0] = g_RenderSettings.fogColor[0];
    params.fogColorDensity[1] = g_RenderSettings.fogColor[1];
    params.fogColorDensity[2] = g_RenderSettings.fogColor[2];
    params.fogColorDensity[3] = g_RenderSettings.fogDensity;
    params.fogParams[0] = g_RenderSettings.fogHeight;
    params.fogParams[1] = g_RenderSettings.fogFalloff;
    params.fogParams[2] = g_RenderSettings.fogSunScatter;
    params.fogParams[3] = g_RenderSettings.enableHeightFog ? 1.0f : 0.0f;
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, (width + 7u) / 8u, (height + 7u) / 8u, 1);
    SDL_EndGPUComputePass(pass);
}

void DispatchMLAACompute(SDL_GPUCommandBuffer* cmd, u32 width, u32 height, f32 threshold, bool showEdges)
{
    WindowState* winstate = &g_WindowState;
    SDL_GPUTexture* source = winstate->tex_post;
    SDL_GPUTexture* destination = winstate->tex_mlaa_output;
    if (!source || !destination || !winstate->tex_mlaa_edge_mask || !winstate->tex_mlaa_edge_count)
    {
        AX_WARN("MLAA resources are not ready");
        return;
    }
    CHECK_CREATE(g_MLAAEdgeMaskComputePipeline, "MLAA Edge Mask Compute Pipeline");
    CHECK_CREATE(g_MLAALineLengthComputePipeline, "MLAA Line Length Compute Pipeline");
    CHECK_CREATE(g_MLAABlendComputePipeline, "MLAA Blend Compute Pipeline");

    struct {
        u32 outputSize[2];
        f32 threshold;
        u32 showEdges;
    } params = {
        { width, height },
        threshold,
        showEdges ? 1u : 0u
    };

    SDL_GPUStorageTextureReadWriteBinding edgeMaskOutput = {
        .texture = winstate->tex_mlaa_edge_mask,
        .mip_level = 0,
        .layer = 0,
        .cycle = true
    };
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &edgeMaskOutput, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_MLAAEdgeMaskComputePipeline);
    SDL_GPUTextureSamplerBinding sourceBinding = { .texture = source, .sampler = g_RenderState.hiZSampler };
    SDL_BindGPUComputeSamplers(pass, 0, &sourceBinding, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, (width + 7u) / 8u, (height + 7u) / 8u, 1);
    SDL_EndGPUComputePass(pass);

    SDL_GPUStorageTextureReadWriteBinding edgeCountOutput = {
        .texture = winstate->tex_mlaa_edge_count,
        .mip_level = 0,
        .layer = 0,
        .cycle = true
    };
    pass = SDL_BeginGPUComputePass(cmd, &edgeCountOutput, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_MLAALineLengthComputePipeline);
    SDL_BindGPUComputeStorageTextures(pass, 0, &winstate->tex_mlaa_edge_mask, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, (width + 7u) / 8u, (height + 7u) / 8u, 1);
    SDL_EndGPUComputePass(pass);

    SDL_GPUStorageTextureReadWriteBinding blendOutput = {
        .texture = destination,
        .mip_level = 0,
        .layer = 0,
        .cycle = true
    };
    pass = SDL_BeginGPUComputePass(cmd, &blendOutput, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_MLAABlendComputePipeline);
    SDL_BindGPUComputeSamplers(pass, 0, &sourceBinding, 1);
    SDL_GPUTexture* readonlyTextures[2] = { winstate->tex_mlaa_edge_mask, winstate->tex_mlaa_edge_count };
    SDL_BindGPUComputeStorageTextures(pass, 0, readonlyTextures, SDL_arraysize(readonlyTextures));
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, (width + 7u) / 8u, (height + 7u) / 8u, 1);
    SDL_EndGPUComputePass(pass);
}

void DispatchAnimationCompute(SDL_GPUCommandBuffer* cmd, RenderSet* renderSet, RenderSetBuffers* setBuffers, AnimationSystem* anims)
{
    if (renderSet->numEntities == 0 || !anims->poseBuffer) return;
    CHECK_CREATE(g_AnimComputePipeline, "Animation Compute Pipeline")

    struct {
        float timeSinceStartup;
        int   numInstances;
    } params;
    params.timeSinceStartup = (float)TimeSinceStartup();
    params.numInstances     = (int)renderSet->numEntities;

    SDL_GPUStorageBufferReadWriteBinding rw_bindings[1] = {
        { anims->boneBuffer }
    };

    SDL_GPUBuffer* buffers[7] = {
        anims->poseBuffer,
        anims->hierarchyBuffer,
        anims->dataBuffer,
        anims->jointsBuffer,
        anims->invBindBuffer,
        anims->instanceBuffer,
        setBuffers->visibleSparseIndices
    };

    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, NULL, 0, rw_bindings, SDL_arraysize(rw_bindings));
    SDL_BindGPUComputePipeline(pass, g_AnimComputePipeline);
    SDL_BindGPUComputeStorageBuffers(pass, 0, buffers, SDL_arraysize(buffers));
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUComputeIndirect(pass, setBuffers->dispatchArgs, 0);
    SDL_EndGPUComputePass(pass);
}

static void GetSkinnedAnimationDispatchSize(RenderSet* renderSet, u32* maxGroupEntities, u32* maxLODVertices)
{
    *maxGroupEntities = 0;
    *maxLODVertices = 0;

    for (u32 groupIdx = 0; groupIdx < renderSet->numGroups; groupIdx++)
    {
        PrimitiveGroup* group = renderSet->primitiveGroups + groupIdx;
        *maxGroupEntities = Maxu32(*maxGroupEntities, group->numEntities);
        for (u32 lod = 0; lod < MESH_LOD_COUNT; lod++)
            *maxLODVertices = Maxu32(*maxLODVertices, group->lodNumVertices[lod]);
    }
}

void DispatchAnimateVerticesCompute(SDL_GPUCommandBuffer* cmd, RenderSet* renderSet, RenderSetBuffers* setBuffers, AnimationSystem* anims)
{
    if (renderSet->numGroups == 0 || !anims->boneBuffer) return;
    CHECK_CREATE(g_AnimVerticesPipeline, "Animation vertices Pipeline")

        struct {
        u32 numPrimitiveGroups;
        u32 viewportSize[2];
        u32 shadowLOD;
        f32 lodDistanceModifier;
        f32 padding[3];
        mat4x4 viewProjection;
    } params;
    params.numPrimitiveGroups = renderSet->numGroups * MESH_LOD_COUNT;
    params.viewportSize[0] = (u32)Maxs32(g_Camera.viewportSize.x, 1);
    params.viewportSize[1] = (u32)Maxs32(g_Camera.viewportSize.y, 1);
    params.shadowLOD = Minu32(1u, MESH_LOD_COUNT - 1u);
    params.lodDistanceModifier = Maxf32(g_RenderSettings.lodDistanceModifier, 0.001f);
    params.padding[0] = params.padding[1] = params.padding[2] = 0.0f;
    params.viewProjection = M44Multiply(g_Camera.view, g_Camera.projection);

    u32 maxGroupEntities = 0;
    u32 maxLODVertices = 0;
    GetSkinnedAnimationDispatchSize(renderSet, &maxGroupEntities, &maxLODVertices);
    if (maxGroupEntities == 0 || maxLODVertices == 0) return;

    SDL_GPUStorageBufferReadWriteBinding rw_bindings[1] = {
        { g_RenderState.skinned.animatedVertices }
    };

    SDL_GPUBuffer* ro_buffers[8] = {
        anims->boneBuffer,
        setBuffers->entity,
        setBuffers->primitiveGroup,
        setBuffers->visibleSparseIndices,
        g_RenderState.skinned.vertexBuffer,
        setBuffers->sparseToDense,
        setBuffers->visibleCount,
        setBuffers->primitiveGroupLOD
    };

    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, NULL, 0, rw_bindings, SDL_arraysize(rw_bindings));
    SDL_BindGPUComputePipeline(pass, g_AnimVerticesPipeline);
    SDL_BindGPUComputeStorageBuffers(pass, 0, ro_buffers, SDL_arraysize(ro_buffers));
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUComputeIndirect(pass, setBuffers->dispatchArgs, sizeof(u32) * 3);
    SDL_EndGPUComputePass(pass);
}
