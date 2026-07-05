#ifndef TERRAIN_INTERNAL_H
#define TERRAIN_INTERNAL_H

#include "Include/Common.h"
#include "Math/Vector.h"

// chunk grid: 16^3 cells, 17^3 cell corners, one extra sample on every side for
// central difference gradients -> 19^3 samples. sample (x,y,z) maps to corner (x-1,y-1,z-1)
#define TERRAIN_CHUNK_CELLS    16
#define TERRAIN_CHUNK_CORNERS  17
#define TERRAIN_SAMPLES_AXIS   19
#define TERRAIN_SAMPLES_TOTAL  (19 * 19 * 19)

#define TERRAIN_VOXEL_SIZE     1.0f   // meters per cell at lod 0
#define TERRAIN_LOD_COUNT      4      // chunk world sizes 16/32/64/128 m
#define TERRAIN_MAX_VERTICES   (1u << 20) // 16 MB of TerrainVertex
#define TERRAIN_MAX_INDICES    (4u << 20) // 16 MB of u32

#define TERRAIN_MAX_CHUNKS     2048u
#define GRASS_PER_METER        4
#define GRASS_PER_ROW          (GRASS_PER_METER * TERRAIN_CHUNK_CELLS)     // 64 blades per chunk axis
#define GRASS_PER_CHUNK        (GRASS_PER_ROW * GRASS_PER_ROW)            // 4096, per grass column cap
// element COUNT (not bytes) of the shared grass instance heap. only near lod0 chunks
// grow grass, so a fraction of MAX_CHUNKS * GRASS_PER_CHUNK is plenty. ~1M * 8B = 8 MB
#define TERRAIN_MAX_GRASS      (1u << 20)

// densities are clamped signed distance in meters, negative inside the solid, scaled so
// that +-TERRAIN_SDF_CLAMP maps to +-127. the scale is world fixed (NOT per lod): the
// transvoxel transition cells are only crack free when a coarse sample equals the fine
// sample at the same world position, which per lod scaling would break
#define TERRAIN_SDF_CLAMP      4.0f

// transition faces shrink the boundary layer of regular cells inward by this fraction
// of a cell, the gap is filled by transition cells (Lengyel 4.4)
#define TERRAIN_TRANSITION_SHRINK 0.5f

// positions are fixed point with TERRAIN_POS_PER_CELL steps per cell. the mesher
// computes boundary vertices with pure integer math (corner * 32768 + t256 * 128 * edge),
// so a vertex shared by two chunks (same or neighboring lod) packs bit-identically on
// both sides and lod seams are exact, not just close
#define TERRAIN_POS_PER_CELL 32768
#define TERRAIN_POS_MAX      (TERRAIN_CHUNK_CELLS * TERRAIN_POS_PER_CELL) // 524288, needs 21 bits

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

typedef struct TerrainMeshOut_
{
    TerrainVertex* vertices;   // SDL_malloc, grown by the mesher
    u32*           indices;    // chunk local, rebased by the draw's vertex_offset
    u32 numVertices, vertexCapacity;
    u32 numIndices, indexCapacity;
    float3 aabbMin, aabbMax; // chunk local meters, tight bounds of emitted vertices
    // inputs the mesher fills from MeshChunk's arguments: world chunk origin in lod0
    // meters and the lod, used to look up painted materials per vertex
    s32 worldOrigin[3];
    u32 lod;
} TerrainMeshOut;

// per worker scratch for the vertex reuse maps, allocated once per job slot
typedef struct TerrainMeshScratch_
{
    u32* edgeVertex;   // [3][17][17][17] vertex index per axis aligned edge
    u32* cornerVertex; // [17][17][17] vertex index for crossings exactly on a corner
    u32* faceHash;     // open addressing table for transition cell vertices
} TerrainMeshScratch;

void Transvoxel_ScratchInit(TerrainMeshScratch* scratch);   // SDL_malloc, thread safe
void Transvoxel_ScratchDestroy(TerrainMeshScratch* scratch);
void Transvoxel_MeshOutDestroy(TerrainMeshOut* out);

// meshes one chunk from a 19^3 s8 density grid. transitionMask bit per face
// (-x,+x,-y,+y,-z,+z) marks a coarser neighbor on that face. cx/cy/cz are the chunk
// coords at this lod, used for painted material lookups. thread safe, allocates the
// output with SDL_malloc. out: 1 on success, 0 when out of memory
s32 Transvoxel_MeshChunk(const s8* density, u32 lod, u8 transitionMask,
                         s32 cx, s32 cy, s32 cz,
                         TerrainMeshOut* out, TerrainMeshScratch* scratch);

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

#define TERRAIN_EDIT_CELLS 16  // grid axis, == TERRAIN_CHUNK_CELLS at lod 0

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

#endif // TERRAIN_INTERNAL_H
