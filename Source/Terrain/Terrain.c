// streamed transvoxel terrain. owns its chunk streaming, worker jobs, gpu buffers and
// pipelines, fully independent from the scene render sets: chunks draw as direct indexed
// calls after cpu frustum culling, lod selection is decided here (transition cells must
// match the neighbor lod, the engine's gpu lod picking cannot be used).
//
// streaming: concentric per lod boxes around the camera, snapped to the parent (coarser)
// grid so neighboring chunks never differ by more than one lod level. the finer chunk
// owns the transition cells on faces toward a coarser neighbor.
#include "Include/Terrain.h"
#include "Include/JobSystem.h"
#include "TerrainInternal.h"
#include "Source/Rendering/RenderingInternal.h"
#include "Include/Memory.h"
#include "Include/FileSystem.h"
#include "Include/Algorithm.h"
#include "Include/Random.h"
#include "Include/DataStructures/HashMap.h"

#include "Extern/stb/stb_image.h"
#include "Extern/stb/stb_image_resize2.h"

#define TERRAIN_OLD_RUNTIME_DISABLED 1

#if TERRAIN_OLD_RUNTIME_DISABLED
// port mode: the old streaming runtime stays compiled out; the editor-facing wrappers
// below carry world state here and delegate remeshing to the transvoxel unity example
#include "Source/Terrain/TransvoxelUnity.h"
static bool tvWorldEnabled;
static bool tvPortInitialized;
static bool tvGrassReady;
#endif

#if defined(PLATFORM_MACOSX)
#include "Shaders/msl/TerrainForwardVert.msl.h"
#include "Shaders/msl/TerrainForwardFrag.msl.h"
#include "Shaders/msl/TerrainDepthOnlyVert.msl.h"
#include "Shaders/msl/TerrainDepthOnlyFrag.msl.h"
#include "Shaders/msl/TerrainWireFrag.msl.h"
#include "Shaders/msl/GrassVert.msl.h"
#include "Shaders/msl/GrassFrag.msl.h"

#define Shaders_TerrainForwardVert_spv Shaders_TerrainForwardVert_msl
#define Shaders_TerrainForwardFrag_spv Shaders_TerrainForwardFrag_msl
#define Shaders_TerrainDepthOnlyVert_spv Shaders_TerrainDepthOnlyVert_msl
#define Shaders_TerrainDepthOnlyFrag_spv Shaders_TerrainDepthOnlyFrag_msl
#define Shaders_TerrainWireFrag_spv Shaders_TerrainWireFrag_msl
#define Shaders_GrassVert_spv Shaders_GrassVert_msl
#define Shaders_GrassFrag_spv Shaders_GrassFrag_msl
#elif defined(PLATFORM_WINDOWS)
#include "Shaders/spv/TerrainForwardVert.spv.h"
#include "Shaders/spv/TerrainForwardFrag.spv.h"
#include "Shaders/spv/TerrainDepthOnlyVert.spv.h"
#include "Shaders/spv/TerrainDepthOnlyFrag.spv.h"
#include "Shaders/spv/TerrainWireFrag.spv.h"
#include "Shaders/spv/GrassVert.spv.h"
#include "Shaders/spv/GrassFrag.spv.h"
#endif

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <box3d/math_functions.h>

#include "Math/Noise.h"

typedef struct Scene_ Scene;
Scene* Scene_GetActive(void);
bool Scene_PhysicsSyncTerrainChunkMesh(Scene* scene, u32 chunkSlot,
                                       const b3Vec3* vertices, u32 vertexCount,
                                       const s32* indices, u32 indexCount);
void Scene_PhysicsDestroyTerrainChunk(Scene* scene, u32 chunkSlot);

#define TERRAIN_MAX_JOBS        16u
// grass columns are scattered from the density field on worker threads; this many can be
// in flight at once, consumed (uploaded) on the main thread as they finish
#define TERRAIN_MAX_GRASS_JOBS  8u
// grass tiles are keyed by lod0 coord; sized to match the chunk pool so the origin /
// indirect buffers (TERRAIN_MAX_CHUNKS wide) can be indexed by tile slot directly
#define TERRAIN_MAX_GRASS_TILES TERRAIN_MAX_CHUNKS
#define TERRAIN_RING_RADIUS     2
#define TERRAIN_TRANSFER_BYTES  (8u * 1024u * 1024u)
#define GRASS_UPLOAD_BYTES      (1u * 1024u * 1024u) // grass staged per frame, ~30 chunks, rest retried next frame
#define TERRAIN_PENDING_CAP     4096u
#define TERRAIN_DEBUG_HOLES     0    // 1: log a streaming audit + raycast hole probe every 240 frames

// chunks up to this lod keep a cpu copy of their mesh for Terrain_Raycast. the coarse
// rings are most of the resident geometry but gameplay rays rarely need exact hits
// out there, so by default only the near lod 0/1 rings are raycastable. the debug
// hole probe compares the whole drawn set against the density field, it needs all lods
#define TERRAIN_RAYCAST_KEEP_LOD 1u
// Physics uses resident visual chunks up to this lod. Exact lod 1 would leave the
// near lod 0 child box without colliders because the streamer does not create
// overlapping lod 1 chunks there.
#define TERRAIN_PHYSICS_MAX_LOD  1u
#define TERRAIN_ALBEDO_SIZE     2048
#define TERRAIN_DETAIL_SIZE     1024                   // normal + arm layers

enum TerrainChunkState_
{
    ChunkState_Queued = 0,  // waiting for a worker
    ChunkState_Generating,  // worker running, nothing to draw yet
    ChunkState_Live,        // mesh resident (or known empty), drawn
};

enum TerrainJobState_
{
    JobState_Free = 0,
    JobState_Queued,   // submitted, waiting for a pool worker
    JobState_Running,
    JobState_Done,
};

typedef struct TerrainChunk_
{
    s32 x, y, z;
    u8  lod;
    u8  state;
    u8  transitionMask; // desired mask, bit per face -x +x -y +y -z +z
    u8  appliedMask;    // mask the resident mesh was built with
    u8  empty;          // no surface crossing, stays in the map so it is not requeried
    u8  dying;          // evicted while a worker still runs, slot freed at job consume
    u8  needRemesh;
    u8  used;
    u8  retiring;       // left the desired set but keeps drawing until replacements are live
    u8  hidden;         // live replacement waiting for the whole split/merge swap to be ready
    u8  padd0;
    u8  padd1;
	u32 gen;            // desired set generation, stale chunks get evicted
    s32 jobSlot;        // -1 when no worker owns this chunk
    u32 vertexOffset, numVertices;
    u32 indexOffset , numIndices;
    float3 aabbMin, aabbMax; // world space, tight bounds from the mesher
    void* vertexHeapPtr;
    void* indexHeapPtr;
} TerrainChunk;

// grass lives in world-anchored tiles keyed by lod0 chunk coord, decoupled from the
// TerrainChunk lifecycle: a tile is built once from the lod0 mesh and keeps drawing even
// after the terrain under it merges to a coarser lod, until it drifts out of view distance.
// one 16 m surface column of grass, generated straight from the density field (no mesh
// needed), so it loads at any distance regardless of terrain lod. keyed by lod0 column.
typedef struct GrassTile_
{
    s32 cx, cz;          // lod0 column coord (16 m grid); world origin = coord * 16
    u8  used;
    u8  dirty;           // density under it changed (edit) / freshly created; (re)build
    u8  building;        // a worker job is scattering this column right now
    u8  dying;           // reclaimed mid-build; slot released when the job is consumed
    u32 grassStart, grassCount;
    void* grassHeapPtr;
    float3 aabbMin, aabbMax; // grass world bounds, for distance + frustum cull
} GrassTile;

// one in-flight grass column scatter, run on a JobSystem worker. the CPU-heavy density
// marching (TerrainBuildTileGrass) writes into this job's own buffer off the main thread;
// the main thread only allocates the heap range and uploads once the job is done.
typedef struct GrassJob_
{
    SDL_AtomicInt  state;    // JobState_Free / _Running / _Done
    s32 cx, cz;              // column being built
    u32 tileSlot;            // grass tile that dispatched this job
    u32 count;               // instances the worker produced
    float3 aabbMin, aabbMax; // grass world bounds the worker computed
    GrassInstance* out;      // GRASS_PER_CHUNK scratch, allocated once per slot, reused
} GrassJob;

typedef struct TerrainJob_
{
    SDL_AtomicInt state;  // 0 free, 1 running, 2 done
    SDL_AtomicInt cancel;
    s32 x, y, z;
    u32 lod;
    u8  transitionMask;
    u8  empty;
    s32 result;
    u32 chunkSlot;
    s8* density;                 // allocated once per slot, reused
    TerrainMeshScratch scratch;  // allocated once per slot, reused
    TerrainMeshOut mesh;         // capacity persists across jobs
} TerrainJob;

typedef struct TerrainBox_ 
{
	s32 min[3];
	s32 max[3];
} TerrainBox; // inclusive chunk coords

typedef struct TerrainState_
{
    bool initialized;
    bool enabled;

    TerrainChunk* chunks;     // tlsf, TERRAIN_MAX_CHUNKS
    u32*          freeSlots;
    u32           numFreeSlots;
    HashMap       chunkMap;   // packed (x,y,z,lod) -> chunk slot

    u32* pending;             // chunk slots waiting for a worker, entries can go stale
    u32  numPending;

    GrassTile* grassTiles;    // TERRAIN_MAX_GRASS_TILES, world-anchored, density-generated
    u32*       grassTileFree;
    u32        numGrassTileFree;
    HashMap    grassTileMap;  // packed (cx,0,cz,0) -> grass tile slot
    GrassJob   grassJobs[TERRAIN_MAX_GRASS_JOBS]; // column scatter run on the shared pool
    s32        grassCamCol[2]; // camera lod0 column last frame, to detect ring moves
    f32        grassLastViewDist;
    bool       grassScanDirty; // ring needs a (re)scan: camera moved / view dist / edit / truncated

    TerrainJob jobs[TERRAIN_MAX_JOBS];

    u32 numAllocatedVertices;
    u32 numAllocatedIndices;

    u32 gen;
    s32 lastCamChunk[3];
    bool desiredValid;
    TerrainBox levelBox[TERRAIN_LOD_COUNT];
    FrustumPlanes cameraFrustum; // updated every frame for load prioritization
    bool frustumValid;
    f32 lodFactor;               // snapshot of g_RenderSettings.terrainLodFactor

    TerrainGenParams genParams;
    TerrainAuthoring authoring;  // name/paint-layers/grass, persisted in the .terrain file
    bool fixedCenterValid;       // fixedArea captures the camera once, then never moves
    f32  fixedCenter[2];         // world x/z the rings stay centered on

    // editor brush cursor, pushed to the fragment shader for the surface highlight
    float3 brushPos;
    f32   brushRadius;            // 0 while inactive

    JobSystem* jobSystem;

    u32 numDrawable;          // live chunks with indices
    u32 drawnLastFrame;

    SDL_GPUBuffer*           vertexBuffer;
    SDL_GPUBuffer*           indexBuffer;
    SDL_GPUBuffer*           grassBuffer;        // GrassInstance heap mirror, bound as instance vertex buffer
    SDL_GPUBuffer*           grassChunkBuffer;   // GrassChunkInfo per grass tile slot, read as a storage buffer
    SDL_GPUBuffer*           grassIndirectBuffer;// SDL_GPUIndirectDrawCommand per visible grass tile, rebuilt each frame
    SDL_GPUTransferBuffer*   transferBuffer;
    SDL_GPUTransferBuffer*   grassUploadTransfer; // stages grass instances + chunk origins, drained per frame
    SDL_GPUTransferBuffer*   grassIndirectTransfer;
    u32                      grassDrawCount;     // number of indirect commands staged this frame
    SDL_GPUGraphicsPipeline* forwardPipeline;
    SDL_GPUGraphicsPipeline* depthPipeline;
    SDL_GPUGraphicsPipeline* wirePipeline;
    SDL_GPUGraphicsPipeline* grassPipeline;
    Texture albedoLayers;
    Texture normalLayers;
    Texture armLayers;
    Texture grassLayers;     // 2-layer array: Grass0 / Grass1 blade atlases
} TerrainState;

static TerrainState g_Terrain;

static void TerrainChunkSyncPhysics(u32 slot);
static void TerrainChunkDestroyPhysics(u32 slot);
static u32  TerrainBuildTileGrass(u32 tileSlot, s32 cx, s32 cz, GrassInstance* out, float3* outMin, float3* outMax);
static GrassTile* TerrainGrassTileFind(s32 cx, s32 cz);

// matches the vs_params cbuffer in TerrainForward.hlsl / TerrainDepthOnly.hlsl
typedef struct TerrainVSParams_
{
    mat4x4 viewProj;
    f32 chunkOriginSize[4];
    f32 cameraPosition[4];
    f32 cameraForward[4];
} TerrainVSParams;

typedef struct TerrainForwardFragmentParams_
{
    f32 sunDirection[4];
    f32 brushPosRadius[4];
    f32 cameraPosition[4];
    u32 outputSize[2];
    u32 pad0[2];
} TerrainForwardFragmentParams;

// matches vs_params in Grass.hlsl. view/proj are kept separate (not premultiplied)
// so the vertex shader can build the camera-facing billboard in view space. one push
// covers the whole indirect multidraw; per-chunk origin comes from the chunks buffer.
typedef struct GrassVSParams_
{
    mat4x4 view;
    mat4x4 proj;
    f32 cameraTime[4];   // xyz: camera world pos, w: time in seconds (wind phase)
    f32 grassParams[4];  // x: view distance (meters)
} GrassVSParams;

// one float4 per terrain chunk slot: world origin of the chunk in meters (w unused).
// the grass vertex shader adds it to the fp16 chunk-relative instance position.
typedef struct GrassChunkInfo_ { f32 origin[4]; } GrassChunkInfo;

// matches ps_params in Grass.hlsl
typedef struct GrassFSParams_
{
    f32 sunDirection[4];    // xyz sun direction, w unused
    f32 grassColor[4];      // rgb tint, a unused
    f32 grassColorVariant[4];
} GrassFSParams;

static f32 TerrainChunkWorldSize(u32 lod)
{
    return (f32)TERRAIN_CHUNK_CELLS * TERRAIN_VOXEL_SIZE * (f32)(1u << lod);
}

static u64 TerrainChunkKey(s32 x, s32 y, s32 z, u32 lod)
{
    return ((u64)((u32)(x + 0x100000) & 0x1FFFFFu) << 40)
         | ((u64)((u32)(y + 0x8000)   & 0xFFFFu)   << 24)
         | ((u64)((u32)(z + 0x100000) & 0x1FFFFFu) << 3)
         | (u64)lod;
}

#define TERRAIN_HEAP_FAIL GEOMETRY_ALLOC_FAIL

// ---------------------------------------------------------------------------------
// chunk pool
// ---------------------------------------------------------------------------------

static u32 TerrainChunkAlloc(void)
{
    if (g_Terrain.numFreeSlots == 0u) return TERRAIN_HEAP_FAIL;
    return g_Terrain.freeSlots[--g_Terrain.numFreeSlots];
}

static void TerrainChunkRelease(u32 slot)
{
    g_Terrain.chunks[slot].used = 0;
    g_Terrain.freeSlots[g_Terrain.numFreeSlots++] = slot;
}

static void TerrainChunkFreeMeshEx(u32 slot, bool destroyPhysics)
{
    TerrainChunk* chunk = &g_Terrain.chunks[slot];
    if (destroyPhysics) TerrainChunkDestroyPhysics(slot);
    if (chunk->vertexHeapPtr) GeometryHeapFree(GeometryBuffer_TerrainVertex, chunk->vertexHeapPtr);
    if (chunk->indexHeapPtr)  GeometryHeapFree(GeometryBuffer_TerrainIndex, chunk->indexHeapPtr);
    if (chunk->numVertices) g_Terrain.numAllocatedVertices -= chunk->numVertices;
    if (chunk->numIndices)  g_Terrain.numAllocatedIndices  -= chunk->numIndices;
    if (chunk->numIndices && g_Terrain.numDrawable) g_Terrain.numDrawable--;
    chunk->numVertices = chunk->numIndices = 0u;
    chunk->vertexHeapPtr = NULL;
    chunk->indexHeapPtr = NULL;
    // grass is owned by GrassTiles (world-anchored), not by the chunk, so it survives here
}

static void TerrainChunkFreeMesh(u32 slot)
{
    TerrainChunkFreeMeshEx(slot, true);
}

static void TerrainPendingPush(u32 slot)
{
    if (g_Terrain.numPending < TERRAIN_PENDING_CAP)
        g_Terrain.pending[g_Terrain.numPending++] = slot;
    else // a dropped chunk would never mesh, the desired walk has no re-push for queued chunks
        AX_WARN("terrain pending queue full, chunk (%d,%d,%d) lod %u stuck",
                g_Terrain.chunks[slot].x, g_Terrain.chunks[slot].y,
                g_Terrain.chunks[slot].z, g_Terrain.chunks[slot].lod);
}

static void TerrainChunkEvict(u32 slot)
{
    TerrainChunk* chunk = &g_Terrain.chunks[slot];
    HMErase(&g_Terrain.chunkMap, TerrainChunkKey(chunk->x, chunk->y, chunk->z, chunk->lod));
    TerrainChunkFreeMesh(slot);
    if (chunk->jobSlot >= 0)
    {
        SDL_SetAtomicInt(&g_Terrain.jobs[chunk->jobSlot].cancel, 1);
        chunk->dying = 1; // slot is released when the job is consumed
        return;
    }
    TerrainChunkRelease(slot);
}

// finds a chunk slot in the map, out: chunk pointer or NULL
static TerrainChunk* TerrainFindChunk(s32 x, s32 y, s32 z, u32 lod)
{
    u32* found = (u32*)HMFind(&g_Terrain.chunkMap, TerrainChunkKey(x, y, z, lod));
    return found ? &g_Terrain.chunks[*found] : NULL;
}

// lod changes are not always one level: at ring corners (or under fast movement) a
// retiring chunk's volume gets replaced by a mix of levels, so coverage checks have
// to walk the whole hierarchy instead of only the direct parent/children

// any retiring chunk inside this volume? checked downward through all finer levels
static bool TerrainAnyRetiringDescendant(s32 x, s32 y, s32 z, u32 lod)
{
    if (lod == 0) return false;
    for (u32 i = 0; i < 8u; i++)
    {
        s32 cx = x * 2 + (s32)(i & 1u);
        s32 cy = y * 2 + (s32)((i >> 1) & 1u);
        s32 cz = z * 2 + (s32)((i >> 2) & 1u);
        TerrainChunk* chunk = TerrainFindChunk(cx, cy, cz, lod - 1u);
        if (chunk && chunk->used && chunk->retiring) return true;
        if (TerrainAnyRetiringDescendant(cx, cy, cz, lod - 1u)) return true;
    }
    return false;
}

// true while any retiring chunk still covers this chunk's volume, the chunk stays
// hidden until the whole swap group can switch in the same frame
static bool TerrainCoveredByRetiring(const TerrainChunk* chunk)
{
    s32 px = chunk->x, py = chunk->y, pz = chunk->z;
    for (u32 l = chunk->lod + 1u; l < TERRAIN_LOD_COUNT; l++)
    {
        px >>= 1; py >>= 1; pz >>= 1;
        TerrainChunk* ancestor = TerrainFindChunk(px, py, pz, l);
        if (ancestor && ancestor->used && ancestor->retiring) return true;
    }
    return TerrainAnyRetiringDescendant(chunk->x, chunk->y, chunk->z, chunk->lod);
}

// is this volume fully covered by live (non retiring) chunks at any level? a missing
// octant whose location has no chunk at any level is abandoned space and counts covered
static bool TerrainRegionReady(s32 x, s32 y, s32 z, u32 lod)
{
    TerrainChunk* chunk = TerrainFindChunk(x, y, z, lod);
    if (chunk && chunk->used && !chunk->retiring)
        return chunk->state == ChunkState_Live;

    s32 px = x, py = y, pz = z;
    for (u32 l = lod + 1u; l < TERRAIN_LOD_COUNT; l++)
    {
        px >>= 1; py >>= 1; pz >>= 1;
        TerrainChunk* ancestor = TerrainFindChunk(px, py, pz, l);
        if (ancestor && ancestor->used && !ancestor->retiring)
            return ancestor->state == ChunkState_Live;
    }
    if (lod == 0) return true; // no chunk wants this volume on any level

    for (u32 i = 0; i < 8u; i++)
    {
        if (!TerrainRegionReady(x * 2 + (s32)(i & 1u),
                                y * 2 + (s32)((i >> 1) & 1u),
                                z * 2 + (s32)((i >> 2) & 1u), lod - 1u))
            return false;
    }
    return true;
}

// a retiring chunk may be removed once its whole volume is covered again
static bool TerrainReplacementsReady(const TerrainChunk* chunk)
{
    return TerrainRegionReady(chunk->x, chunk->y, chunk->z, chunk->lod);
}

// ---------------------------------------------------------------------------------
// desired set: per lod boxes snapped to the parent grid
// ---------------------------------------------------------------------------------

static bool TerrainBoxContains(const TerrainBox* box, s32 x, s32 y, s32 z)
{
    return x >= box->min[0] && x <= box->max[0]
        && y >= box->min[1] && y <= box->max[1]
        && z >= box->min[2] && z <= box->max[2];
}

// child region of level L in level L units (the snapping makes the division exact)
static TerrainBox TerrainChildBox(u32 level)
{
    const TerrainBox* child = &g_Terrain.levelBox[level - 1u];
    TerrainBox box;
    for (s32 a = 0; a < 3; a++)
    {
        box.min[a] = child->min[a] >> 1;
        box.max[a] = child->max[a] >> 1;
    }
    return box;
}

static u8 TerrainComputeTransitionMask(u32 level, s32 x, s32 y, s32 z)
{
    if (level + 1u >= TERRAIN_LOD_COUNT) return 0;
    static const s32 faceDir[6][3] = {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };
    const TerrainBox* box = &g_Terrain.levelBox[level];
    u8 mask = 0;
    for (s32 f = 0; f < 6; f++)
    {
        if (!TerrainBoxContains(box, x + faceDir[f][0], y + faceDir[f][1], z + faceDir[f][2]))
            mask |= (u8)(1 << f);
    }
    return mask;
}

static void TerrainDesireChunk(u32 level, s32 x, s32 y, s32 z)
{
    u64 key = TerrainChunkKey(x, y, z, level);
    u8 mask = TerrainComputeTransitionMask(level, x, y, z);

    u32* found = (u32*)HMFind(&g_Terrain.chunkMap, key);
    if (found)
    {
        TerrainChunk* chunk = &g_Terrain.chunks[*found];
        chunk->gen = g_Terrain.gen;
        chunk->retiring = 0; // came back into the desired set, keep the resident mesh
        if (chunk->transitionMask != mask)
        {
            chunk->transitionMask = mask;
            // empty chunks have no geometry, the mask only matters for meshes
            if (chunk->state == ChunkState_Live && !chunk->empty && !chunk->needRemesh && chunk->jobSlot < 0)
            {
                chunk->needRemesh = 1;
                TerrainPendingPush(*found);
            }
        }
        return;
    }

    u32 slot = TerrainChunkAlloc();
    if (slot == TERRAIN_HEAP_FAIL) { AX_WARN("terrain chunk pool exhausted"); return; }
    TerrainChunk* chunk = &g_Terrain.chunks[slot];
    SDL_memset(chunk, 0, sizeof(*chunk));
    chunk->x = x; chunk->y = y; chunk->z = z;
    chunk->lod = (u8)level;
    chunk->transitionMask = mask;
    chunk->state = ChunkState_Queued;
    chunk->jobSlot = -1;
    chunk->gen = g_Terrain.gen;
    chunk->used = 1;
    HMInsert(&g_Terrain.chunkMap, key, &slot);
    TerrainPendingPush(slot);
}

static void TerrainComputeDesired(const Camera* camera)
{
    g_Terrain.gen++;

    f32 bandMin, bandMax;
    TerrainDensity_GetYRange(&bandMin, &bandMax);

    // fixed area worlds keep their rings where they were created instead of
    // following the camera
    f32 centerX = camera->position.x, centerZ = camera->position.z;
    if (g_Terrain.genParams.fixedArea && g_Terrain.fixedCenterValid)
    {
        centerX = g_Terrain.fixedCenter[0];
        centerZ = g_Terrain.fixedCenter[1];
    }

    // the editor lod factor scales how far every detail ring reaches, the pool warns
    // and degrades gracefully if a huge factor exhausts the chunk budget
    s32 radius = Clamps32((s32)((f32)TERRAIN_RING_RADIUS * g_Terrain.lodFactor + 0.5f), 1, 4);
    for (u32 level = 0; level < TERRAIN_LOD_COUNT; level++)
    {
        f32 size = TerrainChunkWorldSize(level);
        s32 camX = (s32)Floorf32(centerX / size);
        s32 camZ = (s32)Floorf32(centerZ / size);
        TerrainBox* box = &g_Terrain.levelBox[level];
        box->min[0] = (camX - radius) & ~1;
        box->max[0] = (camX + radius) | 1;
        box->min[2] = (camZ - radius) & ~1;
        box->max[2] = (camZ + radius) | 1;
        box->min[1] = ((s32)Floorf32(bandMin / size)) & ~1;
        box->max[1] = ((s32)Floorf32(bandMax / size)) | 1;
    }

    for (u32 level = 0; level < TERRAIN_LOD_COUNT; level++)
    {
        const TerrainBox* box = &g_Terrain.levelBox[level];
        TerrainBox child = { 0 };
        if (level > 0) child = TerrainChildBox(level);
        for (s32 z = box->min[2]; z <= box->max[2]; z++)
        for (s32 y = box->min[1]; y <= box->max[1]; y++)
        for (s32 x = box->min[0]; x <= box->max[0]; x++)
        {
            if (level > 0 && TerrainBoxContains(&child, x, y, z)) continue;
            TerrainDesireChunk(level, x, y, z);
        }
    }

    // chunks that fell out of the desired set: visible geometry keeps drawing as
    // retiring until its replacement lod coverage is fully live (no holes/pops),
    // everything invisible goes away immediately
    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
    {
        TerrainChunk* chunk = &g_Terrain.chunks[i];
        if (!chunk->used || chunk->dying || chunk->gen == g_Terrain.gen) continue;
        if (chunk->state == ChunkState_Live && !chunk->hidden && chunk->numIndices > 0u)
            chunk->retiring = 1;
        else
            TerrainChunkEvict(i);
    }
}

// resolves pending split/merge swaps: retiring chunks leave the moment their
// replacements are all live, and the replacements unhide in the same frame
static void TerrainResolveRetiring(void)
{
    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
    {
        TerrainChunk* chunk = &g_Terrain.chunks[i];
        if (!chunk->used || !chunk->retiring || chunk->dying) continue;
        if (TerrainReplacementsReady(chunk))
            TerrainChunkEvict(i);
    }
    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
    {
        TerrainChunk* chunk = &g_Terrain.chunks[i];
        if (!chunk->used || !chunk->hidden) continue;
        if (!TerrainCoveredByRetiring(chunk))
        {
            chunk->hidden = 0;
            TerrainChunkSyncPhysics(i);
        }
    }
}

// ---------------------------------------------------------------------------------
// worker jobs
// ---------------------------------------------------------------------------------

static void TerrainJobRun(TerrainJob* job)
{
    job->result = 1;
    job->empty = 0;

    if (SDL_GetAtomicInt(&job->cancel))
    {
        SDL_SetAtomicInt(&job->state, JobState_Done);
        return;
    }

    TerrainDensity_SampleChunk(job->x, job->y, job->z, job->lod, job->density);

    if (!SDL_GetAtomicInt(&job->cancel))
    {
        bool firstInside = job->density[0] < 0;
        bool mixed = false;
        for (s32 i = 1; i < TERRAIN_SAMPLES_TOTAL; i++)
        {
            if ((job->density[i] < 0) != firstInside) { mixed = true; break; }
        }
        if (!mixed)
            job->empty = 1;
        else
            job->result = Transvoxel_MeshChunk(job->density, job->lod, job->transitionMask,
                                               job->x, job->y, job->z,
                                               &job->mesh, &job->scratch);
    }

    SDL_SetAtomicInt(&job->state, JobState_Done);
}

static void TerrainJobRunTask(void* data)
{
    TerrainJobRun((TerrainJob*)data);
}

static void TerrainDispatchJobs(const Camera* camera)
{
    for (u32 slot = 0; slot < TERRAIN_MAX_JOBS; slot++)
    {
        TerrainJob* job = &g_Terrain.jobs[slot];
        if (SDL_GetAtomicInt(&job->state) != JobState_Free) continue;

        // pick the best pending chunk: nearest first, chunks inside the view frustum
        // are strongly preferred so visible holes fill before anything offscreen
        u32 bestIdx = TERRAIN_HEAP_FAIL;
        f32 bestScore = 0.0f;
        u32 i = 0;
        while (i < g_Terrain.numPending)
        {
            u32 chunkSlot = g_Terrain.pending[i];
            TerrainChunk* chunk = &g_Terrain.chunks[chunkSlot];
            bool wantsJob = chunk->used && !chunk->dying && !chunk->retiring && chunk->jobSlot < 0 &&
                            (chunk->state == ChunkState_Queued || chunk->needRemesh);
            if (!wantsJob)
            {
                g_Terrain.pending[i] = g_Terrain.pending[--g_Terrain.numPending];
                continue;
            }
            f32 size = TerrainChunkWorldSize(chunk->lod);
            f32 ox = (f32)chunk->x * size, oy = (f32)chunk->y * size, oz = (f32)chunk->z * size;
            f32 dx = ox + size * 0.5f - camera->position.x;
            f32 dy = oy + size * 0.5f - camera->position.y;
            f32 dz = oz + size * 0.5f - camera->position.z;
            f32 score = dx * dx + dy * dy + dz * dz;
            if (g_Terrain.frustumValid)
            {
                v128f aabbMin = VecSetR(ox, oy, oz, 0.0f);
                v128f aabbMax = VecSetR(ox + size, oy + size, oz + size, 0.0f);
                // CheckAABBCulled returns true when the box intersects the frustum
                if (!CheckAABBCulled(aabbMin, aabbMax, g_Terrain.cameraFrustum.planes))
                    score *= 16.0f; // offscreen, fill visible terrain first
            }
            if (bestIdx == TERRAIN_HEAP_FAIL || score < bestScore) { bestIdx = i; bestScore = score; }
            i++;
        }
        if (bestIdx == TERRAIN_HEAP_FAIL) return;

        u32 chunkSlot = g_Terrain.pending[bestIdx];
        g_Terrain.pending[bestIdx] = g_Terrain.pending[--g_Terrain.numPending];
        TerrainChunk* chunk = &g_Terrain.chunks[chunkSlot];

        job->x = chunk->x; job->y = chunk->y; job->z = chunk->z;
        job->lod = chunk->lod;
        job->transitionMask = chunk->transitionMask;
        job->chunkSlot = chunkSlot;
        SDL_SetAtomicInt(&job->cancel, 0);

        chunk->needRemesh = 0;
        chunk->jobSlot = (s32)slot;
        if (chunk->state == ChunkState_Queued) chunk->state = ChunkState_Generating;

        SDL_SetAtomicInt(&job->state, JobState_Running);
        if (JobSystem_Execute(g_Terrain.jobSystem, TerrainJobRunTask, job) == 0)
        {
            AX_WARN("terrain job dispatch failed, chunk requeued");
            chunk->jobSlot = -1;
            chunk->state = ChunkState_Queued;
            chunk->needRemesh = 0;
            TerrainPendingPush(chunkSlot);
            SDL_SetAtomicInt(&job->state, JobState_Free);
            return;
        }
    }
}

// ---------------------------------------------------------------------------------
// upload: consumes finished jobs, called from Render() before any render pass
// ---------------------------------------------------------------------------------

typedef struct TerrainCopyRegion_
{
    SDL_GPUBuffer* dst;
    u32 transferOffset;
    u32 dstOffset;
    u32 size;
} TerrainCopyRegion;

static bool ShouldSkipGenerate(u32 slot, TerrainChunk* chunk, TerrainJob* job)
{
	bool owns = chunk->used && chunk->jobSlot == (s32)slot;
	if (!owns)
	{
		SDL_SetAtomicInt(&job->state, JobState_Free);
		return true;
	}
	if (chunk->dying)
	{
		chunk->jobSlot = -1;
		TerrainChunkRelease(job->chunkSlot);
		SDL_SetAtomicInt(&job->state, JobState_Free);
		return true;
	}
	if (SDL_GetAtomicInt(&job->cancel) || job->result == 0)
	{
		// cancelled but still desired, or mesher out of memory: try again
		chunk->jobSlot = -1;
		chunk->state = ChunkState_Queued;
		TerrainPendingPush(job->chunkSlot);
		SDL_SetAtomicInt(&job->state, JobState_Free);
		return true;
	}

	if (job->empty || job->mesh.numVertices == 0u || job->mesh.numIndices == 0u)
	{
		chunk->jobSlot = -1;
		TerrainChunkFreeMesh(job->chunkSlot);
		chunk->empty = 1;
		chunk->hidden = 0;
		chunk->appliedMask = job->transitionMask;
		chunk->state = ChunkState_Live;
		SDL_SetAtomicInt(&job->state, JobState_Free);
		return true;
	}
	return false;
}

// ---- density-driven grass tiles: one 16 m surface column, generated from the SDF ----

static GrassTile* TerrainGrassTileFind(s32 cx, s32 cz)
{
    u32* found = (u32*)HMFind(&g_Terrain.grassTileMap, TerrainChunkKey(cx, 0, cz, 0u));
    return found ? &g_Terrain.grassTiles[*found] : NULL;
}

static void TerrainGrassTileFree(u32 slot)
{
    GrassTile* t = &g_Terrain.grassTiles[slot];
    if (!t->used) return;
    // a worker still owns this column: keep the slot reserved and release it when the job
    // is consumed, otherwise the slot could be reused while the worker writes stale data
    if (t->building) { t->dying = 1u; return; }
    if (t->grassHeapPtr) GeometryHeapFree(GeometryBuffer_GrassInstance, t->grassHeapPtr);
    HMErase(&g_Terrain.grassTileMap, TerrainChunkKey(t->cx, 0, t->cz, 0u));
    t->used = 0u; t->dirty = 0u; t->dying = 0u; t->grassHeapPtr = NULL;
    t->grassStart = t->grassCount = 0u;
    g_Terrain.grassTileFree[g_Terrain.numGrassTileFree++] = slot;
}

// horizontal distance from the camera to a column's nearest point (center minus half the
// 16 m diagonal), the metric both the tile ring build and the reclaim use
static f32 TerrainGrassColumnDist(float3 cam, s32 cx, s32 cz, f32 size0)
{
    f32 ccx = ((f32)cx + 0.5f) * size0, ccz = ((f32)cz + 0.5f) * size0;
    f32 dx = cam.x - ccx, dz = cam.z - ccz;
    return Sqrtf(dx * dx + dz * dz) - size0 * 0.70710678f;
}

// worker body: scatter one column off the main thread. TerrainBuildTileGrass only reads
// the (thread safe) density field and gen params, writing into the job's own buffer, so
// this is safe to run on the shared pool alongside the mesher.
static void TerrainGrassJobRunTask(void* data)
{
    GrassJob* job = (GrassJob*)data;
    job->count = TerrainBuildTileGrass(job->tileSlot, job->cx, job->cz,
                                       job->out, &job->aabbMin, &job->aabbMax);
    SDL_SetAtomicInt(&job->state, JobState_Done);
}

static GrassJob* TerrainGrassJobAcquire(void)
{
    for (u32 i = 0; i < TERRAIN_MAX_GRASS_JOBS; i++)
        if (SDL_GetAtomicInt(&g_Terrain.grassJobs[i].state) == JobState_Free)
            return &g_Terrain.grassJobs[i];
    return NULL;
}

// per frame, main thread: reclaim columns that left the view distance, then dispatch scatter
// jobs for columns inside it, nearest first, bounded by the free worker slots. the actual
// density marching runs on the pool; results are picked up by TerrainGrassConsume.
static void TerrainGrassDispatch(void)
{
    f32 viewDist = g_Terrain.authoring.grassViewDistance;
    f32 size0 = TerrainChunkWorldSize(0);
    float3 cam = g_Camera.position;

    // 1) reclaim columns out of range (or every column, when grass is disabled). building
    // tiles are marked dying and released by the consume step when their job returns.
    f32 keepDist = viewDist > 0.0f ? viewDist * 1.15f : -1.0f; // hysteresis so edge tiles don't thrash
    for (u32 i = 0; i < TERRAIN_MAX_GRASS_TILES; i++)
    {
        GrassTile* t = &g_Terrain.grassTiles[i];
        if (!t->used || t->dying) continue;
        if (keepDist < 0.0f || TerrainGrassColumnDist(cam, t->cx, t->cz, size0) > keepDist)
        {
            TerrainGrassTileFree(i);
            g_Terrain.grassScanDirty = true; // a slot freed up, the ring may build there
        }
    }
    if (viewDist <= 0.0f) return;

    // only rescan the ring when something changed (camera column moved, view distance edited,
    // a tile was dirtied, or the last scan was truncated); otherwise the columns are all built
    s32 camCx = (s32)Floorf32(cam.x / size0), camCz = (s32)Floorf32(cam.z / size0);
    if (camCx != g_Terrain.grassCamCol[0] || camCz != g_Terrain.grassCamCol[1] ||
        viewDist != g_Terrain.grassLastViewDist)
    {
        g_Terrain.grassCamCol[0] = camCx; g_Terrain.grassCamCol[1] = camCz;
        g_Terrain.grassLastViewDist = viewDist;
        g_Terrain.grassScanDirty = true;
    }
    if (!g_Terrain.grassScanDirty) return;

    // 2) dispatch columns inside the view distance, nearest-first (expanding Chebyshev rings)
    s32 R = (s32)Ceilf(viewDist / size0);
    if (R > 64) R = 64; // bound the scan / pool span for extreme view distances
    bool full = false;  // ran out of worker slots or the tile pool: resume next frame
    for (s32 r = 0; r <= R && !full; r++)
    for (s32 dz = -r; dz <= r && !full; dz++)
    for (s32 dx = -r; dx <= r; dx++)
    {
        s32 adx = Absi32(dx), adz = Absi32(dz);
        if (r != 0 && adx != r && adz != r) continue; // only this ring's border cells
        s32 cx = camCx + dx, cz = camCz + dz;
        if (TerrainGrassColumnDist(cam, cx, cz, size0) > viewDist) continue;

        GrassTile* tile = TerrainGrassTileFind(cx, cz);
        if (tile && (tile->building || !tile->dirty)) continue; // in flight or already built

        GrassJob* job = TerrainGrassJobAcquire();
        if (!job) { full = true; break; } // all workers busy, resume next frame

        u32 tileSlot;
        if (!tile)
        {
            if (g_Terrain.numGrassTileFree == 0u) { full = true; break; } // pool full
            tileSlot = g_Terrain.grassTileFree[--g_Terrain.numGrassTileFree];
            tile = &g_Terrain.grassTiles[tileSlot];
            *tile = (GrassTile){ .used = 1u, .dirty = 1u, .cx = cx, .cz = cz };
            HMInsert(&g_Terrain.grassTileMap, TerrainChunkKey(cx, 0, cz, 0u), &tileSlot);
        }
        else tileSlot = (u32)(tile - g_Terrain.grassTiles);

        tile->building = 1u;
        tile->dirty = 0u; // an edit landing mid-build re-sets this, triggering a rebuild
        job->cx = cx; job->cz = cz; job->tileSlot = tileSlot;
        SDL_SetAtomicInt(&job->state, JobState_Running);
        if (JobSystem_Execute(g_Terrain.jobSystem, TerrainGrassJobRunTask, job) == 0)
        {
            tile->building = 0u; tile->dirty = 1u; // pool queue full, retry next frame
            SDL_SetAtomicInt(&job->state, JobState_Free);
            full = true; break;
        }
    }
    // keep scanning next frames until every in-range column has a tile that is built or in
    // flight; once steady state is reached this drops to false and the ring scan stops
    g_Terrain.grassScanDirty = full;
}

// per frame, main thread: pick up finished scatter jobs, allocate their heap range and
// upload it. bounded by the per-frame staging budget; anything that does not fit stays Done
// and is retried next frame.
static void TerrainGrassConsume(SDL_GPUCommandBuffer* cmd)
{
    f32 size0 = TerrainChunkWorldSize(0);
    TerrainCopyRegion regions[TERRAIN_MAX_GRASS_JOBS * 2u]; // grass range + chunk origin each
    u32 numRegions = 0u, cursor = 0u;
    u8* mapped = NULL;

    for (u32 s = 0; s < TERRAIN_MAX_GRASS_JOBS; s++)
    {
        GrassJob* job = &g_Terrain.grassJobs[s];
        if (SDL_GetAtomicInt(&job->state) != JobState_Done) continue;

        GrassTile* tile = &g_Terrain.grassTiles[job->tileSlot];
        // the tile may have been reclaimed or reused for another column while the job ran;
        // only accept a result that still matches the column we dispatched
        bool matches = tile->used && tile->building && tile->cx == job->cx && tile->cz == job->cz;
        if (!matches)
        {
            SDL_SetAtomicInt(&job->state, JobState_Free);
            continue;
        }
        if (tile->dying) // column left the view distance mid-build: drop the result, free it
        {
            tile->building = 0u;
            TerrainGrassTileFree(job->tileSlot);
            SDL_SetAtomicInt(&job->state, JobState_Free);
            continue;
        }

        // empty column (all steep / underwater): drop any previous range, keep the tile live
        if (job->count == 0u)
        {
            if (tile->grassHeapPtr) GeometryHeapFree(GeometryBuffer_GrassInstance, tile->grassHeapPtr);
            tile->grassHeapPtr = NULL; tile->grassStart = tile->grassCount = 0u;
            tile->aabbMin = job->aabbMin; tile->aabbMax = job->aabbMax;
            tile->building = 0u;
            SDL_SetAtomicInt(&job->state, JobState_Free);
            continue;
        }

        u32 grassBytes = job->count * (u32)sizeof(GrassInstance);
        if (cursor + grassBytes + sizeof(GrassChunkInfo) > GRASS_UPLOAD_BYTES)
            break; // out of staging budget, leave the job Done and retry next frame

        void* grassRaw = NULL;
        u32 grassOffset = GeometryHeapAlloc(GeometryBuffer_GrassInstance, job->count, &grassRaw);
        if (grassOffset == TERRAIN_HEAP_FAIL) break; // heap full, retry next frame

        if (!mapped)
        {
            mapped = (u8*)SDL_MapGPUTransferBuffer(g_GPUDevice, g_Terrain.grassUploadTransfer, true);
            if (!mapped) { GeometryHeapFree(GeometryBuffer_GrassInstance, grassRaw); break; }
        }

        GrassInstance* cpuGrass = (GrassInstance*)gGFX.TerrainGrassBuffer + grassOffset;
        MemCopy(cpuGrass, job->out, grassBytes);
        MemCopy(mapped + cursor, cpuGrass, grassBytes);
        regions[numRegions++] = (TerrainCopyRegion){
            g_Terrain.grassBuffer, cursor, grassOffset * (u32)sizeof(GrassInstance), grassBytes };
        cursor += grassBytes;

        // origin.y is 0: the instance stores absolute world Y (fp16 keeps ~cm precision here)
        GrassChunkInfo info = { { (f32)job->cx * size0, 0.0f, (f32)job->cz * size0, 0.0f } };
        MemCopy(mapped + cursor, &info, sizeof(info));
        regions[numRegions++] = (TerrainCopyRegion){
            g_Terrain.grassChunkBuffer, cursor, job->tileSlot * (u32)sizeof(GrassChunkInfo), (u32)sizeof(info) };
        cursor += (u32)sizeof(info);

        if (tile->grassHeapPtr) // rebuild: release the old range now the new one is staged
            GeometryHeapFree(GeometryBuffer_GrassInstance, tile->grassHeapPtr);
        tile->grassHeapPtr = grassRaw;
        tile->grassStart   = grassOffset;
        tile->grassCount   = job->count;
        tile->aabbMin = job->aabbMin; tile->aabbMax = job->aabbMax;
        tile->building = 0u;
        SDL_SetAtomicInt(&job->state, JobState_Free);
    }

    if (!mapped) return;
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, g_Terrain.grassUploadTransfer);
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
    for (u32 i = 0; i < numRegions; i++)
    {
        SDL_GPUTransferBufferLocation src = { g_Terrain.grassUploadTransfer, regions[i].transferOffset };
        SDL_GPUBufferRegion dst = { regions[i].dst, regions[i].dstOffset, regions[i].size };
        SDL_UploadToGPUBuffer(pass, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(pass);
}

// rebuilds the grass indirect draw list each frame: one command per grass column inside the
// view distance and camera frustum. first_instance points the instance-rate vertex fetch at
// that column's grass range.
static void TerrainGrassBuildDrawArgs(SDL_GPUCommandBuffer* cmd)
{
    g_Terrain.grassDrawCount = 0u;
    if (!g_Terrain.grassIndirectBuffer) return;
    f32 viewDist = g_Terrain.authoring.grassViewDistance;
    f32 size0 = TerrainChunkWorldSize(0);
    float3 cam = g_Camera.position;

    SDL_GPUIndirectDrawCommand* cmds =
        (SDL_GPUIndirectDrawCommand*)SDL_MapGPUTransferBuffer(g_GPUDevice, g_Terrain.grassIndirectTransfer, true);
    if (!cmds) return;

    u32 n = 0u;
    for (u32 i = 0; i < TERRAIN_MAX_GRASS_TILES; i++)
    {
        GrassTile* t = &g_Terrain.grassTiles[i];
        if (!t->used || t->dying || t->grassCount == 0u) continue;
        if (viewDist > 0.0f && TerrainGrassColumnDist(cam, t->cx, t->cz, size0) > viewDist) continue;
        if (g_Terrain.frustumValid)
        {
            v128f bmin = VecSetR(t->aabbMin.x, t->aabbMin.y, t->aabbMin.z, 0.0f);
            v128f bmax = VecSetR(t->aabbMax.x, t->aabbMax.y, t->aabbMax.z, 0.0f);
            if (!CheckAABBCulled(bmin, bmax, g_Terrain.cameraFrustum.planes)) continue;
        }
        cmds[n++] = (SDL_GPUIndirectDrawCommand){ 6u, t->grassCount, 0u, t->grassStart };
    }
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, g_Terrain.grassIndirectTransfer);

    if (n == 0u) return;
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = { g_Terrain.grassIndirectTransfer, 0 };
    SDL_GPUBufferRegion dst = { g_Terrain.grassIndirectBuffer, 0, n * (u32)sizeof(SDL_GPUIndirectDrawCommand) };
    SDL_UploadToGPUBuffer(pass, &src, &dst, false);
    SDL_EndGPUCopyPass(pass);
    g_Terrain.grassDrawCount = n;
}

#if TERRAIN_OLD_RUNTIME_DISABLED
// port mode grass: the density-driven grass tiles are self-contained (the scatter jobs
// march TerrainDensity_SurfaceY on their own pool), so only their slice of the old
// Terrain_Init has to exist. lazily created on the first flush with a world enabled.
static void TerrainInitPipelines(void);
static Texture TerrainLoadGrassArray(void);

static bool TerrainPortEnsureGrass(void)
{
    if (tvGrassReady) return true;
    if (!g_GPUDevice) return false;

    g_Terrain.grassTiles    = (GrassTile*)AllocZeroTLSFGlobal(TERRAIN_MAX_GRASS_TILES, sizeof(GrassTile));
    g_Terrain.grassTileFree = (u32*)AllocateTLSFGlobal(TERRAIN_MAX_GRASS_TILES * sizeof(u32));
    if (!g_Terrain.grassTiles || !g_Terrain.grassTileFree)
    {
        AX_WARN("terrain port grass allocation failed");
        return false;
    }
    for (u32 i = 0; i < TERRAIN_MAX_GRASS_TILES; i++)
        g_Terrain.grassTileFree[i] = TERRAIN_MAX_GRASS_TILES - 1u - i;
    g_Terrain.numGrassTileFree = TERRAIN_MAX_GRASS_TILES;
    g_Terrain.grassTileMap = HMCreate(TERRAIN_MAX_GRASS_TILES, sizeof(u32));

    for (u32 i = 0; i < TERRAIN_MAX_GRASS_JOBS; i++)
    {
        GrassJob* job = &g_Terrain.grassJobs[i];
        job->out = (GrassInstance*)SDL_malloc(GRASS_PER_CHUNK * sizeof(GrassInstance));
        SDL_SetAtomicInt(&job->state, JobState_Free);
    }

    s32 cores = SDL_GetNumLogicalCPUCores();
    u32 grassWorkers = (u32)Clamps32(cores - 2, 2, 8);
    g_Terrain.jobSystem = JobSystem_Create(grassWorkers, TERRAIN_MAX_JOBS);
    CHECK_CREATE(g_Terrain.jobSystem, "Terrain Port Grass Job System");

    g_Terrain.grassBuffer         = CreateBuffer(NULL, TERRAIN_MAX_GRASS * sizeof(GrassInstance), BVertexBit, "TerrainGrassBuffer");
    g_Terrain.grassChunkBuffer    = CreateBuffer(NULL, TERRAIN_MAX_CHUNKS * sizeof(GrassChunkInfo), BReadRasterBit, "TerrainGrassChunkBuffer");
    g_Terrain.grassIndirectBuffer = CreateBuffer(NULL, TERRAIN_MAX_CHUNKS * sizeof(SDL_GPUIndirectDrawCommand), SDL_GPU_BUFFERUSAGE_INDIRECT, "TerrainGrassIndirect");
    g_Terrain.grassIndirectTransfer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = TERRAIN_MAX_CHUNKS * (u32)sizeof(SDL_GPUIndirectDrawCommand)
    });
    CHECK_CREATE(g_Terrain.grassIndirectTransfer, "Terrain Grass Indirect Transfer");
    g_Terrain.grassUploadTransfer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = GRASS_UPLOAD_BYTES
    });
    CHECK_CREATE(g_Terrain.grassUploadTransfer, "Terrain Grass Upload Transfer");

    TerrainInitPipelines();
    g_Terrain.grassLayers = TerrainLoadGrassArray();
    g_Terrain.grassScanDirty = true;
    tvGrassReady = true;
    return true;
}
#endif

void Terrain_GPUFlush(SDL_GPUCommandBuffer* cmd)
{
    (void)cmd;
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (!tvWorldEnabled || g_Terrain.authoring.grassViewDistance <= 0.0f)
    {
        g_Terrain.grassDrawCount = 0;
        return;
    }
    if (!TerrainPortEnsureGrass())
        return;

    // the grass ring scan, frustum cull, and consume all read state Terrain_Update used
    // to maintain; feed it from the camera directly since the old update never runs
    g_Terrain.cameraFrustum = CreateFrustumPlanesRevZ(M44Multiply(g_Camera.view, g_Camera.projection));
    g_Terrain.frustumValid = true;
    TerrainGrassConsume(cmd);   // pick up finished scatter jobs -> upload, frees job slots
    TerrainGrassDispatch();     // reclaim + ring scan -> dispatch new scatter jobs
    TerrainGrassBuildDrawArgs(cmd);
    return;
#endif
    if (!g_Terrain.initialized) return;

    TerrainCopyRegion regions[TERRAIN_MAX_JOBS * 4u]; // vertex+index (+grass+grassInfo for lod0)
    u32 numRegions = 0;
    u32 cursor = 0;
    u8* mapped = NULL;

    for (u32 slot = 0; slot < TERRAIN_MAX_JOBS; slot++)
    {
        TerrainJob* job = &g_Terrain.jobs[slot];
        if (SDL_GetAtomicInt(&job->state) != JobState_Done) continue;

        TerrainChunk* chunk = &g_Terrain.chunks[job->chunkSlot];
		if (ShouldSkipGenerate(slot, chunk, job))
			continue;

        u32 vertexBytes = job->mesh.numVertices * (u32)sizeof(TerrainVertex);
        u32 indexBytes  = job->mesh.numIndices * (u32)sizeof(u32);
        if (cursor + vertexBytes + indexBytes > TERRAIN_TRANSFER_BYTES)
            break; // out of staging space, the job stays done and retries next frame

        void* vertexRaw = NULL;
        void* indexRaw = NULL;
        u32 vertexOffset = GeometryHeapAlloc(GeometryBuffer_TerrainVertex, job->mesh.numVertices, &vertexRaw);
        u32 indexOffset  = vertexOffset == TERRAIN_HEAP_FAIL ? TERRAIN_HEAP_FAIL
                         : GeometryHeapAlloc(GeometryBuffer_TerrainIndex, job->mesh.numIndices, &indexRaw);
        if (indexOffset == TERRAIN_HEAP_FAIL)
        {
            if (vertexOffset != TERRAIN_HEAP_FAIL)
                GeometryHeapFree(GeometryBuffer_TerrainVertex, vertexRaw);
            AX_WARN("terrain geometry heap full, chunk dropped to retry");
            chunk->jobSlot = -1;
            chunk->state = ChunkState_Queued;
            TerrainPendingPush(job->chunkSlot);
            SDL_SetAtomicInt(&job->state, JobState_Free);
            continue;
        }
        TerrainVertex* cpuVertices = (TerrainVertex*)gGFX.TerrainVertexBuffer + vertexOffset;
        u32* cpuIndices = gGFX.TerrainIndexBuffer + indexOffset;
        MemCopy(cpuVertices, job->mesh.vertices, vertexBytes);
        MemCopy(cpuIndices, job->mesh.indices, indexBytes);

        if (!mapped)
        {
            mapped = (u8*)SDL_MapGPUTransferBuffer(g_GPUDevice, g_Terrain.transferBuffer, true);
            if (!mapped)
            {
                GeometryHeapFree(GeometryBuffer_TerrainVertex, vertexRaw);
                GeometryHeapFree(GeometryBuffer_TerrainIndex, indexRaw);
                AX_WARN("terrain transfer map failed: %s", SDL_GetError());
                break;
            }
        }

        MemCopy(mapped + cursor, cpuVertices, vertexBytes);
        regions[numRegions++] = (TerrainCopyRegion){
            g_Terrain.vertexBuffer, cursor, vertexOffset * (u32)sizeof(TerrainVertex), vertexBytes };
        cursor += vertexBytes;

        MemCopy(mapped + cursor, cpuIndices, indexBytes);
        regions[numRegions++] = (TerrainCopyRegion){
            g_Terrain.indexBuffer, cursor, indexOffset * (u32)sizeof(u32), indexBytes };
        cursor += indexBytes;

        // swap the resident mesh, the freed range can be reused immediately: the copy
        // pass lands before this frame's draws and the draws use the new offsets
        bool wasVisible = chunk->numIndices > 0u;
        bool wasEmpty   = chunk->empty; // was all-solid/all-air; capture before clearing below
        TerrainChunkFreeMeshEx(job->chunkSlot, false);
        chunk->vertexHeapPtr = vertexRaw;
        chunk->indexHeapPtr  = indexRaw;
        chunk->vertexOffset = vertexOffset;
        chunk->numVertices  = job->mesh.numVertices;
        chunk->indexOffset  = indexOffset;
        chunk->numIndices   = job->mesh.numIndices;
        g_Terrain.numAllocatedVertices += chunk->numVertices;
        g_Terrain.numAllocatedIndices  += chunk->numIndices;
        chunk->empty = 0;
        chunk->appliedMask = job->transitionMask;
        chunk->state = ChunkState_Live;
        chunk->jobSlot = -1;
        // fresh split/merge replacements stay hidden until the whole group is live, so a
        // half-built lod swap never shows a hole. remeshes of an already-visible chunk, and
        // edits that reveal a previously-empty chunk (digging into solid ground below the
        // surface), are in-place swaps that must show immediately or they read as see-through.
        if (!wasVisible && !wasEmpty) chunk->hidden = TerrainCoveredByRetiring(chunk);
        g_Terrain.numDrawable++;

        f32 size = TerrainChunkWorldSize(chunk->lod);
        float3 origin = { (f32)chunk->x * size, (f32)chunk->y * size, (f32)chunk->z * size };
        chunk->aabbMin = F3Add(origin, job->mesh.aabbMin);
        chunk->aabbMax = F3Add(origin, job->mesh.aabbMax);
        TerrainChunkSyncPhysics(job->chunkSlot);

        // a lod0 remesh means the density under this column may have been sculpted; flag the
        // grass tile so it rebuilds from the new surface. (grass itself is generated from the
        // density field, so it does not otherwise depend on the mesh being ready.)
        if (chunk->lod == 0u)
        {
            GrassTile* tile = TerrainGrassTileFind(chunk->x, chunk->z);
            if (tile) { tile->dirty = 1u; g_Terrain.grassScanDirty = true; }
        }

        // the desired mask moved on or a brush edit landed while the worker was
        // running: remesh with fresh data
        if (chunk->transitionMask != chunk->appliedMask || chunk->needRemesh)
        {
            chunk->needRemesh = 1;
            TerrainPendingPush(job->chunkSlot);
        }
        SDL_SetAtomicInt(&job->state, JobState_Free);
    }

    if (mapped)
    {
        SDL_UnmapGPUTransferBuffer(g_GPUDevice, g_Terrain.transferBuffer);
        SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
        for (u32 i = 0; i < numRegions; i++)
        {
            SDL_GPUTransferBufferLocation src = { g_Terrain.transferBuffer, regions[i].transferOffset };
            SDL_GPUBufferRegion dst = { regions[i].dst, regions[i].dstOffset, regions[i].size };
            SDL_UploadToGPUBuffer(pass, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(pass);
    }

    if (g_Terrain.enabled)
    {
        TerrainGrassConsume(cmd);   // pick up finished scatter jobs -> upload, frees job slots
        TerrainGrassDispatch();     // reclaim + ring scan -> dispatch new scatter jobs
        TerrainGrassBuildDrawArgs(cmd);
    }
}

// ---------------------------------------------------------------------------------
// raycast: cpu mesh copies of the drawn chunk set, used by gameplay and the editor
// ---------------------------------------------------------------------------------

static v128f TerrainDecodePosition(const TerrainVertex* v, v128f chunkOrigin, f32 metersPerStep)
{
    u32 qx = v->posA & 0x1FFFFFu;
    u32 qy = (v->posA >> 21) | ((v->posB & 0x3FFu) << 11);
    u32 qz = (v->posB >> 10) & 0x1FFFFFu;
    v128f local = VecSetR((f32)qx, (f32)qy, (f32)qz, 0.0f);
    return VecAdd(chunkOrigin, VecMulf(local, metersPerStep));
}


// grass placement tuning (see TerrainBuildTileGrass)
#define GRASS_BEACH_BAND      5.0f  // meters above sea level kept blade-free; matches the terrain
                                    // grass texture onset (sand->dirt->grass) so blades sit on green
#define GRASS_ISLAND_MAX      0.02f // island fade above this is off the island proper -> no grass
#define GRASS_PATCH_THRESHOLD 0.55f // cellular F1 cutoff; lower = more bare patches, higher = denser
#define GRASS_BLADE_EXTENT    0.7f  // billboard half-width + wind sway, padded into the tile AABB
#define GRASS_BLADE_HEIGHT    2.0f  // billboard height above the blade root, padded into the AABB

// scatter camera-facing grass over one 16 m surface column by marching the density field:
// jitter (x,z) inside the column, find the surface Y from the SDF, filter by island/beach/
// slope and cellular thinning, and write origin-relative fp16 positions plus the tile slot.
// no mesh required, so a column loads at any distance regardless of terrain lod. also writes
// the grass world aabb for culling. returns the instance count (capped at GRASS_PER_CHUNK).
static u32 TerrainBuildTileGrass(u32 tileSlot, s32 cx, s32 cz, GrassInstance* out,
                                 float3* outMin, float3* outMax)
{
    f32 size0 = TerrainChunkWorldSize(0);
    f32 originX = (f32)cx * size0, originZ = (f32)cz * size0;
    f32 seaLevel = g_Terrain.genParams.seaLevel;
    f32 density = g_Terrain.authoring.grassDensity;
    f32 bladesPerSqM = density > 0.0f ? 2.0f * density : 0.0f;
    if (bladesPerSqM <= 0.0f) return 0u;

    u32 target = (u32)(bladesPerSqM * size0 * size0);
    if (target > GRASS_PER_CHUNK) target = GRASS_PER_CHUNK;
    u32 tileIndex = tileSlot & 0xFFFFu;
    // deterministic per-column seed so grass placement stays stable across remeshes
    u32 seed = WangHash((u32)cx * 73856093u ^ (u32)cz * 19349663u);

    float3 aMin = { 1e30f, 1e30f, 1e30f }, aMax = { -1e30f, -1e30f, -1e30f };
    u32 count = 0u;
    for (u32 b = 0u; b < target; b++)
    {
        f32 wx = originX + NextFloat01(PCG2Next(&seed)) * size0;
        f32 wz = originZ + NextFloat01(PCG2Next(&seed)) * size0;
        // island only: skip the open-sea plane before paying for the surface march (a no-op
        // when island mode is off, where the mask is always 0)
        if (TerrainDensity_IslandMask(wx, wz) > GRASS_ISLAND_MAX) continue;
        float3 normal;
        f32 wy = TerrainDensity_SurfaceY(wx, wz, TerrainDensity_Height(wx, wz), &normal);
        if (wy < seaLevel + GRASS_BEACH_BAND) continue; // no grass underwater or on the beach
        if (normal.y < 0.55f) continue;                 // skip steep slopes

        float2 cell = NoiseCellular2D((float2){ wx * 0.15f, wz * 0.15f });
        if (cell.x > GRASS_PATCH_THRESHOLD) continue;   // thin into patches

        out[count].positionXY = MakeHalf2(FloatToHalf(wx - originX), FloatToHalf(wy));
        out[count].positionZChunkIndex = (u32)FloatToHalf(wz - originZ) | (tileIndex << 16);
        count++;
        float3 root = { wx, wy, wz };
        aMin = F3Min(aMin, root);
        aMax = F3Max(aMax, root);
    }

    if (count == 0u) { *outMin = *outMax = (float3){ originX, 0.0f, originZ }; return 0u; }
    // pad the root bounds by the blade extent: billboards are camera-facing and sway/rise
    // above their root, so an unpadded box frustum-culls edge tiles and leaves grass holes
    *outMin = (float3){ aMin.x - GRASS_BLADE_EXTENT, aMin.y, aMin.z - GRASS_BLADE_EXTENT };
    *outMax = (float3){ aMax.x + GRASS_BLADE_EXTENT, aMax.y + GRASS_BLADE_HEIGHT, aMax.z + GRASS_BLADE_EXTENT };
    return count;
}

static f32 TerrainPhysicsTriangleAreaSq(const b3Vec3* vertices, u32 a, u32 b, u32 c)
{
    v128f v0 = Vec3Load(&vertices[a].x);
    v128f v1 = Vec3Load(&vertices[b].x);
    v128f v2 = Vec3Load(&vertices[c].x);
    v128f cross = Vec3Cross(VecSub(v1, v0), VecSub(v2, v0));
    return Vec3DotfV(cross, cross);
}

static void TerrainChunkDestroyPhysics(u32 slot)
{
    Scene* scene = Scene_GetActive();
    if (!scene) return;
    Scene_PhysicsDestroyTerrainChunk(scene, slot);
}

static void TerrainChunkSyncPhysics(u32 slot)
{
    Scene* scene = Scene_GetActive();
    if (!scene || slot >= TERRAIN_MAX_CHUNKS) return;

    TerrainChunk* chunk = &g_Terrain.chunks[slot];
    if (!chunk->used || chunk->state != ChunkState_Live || chunk->numIndices < 3u ||
        chunk->numVertices == 0u || chunk->hidden || chunk->lod > TERRAIN_PHYSICS_MAX_LOD)
    {
        Scene_PhysicsDestroyTerrainChunk(scene, slot);
        return;
    }

    ArenaMark mark = ArenaSave(&GlobalArena);
    b3Vec3* vertices = (b3Vec3*)ArenaAllocGlobal(chunk->numVertices * sizeof(b3Vec3));
    s32* indices = (s32*)ArenaAllocGlobal(chunk->numIndices * sizeof(s32));
    if (!vertices || !indices)
    {
        ArenaRestore(&GlobalArena, mark);
        AX_WARN("terrain physics: scratch allocation failed for chunk slot %u", slot);
        Scene_PhysicsDestroyTerrainChunk(scene, slot);
        return;
    }

    f32 size = TerrainChunkWorldSize(chunk->lod);
    v128f chunkOrigin = VecSetR((f32)chunk->x * size, (f32)chunk->y * size, (f32)chunk->z * size, 0.0f);
    f32 metersPerStep = size / (f32)TERRAIN_POS_MAX;
    TerrainVertex* srcVertices = (TerrainVertex*)gGFX.TerrainVertexBuffer + chunk->vertexOffset;
    for (u32 v = 0; v < chunk->numVertices; v++)
    {
        v128f p = TerrainDecodePosition(&srcVertices[v], chunkOrigin, metersPerStep);
        vertices[v] = (b3Vec3){ VecGetX(p), VecGetY(p), VecGetZ(p) };
    }

    u32 outIndexCount = 0u;
    const u32* srcIndices = gGFX.TerrainIndexBuffer + chunk->indexOffset;
    for (u32 t = 0; t + 2u < chunk->numIndices; t += 3u)
    {
        u32 i0 = srcIndices[t + 0u];
        u32 i1 = srcIndices[t + 1u];
        u32 i2 = srcIndices[t + 2u];
        if (i0 >= chunk->numVertices || i1 >= chunk->numVertices || i2 >= chunk->numVertices)
            continue;
        if (TerrainPhysicsTriangleAreaSq(vertices, i0, i1, i2) <= 1.0e-10f)
            continue;

        indices[outIndexCount++] = (s32)i0;
        indices[outIndexCount++] = (s32)i1;
        indices[outIndexCount++] = (s32)i2;
    }

	if (!Scene_PhysicsSyncTerrainChunkMesh(scene, slot, vertices, chunk->numVertices, indices, outIndexCount))
		Scene_PhysicsDestroyTerrainChunk(scene, slot);
    ArenaRestore(&GlobalArena, mark);
}

s32 Terrain_Raycast(float3 origin, float3 dir, f32 maxDist, u32 maxLod, BVHHit* hit)
{
    (void)origin;
    (void)dir;
    (void)maxDist;
    (void)maxLod;
    (void)hit;
#if TERRAIN_OLD_RUNTIME_DISABLED
    return 0;
#endif
    if (!g_Terrain.initialized || !g_Terrain.enabled) return 0;

    v128f rayOrigin = VecSetR(origin.x, origin.y, origin.z, 0.0f);
    v128f rayDir    = VecSetR(dir.x, dir.y, dir.z, 0.0f);
    v128f invDir    = VecDiv(VecOne(), rayDir);

    hit->hit.t = maxDist;
    bool anyHit = false;

    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
    {
        TerrainChunk* chunk = &g_Terrain.chunks[i];
        // exactly the drawn set: live with geometry, hidden swaps excluded
        if (!chunk->used || chunk->state != ChunkState_Live || chunk->numIndices == 0u || chunk->hidden) continue;
        if (chunk->lod > maxLod) continue;
        if (chunk->lod > TERRAIN_RAYCAST_KEEP_LOD) continue;

        v128f bmin = Vec3Load(&chunk->aabbMin.x);
        v128f bmax = Vec3Load(&chunk->aabbMax.x);
        if (!IntersectAABB(rayOrigin, invDir, bmin, bmax, hit->hit.t)) continue;

        f32 size = TerrainChunkWorldSize(chunk->lod);
        v128f chunkOrigin = VecSetR((f32)chunk->x * size, (f32)chunk->y * size, (f32)chunk->z * size, 0.0f);
        f32 metersPerStep = size / (f32)TERRAIN_POS_MAX;
        TerrainVertex* vertices = (TerrainVertex*)gGFX.TerrainVertexBuffer + chunk->vertexOffset;
        u32* indices = gGFX.TerrainIndexBuffer + chunk->indexOffset;

        for (u32 t = 0; t + 2 < chunk->numIndices; t += 3)
        {
            v128f v0 = TerrainDecodePosition(&vertices[indices[t + 0]], chunkOrigin, metersPerStep);
            v128f v1 = TerrainDecodePosition(&vertices[indices[t + 1]], chunkOrigin, metersPerStep);
            v128f v2 = TerrainDecodePosition(&vertices[indices[t + 2]], chunkOrigin, metersPerStep);
            if (IntersectTriangle(rayOrigin, rayDir, v0, v1, v2, &hit->hit))
            {
                anyHit = true;
                hit->triIndex   = t / 3u;
                hit->entityIdx  = i;            // terrain chunk slot
                hit->groupIdx   = chunk->lod;
                hit->skinnedSet = 0xFFFFFFFFu;
                hit->bundleIdx  = 0xFFFFFFFFu;
            }
        }
    }

    if (!anyHit) return 0;
    return 1;
}

// ---------------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------------

bool Terrain_HasDraws(void)
{
#if TERRAIN_OLD_RUNTIME_DISABLED
    return false;
#endif
    return g_Terrain.initialized && g_Terrain.enabled && g_Terrain.numDrawable > 0u;
}

static void TerrainFillVSParams(TerrainVSParams* params, const TerrainChunk* chunk, mat4x4 viewProj)
{
    f32 size = TerrainChunkWorldSize(chunk->lod);
    params->viewProj = viewProj;
    params->chunkOriginSize[0] = (f32)chunk->x * size;
    params->chunkOriginSize[1] = (f32)chunk->y * size;
    params->chunkOriginSize[2] = (f32)chunk->z * size;
    params->chunkOriginSize[3] = size;
    params->cameraPosition[0] = g_Camera.position.x;
    params->cameraPosition[1] = g_Camera.position.y;
    params->cameraPosition[2] = g_Camera.position.z;
    params->cameraPosition[3] = 0.0f;
    params->cameraForward[0] = g_Camera.Front.x;
    params->cameraForward[1] = g_Camera.Front.y;
    params->cameraForward[2] = g_Camera.Front.z;
    params->cameraForward[3] = 0.0f;
}

// one indirect multidraw over every frustum-visible grass chunk built this frame. the
// camera-facing billboards are procedural (SV_VertexID quad), so no index buffer; the
// instance stream + chunk origin storage buffer give each blade its world position.
static void TerrainDrawGrass(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass)
{
    if (g_Terrain.grassDrawCount == 0u || !g_Terrain.grassPipeline || !g_Terrain.grassLayers.handle) return;

    SDL_BindGPUGraphicsPipeline(pass, g_Terrain.grassPipeline);

    SDL_GPUBufferBinding instanceBinding = { g_Terrain.grassBuffer, 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &instanceBinding, 1);
    SDL_GPUBuffer* vsStorage[1] = { g_Terrain.grassChunkBuffer };
    SDL_BindGPUVertexStorageBuffers(pass, 0, vsStorage, 1);

    SDL_GPUTextureSamplerBinding grassTex = { .texture = g_Terrain.grassLayers.handle, .sampler = g_RenderState.sampler };
    SDL_BindGPUFragmentSamplers(pass, 0, &grassTex, 1);

    GrassVSParams vs = {0};
    vs.view = g_Camera.view;
    vs.proj = g_Camera.projection;
    vs.cameraTime[0] = g_Camera.position.x;
    vs.cameraTime[1] = g_Camera.position.y;
    vs.cameraTime[2] = g_Camera.position.z;
    vs.cameraTime[3] = (f32)SDL_GetTicks() * 0.001f;
    vs.grassParams[0] = g_Terrain.authoring.grassViewDistance;
    SDL_PushGPUVertexUniformData(cmd, 0, &vs, sizeof(vs));

    float3 sun = GetRenderSunDirection();
    GrassFSParams fs = {0};
    fs.sunDirection[0] = sun.x; fs.sunDirection[1] = sun.y; fs.sunDirection[2] = sun.z;
    // base #495727, variant #a46d48 (sRGB), blended per blade by the texture noise
    fs.grassColor[0] = 0x49 / 255.0f; fs.grassColor[1] = 0x57 / 255.0f; fs.grassColor[2] = 0x27 / 255.0f;
    fs.grassColorVariant[0] = 0xA4 / 255.0f; fs.grassColorVariant[1] = 0x6D / 255.0f; fs.grassColorVariant[2] = 0x48 / 255.0f;
    SDL_PushGPUFragmentUniformData(cmd, 0, &fs, sizeof(fs));

    SDL_DrawGPUPrimitivesIndirect(pass, g_Terrain.grassIndirectBuffer, 0, g_Terrain.grassDrawCount);
}

// port mode: the transvoxel unity example draws the terrain surface, but the grass
// still lives here; the forward pass calls this right after the port terrain draw.
// safe no-op while grass is not initialized (grassDrawCount stays 0).
void Terrain_RenderGrass(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass)
{
    TerrainDrawGrass(cmd, pass);
}

static u32 TerrainDrawChunks(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, mat4x4 viewProj)
{
    FrustumPlanes frustum = CreateFrustumPlanesRevZ(viewProj);
    u32 drawn = 0;
    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
    {
        TerrainChunk* chunk = &g_Terrain.chunks[i];
        if (!chunk->used || chunk->state != ChunkState_Live || chunk->numIndices == 0u || chunk->hidden) continue;

        v128f aabbMin = Vec3Load(&chunk->aabbMin.x);
        v128f aabbMax = Vec3Load(&chunk->aabbMax.x);
        if (!CheckAABBCulled(aabbMin, aabbMax, frustum.planes)) continue;

        TerrainVSParams params;
        TerrainFillVSParams(&params, chunk, viewProj);
        SDL_PushGPUVertexUniformData(cmd, 0, &params, sizeof(params));
        SDL_DrawGPUIndexedPrimitives(pass, chunk->numIndices, 1, chunk->indexOffset, (s32)chunk->vertexOffset, 0);
        drawn++;
    }
    return drawn;
}

void Terrain_RenderDepth(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, mat4x4 viewProj)
{
    (void)cmd;
    (void)pass;
    (void)viewProj;
#if TERRAIN_OLD_RUNTIME_DISABLED
    return;
#endif
    if (!Terrain_HasDraws()) return;

    SDL_BindGPUGraphicsPipeline(pass, g_Terrain.depthPipeline);
    SDL_GPUBufferBinding vertexBinding = { g_Terrain.vertexBuffer, 0 };
    SDL_GPUBufferBinding indexBinding  = { g_Terrain.indexBuffer, 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    TerrainDrawChunks(cmd, pass, viewProj);
}

void Terrain_RenderForward(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, mat4x4 viewProj, u32 width, u32 height)
{
    (void)cmd;
    (void)pass;
    (void)viewProj;
    (void)width;
    (void)height;
#if TERRAIN_OLD_RUNTIME_DISABLED
    return;
#endif
    if (!Terrain_HasDraws()) return;
    if (!g_WindowState.tex_shadow_color || !g_RenderState.shadowCascadeBuffer ||
        !g_WindowState.tex_hbao_blur || !g_WindowState.tex_contact_shadow)
        return;
    
	SDL_BindGPUGraphicsPipeline(pass, g_Terrain.forwardPipeline);
    SDL_GPUBufferBinding vertexBinding = { g_Terrain.vertexBuffer, 0 };
    SDL_GPUBufferBinding indexBinding  = { g_Terrain.indexBuffer, 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUBuffer* vertexStorage[1] = { g_RenderState.shadowCascadeBuffer };
    SDL_BindGPUVertexStorageBuffers(pass, 0, vertexStorage, 1);

    SDL_GPUTextureSamplerBinding samplers[6] = {
        { .texture = g_Terrain.albedoLayers.handle, .sampler = g_RenderState.sampler },
        { .texture = g_Terrain.normalLayers.handle, .sampler = g_RenderState.sampler },
        { .texture = g_Terrain.armLayers.handle,    .sampler = g_RenderState.sampler },
        { .texture = g_WindowState.tex_shadow_color, .sampler = g_RenderState.shadowSampler },
        { .texture = g_WindowState.tex_hbao_blur, .sampler = g_RenderState.sampler },
        { .texture = g_WindowState.tex_contact_shadow, .sampler = g_RenderState.sampler }
    };
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, SDL_arraysize(samplers));

    float3 sunDirection = GetRenderSunDirection();
    TerrainForwardFragmentParams fragmentParams = {0};
    fragmentParams.sunDirection[0] = sunDirection.x;
    fragmentParams.sunDirection[1] = sunDirection.y;
    fragmentParams.sunDirection[2] = sunDirection.z;
    fragmentParams.brushPosRadius[0] = g_Terrain.brushPos.x;
    fragmentParams.brushPosRadius[1] = g_Terrain.brushPos.y;
    fragmentParams.brushPosRadius[2] = g_Terrain.brushPos.z;
    fragmentParams.brushPosRadius[3] = g_Terrain.brushRadius;
    fragmentParams.cameraPosition[0] = g_Camera.position.x;
    fragmentParams.cameraPosition[1] = g_Camera.position.y;
    fragmentParams.cameraPosition[2] = g_Camera.position.z;
    fragmentParams.outputSize[0] = width;
    fragmentParams.outputSize[1] = height;
    SDL_PushGPUFragmentUniformData(cmd, 0, &fragmentParams, sizeof(fragmentParams));

    g_Terrain.drawnLastFrame = TerrainDrawChunks(cmd, pass, viewProj);

    // grass draws into the same forward pass, right after the terrain surface it sits on
    TerrainDrawGrass(cmd, pass);
}

// debug line overlay over the lit scene, toggled from the graphics settings panel
void Terrain_RenderWireframe(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget,
                             SDL_GPUDepthStencilTargetInfo* depthTarget, mat4x4 viewProj)
{
    (void)cmd;
    (void)colorTarget;
    (void)depthTarget;
    (void)viewProj;
#if TERRAIN_OLD_RUNTIME_DISABLED
    return;
#endif
    if (!g_RenderSettings.terrainWireframe || !Terrain_HasDraws()) return;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, colorTarget, 1, depthTarget);
    SDL_BindGPUGraphicsPipeline(pass, g_Terrain.wirePipeline);
    SDL_GPUBufferBinding vertexBinding = { g_Terrain.vertexBuffer, 0 };
    SDL_GPUBufferBinding indexBinding  = { g_Terrain.indexBuffer, 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    TerrainDrawChunks(cmd, pass, viewProj);
    SDL_EndGPURenderPass(pass);
}

// ---------------------------------------------------------------------------------
// init: pipelines, buffers, triplanar texture arrays
// ---------------------------------------------------------------------------------

static SDL_GPUShader* TerrainCreateShader(const u8* code, size_t codeSize, SDL_GPUShaderStage stage,
                                          const char* entry, u32 numUniforms, u32 numSamplers, u32 numStorage)
{
    SDL_GPUShader* shader = SDL_CreateGPUShader(g_GPUDevice, &(SDL_GPUShaderCreateInfo){
        .code = code, .code_size = codeSize,
        .format = AX_GPU_SHADER_FORMAT,
        .stage = stage,
        .entrypoint = entry,
        .num_uniform_buffers = numUniforms,
        .num_samplers = numSamplers,
        .num_storage_buffers = numStorage
    });
    CHECK_CREATE(shader, "Terrain Shader");
    return shader;
}

static void TerrainInitPipelines(void)
{
    const SDL_GPUVertexAttribute vertexAttributes[1] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT4, .offset = 0 }
    };
    SDL_GPUVertexInputState vertexInput = {
        .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){
            0, sizeof(TerrainVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0
        },
        .num_vertex_buffers = 1,
        .vertex_attributes = vertexAttributes,
        .num_vertex_attributes = 1
    };

    // forward pass, formats and depth state match the opaque forward surface pass
    {
        SDL_GPUShader* vert = TerrainCreateShader(Shaders_TerrainForwardVert_spv, sizeof(Shaders_TerrainForwardVert_spv),
                                                  SDL_GPU_SHADERSTAGE_VERTEX, "vert", 1, 0, 1);
        SDL_GPUShader* frag = TerrainCreateShader(Shaders_TerrainForwardFrag_spv, sizeof(Shaders_TerrainForwardFrag_spv),
                                                  SDL_GPU_SHADERSTAGE_FRAGMENT, "frag", 1, 6, 0);
        g_Terrain.forwardPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
            .vertex_shader   = vert,
            .fragment_shader = frag,
            .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
                .num_color_targets         = 1,
                .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT },
                .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                .has_depth_stencil_target  = true
            },
            .depth_stencil_state = (SDL_GPUDepthStencilState){
                .enable_depth_test  = true,
                .enable_depth_write = g_RenderState.sceneSampleCount != SDL_GPU_SAMPLECOUNT_1,
                .compare_op         = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
            },
            .multisample_state  = (SDL_GPUMultisampleState){ .sample_count = g_RenderState.sceneSampleCount },
            .vertex_input_state = vertexInput
        });
        CHECK_CREATE(g_Terrain.forwardPipeline, "Terrain Forward Pipeline");
        SDL_ReleaseGPUShader(g_GPUDevice, vert);
        SDL_ReleaseGPUShader(g_GPUDevice, frag);
    }

    // depth prepass, formats match InitDepthOnlyPipelines
    {
        SDL_GPUShader* vert = TerrainCreateShader(Shaders_TerrainDepthOnlyVert_spv, sizeof(Shaders_TerrainDepthOnlyVert_spv),
                                                  SDL_GPU_SHADERSTAGE_VERTEX, "vert", 1, 0, 0);
        SDL_GPUShader* frag = TerrainCreateShader(Shaders_TerrainDepthOnlyFrag_spv, sizeof(Shaders_TerrainDepthOnlyFrag_spv),
                                                  SDL_GPU_SHADERSTAGE_FRAGMENT, "frag", 0, 0, 0);
        g_Terrain.depthPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
            .vertex_shader   = vert,
            .fragment_shader = frag,
            .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
                .num_color_targets         = 1,
                .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT },
                .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                .has_depth_stencil_target  = true
            },
            .depth_stencil_state = (SDL_GPUDepthStencilState){
                .enable_depth_test  = true,
                .enable_depth_write = true,
                .compare_op         = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
            },
            .multisample_state  = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 },
            .vertex_input_state = vertexInput
        });
        CHECK_CREATE(g_Terrain.depthPipeline, "Terrain Depth Pipeline");
        SDL_ReleaseGPUShader(g_GPUDevice, vert);
        SDL_ReleaseGPUShader(g_GPUDevice, frag);
    }

    // wireframe overlay: line fill over the lit hdr color target, depth tested against
    // the scene with a small bias so the lines win against their own triangles
    {
        SDL_GPUShader* vert = TerrainCreateShader(Shaders_TerrainDepthOnlyVert_spv, sizeof(Shaders_TerrainDepthOnlyVert_spv),
                                                  SDL_GPU_SHADERSTAGE_VERTEX, "vert", 1, 0, 0);
        SDL_GPUShader* frag = TerrainCreateShader(Shaders_TerrainWireFrag_spv, sizeof(Shaders_TerrainWireFrag_spv),
                                                  SDL_GPU_SHADERSTAGE_FRAGMENT, "wireFrag", 0, 0, 0);
        g_Terrain.wirePipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
            .vertex_shader   = vert,
            .fragment_shader = frag,
            .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
                .num_color_targets         = 1,
                .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT },
                .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                .has_depth_stencil_target  = true
            },
            .rasterizer_state = (SDL_GPURasterizerState){
                .fill_mode = SDL_GPU_FILLMODE_LINE,
                .enable_depth_bias = true,
                .depth_bias_constant_factor = -1.0f,
                .depth_bias_slope_factor    = -1.0f
            },
            .depth_stencil_state = (SDL_GPUDepthStencilState){
                .enable_depth_test  = true,
                .enable_depth_write = false,
                .compare_op         = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
            },
            .multisample_state  = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 },
            .vertex_input_state = vertexInput
        });
        CHECK_CREATE(g_Terrain.wirePipeline, "Terrain Wireframe Pipeline");
        SDL_ReleaseGPUShader(g_GPUDevice, vert);
        SDL_ReleaseGPUShader(g_GPUDevice, frag);
    }

    // grass: instanced camera-facing billboards. the instance data is an instance-rate
    // vertex stream (so indirect first_instance offsets it), the quad corner comes from
    // SV_VertexID, and the chunk origin from a storage buffer. alpha cutout, so it draws
    // as opaque geometry into the hdr forward target with depth write on.
    {
        SDL_GPUVertexAttribute grassAttr = {
            .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT2, .offset = 0
        };
        SDL_GPUVertexInputState grassInput = {
            .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){
                0, sizeof(GrassInstance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0
            },
            .num_vertex_buffers = 1,
            .vertex_attributes = &grassAttr,
            .num_vertex_attributes = 1
        };
        SDL_GPUShader* vert = TerrainCreateShader(Shaders_GrassVert_spv, sizeof(Shaders_GrassVert_spv),
                                                  SDL_GPU_SHADERSTAGE_VERTEX, "vert", 1, 0, 1);
        SDL_GPUShader* frag = TerrainCreateShader(Shaders_GrassFrag_spv, sizeof(Shaders_GrassFrag_spv),
                                                  SDL_GPU_SHADERSTAGE_FRAGMENT, "frag", 1, 1, 0);
        g_Terrain.grassPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
            .vertex_shader   = vert,
            .fragment_shader = frag,
            .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
                .num_color_targets         = 1,
                .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT },
                .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                .has_depth_stencil_target  = true
            },
            .rasterizer_state    = (SDL_GPURasterizerState){ .cull_mode = SDL_GPU_CULLMODE_NONE },
            .depth_stencil_state = (SDL_GPUDepthStencilState){
                .enable_depth_test  = true,
                .enable_depth_write = true,
                .compare_op         = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
            },
            .multisample_state  = (SDL_GPUMultisampleState){ .sample_count = g_RenderState.sceneSampleCount },
            .vertex_input_state = grassInput
        });
        CHECK_CREATE(g_Terrain.grassPipeline, "Terrain Grass Pipeline");
        SDL_ReleaseGPUShader(g_GPUDevice, vert);
        SDL_ReleaseGPUShader(g_GPUDevice, frag);
    }
}

// loads count pngs into one rgba8 texture array layer by layer, resizing to the target
// size when needed. srgb only affects the resize filter, the formats stay unorm and the
// shader converts srgb to linear like the surface shader does.
static Texture TerrainLoadTextureArray(const char* const* paths, u32 count, s32 size, bool srgb,
                                       const char* label, const char* errorLabel)
{
    Texture tex = rCreateTexture2DArray(size, size, count, NULL, TEX_FMT_8UNORM4,
                                        TexFlags_MipMap, TEX_SAMPLER | TEX_COLOR_TARGET, label);
    for (s32 layer = 0; layer < (s32)count; layer++)
    {
        int w, h, channels;
        u8* image = stbi_load(paths[layer], &w, &h, &channels, 4);
        if (!image)
        {
            AX_ERROR("%s texture missing: %s", errorLabel, paths[layer]);
            continue;
        }
        u8* upload = image;
        u8* resized = NULL;
        if (w != size || h != size)
        {
            resized = (u8*)SDL_malloc((size_t)size * size * 4u);
            if (srgb) stbir_resize_uint8_srgb(image, w, h, 0, resized, size, size, 0, STBIR_RGBA);
            else      stbir_resize_uint8_linear(image, w, h, 0, resized, size, size, 0, STBIR_RGBA);
            upload = resized;
        }
        UploadTextureRegion(tex, (u32)layer, 0, 0, (u32)size, (u32)size, (u32)size, (u32)size, upload);
        if (resized) SDL_free(resized);
        stbi_image_free(image);
    }
    GenerateTextureMips(tex);
    return tex;
}

static Texture TerrainLoadLayerArray(const char* const* paths, u32 count, s32 size, bool srgb, const char* label)
{
    return TerrainLoadTextureArray(paths, count, size, srgb, label, "terrain");
}

// terrain material layers. index 0 is the flat-ground ("grass") layer, 1 the slope
// ("dirt") layer, 2 the high-rock layer, 3 the beach/underwater sand. layers 0 and 1 are
// swapped from the raw asset order so flat surfaces read grassy and slopes read dirty.
#define TERRAIN_LAYER_COUNT 4u
static const char* const albedoPaths[TERRAIN_LAYER_COUNT] = {
	"Assets/Textures/Terrain/rocky_terrain_02_diff_2k.png",    // 0 flat / grass
	"Assets/Textures/Terrain/brown_mud_leaves_01_diff_2k.png", // 1 slope / dirt
	"Assets/Textures/Terrain/rocky_terrain_diff_2k.png",       // 2 high rock
	"Assets/Textures/Terrain/sandydrysoil-albedo2b.png"        // 3 beach / underwater
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
	"Assets/Textures/Terrain/brown_mud_leaves_01_arm_2k.png"   // sand has no arm map; reuse the matte mud arm
};

// the two camera-facing grass blade atlases. rgba with a green alpha-cutout key in the
// .g channel, matching the unity shader the grass draw is ported from.
static const char* const grassPaths[2] = {
    "Assets/Textures/Grass0.png",
    "Assets/Textures/Grass1.png"
};

#define TERRAIN_GRASS_SIZE 512

static Texture TerrainLoadGrassArray(void)
{
    return TerrainLoadTextureArray(grassPaths, 2u, TERRAIN_GRASS_SIZE, true, "TerrainGrass", "grass");
}

static void TerrainInitTextures(void)
{
    u64 start = SDL_GetTicks();
    g_Terrain.albedoLayers = TerrainLoadLayerArray(albedoPaths, TERRAIN_LAYER_COUNT, TERRAIN_ALBEDO_SIZE, true, "TerrainAlbedo");
    g_Terrain.normalLayers = TerrainLoadLayerArray(normalPaths, TERRAIN_LAYER_COUNT, TERRAIN_DETAIL_SIZE, false, "TerrainNormal");
    g_Terrain.armLayers    = TerrainLoadLayerArray(metallicRoughnessPaths, TERRAIN_LAYER_COUNT, TERRAIN_DETAIL_SIZE, false, "TerrainARM");
    g_Terrain.grassLayers  = TerrainLoadGrassArray();
    AX_LOG("terrain textures loaded in %llu ms (png decode, consider baking)", (unsigned long long)(SDL_GetTicks() - start));
}

// the three layers the engine loads into the terrain texture arrays, see
// TerrainInitTextures. extra slots stay disabled until custom layer loading lands
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

    authoring->grassDensity  = 1.0f;
    authoring->grassScaleMin = 0.6f;
    authoring->grassScaleMax = 1.2f;
    authoring->grassViewDistance = 300.0f;
    CopyString(authoring->grassColorHex, sizeof(authoring->grassColorHex), "77AA55FF");
}

TerrainAuthoring* Terrain_GetAuthoring(void)
{
    return &g_Terrain.authoring;
}

void Terrain_Init(void)
{
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (tvPortInitialized) return;
    AX_LOG("old terrain runtime disabled; using transvoxel unity example");
    g_Terrain.genParams = Terrain_DefaultGenParams();
    TerrainAuthoringDefaults(&g_Terrain.authoring);
    TerrainDensity_SetParams(&g_Terrain.genParams);
    TerrainEdit_Init();
    tvPortInitialized = true;
    return;
#endif
    if (g_Terrain.initialized) return;
    SDL_memset(&g_Terrain, 0, sizeof(g_Terrain));
    g_Terrain.genParams = Terrain_DefaultGenParams();
    TerrainAuthoringDefaults(&g_Terrain.authoring);
    TerrainDensity_SetParams(&g_Terrain.genParams);
    TerrainEdit_Init();
    // Transvoxel_SelfTest();

    g_Terrain.chunks    = (TerrainChunk*)AllocZeroTLSFGlobal(TERRAIN_MAX_CHUNKS, sizeof(TerrainChunk));
    g_Terrain.freeSlots = (u32*)AllocateTLSFGlobal(TERRAIN_MAX_CHUNKS * sizeof(u32));
    g_Terrain.pending   = (u32*)AllocateTLSFGlobal(TERRAIN_PENDING_CAP * sizeof(u32));
    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
        g_Terrain.freeSlots[i] = TERRAIN_MAX_CHUNKS - 1u - i;
    g_Terrain.numFreeSlots = TERRAIN_MAX_CHUNKS;
    g_Terrain.chunkMap = HMCreate(TERRAIN_MAX_CHUNKS, sizeof(u32));

    g_Terrain.grassTiles    = (GrassTile*)AllocZeroTLSFGlobal(TERRAIN_MAX_GRASS_TILES, sizeof(GrassTile));
    g_Terrain.grassTileFree = (u32*)AllocateTLSFGlobal(TERRAIN_MAX_GRASS_TILES * sizeof(u32));
    for (u32 i = 0; i < TERRAIN_MAX_GRASS_TILES; i++)
        g_Terrain.grassTileFree[i] = TERRAIN_MAX_GRASS_TILES - 1u - i;
    g_Terrain.numGrassTileFree = TERRAIN_MAX_GRASS_TILES;
    g_Terrain.grassTileMap = HMCreate(TERRAIN_MAX_GRASS_TILES, sizeof(u32));

    for (u32 i = 0; i < TERRAIN_MAX_JOBS; i++)
    {
        TerrainJob* job = &g_Terrain.jobs[i];
        job->density = (s8*)SDL_malloc(TERRAIN_SAMPLES_TOTAL);
        Transvoxel_ScratchInit(&job->scratch);
        SDL_SetAtomicInt(&job->state, JobState_Free);
    }

    for (u32 i = 0; i < TERRAIN_MAX_GRASS_JOBS; i++)
    {
        GrassJob* job = &g_Terrain.grassJobs[i];
        job->out = (GrassInstance*)SDL_malloc(GRASS_PER_CHUNK * sizeof(GrassInstance));
        SDL_SetAtomicInt(&job->state, JobState_Free);
    }

    // persistent job pool, sized below full machine capacity so terrain meshing does
    // not starve the rest of the engine.
    s32 cores = SDL_GetNumLogicalCPUCores();
    u32 terrainWorkers = (u32)Clamps32(cores - 2, 2, 8);
    g_Terrain.jobSystem = JobSystem_Create(terrainWorkers, TERRAIN_MAX_JOBS);
    CHECK_CREATE(g_Terrain.jobSystem, "Terrain Job System");

    g_Terrain.vertexBuffer   = CreateBuffer(NULL, TERRAIN_MAX_VERTICES * sizeof(TerrainVertex), BVertexBit, "TerrainVertexBuffer");
    g_Terrain.indexBuffer    = CreateBuffer(NULL, TERRAIN_MAX_INDICES * sizeof(u32), SDL_GPU_BUFFERUSAGE_INDEX, "TerrainIndexBuffer");
    g_Terrain.transferBuffer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = TERRAIN_TRANSFER_BYTES
    });
    CHECK_CREATE(g_Terrain.transferBuffer, "Terrain Transfer Buffer");

    // grass: instance data fed as an instance-rate vertex buffer, per-chunk origins as a
    // storage buffer, and one indirect draw command per visible chunk rebuilt each frame
    g_Terrain.grassBuffer        = CreateBuffer(NULL, TERRAIN_MAX_GRASS * sizeof(GrassInstance), BVertexBit, "TerrainGrassBuffer");
    g_Terrain.grassChunkBuffer   = CreateBuffer(NULL, TERRAIN_MAX_CHUNKS * sizeof(GrassChunkInfo), BReadRasterBit, "TerrainGrassChunkBuffer");
    g_Terrain.grassIndirectBuffer = CreateBuffer(NULL, TERRAIN_MAX_CHUNKS * sizeof(SDL_GPUIndirectDrawCommand), SDL_GPU_BUFFERUSAGE_INDIRECT, "TerrainGrassIndirect");
    g_Terrain.grassIndirectTransfer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = TERRAIN_MAX_CHUNKS * (u32)sizeof(SDL_GPUIndirectDrawCommand)
    });
    CHECK_CREATE(g_Terrain.grassIndirectTransfer, "Terrain Grass Indirect Transfer");
    g_Terrain.grassUploadTransfer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = GRASS_UPLOAD_BYTES
    });
    CHECK_CREATE(g_Terrain.grassUploadTransfer, "Terrain Grass Upload Transfer");

    TerrainInitPipelines();
    TerrainInitTextures();

    g_Terrain.enabled = false;
    g_Terrain.initialized = true;
}

void Terrain_Destroy(void)
{
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (!tvPortInitialized) return;
    if (tvGrassReady)
    {
        JobSystem_Wait(g_Terrain.jobSystem);
        JobSystem_Destroy(g_Terrain.jobSystem);
        for (u32 i = 0; i < TERRAIN_MAX_GRASS_JOBS; i++)
            SDL_free(g_Terrain.grassJobs[i].out);
        for (u32 i = 0; i < TERRAIN_MAX_GRASS_TILES; i++)
        {
            g_Terrain.grassTiles[i].building = 0u; // jobs drained, release every range
            TerrainGrassTileFree(i);
        }
        if (g_Terrain.grassPipeline)   SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_Terrain.grassPipeline);
        if (g_Terrain.forwardPipeline) SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_Terrain.forwardPipeline);
        if (g_Terrain.depthPipeline)   SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_Terrain.depthPipeline);
        if (g_Terrain.wirePipeline)    SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_Terrain.wirePipeline);
        if (g_Terrain.grassBuffer)         SDL_ReleaseGPUBuffer(g_GPUDevice, g_Terrain.grassBuffer);
        if (g_Terrain.grassChunkBuffer)    SDL_ReleaseGPUBuffer(g_GPUDevice, g_Terrain.grassChunkBuffer);
        if (g_Terrain.grassIndirectBuffer) SDL_ReleaseGPUBuffer(g_GPUDevice, g_Terrain.grassIndirectBuffer);
        if (g_Terrain.grassIndirectTransfer) SDL_ReleaseGPUTransferBuffer(g_GPUDevice, g_Terrain.grassIndirectTransfer);
        if (g_Terrain.grassUploadTransfer)   SDL_ReleaseGPUTransferBuffer(g_GPUDevice, g_Terrain.grassUploadTransfer);
        ReleaseTexture(&g_Terrain.grassLayers);
        HMDestroy(&g_Terrain.grassTileMap);
        DeAllocateTLSFGlobal(g_Terrain.grassTiles);
        DeAllocateTLSFGlobal(g_Terrain.grassTileFree);
        tvGrassReady = false;
    }
    TerrainEdit_Destroy();
    tvPortInitialized = false;
    tvWorldEnabled = false;
    return;
#endif
    if (!g_Terrain.initialized) return;

    // stop the pool after in-flight jobs observe cancellation and finish
    for (u32 i = 0; i < TERRAIN_MAX_JOBS; i++)
        SDL_SetAtomicInt(&g_Terrain.jobs[i].cancel, 1);
    JobSystem_Wait(g_Terrain.jobSystem);
    JobSystem_Destroy(g_Terrain.jobSystem);

    for (u32 i = 0; i < TERRAIN_MAX_JOBS; i++)
    {
        TerrainJob* job = &g_Terrain.jobs[i];
        SDL_free(job->density);
        Transvoxel_ScratchDestroy(&job->scratch);
        Transvoxel_MeshOutDestroy(&job->mesh);
    }
    for (u32 i = 0; i < TERRAIN_MAX_GRASS_JOBS; i++)
        SDL_free(g_Terrain.grassJobs[i].out);

    if (g_Terrain.forwardPipeline) SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_Terrain.forwardPipeline);
    if (g_Terrain.depthPipeline)   SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_Terrain.depthPipeline);
    if (g_Terrain.wirePipeline)    SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_Terrain.wirePipeline);
    if (g_Terrain.grassPipeline)   SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_Terrain.grassPipeline);
    if (g_Terrain.vertexBuffer)    SDL_ReleaseGPUBuffer(g_GPUDevice, g_Terrain.vertexBuffer);
    if (g_Terrain.indexBuffer)     SDL_ReleaseGPUBuffer(g_GPUDevice, g_Terrain.indexBuffer);
    if (g_Terrain.grassBuffer)         SDL_ReleaseGPUBuffer(g_GPUDevice, g_Terrain.grassBuffer);
    if (g_Terrain.grassChunkBuffer)    SDL_ReleaseGPUBuffer(g_GPUDevice, g_Terrain.grassChunkBuffer);
    if (g_Terrain.grassIndirectBuffer) SDL_ReleaseGPUBuffer(g_GPUDevice, g_Terrain.grassIndirectBuffer);
    if (g_Terrain.transferBuffer)  SDL_ReleaseGPUTransferBuffer(g_GPUDevice, g_Terrain.transferBuffer);
    if (g_Terrain.grassUploadTransfer)   SDL_ReleaseGPUTransferBuffer(g_GPUDevice, g_Terrain.grassUploadTransfer);
    if (g_Terrain.grassIndirectTransfer) SDL_ReleaseGPUTransferBuffer(g_GPUDevice, g_Terrain.grassIndirectTransfer);
    ReleaseTexture(&g_Terrain.albedoLayers);
    ReleaseTexture(&g_Terrain.normalLayers);
    ReleaseTexture(&g_Terrain.armLayers);
    ReleaseTexture(&g_Terrain.grassLayers);

    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
        TerrainChunkFreeMesh(i);
    // jobs are drained above, so no tile is really building anymore; clear the flag so
    // TerrainGrassTileFree releases every heap range instead of deferring it
    for (u32 i = 0; i < TERRAIN_MAX_GRASS_TILES; i++)
    {
        g_Terrain.grassTiles[i].building = 0u;
        TerrainGrassTileFree(i);
    }

    HMDestroy(&g_Terrain.chunkMap);
    HMDestroy(&g_Terrain.grassTileMap);
    DeAllocateTLSFGlobal(g_Terrain.chunks);
    DeAllocateTLSFGlobal(g_Terrain.freeSlots);
    DeAllocateTLSFGlobal(g_Terrain.pending);
    DeAllocateTLSFGlobal(g_Terrain.grassTiles);
    DeAllocateTLSFGlobal(g_Terrain.grassTileFree);
    TerrainEdit_Destroy();
    SDL_memset(&g_Terrain, 0, sizeof(g_Terrain));
}

// ---------------------------------------------------------------------------------
// per frame update
// ---------------------------------------------------------------------------------

void Terrain_Update(const Camera* camera)
{
    (void)camera;
#if TERRAIN_OLD_RUNTIME_DISABLED
    return;
#endif
    if (!g_Terrain.initialized || !g_Terrain.enabled) return;

    g_Terrain.cameraFrustum = CreateFrustumPlanesRevZ(M44Multiply(camera->view, camera->projection));
    g_Terrain.frustumValid = true;

    if (g_Terrain.genParams.fixedArea && !g_Terrain.fixedCenterValid)
    {
        g_Terrain.fixedCenter[0] = camera->position.x;
        g_Terrain.fixedCenter[1] = camera->position.z;
        g_Terrain.fixedCenterValid = true;
    }

    f32 size0 = TerrainChunkWorldSize(0);
    s32 cam[3] = {
        (s32)Floorf32(camera->position.x / size0),
        (s32)Floorf32(camera->position.y / size0),
        (s32)Floorf32(camera->position.z / size0)
    };
    bool camMoved = cam[0] != g_Terrain.lastCamChunk[0] || cam[2] != g_Terrain.lastCamChunk[2];
    if (g_Terrain.genParams.fixedArea) camMoved = false; // rings stay put
    if (!g_Terrain.desiredValid || camMoved
        || g_RenderSettings.terrainLodFactor != g_Terrain.lodFactor)
    {
        g_Terrain.lastCamChunk[0] = cam[0];
        g_Terrain.lastCamChunk[1] = cam[1];
        g_Terrain.lastCamChunk[2] = cam[2];
        g_Terrain.lodFactor = g_RenderSettings.terrainLodFactor;
        g_Terrain.desiredValid = true;
        TerrainComputeDesired(camera);
    }

    TerrainResolveRetiring();
    TerrainDispatchJobs(camera);
}

void Terrain_SetEnabled(bool enabled)
{
    (void)enabled;
#if TERRAIN_OLD_RUNTIME_DISABLED
    tvWorldEnabled = enabled;
    return;
#endif
    if (!g_Terrain.initialized) return;
    g_Terrain.enabled = enabled;
}

bool Terrain_GetEnabled(void)
{
#if TERRAIN_OLD_RUNTIME_DISABLED
    return tvWorldEnabled;
#endif
    return g_Terrain.initialized && g_Terrain.enabled;
}

// frees every resident chunk. in flight worker jobs are marked dying and release
// when their result is consumed, nothing of theirs uploads
static void TerrainEvictAll(void)
{
    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
        if (g_Terrain.chunks[i].used && !g_Terrain.chunks[i].dying)
            TerrainChunkEvict(i);
    // the surface changed underneath every grass tile; drop them so they rebuild fresh
    for (u32 i = 0; i < TERRAIN_MAX_GRASS_TILES; i++)
        TerrainGrassTileFree(i);
    g_Terrain.numPending = 0;
    g_Terrain.desiredValid = false;
}

void Terrain_ApplyGenParams(const TerrainGenParams* params)
{
    (void)params;
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (!tvPortInitialized) Terrain_Init();
    g_Terrain.genParams = *params;
    g_Terrain.genParams.fixedWorldSize = (u32)Clamps32((s32)g_Terrain.genParams.fixedWorldSize, TERRAIN_FIXED_WORLD_MIN_SIZE, TERRAIN_FIXED_WORLD_MAX_SIZE);
    TerrainDensity_SetParams(&g_Terrain.genParams);
    tTransvoxelExampleInvalidateAll();
    if (tvGrassReady)
    {
        // the surface changed everywhere; drop every grass tile so the ring rebuilds
        for (u32 i = 0; i < TERRAIN_MAX_GRASS_TILES; i++)
            TerrainGrassTileFree(i);
        g_Terrain.grassScanDirty = true;
    }
    return;
#endif
    if (!g_Terrain.initialized)
    {
        AX_WARN("Terrain_ApplyGenParams called without terrain instance");
        return;
    }
    g_Terrain.genParams = *params;
    g_Terrain.genParams.fixedWorldSize = (u32)Clamps32((s32)g_Terrain.genParams.fixedWorldSize, TERRAIN_FIXED_WORLD_MIN_SIZE, TERRAIN_FIXED_WORLD_MAX_SIZE);
    g_Terrain.fixedCenterValid = false; // recapture at the next update
    // jobs sampling while the params swap produce torn results, but every live and
    // generating chunk is evicted right after, so nothing stale survives
    TerrainDensity_SetParams(&g_Terrain.genParams);
    TerrainEvictAll();
}

const TerrainGenParams* Terrain_GetGenParams(void)
{
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (!tvPortInitialized) Terrain_Init();
    return &g_Terrain.genParams;
#endif
    static TerrainGenParams defaultParams;
    static bool defaultValid;
    if (!g_Terrain.initialized)
    {
        if (!defaultValid)
        {
            defaultParams = Terrain_DefaultGenParams();
            defaultValid = true;
        }
        return &defaultParams;
    }
    return &g_Terrain.genParams;
}

void Terrain_CreateWorld(const TerrainGenParams* params)
{
    (void)params;
#if TERRAIN_OLD_RUNTIME_DISABLED
    Terrain_ApplyGenParams(params);
    tvWorldEnabled = true;
    return;
#endif
    if (!g_Terrain.initialized) return;
    Terrain_ApplyGenParams(params);
    g_Terrain.enabled = true;
}

void Terrain_DeleteWorld(void)
{
#if TERRAIN_OLD_RUNTIME_DISABLED
    tvWorldEnabled = false;
    TerrainEdit_Clear();
    tTransvoxelExampleInvalidateAll();
    return;
#endif
    if (!g_Terrain.initialized) return;
    g_Terrain.enabled = false;
    g_Terrain.brushRadius = 0.0f;
    TerrainEvictAll();
    TerrainEdit_Clear();
}

// ---------------------------------------------------------------------------------
// editor brush
// ---------------------------------------------------------------------------------

void Terrain_SetBrushCursor(float3 position, f32 radius, bool active)
{
    (void)position;
    (void)radius;
    (void)active;
#if TERRAIN_OLD_RUNTIME_DISABLED
    tTransvoxelExampleSetBrushCursor(position, radius, active);
    return;
#endif
    if (!g_Terrain.initialized) return;
    g_Terrain.brushPos = position;
    g_Terrain.brushRadius = active ? radius : 0.0f;
}

// queues a remesh for every chunk whose sample grid can see the edited region.
// the pad ring feeds gradients, so the box grows by two voxels of each chunk's lod
static void TerrainRemeshRegion(float3 mn, float3 mx)
{
    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
    {
        TerrainChunk* chunk = &g_Terrain.chunks[i];
        if (!chunk->used || chunk->dying || chunk->retiring) continue;
        f32 size = TerrainChunkWorldSize(chunk->lod);
        f32 pad = 2.0f * TERRAIN_VOXEL_SIZE * (f32)(1u << chunk->lod);
        f32 ox = (f32)chunk->x * size, oy = (f32)chunk->y * size, oz = (f32)chunk->z * size;
        if (mx.x < ox - pad || mn.x > ox + size + pad ||
            mx.y < oy - pad || mn.y > oy + size + pad ||
            mx.z < oz - pad || mn.z > oz + size + pad) continue;
        if (chunk->needRemesh) continue;
        chunk->needRemesh = 1;
        // queued chunks are already pending, generating ones requeue at consume
        if (chunk->jobSlot < 0 && chunk->state == ChunkState_Live)
            TerrainPendingPush(i);
    }
}

void Terrain_SculptSphere(float3 center, f32 radius, f32 strength, f32 softness)
{
    (void)center;
    (void)radius;
    (void)strength;
    (void)softness;
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (!tvWorldEnabled) return;
    float3 tvMn, tvMx;
    TerrainEdit_SculptSphere(center, radius, strength, softness, &tvMn, &tvMx);
    tTransvoxelExampleInvalidateRegion(tvMn, tvMx);
    if (tvGrassReady)
    {
        // grass sits on the sculpted surface; dirty the touched columns so they rescatter
        f32 size0 = TerrainChunkWorldSize(0);
        s32 cx0 = (s32)Floorf32(tvMn.x / size0), cx1 = (s32)Floorf32(tvMx.x / size0);
        s32 cz0 = (s32)Floorf32(tvMn.z / size0), cz1 = (s32)Floorf32(tvMx.z / size0);
        for (s32 cz = cz0; cz <= cz1; cz++)
        {
            for (s32 cx = cx0; cx <= cx1; cx++)
            {
                GrassTile* tile = TerrainGrassTileFind(cx, cz);
                if (tile && !tile->dying)
                {
                    tile->dirty = 1u;
                    g_Terrain.grassScanDirty = true;
                }
            }
        }
    }
    return;
#endif
    if (!g_Terrain.initialized || !g_Terrain.enabled) return;
    float3 mn, mx;
    TerrainEdit_SculptSphere(center, radius, strength, softness, &mn, &mx);
    TerrainRemeshRegion(mn, mx);
}

void Terrain_PaintSphere(float3 center, f32 radius, u32 layer, f32 strength, f32 softness)
{
    (void)center;
    (void)radius;
    (void)layer;
    (void)strength;
    (void)softness;
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (!tvWorldEnabled) return;
    float3 tvMn, tvMx;
    TerrainEdit_PaintSphere(center, radius, (u8)Clamps32((s32)layer + 1, 1, 15), strength, softness, &tvMn, &tvMx);
    tTransvoxelExampleInvalidateRegion(tvMn, tvMx);
    return;
#endif
    if (!g_Terrain.initialized || !g_Terrain.enabled) return;
    float3 mn, mx;
    TerrainEdit_PaintSphere(center, radius, (u8)Clamps32((s32)layer + 1, 1, 15), strength, softness, &mn, &mx);
    TerrainRemeshRegion(mn, mx);
}

// sphere-traces the density field (noise + sculpt edits) instead of the meshes, so
// it works at any distance regardless of which lods keep cpu copies. the hit is the
// analytic surface, within ~half a coarse cell of the rendered mesh
s32 Terrain_RaycastField(float3 origin, float3 dir, f32 maxDist, BVHHit* hit)
{
    (void)origin;
    (void)dir;
    (void)maxDist;
    (void)hit;
#if TERRAIN_OLD_RUNTIME_DISABLED
    // pure field march over TerrainDensity_At; works without the old runtime
    if (!tvWorldEnabled) return 0;
#else
    if (!g_Terrain.initialized || !g_Terrain.enabled) return 0;
#endif

    f32 t = 0.0f, lastT = 0.0f;
    for (u32 step = 0; step < 256u && t < maxDist; step++)
    {
        f32 px = origin.x + dir.x * t, py = origin.y + dir.y * t, pz = origin.z + dir.z * t;
        f32 sdf = TerrainDensity_At(px, py, pz);
        if (sdf < 0.0f)
        {
            // bisect between the last outside sample and this inside one
            f32 lo = lastT, hi = t;
            for (u32 i = 0; i < 16u; i++)
            {
                f32 mid = (lo + hi) * 0.5f;
                f32 mx = origin.x + dir.x * mid, my = origin.y + dir.y * mid, mz = origin.z + dir.z * mid;
                f32 d = TerrainDensity_At(mx, my, mz);
                if (d < 0.0f) hi = mid; else lo = mid;
            }
            hit->hit.t = lo;
            hit->hit.u = hit->hit.v = 0.0f;
            hit->triIndex   = 0u;
            hit->entityIdx  = 0xFFFFFFFFu;
            hit->groupIdx   = 0u;
            hit->skinnedSet = 0xFFFFFFFFu;
            hit->bundleIdx  = 0xFFFFFFFFu;
            return 1;
        }
        lastT = t;
        // the field is not a true distance (heightfield slopes exceed 1), step conservatively
        t += Maxf32(sdf * 0.5f, 0.3f);
    }
    return 0;
}

u32 Terrain_NumEditedRegions(void)
{
#if TERRAIN_OLD_RUNTIME_DISABLED
    return TerrainEdit_NumChunks();
#endif
    return g_Terrain.initialized ? TerrainEdit_NumChunks() : 0u;
}

bool Terrain_SaveEditChunks(const char* path)
{
    (void)path;
#if TERRAIN_OLD_RUNTIME_DISABLED
    return tvPortInitialized && TerrainEdit_SaveChunks(path);
#endif
    return g_Terrain.initialized && TerrainEdit_SaveChunks(path);
}

bool Terrain_LoadEditChunks(const char* path)
{
    (void)path;
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (!tvPortInitialized) Terrain_Init();
    return tvPortInitialized && TerrainEdit_LoadChunks(path);
#endif
    if (!g_Terrain.initialized) Terrain_Init();
    return g_Terrain.initialized && TerrainEdit_LoadChunks(path);
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
	ChangeExtension(dst, len, "chunks");
    return true;
}

bool Terrain_SaveWorld(const char* path)
{
    (void)path;
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (!path || !path[0] || !tvPortInitialized || !tvWorldEnabled) return false;
#else
    if (!path || !path[0] || !g_Terrain.initialized || !g_Terrain.enabled) return false;
#endif
    EnsurePath(path);

    char* text = (char*)SDL_malloc(4096u);
    if (!text) return false;

    TerrainGenParams* params = &g_Terrain.genParams;
    TerrainAuthoring* authoring = &g_Terrain.authoring;
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
    (void)path;
#if TERRAIN_OLD_RUNTIME_DISABLED
    if (!path || !path[0]) return false;
    if (!tvPortInitialized) Terrain_Init();
    if (!tvPortInitialized) return false;
#else
    if (!path || !path[0]) return false;
    if (!g_Terrain.initialized) Terrain_Init();
    if (!g_Terrain.initialized) return false;
#endif

    char* text = ReadAllFileAlloc(path);
    if (!text) return false;

    TerrainGenParams params = Terrain_DefaultGenParams();
    TerrainAuthoring* authoring = &g_Terrain.authoring;
    TerrainAuthoringDefaults(authoring);
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
        else if (TerrainKeyIs(line, "island", &value))           params.island = value[0] == '1';
        else if (TerrainKeyIs(line, "seed", &value))             { f32 f; ParseFloat(value, &f); params.seed = (u32)f; }
        else if (TerrainKeyIs(line, "sea_level", &value))        ParseFloat(value, &params.seaLevel);
        else if (TerrainKeyIs(line, "base_height", &value))      ParseFloat(value, &params.baseHeight);
        else if (TerrainKeyIs(line, "hill_amplitude", &value))   ParseFloat(value, &params.hillAmplitude);
        else if (TerrainKeyIs(line, "hill_frequency", &value))   ParseFloat(value, &params.hillFrequency);
        else if (TerrainKeyIs(line, "ridge_amplitude", &value))  ParseFloat(value, &params.ridgeAmplitude);
        else if (TerrainKeyIs(line, "ridge_frequency", &value))  ParseFloat(value, &params.ridgeFrequency);
        else if (TerrainKeyIs(line, "cave_amplitude", &value))   ParseFloat(value, &params.carveAmplitude);
        else if (TerrainKeyIs(line, "cave_frequency", &value))   ParseFloat(value, &params.carveFrequency);
        else if (TerrainKeyIs(line, "island_radius", &value))    ParseFloat(value, &params.islandRadius);
        else if (TerrainKeyIs(line, "island_falloff", &value))   ParseFloat(value, &params.islandFalloff);
        else if (TerrainKeyIs(line, "grass_density", &value))       ParseFloat(value, &authoring->grassDensity);
        else if (TerrainKeyIs(line, "grass_view_distance", &value)) ParseFloat(value, &authoring->grassViewDistance);

        line = hadNewline ? next + 1 : NULL;
    }
    FreeAllText(text);

    char chunksPath[512];
    if (!TerrainChunksPathFromWorld(path, chunksPath, sizeof(chunksPath))) return false;
    if (FileExist(chunksPath) && !Terrain_LoadEditChunks(chunksPath)) return false;
    Terrain_CreateWorld(&params);
    return true;
}

TerrainStats Terrain_GetStats(void)
{
    TerrainStats stats = {0};
#if TERRAIN_OLD_RUNTIME_DISABLED
    return stats;
#endif
    if (!g_Terrain.initialized) return stats;
    for (u32 i = 0; i < TERRAIN_MAX_CHUNKS; i++)
    {
        TerrainChunk* chunk = &g_Terrain.chunks[i];
        if (!chunk->used) continue;
        if (chunk->state == ChunkState_Live) { stats.liveChunks++; if (chunk->empty) stats.emptyChunks++; }
        else stats.queuedChunks++;
    }
    for (u32 i = 0; i < TERRAIN_MAX_JOBS; i++)
    {
        s32 jobState = SDL_GetAtomicInt((SDL_AtomicInt*)&g_Terrain.jobs[i].state);
        if (jobState == JobState_Running) stats.jobsInFlight++;
    }
    stats.drawnChunks = g_Terrain.drawnLastFrame;
    stats.numVertices = g_Terrain.numAllocatedVertices;
    stats.numIndices  = g_Terrain.numAllocatedIndices;
    return stats;
}
