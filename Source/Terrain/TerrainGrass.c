#include "Include/Terrain.h"
#include "Source/Terrain/TerrainInternal.h"
#include "Source/Rendering/RenderingInternal.h"
#include "Include/Graphics.h"
#include "Include/Random.h"
#include "Math/Noise.h"

#if defined(PLATFORM_MACOSX)
    #include "Shaders/msl/GrassVert.msl.h"
    #include "Shaders/msl/GrassFrag.msl.h"
    #define Shaders_GrassVert_spv Shaders_GrassVert_msl
    #define Shaders_GrassFrag_spv Shaders_GrassFrag_msl
#elif defined(PLATFORM_WINDOWS)
    #include "Shaders/spv/GrassVert.spv.h"
    #include "Shaders/spv/GrassFrag.spv.h"
#endif

#define GRASS_BEACH_BAND      5.0f
#define GRASS_ISLAND_MAX      0.02f
#define GRASS_PATCH_THRESHOLD 0.55f
#define GRASS_BLADE_EXTENT    0.7f
#define GRASS_BLADE_HEIGHT    2.0f
#define TERRAIN_GRASS_SIZE    512

typedef struct GrassVSParams_
{
    mat4x4 view;
    mat4x4 proj;
    f32 cameraTime[4];
    f32 grassParams[4];
} GrassVSParams;

typedef struct GrassFSParams_
{
    f32 sunDirection[4];
    f32 grassColor[4];
    f32 grassColorVariant[4];
} GrassFSParams;

static TerrainGrassState* tgActive;

static const char* const grassPaths[2] = {
    "Assets/Textures/Grass0.png",
    "Assets/Textures/Grass1.png"
};

static f32 TerrainChunkSize0(void) {
    return (f32)T_CHUNK_CELLS * T_VOXEL_SIZE;
}

u32 Terrain_BuildChunkGrass(int3 chunkMin, GrassInstance* out, float3* outMin, float3* outMax)
{
    TerrainAuthoring* authoring = Terrain_GetAuthoring();
    const TerrainGenParams* params = Terrain_GetGenParams();
    f32 size0 = TerrainChunkSize0();
    f32 originX = (f32)chunkMin.x;
    f32 originY = (f32)chunkMin.y;
    f32 originZ = (f32)chunkMin.z;
    f32 seaLevel = params ? params->seaLevel : 0.0f;
    f32 density = GRASS_PER_METER;
    f32 bladesPerSqM = density > 0.0f ? 2.0f * density : 0.0f;
    if (bladesPerSqM <= 0.0f) return 0u;

    u32 target = (u32)(bladesPerSqM * size0 * size0);
    if (target > GRASS_PER_CHUNK) target = GRASS_PER_CHUNK;

    s32 cx = FloorDiv(chunkMin.x, T_CHUNK_CELLS);
    s32 cz = FloorDiv(chunkMin.z, T_CHUNK_CELLS);
    u32 seed = WangHash((u32)cx * 73856093u ^ (u32)cz * 19349663u);
    float3 aMin = { 1e30f, 1e30f, 1e30f };
    float3 aMax = { -1e30f, -1e30f, -1e30f };
    u32 count = 0u;
    for (u32 b = 0u; b < target; b++)
    {
        f32 wx = originX + NextFloat01(PCG2Next(&seed)) * size0;
        f32 wz = originZ + NextFloat01(PCG2Next(&seed)) * size0;
        if (TerrainDensity_IslandMask(wx, wz) > GRASS_ISLAND_MAX) continue;

        float3 normal;
        f32 wy = TerrainDensity_SurfaceY(wx, wz, TerrainDensity_Height(wx, wz), &normal);
        if (wy < originY || wy >= originY + size0) continue;
        if (wy < seaLevel + GRASS_BEACH_BAND) continue;
        if (normal.y < 0.55f) continue;

        float2 cell = NoiseCellular2D((float2){ wx * 0.15f, wz * 0.15f });
        if (cell.x > GRASS_PATCH_THRESHOLD) continue;

        out[count].positionXY = MakeHalf2(FloatToHalf(wx - originX), FloatToHalf(wy));
        out[count].positionZChunkIndex = (u32)FloatToHalf(wz - originZ);
        count++;

        float3 root = { wx, wy, wz };
        aMin = F3Min(aMin, root);
        aMax = F3Max(aMax, root);
    }

    if (count == 0u)
    {
        *outMin = *outMax = (float3){ originX, originY, originZ };
        return 0u;
    }

    *outMin = (float3){ aMin.x - GRASS_BLADE_EXTENT, aMin.y, aMin.z - GRASS_BLADE_EXTENT };
    *outMax = (float3){ aMax.x + GRASS_BLADE_EXTENT, aMax.y + GRASS_BLADE_HEIGHT, aMax.z + GRASS_BLADE_EXTENT };
    return count;
}

static void TerrainGrassCreatePipeline(TerrainGrassState* tf)
{
    SDL_GPUVertexAttribute grassAttr = { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT2, .offset = 0 };
    SDL_GPUVertexInputState grassInput = {
        .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){ 0, sizeof(GrassInstance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
        .num_vertex_buffers = 1,
        .vertex_attributes = &grassAttr,
        .num_vertex_attributes = 1
    };

    SDL_GPUShader* vert = SDL_CreateGPUShader(g_GPUDevice, &(SDL_GPUShaderCreateInfo){
        .code = Shaders_GrassVert_spv,
        .code_size = sizeof(Shaders_GrassVert_spv),
        .format = AX_GPU_SHADER_FORMAT,
        .stage = SDL_GPU_SHADERSTAGE_VERTEX,
        .entrypoint = "vert",
        .num_uniform_buffers = 1,
        .num_storage_buffers = 1
    });
    SDL_GPUShader* frag = SDL_CreateGPUShader(g_GPUDevice, &(SDL_GPUShaderCreateInfo){
        .code = Shaders_GrassFrag_spv,
        .code_size = sizeof(Shaders_GrassFrag_spv),
        .format = AX_GPU_SHADER_FORMAT,
        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
        .entrypoint = "frag",
        .num_uniform_buffers = 1,
        .num_samplers = 1
    });
    CHECK_CREATE(vert, "Grass Vertex Shader");
    CHECK_CREATE(frag, "Grass Fragment Shader");

    tf->pipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
        .vertex_shader = vert,
        .fragment_shader = frag,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets = 1,
            .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT },
            .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            .has_depth_stencil_target = true
        },
        .rasterizer_state = (SDL_GPURasterizerState){ .cull_mode = SDL_GPU_CULLMODE_NONE },
        .depth_stencil_state = (SDL_GPUDepthStencilState){
            .enable_depth_test = true,
            .enable_depth_write = true,
            .compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
        },
        .multisample_state = (SDL_GPUMultisampleState){ .sample_count = g_RenderState.sceneSampleCount },
        .vertex_input_state = grassInput
    });
    CHECK_CREATE(tf->pipeline, "Terrain Grass Pipeline");
    SDL_ReleaseGPUShader(g_GPUDevice, vert);
    SDL_ReleaseGPUShader(g_GPUDevice, frag);
}

bool TerrainGrass_Init(TerrainGrassState* tg)
{
    if (!tg) return false;
    if (tg->initialized) { tgActive = tg; return true; }
    if (!g_GPUDevice) return false;

    MemSet(tg, 0, sizeof(*tg));
    for (u32 i = 0; i < T_GRASS_MAX_SLOTS; i++)
        tg->freeSlots[tg->freeCount++] = (u16)(T_GRASS_MAX_SLOTS - 1u - i);

    tg->grassBuffer = CreateBuffer(NULL, sizeof(GrassInstance) * T_MAX_GRASS, BVertexBit, "TerrainGrassBuffer");
    tg->chunkBuffer = CreateBuffer(NULL, sizeof(TerrainGrassChunkInfo) * T_GRASS_MAX_SLOTS, BReadRasterBit, "TerrainGrassChunks");
    tg->indirectBuffer = CreateBuffer(NULL, sizeof(SDL_GPUIndirectDrawCommand) * T_GRASS_MAX_DRAWS, BIndirectBit, "TerrainGrassIndirect");
    TerrainGrassCreatePipeline(tg);
    tg->grassLayers = LoadTextureArray(grassPaths, 2u, TERRAIN_GRASS_SIZE, true, "TerrainGrass", "grass");
    tg->initialized = true;
    tgActive = tg;
    return true;
}

void TerrainGrass_Clear(TerrainGrassState* tg)
{
    if (!tg || !tg->initialized) return;
    tg->freeCount = 0u;
    for (u32 i = 0; i < T_GRASS_MAX_SLOTS; i++)
        tg->freeSlots[tg->freeCount++] = (u16)(T_GRASS_MAX_SLOTS - 1u - i);
    tg->drawCount = 0u;
}

void TerrainGrass_Destroy(TerrainGrassState* tg)
{
    if (!tg || !tg->initialized) return;
    if (tg->pipeline)       SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, tg->pipeline);
    if (tg->grassBuffer)    SDL_ReleaseGPUBuffer(g_GPUDevice, tg->grassBuffer);
    if (tg->chunkBuffer)    SDL_ReleaseGPUBuffer(g_GPUDevice, tg->chunkBuffer);
    if (tg->indirectBuffer) SDL_ReleaseGPUBuffer(g_GPUDevice, tg->indirectBuffer);
    ReleaseTexture(&tg->grassLayers);
    if (tgActive == tg) tgActive = NULL;
    MemSet(tg, 0, sizeof(*tg));
}

void TerrainGrass_BeginFrame(TerrainGrassState* tg, const FrustumPlanes* frustum)
{
    if (!TerrainGrass_Init(tg)) return;
    tg->drawCount = 0u;
    tg->frustum = *frustum;
}

bool TerrainGrass_UploadChunk(TerrainGrassState* tg, TerrainGrassChunk* chunk, const GrassInstance* src)
{
    if (!tg || !tg->initialized || !chunk || !src || chunk->count == 0u) return false;
    if (chunk->slot != UINT32_MAX) return true;
    if (tg->freeCount == 0u) return false;

    u32 slot = tg->freeSlots[--tg->freeCount];
    GrassInstance instances[GRASS_PER_CHUNK];
    for (u32 i = 0; i < chunk->count; i++) {
        instances[i] = src[i];
        instances[i].positionZChunkIndex = (instances[i].positionZChunkIndex & 0xFFFFu) | (slot << 16);
    }

    TerrainGrassChunkInfo info = { .origin = { chunk->origin.x, 0.0f, chunk->origin.z, 0.0f } };
    UpdateGPUBuffer(tg->grassBuffer, instances, chunk->count * sizeof(GrassInstance), (size_t)slot * GRASS_PER_CHUNK * sizeof(GrassInstance));
    UpdateGPUBuffer(tg->chunkBuffer, &info, sizeof(info), (size_t)slot * sizeof(TerrainGrassChunkInfo));
    chunk->slot = slot;
    return true;
}

void TerrainGrass_FreeChunk(TerrainGrassState* tg, TerrainGrassChunk* chunk)
{
    if (!tg || !chunk || chunk->slot == UINT32_MAX) { if (chunk) *chunk = (TerrainGrassChunk){ .slot = UINT32_MAX }; return; }
    if (tg->freeCount < T_GRASS_MAX_SLOTS)
        tg->freeSlots[tg->freeCount++] = (u16)chunk->slot;
    *chunk = (TerrainGrassChunk){ .slot = UINT32_MAX };
}

void TerrainGrass_AppendDraw(TerrainGrassState* tg, const TerrainGrassChunk* chunk)
{
    if (!tg || !tg->initialized || !chunk || chunk->slot == UINT32_MAX || chunk->count == 0u) return;
    if (!CheckAABBCulled(Vec3Load(&chunk->aabbMin.x), Vec3Load(&chunk->aabbMax.x), tg->frustum.planes)) return;
    if (tg->drawCount >= T_GRASS_MAX_DRAWS) return;
    tg->draws[tg->drawCount++] = (SDL_GPUIndirectDrawCommand){ 6u, chunk->count, 0u, chunk->slot * GRASS_PER_CHUNK };
}

void TerrainGrass_EndFrame(TerrainGrassState* tg)
{
    if (!tg || !tg->initialized || tg->drawCount == 0u) return;
    UpdateGPUBufferCycle(tg->indirectBuffer, tg->draws, tg->drawCount * sizeof(SDL_GPUIndirectDrawCommand), 0, true);
}

static void TerrainGrassFillColor(GrassFSParams* fs)
{
    fs->grassColor[0] = 0x49 / 255.0f;
    fs->grassColor[1] = 0x57 / 255.0f;
    fs->grassColor[2] = 0x27 / 255.0f;
    fs->grassColorVariant[0] = Minf32(fs->grassColor[0] * 1.35f, 1.0f);
    fs->grassColorVariant[1] = Minf32(fs->grassColor[1] * 1.35f, 1.0f);
    fs->grassColorVariant[2] = Minf32(fs->grassColor[2] * 1.35f, 1.0f);
}

void tRenderGrass(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass)
{
    TerrainGrassState* tg = tgActive;
    if (!tg || !tg->initialized || tg->drawCount == 0u || !tg->pipeline || !tg->grassLayers.handle) return;

    SDL_BindGPUGraphicsPipeline(pass, tg->pipeline);
    SDL_GPUBufferBinding instanceBinding = { tg->grassBuffer, 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &instanceBinding, 1);
    SDL_GPUBuffer* storage[1] = { tg->chunkBuffer };
    SDL_BindGPUVertexStorageBuffers(pass, 0, storage, 1);
    SDL_GPUTextureSamplerBinding grassTex = { .texture = tg->grassLayers.handle, .sampler = g_RenderState.sampler };
    SDL_BindGPUFragmentSamplers(pass, 0, &grassTex, 1);

    TerrainAuthoring* authoring = Terrain_GetAuthoring();
    GrassVSParams vs = {0};
    vs.view = g_Camera.view;
    vs.proj = g_Camera.projection;
    vs.cameraTime[0] = g_Camera.position.x;
    vs.cameraTime[1] = g_Camera.position.y;
    vs.cameraTime[2] = g_Camera.position.z;
    vs.cameraTime[3] = (f32)SDL_GetTicks() * 0.001f;
    SDL_PushGPUVertexUniformData(cmd, 0, &vs, sizeof(vs));

    float3 sun = GetRenderSunDirection();
    GrassFSParams fs = {0};
    fs.sunDirection[0] = sun.x;
    fs.sunDirection[1] = sun.y;
    fs.sunDirection[2] = sun.z;
    TerrainGrassFillColor(&fs);
    SDL_PushGPUFragmentUniformData(cmd, 0, &fs, sizeof(fs));

    SDL_DrawGPUPrimitivesIndirect(pass, tg->indirectBuffer, 0, tg->drawCount);
}
