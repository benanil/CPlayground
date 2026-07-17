
#include "Include/SceneBundleCache.h"
#include "Include/Scene.h"
#include "Include/Async.h"
#include "Include/Graphics.h"
#include "Include/Memory.h"
#include "Include/Platform.h"
#include "Include/AssetManager.h"
#include "Include/Animation.h"
#include "Include/FileSystem.h"
#include "Include/Rendering.h"
#include "Include/Algorithm.h"
#include "Include/Random.h"
#include "Include/GLTFParser.h"
#include "Include/BVH.h"
#include "Include/DataStructures/HashMap.h"

#include <SDL3/SDL_stdinc.h>

extern Graphics gGFX;

// returns the bundle's vertex/index ranges to the geometry heaps. safe to call
// twice, the pointers are nulled after the free. bundles whose geometry lives
// outside the mega buffers (fbx path) have NULL heap pointers and are skipped
static void Scene_FreeBundleGeometry(SceneBundle* bundle, BundleCacheEntry* cache, bool skinned)
{
    if (cache->vertexHeapPtr)
    {
        GeometryHeapFree(skinned ? GeometryBuffer_SkinnedVertex : GeometryBuffer_SurfaceVertex, cache->vertexHeapPtr);
        if (skinned) gGFX.NumSkinnedVertices -= (u32)bundle->totalVertices;
        else         gGFX.NumSurfaceVertices -= (u32)bundle->totalVertices;
        cache->vertexHeapPtr = NULL;
        bundle->allVertices = NULL;
    }
    if (cache->indexHeapPtr)
    {
        GeometryHeapFree(GeometryBuffer_Index, cache->indexHeapPtr);
        gGFX.NumIndices -= (u32)bundle->totalIndices;
        cache->indexHeapPtr = NULL;
        bundle->allIndices = NULL;
    }
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                              Async                                       */
/*//////////////////////////////////////////////////////////////////////////*/

SceneAsyncRequest* sceneAsyncRequest;

// Acquire a bundle into the cache on the worker thread and HOLD the reference (recording its key on
// the request). The main-thread callback then adds the bundle to the scene as a cache hit instead of
// re-baking it; SceneAsyncUpdate drops these warming references afterwards. out: false on load failure.
static bool SceneAsyncHold(SceneAsyncRequest* request, const char* path)
{
    if (!Scene_AcquireBundlePeek(path)) return false;

    if (request->heldCount == request->heldCap)
    {
        u32 newCap  = request->heldCap ? request->heldCap * 2u : 8u;
        u64* grown  = (u64*)SDL_realloc(request->heldKeys, (size_t)newCap * sizeof(u64));
        if (!grown) { Scene_ReleaseBundlePeek(path); return false; }
        request->heldKeys = grown;
        request->heldCap  = newCap;
    }
    request->heldKeys[request->heldCount++] = StringToHash64(path);
    return true;
}

static s32 SceneAsyncProbe(void* userData)
{
    SceneAsyncRequest* request = (SceneAsyncRequest*)userData;

    if (request->op == SceneAsyncOp_ImportMesh)
        return SceneAsyncHold(request, request->path);

    if (request->op == SceneAsyncOp_OpenScene)
    {
        AFile file = AFileOpen(request->path, AOpenFlag_ReadBinary);
        if (!AFileExist(file)) return 0;

        char line[2048];
        while (AFileReadLine(line, sizeof(line), file) > 0)
        {
            int len = StringLength(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
                line[--len] = '\0';

            const char prefix[] = "bundle ";
            if (!StringEqual(line, prefix, StringLength(prefix))) continue;

            const char* path = line + StringLength(prefix);
            for (u32 spaces = 0u; spaces < 3u && *path; path++)
                if (*path == ' ') spaces++;
            while (*path == ' ') path++;
            if (!*path) continue;

            // Bake/load each bundle off the main thread and HOLD it, so the main-thread open hits the
            // cache (no re-bake) and the worker-built picking BVH is reused. SceneAsyncUpdate releases
            // these after the scene has taken its own references. The cache is keyed + lock-guarded,
            // so this is safe against the main thread picking the still-active scene.
            if (!SceneAsyncHold(request, path))
            {
                AFileClose(file);
                return 0;
            }
        }
        AFileClose(file);
        return 1;
    }
    return 1;
}

static void SceneAsyncDone(void* userData, s32 result)
{
    SceneAsyncRequest* request = (SceneAsyncRequest*)userData;
    request->result = result;
	if (!result) AX_WARN("scene async request start failed: %s", request->path);
	SDL_SetAtomicInt(&request->done, 1);
}

bool SceneAsyncBegin(SceneAsyncOp op, const char* path, const char* taskName, SceneAsyncRequestCallback callback)
{
    if (!path || path[0] == '\0')
    {
        AX_WARN("scene async request invalid path");
        return false;
    }

    if (sceneAsyncRequest)
    {
        AX_WARN("scene async request already running: %s", sceneAsyncRequest->path);
        return false;
    }

    char normalized[512];
    NormalizePath(path, normalized, sizeof(normalized));
    if (normalized[0] == '\0')
    {
        AX_WARN("scene async request invalid path");
        return false;
    }

    SceneAsyncRequest* request = (SceneAsyncRequest*)SDL_calloc(1, sizeof(SceneAsyncRequest));
    request->callback = callback;
    request->op = op;
    MemCopy(request->path, normalized, StringLength(normalized) + 1);
	AsyncRun(taskName, SceneAsyncProbe, SceneAsyncDone, request);
    sceneAsyncRequest = request;
    return true;
}

void Scene_AsyncUpdate(void)
{
    SceneAsyncRequest* request = sceneAsyncRequest;
    if (!request || !SDL_GetAtomicInt(&request->done)) return;
    sceneAsyncRequest = NULL;

    if (!request->result)
    {
		AX_ERROR("scene async request failed: %s", request->path);
    }
	else if (request->callback)
	{
		request->callback(request);
	}

    // The probe held one warming reference per bundle so the callback's scene/import load hit the
    // cache instead of re-baking. The scene took its own references in the callback, so drop the
    // warming ones now (this also frees the bundles when the request failed and no callback ran).
    for (u32 i = 0; i < request->heldCount; i++)
        BundleCacheReleaseKey(request->heldKeys[i]);
    SDL_free(request->heldKeys);
    SDL_free(request);
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                              Bundle Cache                                */
/*//////////////////////////////////////////////////////////////////////////*/

// resident mesh bundles keyed by path hash, shared between scenes and repeated adds.
// async scene/mesh import touches this from worker threads, so every map operation (and every
// access to an entry) is serialized through g_BundleCacheLock. Entries are addressed by key, never
// by stored pointer, because HashMap relocates values on grow and on swap-with-last erase.
static HashMap gBundleCache;
static SDL_SpinLock g_BundleCacheLock;

// Look up a cached bundle by key under the cache lock. The returned SceneBundle* is stable (each
// bundle is allocated separately and never moves in the map), so it stays valid after the lock is
// released. out: NULL when no entry has that key.
static SceneBundle* BundleCacheFindBundle(u64 key)
{
    SDL_LockSpinlock(&g_BundleCacheLock);
    BundleCacheEntry* entry = (BundleCacheEntry*)HMFind(&gBundleCache, key);
    SceneBundle* bundle = entry ? entry->bundle : NULL;
    SDL_UnlockSpinlock(&g_BundleCacheLock);
    return bundle;
}

// Persists a freshly baked bundle's .abm mesh cache to disk on a worker thread. data is the cache
// key: the bundle is re-found by key and a reference is held for the duration, so its resident
// geometry stays alive while SaveGLTFBinary reads it (the save never mutates it).
typedef struct BundleSaveTask_
{
    u64  cacheKey;
    char abmPath[1024 - sizeof(u64)];
} BundleSaveTask;

static s32 SaveBundleCacheTask(void* data)
{
    BundleSaveTask* task = (BundleSaveTask*)data;
    SceneBundle* bundle = BundleCacheFindBundle(task->cacheKey);
    if (!bundle) return 0;
    return SaveGLTFBinary(bundle, task->abmPath);
}

static void SaveBundleCacheDone(void* data, s32 result)
{
    BundleSaveTask* task = (BundleSaveTask*)data;
    if (!result) AX_WARN("abm cache save failed: %s", task->abmPath);
    BundleCacheReleaseKey(task->cacheKey); // drop the reference held for the save
    SDL_free(task);
}

// Build + queue (or, on spawn failure, run) the async .abm persist for a just-baked bundle. The
// caller must already hold the reference this releases (incremented under the cache lock at insert).
static void BundleCacheQueueSave(const char* path, u64 key)
{
    BundleSaveTask* task = (BundleSaveTask*)SDL_calloc(1, sizeof(BundleSaveTask));
    if (!task) { BundleCacheReleaseKey(key); return; }

    int pathLen = StringLength(path);
    MemCopy(task->abmPath, path, (size_t)pathLen + 1);
    ChangeExtension(task->abmPath, pathLen, "abm");
    task->cacheKey = key;

	AsyncRun("Save Bundle Cache", SaveBundleCacheTask, SaveBundleCacheDone, task);
}

static void BVHCallback(void* data, s32 result)
{
    (void)data;
    if (result) AX_LOG("bvh creation success");
    else AX_LOG("bvh creation skipped/failed");
}

// Builds the picking BVH off the main thread. data is the cache key (not a pointer): entries move in
// the map, so we re-find by key under the lock to read the bundle and to publish the result.
static s32 CreateBVH(void* data)
{
    u64 key = (u64)(uintptr_t)data;

    SceneBundle* bundle = BundleCacheFindBundle(key);
    if (!bundle) return 0;

    // Build using the stable bundle pointer (the SceneBundle is allocated separately, it never moves).
    // Results land in a local entry; bvhNodes/bvhTris are SDL_malloc'd and stable once built. A second
    // build is prevented by the !entry->bvhNodes check when publishing below (only one task is spawned
    // per bundle anyway, on the fresh insert).
    BundleCacheEntry built;
    MemsetZero(&built, sizeof(built));
    built.bundle = bundle;
    if (!BVH_BuildBundleCached(bundle, &built, bundle->numSkins > 0))
        return 0;

    SDL_LockSpinlock(&g_BundleCacheLock);
    BundleCacheEntry* entry = (BundleCacheEntry*)HMFind(&gBundleCache, key);
    if (entry && !entry->bvhNodes)
    {
        entry->bvhNodes    = built.bvhNodes;
        entry->bvhTris     = built.bvhTris;
        entry->numBvhNodes = built.numBvhNodes;
        entry->numBvhTris  = built.numBvhTris;
        SDL_UnlockSpinlock(&g_BundleCacheLock);
        return 1;
    }
    // entry was released while we built (or another build won the race): drop our copy
    SDL_UnlockSpinlock(&g_BundleCacheLock);
    BVH_FreeBundle(&built);
    return 0;
}

// out: cache entry with one reference added, NULL on load failure. The returned pointer is only
// valid until the next map mutation; callers use it transiently and store the key, not the pointer.
BundleCacheEntry* BundleCacheAcquire(const char* path)
{
    u64 key = StringToHash64(path);

    SDL_LockSpinlock(&g_BundleCacheLock);
    if (gBundleCache.valueSize == 0)
        gBundleCache = HMCreate(64u, sizeof(BundleCacheEntry));
    BundleCacheEntry* entry = (BundleCacheEntry*)HMFind(&gBundleCache, key);
    if (entry)
    {
        entry->refCount++;
        AX_LOG("bundle cache hit: %s refs=%d", path, entry->refCount);
        SDL_UnlockSpinlock(&g_BundleCacheLock);
        return entry;
    }
    SDL_UnlockSpinlock(&g_BundleCacheLock);

    // Load/bake outside the lock (slow). Only one importer touches a given path at a time
    // (the async op guard plus callbacks running after the worker finishes), so no double bake.
    SceneBundle* bundle = (SceneBundle*)AllocZeroTLSFGlobal(1, sizeof(SceneBundle));
    void* vertexHeapPtr = NULL;
    void* indexHeapPtr = NULL;
    bool baked = false;
    if (!LoadBundleMeshCached(path, bundle, &vertexHeapPtr, &indexHeapPtr, &baked))
    {
        DeAllocateTLSFGlobal(bundle);
        return NULL;
    }
    int pathLen = StringLength(path);
    char* pathCopy = (char*)AllocateTLSFGlobal(pathLen + 1);
    MemCopy(pathCopy, path, pathLen + 1);

    BundleCacheEntry value = { bundle, pathCopy, 1u };
    value.vertexHeapPtr = vertexHeapPtr;
    value.indexHeapPtr = indexHeapPtr;
    SDL_LockSpinlock(&g_BundleCacheLock);
    entry = (BundleCacheEntry*)HMInsert(&gBundleCache, key, &value);
    if (entry && baked) entry->refCount++; // hold a reference for the async cache save
    SDL_UnlockSpinlock(&g_BundleCacheLock);

    // Persist the freshly baked .abm/.bdc cache on a worker thread (it holds the reference above and
    // reads the resident geometry read-only). A plain cache hit already has its cache on disk.
    if (baked)
        BundleCacheQueueSave(path, key);

    // BVH builds asynchronously and addresses the entry by key, so it is safe across later inserts.
	AsyncRun("Create BVH", CreateBVH, BVHCallback, (void*)(uintptr_t)key);
    return entry;
}

void BundleCacheReleaseKey(u64 key)
{
    SDL_LockSpinlock(&g_BundleCacheLock);
    BundleCacheEntry* entry = (BundleCacheEntry*)HMFind(&gBundleCache, key);
    if (!entry || entry->refCount == 0) { SDL_UnlockSpinlock(&g_BundleCacheLock); return; }
    if (--entry->refCount > 0) { SDL_UnlockSpinlock(&g_BundleCacheLock); return; }

    // Snapshot what we need to free, erase the entry, then free outside the lock. Freeing through the
    // snapshot is safe: bvh arrays and geometry heap blocks are stable heap allocations, not in the map.
    BundleCacheEntry freed = *entry;
    HMErase(&gBundleCache, key);
    SDL_UnlockSpinlock(&g_BundleCacheLock);

    // geometry returns to the mega buffers. the rest of the bundle cpu data stays
    // allocated, consistent with the engine teardown ownership model
    BVH_FreeBundle(&freed);
    Scene_FreeBundleGeometry(freed.bundle, &freed, freed.bundle->numSkins > 0);
    DeAllocateTLSFGlobal(freed.path);
}

void BundleCacheRelease(const char* path)
{
    BundleCacheReleaseKey(StringToHash64(path));
}

bool FindCacheForSceneBundle(const Scene* scene, u32 bundleIdx, BundleCacheEntry* out)
{
    if (bundleIdx >= scene->numBundles || !scene->bundleRefs[bundleIdx].bundle) return false;
    const SceneBundleRef* ref = &scene->bundleRefs[bundleIdx];

    // Copy the entry out under the lock, addressed by key: gBundleCache relocates entries on grow
    // and on swap-with-last erase, so a stored/returned pointer could dangle. The copied
    // bvhNodes/bvhTris are stable heap allocations that live while the bundle is referenced.
    SDL_LockSpinlock(&g_BundleCacheLock);
    const BundleCacheEntry* entry = (const BundleCacheEntry*)HMFind(&gBundleCache, ref->cacheKey);
    if (entry) *out = *entry;
    SDL_UnlockSpinlock(&g_BundleCacheLock);
    return entry != NULL;
}
