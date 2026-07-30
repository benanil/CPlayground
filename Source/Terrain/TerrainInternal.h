#ifndef TERRAIN_INTERNAL_H
#define TERRAIN_INTERNAL_H

#include "Include/Graphics.h"
#include "Include/Memory.h"
#include "Include/JobSystem.h"

#define GRASS_PER_METER        4
#define GRASS_PER_ROW          (GRASS_PER_METER * T_CHUNK_CELLS)     // 64 blades per chunk axis
#define GRASS_PER_CHUNK        (GRASS_PER_ROW * GRASS_PER_ROW)            // 4096, per grass column cap
// element COUNT (not bytes) of the shared grass instance heap. only near lod0 chunks
// grow grass, so a fraction of MAX_CHUNKS * GRASS_PER_CHUNK is plenty. ~1M * 8B = 8 MB
#define T_MAX_GRASS      (1u << 20)
#define T_GRASS_MAX_SLOTS (T_MAX_GRASS / GRASS_PER_CHUNK)
#define T_GRASS_MAX_DRAWS 4096u

// chunk grid: 16^3 cells, 17^3 cell corners, one extra sample on every side for
// central difference gradients -> 19^3 samples. sample (x,y,z) maps to corner (x-1,y-1,z-1)
#define T_CHUNK_CELLS    16
#define T_SAMPLES_AXIS   19
#define T_VOXEL_SIZE     1.0f   // meters per cell at lod 0
#define T_SAMPLES_TOTAL  (T_SAMPLES_AXIS * T_SAMPLES_AXIS * T_SAMPLES_AXIS)

// world y of the solid bedrock floor. The visible floor surface is half a voxel above
// this so integer lattice samples at the protected bottom stay solid instead of zero.
#define TERRAIN_BEDROCK_Y      (-60.0f)
#define TERRAIN_BEDROCK_SURFACE_Y (TERRAIN_BEDROCK_Y + 0.5f)

#define T_LOD_COUNT                   4u
#define T_MAX_CHUNKS                 (16384u*2)
#define T_CHUNK_BITSET_WORDS        ((T_MAX_CHUNKS + 63ull) / 64ull)
#define T_CHUNK_CELLS_PER_VOLUME (T_CHUNK_CELLS * T_CHUNK_CELLS * T_CHUNK_CELLS)
// Regular cells: max 12 edge vertices/cell before sharing. Shared-grid edge count is lower,
// but this keeps cap simple and safe. Transition slots add more.
#define T_CHUNK_VERTEX_CAP (T_CHUNK_CELLS_PER_VOLUME * 8u)
// MC regular cells can emit up to 5 triangles/cell. Transitions add some slack.
#define T_CHUNK_INDEX_CAP  (T_CHUNK_CELLS_PER_VOLUME * 24u)
// Non-indexed Marching Cubes needs one vertex for every triangle index.
#define T_MARCHING_VERTEX_CAP (T_CHUNK_INDEX_CAP * 2u / 3u)
#define T_CHUNK_SECONDARY_VERTEX_CAP (2048u)
#define T_MAX_BUILDS_PER_FRAME        32u
// bump buffer for buildVertices + buildIndices + mesher edgeCache; scales ~cubically
// with T_CHUNK_CELLS. undersizing spills to the slow TLSF fallback (AX_WARN "arena spilled")
#define T_BUILD_SCRATCH_SIZE         (4ull * 1024ull * 1024ull)
#define T_MAX_PHYSICS_SYNCS_PER_FRAME 2u
// max concurrent chunk builds (job system thread pool auto-sizes to core count, but only
// this many can be in-flight); too small causes bursty main-thread integration -> stutter
#define T_MAX_BUILD_JOBS 16u
#define T_ENABLE_STATS 0

// these back both the CPU mirrors (Graphics.c) and the GPU mirror (Rendering.c) - keep sane.
// bigger T_CHUNK_CELLS means fewer resident chunks short-circuit as fully empty
// (each spans more height), so real occupancy per view volume goes up - rescale
// this budget when T_CHUNK_CELLS changes, don't just watch the eviction warning.
#define T_MAX_VERTICES        (21u << 20) // 252MB at 12B/vertex
// indices are chunk-relative u16 (per-chunk vertex count is capped at
// T_MARCHING_VERTEX_CAP == 65536); sized ~5x T_MAX_VERTICES for the real indices:vertices
// ratio of an indexed mesh, or the index heap exhausts first and silently caps vertex use
#define T_MAX_INDICES          (105u << 20) // 210MB at 2B/index, 5x T_MAX_VERTICES
#define T_VERTEX_CACHE_BUDGET (T_MAX_VERTICES * 7u / 8u)
#define T_VERTEX_CACHE_TARGET (T_MAX_VERTICES * 3u / 4u)
#define T_INDEX_CACHE_BUDGET  (T_MAX_INDICES * 7u / 8u)
#define T_INDEX_CACHE_TARGET  (T_MAX_INDICES * 3u / 4u)
#define T_HEAP_PRESSURE_EVICT_CHUNKS 32u
#define T_CACHE_KEEP_FRAMES 2u

// densities are clamped signed distance in meters, negative inside the solid, scaled so
// that +-TERRAIN_SDF_CLAMP maps to +-127. the scale is world fixed (NOT per lod): the
// transvoxel transition cells are only crack free when a coarse sample equals the fine
// sample at the same world position, which per lod scaling would break
#define TERRAIN_SDF_CLAMP      4.0f

// chunk density array storage: source samples already are s8 (+-127), so the mesher
// array stores them unchanged (1 byte/sample) and decodes to meters on read
#define T_DENSITY_DECODE_SCALE (-(TERRAIN_SDF_CLAMP) / 127.0f)

#define T_LAYER_COUNT 4u

// one camera-facing grass blade, 8 bytes, fed to the grass draw as an instance-rate
// vertex attribute. positions are chunk-relative meters so a 16 m lod0 chunk keeps full
// fp16 precision; the shader adds chunks[chunkIndex].origin. per-blade scale/phase is
// derived procedurally from the world position, so no random needs storing.
typedef struct GrassInstance_
{
	u32 positionXY;         // fp16 x | fp16 y  (chunk-relative)
	u32 positionZChunkIndex; // fp16 z (low 16) | u16 terrain chunk slot index (high 16)
} GrassInstance;

typedef struct TerrainGrassChunkInfo_
{
    f32 origin[4];
} TerrainGrassChunkInfo;

typedef struct TerrainGrassChunk_
{
    u32 slot;
    u32 count;
    float3 origin;
    float3 aabbMin;
    float3 aabbMax;
} TerrainGrassChunk;

typedef struct TerrainFoliageState_
{
    SDL_GPUGraphicsPipeline* pipeline;
    SDL_GPUBuffer* grassBuffer;
    SDL_GPUBuffer* chunkBuffer;
    SDL_GPUBuffer* indirectBuffer;
    Texture grassLayers;
    FrustumPlanes frustum;
    SDL_GPUIndirectDrawCommand draws[T_GRASS_MAX_DRAWS];
    u16 freeSlots[T_GRASS_MAX_SLOTS];
    u32 drawCount;
    u32 freeCount;
    bool initialized;
} TerrainGrassState;

typedef struct MaterialBlend_
{
    u8 primary;
    u8 secondary;
    u8 primaryWeight;
} MaterialBlend;

typedef struct tVertexData_
{
    u32 position;  // u16 fixed x/y/z local to chunk, w unused
    u32 normal;    // packed normal/tangent (PackNormalTangent)
    u32 materials; 
} tVertex;

// 20 oct, 8 material index, 4 weight
typedef struct tMeshData_
{
    tVertex*  vertices;      // fixed-capacity ranges in the terrain geometry heaps
    u16*      indices;       // chunk-relative, values always < T_MARCHING_VERTEX_CAP
	s32 numIndices;
	s32 numVertices;
	s32 vertexCapacity;          // pushes beyond capacity are dropped (heaps are shared,
	s32 indexCapacity;           // an overrun would corrupt other chunks' meshes)
	s32 secondaryCapacity;
} tMeshData;

typedef struct GeometryRange_
{
    void* heapPtr;
    u32   first;
    u32   count;
} GeometryRange;

typedef struct PhysicsMesh_
{
    struct b3Vec3* vertices;
    s32*    indices;
    u32     vertexCount;
    u32     indexCount;
} PhysicsMesh;

typedef struct tMeshHandle_
{
    GeometryRange vertices;
    GeometryRange indices;
    PhysicsMesh   physics;
} tMeshHandle;

typedef enum ChunkBuildState_
{
    // Valid lifecycle transitions:
    // UNBUILT  -> QUEUED -> BUILDING -> PENDING -> READY
    // UNBUILT  -> BUILDING
    // BUILDING -> QUEUED    (job submit/drain failed; retry later)
    // BUILDING -> READY     (build produced an empty, presentable chunk)
    // BUILDING -> FAILED    (first build failed and no live/pending mesh exists)
    // READY    -> PENDING   (background rebuild finished; old mesh stays visible until promote)
    // FAILED   -> QUEUED/BUILDING
    // READY can also have a rebuild in flight without changing state; ChunkBuildInFlight
    // tracks that so the existing mesh/empty chunk remains presentable.
    CHUNK_UNBUILT,
    CHUNK_QUEUED,
    CHUNK_BUILDING,
    CHUNK_PENDING,
    CHUNK_READY,
    CHUNK_FAILED,
    CHUNK_MAX_STATE
} ChunkBuildState;

typedef enum PendingMeshState_
{
    PENDING_NONE,
    PENDING_MESH,
    PENDING_EMPTY
} PendingMeshState;

// one procedural foliage placement living in a render set entity. sparseIdx/packed
// address gFoliage.scene's render sets (packed = (groupIdx << 3) | renderSetIndex,
// renderSetIndex reserved for surface/skinned/transparent, only surface used today).
// physicsBody is a b3StoreBodyId() value in the active gameplay scene's physics world,
// 0 when the owning foliage type has no collider
typedef struct tFoliageEntity_
{
    u32 sparseIdx;
    u32 packed;
} tFoliageEntity;

typedef struct tChunk_
{
    int3   min;
    float3 aabbMin;
    float3 aabbMax;
    // chunk mesh lives in the tVertexData geometry heap; the GPU mirror draws it
    // without any per-frame copy
    tMeshHandle mesh;
    tMeshHandle pendingMesh;
    // cached 19^3 density grid backing the last successful build, TLSF-owned, T_SAMPLES_TOTAL
    // bytes, same [x][y][z] transposed layout tBuildDensity feeds the mesher (see
    // MarchingTerrain.c IntegrateFinishedBuilds). Foliage placement reads this directly
    // instead of resampling the field (noise + edit overlay), which is expensive.
    s8*   density;
    // procedural foliage instances for this chunk, TLSF-owned; rebuilt whenever a
    // foliage type's params change (see tFoliageType.paramsDirty in TerrainFoliage.c)
    tFoliageEntity* foliageEntities;
    u32   lastTouchedFrame;
    // intrusive residency LRU (MarchingTerrain.c), T_CHUNK_LRU_NONE-terminated; lets
    // eviction pick the true least-recently-touched chunk without scanning the array
    u32   lruPrev, lruNext;
    u16   foliageCount;
    // false until tFoliage_Update has scheduled at least one job for this chunk (zero
    // placements is a valid outcome and still sets this - it means "decided", not "has
    // foliage"). Lets newly streamed-in chunks get an initial placement pass even on a
    // frame where no foliage type's params changed; MemsetZero on chunk reuse resets it.
    bool  foliageBuilt;
    u8    pendingFrames;
    // terrain collider owned directly by this chunk (no shared slot pool/cap anymore).
    // physicsBody is a stored b3BodyId (b3StoreBodyId/b3LoadBodyId), 0 = none.
    u64   physicsBody;
    struct b3MeshData* physicsMesh;
    ChunkBuildState  buildState;
    PendingMeshState pendingState;
    bool  dirty;
    bool  physicsDirty;
} tChunk;

// resident chunk accessors for TerrainFoliage.c (MarchingTerrain.c owns the array;
// indices are NOT stable across evictions, callers must re-resolve by position after
// yielding a frame - tFindChunkByMin is the stable lookup)
u32     tGetChunkCount(void);
tChunk* tGetChunkByIndex(u32 index);
tChunk* tFindChunkByMin(int3 min);
JobSystem* tGetTerrainJobSystem(void);
// bit i set means chunks[i] is resident. T_CHUNK_BITSET_WORDS words, index space matches
// tGetChunkByIndex exactly (tAllocChunkSlot hands out the chunk's storage slot from this
// same bitset). Lets callers walk residents by scanning set bits instead of trusting
// chunkCount to stay a dense [0, count) prefix.
const u64* tGetOccupiedChunksBitset(void);

// called by MarchingTerrain.c before a chunk slot is wiped/reused (eviction, cache
// clear, shutdown): frees the chunk's render entities, colliders and instance array
void tFoliage_DestroyChunkFoliage(tChunk* chunk);
// per-frame: schedules foliage (re)builds for stale resident chunks and integrates
// finished worker jobs. called once from tUpdate
void tFoliage_Update(void);

// one in-flight chunk build on a JobSystem worker. the main thread fills the inputs,
// launches the job and reads the outputs after JobSystem_IsJobDone; exactly one job
// ever touches a chunk, so no locks on chunk state are needed
typedef struct tBuildJob_
{
    // inputs, main thread
    u32          chunkIndex;
    int3         min;
    // per-slot scratch, initialized once (ranges in the TerrainVertNew/Second/Index2 heaps)
    tMeshData    scratchMesh;
    ArenaScratch scratchArena;
    // worker-local append state, valid only while the job runs (thread scratch arena)
    tVertex* buildVertices;
    u32          buildVertexCount;
    u16*         buildIndices;    // chunk-relative, values always < T_MARCHING_VERTEX_CAP
    u32          buildIndexCount;
    // output, worker; mesh ranges are zero when empty or failed
    tMeshHandle  mesh;
    JobHandle    handle;
    bool         failed;
    bool         busy;
} tBuildJob;

typedef f32 (*tNoise2DFn)(f32 x, f32 z, void* userData);
typedef f32 (*tNoise3DFn)(f32 x, f32 y, f32 z, void* userData);

typedef struct tDensityGenerator_
{
    tNoise2DFn heightMapNoise;
    void*      heightMapUserData;
    tNoise3DFn noise3D;
    void*      noise3DUserData;
    f32        heightMapStrength;
    f32        noise3DStrength;
} tDensityGenerator;

extern const char* const tAlbedoPaths[T_LAYER_COUNT];
extern const char* const tNormalPaths[T_LAYER_COUNT];
extern const char* const tMetallicRoughnessPaths[T_LAYER_COUNT];

// tChunkPositionKey packs the position into bits 3..63
purefn u64 tChunkKey(int3 position)
{
	return ((u64)((u32)(position.x + 0x100000) & 0x1FFFFFu) << 40)
		| ((u64)((u32)(position.y + 0x8000)   & 0xFFFFu)   << 24)
		| ((u64)((u32)(position.z + 0x100000) & 0x1FFFFFu) << 3);
}

inline void tCoordsFromKey(u64 key, s32* x, s32* y, s32* z)
{
    *x = (s32)((key >> 40) & 0x1FFFFF) - 0x100000;
    *y = (s32)((key >> 24) & 0xFFFF) - 0x8000;
    *z = (s32)((key >> 3) & 0x1FFFFF) - 0x100000;
}

void TerrainInitMaterialTextures(void);
void tTerrainMaterial(float3 worldPos, float3 normal, u32* materials, u32* blend);

// procedural density field, pure and thread safe (TerrainDensity.c)
f32  TerrainDensity_SDF(f32 x, f32 y, f32 z);
void TerrainDensity_SampleChunk(s32 cx, s32 cy, s32 cz, s8* out /*19^3*/);
// world vertical band that can contain surface, chunks outside it are never created
void TerrainDensity_GetYRange(f32* outMin, f32* outMax);
// true when the chunk sits past the island falloff - no ocean yet, so callers skip
// sampling/meshing/slot allocation (and physics, since no mesh means no collider) entirely
bool TerrainDensity_ChunkOutsideIslandEmpty(int3 chunkMin);

// analytic column surface height (heightfield term, before the 3D carve). a good seed
// for the vertical surface march below.
f32  TerrainDensity_Height(f32 x, f32 z);
// island falloff mask 0..1: 0 on the island proper, ramping to 1 out on the open sea
// plane (always 0 when island mode is off). lets grass stay on the island, off the beach.
f32  TerrainDensity_IslandMask(f32 x, f32 z);
// signed field at a world point, sculpt edits included: > 0 air, < 0 solid, 0 = surface.
f32  TerrainDensity_At(f32 x, f32 y, f32 z);
// nearest surface world Y at column (x,z), marched from startY (Newton on the sdf, whose
// d/dy is ~1). outNormal, when non-null, gets the up-facing surface normal. lets grass and
// anything else sample the surface directly from the density field without a meshed chunk.
f32  TerrainDensity_SurfaceY(f32 x, f32 z, f32 startY, float3* outNormal);

struct TerrainGenParams_;
void TerrainDensity_SetParams(const struct TerrainGenParams_* params);
const struct TerrainGenParams_* TerrainDensity_GetParams(void);

bool tMeshDataInit(tMeshData* data);
void tMeshDataDestroy(tMeshData* data);
void tMeshDataClear(tMeshData* data);
bool tMeshDataPushVertex(tMeshData* data, tVertex vertex);
bool tMeshDataPushIndex(tMeshData* data, u32 index);
u16* tMeshDataBuildValidIndices(const tMeshData* data);
bool tMesherMesh(const tDensityGenerator* generator, const s8* density, tBuildJob* job);

// ---------------------------------------------------------------------------------
// sparse sculpt/paint edits (TerrainEdit.c): 16^3 grids of s8 density deltas and u8
// material indices on the world fixed voxel lattice, keyed by chunk coords.
// thread safe: workers overlay while the main thread sculpts, a mutex guards the map
// ---------------------------------------------------------------------------------

#define TERRAIN_EDIT_CELLS T_CHUNK_CELLS  // grid axis, == T_CHUNK_CELLS

void TerrainEdit_Init(void);
void TerrainEdit_Destroy(void);
void TerrainEdit_Clear(void);
u32  TerrainEdit_NumChunks(void);

// adds the quantized density deltas of every edited region intersecting the chunk's
// 19^3 sample grid, and clamps. called from worker jobs inside SampleChunk
void TerrainEdit_OverlayChunk(s32 cx, s32 cy, s32 cz, s8* samples);

// packed paint value of a voxel: layerA | layerB<<4 | blendWeight<<8.
// 0 = untouched procedural default
u16 TerrainEdit_MaterialAt(s32 wx, s32 wy, s32 wz);

// sculpted sdf offset in meters a voxel, 0 when untouched
f32 TerrainEdit_DeltaAt(s32 wx, s32 wy, s32 wz);

// splits the containing voxel's paint into the two TerrainVertex.spare slots:
// indices A/B plus weights with wA + wB == 255 (index 0 = procedural)
void TerrainEdit_MaterialWeights(float3 pos, u8 outIndex[2], u8 outWeight[2]);

// brush writes from the main thread, both return the touched world AABB so the
// caller can remesh intersecting chunks. strength in sdf meters per apply
void TerrainEdit_SculptSphere(float3 center, f32 radius, f32 strength, f32 softness,
                              float3* outMin, float3* outMax);
// strength is blend weight units (0..255) pushed into the voxels per apply
void TerrainEdit_PaintSphere(float3 center, f32 radius, u8 material, f32 strength, f32 softness,
                             float3* outMin, float3* outMax);

// persistence: binary .chunks files store one record per edited region, each with
// 16^3 s8 density deltas and 16^3 u16 packed material values.
bool TerrainEdit_SaveChunks(const char* path);
bool TerrainEdit_LoadChunks(const char* path);

// density-driven grass helpers. Transvoxel owns the state and chunk lifetime;
// this file owns only placement, GPU upload, draw submission, and shader binding.
u32  Terrain_BuildChunkGrass(int3 chunkMin, GrassInstance* out, float3* outMin, float3* outMax);
bool TerrainGrass_Init(TerrainGrassState* tf);
void TerrainGrass_BeginFrame(TerrainGrassState* tf, const FrustumPlanes* frustum);
bool TerrainGrass_UploadChunk(TerrainGrassState* tf, TerrainGrassChunk* chunk, const GrassInstance* src);
void TerrainGrass_FreeChunk(TerrainGrassState* tf, TerrainGrassChunk* chunk);
void TerrainGrass_AppendDraw(TerrainGrassState* tf, const TerrainGrassChunk* chunk);
void TerrainGrass_EndFrame(TerrainGrassState* tf);
void TerrainGrass_Clear(TerrainGrassState* tf);
void TerrainGrass_Destroy(TerrainGrassState* tf);

#endif // TERRAIN_INTERNAL_H
