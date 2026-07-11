#include "Include/Terrain.h"
#include "Source/Terrain/TransvoxelUnity.h"
#include "Source/Terrain/TerrainInternal.h"
#include "Include/Graphics.h"
#include "Include/FileSystem.h"
#include "Include/Algorithm.h"

#define TERRAIN_LAYER_COUNT 4u
#define TERRAIN_ALBEDO_SIZE 2048
#define TERRAIN_DETAIL_SIZE 1024

typedef struct TerrainPortState_
{
    bool initialized;
    bool enabled;
    TerrainGenParams genParams;
    TerrainAuthoring authoring;
    Texture albedoLayers;
    Texture normalLayers;
    Texture armLayers;
} TerrainPortState;

static TerrainPortState tp;

static const char* const albedoPaths[TERRAIN_LAYER_COUNT] = {
    "Assets/Textures/Terrain/rocky_terrain_02_diff_2k.png",
    "Assets/Textures/Terrain/brown_mud_leaves_01_diff_2k.png",
    "Assets/Textures/Terrain/rocky_terrain_diff_2k.png",
    "Assets/Textures/Terrain/sandydrysoil-albedo2b.png"
};

static const char* const normalPaths[TERRAIN_LAYER_COUNT] = {
    "Assets/Textures/Terrain/rocky_terrain_02_nor_dx_1k.png",
    "Assets/Textures/Terrain/brown_mud_leaves_01_nor_dx_1k.png",
    "Assets/Textures/Terrain/rocky_terrain_nor_dx_1k.png",
    "Assets/Textures/Terrain/sandydrysoil-normal.png"
};

static const char* const metallicRoughnessPaths[TERRAIN_LAYER_COUNT] = {
    "Assets/Textures/Terrain/rocky_terrain_02_arm_1k.png",
    "Assets/Textures/Terrain/brown_mud_leaves_01_arm_2k.png",
    "Assets/Textures/Terrain/rocky_terrain_arm_1k.png",
    "Assets/Textures/Terrain/brown_mud_leaves_01_arm_2k.png"
};

static void TerrainAuthoringDefaults(TerrainAuthoring* authoring)
{
    SDL_memset(authoring, 0, sizeof(*authoring));
    for (u32 i = 0; i < TERRAIN_LAYER_COUNT; i++)
    {
        authoring->layers[i].enabled = true;
        CopyString(authoring->layers[i].albedo, sizeof(authoring->layers[i].albedo), albedoPaths[i]);
        CopyString(authoring->layers[i].normal, sizeof(authoring->layers[i].normal), normalPaths[i]);
        CopyString(authoring->layers[i].metallicRoughness, sizeof(authoring->layers[i].metallicRoughness), metallicRoughnessPaths[i]);
    }
    authoring->grassDensity = 4.0f;
    authoring->grassScaleMin = 0.6f;
    authoring->grassScaleMax = 1.2f;
    authoring->grassViewDistance = 300.0f;
    CopyString(authoring->grassColorHex, sizeof(authoring->grassColorHex), "77AA55FF");
}

static void TerrainInitMaterialTextures(void)
{
    if (tp.albedoLayers.handle && tp.normalLayers.handle && tp.armLayers.handle) return;
    tp.albedoLayers = LoadTextureArray(albedoPaths, TERRAIN_LAYER_COUNT, TERRAIN_ALBEDO_SIZE, true, "TerrainAlbedo", "terrain albedo");
    tp.normalLayers = LoadTextureArray(normalPaths, TERRAIN_LAYER_COUNT, TERRAIN_DETAIL_SIZE, false, "TerrainNormal", "terrain normal");
    tp.armLayers = LoadTextureArray(metallicRoughnessPaths, TERRAIN_LAYER_COUNT, TERRAIN_DETAIL_SIZE, false, "TerrainARM", "terrain arm");
}

TerrainAuthoring* Terrain_GetAuthoring(void)
{
    if (!tp.initialized) Terrain_Init();
    return &tp.authoring;
}

void Terrain_Init(void)
{
    if (tp.initialized) return;
    tp.genParams = Terrain_DefaultGenParams();
    TerrainAuthoringDefaults(&tp.authoring);
    TerrainDensity_SetParams(&tp.genParams);
    TerrainEdit_Init();
    TerrainInitMaterialTextures();
    tp.initialized = true;
}

void Terrain_Destroy(void)
{
    if (!tp.initialized) return;
    ReleaseTexture(&tp.albedoLayers);
    ReleaseTexture(&tp.normalLayers);
    ReleaseTexture(&tp.armLayers);
    TerrainEdit_Destroy();
    SDL_memset(&tp, 0, sizeof(tp));
}

void Terrain_Update(const Camera* camera)
{
    (void)camera;
}

void Terrain_SetEnabled(bool enabled)
{
    if (!tp.initialized) Terrain_Init();
    tp.enabled = enabled;
}

bool Terrain_GetEnabled(void)
{
    return tp.initialized && tp.enabled;
}

void Terrain_InvalidatePhysics(void)
{
    tInvalidatePhysics();
}

void Terrain_ApplyGenParams(const TerrainGenParams* params)
{
    if (!params) return;
    if (!tp.initialized) Terrain_Init();
    tp.genParams = *params;
    tp.genParams.fixedWorldSize = (u32)Clamps32((s32)tp.genParams.fixedWorldSize, TERRAIN_FIXED_WORLD_MIN_SIZE, TERRAIN_FIXED_WORLD_MAX_SIZE);
    TerrainDensity_SetParams(&tp.genParams);
    tInvalidateAll();
}

const TerrainGenParams* Terrain_GetGenParams(void)
{
    if (!tp.initialized) Terrain_Init();
    return &tp.genParams;
}

void Terrain_CreateWorld(const TerrainGenParams* params)
{
    Terrain_ApplyGenParams(params);
    tp.enabled = true;
}

void Terrain_DeleteWorld(void)
{
    tp.enabled = false;
    TerrainEdit_Clear();
    tInvalidateAll();
}

void Terrain_SetBrushCursor(float3 position, f32 radius, bool active)
{
    tSetBrushCursor(position, radius, active);
}

void Terrain_SculptSphere(float3 center, f32 radius, f32 strength, f32 softness)
{
    if (!Terrain_GetEnabled()) return;
    float3 mn, mx;
    TerrainEdit_SculptSphere(center, radius, strength, softness, &mn, &mx);
    tInvalidateRegion(mn, mx);
}

void Terrain_PaintSphere(float3 center, f32 radius, u32 layer, f32 strength, f32 softness)
{
    if (!Terrain_GetEnabled()) return;
    float3 mn, mx;
    TerrainEdit_PaintSphere(center, radius, (u8)Clamps32((s32)layer + 1, 1, 15), strength, softness, &mn, &mx);
    tInvalidateRegion(mn, mx);
}

s32 Terrain_Raycast(float3 origin, float3 dir, f32 maxDist, u32 maxLod, BVHHit* hit)
{
    (void)origin;
    (void)dir;
    (void)maxDist;
    (void)maxLod;
    (void)hit;
    return 0;
}

s32 Terrain_RaycastField(float3 origin, float3 dir, f32 maxDist, BVHHit* hit)
{
    if (!Terrain_GetEnabled()) return 0;
    f32 t = 0.0f;
    f32 lastT = 0.0f;
    for (u32 step = 0; step < 256u && t < maxDist; step++)
    {
        f32 px = origin.x + dir.x * t;
        f32 py = origin.y + dir.y * t;
        f32 pz = origin.z + dir.z * t;
        f32 sdf = TerrainDensity_At(px, py, pz);
        if (sdf < 0.0f)
        {
            f32 lo = lastT;
            f32 hi = t;
            for (u32 i = 0; i < 16u; i++)
            {
                f32 mid = (lo + hi) * 0.5f;
                f32 mx = origin.x + dir.x * mid;
                f32 my = origin.y + dir.y * mid;
                f32 mz = origin.z + dir.z * mid;
                if (TerrainDensity_At(mx, my, mz) < 0.0f) hi = mid; else lo = mid;
            }
            hit->hit.t = lo;
            hit->hit.u = 0.0f;
            hit->hit.v = 0.0f;
            hit->triIndex = 0u;
            hit->entityIdx = 0xFFFFFFFFu;
            hit->groupIdx = 0u;
            hit->skinnedSet = 0xFFFFFFFFu;
            hit->bundleIdx = 0xFFFFFFFFu;
            return 1;
        }
        lastT = t;
        t += Maxf32(sdf * 0.5f, 0.3f);
    }
    return 0;
}

bool Terrain_HasDraws(void)
{
    return false;
}

TerrainStats Terrain_GetStats(void)
{
    return (TerrainStats){0};
}

void Terrain_GPUFlush(SDL_GPUCommandBuffer* cmd)
{
    (void)cmd;
}

void Terrain_RenderDepth(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, mat4x4 viewProj)
{
    (void)cmd;
    (void)pass;
    (void)viewProj;
}

void Terrain_RenderForward(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, mat4x4 viewProj, u32 width, u32 height)
{
    (void)cmd;
    (void)pass;
    (void)viewProj;
    (void)width;
    (void)height;
}

void Terrain_RenderWireframe(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget, SDL_GPUDepthStencilTargetInfo* depthTarget, mat4x4 viewProj)
{
    (void)cmd;
    (void)colorTarget;
    (void)depthTarget;
    (void)viewProj;
}

bool Terrain_GetMaterialTextures(SDL_GPUTexture** albedo, SDL_GPUTexture** normal, SDL_GPUTexture** arm)
{
    if (!tp.initialized) Terrain_Init();
    TerrainInitMaterialTextures();
    if (albedo) *albedo = tp.albedoLayers.handle;
    if (normal) *normal = tp.normalLayers.handle;
    if (arm) *arm = tp.armLayers.handle;
    return tp.albedoLayers.handle && tp.normalLayers.handle && tp.armLayers.handle;
}

u32 Terrain_NumEditedRegions(void)
{
    return TerrainEdit_NumChunks();
}

bool Terrain_SaveEditChunks(const char* path)
{
    return tp.initialized && TerrainEdit_SaveChunks(path);
}

bool Terrain_LoadEditChunks(const char* path)
{
    if (!tp.initialized) Terrain_Init();
    return TerrainEdit_LoadChunks(path);
}

static char* TerrainWriteString(char* p, const char* s)
{
    u32 len = (u32)StringLength(s);
    MemCopy(p, s, len);
    return p + len;
}

static char* TerrainWriteF32(char* p, const char* key, f32 value, int decimals)
{
    p = TerrainWriteString(p, key);
    *p++ = ' ';
    p += FloatToString(p, value, decimals);
    *p++ = '\n';
    return p;
}

static char* TerrainWriteBool(char* p, const char* key, bool value)
{
    p = TerrainWriteString(p, key);
    *p++ = ' ';
    *p++ = value ? '1' : '0';
    *p++ = '\n';
    return p;
}

static bool TerrainKeyIs(const char* line, const char* key, const char** value)
{
    u32 len = (u32)StringLength(key);
    for (u32 i = 0; i < len; i++)
        if (line[i] != key[i]) return false;
    if (line[len] != ' ') return false;
    *value = line + len + 1;
    return true;
}

static bool TerrainChunksPathFromWorld(const char* terrainPath, char* dst, u32 dstSize)
{
    u32 len = Minu32((u32)StringLength(terrainPath), dstSize - 1u);
    MemCopy(dst, terrainPath, len);
    dst[len] = '\0';
    ChangeExtension(dst, (int)len, "chunks");
    return true;
}

bool Terrain_SaveWorld(const char* path)
{
    if (!path || !path[0] || !Terrain_GetEnabled()) return false;
    EnsurePath(path);

    char* text = (char*)SDL_malloc(4096u);
    if (!text) return false;

    TerrainGenParams* params = &tp.genParams;
    TerrainAuthoring* authoring = &tp.authoring;
    char* p = text;
    p = TerrainWriteString(p, "terrain 1\n");
    p = TerrainWriteBool(p, "fixed_chunk_size", params->fixedArea);
    p = TerrainWriteF32(p, "fixed_world_size", (f32)params->fixedWorldSize, 0);
    p = TerrainWriteBool(p, "island", params->island);
    p = TerrainWriteF32(p, "seed", (f32)params->seed, 0);
    p = TerrainWriteF32(p, "sea_level", params->seaLevel, 3);
    p = TerrainWriteF32(p, "base_height", params->baseHeight, 3);
    p = TerrainWriteF32(p, "hill_amplitude", params->hillAmplitude, 3);
    p = TerrainWriteF32(p, "hill_frequency", params->hillFrequency, 6);
    p = TerrainWriteF32(p, "ridge_amplitude", params->ridgeAmplitude, 3);
    p = TerrainWriteF32(p, "ridge_frequency", params->ridgeFrequency, 6);
    p = TerrainWriteF32(p, "cave_amplitude", params->carveAmplitude, 3);
    p = TerrainWriteF32(p, "cave_frequency", params->carveFrequency, 6);
    p = TerrainWriteF32(p, "island_radius", params->islandRadius, 3);
    p = TerrainWriteF32(p, "island_falloff", params->islandFalloff, 3);
    p = TerrainWriteF32(p, "grass_density", authoring->grassDensity, 3);
    p = TerrainWriteF32(p, "grass_view_distance", authoring->grassViewDistance, 3);

    WriteAllBytes(path, text, (unsigned long)(p - text));
    SDL_free(text);

    char chunksPath[512];
    if (!TerrainChunksPathFromWorld(path, chunksPath, sizeof(chunksPath))) return false;
    EnsurePath(chunksPath);
    return FileExist(path) && Terrain_SaveEditChunks(chunksPath);
}

bool Terrain_LoadWorld(const char* path)
{
    if (!path || !path[0]) return false;
    if (!tp.initialized) Terrain_Init();

    char* text = ReadAllFileAlloc(path);
    if (!text) return false;

    TerrainGenParams params = Terrain_DefaultGenParams();
    TerrainAuthoringDefaults(&tp.authoring);
    const char* value;
    char* line = text;
    while (line && *line)
    {
        char* next = line;
        while (*next && *next != '\n') next++;
        bool hadNewline = *next == '\n';
        *next = '\0';

        if      (TerrainKeyIs(line, "fixed_chunk_size", &value)) params.fixedArea = value[0] == '1';
        else if (TerrainKeyIs(line, "fixed_world_size", &value)) { f32 f; ParseFloat(value, &f); params.fixedWorldSize = (u32)Clamps32((s32)f, TERRAIN_FIXED_WORLD_MIN_SIZE, TERRAIN_FIXED_WORLD_MAX_SIZE); }
        else if (TerrainKeyIs(line, "island", &value)) params.island = value[0] == '1';
        else if (TerrainKeyIs(line, "seed", &value)) { f32 f; ParseFloat(value, &f); params.seed = (u32)f; }
        else if (TerrainKeyIs(line, "sea_level", &value)) ParseFloat(value, &params.seaLevel);
        else if (TerrainKeyIs(line, "base_height", &value)) ParseFloat(value, &params.baseHeight);
        else if (TerrainKeyIs(line, "hill_amplitude", &value)) ParseFloat(value, &params.hillAmplitude);
        else if (TerrainKeyIs(line, "hill_frequency", &value)) ParseFloat(value, &params.hillFrequency);
        else if (TerrainKeyIs(line, "ridge_amplitude", &value)) ParseFloat(value, &params.ridgeAmplitude);
        else if (TerrainKeyIs(line, "ridge_frequency", &value)) ParseFloat(value, &params.ridgeFrequency);
        else if (TerrainKeyIs(line, "cave_amplitude", &value)) ParseFloat(value, &params.carveAmplitude);
        else if (TerrainKeyIs(line, "cave_frequency", &value)) ParseFloat(value, &params.carveFrequency);
        else if (TerrainKeyIs(line, "island_radius", &value)) ParseFloat(value, &params.islandRadius);
        else if (TerrainKeyIs(line, "island_falloff", &value)) ParseFloat(value, &params.islandFalloff);
        else if (TerrainKeyIs(line, "grass_density", &value)) ParseFloat(value, &tp.authoring.grassDensity);
        else if (TerrainKeyIs(line, "grass_view_distance", &value)) ParseFloat(value, &tp.authoring.grassViewDistance);

        line = hadNewline ? next + 1 : NULL;
    }
    FreeAllText(text);

    char chunksPath[512];
    if (!TerrainChunksPathFromWorld(path, chunksPath, sizeof(chunksPath))) return false;
    if (FileExist(chunksPath) && !Terrain_LoadEditChunks(chunksPath)) return false;
    Terrain_CreateWorld(&params);
    return true;
}
