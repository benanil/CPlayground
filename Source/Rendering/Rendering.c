#include "RenderingInternal.h"
#include "Include/Slug.h"
#include "Include/UIRenderer.h"

// Tiled Forward+ opaque path. Set to 0 to fall back to the deferred renderer (the two
// paths both leave the lit HDR image in tex_color, so everything downstream is shared).
#include "Include/Platform.h"
#include "Include/Animation.h"
#include "Include/Memory.h"
#include "Include/Scene.h"
#include "Include/Terrain.h"
#include "Source/Terrain/TerrainInternal.h"
#include "Source/Terrain/Transvoxel.h"

#define RESIZE_RELEASE_DELAY 4u

typedef struct FrameTextureSet_
{
    SDL_GPUTexture* tex_depth;
    SDL_GPUTexture* tex_hiz_depth;
    SDL_GPUTexture* tex_color;
    SDL_GPUTexture* tex_color_msaa;
    SDL_GPUTexture* tex_depth_msaa;
    SDL_GPUTexture* tex_post;
    SDL_GPUTexture* tex_hiz;
    SDL_GPUTexture* tex_hbao;
    SDL_GPUTexture* tex_hbao_blur;
    SDL_GPUTexture* tex_hbao_normal;
    SDL_GPUTexture* tex_contact_shadow;
    SDL_GPUTexture* tex_bloom_downsample[BLOOM_MAX_MIPS];
    SDL_GPUTexture* tex_bloom_upsample[BLOOM_MAX_MIPS];
    SDL_GPUTexture* tex_mlaa_edge_mask;
    SDL_GPUTexture* tex_mlaa_edge_count;
    SDL_GPUTexture* tex_mlaa_output;
    u64 releaseFrame;
} FrameTextureSet;

WindowState    g_WindowState;
RenderState    g_RenderState;
SDL_GPUDevice* g_GPUDevice = NULL;
LightGPU       g_RenderLights[MAX_LIGHT_COUNT];

// Light visibility readback (see RenderingInternal.h). Default to all-visible so
// shadows behave normally until the first cull result has been read back.
u32                    g_LightVisiblePrev[MAX_LIGHT_COUNT];
bool                   g_LightVisibilityGate = false;
static SDL_GPUTransferBuffer* g_LightVisTransfer = NULL;
static SDL_GPUFence*          g_LightVisFence    = NULL;
static bool                   g_LightVisPending  = false;
static bool                   g_LightVisHasData  = false;

RenderSettings g_RenderSettings = {
    .enableOcclusion             = true,
    .enableHBAO                  = true,
    .enableContactShadows        = true,
    .enableMLAA                  = true,
    .enableBloom                 = true,
    .msaaSamples                 = 4u,
    .showMLAAEdges               = false,
    .enableLocalLights           = true,
    .enableLightFrustumCulling   = true,
    .enableLightOcclusionCulling = true,
    .showLightRects              = false,
    .terrainWireframe            = false,
    .terrainLodFactor            = 1.0f,
    .hbaoRadius                  = 1.3f,
    .hbaoBias                    = 0.5f,
    .hbaoIntensity               = 2.0f,
    .hbaoPower                   = 2.0f,
    .SSSBilinearThreshold        = 0.025f,
    .SSSThickness                = 0.005f,
    .SSSIntensity                = 0.75f,
    .SSSContrast                 = 4.0f,
    .mlaaThreshold               = 0.08f,
    .bloomThreshold              = 0.9f,
    .bloomKnee                   = 0.5f,
    .bloomClamp                  = 64.0f,
    .bloomIntensity              = 0.2f,
    .bloomRadius                 = 2.0f,
    .exposure                    = 1.0f,
    .gamma                       = 2.2f,
    .godRayIntensity             = 2.5f,
    .godRaySamples               = 64.0f,
    .enableHeightFog             = true,
    .fogColor                    = { 0.62f, 0.70f, 0.80f },
    .fogDensity                  = 0.1f,
    .fogHeight                   = 0.0f,
    .fogFalloff                  = 0.04f,
    .fogSunScatter               = 0.6f,
    .hbaoDirections              = 8.0f,
    .lodDistanceModifier         = 1.0f,
    .renderScale                 = 1.0f,
    .sunYaw                      = 116.565f,
    .sunPitch                    = 63.435f,
    .shadowMaxDistance           = SHADOW_MAX_DISTANCE,
    .shadowCameraDistance        = SHADOW_CAMERA_DISTANCE,
    .shadowCasterDepthMargin     = SHADOW_CASTER_DEPTH_MARGIN,
    .shadowCascadeOverlap        = SHADOW_CASCADE_OVERLAP,
    .shadowSplitNearDistance     = SHADOW_SPLIT_NEAR_DISTANCE,
    .shadowPSSMLambda            = SHADOW_PSSM_LAMBDA,
    .maxVisiblePointShadows      = 8.0f,
    .maxVisibleSpotShadows       = 8.0f
};

static u64 g_RenderFrameIndex;
static SDL_GPUTexture* g_RenderFinalTexture;

SDL_GPUTexture* RenderGetFinalTexture(void)
{
    return g_RenderFinalTexture;
}

static RenderLightDebugInfo g_LightDebugInfo;
static FrameTextureSet g_ResizeReleaseQueue[RESIZE_RELEASE_DELAY];

extern void UIRenderCallback(void); // Editor.c
extern ShadowCascadeData CascadedShadowmaps(SDL_GPUCommandBuffer* cmd);

OutlineTarget g_OutlineTargets[MAX_OUTLINE_TARGETS];
u32           g_NumOutlineTargets;
ALineVertex   g_GizmoVertices[MAX_GIZMO_VERTICES];
u32           g_NumGizmoVertices;

static void ReleaseFrameTextureSet(FrameTextureSet* set)
{
    if (set->tex_depth)                    SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_depth);
    if (set->tex_hiz_depth)                SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_hiz_depth);
    if (set->tex_color)                    SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_color);
    if (set->tex_color_msaa)               SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_color_msaa);
    if (set->tex_depth_msaa)               SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_depth_msaa);
    if (set->tex_post)                     SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_post);
    if (set->tex_hiz)                      SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_hiz);
    if (set->tex_hbao)                     SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_hbao);
    if (set->tex_hbao_blur)                SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_hbao_blur);
    if (set->tex_hbao_normal)              SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_hbao_normal);
    if (set->tex_contact_shadow)           SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_contact_shadow);
    for (u32 i = 0; i < BLOOM_MAX_MIPS; i++)
    {
        if (set->tex_bloom_downsample[i]) SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_bloom_downsample[i]);
        if (set->tex_bloom_upsample[i])   SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_bloom_upsample[i]);
    }
    if (set->tex_mlaa_edge_mask)           SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_mlaa_edge_mask);
    if (set->tex_mlaa_edge_count)          SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_mlaa_edge_count);
    if (set->tex_mlaa_output)              SDL_ReleaseGPUTexture(g_GPUDevice, set->tex_mlaa_output);
    *set = (FrameTextureSet){0};
}

static void QueueWindowFrameTexturesForRelease(WindowState* winstate)
{
    FrameTextureSet old = {
        .tex_depth                    = winstate->tex_depth,
        .tex_hiz_depth                = winstate->tex_hiz_depth,
        .tex_color                    = winstate->tex_color,
        .tex_color_msaa               = winstate->tex_color_msaa,
        .tex_depth_msaa               = winstate->tex_depth_msaa,
        .tex_post                     = winstate->tex_post,
        .tex_hiz                      = winstate->tex_hiz,
        .tex_hbao                     = winstate->tex_hbao,
        .tex_hbao_blur                = winstate->tex_hbao_blur,
        .tex_hbao_normal              = winstate->tex_hbao_normal,
        .tex_contact_shadow           = winstate->tex_contact_shadow,
        .tex_mlaa_edge_mask           = winstate->tex_mlaa_edge_mask,
        .tex_mlaa_edge_count          = winstate->tex_mlaa_edge_count,
        .tex_mlaa_output              = winstate->tex_mlaa_output,
        .releaseFrame                 = g_RenderFrameIndex + RESIZE_RELEASE_DELAY
    };
    for (u32 i = 0; i < BLOOM_MAX_MIPS; i++)
    {
        old.tex_bloom_downsample[i] = winstate->tex_bloom_downsample[i];
        old.tex_bloom_upsample[i] = winstate->tex_bloom_upsample[i];
        winstate->tex_bloom_downsample[i] = NULL;
        winstate->tex_bloom_upsample[i] = NULL;
    }
    winstate->tex_depth = winstate->tex_hiz_depth = winstate->tex_color = winstate->tex_color_msaa
                        = winstate->tex_depth_msaa = winstate->tex_post = winstate->tex_hiz = winstate->tex_hbao 
                        = winstate->tex_hbao_blur = winstate->tex_hbao_normal = winstate->tex_bloom_ping
                        = winstate->tex_contact_shadow = winstate->tex_bloom_pong = winstate->tex_mlaa_edge_mask 
                        = winstate->tex_mlaa_edge_count = winstate->tex_mlaa_output = NULL;
    
    u32 slot = (u32)(g_RenderFrameIndex % RESIZE_RELEASE_DELAY);
    ReleaseFrameTextureSet(&g_ResizeReleaseQueue[slot]);
    g_ResizeReleaseQueue[slot] = old;
}

void RendererSetGizmoLines(const ALineVertex* vertices, u32 count)
{
    count = Minu32(count, MAX_GIZMO_VERTICES);
    if (count > 0 && vertices)
        MemCopy(g_GizmoVertices, vertices, count * sizeof(ALineVertex));
    g_NumGizmoVertices = vertices ? count : 0;
}

// per-chunk vertex ranges resident in the terrain geometry heap, converted to indirect
// draw commands so the depth and forward passes each render every chunk with a single
// SDL_DrawGPUPrimitivesIndirect (no per-frame vertex copy either, the heap's GPU mirror
// is flushed by UploadDirtyGeometry)
TerrainChunkDraw g_TerrainChunkDraws[MAX_TERRAIN_CHUNK_DRAWS];
u32              g_NumTerrainChunkDraws;
f32              g_TerrainBrushPosRadius[4]; // xyz brush position, w radius (<= 0 inactive)

void RendererSetTerrainChunkDraws(const TerrainChunkDraw* draws, u32 count)
{
    count = draws ? Minu32(count, MAX_TERRAIN_CHUNK_DRAWS) : 0;
    if (count > 0)
        MemCopy(g_TerrainChunkDraws, draws, count * sizeof(TerrainChunkDraw));
    g_NumTerrainChunkDraws = count;
    if (count == 0 || !g_RenderState.terrainDrawArgsBuffer)
        return;

    // first_instance stays 0: SDL only guarantees it with the drawIndirectFirstInstance
    // feature, and the terrain shader does not use instancing anyway
    static SDL_GPUIndexedIndirectDrawCommand commands[MAX_TERRAIN_CHUNK_DRAWS];
    for (u32 i = 0; i < count; i++)
        commands[i] = (SDL_GPUIndexedIndirectDrawCommand){ draws[i].indexCount, 1u, draws[i].firstIndex, draws[i].baseVertex, 0u };
    UpdateGPUBufferCycle(g_RenderState.terrainDrawArgsBuffer, commands,
                         count * sizeof(SDL_GPUIndexedIndirectDrawCommand), 0, true);
}

void RendererSetTerrainBrush(float3 position, f32 radius)
{
    g_TerrainBrushPosRadius[0] = position.x;
    g_TerrainBrushPosRadius[1] = position.y;
    g_TerrainBrushPosRadius[2] = position.z;
    g_TerrainBrushPosRadius[3] = radius;
}

void RendererSetOutlineTargets(const OutlineTarget* targets, u32 count)
{
    count = Minu32(count, MAX_OUTLINE_TARGETS);
    if (count > 0 && targets)
        MemCopy(g_OutlineTargets, targets, count * sizeof(OutlineTarget));
    g_NumOutlineTargets = targets ? count : 0;
}

void RendererClearOutlineTarget(void)
{
    g_NumOutlineTargets = 0;
}

void RendererSetLights(const LightGPU* lights, u32 count)
{
    if (!lights && count > 0u)
    {
        AX_WARN("RendererSetLights called with null lights");
        count = 0u;
    }
    count = Minu32(count, MAX_LIGHT_COUNT);
    if (count > 0u) MemCopy(g_RenderLights, lights, sizeof(LightGPU) * count);
    g_RenderState.numLights = count;
    g_LightDebugInfo.totalLights = count;
    g_LightDebugInfo.maxLights = MAX_LIGHT_COUNT;
}

u32 RendererGetLightCount(void)
{
    return g_RenderState.numLights;
}

RenderLightDebugInfo RendererGetLightDebugInfo(void)
{
    g_LightDebugInfo.totalLights = g_RenderState.numLights;
    if (!g_RenderSettings.enableLocalLights || g_RenderState.numLights == 0u)
        g_LightDebugInfo.submittedLights = 0u;
    g_LightDebugInfo.maxLights = MAX_LIGHT_COUNT;
    return g_LightDebugInfo;
}

void CreateRenderSetBuffers(RenderSetBuffers* buffers, u32 maxEntities, u32 maxGroups)
{
    size_t groupBytes  = maxGroups * sizeof(PrimitiveGroupGPU);
    size_t groupLODBytes = maxGroups * sizeof(PrimitiveGroupLOD);
    size_t entityBytes = maxEntities * sizeof(Entity);
    size_t lodMultiplier = MESH_LOD_COUNT;

    buffers->primitiveGroup    = CreateBuffer(NULL, groupBytes, BReadCompute, "CPPrimitiveGroups");
    buffers->primitiveGroupLOD = CreateBuffer(NULL, groupLODBytes, BReadCompute, "CPPrimitiveGroupLODs");
    buffers->drawSparseIndices = CreateBuffer(NULL, maxEntities * lodMultiplier * sizeof(u32), BReadRasterBit |  BWriteComputeBit, "CPDrawSparseIndices");
    buffers->sparseToDense     = CreateBuffer(NULL, maxEntities * sizeof(u32), BReadCompute, "CPSparseToDense");
    buffers->drawArgs          = CreateBuffer(NULL, maxGroups * lodMultiplier * sizeof(SDL_GPUIndexedIndirectDrawCommand), BIndirectBit | BWriteComputeBit, "CPDrawArgs");
    buffers->entity            = CreateBuffer(NULL, entityBytes, BReadRasterBit | BReadCompute, "CPEntities");
    buffers->visibilityMask    = CreateBuffer(NULL, maxEntities * sizeof(u32), BWriteComputeBit, "CPVisibilityMask");
    buffers->visibleCount      = CreateBuffer(NULL, sizeof(u32), BWriteComputeBit, "CPVisibleCount");
    buffers->dispatchArgs      = CreateBuffer(NULL, sizeof(u32) * 6, BIndirectBit | BWriteComputeBit, "CPDispatchArgs");
    buffers->visibleSparseIndices = CreateBuffer(NULL, maxEntities * sizeof(u32), BWriteComputeBit, "CPVisibleSparseIndices");
}

// re-uploads the render set data that only changes when entities or bundles are added/removed
static void UploadRenderSetStatics(const RenderSet* set, RenderSetBuffers* buffers)
{
	if (set->numGroups == 0) return;
	ArenaMark mark = ArenaSave(&GlobalArena);
    PrimitiveGroupGPU* gpuGroups = (PrimitiveGroupGPU*)ArenaAllocAlign(&GlobalArena, set->numGroups * sizeof(PrimitiveGroupGPU), 16u);
    PrimitiveGroupLOD* lodGroups = (PrimitiveGroupLOD*)ArenaAllocAlign(&GlobalArena, set->numGroups * sizeof(PrimitiveGroupLOD), 16u);
	if (!gpuGroups || !lodGroups) {ArenaRestore(&GlobalArena, mark); return;} 
	MemsetZero(gpuGroups, set->numGroups * sizeof(PrimitiveGroupGPU));
    MemsetZero(lodGroups, set->numGroups * sizeof(PrimitiveGroupLOD));

    for (u32 i = 0; i < set->numGroups; i++)
    {
        const PrimitiveGroup* group = &set->primitiveGroups[i];
        v128u aabbMinEntity   = VecBitcastU32(group->aabbMin);
        v128u aabbMaxMaterial = VecBitcastU32(group->aabbMax);
        VeciSetW(aabbMinEntity  , (u32)group->entityOffset);
        VeciSetW(aabbMaxMaterial, (u32)group->materialIndex | ((u32)group->numEntities << 16u));
        VecStoreI(gpuGroups[i].aabbMinEntity  , aabbMinEntity);
        VecStoreI(gpuGroups[i].aabbMaxMaterial, aabbMaxMaterial);
    }
		
	for (u32 i = 0; i < set->numGroups; i++)
	{
        const PrimitiveGroup* group = &set->primitiveGroups[i];
        VecStoreI(lodGroups[i].lodIndexOffset , VeciLoad(group->lodIndexOffset));
        VecStoreI(lodGroups[i].lodNumIndices  , VeciLoad(group->lodNumIndices));
        VecStoreI(lodGroups[i].lodVertexOffset, VeciLoad(group->lodVertexOffset));
        VecStoreI(lodGroups[i].lodNumVertices , VeciLoad(group->lodNumVertices));
    }

    UpdateGPUBuffer(buffers->primitiveGroup, gpuGroups, set->numGroups * sizeof(PrimitiveGroupGPU), 0);
    UpdateGPUBuffer(buffers->primitiveGroupLOD, lodGroups, set->numGroups * sizeof(PrimitiveGroupLOD), 0);
    UpdateGPUBuffer(buffers->sparseToDense, set->sparseID, set->maxEntities * sizeof(u32), 0);
	ArenaRestore(&GlobalArena, mark);
}

// geometry ranges loaders queued for upload, flushed once the gpu buffers exist.
// element units, bundles can land anywhere in the mega buffers now
#define GEOMETRY_DIRTY_MAX 32u

typedef struct GeometryDirtyRange_ { u32 begin, end; } GeometryDirtyRange;
static GeometryDirtyRange g_GeometryDirty[GeometryBuffer_Count][GEOMETRY_DIRTY_MAX];
static u32 g_NumGeometryDirty[GeometryBuffer_Count];
// Editor mesh import bakes on a worker thread and queues its uploads from there, so the queue is
// shared with the main thread's per-frame drain. The critical sections are tiny (push a range /
// snapshot+clear the queue), so a spinlock is enough.
static SDL_SpinLock g_GeometryUploadLock;

void Rendering_QueueGeometryUpload(GeometryBufferKind kind, u32 begin, u32 end)
{
    if (end <= begin || kind < 0 || kind >= GeometryBuffer_Count) return;
    SDL_LockSpinlock(&g_GeometryUploadLock);
    u32* num = &g_NumGeometryDirty[kind];
    if (*num == GEOMETRY_DIRTY_MAX)
    {
        // full, widen the last range instead of dropping the upload
        GeometryDirtyRange* last = &g_GeometryDirty[kind][GEOMETRY_DIRTY_MAX - 1u];
        last->begin = Minu32(last->begin, begin);
        last->end   = Maxu32(last->end, end);
    }
    else
    {
        g_GeometryDirty[kind][(*num)++] = (GeometryDirtyRange){ begin, end };
    }
    SDL_UnlockSpinlock(&g_GeometryUploadLock);
}

// uploads the queued geometry ranges, called every frame and on init
static void UploadDirtyGeometry(void)
{
    if (!g_RenderState.indexBuffer) return;

    SDL_GPUBuffer* gpuBuffers[GeometryBuffer_Count] = {
        g_RenderState.skinned.vertexBuffer, g_RenderState.surface.vertexBuffer, g_RenderState.indexBuffer,
        g_RenderState.terrainGrassBuffer, g_RenderState.terrainVertexBuffer, g_RenderState.terrainIndexBuffer
    };
    const u8* sources[GeometryBuffer_Count] = {
        (const u8*)gGFX.SkinnedVertexBuffer, (const u8*)gGFX.SurfaceVertexBuffer, (const u8*)gGFX.IndexBuffer,
        (const u8*)gGFX.TerrainGrassBuffer , (const u8*)gGFX.TerrainVertexBuffer, (const u8*)gGFX.TerrainIndexBuffer
    };
    const size_t strides[GeometryBuffer_Count] = {
        sizeof(ASkinedVertex), sizeof(AVertex), sizeof(u32), 
        sizeof(GrassInstance), sizeof(tVertexData), sizeof(u32)
    };

    // Snapshot and clear the queue under the lock, then do the (slower) GPU copies without holding
    // it, so a baking worker thread never spins waiting on a transfer. The source ranges are stable
    // once queued: the worker writes all geometry before it calls Rendering_QueueGeometryUpload.
    GeometryDirtyRange ranges[GeometryBuffer_Count][GEOMETRY_DIRTY_MAX];
    u32 counts[GeometryBuffer_Count];
    SDL_LockSpinlock(&g_GeometryUploadLock);
    for (u32 kind = 0; kind < GeometryBuffer_Count; kind++)
    {
        counts[kind] = g_NumGeometryDirty[kind];
        MemCopy(ranges[kind], g_GeometryDirty[kind], counts[kind] * sizeof(GeometryDirtyRange));
        g_NumGeometryDirty[kind] = 0;
    }
    SDL_UnlockSpinlock(&g_GeometryUploadLock);

    for (u32 kind = 0; kind < GeometryBuffer_Count; kind++)
    {
        if (!gpuBuffers[kind]) continue;
        for (u32 i = 0; i < counts[kind]; i++)
        {
            GeometryDirtyRange range = ranges[kind][i];
            UpdateGPUBuffer(gpuBuffers[kind],
                            sources[kind] + (size_t)range.begin * strides[kind],
                            (size_t)(range.end - range.begin) * strides[kind],
                            (size_t)range.begin * strides[kind]);
        }
    }
}

void InitBuffers(void)
{
    // AnimatedVert is position-only, 16/16/16 unorm in 2x u32 -> 512 MB at full provisioning.
    // (Lever 2 / compaction by visible animated-vertex count reclaims this later.)
    const size_t animatedVertexSize = sizeof(u32) * 2 * MAX_ANIMATED_VERTEX;
    g_RenderState.skinned.vertexBuffer     = CreateBuffer(NULL, MAX_SKINNED_SOURCE_VERTEX * sizeof(ASkinedVertex), BVertexBit | BReadCompute, "CPSkinnedVertexBuffer");
    g_RenderState.surface.vertexBuffer     = CreateBuffer(NULL, MAX_VERTEX * sizeof(AVertex), BVertexBit, "CPSurfaceVertexBuffer");
    g_RenderState.indexBuffer              = CreateBuffer(NULL, MAX_INDEX  * sizeof(int)    , SDL_GPU_BUFFERUSAGE_INDEX, "CPIndexBuffer");
    g_RenderState.lineBuffer               = CreateBuffer(NULL, sizeof(ALineVertex) * MAX_LINE_COUNT   , BVertexBit     | BWriteComputeBit, "CPLineVertexBuffer");
    g_RenderState.lineDrawArgsBuffer       = CreateBuffer(NULL, sizeof(u32) * 8                        , BIndirectBit   | BWriteComputeBit, "CPLinedrawArgsBuffer");
    g_RenderState.gizmoLineBuffer          = CreateBuffer(NULL, sizeof(ALineVertex) * MAX_GIZMO_VERTICES, BVertexBit                      , "CPGizmoLineBuffer");
    g_RenderState.terrainVertexBuffer      = CreateBuffer(NULL, sizeof(tVertexData) * T_MAX_VERTICES    , BVertexBit, "CPTerrainChunkVertexBuffer");
    g_RenderState.terrainIndexBuffer       = CreateBuffer(NULL, sizeof(u32) * T_MAX_INDICES, SDL_GPU_BUFFERUSAGE_INDEX, "CPTerrainChunkIndexBuffer");
    g_RenderState.terrainDrawArgsBuffer    = CreateBuffer(NULL, sizeof(SDL_GPUIndexedIndirectDrawCommand) * MAX_TERRAIN_CHUNK_DRAWS, BIndirectBit, "CPTerrainDrawArgsBuffer");
    g_RenderState.lightBuffer              = CreateBuffer(NULL, sizeof(LightGPU) * MAX_LIGHT_COUNT     , BReadRasterBit | BReadCompute    , "CPLightBuffer");
    g_RenderState.lightVisibilityBuffer    = CreateBuffer(NULL, sizeof(u32) * MAX_LIGHT_COUNT          , BWriteComputeBit, "CPLightVisibilityBuffer");
    // Forward+ tiled light grid. lightGrid holds a {offset,count} per tile; lightIndex is a
    // flat list the forward shaders walk; lightIndexCounter is the global allocator.
    g_RenderState.lightGridBuffer      = CreateBuffer(NULL, sizeof(u32) * 2u * FORWARD_MAX_TILES                  , BReadRasterBit | BReadCompute | BWriteComputeBit, "CPLightGridBuffer");
    g_RenderState.lightIndexBuffer     = CreateBuffer(NULL, sizeof(u32) * FORWARD_MAX_TILES * MAX_LIGHTS_PER_TILE , BReadRasterBit | BWriteComputeBit, "CPLightIndexBuffer");
    g_RenderState.lightIndexCounter    = CreateBuffer(NULL, sizeof(u32)                                           , BWriteComputeBit, "CPLightIndexCounter");

    g_LightVisTransfer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size  = sizeof(u32) * MAX_LIGHT_COUNT
    });
    for (u32 i = 0; i < MAX_LIGHT_COUNT; i++)
        g_LightVisiblePrev[i] = 1u;
    g_RenderState.skinned.animatedVertices = CreateBuffer(NULL, animatedVertexSize, BReadRasterBit | BWriteComputeBit, "CPAnimatedVertices");

    UploadDirtyGeometry();
    g_LightDebugInfo.maxLights = MAX_LIGHT_COUNT;
    UIInit();
}

static void UploadLightBuffer(void)
{
    if (!g_RenderState.lightBuffer || g_RenderState.numLights == 0u)
    {
        g_LightDebugInfo.submittedLights = 0u;
        return;
    }
    UpdateGPUBuffer(g_RenderState.lightBuffer, g_RenderLights, sizeof(LightGPU) * g_RenderState.numLights, 0);
    g_LightDebugInfo.submittedLights = g_RenderState.numLights;
}

static void ReleaseQueuedResizeTextures(bool force)
{
    for (u32 i = 0; i < RESIZE_RELEASE_DELAY; i++)
    {
        FrameTextureSet* set = &g_ResizeReleaseQueue[i];
        if (!set->releaseFrame) continue;
        if (force || g_RenderFrameIndex >= set->releaseFrame) ReleaseFrameTextureSet(set);
    }
}

static void ResizeWindowFrameTextures(WindowState* winstate, u32 width, u32 height)
{
    QueueWindowFrameTexturesForRelease(winstate);
    CreateWindowBuffers();
    Camera_RecalculateProjection(&g_Camera, (s32)width, (s32)height);
}

static SDL_GPUColorTargetInfo MakeMainColorTarget(WindowState* winstate)
{
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.load_op  = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.clear_color.a = 1.0f;
    target.texture  = winstate->tex_color;
    target.cycle    = true;
    if (g_RenderState.sceneSampleCount != SDL_GPU_SAMPLECOUNT_1 && winstate->tex_color_msaa)
    {
        target.texture = winstate->tex_color_msaa;
        target.store_op = SDL_GPU_STOREOP_RESOLVE;
        target.resolve_texture = winstate->tex_color;
        target.cycle_resolve_texture = true;
    }
    return target;
}

static SDL_GPUColorTargetInfo MakeLoadedSceneColorTarget(WindowState* winstate)
{
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.load_op  = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.texture  = winstate->tex_color;
    target.cycle    = false;
    return target;
}

static SDL_GPUColorTargetInfo MakeLoadedTextureTarget(SDL_GPUTexture* texture)
{
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.load_op  = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.texture  = texture;
    target.cycle    = false;
    return target;
}

SDL_GPUDepthStencilTargetInfo MakeDepthTarget(SDL_GPUTexture* texture, SDL_GPULoadOp loadOp, bool cycle)
{
    SDL_GPUDepthStencilTargetInfo target;
    SDL_zero(target);
    target.clear_depth      = 1.0f; // standard-Z default (shadow maps); camera overrides to 0 (reversed-Z)
    target.load_op          = loadOp;
    target.store_op         = SDL_GPU_STOREOP_STORE;
    target.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
    target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    target.texture          = texture;
    target.cycle            = cycle;
    return target;
}

static SDL_GPUDepthStencilTargetInfo MakeForwardDepthTarget(WindowState* winstate)
{
    if (g_RenderState.sceneSampleCount != SDL_GPU_SAMPLECOUNT_1 && winstate->tex_depth_msaa)
    {
        SDL_GPUDepthStencilTargetInfo target = MakeDepthTarget(winstate->tex_depth_msaa, SDL_GPU_LOADOP_CLEAR, true);
        target.store_op = SDL_GPU_STOREOP_DONT_CARE;
        target.clear_depth = 0.0f; // reversed-Z camera depth: far plane = 0
        return target;
    }
    return MakeDepthTarget(winstate->tex_depth, SDL_GPU_LOADOP_LOAD, false);
}

static void ApplyRuntimeGraphicsSettings(WindowState* winstate)
{
    if (!GraphicsApplyMSAASettings()) return;

    SDL_WaitForGPUIdle(g_GPUDevice);
    DestroyRenderPipelines();
    InitRenderPipelines();
    ReleaseQueuedResizeTextures(true);
    QueueWindowFrameTexturesForRelease(winstate);
    CreateWindowBuffers();
}

static SDL_GPUColorTargetInfo MakeHiZDepthTarget(WindowState* winstate)
{
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.load_op  = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.clear_color.r = 0.0f; // reversed-Z: empty/far depth = 0 (sky, HBAO and light-grid read this)
    target.texture = winstate->tex_hiz_depth;
    target.cycle = true;
    return target;
}

static void UploadRenderSetEntities(RenderSet* set, RenderSetBuffers* buffers)
{
    if (set->numEntities == 0) return;
    UpdateGPUBufferCycle(buffers->entity, set->entities, set->numEntities * sizeof(Entity), 0ull, true);
}

void CullScene(SDL_GPUCommandBuffer* cmd, FrustumPlanes planes, mat4x4 viewProj, CullDrawFlags flags, u32 forcedLOD)
{
    Scene* scene = g_ActiveScene;
    DispatchCullDrawArgsCompute(cmd, &scene->skinnedSet, &scene->skinnedBuffers, planes, viewProj, flags, forcedLOD, 1u, NULL);
    DispatchCullDrawArgsCompute(cmd, &scene->surfaceSet, &scene->surfaceBuffers, planes, viewProj, flags, forcedLOD, 1u, NULL);
    DispatchCullDrawArgsCompute(cmd, &scene->transparentSet, &scene->transparentBuffers, planes, viewProj, flags, forcedLOD, 1u, NULL);
}

static void GatherSkinnedAnimationVisibility(SDL_GPUCommandBuffer* cmd, RenderSet* skinnedSet, RenderSetBuffers* skinnedBuffers,
                                             FrustumPlanes cameraFrustum, mat4x4 cameraViewProj, bool enableHiZ,
                                             const ShadowData* pointShadows, const ShadowData* spotShadows)
{

    u32 flags = CullDrawFlag_VisibilityOutput | CullDrawFlag_ResetVisibility | CullDrawFlag_EnableLODSelection;
    if (enableHiZ) flags |= CullDrawFlag_EnableHiZ; // only test occlusion when the Hi-Z (matrix+depth) pair is valid
    DispatchCullDrawArgsCompute(cmd, skinnedSet, skinnedBuffers, cameraFrustum, cameraViewProj, flags, ~0u, 1u, NULL);

    ShadowCascadeData cascades = GetShadowCascades();
    for (u32 cascade = 0; cascade < SHADOW_CASCADE_COUNT; cascade++)
    {
        mat4x4 shadowViewProj = cascades.lightViewProj[cascade];
        FrustumPlanes shadowFrustum = CreateFrustumPlanes(shadowViewProj);
        DispatchCullDrawArgsCompute(cmd, skinnedSet, skinnedBuffers, shadowFrustum, shadowViewProj,
                                      CullDrawFlag_VisibilityOutput, 1u, 1u, NULL);
    }

    for (u32 shadow = 0; shadow < pointShadows->count; shadow++)
    {
        LightGPU* light = &g_RenderLights[pointShadows->lightIndices[shadow]];
        u32 baseLayer = light->shadowIndex * POINT_SHADOW_FACE_COUNT;
        DispatchCullDrawArgsCompute(cmd, skinnedSet, skinnedBuffers, (FrustumPlanes){0}, pointShadows->lightViewProj[baseLayer],
                                      CullDrawFlag_VisibilityOutput | CullDrawFlag_CullSphere, 1u, 1u, light->positionRadius);
    }

    for (u32 shadow = 0; shadow < spotShadows->count; shadow++)
    {
        LightGPU* light = &g_RenderLights[spotShadows->lightIndices[shadow]];
        mat4x4 shadowViewProj = spotShadows->lightViewProj[light->shadowIndex];
        DispatchCullDrawArgsCompute(cmd, skinnedSet, skinnedBuffers, CreateFrustumPlanes(shadowViewProj), shadowViewProj,
                                    CullDrawFlag_VisibilityOutput, 1u, 1u, NULL);
    }
}

static void AnimateSkinned(SDL_GPUCommandBuffer* cmd)
{
    Scene* scene = g_ActiveScene;
    DispatchAnimationCompute(cmd, &scene->skinnedSet, &scene->skinnedBuffers, &scene->animSystem);
    DispatchAnimateVerticesCompute(cmd, &scene->skinnedSet, &scene->skinnedBuffers, &scene->animSystem);
}

void Render(void)
{
    g_RenderFrameIndex++;
    ReleaseQueuedResizeTextures(false);
    WindowState* winstate = &g_WindowState;
    ApplyRuntimeGraphicsSettings(winstate);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    if (!cmd)
    {
        AX_WARN("Failed to acquire command buffer :%s", SDL_GetError());
        Quit(2);
    }

    static int swapchainLogged = 0;
    SDL_GPUTexture* swapchainTexture;
    Uint32 screenW, screenH;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, g_SDLWindow, &swapchainTexture, &screenW, &screenH))
    {
        if (swapchainLogged++ < 4) AX_WARN("Failed to acquire swapchain texture: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }

    if (swapchainTexture == NULL || screenW == 0u || screenH == 0u)
    {
        if (swapchainLogged++ < 4) AX_WARN("Swapchain texture unavailable");
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }

    // the camera projection and picking work in scene view coordinates while one is open
    u32 viewW = screenW, viewH = screenH;
    bool sceneViewActive = GetSceneViewSize(&viewW, &viewH);
    u32 renderW, renderH;
    GetRenderResolution(screenW, screenH, &renderW, &renderH);
    if (winstate->prev_width != screenW || winstate->prev_height != screenH ||
        winstate->render_width != renderW || winstate->render_height != renderH)
    {
        ResizeWindowFrameTextures(winstate, viewW, viewH);
        renderW = winstate->render_width;
        renderH = winstate->render_height;
        swapchainLogged = 0;
    }
    winstate->prev_width = screenW;
    winstate->prev_height = screenH;
    SDL_GPUColorTargetInfo        color_target      = MakeMainColorTarget(winstate);
    SDL_GPUColorTargetInfo        color_load_target = MakeLoadedSceneColorTarget(winstate);
    SDL_GPUDepthStencilTargetInfo depth_target      = MakeDepthTarget(winstate->tex_depth, SDL_GPU_LOADOP_CLEAR, true);
    depth_target.clear_depth = 0.0f; // reversed-Z camera depth: far plane = 0
    SDL_GPUDepthStencilTargetInfo main_depth_target = MakeDepthTarget(winstate->tex_depth, SDL_GPU_LOADOP_LOAD, false);
    SDL_GPUDepthStencilTargetInfo forward_depth_target = MakeForwardDepthTarget(winstate);
    SDL_GPUColorTargetInfo        hiz_depth_target  = MakeHiZDepthTarget(winstate);
    UploadDirtyGeometry();
    
    mat4x4 viewProj = M44Multiply(g_Camera.view, g_Camera.projection);
    bool enableHiZ  = g_RenderSettings.enableOcclusion && winstate->hiz_valid;
    mat4x4 hiZViewProj = enableHiZ ? winstate->hiz_view_proj : viewProj;
    FrustumPlanes cameraFrustum = CreateFrustumPlanesRevZ(viewProj);
    SDL_GPUTexture* finalTexture = winstate->tex_post;
    SDL_GPUColorTargetInfo final_load_target = MakeLoadedTextureTarget(finalTexture);
    bool submitLightVisReadback = false;
    u32 tonemapTilesX = 0u;
    bool tonemapTileHeat = false;
    
    if (g_ActiveScene)
    {
        Scene* scene = g_ActiveScene;
        if (scene->renderDataDirty)
        {
            UploadRenderSetStatics(&scene->skinnedSet, &scene->skinnedBuffers);
            UploadRenderSetStatics(&scene->surfaceSet, &scene->surfaceBuffers);
            UploadRenderSetStatics(&scene->transparentSet, &scene->transparentBuffers);
            scene->renderDataDirty = 0;
        }
        UploadRenderSetEntities(&scene->skinnedSet, &scene->skinnedBuffers);
        UploadRenderSetEntities(&scene->surfaceSet, &scene->surfaceBuffers);
        UploadRenderSetEntities(&scene->transparentSet, &scene->transparentBuffers);

        // Consume the previous frame's light-visibility readback if the GPU has
        // finished it (non-blocking: if not ready, keep last frame's data). This
        // drives occlusion-based shadow culling without a pipeline stall.
        if (g_LightVisFence && SDL_QueryGPUFence(g_GPUDevice, g_LightVisFence))
        {
            void* mapped = SDL_MapGPUTransferBuffer(g_GPUDevice, g_LightVisTransfer, false);
            if (mapped)
            {
                MemCopy(g_LightVisiblePrev, mapped, sizeof(u32) * MAX_LIGHT_COUNT);
                SDL_UnmapGPUTransferBuffer(g_GPUDevice, g_LightVisTransfer);
                g_LightVisHasData = true;
            }
            SDL_ReleaseGPUFence(g_GPUDevice, g_LightVisFence);
            g_LightVisFence = NULL;
            g_LightVisPending = false;
        }
        g_LightVisibilityGate = g_LightVisHasData && g_RenderSettings.enableLocalLights &&
                                g_RenderSettings.enableOcclusion && g_RenderSettings.enableLightOcclusionCulling;

        UpdateLightShadows();
        UploadLightBuffer();
        AnimationSystem_FlushInstances(&scene->animSystem);
        GatherSkinnedAnimationVisibility(cmd, &scene->skinnedSet, &scene->skinnedBuffers,
                                         cameraFrustum, hiZViewProj, enableHiZ, &pointShadows, &spotShadows);
        AnimateSkinned(cmd);

        if (g_RenderSettings.enableLocalLights)
        {
            RenderShadows(cmd);
        }

        ShadowCascadeData shadowCascades = CascadedShadowmaps(cmd);
        UploadShadowCascadeBuffer(&shadowCascades);
        // Only enable the Hi-Z occlusion test when the Hi-Z pair is valid: hiZViewProj is the
        // matrix the depth pyramid was rendered with (last frame). With occlusion off, hiZViewProj
        // falls back to the *current* viewProj, which would not match last frame's depth texture and
        // would produce flicker, so the flag must be gated on enableHiZ rather than hardcoded on.
        CullDrawFlags cullFlags = CullDrawFlag_EnableLODSelection;
        if (enableHiZ) cullFlags |= CullDrawFlag_EnableHiZ;
        CullScene(cmd, cameraFrustum, hiZViewProj, cullFlags, ~0u);

        RenderDepth(cmd, &(DepthPassContext){
            .colorTarget       = &hiz_depth_target,
            .depthTarget       = &depth_target,
            .skinnedPipeline   = g_RenderState.skinned.depthPipeline,
            .surfacePipeline   = g_RenderState.surface.depthPipeline,
            .viewProj          = viewProj,
            .cascadeIndex      = 0,
            .flags             = DepthPassFlag_AlphaClip | DepthPassFlag_EnableLOD
        });
        // Tiled Forward+: AO + light grid run off the prepass depth, then a single
        // forward opaque pass shades everything into tex_color.
        u32 tilesX = (renderW + FORWARD_TILE_SIZE - 1u) / FORWARD_TILE_SIZE;
        u32 tilesY = (renderH + FORWARD_TILE_SIZE - 1u) / FORWARD_TILE_SIZE;
        bool tilesFit = tilesX <= FORWARD_MAX_TILES_X && tilesY <= FORWARD_MAX_TILES_Y;
        bool forwardLocalLights = g_RenderSettings.enableLocalLights && tilesFit;
        tonemapTilesX = tilesX;
        tonemapTileHeat = forwardLocalLights;
        static bool s_tileBudgetWarned = false;
        if (!tilesFit && !s_tileBudgetWarned)
        {
            s_tileBudgetWarned = true;
            AX_WARN("Forward+ render resolution exceeds the tile budget; local lights disabled");
        }

        DispatchReconstructNormalCompute(cmd, viewProj, renderW, renderH);
        DispatchHBAOCompute(cmd, g_RenderSettings.enableHBAO, renderW, renderH);
        DispatchContactShadowsCompute(cmd, g_RenderSettings.enableContactShadows, viewProj, renderW, renderH);
        if (forwardLocalLights)
            DispatchBuildLightGridCompute(cmd, renderW, renderH, tilesX, tilesY);
        RenderSceneForward(cmd, &(ScenePassContext){
            .colorTargets    = &color_target,
            .numColorTargets = 1u,
            .depthTarget     = &forward_depth_target,
            .shadowCascades  = shadowCascades,
            .viewProj        = viewProj
        }, renderW, renderH, tilesX, forwardLocalLights);
        DispatchHiZBuildCompute(cmd);

        // Per-light visibility readback (one frame of latency) for shadow assignment.
        // The forward light-grid pass fills lightVisibilityBuffer. Only issue when the
        // previous readback has been consumed to avoid overwriting an in-flight transfer buffer.
        if (g_RenderSettings.enableLocalLights && !g_LightVisPending &&
            g_RenderState.numLights > 0u && g_RenderState.lightVisibilityBuffer)
        {
            SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
            SDL_DownloadFromGPUBuffer(copyPass,
                &(SDL_GPUBufferRegion){ .buffer = g_RenderState.lightVisibilityBuffer, .offset = 0, .size = sizeof(u32) * MAX_LIGHT_COUNT },
                &(SDL_GPUTransferBufferLocation){ .transfer_buffer = g_LightVisTransfer, .offset = 0 });
            SDL_EndGPUCopyPass(copyPass);
            submitLightVisReadback = true;
        }
        RenderTerrainWireframe(cmd, &color_load_target, &main_depth_target, viewProj);

        winstate->hiz_view_proj = viewProj;
        winstate->hiz_valid = true;

    }

    RenderLines(cmd, &color_load_target, &main_depth_target, viewProj);
    DispatchBloomCompute(cmd, renderW, renderH);
    DispatchTonemapCompute(cmd, renderW, renderH, viewProj, tonemapTilesX, tonemapTileHeat);

    if (g_RenderSettings.enableMLAA && winstate->tex_mlaa_output)
    {
        DispatchMLAACompute(cmd, renderW, renderH, g_RenderSettings.mlaaThreshold, g_RenderSettings.showMLAAEdges);
        finalTexture = winstate->tex_mlaa_output;
        final_load_target = MakeLoadedTextureTarget(finalTexture);
    }

    if (g_ActiveScene)
        RenderOutline(cmd, &final_load_target, &main_depth_target, viewProj);

    RenderGizmo(cmd, &final_load_target, viewProj);
    RenderSlugDemo(cmd, &final_load_target, &main_depth_target, viewProj);
    g_RenderFinalTexture = finalTexture;

    if (sceneViewActive)
    {
        // the scene view window shows the texture, only clear the swapchain for the ui
        SDL_GPUColorTargetInfo clear_target;
        SDL_zero(clear_target);
        clear_target.load_op = SDL_GPU_LOADOP_CLEAR;
        clear_target.store_op = SDL_GPU_STOREOP_STORE;
        clear_target.clear_color = (SDL_FColor){ 0.07f, 0.07f, 0.08f, 1.0f };
        clear_target.texture = swapchainTexture;
        SDL_GPURenderPass* clearPass = SDL_BeginGPURenderPass(cmd, &clear_target, 1, NULL);
        SDL_EndGPURenderPass(clearPass);
    }
    else
    {
        // the scene upscales to the swapchain, the ui draws on top at native resolution
        SDL_GPUBlitInfo blit_info;
        SDL_zero(blit_info);
        blit_info.source.texture = finalTexture;
        blit_info.source.w = renderW;
        blit_info.source.h = renderH;
        blit_info.destination.texture = swapchainTexture;
        blit_info.destination.w = screenW;
        blit_info.destination.h = screenH;
        blit_info.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit_info.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cmd, &blit_info);
    }

    SDL_GPUColorTargetInfo ui_target = MakeLoadedTextureTarget(swapchainTexture);
    UIBeginFrame();
    UIRenderCallback();
    UIEndFrame(cmd, &ui_target);

    if (submitLightVisReadback)
    {
        // Acquire a fence so next frame can tell when the visibility download has
        // landed before mapping the transfer buffer.
        g_LightVisFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        g_LightVisPending = (g_LightVisFence != NULL);
    }
    else
    {
        SDL_SubmitGPUCommandBuffer(cmd);
    }
}

void RendererInit(void)
{
    InitRenderPipelines();
    SlugInitDemo();
}

void DestroyRenderSetBuffers(RenderSetBuffers* buffers)
{
    if (buffers->entity)               SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->entity);
    if (buffers->primitiveGroup)       SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->primitiveGroup);
    if (buffers->primitiveGroupLOD)    SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->primitiveGroupLOD);
    if (buffers->drawSparseIndices)    SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->drawSparseIndices);
    if (buffers->drawArgs)             SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->drawArgs);
    if (buffers->sparseToDense)        SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->sparseToDense);
    if (buffers->visibleSparseIndices) SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->visibleSparseIndices);
    if (buffers->visibilityMask)       SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->visibilityMask);
    if (buffers->visibleCount)         SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->visibleCount);
    if (buffers->dispatchArgs)         SDL_ReleaseGPUBuffer(g_GPUDevice, buffers->dispatchArgs);
}

void DestroyPipeline(void)
{
    ReleaseQueuedResizeTextures(true);
    if (g_RenderState.skinned.vertexBuffer)     SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.skinned.vertexBuffer);
    if (g_RenderState.skinned.animatedVertices) SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.skinned.animatedVertices);
    if (g_RenderState.surface.vertexBuffer)     SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.surface.vertexBuffer);
    if (g_RenderState.indexBuffer)              SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.indexBuffer);
    if (g_RenderState.lineBuffer)               SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.lineBuffer);
    if (g_RenderState.lineDrawArgsBuffer)       SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.lineDrawArgsBuffer);
    if (g_RenderState.terrainVertexBuffer) SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.terrainVertexBuffer);
    if (g_RenderState.terrainIndexBuffer)  SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.terrainIndexBuffer);
    if (g_RenderState.terrainDrawArgsBuffer)    SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.terrainDrawArgsBuffer);
    if (g_RenderState.lightBuffer)              SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.lightBuffer);
    if (g_RenderState.lightVisibilityBuffer)    SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.lightVisibilityBuffer);
    if (g_RenderState.lightGridBuffer)          SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.lightGridBuffer);
    if (g_RenderState.lightIndexBuffer)         SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.lightIndexBuffer);
    if (g_RenderState.lightIndexCounter)        SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.lightIndexCounter);
    if (g_LightVisFence)                        SDL_ReleaseGPUFence(g_GPUDevice, g_LightVisFence);
    if (g_LightVisTransfer)                     SDL_ReleaseGPUTransferBuffer(g_GPUDevice, g_LightVisTransfer);
    if (g_RenderState.uiShapeBuffer)            SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.uiShapeBuffer);
    if (g_RenderState.uiShapeDrawArgsBuffer)    SDL_ReleaseGPUBuffer(g_GPUDevice, g_RenderState.uiShapeDrawArgsBuffer);
    if (g_RenderState.sampler)                  SDL_ReleaseGPUSampler(g_GPUDevice, g_RenderState.sampler);
    if (g_RenderState.hiZSampler)               SDL_ReleaseGPUSampler(g_GPUDevice, g_RenderState.hiZSampler);
    if (g_RenderState.shadowSampler)            SDL_ReleaseGPUSampler(g_GPUDevice, g_RenderState.shadowSampler);
    if (g_ActiveScene) Scene_Destroy(g_ActiveScene);
    TextureSystem_DestroyDevice();
    UIDestroy();
    SlugDestroyDemo();
    DestroyRenderPipelines();
    SDL_zero(g_RenderState);
    for (u32 kind = 0; kind < GeometryBuffer_Count; kind++)
        g_NumGeometryDirty[kind] = 0;
    g_GPUDevice = NULL;
}

void Quit(s32 rc)
{
    Terrain_Destroy();
    DestroyPipeline();
    GraphicsDestroy();
    exit(rc);
}
