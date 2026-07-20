#include "RenderingInternal.h"
#include "Include/Algorithm.h"
#include "Include/Terrain.h"

typedef struct ShadowCandidate_
{
    u32 lightIndex;
    f32 distanceSq;
} ShadowCandidate;

ShadowData pointShadows;
ShadowData spotShadows;

static void CullFoliageShadowCasters(SDL_GPUCommandBuffer* cmd, FrustumPlanes planes, mat4x4 viewProj,
                                     CullDrawFlags flags, u32 forcedLOD, u32 instanceMultiplier,
                                     const f32 cullSphere[4])
{
    Scene* foliageScene = tFoliage_GetScene();
    if (!foliageScene || foliageScene->surfaceSet.numGroups == 0u)
        return;

    DispatchCullDrawArgsCompute(cmd, &foliageScene->surfaceSet, &foliageScene->surfaceBuffers,
                                planes, viewProj, flags, forcedLOD, instanceMultiplier, cullSphere);
}

static inline v128f VCALL TransformPoint(mat4x4 m, v128f p)
{
    return Vec3Transform(p, m.r);
}

static inline v128f VCALL AddScaled(v128f a, v128f b, float scale)
{
    return VecFmadd(b, VecSet1(scale), a);
}

static float CascadeSplitDistance(float shadowNear, float shadowFar, u32 cascade)
{
    float splitNear = Maxf32(shadowNear, Minf32(g_RenderSettings.shadowSplitNearDistance, shadowFar * 0.5f));
    float p = (float)(cascade + 1u) / (float)SHADOW_CASCADE_COUNT;
    float logSplit = splitNear * Powf(shadowFar / splitNear, p);
    float uniformSplit = Lerpf(splitNear, shadowFar, p);
    return Maxf32(shadowNear, Minf32(shadowFar, Lerpf(uniformSplit, logSplit, Saturatef32(g_RenderSettings.shadowPSSMLambda))));
}

void UploadShadowCascadeBuffer(const ShadowCascadeData* cascades)
{
    struct {
        mat4x4 lightViewProj[SHADOW_CASCADE_COUNT];
        float  splitDistances[4];
    } gpuCascades;

    for (u32 cascade = 0; cascade < SHADOW_CASCADE_COUNT; cascade++)
    {
        gpuCascades.lightViewProj[cascade] = M44Transpose(cascades->lightViewProj[cascade]);
        gpuCascades.splitDistances[cascade] = cascades->splitDistances[cascade];
    }
    gpuCascades.splitDistances[3] = cascades->splitDistances[SHADOW_CASCADE_COUNT - 1u];

    UpdateGPUBuffer(g_RenderState.shadowCascadeBuffer, &gpuCascades, sizeof(gpuCascades), 0);
}

static SDL_GPUColorTargetInfo MakeShadowColorTarget(WindowState* winstate, u32 layer)
{
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.load_op  = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.clear_color.r = 1.0f;
    target.texture = winstate->tex_shadow_color;
    target.mip_level = layer;
    target.cycle = false;
    return target;
}

static SDL_GPUColorTargetInfo MakeLocalShadowColorTarget(SDL_GPUTexture* texture, u32 layer)
{
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.load_op  = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.clear_color.r = 1.0f;
    target.texture = texture;
    target.layer_or_depth_plane = layer;
    target.cycle = false;
    return target;
}

static SDL_GPUDepthStencilTargetInfo MakeShadowDepthTarget(SDL_GPUTexture* texture, u32 layer)
{
    SDL_GPUDepthStencilTargetInfo target = MakeDepthTarget(texture, SDL_GPU_LOADOP_CLEAR, false);
    target.mip_level = layer;
    return target;
}

static SDL_GPUDepthStencilTargetInfo MakeLocalShadowDepthTarget(SDL_GPUTexture* texture)
{
    return MakeDepthTarget(texture, SDL_GPU_LOADOP_CLEAR, false);
}

ShadowCascadeData CascadedShadowmaps(SDL_GPUCommandBuffer* cmd)
{
    static ShadowCascadeData cachedShadowCascades;
    static u32 shadowFrameIndex = 0;
    static bool shadowCacheValid = false;
    // static const u8 cascadeCadance[] = { 0x5D, 0x22, 0x44 }; // 01011101, 00100010, 01000100
    static const u8 cascadeCadance[] = { 0xFF, 0xFF, 0xFF };
    ShadowCascadeData shadowCascades = GetShadowCascades();
    bool updateCascades[SHADOW_CASCADE_COUNT];
    u32 shadowFrame = (shadowFrameIndex++) & 7;
    u8 frameBit = (1u << shadowFrame);

    for (u32 cascade = 0; cascade < SHADOW_CASCADE_COUNT; cascade++)
    {
        bool updateCascade = !shadowCacheValid || ((cascadeCadance[cascade] & frameBit) != 0);
        updateCascades[cascade] = updateCascade;
        if (!updateCascade) continue;

        cachedShadowCascades.lightViewProj[cascade] = shadowCascades.lightViewProj[cascade];
        cachedShadowCascades.splitDistances[cascade] = shadowCascades.splitDistances[cascade];
    }

    UploadShadowCascadeBuffer(&cachedShadowCascades);

    for (u32 cascade = 0; cascade < SHADOW_CASCADE_COUNT; cascade++)
    {
        if (!updateCascades[cascade]) continue;

        mat4x4 shadowViewProj = cachedShadowCascades.lightViewProj[cascade];
        FrustumPlanes shadowFrustum = CreateFrustumPlanes(shadowViewProj);
        // planes.planes[4] = planes.planes[5] = VecZero(); // disable near, far plane frustum check
        CullScene(cmd, shadowFrustum, shadowViewProj, CullDrawFlag_None, 1u);
        CullFoliageShadowCasters(cmd, shadowFrustum, shadowViewProj, CullDrawFlag_None, 1u, 1u, NULL);

        WindowState* winstate = &g_WindowState;
        SDL_GPUColorTargetInfo shadow_color_target = MakeShadowColorTarget(winstate, cascade);
        SDL_GPUDepthStencilTargetInfo shadow_depth_target = MakeShadowDepthTarget(winstate->tex_shadow_depth, cascade);
        RenderDepth(cmd, &(DepthPassContext){
            .colorTarget       = &shadow_color_target,
            .depthTarget       = &shadow_depth_target,
            .skinnedPipeline   = g_RenderState.skinned.shadowPipeline,
            .surfacePipeline   = g_RenderState.surface.shadowPipeline,
            .viewProj          = shadowViewProj,
            .shadowMatrixBuffer = g_RenderState.shadowCascadeBuffer,
            .cascadeIndex      = cascade,
            .flags             = DepthPassFlag_ShadowCascades
        });
    }
    shadowCacheValid = true;
    return cachedShadowCascades;
}

static void PointLightShadowMaps(SDL_GPUCommandBuffer* cmd)
{
    WindowState* winstate = &g_WindowState;
    if (pointShadows.count == 0u || !winstate->tex_point_shadow_depth || !winstate->tex_point_shadow_color)
        return;

    for (u32 shadow = 0; shadow < pointShadows.count; shadow++)
    {
        LightGPU* light = &g_RenderLights[pointShadows.lightIndices[shadow]];
        u32 baseLayer = light->shadowIndex * POINT_SHADOW_FACE_COUNT;
        f32 cullSphere[4] = { light->positionRadius[0], light->positionRadius[1], light->positionRadius[2], light->positionRadius[3] };
        DispatchCullDrawArgsCompute(cmd, &g_ActiveScene->skinnedSet, &g_ActiveScene->skinnedBuffers,
                                    (FrustumPlanes){0}, pointShadows.lightViewProj[baseLayer], CullDrawFlag_CullSphere, 1u,
                                    POINT_SHADOW_FACE_COUNT, cullSphere);
        DispatchCullDrawArgsCompute(cmd, &g_ActiveScene->surfaceSet, &g_ActiveScene->surfaceBuffers,
                                    (FrustumPlanes){0}, pointShadows.lightViewProj[baseLayer], CullDrawFlag_CullSphere, 1u,
                                    POINT_SHADOW_FACE_COUNT, cullSphere);
        CullFoliageShadowCasters(cmd, (FrustumPlanes){0}, pointShadows.lightViewProj[baseLayer],
                                 CullDrawFlag_CullSphere, 1u, POINT_SHADOW_FACE_COUNT, cullSphere);

        SDL_GPUColorTargetInfo shadow_color_target = MakeLocalShadowColorTarget(winstate->tex_point_shadow_color, light->shadowIndex);
        SDL_GPUDepthStencilTargetInfo shadow_depth_target = MakeLocalShadowDepthTarget(winstate->tex_point_shadow_depth);
        RenderDepth(cmd, &(DepthPassContext){
            .colorTarget          = &shadow_color_target,
            .depthTarget          = &shadow_depth_target,
            .skinnedPipeline      = g_RenderState.skinned.pointShadowPipeline,
            .surfacePipeline      = g_RenderState.surface.pointShadowPipeline,
            .viewProj             = pointShadows.lightViewProj[baseLayer],
            .shadowMatrixBuffer   = g_RenderState.pointShadowMatrixBuffer,
            .cascadeIndex         = baseLayer,
            .flags                = DepthPassFlag_PointShadowSides
        });
    }
}

static void SpotLightShadowMaps(SDL_GPUCommandBuffer* cmd)
{
    WindowState* winstate = &g_WindowState;
    if (spotShadows.count == 0u || !winstate->tex_spot_shadow_depth || !winstate->tex_spot_shadow_color)
        return;

    for (u32 shadow = 0; shadow < spotShadows.count; shadow++)
    {
        LightGPU* light = &g_RenderLights[spotShadows.lightIndices[shadow]];
        u32 layer = light->shadowIndex;
        mat4x4 shadowViewProj = spotShadows.lightViewProj[layer];
        CullScene(cmd, CreateFrustumPlanes(shadowViewProj), shadowViewProj, CullDrawFlag_None, 1u);
        CullFoliageShadowCasters(cmd, CreateFrustumPlanes(shadowViewProj), shadowViewProj,
                                 CullDrawFlag_None, 1u, 1u, NULL);

        SDL_GPUColorTargetInfo shadow_color_target = MakeLocalShadowColorTarget(winstate->tex_spot_shadow_color, layer);
        SDL_GPUDepthStencilTargetInfo shadow_depth_target = MakeLocalShadowDepthTarget(winstate->tex_spot_shadow_depth);
            RenderDepth(cmd, &(DepthPassContext){
                .colorTarget          = &shadow_color_target,
                .depthTarget          = &shadow_depth_target,
                .skinnedPipeline      = g_RenderState.skinned.spotShadowPipeline,
                .surfacePipeline      = g_RenderState.surface.spotShadowPipeline,
                .viewProj             = shadowViewProj,
            .shadowMatrixBuffer   = g_RenderState.spotShadowMatrixBuffer,
            .cascadeIndex         = layer,
            .flags                = DepthPassFlag_SpotShadowSides
        });
    }
}

static f32 LightCameraDistanceSq(const LightGPU* light)
{
    v128f l = VecSub(VecLoad(light->positionRadius), VecLoad(&g_Camera.position.x));
    return Vec3DotfV(l, l);
}

static bool LightIntersectsCameraFrustum(const LightGPU* light)
{
    if (!g_RenderSettings.enableLightFrustumCulling)
        return true;

    mat4x4 viewProj = M44Multiply(g_Camera.view, g_Camera.projection);
    FrustumPlanes frustum = CreateFrustumPlanesRevZ(viewProj);
    v128f center = VecLoad(light->positionRadius);
    f32 radius = Maxf32(light->positionRadius[3], 0.001f);

    for (u32 i = 0u; i < 6u; i++)
    {
        v128f plane = frustum.planes[i];
        f32 distance = VecDotf(plane, VecSelect(VecOne(), center, VecSelect1110));
        f32 planeScale = Sqrtf(Vec3DotfV(plane, plane));
        if (distance + planeScale * radius < -0.001f)
            return false;
    }
    return true;
}

static void AssignNearestShadowSlots(LightType type, u32 maxSlots)
{
    ShadowCandidate candidates[POINT_SHADOW_MAX_LIGHTS + 1];
    u32 count = 0u;
    maxSlots = Minu32(maxSlots, POINT_SHADOW_MAX_LIGHTS);
    if (maxSlots == 0u)
        return;

    for (u32 lightIndex = 0; lightIndex < g_RenderState.numLights; lightIndex++)
    {
        LightGPU* light = &g_RenderLights[lightIndex];
        if (light->type != type || (light->flags & LIGHT_FLAG_SHADOWED) == 0u)
            continue;
        if (!LightIntersectsCameraFrustum(light))
            continue;
        // Skip lights the GPU cull found fully occluded last frame: they
        // contribute no light, so their shadow map would never be sampled.
        if (g_LightVisibilityGate && !g_LightVisiblePrev[lightIndex])
            continue;

        f32 distanceSq = LightCameraDistanceSq(light);
        u32 insert = count;
        while (insert > 0u && distanceSq < candidates[insert - 1u].distanceSq)
            insert--;

        if (insert >= maxSlots)
            continue;

        candidates[count] = (ShadowCandidate){ lightIndex, distanceSq };
        XSWAP(ShadowCandidate, candidates[insert], candidates[count]);
        count = Minu32(count + 1u, maxSlots);
    }

    for (u32 i = 0; i < count; i++)
        g_RenderLights[candidates[i].lightIndex].shadowIndex = i;
}

static void AssignVisibleShadowSlots(void)
{
    for (u32 i = 0; i < g_RenderState.numLights; i++)
        g_RenderLights[i].shadowIndex = LIGHT_SHADOW_INDEX_INVALID;

    u32 maxPointShadows = Minu32((u32)(g_RenderSettings.maxVisiblePointShadows + 0.5f), POINT_SHADOW_MAX_LIGHTS);
    u32 maxSpotShadows  = Minu32((u32)(g_RenderSettings.maxVisibleSpotShadows + 0.5f), SPOT_SHADOW_MAX_LIGHTS);
    AssignNearestShadowSlots(LightType_Point, maxPointShadows);
    AssignNearestShadowSlots(LightType_Spot, maxSpotShadows);
}

static void BuildPointShadowData(ShadowData* data)
{
    static const f32 faceDirs[POINT_SHADOW_FACE_COUNT][4] = {
        {  1.0f,  0.0f,  0.0f, 0.0f }, { -1.0f,  0.0f,  0.0f, 0.0f },
        {  0.0f,  1.0f,  0.0f, 0.0f }, {  0.0f, -1.0f,  0.0f, 0.0f },
        {  0.0f,  0.0f,  1.0f, 0.0f }, {  0.0f,  0.0f, -1.0f, 0.0f }
    };
    static const f32 faceUps[POINT_SHADOW_FACE_COUNT][4] = {
        { 0.0f, -1.0f,  0.0f, 0.0f }, { 0.0f, -1.0f,  0.0f, 0.0f },
        { 0.0f,  0.0f,  1.0f, 0.0f }, { 0.0f,  0.0f, -1.0f, 0.0f },
        { 0.0f, -1.0f,  0.0f, 0.0f }, { 0.0f, -1.0f,  0.0f, 0.0f }
    };

    SDL_zero(*data);
    for (u32 lightIndex = 0; lightIndex < g_RenderState.numLights; lightIndex++)
    {
        LightGPU* light = &g_RenderLights[lightIndex];
        if (light->type != LightType_Point || (light->flags & LIGHT_FLAG_SHADOWED) == 0u || light->shadowIndex >= POINT_SHADOW_MAX_LIGHTS)
            continue;

        u32 shadowIndex = light->shadowIndex;
        data->lightIndices[data->count++] = lightIndex;
        float radius = Maxf32(light->positionRadius[3], POINT_SHADOW_NEAR_PLANE + 0.1f);
        mat4x4 proj = PerspectiveFovRH(90.0f * MATH_DegToRad, 1.0f, 1.0f, POINT_SHADOW_NEAR_PLANE, radius);
        v128f eye = VecLoad(light->positionRadius);

        for (u32 face = 0; face < POINT_SHADOW_FACE_COUNT; face++)
        {
            mat4x4 view = M44LookAtRHVec(eye, VecLoad(faceDirs[face]), VecLoad(faceUps[face]));
            u32 layer = shadowIndex * POINT_SHADOW_FACE_COUNT + face;
            data->lightViewProj[layer] = M44Multiply(view, proj);
        }
    }
}

static void UploadPointShadowMatrixBuffer(const ShadowData* data)
{
    if (!g_RenderState.pointShadowMatrixBuffer || data->count == 0u) return;
    u32 layerCount = data->count * POINT_SHADOW_FACE_COUNT;
    mat4x4 gpuMatrices[POINT_SHADOW_LAYER_COUNT];
    for (u32 layer = 0; layer < layerCount; layer++)
        gpuMatrices[layer] = M44Transpose(data->lightViewProj[layer]);
    UpdateGPUBufferCycle(g_RenderState.pointShadowMatrixBuffer, gpuMatrices, sizeof(mat4x4) * layerCount, 0, true);
}

static void BuildSpotShadowData(ShadowData* data)
{
    SDL_zero(*data);
    for (u32 lightIndex = 0; lightIndex < g_RenderState.numLights; lightIndex++)
    {
        LightGPU* light = &g_RenderLights[lightIndex];
        if (light->type != LightType_Spot || (light->flags & LIGHT_FLAG_SHADOWED) == 0u || light->shadowIndex >= SPOT_SHADOW_MAX_LIGHTS)
            continue;

        u32 shadowIndex = light->shadowIndex;
        data->lightIndices[data->count++] = lightIndex;
        float radius = Maxf32(light->positionRadius[3], SPOT_SHADOW_NEAR_PLANE + 0.1f);
        f32 directionCone[4];
        LightGPU_GetDirectionCone(light, directionCone);
        float coneCos = Clampf32(directionCone[3], -0.95f, 0.995f);
        float fov = 2.0f * ACos(coneCos);
        mat4x4 proj = PerspectiveFovRH(fov, 1.0f, 1.0f, SPOT_SHADOW_NEAR_PLANE, radius);
        v128f eye = VecLoad(light->positionRadius);
        v128f dir = VecLoad(directionCone);
        dir  = VecNorm(dir);
        v128f up = VecSetR(0.0f, 1.0f, 0.0f, 0.0f);
        if (Absf32(directionCone[1]) > 0.999f) up = VecSetR(0.0f, 0.0f, 1.0f, 0.0f);
        data->lightViewProj[shadowIndex] = M44Multiply(M44LookAtRHVec(eye, dir, up), proj);
    }
}

static void UploadSpotShadowMatrixBuffer(const ShadowData* data)
{
    if (!g_RenderState.spotShadowMatrixBuffer || data->count == 0u) return;
    mat4x4 gpuMatrices[SPOT_SHADOW_MAX_LIGHTS];
    for (u32 i = 0; i < data->count; i++)
        gpuMatrices[i] = M44Transpose(data->lightViewProj[i]);
    UpdateGPUBufferCycle(g_RenderState.spotShadowMatrixBuffer, gpuMatrices, sizeof(mat4x4) * data->count, 0, true);
}


void InitShadows()
{
    g_RenderState.shadowCascadeBuffer = CreateBuffer(NULL, sizeof(mat4x4) * SHADOW_CASCADE_COUNT + sizeof(float) * 4,
                                                     BReadRasterBit | BReadCompute | BWriteComputeBit, "ShadowCascadeBuffer");
    g_RenderState.pointShadowMatrixBuffer = CreateBuffer(NULL, sizeof(mat4x4) * POINT_SHADOW_LAYER_COUNT,
                                                         BReadRasterBit | BReadCompute | BWriteComputeBit, "PointShadowMatrixBuffer");
    g_RenderState.spotShadowMatrixBuffer = CreateBuffer(NULL, sizeof(mat4x4) * SPOT_SHADOW_MAX_LIGHTS,
                                                        BReadRasterBit | BReadCompute | BWriteComputeBit, "SpotShadowMatrixBuffer");
}

void RenderShadows(SDL_GPUCommandBuffer* cmd)
{
    PointLightShadowMaps(cmd);
    SpotLightShadowMaps(cmd);
}

void UpdateLightShadows()
{
    SDL_zero(pointShadows);
    SDL_zero(spotShadows);
    if (g_RenderSettings.enableLocalLights)
    {
        AssignVisibleShadowSlots();
        BuildPointShadowData(&pointShadows);
        BuildSpotShadowData(&spotShadows);
        UploadPointShadowMatrixBuffer(&pointShadows);
        UploadSpotShadowMatrixBuffer(&spotShadows);
    }
    else
    {
        for (u32 i = 0; i < g_RenderState.numLights; i++)
            g_RenderLights[i].shadowIndex = LIGHT_SHADOW_INDEX_INVALID;
    }
}

ShadowCascadeData GetShadowCascades(void)
{
    ShadowCascadeData result;
    float shadowNear = Maxf32(g_Camera.nearClip, 0.01f);
    float shadowFar  = Minf32(g_Camera.farClip, Maxf32(g_RenderSettings.shadowMaxDistance, g_Camera.nearClip + 1.0f));
    float aspect     = (float)Maxs32(g_Camera.viewportSize.x, 1) / (float)Maxs32(g_Camera.viewportSize.y, 1);
    float tanHalfFov = Tan(g_Camera.verticalFOV * MATH_DegToRad * 0.5f);

    float3 sunLightDir   = GetRenderSunDirection();
    v128f lightDir       = Vec3Load(&sunLightDir.x);
    v128f lightViewDir   = VecMul(lightDir, VecSet1(-1.0f));
    v128f cameraPosition = Vec3Load(&g_Camera.position.x);

    v128f cameraFront    = Vec3Load(&g_Camera.Front.x);
    v128f cameraRight    = Vec3Load(&g_Camera.Right.x);
    v128f cameraUp       = Vec3Load(&g_Camera.Up.x);
    float previousSplit = shadowNear;

    for (u32 cascade = 0; cascade < SHADOW_CASCADE_COUNT; cascade++)
    {
        float split = CascadeSplitDistance(shadowNear, shadowFar, cascade);
        result.splitDistances[cascade] = split;
        float overlap = Maxf32(g_RenderSettings.shadowCascadeOverlap, 0.0f);
        float nearDist = cascade > 0 ? Maxf32(shadowNear, previousSplit - overlap) : previousSplit;
        float farDist  = cascade + 1u < SHADOW_CASCADE_COUNT ? Minf32(shadowFar, split + overlap) : split;
        previousSplit  = split;

        float nearH = tanHalfFov * nearDist;
        float nearW = nearH * aspect;
        float farH  = tanHalfFov * farDist;
        float farW  = farH * aspect;
        v128f nearCenter = AddScaled(cameraPosition, cameraFront, nearDist);
        v128f farCenter  = AddScaled(cameraPosition, cameraFront, farDist);
        v128f corners[8] = {
            AddScaled(AddScaled(nearCenter, cameraUp,  nearH), cameraRight, -nearW),
            AddScaled(AddScaled(nearCenter, cameraUp,  nearH), cameraRight,  nearW),
            AddScaled(AddScaled(nearCenter, cameraUp, -nearH), cameraRight, -nearW),
            AddScaled(AddScaled(nearCenter, cameraUp, -nearH), cameraRight,  nearW),
            AddScaled(AddScaled(farCenter,  cameraUp,  farH),  cameraRight, -farW),
            AddScaled(AddScaled(farCenter,  cameraUp,  farH),  cameraRight,  farW),
            AddScaled(AddScaled(farCenter,  cameraUp, -farH),  cameraRight, -farW),
            AddScaled(AddScaled(farCenter,  cameraUp, -farH),  cameraRight,  farW)
        };

        v128f center = VecAdd(VecAdd(corners[0], corners[1]), VecAdd(corners[2], corners[3]));
        center = VecAdd(center, VecAdd(VecAdd(corners[4], corners[5]), VecAdd(corners[6], corners[7])));
        center = VecMul(center, VecSet1(1.0f / 8.0f));

        v128f radiusSq = VecZero();
        for (u32 i = 0; i < 8u; i++)
        {
            v128f toCorner = VecSub(corners[i], center);
            radiusSq = VecMax(radiusSq, Vec3DotV(toCorner, toCorner));
        }
        float radius = Maxf32(Sqrtf(VecGetX(radiusSq)), 1.0f);
        float casterMargin = Maxf32(g_RenderSettings.shadowCasterDepthMargin, 1.0f);
        float eyeDistance = radius + Maxf32(g_RenderSettings.shadowCameraDistance, 1.0f) + casterMargin;
        v128f eye = AddScaled(center, lightDir, eyeDistance);
        
        // prevent LookAt NaN failures if the sun points straight down (e.g. at noon)
        v128f upVec = VecSetR(0.0f, 1.0f, 0.0f, 0.0f);
        if (Absf32(sunLightDir.y) > 0.999f) {
            upVec = VecSetR(0.0f, 0.0f, 1.0f, 0.0f);
        }
        mat4x4 view = M44LookAtRHVec(eye, lightViewDir, upVec);
        v128f minLight = VecSet1( FLT_MAX);
        v128f maxLight = VecSet1(-FLT_MAX);
        for (u32 i = 0; i < 8u; i++)
        {
            v128f cornerLight = TransformPoint(view, corners[i]);
            minLight = VecMin(minLight, cornerLight);
            maxLight = VecMax(maxLight, cornerLight);
        }

        v128f extentXY = VecSub(maxLight, minLight);
        extentXY = VecMax(extentXY, VecSwapPairs(extentXY));
        float extent = Ceilf(VecGetX(VecAddf(extentXY, 2.0f)));
        float halfExtent = extent * 0.5f;
        float texelSize = extent / (float)Maxu32(SHADOW_MAP_SIZE >> cascade, 1u);
        v128f centerLight = VecMul(VecAdd(minLight, maxLight), VecSet1(0.5f));
        centerLight = VecMul(VecFloor(VecAddf(VecDiv(centerLight, VecSet1(texelSize)), 0.5f)), VecSet1(texelSize));

        mat4x4 proj = M44OrthoRH(VecGetX(centerLight) - halfExtent, VecGetX(centerLight) + halfExtent,
                                 VecGetY(centerLight) - halfExtent, VecGetY(centerLight) + halfExtent,
                                 SHADOW_NEAR_PLANE, eyeDistance + radius + casterMargin);
        result.lightViewProj[cascade] = M44Multiply(view, proj);
    }

    return result;
}


void DestroyShadows()
{
    if (g_RenderState.shadowCascadeBuffer)     SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.shadowCascadeBuffer);
    if (g_RenderState.pointShadowMatrixBuffer) SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.pointShadowMatrixBuffer);
    if (g_RenderState.spotShadowMatrixBuffer)  SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.spotShadowMatrixBuffer);
    g_RenderState.shadowCascadeBuffer = NULL;
    g_RenderState.pointShadowMatrixBuffer = NULL;
    g_RenderState.spotShadowMatrixBuffer = NULL;
}
