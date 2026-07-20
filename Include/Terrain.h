#ifndef TERRAIN_H
#define TERRAIN_H

#include "Common.h"
#include "Camera.h"
#include "BVH.h"
#include <SDL3/SDL_gpu.h>

#if defined(__cplusplus)
extern "C" {
#endif

// streamed voxel terrain meshed with the Transvoxel algorithm (https://transvoxel.org/).
// chunks generate on worker threads from a procedural density field and render through
// a dedicated pipeline, independent from the scene render sets

// procedural generation settings, the .terrain file stores these instead of meshes.
// chunks regenerate from them plus the sparse sculpt/paint edits
typedef struct TerrainGenParams_
{
    u32  seed;           // offsets the noise domain, 0 keeps the legacy field
    f32  seaLevel;       // world y reference for the island mask
    f32  baseHeight;
    f32  hillAmplitude,  hillFrequency;
    f32  ridgeAmplitude, ridgeFrequency;
    f32  carveAmplitude, carveFrequency;  // 3d term, gives overhangs and caves
    bool island;         // fade terrain to a flat sea-level plane outside the island radius
    f32  islandRadius, islandFalloff;
    bool fixedArea;      // freeze the chunk rings where they were created, no streaming
    u32  fixedWorldSize; // horizontal fixed terrain size in meters, clamped by TERRAIN_FIXED_WORLD_MAX_SIZE
} TerrainGenParams;

#define TERRAIN_FIXED_WORLD_MIN_SIZE     16u
#define TERRAIN_FIXED_WORLD_DEFAULT_SIZE 128u
#define TERRAIN_FIXED_WORLD_MAX_SIZE     512u

#define TERRAIN_MAX_LAYERS 8u

typedef struct TerrainLayerDesc_
{
    bool enabled;
    char albedo[256];
    char normal[256];
    char metallicRoughness[256];
} TerrainLayerDesc;

// authoring metadata persisted in the .terrain file alongside the gen params. the
// runtime does not consume the paint layers or grass yet (the terrain textures are
// still the hardcoded set loaded in Terrain.c), but the editor edits these and
// Terrain_SaveWorld/Terrain_LoadWorld round-trip them so nothing is lost on reload.
typedef struct TerrainAuthoring_
{
    TerrainLayerDesc layers[TERRAIN_MAX_LAYERS];
} TerrainAuthoring;

// mutable authoring settings owned by the terrain; the editor UI binds directly to
// this and Terrain_SaveWorld/LoadWorld are the single owners of its serialization
TerrainAuthoring* Terrain_GetAuthoring(void);

TerrainGenParams Terrain_DefaultGenParams(void);
// rebuilds the whole terrain with new generation settings: all chunks evict and
// regenerate. in flight worker jobs are discarded safely. cheap params reads
// happen on workers, only call this from the main thread
void Terrain_ApplyGenParams(const TerrainGenParams* params);
const TerrainGenParams* Terrain_GetGenParams(void);

// editor entry points: create applies the params and enables drawing/streaming,
// delete disables and frees every resident chunk
void Terrain_CreateWorld(const TerrainGenParams* params);
void Terrain_DeleteWorld(void);

// editor brush. the cursor whitens the surface around the hit point while active.
// sculpt adds/removes density (strength in meters, sign raises/lowers), paint stamps
// a texture layer index. both queue remeshes for the affected chunks; edits persist
// into the terrain's sibling .chunks file
void Terrain_SetBrushCursor(float3 position, f32 radius, bool active);
void Terrain_SculptSphere(float3 center, f32 radius, f32 strength, f32 softness);
// paint accumulates pressure into the voxels' two-slot blend weight, strength is
// weight units (0..255) per apply, call every frame while the brush drags
void Terrain_PaintSphere(float3 center, f32 radius, u32 layer, f32 strength, f32 softness);

// edited-region persistence for .chunks files. Save/Load store one binary record per
// sculpted/painted 16m region; apply gen params afterwards so chunks remesh with edits.
u32  Terrain_NumEditedRegions(void);
bool Terrain_SaveEditChunks(const char* path);
bool Terrain_LoadEditChunks(const char* path);
bool Terrain_SaveWorld(const char* path);
bool Terrain_LoadWorld(const char* path);

typedef struct TerrainStats_
{
    u32 liveChunks;     // chunks holding a mesh or known empty
    u32 emptyChunks;    // chunks with no surface crossing
    u32 queuedChunks;   // waiting for a worker
    u32 jobsInFlight;
    u32 drawnChunks;    // after frustum culling, last g-buffer pass
    u32 numVertices;    // resident in the terrain vertex heap
    u32 numIndices;
} TerrainStats;

// discrete procedural foliage (trees/rocks/props): one entity per placement, built by
// worker jobs and attached to the resident terrain chunks. TerrainFoliage.c owns this.
typedef struct tFoliageParams_
{
    f32  density;      // meters between placement grid samples, smaller = denser
    f32  rarity;       // 0..1, higher = sparser (noise gate threshold)
    f32  size;         // uniform scale multiplier, 1.0 = native mesh size
    bool enabled;
    bool collider;      // spawn a static physics collider per instance
    bool sizeVariance;  // +-30% random scale spread on top of size, when enabled
} tFoliageParams;

void tFoliage_Init();
void tFoliage_Destroy();
// the scene foliage entities render into (separate from g_ActiveScene, drawn by an
// explicit extra pass in RenderDepth/RenderSceneForward). NULL only before tFoliage_Init
struct Scene_* tFoliage_GetScene(void);

// one folliage type per mesh discovered under Assets/Foliage. index is stable for the
// process lifetime (load order), used by the editor to bind per-type UI and params
u32         tFoliage_NumTypes(void);
// display label for the editor: file name without directory/extension
const char* tFoliage_TypeName(u32 index);
bool        tFoliage_GetParams(u32 index, tFoliageParams* out);
// bumps the type's generation: every resident chunk rebuilds its foliage set (not its
// terrain mesh) over the next few frames, throttled the same way chunk streaming is
void        tFoliage_SetParams(u32 index, const tFoliageParams* params);

void tUpdate(void);
void tInvalidateAll(void);
void tInvalidateRegion(float3 mn, float3 mx);
void tSetBrushCursor(float3 position, f32 radius, bool active);

void tInit(void);     // after RendererInit + InitBuffers, gpu device must exist
void tDestroy(void);
bool tMarchingInit();
void tMarchingDestroy();

void tSetEnabled(bool enabled);
bool tGetEnabled(void);

// re-creates every live terrain chunk collider on the next terrain update. call after
// anything that destroys the scene's terrain physics bodies wholesale (the async
// static-collider rebuild on scene load does), otherwise the wiped colliders stay gone.
// safe from any thread
void tInvalidatePhysics(void);

// casts a world space ray against the resident terrain meshes (the same set that
// draws: live, not hidden, retiring included). dir must be normalized, hits beyond
// maxDist are rejected. only chunks with lod <= maxLod are tested, lower values make
// cheap queries cheaper. raycast uses the terrain geometry heap cpu mirrors for lod
// 0..1 only (the near rings, TERRAIN_RAYCAST_KEEP_LOD), coarser chunks never hit.
// fills the scene BVHHit: t/u/v/position/triIndex are valid, entityIdx holds the
// terrain chunk slot, groupIdx the chunk lod, and skinnedSet/bundleIdx are 0xFFFFFFFF
// to mark a terrain hit. out: 1 when hit
s32 tRaycast(float3 origin, float3 dir, f32 maxDist, u32 maxLod, BVHHit* hit);

// sphere-traces the analytic density field (noise + sculpt edits) instead of the
// meshes: works at any distance, approximate within ~half a coarse cell of the
// rendered surface. entityIdx/groupIdx are not meaningful on field hits
s32 tRaycastField(float3 origin, float3 dir, f32 maxDist, BVHHit* hit);
TerrainStats tGetStats(void);

// grass multidraw for the port terrain path, called inside the forward pass right
// after the port terrain surface; no-op until the grass system is initialized
void tRenderGrass(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass);
bool tGetMaterialTextures(SDL_GPUTexture** albedo, SDL_GPUTexture** normal, SDL_GPUTexture** arm);
// line overlay over the lit scene, enabled by g_RenderSettings.terrainWireframe
void RenderTerrainWireframe(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget,
                             SDL_GPUDepthStencilTargetInfo* depthTarget, mat4x4 viewProj);


#if defined(__cplusplus)
}
#endif

#endif // TERRAIN_H
