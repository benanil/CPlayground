#ifndef TERRAIN_INTERNAL_H
#define TERRAIN_INTERNAL_H

#include "Include/Graphics.h"

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
#define T_SAMPLES_TOTAL  (19 * 19 * 19)

// world y of the solid bedrock floor. The visible floor surface is half a voxel above
// this so integer lattice samples at the protected bottom stay solid instead of zero.
#define TERRAIN_BEDROCK_Y      (-60.0f)
#define TERRAIN_BEDROCK_SURFACE_Y (TERRAIN_BEDROCK_Y + 0.5f)

#define T_LOD_COUNT                   4u
#define T_MAX_CHUNKS                 (16384u)
#define T_CHUNK_BITSET_WORDS        ((T_MAX_CHUNKS + 63ull) / 64ull)
#define T_CHUNK_CELLS_PER_VOLUME (T_CHUNK_CELLS * T_CHUNK_CELLS * T_CHUNK_CELLS)
// Regular cells: max 12 edge vertices/cell before sharing. Shared-grid edge count is lower,
// but this keeps cap simple and safe. Transition slots add more.
#define T_CHUNK_VERTEX_CAP (T_CHUNK_CELLS_PER_VOLUME * 8u)
// MC regular cells can emit up to 5 triangles/cell. Transitions add some slack.
#define T_CHUNK_INDEX_CAP  (T_CHUNK_CELLS_PER_VOLUME * 24u)
#define T_CHUNK_SECONDARY_VERTEX_CAP (2048u)
#define T_MAX_BUILDS_PER_FRAME        16u
#define T_BUILD_SCRATCH_SIZE         (3ull * 1024ull * 1024ull)
#define T_MAX_PHYSICS_SYNCS_PER_FRAME 2u
#define T_MAX_BUILD_JOBS 8u
#define T_ENABLE_STATS 0

// the transvoxel-unity port parks non-indexed triangle soup in the vertex heap
// (~3x an indexed mesh), so it is sized above the old runtime's needs. these back
// both the CPU mirrors (Graphics.c) and the GPU mirror (Rendering.c) - keep sane.
#define T_MAX_VERTICES        (2u << 22) // 128MB if vertex is 16B
#define T_MAX_INDICES         (2u << 22) // 32MB of u32
#define T_VERTEX_CACHE_BUDGET (T_MAX_VERTICES / 2u)
#define T_VERTEX_CACHE_TARGET (T_MAX_VERTICES / 3u)
#define T_INDEX_CACHE_BUDGET  (T_MAX_INDICES / 2u)
#define T_INDEX_CACHE_TARGET  (T_MAX_INDICES / 3u)
#define T_HEAP_PRESSURE_EVICT_CHUNKS 32u
#define T_CACHE_KEEP_FRAMES 2u

// densities are clamped signed distance in meters, negative inside the solid, scaled so
// that +-TERRAIN_SDF_CLAMP maps to +-127. the scale is world fixed (NOT per lod): the
// transvoxel transition cells are only crack free when a coarse sample equals the fine
// sample at the same world position, which per lod scaling would break
#define TERRAIN_SDF_CLAMP      4.0f

#define T_LAYER_COUNT 4u

// compact vertex, 16 bytes: 3 x 21 bit fixed point position in chunk bounds, the chunk
// origin and size come from a per draw uniform. normal is 16+16 bit octahedral
typedef struct TerrainVertex_
{
    u32 posA;      // x:21 | y low 11
    u32 posB;      // y high 10 | z:21 | 1 spare
    u32 octNormal; // octahedral x:16 | y:16 unorm
    u32 spare;     // future material weights / ao
} TerrainVertex;

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

extern const char* const tAlbedoPaths[T_LAYER_COUNT];
extern const char* const tNormalPaths[T_LAYER_COUNT];
extern const char* const tMetallicRoughnessPaths[T_LAYER_COUNT];
void TerrainInitMaterialTextures(void);
void tTerrainMaterial(float3 worldPos, float3 normal, u32* materials, u32* blend);

// logs mesher validation results against an analytic sphere, called once from Terrain_Init
void Transvoxel_SelfTest(void);

// procedural density field, pure and thread safe (TerrainDensity.c)
f32  TerrainDensity_SDF(f32 x, f32 y, f32 z);
void TerrainDensity_SampleChunk(s32 cx, s32 cy, s32 cz, u32 lod, s8* out /*19^3*/);
// world vertical band that can contain surface, chunks outside it are never created
void TerrainDensity_GetYRange(f32* outMin, f32* outMax);

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

// ---------------------------------------------------------------------------------
// sparse sculpt/paint edits (TerrainEdit.c): 16^3 grids of s8 density deltas and u8
// material indices on the world fixed lod0 voxel lattice, keyed by lod0 chunk coords.
// thread safe: workers overlay while the main thread sculpts, a mutex guards the map
// ---------------------------------------------------------------------------------

#define TERRAIN_EDIT_CELLS 16  // grid axis, == T_CHUNK_CELLS at lod 0

void TerrainEdit_Init(void);
void TerrainEdit_Destroy(void);
void TerrainEdit_Clear(void);
u32  TerrainEdit_NumChunks(void);

// adds the quantized density deltas of every edited region intersecting the chunk's
// 19^3 sample grid, and clamps. called from worker jobs inside SampleChunk
void TerrainEdit_OverlayChunk(s32 cx, s32 cy, s32 cz, u32 lod, s8* samples);

// packed paint value of a lod0 voxel: layerA | layerB<<4 | blendWeight<<8.
// 0 = untouched procedural default
u16 TerrainEdit_MaterialAt(s32 wx, s32 wy, s32 wz);

// sculpted sdf offset in meters at a lod0 voxel, 0 when untouched
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
void TerrainFoliage_AppendDraw(TerrainGrassState* tf, const TerrainGrassChunk* chunk);
void TerrainFoliage_EndFrame(TerrainGrassState* tf);
void TerrainGrass_Clear(TerrainGrassState* tf);
void TerrainGrass_Destroy(TerrainGrassState* tf);

#endif // TERRAIN_INTERNAL_H
