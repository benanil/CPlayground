#include "Source/Terrain/TerrainInternal.h"
#include "Include/Camera.h"
#include "Include/Terrain.h"
#include "Include/Rendering.h"
#include "Include/Platform.h"
#include "Include/Scene.h"
#include "Include/Memory.h"
#include "Include/JobSystem.h"
#include "Include/DataStructures/HashMap.h"
#include "Include/Bitset.h"
#include "Math/Bitpack.h"

#define T_MARCHING_DRAW_DISTANCE 250.0f

typedef struct tMarchingTerrainState_
{
    tDensityGenerator generator;
	JobSystem* jobSystem;
    tBuildJob  buildJobs[T_MAX_BUILD_JOBS];
    TerrainChunkDraw* chunkDraws; // per-frame heap ranges, one indirect multidraw
    tChunk  chunks[T_MAX_CHUNKS];
    HashMap chunkLookup; // key: tChunkKey, value: u32 index into chunks
	const FrustumPlanes* frustum;
	u64*    occupiedChunksBitset;
	s8*     chunkDensity; // T_MAX_BUILD_JOBS * T_SAMPLES_TOTAL, one (T_CHUNK_CELLS+3)^3 slab per build-job slot
    u32     numChunkDraws;
	u32     chunkCount;
    u32     builtThisFrame; // jobs scheduled this frame, capped by T_MAX_BUILDS_PER_FRAME
    u32     cacheVertices;  // live + pending chunk vertices resident in GeometryBuffer_TerrainVertNew
    u32     cacheIndices;   // live + pending chunk indices resident in GeometryBuffer_TerrainIndex2
    u32     frameIndex;
    u32     culledChunks;
    u32     emptyChunks;
    u32     physicsSyncCursor;
    SDL_AtomicInt physicsInvalidated;
    SDL_AtomicInt heapPressure;
    u64    lastStatsTicks;
    float3 brushPos;
    f32    brushRadius;
    bool   brushActive;
    bool   initialized;
} tMarchingTerrain;

extern Camera g_Camera;
extern Graphics gGFX; // geometry heap CPU mirrors (defined in Graphics.c)

static tMarchingTerrain gMarchingTerrain;

static void tPruneChunkCache(u32 targetVertices, u32 targetIndices, bool respectKeepFrames);
static void tClearChunkCache(void);

static bool tAABBVisible(float3 aabbMin, float3 aabbMax, const FrustumPlanes* frustum)
{
    return CheckAABBCulled(Vec3Load(&aabbMin.x), Vec3Load(&aabbMax.x), frustum->planes);
}

// the engine density field (TerrainDensity.c noise) plugged into the port's generator
// contract: tDensityGeneratorGetValue returns -y + noise3D, and the port treats
// positive values as solid, so returning y - At makes the total exactly -At.
// TerrainDensity_At = SDF + sculpt edits, so editor manipulation shows up in the mesh.
static f32 tDensityNoise(f32 x, f32 y, f32 z, void* userData) {
    (void)userData;
    return y - TerrainDensity_At(x, y, z);
}

// thread safe: TerrainDensity_SampleChunk is pure + edit overlay is mutex guarded
static void tBuildDensity(int3 chunkMin, s8* out)
{
    const s32 densitySize = T_SAMPLES_AXIS;
    s32 cx = chunkMin.x / T_CHUNK_CELLS;
    s32 cy = chunkMin.y / T_CHUNK_CELLS;
    s32 cz = chunkMin.z / T_CHUNK_CELLS;
    s8 samples[T_SAMPLES_TOTAL];
    TerrainDensity_SampleChunk(cx, cy, cz, samples);

    for (s32 x = 0; x < T_SAMPLES_AXIS; x++)
    for (s32 y = 0; y < T_SAMPLES_AXIS; y++)
    for (s32 z = 0; z < T_SAMPLES_AXIS; z++)
    {
        size_t dst = (size_t)x * (size_t)densitySize * (size_t)densitySize + (size_t)y * (size_t)densitySize + (size_t)z;
        size_t src = ((size_t)z * (size_t)densitySize + (size_t)y) * (size_t)densitySize + (size_t)x;
        out[dst] = samples[src];
    }
}

static v128f tUnpackNormal(u32 packed)
{
    f32 x = (f32)((s32)(packed << 21) >> 21) * (1.0f / 1023.0f);
    f32 y = (f32)((s32)(packed << 10) >> 21) * (1.0f / 1023.0f);
    return OctDecode(VecSetR(x, y, 0.0f, 0.0f));
}

// runs on a worker: reads only the job's own scratch mesh and buffers, plus the
// thread safe density/edit/params reads inside tTerrainMaterial. emits an indexed mesh
// (shared source vertices materialized once, triangles reference them) so shared verts
// are shaded once instead of once per triangle, and the GPU draws indexed
static void tAppendMeshSlotTriangles(tBuildJob* job)
{
	const tMeshData* mesh = &job->scratchMesh;
    if (!mesh || !mesh->vertices || !mesh->indices || !job->buildVertices || !job->buildIndices)
        return;

    size_t vertexCount = (size_t)mesh->numVertices;
    size_t indexCount = (size_t)mesh->numIndices;
    if (vertexCount == 0 || indexCount < 3)
        return;

    if (job->buildVertexCount + vertexCount > T_MARCHING_VERTEX_CAP)
        return;
    u32 base = job->buildVertexCount;

    v128f offset = VecI32ToF32(VeciLoad((const u32*)&job->min.x));
    f32 chunkSize = (f32)(T_CHUNK_CELLS);
    for (size_t v = 0; v < vertexCount; v++)
    {
        v128f local = Unpack16x4Fixed(mesh->vertices[v].position, chunkSize);
        v128f p = VecAdd(local, offset);
        // Safe normalize: flat or nearly collapsed triangles can leave zero-length
        // normals; a plain normalize would spray NaN colors.
        float3 world = Vec3Get(p);
        float3 n = Vec3Get(tUnpackNormal(mesh->vertices[v].normal));
        u32 materials, blend;
        tTerrainMaterial(world, n, &materials, &blend);
		tVertexData vertex = {0};
		vertex.position  = mesh->vertices[v].position;
		vertex.normal    = mesh->vertices[v].normal;
		vertex.materials = materials | (blend << 16);
		job->buildVertices[job->buildVertexCount++] = vertex;
    }

    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
        if (job->buildIndexCount + 3u > T_CHUNK_INDEX_CAP)
            return;

        u32 ia = mesh->indices[i + 0];
        u32 ib = mesh->indices[i + 1];
        u32 ic = mesh->indices[i + 2];
        if ((size_t)ia >= vertexCount || (size_t)ib >= vertexCount || (size_t)ic >= vertexCount)
            continue;

        v128f pa = Unpack16x4Fixed(mesh->vertices[ia].position, chunkSize);
        v128f pb = Unpack16x4Fixed(mesh->vertices[ib].position, chunkSize);
        v128f pc = Unpack16x4Fixed(mesh->vertices[ic].position, chunkSize);

        // Skip zero-area marching-cubes triangles instead of shading degenerate faces.
        v128f ab = VecSub(pb, pa);
        v128f ac = VecSub(pc, pa);
        v128f cross = Vec3Cross(ab, ac);
        if (Vec3DotfV(cross, cross) <= 1.0e-12f)
            continue;

        job->buildIndices[job->buildIndexCount++] = base + ia;
        job->buildIndices[job->buildIndexCount++] = base + ib;
        job->buildIndices[job->buildIndexCount++] = base + ic;
    }
}

static void SaturatingSubtractU32(u32* value, u32 amount)
{
    if (*value >= amount) *value -= amount; else *value = 0u;
}

static void tFreeMeshHandle(tMeshHandle* mesh)
{
    if (mesh->vertices.heapPtr) {
        GeometryHeapFree(GeometryBuffer_TerrainVert, mesh->vertices.heapPtr);
        SaturatingSubtractU32(&gMarchingTerrain.cacheVertices, mesh->vertices.count);
    }
    if (mesh->indices.heapPtr) {
        GeometryHeapFree(GeometryBuffer_TerrainIndex, mesh->indices.heapPtr);
        SaturatingSubtractU32(&gMarchingTerrain.cacheIndices, mesh->indices.count);
    }
    DeAllocateTLSFGlobal(mesh->physics.vertices);
    DeAllocateTLSFGlobal(mesh->physics.indices);
    *mesh = (tMeshHandle){0};
}

static void tClearChunkPending(tChunk* chunk) {
    if (!chunk) return;
    chunk->pendingFrames = 0;
    chunk->pendingState = PENDING_NONE;
}

static void tFreePendingMesh(tChunk* chunk) {
    tFreeMeshHandle(&chunk->pendingMesh);
    tClearChunkPending(chunk);
}

static void tDestroyChunkPhysics(tChunk* chunk)
{
    if (!chunk) return;
    Scene_PhysicsDestroyTerrainChunk(&chunk->physicsBody, &chunk->physicsMesh);
}

static const char* tChunkStateName(ChunkBuildState state)
{
	const char* results[CHUNK_MAX_STATE] = { "UNBUILT", "QUEUED", "BUILDING", "PENDING", "READY", "FAILED"};
	return results[state % CHUNK_MAX_STATE];
}

static bool tChunkStateTransitionAllowed(ChunkBuildState oldState, ChunkBuildState newState)
{
    switch (oldState) {
        case CHUNK_UNBUILT:  return newState == CHUNK_QUEUED || newState == CHUNK_BUILDING;
        case CHUNK_QUEUED:   return newState == CHUNK_BUILDING;
        case CHUNK_BUILDING: return newState == CHUNK_QUEUED || newState == CHUNK_PENDING || newState == CHUNK_READY || newState == CHUNK_FAILED;
        case CHUNK_PENDING:  return newState == CHUNK_READY;
        case CHUNK_READY:    return newState == CHUNK_PENDING;
        case CHUNK_FAILED:   return newState == CHUNK_QUEUED || newState == CHUNK_BUILDING;
        default: return false;
    }
}

static void tSetChunkState(tChunk* chunk, ChunkBuildState newState)
{
    if (!chunk) return;
    ChunkBuildState oldState = chunk->buildState;
    if (oldState == newState) return;
    if (!tChunkStateTransitionAllowed(oldState, newState)) {
        AX_WARN("invalid marching terrain chunk state transition: %s -> %s", tChunkStateName(oldState), tChunkStateName(newState));
    }
    chunk->buildState = newState;
    if (newState != CHUNK_PENDING)
        chunk->pendingFrames = 0;
    if (newState == CHUNK_READY)
    {
        if (chunk->mesh.vertices.heapPtr && chunk->mesh.vertices.count >= 3u)
            chunk->physicsDirty = true;
        else {
            tDestroyChunkPhysics(chunk);
            chunk->physicsDirty = false;
        }
    }
    else if (newState == CHUNK_FAILED) {
        tDestroyChunkPhysics(chunk);
        chunk->physicsDirty = false;
    }
}

static void tSetChunkPending(tChunk* chunk, PendingMeshState pendingState)
{
    if (!chunk) return;
    chunk->pendingState = pendingState;
    chunk->pendingFrames = 2u;
    tSetChunkState(chunk, CHUNK_PENDING);
}

static void tSyncChunkPhysics(tChunk* chunk)
{
    if (!chunk || !chunk->mesh.vertices.heapPtr || chunk->mesh.vertices.count < 3u ||
        !chunk->mesh.physics.vertices || !chunk->mesh.physics.indices || chunk->mesh.physics.indexCount < 3u) {
        tDestroyChunkPhysics(chunk);
        return;
    }

    // owns its collider directly now (no shared slot pool/cap), so there's nothing to
    // acquire here - just create-or-update in place.
    if (!Scene_PhysicsSyncTerrainChunkMesh(&chunk->physicsBody, &chunk->physicsMesh,
                                           chunk->mesh.physics.vertices, chunk->mesh.physics.vertexCount,
                                           chunk->mesh.physics.indices, chunk->mesh.physics.indexCount))
        tDestroyChunkPhysics(chunk);
}

void tInvalidatePhysics(void) {
    SDL_SetAtomicInt(&gMarchingTerrain.physicsInvalidated, 1);
}

static void BeginBuildJob(tBuildJob* job)
{
    job->mesh = (tMeshHandle){0};
    job->failed = false;
    job->buildVertices = NULL;
    job->buildVertexCount = 0;
    job->buildIndices = NULL;
    job->buildIndexCount = 0;

    ArenaScratchBegin(&job->scratchArena);
}

static bool PrepareBuildScratch(tBuildJob* job)
{
    job->buildVertices = (tVertexData*)ArenaPushGlobal(sizeof(tVertexData) * T_MARCHING_VERTEX_CAP);
    job->buildIndices = (u32*)ArenaPushGlobal(sizeof(u32) * T_CHUNK_INDEX_CAP);
    if (!job->buildVertices || !job->buildIndices) {
        AX_WARN("marching terrain build scratch allocation failed");
        return false;
    }
    return true;
}

static bool GenerateChunkMesh(tBuildJob* job)
{
    // keyed by build-job slot, not job->chunkIndex: the chunk-cache evictor can remap a
    // busy job's chunkIndex from its own thread mid-build (tFreeChunkSlot's swap-compact),
    // so that field isn't stable for the worker to index into. the job slot itself never
    // moves for the life of RunBuildJob, so it's the only race-free key here.
    u32 jobSlot = (u32)(job - gMarchingTerrain.buildJobs);
    s8* density = gMarchingTerrain.chunkDensity + (size_t)jobSlot * T_SAMPLES_TOTAL;
    tBuildDensity(job->min, density);
    if (!tMesherMesh(&gMarchingTerrain.generator, density, job)) {
        AX_WARN("marching cubes chunk mesh build failed");
        return false;
    }
    tAppendMeshSlotTriangles(job);
	return true;
}

static bool BuildPhysicsMesh(tBuildJob* job, u32 vertexCount, u32 indexCount)
{
    if (indexCount < 3u) return true;

    job->mesh.physics.vertexCount = vertexCount;
    job->mesh.physics.indexCount = indexCount;
    job->mesh.physics.vertices = (b3Vec3*)AllocateTLSFGlobal(sizeof(b3Vec3) * vertexCount);
    job->mesh.physics.indices = (s32*)AllocateTLSFGlobal(sizeof(s32) * indexCount);
    if (job->mesh.physics.vertices && job->mesh.physics.indices)
    {
        f32 chunkSize = (f32)(T_CHUNK_CELLS);
        v128f offset = VecI32ToF32(VeciLoad((const u32*)&job->min.x));
        for (u32 v = 0; v < vertexCount; v++) {
            v128f local = Unpack16x4Fixed(job->buildVertices[v].position, chunkSize);
            v128f p = VecAdd(local, offset);
            job->mesh.physics.vertices[v] = (b3Vec3){ VecGetX(p), VecGetY(p), VecGetZ(p) };
        }
        for (u32 k = 0; k < indexCount; k++)
            job->mesh.physics.indices[k] = (s32)job->buildIndices[k];
        return true;
    }

    AX_WARN("marching terrain physics mesh allocation failed");
    DeAllocateTLSFGlobal(job->mesh.physics.vertices);
    DeAllocateTLSFGlobal(job->mesh.physics.indices);
    job->mesh.physics = (PhysicsMesh){0};
    return true;
}

static bool UploadChunkMesh(tBuildJob* job)
{
    u32 vertexCount = job->buildVertexCount;
    u32 indexCount = job->buildIndexCount;
    if (vertexCount == 0 || indexCount == 0)
        return true;

    void* raw = NULL;
    u32 first = GeometryHeapAlloc(GeometryBuffer_TerrainVert, vertexCount, &raw);
    void* idxRaw = NULL;
    u32 idxFirst = GeometryHeapAlloc(GeometryBuffer_TerrainIndex, indexCount, &idxRaw);
    if (first == GEOMETRY_ALLOC_FAIL || idxFirst == GEOMETRY_ALLOC_FAIL)
    {
        if (first != GEOMETRY_ALLOC_FAIL) GeometryHeapFree(GeometryBuffer_TerrainVert, raw);
        if (idxFirst != GEOMETRY_ALLOC_FAIL) GeometryHeapFree(GeometryBuffer_TerrainIndex, idxRaw);
		SDL_SetAtomicInt(&gMarchingTerrain.heapPressure, 1);
        return false;
    }

    MemCopy((tVertexData*)gGFX.TerrainVertexBuffer + first, job->buildVertices, vertexCount * sizeof(tVertexData));
    Rendering_QueueGeometryUpload(GeometryBuffer_TerrainVert, first, first + vertexCount);
    MemCopy((u32*)gGFX.TerrainIndexBuffer + idxFirst, job->buildIndices, indexCount * sizeof(u32));
    Rendering_QueueGeometryUpload(GeometryBuffer_TerrainIndex, idxFirst, idxFirst + indexCount);
    job->mesh.vertices = (GeometryRange){ raw, first, vertexCount };
    job->mesh.indices = (GeometryRange){ idxRaw, idxFirst, indexCount };
    return BuildPhysicsMesh(job, vertexCount, indexCount);
}

static void FinishBuildJob(tBuildJob* job) {
    job->buildVertices = NULL;
    job->buildIndices = NULL;
    ArenaScratchEnd(&job->scratchArena);
}

static void FailBuildJob(tBuildJob* job) {
    job->failed = true;
    tFreeMeshHandle(&job->mesh);
}

// worker-side chunk build: density -> marching cubes mesh -> indexed terrain geometry ->
// parked heap ranges. touches only the job slot; shared systems synchronize internally.
static void RunBuildJob(void* userData)
{
    tBuildJob* job = (tBuildJob*)userData;
    BeginBuildJob(job);
    bool failed = !PrepareBuildScratch(job) ||
			      !GenerateChunkMesh(job) ||
			      !UploadChunkMesh(job);
	if (failed) FailBuildJob(job);
   
    FinishBuildJob(job);
}

// picks a free job slot and launches the chunk build on a worker. false when the
// per-frame cap is hit, every slot is busy, or the job queue is full; the chunk keeps
// its dirty flag in those cases and retries next frame
static bool ChunkBuildInFlight(u32 chunkIndex)
{
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++) {
        tBuildJob* job = &gMarchingTerrain.buildJobs[i];
        if (job->busy && job->chunkIndex == chunkIndex)
            return true;
    }
    return false;
}

static bool ScheduleChunkBuild(u32 chunkIndex, tChunk* chunk)
{
    if (gMarchingTerrain.builtThisFrame >= T_MAX_BUILDS_PER_FRAME) {
        if (chunk->buildState == CHUNK_UNBUILT || chunk->buildState == CHUNK_FAILED)
            tSetChunkState(chunk, CHUNK_QUEUED);
        return false;
    }
    if (gMarchingTerrain.cacheVertices > T_VERTEX_CACHE_BUDGET ||
        gMarchingTerrain.cacheIndices > T_INDEX_CACHE_BUDGET)
    {
        tPruneChunkCache(T_VERTEX_CACHE_TARGET, T_INDEX_CACHE_TARGET, true);
    }

    tBuildJob* job = NULL;
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++) {
        if (!gMarchingTerrain.buildJobs[i].busy) {
            job = &gMarchingTerrain.buildJobs[i];
            break;
        }
    }

    if (!job) {
        if (chunk->buildState == CHUNK_UNBUILT || chunk->buildState == CHUNK_FAILED)
            tSetChunkState(chunk, CHUNK_QUEUED);
        return false;
    }

    job->chunkIndex = chunkIndex;
    job->min = chunk->min;
    job->busy = true;
    // dirty clears at schedule time: a sculpt landing while the job flies re-sets it
    // and the chunk reschedules after integration, so stale results self-heal
    chunk->dirty = false;
    if (!chunk->mesh.vertices.heapPtr && chunk->buildState != CHUNK_READY)
        tSetChunkState(chunk, CHUNK_BUILDING);
    job->handle = JobSystem_Execute(gMarchingTerrain.jobSystem, RunBuildJob, job);
    if (job->handle == 0)
    {
        job->busy = false;
        chunk->dirty = true;
        if (chunk->buildState == CHUNK_BUILDING)
            tSetChunkState(chunk, CHUNK_QUEUED);
        return false;
    }
    gMarchingTerrain.builtThisFrame++;
    return true;
}

// main thread, once per frame before the pending promote: moves finished job results
// into the chunks' pending slots so they run through the usual 2-frame promote
static void IntegrateFinishedBuilds(void)
{
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++)
    {
        tBuildJob* job = &gMarchingTerrain.buildJobs[i];
        if (!job->busy || !JobSystem_IsJobDone(gMarchingTerrain.jobSystem, job->handle))
            continue;

        tChunk* chunk = &gMarchingTerrain.chunks[job->chunkIndex];
        job->busy = false;

        // cache this build's density grid on the chunk itself (job slot i's scratch region,
        // still valid: the slot isn't reused until job->busy is cleared here). Foliage
        // placement reads this instead of resampling the field (noise + edit overlay),
        // which is expensive - recomputing it per consumer was the whole point of caching it.
        {
            const s8* builtDensity = gMarchingTerrain.chunkDensity + (size_t)i * T_SAMPLES_TOTAL;
            if (!chunk->density) chunk->density = (s8*)AllocateTLSFGlobal(T_SAMPLES_TOTAL);
            if (chunk->density) MemCopy(chunk->density, builtDensity, T_SAMPLES_TOTAL);
        }

        if (job->failed) {
            // dropped (mesher failure / heap full); a later invalidation re-dirties it
            tFreeMeshHandle(&job->mesh);
            if (!chunk->mesh.vertices.heapPtr && chunk->pendingState == PENDING_NONE)
                tSetChunkState(chunk, CHUNK_FAILED);
            continue;
        }

        tFreePendingMesh(chunk);
        if (job->mesh.vertices.count == 0)
        {
            if (chunk->mesh.vertices.heapPtr) {
                // genuinely empty now: retire the live mesh through the promote delay
                tSetChunkPending(chunk, PENDING_EMPTY);
            }
            else {
                tSetChunkState(chunk, CHUNK_READY);
            }
            gMarchingTerrain.emptyChunks++;
            continue;
        }

        chunk->pendingMesh = job->mesh;
        tSetChunkPending(chunk, PENDING_MESH);
        gMarchingTerrain.cacheVertices += job->mesh.vertices.count;
        gMarchingTerrain.cacheIndices += job->mesh.indices.count;
        job->mesh = (tMeshHandle){0};
    }
}

// waits for every in-flight build and discards results that never got integrated;
// must run before anything invalidates chunk indices or tears the system down
static void tDrainBuildJobs(void)
{
    if (!gMarchingTerrain.jobSystem) return;
    JobSystem_Wait(gMarchingTerrain.jobSystem);
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++)
    {
        tBuildJob* job = &gMarchingTerrain.buildJobs[i];
        if (!job->busy) continue;
        tFreeMeshHandle(&job->mesh);
        job->busy = false;
        tChunk* chunk = &gMarchingTerrain.chunks[job->chunkIndex];
        if (chunk->buildState == CHUNK_BUILDING)
            tSetChunkState(chunk, CHUNK_QUEUED);
    }
}

static void tRemapBuildJobChunkIndex(u32 oldIndex, u32 newIndex)
{
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++)
    {
        tBuildJob* job = &gMarchingTerrain.buildJobs[i];
        if (job->busy && job->chunkIndex == oldIndex)
            job->chunkIndex = newIndex;
    }
}

static void tFreeChunkSlot(u32 index)
{
    if (index >= gMarchingTerrain.chunkCount) return;
    tChunk* chunk = &gMarchingTerrain.chunks[index];
    HMErase(&gMarchingTerrain.chunkLookup, tChunkKey(chunk->min));
    tDestroyChunkPhysics(chunk);
    tFreeMeshHandle(&chunk->mesh);
    tFreePendingMesh(chunk);
    tFoliage_DestroyChunkFoliage(chunk);
    DeAllocateTLSFGlobal(chunk->density);
    chunk->density = NULL;

    u32 lastIndex = gMarchingTerrain.chunkCount - 1u;
    if (index != lastIndex)
    {
        gMarchingTerrain.chunks[index] = gMarchingTerrain.chunks[lastIndex];
        u64 movedKey = tChunkKey(gMarchingTerrain.chunks[index].min);
        HMInsertOrAssign(&gMarchingTerrain.chunkLookup, movedKey, &index);
        tRemapBuildJobChunkIndex(lastIndex, index);
    }

    MemsetZero(&gMarchingTerrain.chunks[lastIndex], sizeof(gMarchingTerrain.chunks[lastIndex]));
    BitsetReset(gMarchingTerrain.occupiedChunksBitset, (s32)lastIndex);
    gMarchingTerrain.chunkCount--;
    if (gMarchingTerrain.physicsSyncCursor >= gMarchingTerrain.chunkCount)
        gMarchingTerrain.physicsSyncCursor = 0u;
}

static bool tFreeOldestChunkSlot(bool requireMesh, bool respectKeepFrames)
{
    u32 evictIndex  = UINT32_MAX;
    u32 oldestFrame = UINT32_MAX;
    for (u32 i = 0; i < gMarchingTerrain.chunkCount; i++)
    {
        tChunk* chunk = &gMarchingTerrain.chunks[i];
		bool chunkVisible = tAABBVisible(chunk->aabbMin, chunk->aabbMax, gMarchingTerrain.frustum);
        if (ChunkBuildInFlight(i) || chunkVisible) continue;
        if (requireMesh && !chunk->mesh.vertices.heapPtr && !chunk->pendingMesh.vertices.heapPtr)
            continue;
        if (respectKeepFrames && chunk->lastTouchedFrame + T_CACHE_KEEP_FRAMES >= gMarchingTerrain.frameIndex)
            continue;
        if (chunk->lastTouchedFrame < oldestFrame) {
            oldestFrame = chunk->lastTouchedFrame;
            evictIndex = i;
        }
    }

    if (evictIndex == UINT32_MAX) return false;
    tFreeChunkSlot(evictIndex);
    return true;
}

u32 tGetChunkCount(void) { return gMarchingTerrain.chunkCount; }

tChunk* tGetChunkByIndex(u32 index)
{
    return index < gMarchingTerrain.chunkCount ? &gMarchingTerrain.chunks[index] : NULL;
}

tChunk* tFindChunkByMin(int3 min)
{
    u32* found = (u32*)HMFind(&gMarchingTerrain.chunkLookup, tChunkKey(min));
    return found ? &gMarchingTerrain.chunks[*found] : NULL;
}

JobSystem* tGetTerrainJobSystem(void)
{
    return tMarchingInit() ? gMarchingTerrain.jobSystem : NULL;
}

const u64* tGetOccupiedChunksBitset(void)
{
    return gMarchingTerrain.occupiedChunksBitset;
}

static u32 tAllocChunkSlot(void)
{
    if (gMarchingTerrain.chunkCount >= T_MAX_CHUNKS) 
        if (!tFreeOldestChunkSlot(false, false)) 
            tClearChunkCache(); 

    s32 index = BitsetFindFirstEmpty(gMarchingTerrain.occupiedChunksBitset, (s32)T_MAX_CHUNKS);
    if (index < 0) return UINT32_MAX;

    BitsetSet(gMarchingTerrain.occupiedChunksBitset, index);
    gMarchingTerrain.chunkCount++;
    return (u32)index;
}

static void tClearChunkCache(void)
{
	AX_LOG("marching terrain chunk cache reset");
    tDrainBuildJobs();
    RendererSetTerrainChunkDraws(NULL, 0);
    for (u32 i = 0; i < gMarchingTerrain.chunkCount; i++)
    {
        tDestroyChunkPhysics(&gMarchingTerrain.chunks[i]);
        tFreeMeshHandle(&gMarchingTerrain.chunks[i].mesh);
        tFreePendingMesh(&gMarchingTerrain.chunks[i]);
        tFoliage_DestroyChunkFoliage(&gMarchingTerrain.chunks[i]);
        DeAllocateTLSFGlobal(gMarchingTerrain.chunks[i].density);
        gMarchingTerrain.chunks[i].density = NULL;
    }
    gMarchingTerrain.chunkCount = 0;
    gMarchingTerrain.cacheVertices = 0u;
    gMarchingTerrain.cacheIndices = 0u;
    gMarchingTerrain.physicsSyncCursor = 0;
    if (gMarchingTerrain.occupiedChunksBitset)
        MemsetZero(gMarchingTerrain.occupiedChunksBitset, T_CHUNK_BITSET_WORDS * sizeof(u64));
    HMClear(&gMarchingTerrain.chunkLookup);
}

static void tPruneChunkCache(u32 targetVertices, u32 targetIndices, bool respectKeepFrames)
{
	int numFreed = 0;
	while (numFreed < 8 && (gMarchingTerrain.cacheVertices > targetVertices || gMarchingTerrain.cacheIndices > targetIndices))
	{
		if (!tFreeOldestChunkSlot(true, respectKeepFrames))
            break;
		numFreed++;
	}
}

static void tResolveHeapPressure(void)
{
    if (SDL_GetAtomicInt(&gMarchingTerrain.heapPressure) == 0)
        return;
    SDL_SetAtomicInt(&gMarchingTerrain.heapPressure, 0);
    AX_WARN("marching terrain heap pressure; pruning chunk cache");
    tPruneChunkCache(T_VERTEX_CACHE_TARGET / 2u, T_INDEX_CACHE_TARGET / 2u, false);
}

static void tPromotePendingMeshes(void)
{
    for (u32 i = 0; i < gMarchingTerrain.chunkCount; i++)
    {
        tChunk* chunk = &gMarchingTerrain.chunks[i];
        if (chunk->pendingState == PENDING_NONE)
            continue;

        if (chunk->pendingFrames > 0u) {
            chunk->pendingFrames--;
            if (chunk->pendingFrames > 0u)
                continue;
        }

        tFreeMeshHandle(&chunk->mesh);
        if (chunk->pendingState == PENDING_EMPTY) {
            tClearChunkPending(chunk);
            tSetChunkState(chunk, CHUNK_READY);
            continue;
        }

        chunk->mesh = chunk->pendingMesh;
        chunk->pendingMesh = (tMeshHandle){0};
        tClearChunkPending(chunk);
        tSetChunkState(chunk, CHUNK_READY);
    }
}

static void tSyncDirtyPhysics(void)
{
    if (SDL_GetAtomicInt(&gMarchingTerrain.physicsInvalidated) != 0)
    {
        SDL_SetAtomicInt(&gMarchingTerrain.physicsInvalidated, 0);
        for (u32 i = 0; i < gMarchingTerrain.chunkCount; i++)
        {
            tChunk* chunk = &gMarchingTerrain.chunks[i];
            if (chunk->physicsBody != 0u || (chunk->mesh.vertices.heapPtr && chunk->mesh.vertices.count >= 3u))
                chunk->physicsDirty = true;
        }
    }

    if (gMarchingTerrain.chunkCount == 0u) return;

    u32 synced = 0u;
    u32 visited = 0u;
    while (visited < gMarchingTerrain.chunkCount && synced < T_MAX_PHYSICS_SYNCS_PER_FRAME)
    {
        u32 i = gMarchingTerrain.physicsSyncCursor++;
        if (gMarchingTerrain.physicsSyncCursor >= gMarchingTerrain.chunkCount)
            gMarchingTerrain.physicsSyncCursor = 0u;
        visited++;

        tChunk* chunk = &gMarchingTerrain.chunks[i];
        if (!chunk->physicsDirty) continue;
        
		tSyncChunkPhysics(chunk);
        chunk->physicsDirty = false;
        synced++;
    }
}

static tChunk* GetOrCreateChunk(int3 min)
{
    u32* found = (u32*)HMFind(&gMarchingTerrain.chunkLookup, tChunkKey(min));
    tChunk* chunk = found ? &gMarchingTerrain.chunks[*found] : NULL;
    if (chunk)
    {
        chunk->lastTouchedFrame = gMarchingTerrain.frameIndex;
        if (chunk->dirty && !ChunkBuildInFlight(*found) && chunk->pendingState == PENDING_NONE)
            ScheduleChunkBuild(*found, chunk);
        return chunk;
    }

    u32 index = tAllocChunkSlot();
    if (index == UINT32_MAX) {
        AX_WARN("marching terrain chunk allocation failed");
        return NULL;
    }
    chunk = &gMarchingTerrain.chunks[index];
    *chunk = (tChunk){
        .min = min,
        .buildState = CHUNK_UNBUILT,
        .pendingState = PENDING_NONE,
    };
    chunk->lastTouchedFrame = gMarchingTerrain.frameIndex;
    s32 worldSize = T_CHUNK_CELLS;
    chunk->aabbMin = ToFloat3(min);
    chunk->aabbMax = F3AddF(chunk->aabbMin, (f32)worldSize);
    HMInsert(&gMarchingTerrain.chunkLookup, tChunkKey(min), &index);
    chunk->dirty = true;
    ScheduleChunkBuild(index, chunk);
    return chunk;
}

static bool tDrawChunk(const tChunk* chunk)
{
    if (!chunk || chunk->mesh.indices.count == 0 || !chunk->mesh.vertices.heapPtr || !chunk->mesh.indices.heapPtr)
        return true;

    if (gMarchingTerrain.numChunkDraws >= MAX_TERRAIN_CHUNK_DRAWS)
        return false;
    s32 step = T_CHUNK_CELLS;
    s32 chunkX = chunk->min.x / step;
    s32 chunkY = chunk->min.y / step;
    s32 chunkZ = chunk->min.z / step;
    if (chunkX < INT16_MIN || chunkX > INT16_MAX || chunkY < INT16_MIN || chunkY > INT16_MAX || chunkZ < INT16_MIN || chunkZ > INT16_MAX)
    {
        AX_WARN("terrain chunk draw skipped: chunk coord out of s16 range");
        return true;
    }

	// Marching cubes emits an indexed mesh. Indirect commands address index heap
	// ranges; using vertex ranges here made every draw fetch unrelated indices.
	TerrainChunkDraw draw;
	draw.firstIndex = chunk->mesh.indices.first;
	draw.indexCount = chunk->mesh.indices.count;
	draw.baseVertex = (s32)chunk->mesh.vertices.first;
    draw.chunkXY    = (u32)(u16)(s16)chunkX | ((u32)(u16)(s16)chunkY << 16);
    draw.chunkZLod  = (u32)(u16)(s16)chunkZ;
	gMarchingTerrain.chunkDraws[gMarchingTerrain.numChunkDraws++] = draw;
    return true;
}

static bool tChunkPresentable(const tChunk* chunk)
{
    if (!chunk) return false;
    if (chunk->mesh.vertices.heapPtr) return true;
    return chunk->buildState == CHUNK_READY && chunk->pendingState == PENDING_NONE;
}

// vertical band of chunks that can hold surface geometry for the engine density field
// one place so every traversal query, floors the range identically
static void tChunkYRange(s32 step, s32* chunkYMin, s32* chunkYMax)
{
    f32 yMin, yMax;
    TerrainDensity_GetYRange(&yMin, &yMax);
    *chunkYMin = (s32)Floorf32(yMin / (f32)step);
    *chunkYMax = (s32)Floorf32(yMax / (f32)step);
}

static bool tSubmitChunkColumn(s32 chunkX, s32 chunkZ, bool useFrustum)
{
    const s32 step = T_CHUNK_CELLS;
    s32 chunkYMin, chunkYMax;
    tChunkYRange(step, &chunkYMin, &chunkYMax);

    for (s32 y = chunkYMin; y <= chunkYMax; y++)
    {
        int3 min = { chunkX * step, y * step, chunkZ * step };
        float3 aabbMin = ToFloat3(min);
        float3 aabbMax = F3AddF(aabbMin, (f32)step);
        if (useFrustum && !tAABBVisible(aabbMin, aabbMax, gMarchingTerrain.frustum))
        {
            gMarchingTerrain.culledChunks++;
            continue;
        }

        tChunk* chunk = GetOrCreateChunk(min);
        if (!tChunkPresentable(chunk))
            continue;
        if (!tDrawChunk(chunk))
            return false;
    }

    return true;
}

static void tSubmitTerrain(bool useFrustum)
{
    const s32 step = T_CHUNK_CELLS;
	f32 drawDistance = T_MARCHING_DRAW_DISTANCE;
    s32 centerX = (s32)Floorf32(g_Camera.position.x / (f32)step);
    s32 centerZ = (s32)Floorf32(g_Camera.position.z / (f32)step);
    s32 radius = (s32)(drawDistance / (f32)step) + 1;

    // Near shells first so the build budget fills terrain around the camera.
    for (s32 shell = 0; shell <= radius; shell++)
    for (s32 z = -shell; z <= shell; z++)
    for (s32 x = -shell; x <= shell; x++)
	{
		if (Maxs32(Abss32(x), Abss32(z)) != shell)
			continue;
		if (!tSubmitChunkColumn(centerX + x, centerZ + z, useFrustum))
			return;
	}
}

static void tLogStats(void)
{
#if T_ENABLE_STATS
    u64 now = SDL_GetTicks();
    if (now - gMarchingTerrain.lastStatsTicks < 1000u)
        return;
    gMarchingTerrain.lastStatsTicks = now;
    AX_LOG("marching terrain : draws=%u built=%u cached=%u culled=%u empty=%u",
           gMarchingTerrain.numChunkDraws, gMarchingTerrain.builtThisFrame,
           gMarchingTerrain.chunkCount, gMarchingTerrain.culledChunks, gMarchingTerrain.emptyChunks);
#endif
}

static void BeginTerrainFrame(void)
{
    gMarchingTerrain.numChunkDraws = 0;
    gMarchingTerrain.builtThisFrame = 0;
    gMarchingTerrain.culledChunks = 0;
    gMarchingTerrain.emptyChunks = 0;
}

bool tMarchingInit(void)
{
    if (gMarchingTerrain.initialized)
        return true;
    MemSet(&gMarchingTerrain, 0, sizeof(gMarchingTerrain));
	
    gMarchingTerrain.jobSystem = JobSystem_Create(0, 0);
    if (!gMarchingTerrain.jobSystem)
    {
        AX_WARN("terrain job system create failed!");
        return false;
    }

    gMarchingTerrain.generator.noise3D = tDensityNoise;
    gMarchingTerrain.generator.noise3DStrength = 1.0f;

    gMarchingTerrain.chunkDraws = (TerrainChunkDraw*)AllocateTLSFGlobal(sizeof(TerrainChunkDraw) * MAX_TERRAIN_CHUNK_DRAWS);
    gMarchingTerrain.occupiedChunksBitset = (u64*)AllocateTLSFGlobal(T_CHUNK_BITSET_WORDS * sizeof(u64));
    gMarchingTerrain.chunkDensity = (s8*)AllocateTLSFGlobal(sizeof(s8) * (size_t)T_SAMPLES_TOTAL * T_MAX_BUILD_JOBS);
    gMarchingTerrain.chunkLookup = HMCreate(T_MAX_CHUNKS, sizeof(u32));
    if (!gMarchingTerrain.chunkDraws || !gMarchingTerrain.occupiedChunksBitset || !gMarchingTerrain.chunkDensity)
    {
        AX_WARN("marching terrain allocation failed");
        tDestroy();
        return false;
    }
    MemsetZero(gMarchingTerrain.occupiedChunksBitset, T_CHUNK_BITSET_WORDS * sizeof(u64));

    // one scratch mesh container and bump arena per job slot so workers never share output
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++)
    {
		if (!tMeshDataInit(&gMarchingTerrain.buildJobs[i].scratchMesh)) { tDestroy(); return false; }
		if (!ArenaScratchCreate(&gMarchingTerrain.buildJobs[i].scratchArena, T_BUILD_SCRATCH_SIZE, "terrainChunkBuild")) { tDestroy(); return false; }
    }

    gMarchingTerrain.initialized = true;
    return true;
}

void tUpdate(void)
{
    if (!tMarchingInit()) {
        RendererSetTerrainChunkDraws(NULL, 0);
        return;
    }

    IntegrateFinishedBuilds();
    tFoliage_Update();
    tResolveHeapPressure();
    tPromotePendingMeshes();
    gMarchingTerrain.frameIndex++;
    tPruneChunkCache(T_VERTEX_CACHE_TARGET, T_INDEX_CACHE_TARGET, true);
    BeginTerrainFrame();
    mat4x4 viewProj = M44Multiply(g_Camera.view, g_Camera.projection);
    FrustumPlanes frustum = CreateFrustumPlanesRevZ(viewProj);
	gMarchingTerrain.frustum = &frustum;

    tSubmitTerrain(true);
    if (gMarchingTerrain.numChunkDraws == 0) {
        tSubmitTerrain(false);
    }

    tSyncDirtyPhysics();
    tLogStats();
    RendererSetTerrainChunkDraws(gMarchingTerrain.chunkDraws, gMarchingTerrain.numChunkDraws);
    RendererSetTerrainBrush(gMarchingTerrain.brushPos, gMarchingTerrain.brushActive ? gMarchingTerrain.brushRadius : 0.0f);
}

void tInvalidateAll(void)
{
    if (gMarchingTerrain.initialized)
        tClearChunkCache();
}

void tInvalidateRegion(float3 mn, float3 mx)
{
    if (!gMarchingTerrain.initialized)
        return;

    for (u32 i = 0; i < gMarchingTerrain.chunkCount; i++)
    {
        tChunk* chunk = &gMarchingTerrain.chunks[i];
        // the chunk's 19^3 sample grid reaches one voxel below aabbMin and two above
        // aabbMax, and gradients read one more; pad like TerrainRemeshRegion does
        f32 pad = 2.0f;
        if (mx.x < chunk->aabbMin.x - pad || mn.x > chunk->aabbMax.x + pad ||
            mx.y < chunk->aabbMin.y - pad || mn.y > chunk->aabbMax.y + pad ||
            mx.z < chunk->aabbMin.z - pad || mn.z > chunk->aabbMax.z + pad)
        {
            continue;
        }

        chunk->dirty = true;
    }
}

void tSetBrushCursor(float3 position, f32 radius, bool active)
{
    gMarchingTerrain.brushPos = position;
    gMarchingTerrain.brushRadius = radius;
    gMarchingTerrain.brushActive = active && radius > 0.0f;
}

void tMarchingDestroy()
{
    if (!gMarchingTerrain.initialized && !gMarchingTerrain.jobSystem && !gMarchingTerrain.chunkDraws &&
        !gMarchingTerrain.occupiedChunksBitset && !gMarchingTerrain.chunkLookup.keys) {
        return;
    }

    RendererSetTerrainChunkDraws(NULL, 0);
    tClearChunkCache(); // drains in-flight builds first
    tFoliage_DrainJobs();
    HMDestroy(&gMarchingTerrain.chunkLookup);
    DeAllocateTLSFGlobal(gMarchingTerrain.chunkDraws);
    DeAllocateTLSFGlobal(gMarchingTerrain.occupiedChunksBitset);
    DeAllocateTLSFGlobal(gMarchingTerrain.chunkDensity);
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++) {
        tMeshDataDestroy(&gMarchingTerrain.buildJobs[i].scratchMesh);
        ArenaScratchDestroy(&gMarchingTerrain.buildJobs[i].scratchArena);
    }
    if (gMarchingTerrain.jobSystem)
        JobSystem_Destroy(gMarchingTerrain.jobSystem);
    MemSet(&gMarchingTerrain, 0, sizeof(gMarchingTerrain));
}
