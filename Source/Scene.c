
#include "Include/Scene.h"
#include "Include/SceneBundleCache.h"
#include "Include/Async.h"
#include "Include/Graphics.h"
#include "Include/Memory.h"
#include "Include/Platform.h"
#include "Include/AssetManager.h"
#include "Include/Animation.h"
#include "Include/FileSystem.h"
#include "Include/Rendering.h"
#include "Include/SceneSerializer.h"
#include "Include/Terrain.h"
#include "Include/Random.h"
#include "Include/Algorithm.h"
#include "Include/Bitset.h"
#include "Include/GLTFParser.h"
#include "Include/BVH.h"
#include "Include/Camera.h"
#include "Include/DataStructures/HashMap.h"
#include "Math/Quaternion.h"

#include <box3d/box3d.h>
#include <SDL3/SDL_stdinc.h>

Scene* g_ActiveScene = NULL;
extern Camera g_Camera;
extern SDL_GPUDevice* g_GPUDevice;

static Scene g_OwnedActiveScene;
static bool  g_OwnedActiveSceneInit;
static char  g_ActiveScenePath[512];

static void SceneTerrainPath(const char* scenePath, char* out, u32 outSize)
{
    NormalizePath(scenePath, out, outSize);
    ChangeExtension(out, 512, "terrain");
}

static void SceneSaveTerrainSidecar(const char* scenePath)
{
    char terrainPath[512];
    SceneTerrainPath(scenePath, terrainPath, sizeof(terrainPath));

    if (Terrain_GetEnabled())
        Terrain_SaveWorld(terrainPath);
    else if (FileExist(terrainPath))
        RemoveFile(terrainPath);
}

// reserves the lowest free bundle slot so indices stay stable across removals. the ref array is a
// fixed MAX_SCENE_BUNDLES allocation. out: bundle slot index, INVALID_BUNDLE when full
static u32 Scene_AllocBundleSlot(Scene* scene)
{
    s32 slot = BitsetFindFirstEmpty(scene->bundleSlots, (s32)MAX_SCENE_BUNDLES);
    if (slot < 0)
    {
        AX_WARN("maximum scene bundle count reached: %d", MAX_SCENE_BUNDLES);
        return INVALID_BUNDLE;
    }
    BitsetSet(scene->bundleSlots, slot);
    if ((u32)slot + 1u > scene->numBundles) scene->numBundles = (u32)slot + 1u;
    return (u32)slot;
}

// frees a bundle slot and pulls the watermark back over any trailing empty slots
static void Scene_FreeBundleSlot(Scene* scene, u32 bundleIdx)
{
    BitsetReset(scene->bundleSlots, (s32)bundleIdx);
    MemsetZero(&scene->bundleRefs[bundleIdx], sizeof(SceneBundleRef));
    while (scene->numBundles > 0 && !BitsetGet(scene->bundleSlots, (s32)(scene->numBundles - 1u)))
        scene->numBundles--;
}

// stamps every primitive group of a render bundle with its owning scene bundle index. both indices
// are stable handles, so the group keeps pointing at the right bundle even after group compaction,
// giving O(1) render-group -> scene-bundle without a reverse map.
static void Scene_StampGroupBundle(RenderSet* set, u32 renderIdx, u32 sceneIdx)
{
    Range range = set->bundlePrimRange[renderIdx];
    for (u32 g = 0; g < range.count; g++)
        set->primitiveGroups[range.start + g].bundleIdx = (u16)sceneIdx;
}

void Scene_Init(Scene* scene)
{
    MemsetZero(scene, sizeof(*scene));
    RenderSet_InitSet(&scene->skinnedSet, MAX_ANIM_INSTANCES, MAX_GROUP, MAX_BUNDLES, true);
    RenderSet_InitSet(&scene->surfaceSet, MAX_ENTITY, MAX_GROUP, MAX_BUNDLES, false);
    RenderSet_InitSet(&scene->transparentSet, MAX_ENTITY, MAX_GROUP, MAX_BUNDLES, false);
    RenderSet_SetHookScene(&scene->skinnedSet, scene);
    RenderSet_SetHookScene(&scene->surfaceSet, scene);
    RenderSet_SetHookScene(&scene->transparentSet, scene);
    RenderSet_SetMaterialFilter(&scene->surfaceSet, RenderSetMaterialFilter_Opaque);
    RenderSet_SetMaterialFilter(&scene->transparentSet, RenderSetMaterialFilter_Transparent);
    CreateRenderSetBuffers(&scene->skinnedBuffers, MAX_ANIM_INSTANCES, MAX_GROUP);
    CreateRenderSetBuffers(&scene->surfaceBuffers, MAX_ENTITY, MAX_GROUP);
    CreateRenderSetBuffers(&scene->transparentBuffers, MAX_ENTITY, MAX_GROUP);
    TextureSystem_Init(&scene->textureSystem);
    AnimationSystem_Init(&scene->animSystem);
	Scene_InitPhysics(scene);
	scene->lights = (LightGPU*)AllocZeroTLSFGlobal(MAX_SCENE_LIGHTS, sizeof(LightGPU));
    scene->materialSlots = (u64*)AllocZeroTLSFGlobal((MAX_GPU_MATERIALS + 63u) >> 6, sizeof(u64));
    scene->bundleSlots   = (u64*)AllocZeroTLSFGlobal((MAX_SCENE_BUNDLES + 63u) >> 6, sizeof(u64));
    scene->bundleRefs    = (SceneBundleRef*)AllocZeroTLSFGlobal(MAX_SCENE_BUNDLES, sizeof(SceneBundleRef));
}

void Scene_Destroy(Scene* scene)
{
    Scene_Deactivate(scene);
    for (u32 i = 0; i < scene->numBundles; i++)
        if (scene->bundleRefs[i].bundle)
            BundleCacheRelease(scene->bundleRefs[i].path);
    DestroyRenderSetBuffers(&scene->skinnedBuffers);
    DestroyRenderSetBuffers(&scene->surfaceBuffers);
    DestroyRenderSetBuffers(&scene->transparentBuffers);
    TextureSystem_Destroy(&scene->textureSystem);
    AnimationSystem_Destroy(&scene->animSystem);
	RenderSet_Destroy(&scene->skinnedSet);
	RenderSet_Destroy(&scene->surfaceSet);
	RenderSet_Destroy(&scene->transparentSet);
	if (scene->bundleRefs)    DeAllocateTLSFGlobal(scene->bundleRefs);
    if (scene->lights)        DeAllocateTLSFGlobal(scene->lights);
    if (scene->materialSlots) DeAllocateTLSFGlobal(scene->materialSlots);
    if (scene->bundleSlots)   DeAllocateTLSFGlobal(scene->bundleSlots);
	Scene_PhysicsDestroy(scene);
	scene->bundleRefs    = NULL;
    scene->lights        = NULL;
    scene->materialSlots = NULL;
    scene->bundleSlots   = NULL;
	scene->renderDataDirty = 0;
    if (scene == &g_OwnedActiveScene)
    {
        g_OwnedActiveSceneInit = false;
        g_ActiveScenePath[0] = '\0';
    }
    // render set cpu allocations stay, consistent with the rest of the engine teardown
}

Scene* Scene_NewActive(void)
{
    if (g_OwnedActiveSceneInit)
    {
        if (g_GPUDevice) SDL_WaitForGPUIdle(g_GPUDevice);
        Scene_Destroy(&g_OwnedActiveScene);
    }
    Scene_Init(&g_OwnedActiveScene);
    g_OwnedActiveSceneInit = true;
    g_ActiveScenePath[0] = '\0';
    Scene_MakeActive(&g_OwnedActiveScene);
    RendererSetLights(NULL, 0u);
    Terrain_DeleteWorld();
    return &g_OwnedActiveScene;
}

void Scene_Update(float deltaTime)
{
    Scene_AsyncUpdate();

	Scene* activeScene = Scene_GetActive();
	if (activeScene == NULL) return;
	
	Scene_PhysicsUpdate(activeScene, deltaTime);
}

Scene* Scene_OpenActive(const char* path)
{
    char normalized[512];
    NormalizePath(path, normalized, sizeof(normalized));

    Scene* scene = Scene_NewActive();
    if (!scene) return NULL;
	RenderSet_Clear(&scene->surfaceSet);
	if (!SceneSerializer_Load(scene, normalized))
    {
        AX_ERROR("scene load failed: %s", normalized);
        return NULL;
    }
    MemCopy(g_ActiveScenePath, normalized, StringLength(normalized) + 1);

    Terrain_DeleteWorld();
    char terrainPath[512];
    SceneTerrainPath(normalized, terrainPath, sizeof(terrainPath));
    if (FileExist(terrainPath))
        Terrain_LoadWorld(terrainPath);
    return scene;
}

s32 Scene_SaveActive(void)
{
    Scene* scene = Scene_GetActive();
    if (!scene || g_ActiveScenePath[0] == '\0') return 0;
    if (!SceneSerializer_Save(scene, g_ActiveScenePath)) return 0;
    SceneSaveTerrainSidecar(g_ActiveScenePath);
    return 1;
}

s32 Scene_SaveActiveAs(const char* path)
{
    Scene* scene = Scene_GetActive();
    if (!scene || !path || path[0] == '\0') return 0;

    char normalized[512];
    NormalizePath(path, normalized, sizeof(normalized));
    EnsurePath(normalized);
    if (!SceneSerializer_Save(scene, normalized)) return 0;
    MemCopy(g_ActiveScenePath, normalized, StringLength(normalized) + 1);
    SceneSaveTerrainSidecar(g_ActiveScenePath);
    return 1;
}

const char* Scene_GetActivePath(void)
{
    return g_ActiveScenePath;
}

s32 Scene_Activate(Scene* scene)
{
    if (g_ActiveScene == scene) return 1;
    g_ActiveScene = scene;
    scene->renderDataDirty = 1;
    return 1;
}

void Scene_Deactivate(Scene* scene)
{
    if (g_ActiveScene != scene) return;
    g_ActiveScene = NULL;
}

// out: scene bundle index of the path, INVALID_BUNDLE when not present
static u32 Scene_FindBundle(const Scene* scene, const char* path)
{
    for (u32 i = 0; i < scene->numBundles; i++)
        if (scene->bundleRefs[i].bundle && StringEqual(scene->bundleRefs[i].path, path, StringLength(path) + 1))
            return i;
    return INVALID_BUNDLE;
}

static void Scene_UpdateMaterialWatermark(Scene* scene)
{
    while (scene->numMaterials > 0 && !BitsetGet(scene->materialSlots, (s32)(scene->numMaterials - 1u)))
        scene->numMaterials--;
    scene->textureSystem.materialWatermark = scene->numMaterials;
}

static s32 Scene_AllocateMaterialSlots(Scene* scene, u32 numMaterials)
{
    if (numMaterials == 0) return 0;
    s32 firstEmpty = BitsetFindEmptyRange(scene->materialSlots, MAX_GPU_MATERIALS, numMaterials);
    if (firstEmpty != -1)
    {
        BitsetSetRange(scene->materialSlots, firstEmpty, numMaterials, true);
        return firstEmpty;
    }
    AX_WARN("maximum GPU materials reached: count=%d", numMaterials);
    return -1;
}

static s32 Scene_ReserveMaterialSlots(Scene* scene, u32 materialOffset, u32 numMaterials)
{
    if (numMaterials == 0) return 1;
    if (materialOffset + numMaterials > MAX_GPU_MATERIALS)
    {
        AX_WARN("maximum GPU materials reached: offset=%d count=%d", materialOffset, numMaterials);
        return 0;
    }

    for (u32 i = 0; i < numMaterials; i++)
    {
        if (BitsetGet(scene->materialSlots, (s32)(materialOffset + i)))
        {
            AX_WARN("material slot range overlaps existing bundle: offset=%d count=%d", materialOffset, numMaterials);
            return 0;
        }
    }

    BitsetSetRange(scene->materialSlots, materialOffset, numMaterials, true);
    return 1;
}

u32 Scene_AddBundle(Scene* scene, const char* path, bool skinned)
{
    u32 existing = Scene_FindBundle(scene, path);
    if (existing != INVALID_BUNDLE)
        return existing;
    if (BitsetFindFirstEmpty(scene->bundleSlots, (s32)MAX_SCENE_BUNDLES) < 0)
    {
        AX_WARN("maximum scene bundle count reached: %d", MAX_SCENE_BUNDLES);
        return INVALID_BUNDLE;
    }
    if (scene->texturesBaked && !Scene_RepackTextures(scene))
        return INVALID_BUNDLE;

    BundleCacheEntry* entry = BundleCacheAcquire(path);
    if (!entry) {
        AX_WARN("gltf scene load failed: %s", path); return INVALID_BUNDLE;
    }

    SceneBundle* bundle    = entry->bundle;
    const char* storedPath = entry->path;
    ArenaMark mark         = ArenaSave(&GlobalArena);
    Texture* staging       = (Texture*)ArenaAllocZero(&GlobalArena, MAX_SCENE_TEXTURES * sizeof(Texture));

    s32 imageResult = LoadBundleImagesFromCache(storedPath, bundle, staging);
    if (imageResult == 0) goto err_arena;

    AnimationBundleAlloc animAlloc;
    MemsetZero(&animAlloc, sizeof(animAlloc));
    bool animAppended = false;
    if (skinned && !AnimationSystem_AppendBundle(&scene->animSystem, bundle, &animAlloc))
        goto err_arena;
    animAppended = skinned;

    s32 allocatedMaterialOffset = Scene_AllocateMaterialSlots(scene, (u32)bundle->numMaterials);
    if (allocatedMaterialOffset < 0) goto err_arena;

    u32 materialOffset = (u32)allocatedMaterialOffset;
    s32 appended       = TextureSystem_AppendBundle(&scene->textureSystem, bundle, staging, materialOffset);
    TextureSystem_ReleaseTextures(staging, (u32)bundle->numImages);
    ArenaRestore(&GlobalArena, mark);

    if (!appended) goto err_materials;

    RenderSet* set  = skinned ? &scene->skinnedSet : &scene->surfaceSet;
    u32 renderIdx   = RenderSet_AddSceneBundle(set, bundle, materialOffset);
    if (renderIdx == INVALID_BUNDLE) goto err_textures;

    u32 transparentRenderIdx = INVALID_BUNDLE;
    if (!skinned)
    {
        transparentRenderIdx = RenderSet_AddSceneBundle(&scene->transparentSet, bundle, materialOffset);
        if (transparentRenderIdx == INVALID_BUNDLE)
        {
            RenderSet_RemoveSceneBundle(set, renderIdx);
            goto err_textures;
        }
    }

    u32 bundleIdx           = Scene_AllocBundleSlot(scene);
    if (bundleIdx == INVALID_BUNDLE)
    {
        RenderSet_RemoveSceneBundle(set, renderIdx);
        if (transparentRenderIdx != INVALID_BUNDLE)
            RenderSet_RemoveSceneBundle(&scene->transparentSet, transparentRenderIdx);
        goto err_textures;
    }
    SceneBundleRef* ref     = &scene->bundleRefs[bundleIdx];
    ref->path               = storedPath;
    ref->bundle             = bundle;
    ref->renderIdx          = renderIdx;
    ref->transparentRenderIdx = transparentRenderIdx;
    ref->materialOffset     = materialOffset;
    ref->animOffset         = animAlloc.animOffset;
    ref->animAlloc          = animAlloc;
    ref->skinned            = skinned;
    ref->cacheKey           = StringToHash64(storedPath);
    Scene_StampGroupBundle(set, renderIdx, bundleIdx);
    if (transparentRenderIdx != INVALID_BUNDLE)
        Scene_StampGroupBundle(&scene->transparentSet, transparentRenderIdx, bundleIdx);

    if (materialOffset + (u32)bundle->numMaterials > scene->numMaterials)
        scene->numMaterials = materialOffset + (u32)bundle->numMaterials;
    scene->renderDataDirty = 1;
    return bundleIdx;

err_textures:
    AX_ERROR("render set bundle registration failed: %s", storedPath);
    TextureSystem_RemoveBundle(&scene->textureSystem, bundle, materialOffset);
err_materials:
    BitsetSetRange(scene->materialSlots, materialOffset, (u32)bundle->numMaterials, false);
    if (animAppended) AnimationSystem_RemoveBundle(&scene->animSystem, animAlloc);
    BundleCacheRelease(storedPath);
    return INVALID_BUNDLE;
err_arena:
    TextureSystem_ReleaseTextures(staging, (u32)bundle->numImages);
    ArenaRestore(&GlobalArena, mark);
    if (animAppended) AnimationSystem_RemoveBundle(&scene->animSystem, animAlloc);
    BundleCacheRelease(storedPath);
    return INVALID_BUNDLE;
}

const SceneBundle* Scene_AcquireBundlePeek(const char* path)
{
    BundleCacheEntry* entry = BundleCacheAcquire(path);
    return entry ? entry->bundle : NULL;
}

u32 Scene_DefaultAnimation(const Scene* scene, u32 bundleIdx)
{
    if (!scene || bundleIdx >= scene->numBundles) return 0u;
    const SceneBundleRef* ref = &scene->bundleRefs[bundleIdx];
    u32 numAnims = ref->bundle ? (u32)ref->bundle->numAnimations : 0u;
    if (numAnims == 0u) return 0u;
    return ref->animOffset + (numAnims > 1u ? 1u : 0u);
}

u32 Scene_FindBundleForRenderGroup(const Scene* scene, bool skinned, u32 groupIdx)
{
    if (!scene) return INVALID_BUNDLE;
    const RenderSet* set = skinned ? &scene->skinnedSet : &scene->surfaceSet;
    if (groupIdx >= set->numGroups) return INVALID_BUNDLE;

    // each group stores its owning scene bundle index directly. it is a stable handle and rides
    // along through group compaction, so this is a plain O(1) field read.
    return set->primitiveGroups[groupIdx].bundleIdx;
}

void Scene_ReleaseBundlePeek(const char* path)
{
    BundleCacheRelease(path);
}

u32 Scene_AddBundleAuto(Scene* scene, const char* path)
{
    // hold a reference while peeking so the bundle loads only once
    BundleCacheEntry* entry = BundleCacheAcquire(path);
    if (!entry)
    {
        AX_ERROR("gltf scene load failed: %s", path);
        return INVALID_BUNDLE;
    }
    bool skinned = entry->bundle->numSkins > 0;
    u32 bundleIdx = Scene_AddBundle(scene, path, skinned);
    BundleCacheRelease(path);
    return bundleIdx;
}

u32 Scene_AddBundleBaked(Scene* scene, const char* path, u32 materialOffset)
{
    if (BitsetFindFirstEmpty(scene->bundleSlots, (s32)MAX_SCENE_BUNDLES) < 0)
    {
        AX_WARN("maximum scene bundle count reached: %d", MAX_SCENE_BUNDLES);
        return INVALID_BUNDLE;
    }

    BundleCacheEntry* entry = BundleCacheAcquire(path);
    if (!entry) {
        AX_ERROR("scene bundle load failed: %s", path);
        return INVALID_BUNDLE;
    }
    SceneBundle* bundle = entry->bundle;
    const char* storedPath = entry->path;
    bool skinned = bundle->numSkins > 0;

    if (!Scene_ReserveMaterialSlots(scene, materialOffset, (u32)bundle->numMaterials))
    {
        BundleCacheRelease(storedPath);
        return INVALID_BUNDLE;
    }

    AnimationBundleAlloc animAlloc;
    MemsetZero(&animAlloc, sizeof(animAlloc));
    bool animAppended = false;
    if (skinned && !AnimationSystem_AppendBundle(&scene->animSystem, bundle, &animAlloc))
    {
        AX_ERROR("scene animation creation failed: %s", storedPath);
        goto err_bundle;
    }
    animAppended = skinned;

    RenderSet* set = skinned ? &scene->skinnedSet : &scene->surfaceSet;
    u32 renderIdx = RenderSet_AddSceneBundle(set, bundle, materialOffset);
    if (renderIdx == INVALID_BUNDLE)
    {
        AX_ERROR("render set bundle registration failed: %s", storedPath);
        goto err_bundle;
    }

    u32 transparentRenderIdx = INVALID_BUNDLE;
    if (!skinned)
    {
        transparentRenderIdx = RenderSet_AddSceneBundle(&scene->transparentSet, bundle, materialOffset);
        if (transparentRenderIdx == INVALID_BUNDLE)
        {
            AX_ERROR("transparent render set bundle registration failed: %s", storedPath);
            RenderSet_RemoveSceneBundle(set, renderIdx);
            goto err_bundle;
        }
    }

    u32 bundleIdx = Scene_AllocBundleSlot(scene);
    if (bundleIdx == INVALID_BUNDLE)
    {
        RenderSet_RemoveSceneBundle(set, renderIdx);
        if (transparentRenderIdx != INVALID_BUNDLE)
            RenderSet_RemoveSceneBundle(&scene->transparentSet, transparentRenderIdx);
        goto err_bundle;
    }
    SceneBundleRef* ref = &scene->bundleRefs[bundleIdx];
    ref->path           = storedPath;
    ref->bundle         = bundle;
    ref->renderIdx      = renderIdx;
    ref->transparentRenderIdx = transparentRenderIdx;
    ref->materialOffset = materialOffset;
    ref->animOffset     = animAlloc.animOffset;
    ref->animAlloc      = animAlloc;
    ref->skinned        = skinned;
    ref->cacheKey       = StringToHash64(storedPath);
    Scene_StampGroupBundle(set, renderIdx, bundleIdx);
    if (transparentRenderIdx != INVALID_BUNDLE)
        Scene_StampGroupBundle(&scene->transparentSet, transparentRenderIdx, bundleIdx);
    if (materialOffset + (u32)bundle->numMaterials > scene->numMaterials)
        scene->numMaterials = materialOffset + (u32)bundle->numMaterials;
    scene->renderDataDirty = 1;
    return bundleIdx;
err_bundle:
    if (animAppended) AnimationSystem_RemoveBundle(&scene->animSystem, animAlloc);
    BitsetSetRange(scene->materialSlots, materialOffset, (u32)bundle->numMaterials, false);
    BundleCacheRelease(storedPath);
    return INVALID_BUNDLE;
}

u32 Scene_RemoveBundle(Scene* scene, u32 bundleIdx)
{
    if (bundleIdx >= scene->numBundles || !scene->bundleRefs[bundleIdx].bundle) return 0;

    SceneBundleRef* ref = &scene->bundleRefs[bundleIdx];
    bool skinned = ref->skinned != 0;
    RenderSet* set = skinned ? &scene->skinnedSet : &scene->surfaceSet;
    u32 removedRenderIdx = ref->renderIdx;

    u32 removedEntities = RenderSet_RemoveSceneBundle(set, removedRenderIdx);
    if (!skinned && ref->transparentRenderIdx != INVALID_BUNDLE)
        removedEntities += RenderSet_RemoveSceneBundle(&scene->transparentSet, ref->transparentRenderIdx);
    TextureSystem_RemoveBundle(&scene->textureSystem, ref->bundle, ref->materialOffset);
    BitsetSetRange(scene->materialSlots, ref->materialOffset, (u32)ref->bundle->numMaterials, false);
    Scene_UpdateMaterialWatermark(scene);
    if (skinned) AnimationSystem_RemoveBundle(&scene->animSystem, ref->animAlloc);
    BundleCacheRelease(ref->path);

    // both the render set and the scene keep stable slot handles now, so removing this bundle
    // leaves every other bundle's index (and its renderIdx) untouched. just free the slot.
    Scene_FreeBundleSlot(scene, bundleIdx);
    scene->renderDataDirty = 1;
    return removedEntities;
}

s32 Scene_RepackTextures(Scene* scene)
{
    AX_LOG("Scene_RepackTextures");
    TextureSystem_ResetPacking(&scene->textureSystem);
    scene->texturesBaked = 0;

    for (u32 b = 0; b < scene->numBundles; b++)
    {
        SceneBundle* bundle = scene->bundleRefs[b].bundle;
        if (!bundle || (bundle->numImages <= 0 && bundle->numMaterials <= 0)) continue;

        ArenaMark mark = ArenaSave(&GlobalArena);
        Texture* staging = (Texture*)ArenaAllocZero(&GlobalArena, MAX_SCENE_TEXTURES * sizeof(Texture));
        if (LoadBundleImagesFromCache(scene->bundleRefs[b].path, bundle, staging) == 0)
        {
            AX_ERROR("scene image load failed during repack: %s", scene->bundleRefs[b].path);
            ArenaRestore(&GlobalArena, mark);
            return 0;
        }

        s32 appended = TextureSystem_AppendBundle(&scene->textureSystem, bundle, staging, scene->bundleRefs[b].materialOffset);
        TextureSystem_ReleaseTextures(staging, (u32)bundle->numImages);
        ArenaRestore(&GlobalArena, mark);
        if (!appended)
            return 0;
    }
    return 1;
}

u32 Scene_Spawn(Scene* scene, u32 bundleIdx, v128f position, v128f rotation, v128f scale)
{
    if (bundleIdx >= scene->numBundles || !scene->bundleRefs[bundleIdx].bundle) return 0;

    bool skinned = scene->bundleRefs[bundleIdx].skinned != 0;
    RenderSet* set = skinned ? &scene->skinnedSet : &scene->surfaceSet;
    Range range = set->bundlePrimRange[scene->bundleRefs[bundleIdx].renderIdx];
    u32* oldCounts = NULL;
    if (skinned && range.count > 0u)
    {
        oldCounts = (u32*)ArenaAllocGlobal(range.count * sizeof(u32));
        for (u32 i = 0; i < range.count; i++)
            oldCounts[i] = set->primitiveGroups[range.start + i].numEntities;
    }

    u32 added = RenderSet_AddScene(set, scene->bundleRefs[bundleIdx].renderIdx, position, rotation, scale, skinned);
    if (!skinned && scene->bundleRefs[bundleIdx].transparentRenderIdx != INVALID_BUNDLE)
        added += RenderSet_AddScene(&scene->transparentSet, scene->bundleRefs[bundleIdx].transparentRenderIdx, position, rotation, scale, false);
    if (skinned && added && oldCounts)
    {
        GPUAnimationInstance instance = { .animIdx = Scene_DefaultAnimation(scene, bundleIdx), .timeOffset = 0.0f };
        for (u32 i = 0; i < range.count; i++)
        {
            PrimitiveGroup* group = &set->primitiveGroups[range.start + i];
            for (u32 e = oldCounts[i]; e < group->numEntities; e++)
            {
                u32 sparseIdx = set->entities[group->entityOffset + e].sparseIdx;
                AnimationSystem_SetInstance(&scene->animSystem, sparseIdx, instance);
            }
        }
    }
    if (oldCounts) ArenaPopGlobal(range.count * sizeof(u32));

    if (added) scene->renderDataDirty = 1;
    return added;
}

void Scene_ClearEntities(Scene* scene)
{
    RenderSet_ClearEntities(&scene->skinnedSet);
    RenderSet_ClearEntities(&scene->surfaceSet);
    RenderSet_ClearEntities(&scene->transparentSet);
    scene->renderDataDirty = 1;
}

void Scene_SubmitLights(void)
{
    Scene* scene = Scene_GetActive();
    if (scene && scene->numLights)
        RendererSetLights(scene->lights, scene->numLights);
}

s32 Scene_MakeActive(Scene* scene)
{
    if (g_ActiveScene) Scene_Deactivate(g_ActiveScene);
    return Scene_Activate(scene);
}

Scene* Scene_GetActive(void)
{
    return g_ActiveScene;
}
