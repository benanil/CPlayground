#ifndef SCENE_H
#define SCENE_H

#include "RenderSet.h"
#include "TextureSystem.h"
#include "Animation.h"
#include "Async.h"
#include <SDL3/SDL_atomic.h>
#include <box3d/id.h>
#include <box3d/math_functions.h>
#include <box3d/types.h>

#define MAX_SCENE_BUNDLES 1024u
#define MAX_SCENE_LIGHTS  256u
#define MAX_THROWN_SPHERES 512u

// one bundle registered in a scene
typedef struct SceneBundleRef_
{
    const char*  path;           // bundle cache owned string
    SceneBundle* bundle;
    u32          renderIdx;      // bundle index inside its render set
    u32          transparentRenderIdx; // non-skinned BLEND primitives live in transparentSurfaceSet
    u32          materialOffset; // gpu material slot base of the bundle in this scene
    u32          animOffset;     // first animation of the bundle inside the scene's animation system
    u32          skinned;
    AnimationBundleAlloc animAlloc;
    u64          cacheKey;       // gBundleCache key (path hash); never store the entry pointer,
                                 // the cache map relocates entries on grow and on swap-with-last erase
} SceneBundleRef;

typedef struct BundleCacheEntry
{
    SceneBundle* bundle;
    char*        path;     // cache owned copy, scenes reference it in bundlePaths
    u32          refCount;
    // raw geometry heap pointers, needed to free the mega buffer ranges.
    // NULL when the geometry doesn't live in the mega buffers (fbx path)
    void*        vertexHeapPtr;
    void*        indexHeapPtr;
    // blas arrays of every primitive, built after load (BVH.c). gpu shareable layout,
    // primitives point into them through APrimitive.bvhNodeIndex
    void*        bvhNodes;
    void*        bvhTris;
    int          numBvhNodes;
    int          numBvhTris;
} BundleCacheEntry;

// persisted physics overrides for a single surface entity. only entities that deviate
// from the default static-mesh collider are saved; body/shape stored as b3BodyType /
// b3ShapeType, lockBits packs the six b3MotionLocks (linear xyz then angular xyz).
typedef struct ScenePhysicsRecord_
{
    u32 sparseIdx;
    u32 bodyType;
    u32 shapeType;
    u32 lockBits;
    f32 friction, restitution, density;
    f32 linearDamping, angularDamping, gravityScale, sleepThreshold;
} ScenePhysicsRecord;

// global (not per-scene) physics world settings, persisted to PhysicsSettings.txt at the
// working directory root. applied to each scene's world on creation and on edit.
typedef struct PhysicsSettings_
{
    f32  gravity[3];
    u32  substepCount;
    bool enableSleep;
    bool enableContinuous;
} PhysicsSettings;

extern PhysicsSettings g_PhysicsSettings;

// a scene owns one render set for skinned meshes, one for static geometry, their gpu
// buffers, its own texture system and animation system. all gpu resources stay resident,
// activating and deactivating scenes only changes the active list
typedef struct Scene_
{
    RenderSet        skinnedSet;
    RenderSet        surfaceSet;
    RenderSet        transparentSet;
	RenderSetBuffers skinnedBuffers;
    RenderSetBuffers surfaceBuffers;
    RenderSetBuffers transparentBuffers;
	TextureSystem    textureSystem;
    AnimationSystem  animSystem;

    SceneBundleRef*  bundleRefs;   // fixed MAX_SCENE_BUNDLES allocation. bundle indices are stable
                                   // handles, removing one never shifts the others
    u64*             bundleSlots;  // MAX_SCENE_BUNDLES bits, 1 means occupied
    u64*             materialSlots;// MAX_GPU_MATERIALS bits, 1 means occupied

    LightGPU*        lights;    // tlsf, MAX_SCENE_LIGHTS, authored lights pushed by Scene_SubmitLights
    u32              numLights;

    u32 numBundles;      // watermark: highest used bundle slot + 1, slots below may be empty
    u32 numMaterials;    // material slot watermark, offsets stay stable while occupied
    u32 renderDataDirty; // static render set buffers need re-upload, consumed by Render
    u32 texturesBaked;   // pages came from a baked atlas, packer state is unusable until a repack

	bool physicsReady;
	// static collision mesh handles, one per primitive group of each static render set. shapes
	// reference these (box3d does not copy mesh data), so they must outlive the world.
	struct b3MeshData*   surfacePhysicsMeshes[MAX_GROUP];
	struct b3MeshData*   transparentPhysicsMeshes[MAX_GROUP];
	b3BodyId*            surfacePhysicsBodies;
	b3BodyId*            transparentPhysicsBodies;
	SDL_AtomicInt        physicsColliderBuildRunning;
	SDL_AtomicInt        physicsColliderBuildDone;
	AsyncCallback        physicsColliderBuildCallback;
	s32                  physicsColliderBuildResult;
	// physics overrides parsed from a .scene file, applied once the async collider
	// build finishes (bodies do not exist until then). tlsf-owned, freed on apply.
	ScenePhysicsRecord* pendingPhysics;
	u32                 numPendingPhysics;
} Scene;

typedef enum SceneAsyncOp_
{
    SceneAsyncOp_None = 0,
    SceneAsyncOp_ImportMesh,
    SceneAsyncOp_OpenScene
} SceneAsyncOp;

typedef struct SceneAsyncRequest_ SceneAsyncRequest;

typedef void (*SceneAsyncRequestCallback)(SceneAsyncRequest* request);

struct SceneAsyncRequest_
{
    SceneAsyncOp op;
    SDL_AtomicInt done;
    Scene* scene;
    SceneAsyncRequestCallback callback;
    s32    result;
    u32    bundleIdx;
    v128f  position;
    v128f  rotation;
    v128f  scale;
    char   path[512];
    // bundle cache keys the probe acquired (and is holding) on the worker. The scene takes its own
    // references in the callback, then SceneAsyncUpdate drops these warming references. SDL_malloc'd.
    u64*   heldKeys;
    u32    heldCount;
    u32    heldCap;
};

// scenes the renderer draws each frame, in activation order
extern Scene* g_ActiveScene;

void Scene_Init(Scene* scene);
void Scene_Destroy(Scene* scene);

// engine-owned active scene helpers. These are usable without editor code and keep
// the active .scene path in Scene.c.
Scene* Scene_NewActive(void);
Scene* Scene_OpenActive(const char* path);
s32    Scene_SaveActive(void);
s32    Scene_SaveActiveAs(const char* path);
const char* Scene_GetActivePath(void);

// adds the scene to the rendered scenes. out: 0 when the active list is full.
// note: the animated vertex pool is shared and indexed by sparse id, only one
// active scene should contain skinned entities at a time
s32 Scene_Activate(Scene* scene);

void Scene_Deactivate(Scene* scene);

// the box3d world is a single global, scene independent instance (Physics_Init creates
// it lazily on first use) so bodies from different scenes - including gFoliage's private
// scene - can physically interact. Scene_InitPhysics/Scene_PhysicsDestroy only manage the
// scene's own body/mesh bookkeeping inside that shared world.
void Physics_Init(void);
void Physics_Destroy(void);
b3WorldId Physics_GetWorld(void);

void Scene_InitPhysics(Scene* scene);
void Scene_PhysicsDestroy(Scene* scene);
void Scene_PhysicsUpdate(Scene* scene, float deltaTime);
// loads/saves g_PhysicsSettings from PhysicsSettings.txt (load is one-shot, cached).
void PhysicsSettings_Load(void);
void PhysicsSettings_Save(void);
// pushes g_PhysicsSettings (gravity/sleep/continuous) onto the shared physics world.
void Scene_PhysicsApplyWorldSettings(void);

// builds a static rigid body with a triangle-mesh collider for every static mesh instance in the
// scene's surface render sets. call once after a scene finishes loading.
void Scene_BuildStaticCollidersAsync(Scene* scene, AsyncCallback callback);
void Scene_BuildStaticColliders(Scene* scene);
void Scene_PhysicsSyncEntityBody(Scene* scene, bool transparent, u32 groupIdx, const Entity* entity);
// terrain colliders are owned per-chunk (u64 stored body id + mesh pointer live on tChunk),
// not by the scene - no shared slot pool/cap, just the global physics world. inOutBody/
// inOutMesh are read (0/NULL means "none yet") and written back by these calls.
bool Scene_PhysicsSyncTerrainChunkMesh(u64* inOutBody, struct b3MeshData** inOutMesh,
                                       b3Vec3* vertices, u32 vertexCount,
                                       s32* indices, u32 indexCount);
void Scene_PhysicsDestroyTerrainChunk(u64* inOutBody, struct b3MeshData** inOutMesh);
// Swaps the collider shape of the entity's body in place. b3_meshShape restores the
// original triangle collider; sphere/capsule/hull are derived from the primitive
// bounds. Compound/height are unsupported and return false. Runtime-only.
bool Scene_PhysicsSetEntityShape(Scene* scene, bool transparent, u32 groupIdx, const Entity* entity, b3ShapeType type);
// Gives a dynamic body a default box mass from the shape AABB so it responds to gravity
// (mesh shapes compute zero mass). Shared by the inspector and the scene loader.
void Scene_PhysicsApplyDefaultDynamicMass(b3BodyId body, b3ShapeId shape);
// Reads the surface body at sparseIdx into *out; returns false when it matches the default
// static-mesh collider (nothing to persist). Used by the serializer to save only overrides.
bool Scene_PhysicsGetEntityOverride(const Scene* scene, u32 sparseIdx, ScenePhysicsRecord* out);
// Applies scene->pendingPhysics onto the freshly built bodies, then frees the buffer.
void Scene_PhysicsApplyPendingOverrides(Scene* scene);
b3Vec3 ToB3Vec3(v128f v);
b3Quat ToB3Quat(v128f q);
v128f SceneB3PosToVec3(b3Pos p);
u64   SceneB3QuatToEntityRotation(b3Quat q);

// per-frame scene tick: pumps async loads and steps physics for the active scene
void Scene_Update(float deltaTime);

// loads a gltf bundle, packs its textures into the scene's texture system and registers
// its primitives to the matching render set. bundles are shared through a global cache
// keyed by path, repeated adds of the same path reuse the resident mesh data.
// out: scene bundle index, INVALID_BUNDLE otherwise
u32 Scene_AddBundle(Scene* scene, const char* path, bool skinned);

// One staged bundle load: mesh acquire (BundleCacheAcquire) plus cached-texture decode/GPU
// upload (LoadBundleImagesFromCache). Touches only the bundle cache and this stage's own
// buffer, so it's safe off the main thread (ParallelFor); Scene_AddBundleFinalize then
// publishes it serially on the main thread (see tFoliage_Init, SceneSerializer_LoadBundles).
// Shared by Scene_AddBundleStage (mesh+textures) and Scene_AddBundleBakedStage (mesh only,
// materialOffset given directly) - skinned/staging unused on the baked path, materialOffset
// unused on the normal path.
typedef struct SceneBundleStage_
{
    SceneBundle* bundle;
    const char*  storedPath; // bundle cache owned string, stable for the entry's lifetime
    u64          cacheKey;
    u32          materialOffset; // Scene_AddBundleBaked* path only
    bool         skinned;        // Scene_AddBundle* path only
    bool         loaded;         // false when the load failed; Finalize/Abort are still safe to call
    Texture      staging[1024];  // Scene_AddBundle* path only
} SceneBundleStage;

// caller sets stage->storedPath (+ stage->skinned) before calling; result lands in
// stage->loaded (false on failure, Finalize/Abort still safe to call then). void*, single
// argument so callers fan this out with ParallelFor via a small per-index range wrapper.
void Scene_AddBundleStage(void* stage);

// Main-thread only: publishes a staged load into the scene (GPU texture packing, material/
// animation bookkeeping, render set registration) - the part that mutates shared scene state
// and so can't run off-thread. Releases the cache reference Scene_AddBundleStage took either
// way. out: scene bundle index, INVALID_BUNDLE on failure or when the stage itself had failed
u32 Scene_AddBundleFinalize(Scene* scene, SceneBundleStage* stage);

// Drops a staged load without adding it to any scene (e.g. caller decided not to use it).
// No-op when the stage failed to load. Safe from any thread.
void Scene_AddBundleStageAbort(SceneBundleStage* stage);

// Scene_AddBundle with the skinned flag detected from the bundle's skin data
u32 Scene_AddBundleAuto(Scene* scene, const char* path);

// loads (or finds) a bundle through the cache and holds a reference so it can be
// inspected without adding it to a scene. pair with Scene_ReleaseBundlePeek.
// out: NULL on load failure
const SceneBundle* Scene_AcquireBundlePeek(const char* path);
void Scene_ReleaseBundlePeek(const char* path);

// registers an already loaded bundle without touching the texture system, used by the
// baked scene load path where the pages are restored separately.
// out: scene bundle index, INVALID_BUNDLE otherwise
u32 Scene_AddBundleBaked(Scene* scene, const char* path, u32 materialOffset);

// Scene_AddBundleBaked split the same way as Scene_AddBundleStage/Finalize above: the Stage
// half is just BundleCacheAcquire (no texture work at all on the baked path), safe from any
// thread; Finalize does the material-slot/anim/render-set bookkeeping, main-thread only.
// caller sets stage->storedPath + stage->materialOffset before calling; see Scene_AddBundleStage.
void Scene_AddBundleBakedStage(void* stage);
u32  Scene_AddBundleBakedFinalize(Scene* scene, SceneBundleStage* stage);
void Scene_AddBundleBakedStageAbort(SceneBundleStage* stage);

u32 Scene_DefaultAnimation(const Scene* scene, u32 bundleIdx);
u32 Scene_FindBundleForRenderGroup(const Scene* scene, bool skinned, u32 groupIdx);

// pushes the active scene's authored lights to the renderer, call once per frame
void Scene_SubmitLights(void);

// removes the bundle's entities and primitive groups from the render set and clears its
// material slots. page space leaks until Scene_RepackTextures. scene bundle indices after
// bundleIdx shift down by one. out: number of entities removed
u32 Scene_RemoveBundle(Scene* scene, u32 bundleIdx);

// rebuilds the texture pages from the remaining bundles, reclaiming the space of removed
// ones. material offsets are preserved so baked group indices stay valid. stalls on io
// and transcode, do not call mid frame. out: 0 on failure
s32 Scene_RepackTextures(Scene* scene);

// instances the bundle node hierarchy with the given transform. out: number of entities added
u32 Scene_Spawn(Scene* scene, u32 bundleIdx, v128f position, v128f rotation, v128f scale);

void Scene_ClearEntities(Scene* scene);

// makes this the only rendered scene. out: 0 on failure
s32 Scene_MakeActive(Scene* scene);

// out: the first active scene, NULL when none
Scene* Scene_GetActive(void);

// Copies the resident cache entry for a scene render bundle into *out (looked up by key under the
// cache lock). out: false when the bundle is not resident. The copied bvhNodes/bvhTris pointers stay
// valid while the bundle is referenced; never hold the entry pointer itself, the map relocates it.
bool FindCacheForSceneBundle(const Scene* scene, u32 bundleIdx, BundleCacheEntry* out);

// ASYNC

bool SceneAsyncBegin(SceneAsyncOp op, const char* path, const char* taskName, SceneAsyncRequestCallback callback);


#endif // SCENE_H
