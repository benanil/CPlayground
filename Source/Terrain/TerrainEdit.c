// sparse terrain edits: sculpted density deltas and painted material indices live on
// the world fixed lod0 voxel lattice (1m), grouped into 16^3 grids per lod0 chunk
// coordinate. every lod samples the same lattice points, so the overlay stays
// bit-identical across lod boundaries and transvoxel seams remain crack free.
// the brush writes from the main thread while worker jobs overlay concurrently:
// a mutex guards the map, plus a lock free world AABB early-out for untouched chunks
#include "TerrainInternal.h"
#include "Include/DataStructures/HashMap.h"
#include "Include/FileSystem.h"
#include "Include/Platform.h"
#include "Extern/sdefl.h"
#include "Extern/sinfl.h"
#include <SDL3/SDL.h>

#define TERRAIN_EDIT_VALUES (TERRAIN_EDIT_CELLS * TERRAIN_EDIT_CELLS * TERRAIN_EDIT_CELLS)
#define TERRAIN_EDIT_CHUNKS_MAGIC 0x4B4E4843u // "CHNK"
#define TERRAIN_EDIT_CHUNKS_VERSION 2u

typedef struct TerrainEditChunk_
{
    s16 delta[TERRAIN_EDIT_VALUES];    // quantized sdf offsets, same scale as the field
    // paint per voxel: layer A (4 bits) | layer B (4 bits) | blend weight (8 bits,
    // 0 = pure A, 255 = pure B). index 0 is the procedural default, painting
    // accumulates pressure into the weight so transitions stay smooth gradients
    u16 material[TERRAIN_EDIT_VALUES];
} TerrainEditChunk;

typedef struct TerrainEditChunkFileHeader_
{
    u32 magic;
    u32 version;
    u32 valuesPerChunk;
    u32 chunkCount;
    u64 rawSize;
    u64 compressedSize;
} TerrainEditChunkFileHeader;

typedef struct TerrainEditChunkFileRecord_
{
    s32 x, y, z;
    s16 delta[TERRAIN_EDIT_VALUES];
    u16 material[TERRAIN_EDIT_VALUES];
} TerrainEditChunkFileRecord;

typedef struct TerrainEditState_
{
    bool      initialized;
    HashMap   map;          // packed lod0 chunk coord -> TerrainEditChunk*
    SDL_Mutex* lock;
    // world meter bounds of every edit, workers skip the lock outside of it.
    // only grows; cleared with the map
    bool boundsValid;
	int3 boundsMin; 
	int3 boundsMax;
} TerrainEditState;

static TerrainEditState g_TerrainEdit;

void TerrainEdit_Init(void)
{
    if (g_TerrainEdit.initialized) return;
    g_TerrainEdit.map = HMCreate(64, sizeof(TerrainEditChunk*));
    g_TerrainEdit.lock = SDL_CreateMutex();
    g_TerrainEdit.boundsValid = false;
    g_TerrainEdit.initialized = true;
}

void TerrainEdit_Clear(void)
{
    if (!g_TerrainEdit.initialized) return;
    SDL_LockMutex(g_TerrainEdit.lock);
    for (u32 i = 0; i < g_TerrainEdit.map.count; i++)
        SDL_free(((TerrainEditChunk**)g_TerrainEdit.map.values)[i]);
    HMClear(&g_TerrainEdit.map);
    g_TerrainEdit.boundsValid = false;
    SDL_UnlockMutex(g_TerrainEdit.lock);
}

void TerrainEdit_Destroy(void)
{
    if (!g_TerrainEdit.initialized) return;
    TerrainEdit_Clear();
    HMDestroy(&g_TerrainEdit.map);
    SDL_DestroyMutex(g_TerrainEdit.lock);
    g_TerrainEdit.initialized = false;
}

u32 TerrainEdit_NumChunks(void)
{
    return g_TerrainEdit.initialized ? g_TerrainEdit.map.count : 0u;
}

static TerrainEditChunk* TerrainEditFind(s32 ex, s32 ey, s32 ez)
{
	TerrainEditChunk** slot = (TerrainEditChunk* *)HMFind(&g_TerrainEdit.map, tChunkKey((int3){ex, ey, ez}));
    return slot ? *slot : NULL;
}

// callers hold the lock. creates the grid on first touch
static TerrainEditChunk* TerrainEditGetOrCreate(s32 ex, s32 ey, s32 ez)
{
    TerrainEditChunk* chunk = TerrainEditFind(ex, ey, ez);
    if (chunk) return chunk;
    chunk = (TerrainEditChunk*)SDL_calloc(1, sizeof(TerrainEditChunk));
    if (!chunk) return NULL;
	HMInsert(&g_TerrainEdit.map, tChunkKey((int3){ex, ey, ez}), &chunk);
    return chunk;
}

static void TerrainEditGrowBounds(int3 mn, int3 mx)
{
    if (!g_TerrainEdit.boundsValid)
    {
        g_TerrainEdit.boundsMin = mn;
		g_TerrainEdit.boundsMax = mx;
        g_TerrainEdit.boundsValid = true;
        return;
    }
    g_TerrainEdit.boundsMin = I3Min(g_TerrainEdit.boundsMin, mn);
    g_TerrainEdit.boundsMax = I3Max(g_TerrainEdit.boundsMax, mx);
}

static s8 TerrainEdit_ApplyDelta(s8 sample, s16 delta)
{
    s32 v = (s32)sample + (s32)delta;
    v = Clamps32(v, -127, 127);
    if (v == 0 && delta != 0) v = delta < 0 ? -1 : 1;
    return (s8)v;
}

void TerrainEdit_OverlayChunk(s32 cx, s32 cy, s32 cz, s8* samples)
{
    if (!g_TerrainEdit.initialized || !g_TerrainEdit.boundsValid) return;

    // sample lattice of this chunk in world meters: (chunk*16 + i - 1) << lod
    int3 wmin = (int3){ ((cx * T_CHUNK_CELLS) - 1),
                        ((cy * T_CHUNK_CELLS) - 1),
                        ((cz * T_CHUNK_CELLS) - 1)};

    int3 wmax = (int3){ wmin.x + (T_SAMPLES_AXIS - 1),
                        wmin.y + (T_SAMPLES_AXIS - 1),
                        wmin.z + (T_SAMPLES_AXIS - 1)};
    
    if (wmax.x < g_TerrainEdit.boundsMin.x || wmin.x > g_TerrainEdit.boundsMax.x) return;
    if (wmax.y < g_TerrainEdit.boundsMin.y || wmin.y > g_TerrainEdit.boundsMax.y) return;
    if (wmax.z < g_TerrainEdit.boundsMin.z || wmin.z > g_TerrainEdit.boundsMax.z) return;

    SDL_LockMutex(g_TerrainEdit.lock);
    // consecutive x samples usually live in the same 16^3 grid, cache the lookup
    s32 lastEx = INT32_MIN, lastEy = 0, lastEz = 0;
    TerrainEditChunk* last = NULL;
    s8* dst = samples;
    for (s32 z = 0; z < T_SAMPLES_AXIS; z++)
    for (s32 y = 0; y < T_SAMPLES_AXIS; y++)
    for (s32 x = 0; x < T_SAMPLES_AXIS; x++, dst++)
    {
        s32 wx = wmin.x + x, wy = wmin.y + y, wz = wmin.z + z;
        s32 ex = wx >> 4, ey = wy >> 4, ez = wz >> 4;
        if (ex != lastEx || ey != lastEy || ez != lastEz)
        {
            last = TerrainEditFind(ex, ey, ez);
            lastEx = ex; lastEy = ey; lastEz = ez;
        }
        if (!last) continue;
        s16 d = last->delta[((wz & 15) * TERRAIN_EDIT_CELLS + (wy & 15)) * TERRAIN_EDIT_CELLS + (wx & 15)];
        if (d == 0) continue;
        *dst = TerrainEdit_ApplyDelta(*dst, d);
    }
    SDL_UnlockMutex(g_TerrainEdit.lock);
}

// vertex material from the containing voxel: the stored A/B/weight splits into the
// two spare slots, wA = 255 - w, wB = w. smoothness comes from the painted weight
// gradient itself (paint accumulates pressure into w), no mesh time gathering
void TerrainEdit_MaterialWeights(float3 pos, u8 outIndex[2], u8 outWeight[2])
{
    outIndex[0] = 0; outIndex[1] = 0;
    outWeight[0] = 255; outWeight[1] = 0;
    u16 packed = TerrainEdit_MaterialAt((s32)Floorf32(pos.x), (s32)Floorf32(pos.y), (s32)Floorf32(pos.z));
    if (packed == 0) return;
    outIndex[0]  = (u8)(packed & 15u);
    outIndex[1]  = (u8)((packed >> 4) & 15u);
    outWeight[1] = (u8)(packed >> 8);
    outWeight[0] = (u8)(255u - outWeight[1]);
}

static inline bool Int3Outside(int3 mn, int3 mx, s32 wx, s32 wy, s32 wz)
{
	return wx < g_TerrainEdit.boundsMin.x || wx > g_TerrainEdit.boundsMax.x ||
           wy < g_TerrainEdit.boundsMin.y || wy > g_TerrainEdit.boundsMax.y ||
	       wz < g_TerrainEdit.boundsMin.z || wz > g_TerrainEdit.boundsMax.z;
}

// sculpted sdf offset in meters at a lod0 voxel, for field raycasts
f32 TerrainEdit_DeltaAt(s32 wx, s32 wy, s32 wz)
{
    if (!g_TerrainEdit.initialized || !g_TerrainEdit.boundsValid) return 0.0f;
    if (Int3Outside(g_TerrainEdit.boundsMin, g_TerrainEdit.boundsMax, wx, wy, wz)) return 0.0f;

    SDL_LockMutex(g_TerrainEdit.lock);
    TerrainEditChunk* chunk = TerrainEditFind(wx >> 4, wy >> 4, wz >> 4);
    s16 delta = chunk ? chunk->delta[((wz & 15) * TERRAIN_EDIT_CELLS + (wy & 15)) * TERRAIN_EDIT_CELLS + (wx & 15)] : 0;
    SDL_UnlockMutex(g_TerrainEdit.lock);
    return (f32)delta * (TERRAIN_SDF_CLAMP / 127.0f);
}

u16 TerrainEdit_MaterialAt(s32 wx, s32 wy, s32 wz)
{
    if (!g_TerrainEdit.initialized || !g_TerrainEdit.boundsValid) return 0;
    if (Int3Outside(g_TerrainEdit.boundsMin, g_TerrainEdit.boundsMax, wx, wy, wz)) return 0;

    SDL_LockMutex(g_TerrainEdit.lock);
    TerrainEditChunk* chunk = TerrainEditFind(wx >> 4, wy >> 4, wz >> 4);
    u16 material = chunk ? chunk->material[((wz & 15) * TERRAIN_EDIT_CELLS + (wy & 15)) * TERRAIN_EDIT_CELLS + (wx & 15)] : 0;
    SDL_UnlockMutex(g_TerrainEdit.lock);
    return material;
}

// shared brush voxel walk: calls apply(per voxel weight 0..1) for every lod0 lattice
// point inside the sphere. weight is 1 in the core and smooths to 0 at the radius
typedef void (*TerrainBrushFn)(TerrainEditChunk* chunk, u32 voxelIdx, f32 weight, f32 param, f32 param2);

static void TerrainBrushWalk(float3 center, f32 radius, f32 softness, f32 param, f32 param2,
                             TerrainBrushFn apply, float3* outMin, float3* outMax)
{
    s32 mn[3] = { (s32)Floorf32(center.x - radius), (s32)Floorf32(center.y - radius), (s32)Floorf32(center.z - radius) };
    s32 mx[3] = { (s32)Ceilf(center.x + radius), (s32)Ceilf(center.y + radius), (s32)Ceilf(center.z + radius) };
    mn[1] = Maxs32(mn[1], (s32)Floorf32(TERRAIN_BEDROCK_Y) + 1);
    if (mx[1] <= (s32)Floorf32(TERRAIN_BEDROCK_Y))
    {
        outMin->x = (f32)mn[0]; outMin->y = (f32)mn[1]; outMin->z = (f32)mn[2];
        outMax->x = (f32)mx[0]; outMax->y = (f32)mx[1]; outMax->z = (f32)mx[2];
        return;
    }
    f32 core = radius * (1.0f - Clampf32(softness, 0.0f, 1.0f));

    SDL_LockMutex(g_TerrainEdit.lock);
    for (s32 wz = mn[2]; wz <= mx[2]; wz++)
    for (s32 wy = mn[1]; wy <= mx[1]; wy++)
    {
        TerrainEditChunk* chunk = NULL;
        s32 lastEx = INT32_MIN;
        for (s32 wx = mn[0]; wx <= mx[0]; wx++)
        {
            f32 dx = (f32)wx - center.x, dy = (f32)wy - center.y, dz = (f32)wz - center.z;
            f32 dist = Sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist > radius) continue;
            f32 weight = 1.0f;
            if (dist > core && radius > core)
            {
                f32 t = (dist - core) / (radius - core);
                weight = 1.0f - (t * t * (3.0f - 2.0f * t));
            }
            s32 ex = wx >> 4;
            if (ex != lastEx || !chunk)
            {
                chunk = TerrainEditGetOrCreate(ex, wy >> 4, wz >> 4);
                lastEx = ex;
            }
            if (!chunk) continue;
            u32 idx = (u32)(((wz & 15) * TERRAIN_EDIT_CELLS + (wy & 15)) * TERRAIN_EDIT_CELLS + (wx & 15));
            apply(chunk, idx, weight, param, param2);
        }
    }
	TerrainEditGrowBounds((int3){mn[0], mn[1], mn[2]}, (int3){mx[0], mx[1], mx[2]});
    SDL_UnlockMutex(g_TerrainEdit.lock);

    outMin->x = (f32)mn[0]; outMin->y = (f32)mn[1]; outMin->z = (f32)mn[2];
    outMax->x = (f32)mx[0]; outMax->y = (f32)mx[1]; outMax->z = (f32)mx[2];
}

static void TerrainBrushSculpt(TerrainEditChunk* chunk, u32 idx, f32 weight, f32 strength, f32 unused)
{
    (void)unused;
    // positive strength raises the surface (lowers the sdf). quantized like the field
    f32 centerPressure = weight * weight;
    f32 d = -strength * centerPressure * (127.0f / TERRAIN_SDF_CLAMP);
    s32 v = (s32)chunk->delta[idx] + (s32)(d >= 0.0f ? d + 0.5f : d - 0.5f);
    chunk->delta[idx] = (s16)Clamps32(v, -32767, 32767);
}

// pressure painting: the brush pushes the voxel's A/B blend weight toward the painted
// texture, replacing the lesser used slot when a third texture arrives. repeated
// strokes deepen the gradient, so transitions stay smooth over the falloff
static void TerrainBrushPaint(TerrainEditChunk* chunk, u32 idx, f32 weight, f32 texture, f32 strength)
{
    u32 tex = (u32)texture & 15u;
    u16 packed = chunk->material[idx];
    u32 a = packed & 15u, b = (packed >> 4) & 15u, w = packed >> 8;
    f32 pressure = weight;

    if (a == tex)
    {
        pressure = -pressure; // weight slides toward 0 = pure A
    }
    else if (b != tex)
    {
        // texture is in neither slot: replace whichever side currently matters less
        if (w < 128u) {
            b = tex;
            w = (u32)Lerpf((f32)w, 255.0f, pressure * 0.85f);
        }
        else {
            a = tex;
            w = (u32)Lerpf((f32)w, 0.0f, pressure * 0.85f);
            pressure = -pressure;
        }
    }

    w = (u32)Clampf32((f32)w + pressure * strength, 0.0f, 255.0f);
    chunk->material[idx] = (u16)(a | (b << 4) | (w << 8));
}

void TerrainEdit_SculptSphere(float3 center, f32 radius, f32 strength, f32 softness,
                              float3* outMin, float3* outMax)
{
    TerrainEdit_Init();
    TerrainBrushWalk(center, radius, softness, strength, 0.0f, TerrainBrushSculpt, outMin, outMax);
}

void TerrainEdit_PaintSphere(float3 center, f32 radius, u8 material, f32 strength, f32 softness,
                             float3* outMin, float3* outMax)
{
    TerrainEdit_Init();
    TerrainBrushWalk(center, radius, softness, (f32)material, strength, TerrainBrushPaint, outMin, outMax);
}

// ---------------------------------------------------------------------------------
// persistence: binary .chunks file with deflated 16^3 density/material payloads
// ---------------------------------------------------------------------------------

bool TerrainEdit_SaveChunks(const char* path)
{
    TerrainEdit_Init();
    if (!path || !path[0]) { AX_WARN("terrain chunks save skipped: empty path"); return false; }

    SDL_LockMutex(g_TerrainEdit.lock);
    u32 chunkCount = g_TerrainEdit.map.count;
    u64 rawSize = (u64)chunkCount * sizeof(TerrainEditChunkFileRecord);
    if (rawSize > (u64)INT32_MAX) {
        SDL_UnlockMutex(g_TerrainEdit.lock);
        AX_WARN("terrain chunks save failed: too much edit data");
        return false;
    }

    TerrainEditChunkFileRecord* records = rawSize ? (TerrainEditChunkFileRecord*)SDL_malloc((size_t)rawSize) : NULL;
    if (rawSize && !records) {
        SDL_UnlockMutex(g_TerrainEdit.lock);
        AX_WARN("terrain chunks save failed: out of memory");
        return false;
    }

    for (u32 i = 0; i < chunkCount; i++)
    {
        TerrainEditChunkFileRecord* record = &records[i];
        tCoordsFromKey(g_TerrainEdit.map.keys[i], &record->x, &record->y, &record->z);
        TerrainEditChunk* chunk = ((TerrainEditChunk**)g_TerrainEdit.map.values)[i];
        MemCopy(record->delta, chunk->delta, sizeof(record->delta));
        MemCopy(record->material, chunk->material, sizeof(record->material));
    }
    SDL_UnlockMutex(g_TerrainEdit.lock);

    u64 compressBound = rawSize ? (u64)sdefl_bound((int)rawSize) : 0u;
    u64 totalSize = sizeof(TerrainEditChunkFileHeader) + compressBound;
    u8* bytes = (u8*)SDL_malloc((size_t)totalSize);
    if (!bytes) {
        SDL_free(records);
        AX_WARN("terrain chunks save failed: out of memory");
        return false;
    }

    TerrainEditChunkFileHeader* header = (TerrainEditChunkFileHeader*)bytes;
    header->magic = TERRAIN_EDIT_CHUNKS_MAGIC;
    header->version = TERRAIN_EDIT_CHUNKS_VERSION;
    header->valuesPerChunk = TERRAIN_EDIT_VALUES;
    header->chunkCount = chunkCount;
    header->rawSize = rawSize;
    header->compressedSize = 0u;

    if (rawSize) {
        static struct sdefl sdfl;
        header->compressedSize = (u64)zsdeflate(&sdfl, bytes + sizeof(*header), records, (int)rawSize, 5);
    }

    WriteAllBytes(path, (const char*)bytes, (unsigned long)(sizeof(*header) + header->compressedSize));
    SDL_free(records);
    SDL_free(bytes);
    return FileExist(path);
}

bool TerrainEdit_LoadChunks(const char* path)
{
    TerrainEdit_Init();
    TerrainEdit_Clear();
    if (!path || !path[0]) { AX_WARN("terrain chunks load skipped: empty path"); return false; }
    if (!FileExist(path)) return true;

    u64 fileSize = FileSize(path);
    char* bytes = ReadAllFileAlloc(path);
    if (!bytes) { AX_WARN("terrain chunks load failed: %s", path); return false; }

    TerrainEditChunkFileHeader* header = (TerrainEditChunkFileHeader*)bytes;
    bool ok = fileSize >= sizeof(TerrainEditChunkFileHeader);
    if (ok)
        ok = header->magic == TERRAIN_EDIT_CHUNKS_MAGIC &&
             header->version == TERRAIN_EDIT_CHUNKS_VERSION &&
             header->valuesPerChunk == TERRAIN_EDIT_VALUES;
    if (ok)
        ok = header->rawSize == (u64)header->chunkCount * sizeof(TerrainEditChunkFileRecord) &&
             fileSize == sizeof(TerrainEditChunkFileHeader) + header->compressedSize;

    if (!ok) {
        AX_WARN("terrain chunks load failed: invalid file %s", path);
        FreeAllText(bytes);
        return false;
    }

    void* records = NULL;
    if (header->rawSize)
    {
        records = SDL_malloc((size_t)header->rawSize + 16u);
        if (!records)
        {
            AX_WARN("terrain chunks load failed: out of memory");
            FreeAllText(bytes);
            return false;
        }
        size_t inflated = zsinflate(records, (size_t)header->rawSize, bytes + sizeof(*header), (size_t)header->compressedSize);
        if (inflated != header->rawSize)
        {
            AX_WARN("terrain chunks load failed: inflate failed %s", path);
            SDL_free(records);
            FreeAllText(bytes);
            return false;
        }
    }

    SDL_LockMutex(g_TerrainEdit.lock);
    for (u32 i = 0; i < header->chunkCount && ok; i++)
    {
        const TerrainEditChunkFileRecord* record = &((const TerrainEditChunkFileRecord*)records)[i];
        s32 x = record->x;
        s32 y = record->y;
        s32 z = record->z;

        TerrainEditChunk* chunk = TerrainEditGetOrCreate(x, y, z);
        ok = chunk != NULL;
        if (!ok) break;
        MemCopy(chunk->delta, record->delta, sizeof(record->delta));
        MemCopy(chunk->material, record->material, sizeof(record->material));
        int3 mn = (int3){ x * 16, y * 16, z * 16 };
		int3 mx = (int3){ x * 16 + 15, y * 16 + 15, z * 16 + 15 };
        TerrainEditGrowBounds(mn, mx);
    }
    SDL_UnlockMutex(g_TerrainEdit.lock);
    SDL_free(records);
    FreeAllText(bytes);

    if (!ok) AX_WARN("terrain chunks load failed: out of memory");
    return ok;
}
