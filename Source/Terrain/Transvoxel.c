#include "Source/Terrain/Transvoxel.h"
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

typedef enum ChunkBuildState_
{
    // Valid lifecycle transitions:
    // UNBUILT -> QUEUED -> BUILDING -> PENDING -> READY
    // UNBUILT -> BUILDING
    // BUILDING -> QUEUED    (job submit/drain failed; retry later)
    // BUILDING -> READY     (build produced an empty, presentable chunk)
    // BUILDING -> FAILED    (first build failed and no live/pending mesh exists)
    // READY -> PENDING      (background rebuild finished; old mesh stays visible until promote)
    // FAILED -> QUEUED/BUILDING
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

typedef struct GeometryRange_
{
    void* heapPtr;
    u32   first;
    u32   count;
} GeometryRange;

typedef struct PhysicsMesh_
{
    b3Vec3* vertices;
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

typedef struct tChunk_
{
    int3   min;
    s32    lod;
    float3 aabbMin;
    float3 aabbMax;
    // chunk mesh lives in the tVertexData geometry heap; the GPU mirror draws it
    // without any per-frame copy
    tMeshHandle mesh;
    tMeshHandle pendingMesh;
    u32   lastTouchedFrame;
    u8    pendingFrames;
    s32   neighboursMask; // bit per face (-x,-y,-z,+x,+y,+z): neighbor renders at a finer lod
    s32   physicsSlot;    // scene terrain collider slot, -1 = none. chunk indices can
	// exceed MAX_TERRAIN_PHYSICS_CHUNKS, so slots are pooled
    ChunkBuildState buildState;
    PendingMeshState pendingState;
    bool  dirty;
    bool  physicsDirty;
} tChunk;


// one in-flight chunk build on a JobSystem worker. the main thread fills the inputs,
// launches the job and reads the outputs after JobSystem_IsJobDone; exactly one job
// ever touches a chunk, so no locks on chunk state are needed
typedef struct tBuildJob_
{
    // inputs, main thread
    u32  chunkIndex;
    int3 min;
    s32  lod;
    s32  neighboursMask;
    // per-slot scratch, initialized once (ranges in the TerrainVertNew/Second/Index2 heaps)
    tMeshDataContainer scratchMesh;
    ArenaScratch scratchArena;
    // worker-local append state, valid only while the job runs (thread scratch arena)
    tVertexData* buildVertices;
    u32          buildVertexCount;
    u32*         buildIndices;
    u32          buildIndexCount;
    // output, worker; mesh ranges are zero when empty or failed
    tMeshHandle  mesh;
    JobHandle    handle;
    bool         failed;
    bool         busy;
} tBuildJob;

typedef struct tTransvoxelState_
{
    tDensityGenerator generator;
	JobSystem* jobSystem;
    tBuildJob  buildJobs[T_MAX_BUILD_JOBS];
    TerrainChunkDraw* chunkDraws; // per-frame heap ranges, one indirect multidraw
    tChunk  chunks[T_MAX_CHUNKS];
    HashMap chunkLookup; // key: tChunkKey, value: u32 index into chunks
    u64*    occupiedChunksBitset;
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
	// todo use occupiedChunkBitset
    // free-list of scene terrain collider slots; only near lod0/1 chunks hold one
    u16    physicsSlotPool[MAX_TERRAIN_PHYSICS_CHUNKS];
    u32    physicsSlotCount;
    f32    lodFactor;   // this frame's lod distance scale, read by the mask computation
    bool   fixedArea;  // fixed-area worlds are lod0-only: no lod seams, masks stay 0
    u64    lastStatsTicks;
    float3 brushPos;
    f32    brushRadius;
    bool   brushActive;
    bool   initialized;
} tTransvoxelState;

extern Camera g_Camera;
extern Graphics gGFX; // geometry heap CPU mirrors (defined in Graphics.c)

static tTransvoxelState tTransvoxel;

static void tPruneChunkCache(u32 targetVertices, u32 targetIndices, bool respectKeepFrames);
static void tClearChunkCache(void);
static bool tSubmitNode(s32 chunkX, s32 chunkZ, s32 lod, f32 lodFactor, const FrustumPlanes* frustum, bool useFrustum);

static void CalculateProceduralMaterial(float3 worldPos, float3 normal, f32 weights[4], f32* seaLevel);

static MaterialBlend SelectDominantLayers(const f32 weights[4]);
static void ApplyMaterialOverrides(float3 worldPos, f32 seaLevel, MaterialBlend* blend);
void tTerrainMaterial(float3 worldPos, float3 normal, u32* materials, u32* blend);

// the engine density field (TerrainDensity.c noise) plugged into the port's generator
// contract: tDensityGeneratorGetValue returns -y + noise3D, and the port treats
// positive values as solid, so returning y - At makes the total exactly -At.
// TerrainDensity_At = SDF + sculpt edits, so editor manipulation shows up in the mesh.
static f32 tDensityNoise(f32 x, f32 y, f32 z, void* userData) {
    (void)userData;
    return y - TerrainDensity_At(x, y, z);
}

// thread safe: TerrainDensity_SampleChunk is pure + edit overlay is mutex guarded
static void tBuildDensity(int3 chunkMin, s32 lod, f32* out)
{
    s32 densitySize = T_CHUNK_CELLS + 3;
    s32 step = T_CHUNK_CELLS << lod;
    s32 cx = chunkMin.x / step;
    s32 cy = chunkMin.y / step;
    s32 cz = chunkMin.z / step;
    s8 samples[T_SAMPLES_TOTAL];
    TerrainDensity_SampleChunk(cx, cy, cz, (u32)lod, samples);

    const f32 invScale = -(TERRAIN_SDF_CLAMP / 127.0f);
    for (s32 x = 0; x < densitySize; x++)
    for (s32 y = 0; y < densitySize; y++)
    for (s32 z = 0; z < densitySize; z++)
    {
        size_t dst = (size_t)x * (size_t)densitySize * (size_t)densitySize + (size_t)y * (size_t)densitySize + (size_t)z;
        size_t src = ((size_t)z * (size_t)densitySize + (size_t)y) * (size_t)densitySize + (size_t)x;
        out[dst] = (f32)samples[src] * invScale;
    }
}

static const f32 tLODDistance[T_LOD_COUNT] = { 48.0f, 112.0f, 240.0f, 448.0f };

static f32 ChunkColumnDistanceSq(s32 chunkX, s32 chunkZ, s32 lod)
{
    s32 step = T_CHUNK_CELLS << lod;
    f32 minX = (f32)(chunkX * step);
    f32 minZ = (f32)(chunkZ * step);
    f32 closestX = Clampf32(g_Camera.position.x, minX, minX + (f32)step);
    f32 closestZ = Clampf32(g_Camera.position.z, minZ, minZ + (f32)step);
    f32 dx = closestX - g_Camera.position.x;
    f32 dz = closestZ - g_Camera.position.z;
    return dx * dx + dz * dz;
}

// true when the quadtree node at (nodeX, nodeZ, lod) splits into finer children:
// the closest point of its XZ box lies inside the next-finer lod's distance ring.
// same test tSubmitNode uses, so the mask stays consistent with traversal
static bool tNodeSplits(s32 nodeX, s32 nodeZ, s32 lod, f32 lodFactor)
{
    if (lod <= 0) return false;
    f32 splitDistance = tLODDistance[lod - 1] * lodFactor;
    return ChunkColumnDistanceSq(nodeX, nodeZ, lod) < splitDistance * splitDistance;
}

// does the column at (chunkX, chunkZ) on this lod's grid render at a finer lod?
// walks the quadtree top-down: every ancestor must split to reach this lod, and the
// node at this lod itself must split for its children to render instead
static bool tNeighbourFiner(s32 chunkX, s32 chunkZ, s32 lod, f32 lodFactor)
{
    if (lod == 0) return false;
    for (s32 l = (s32)T_LOD_COUNT - 1; l >= lod; l--)
    {
        // arithmetic shift floors negative chunk coords onto the coarser grid
        s32 nodeX = chunkX >> (l - lod);
        s32 nodeZ = chunkZ >> (l - lod);
        if (!tNodeSplits(nodeX, nodeZ, l, lodFactor))
            return false;
    }
    return true;
}

// transvoxel neighbour mask for a chunk column: transition cells live on the COARSE
// side in this port (the mesher samples the face at half steps, i.e. the finer
// neighbour's resolution), so a bit is set when that face's neighbour renders finer.
// vertical neighbours share the column and lod, so the y bits are never set
static s32 tColumnMask(int3 min, s32 lod)
{
    if (tTransvoxel.fixedArea || lod == 0) return 0;
    s32 step = T_CHUNK_CELLS << lod;
    s32 chunkX = FloorDiv(min.x, step);
    s32 chunkZ = FloorDiv(min.z, step);
    f32 lodFactor = tTransvoxel.lodFactor;
    s32 mask = 0;
    if (tNeighbourFiner(chunkX - 1, chunkZ, lod, lodFactor)) mask |= 1;  // -x
    if (tNeighbourFiner(chunkX + 1, chunkZ, lod, lodFactor)) mask |= 8;  // +x
    if (tNeighbourFiner(chunkX, chunkZ - 1, lod, lodFactor)) mask |= 4;  // -z
    if (tNeighbourFiner(chunkX, chunkZ + 1, lod, lodFactor)) mask |= 32; // +z
    return mask;
}

static v128f tUnpackNormal(u32 packed)
{
    f32 x = (f32)((s32)(packed << 21) >> 21) * (1.0f / 1023.0f);
    f32 y = (f32)((s32)(packed << 10) >> 21) * (1.0f / 1023.0f);
    return OctDecode(VecSetR(x, y, 0.0f, 0.0f));
}

static void AppendBuildVertex(tBuildJob* job, v128f p, u32 normal, u32 materials, u32 blend)
{
    tVertexData vertex = {0};
    vertex.position = p;
    vertex.normal = normal;
    vertex.materials = materials;
    vertex.blend = blend;
    job->buildVertices[job->buildVertexCount++] = vertex;
}

// runs on a worker: reads only the job's own scratch mesh and buffers, plus the
// thread safe density/edit/params reads inside tTerrainMaterial. emits an indexed mesh
// (shared source vertices materialized once, triangles reference them) so shared verts
// are shaded once instead of once per triangle, and the GPU draws indexed
static void tAppendMeshSlotTriangles(tBuildJob* job, tMeshDataSlot slot)
{
	const tMeshData* mesh = &job->scratchMesh.mesh[slot];
    if (!mesh || !mesh->vertices || !mesh->indices || !job->buildVertices || !job->buildIndices)
        return;

    size_t vertexCount = (size_t)mesh->numVertices;
    size_t indexCount = (size_t)mesh->numIndices;
    if (vertexCount == 0 || indexCount < 3)
        return;
    // this slot's vertices land contiguously after everything emitted so far; the source
    // indices are local to the slot, so shift them by that base to stay chunk-local
    if (job->buildVertexCount + vertexCount > T_CHUNK_VERTEX_CAP)
        return;
    u32 base = job->buildVertexCount;

    v128f offset = VecI32ToF32(VeciLoad((const u32*)&job->min.x));
    for (size_t v = 0; v < vertexCount; v++)
    {
        v128f p = VecAdd(mesh->vertices[v].position, offset);
        // safe normalize: secondary-vertex snapping can leave zero-length normals, a
        // plain normalize would spray NaN colors
        float3 world = Vec3Get(p);
        float3 n = Vec3Get(tUnpackNormal(mesh->vertices[v].normal));
        u32 m, b;
        tTerrainMaterial(world, n, &m, &b);
        AppendBuildVertex(job, p, mesh->vertices[v].normal, m, b);
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

        v128f pa = VecAdd(mesh->vertices[ia].position, offset);
        v128f pb = VecAdd(mesh->vertices[ib].position, offset);
        v128f pc = VecAdd(mesh->vertices[ic].position, offset);

        // secondary-vertex snapping and transition cells produce degenerate triangles
        // (verts collapse onto each other); skip them here instead of shading zero-area
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
        SaturatingSubtractU32(&tTransvoxel.cacheVertices, mesh->vertices.count);
    }
    if (mesh->indices.heapPtr) {
        GeometryHeapFree(GeometryBuffer_TerrainIndex, mesh->indices.heapPtr);
        SaturatingSubtractU32(&tTransvoxel.cacheIndices, mesh->indices.count);
    }
    DeAllocateTLSFGlobal(mesh->physics.vertices);
    DeAllocateTLSFGlobal(mesh->physics.indices);
    *mesh = (tMeshHandle){0};
}
////
static void tClearChunkPending(tChunk* chunk) {
    if (!chunk) return;
    chunk->pendingFrames = 0;
    chunk->pendingState = PENDING_NONE;
}

static void tFreePendingMesh(tChunk* chunk) {
    tFreeMeshHandle(&chunk->pendingMesh);
    tClearChunkPending(chunk);
}

// releases the chunk's collider slot back to the pool. chunk indices are NOT valid
// collider slots (the chunk pool is 16384, the scene only has MAX_TERRAIN_PHYSICS_CHUNKS
// collider slots), so every physics call goes through the pooled chunk->physicsSlot
static void tDestroyChunkPhysics(tChunk* chunk)
{
    if (!chunk || chunk->physicsSlot < 0) 
		return;
    Scene* scene = Scene_GetActive();
    if (scene)
        Scene_PhysicsDestroyTerrainChunk(scene, (u32)chunk->physicsSlot);
    tTransvoxel.physicsSlotPool[tTransvoxel.physicsSlotCount++] = (u16)chunk->physicsSlot;
    chunk->physicsSlot = -1;
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
        AX_WARN("invalid transvoxel chunk state transition: %s -> %s", tChunkStateName(oldState), tChunkStateName(newState));
    }
    chunk->buildState = newState;
    if (newState != CHUNK_PENDING)
        chunk->pendingFrames = 0;
    if (newState == CHUNK_READY)
    {
        if (chunk->lod <= 1 && chunk->mesh.vertices.heapPtr && chunk->mesh.vertices.count >= 3u)
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
    Scene* scene = Scene_GetActive();
    if (!scene) return;
    if (!chunk || chunk->lod > 1 || !chunk->mesh.vertices.heapPtr || chunk->mesh.vertices.count < 3u ||
        !chunk->mesh.physics.vertices || !chunk->mesh.physics.indices || chunk->mesh.physics.indexCount < 3u) {
        tDestroyChunkPhysics(chunk);
        return;
    }

    if (chunk->physicsSlot < 0)
    {
        if (tTransvoxel.physicsSlotCount == 0u) {
            AX_WARN("transvoxel terrain physics slots exhausted");
            return;
        }
        chunk->physicsSlot = (s32)tTransvoxel.physicsSlotPool[--tTransvoxel.physicsSlotCount];
    }

    if (!Scene_PhysicsSyncTerrainChunkMesh(scene, (u32)chunk->physicsSlot,
                                           chunk->mesh.physics.vertices, chunk->mesh.physics.vertexCount,
                                           chunk->mesh.physics.indices, chunk->mesh.physics.indexCount))
        tDestroyChunkPhysics(chunk);
}

void tInvalidatePhysics(void)
{
    SDL_SetAtomicInt(&tTransvoxel.physicsInvalidated, 1);
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

static bool PrepareBuildScratch(tBuildJob* job, f32** density)
{
    s32 densitySize = T_CHUNK_CELLS + 3;
    size_t densityCount = (size_t)densitySize * (size_t)densitySize * (size_t)densitySize;
    *density = (f32*)ArenaPushGlobal(sizeof(f32) * densityCount);
    job->buildVertices = (tVertexData*)ArenaPushGlobal(sizeof(tVertexData) * T_CHUNK_VERTEX_CAP);
    job->buildIndices = (u32*)ArenaPushGlobal(sizeof(u32) * T_CHUNK_INDEX_CAP);
    if (!*density || !job->buildVertices || !job->buildIndices) {
        AX_WARN("transvoxel build scratch allocation failed");
        return false;
    }
    return true;
}

static bool GenerateChunkMesh(tBuildJob* job, f32* density)
{
    tBuildDensity(job->min, job->lod, density);
    if (!tTransvoxelMesherMesh(&tTransvoxel.generator, job->min, density,
                               job->lod, job->neighboursMask, &job->scratchMesh, NULL)) {
        AX_WARN("transvoxel chunk mesh build failed");
        return false;
    }
    // lod stitching: snap boundary vertices to their secondary positions (shrinks the
    // regular mesh half a cell inward on faces with a finer neighbour) and fill the gap
    // with that face's transition cell strip. faces without a finer neighbour keep
    // primary positions and skip their transition slot
    tMeshDataContainerApplySecondaryVertices(&job->scratchMesh, job->neighboursMask);
    tAppendMeshSlotTriangles(job, tMeshDataSlot_Main);
    for (u32 bit = 0; bit < 6u; bit++) 
        if (job->neighboursMask & (1 << bit))
            tAppendMeshSlotTriangles(job, (tMeshDataSlot)(tMeshDataSlot_LeftTransition + bit));
    
	return true;
}

static bool BuildPhysicsMesh(tBuildJob* job, u32 vertexCount, u32 indexCount)
{
    if (job->lod > 1 || indexCount < 3u) return true;

    job->mesh.physics.vertexCount = vertexCount;
    job->mesh.physics.indexCount = indexCount;
    job->mesh.physics.vertices = (b3Vec3*)AllocateTLSFGlobal(sizeof(b3Vec3) * vertexCount);
    job->mesh.physics.indices = (s32*)AllocateTLSFGlobal(sizeof(s32) * indexCount);
    if (job->mesh.physics.vertices && job->mesh.physics.indices)
    {
        for (u32 v = 0; v < vertexCount; v++) {
            v128f p = job->buildVertices[v].position;
            job->mesh.physics.vertices[v] = (b3Vec3){ VecGetX(p), VecGetY(p), VecGetZ(p) };
        }
        for (u32 k = 0; k < indexCount; k++)
            job->mesh.physics.indices[k] = (s32)job->buildIndices[k];
        return true;
    }

    AX_WARN("transvoxel terrain physics mesh allocation failed");
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
		SDL_SetAtomicInt(&tTransvoxel.heapPressure, 1);
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

// worker-side chunk build: density -> transvoxel mesh -> indexed terrain geometry ->
// parked heap ranges. touches only the job slot; shared systems synchronize internally.
static void RunBuildJob(void* userData)
{
    tBuildJob* job = (tBuildJob*)userData;
    f32* density = NULL;
    BeginBuildJob(job);
    bool failed = !PrepareBuildScratch(job, &density) ||
			      !GenerateChunkMesh(job, density) ||
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
        tBuildJob* job = &tTransvoxel.buildJobs[i];
        if (job->busy && job->chunkIndex == chunkIndex)
            return true;
    }
    return false;
}

static bool ScheduleChunkBuild(u32 chunkIndex, tChunk* chunk)
{
    if (tTransvoxel.builtThisFrame >= T_MAX_BUILDS_PER_FRAME) {
        if (chunk->buildState == CHUNK_UNBUILT || chunk->buildState == CHUNK_FAILED)
            tSetChunkState(chunk, CHUNK_QUEUED);
        return false;
    }
    if (tTransvoxel.cacheVertices > T_VERTEX_CACHE_BUDGET ||
        tTransvoxel.cacheIndices > T_INDEX_CACHE_BUDGET)
    {
        tPruneChunkCache(T_VERTEX_CACHE_TARGET, T_INDEX_CACHE_TARGET, true);
    }

    tBuildJob* job = NULL;
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++) {
        if (!tTransvoxel.buildJobs[i].busy) {
            job = &tTransvoxel.buildJobs[i];
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
    job->lod = chunk->lod;
    job->neighboursMask = chunk->neighboursMask;
    job->busy = true;
    // dirty clears at schedule time: a sculpt landing while the job flies re-sets it
    // and the chunk reschedules after integration, so stale results self-heal
    chunk->dirty = false;
    if (!chunk->mesh.vertices.heapPtr && chunk->buildState != CHUNK_READY)
        tSetChunkState(chunk, CHUNK_BUILDING);
    job->handle = JobSystem_Execute(tTransvoxel.jobSystem, RunBuildJob, job);
    if (job->handle == 0)
    {
        job->busy = false;
        chunk->dirty = true;
        if (chunk->buildState == CHUNK_BUILDING)
            tSetChunkState(chunk, CHUNK_QUEUED);
        return false;
    }
    tTransvoxel.builtThisFrame++;
    return true;
}

// main thread, once per frame before the pending promote: moves finished job results
// into the chunks' pending slots so they run through the usual 2-frame promote
static void IntegrateFinishedBuilds(void)
{
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++)
    {
        tBuildJob* job = &tTransvoxel.buildJobs[i];
        if (!job->busy || !JobSystem_IsJobDone(tTransvoxel.jobSystem, job->handle))
            continue;

        tChunk* chunk = &tTransvoxel.chunks[job->chunkIndex];
        job->busy = false;

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
            tTransvoxel.emptyChunks++;
            continue;
        }

        chunk->pendingMesh = job->mesh;
        tSetChunkPending(chunk, PENDING_MESH);
        tTransvoxel.cacheVertices += job->mesh.vertices.count;
        tTransvoxel.cacheIndices += job->mesh.indices.count;
        job->mesh = (tMeshHandle){0};
    }
}

// waits for every in-flight build and discards results that never got integrated;
// must run before anything invalidates chunk indices or tears the system down
static void tDrainBuildJobs(void)
{
    if (!tTransvoxel.jobSystem) return;
    JobSystem_Wait(tTransvoxel.jobSystem);
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++)
    {
        tBuildJob* job = &tTransvoxel.buildJobs[i];
        if (!job->busy) continue;
        tFreeMeshHandle(&job->mesh);
        job->busy = false;
        tChunk* chunk = &tTransvoxel.chunks[job->chunkIndex];
        if (chunk->buildState == CHUNK_BUILDING)
            tSetChunkState(chunk, CHUNK_QUEUED);
    }
}

// tChunkPositionKey packs the position into bits 3..63, so the 2-bit lod fits in
// the free low bits and one map covers all LOD levels
static u64 tChunkKey(int3 position, s32 lod)
{
	return ((u64)((u32)(position.x + 0x100000) & 0x1FFFFFu) << 40)
		| ((u64)((u32)(position.y + 0x8000)   & 0xFFFFu)   << 24)
		| ((u64)((u32)(position.z + 0x100000) & 0x1FFFFFu) << 3) | (u64)(u32)lod;
}

static tChunk* FindChunk(int3 min, s32 lod)
{
    u32* found = (u32*)HMFind(&tTransvoxel.chunkLookup, tChunkKey(min, lod));
    return found ? &tTransvoxel.chunks[*found] : NULL;
}

static void tRemapBuildJobChunkIndex(u32 oldIndex, u32 newIndex)
{
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++)
    {
        tBuildJob* job = &tTransvoxel.buildJobs[i];
        if (job->busy && job->chunkIndex == oldIndex)
            job->chunkIndex = newIndex;
    }
}

static void tFreeChunkSlot(u32 index)
{
    if (index >= tTransvoxel.chunkCount) return;
    tChunk* chunk = &tTransvoxel.chunks[index];
    HMErase(&tTransvoxel.chunkLookup, tChunkKey(chunk->min, chunk->lod));
    tDestroyChunkPhysics(chunk);
    tFreeMeshHandle(&chunk->mesh);
    tFreePendingMesh(chunk);

    u32 lastIndex = tTransvoxel.chunkCount - 1u;
    if (index != lastIndex)
    {
        tTransvoxel.chunks[index] = tTransvoxel.chunks[lastIndex];
        u64 movedKey = tChunkKey(tTransvoxel.chunks[index].min, tTransvoxel.chunks[index].lod);
        HMInsertOrAssign(&tTransvoxel.chunkLookup, movedKey, &index);
        tRemapBuildJobChunkIndex(lastIndex, index);
    }

    MemsetZero(&tTransvoxel.chunks[lastIndex], sizeof(tTransvoxel.chunks[lastIndex]));
    BitsetReset(tTransvoxel.occupiedChunksBitset, (s32)lastIndex);
    tTransvoxel.chunkCount--;
    if (tTransvoxel.physicsSyncCursor >= tTransvoxel.chunkCount)
        tTransvoxel.physicsSyncCursor = 0u;
}

static bool tFreeOldestChunkSlot(bool requireMesh, bool respectKeepFrames)
{
    u32 evictIndex  = UINT32_MAX;
    u32 oldestFrame = UINT32_MAX;
    for (u32 i = 0; i < tTransvoxel.chunkCount; i++)
    {
        tChunk* chunk = &tTransvoxel.chunks[i];
        if (ChunkBuildInFlight(i)) continue;
        if (requireMesh && !chunk->mesh.vertices.heapPtr && !chunk->pendingMesh.vertices.heapPtr)
            continue;
        if (respectKeepFrames && chunk->lastTouchedFrame + T_CACHE_KEEP_FRAMES >= tTransvoxel.frameIndex)
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

static u32 tAllocChunkSlot(void)
{
    if (tTransvoxel.chunkCount >= T_MAX_CHUNKS) 
        if (!tFreeOldestChunkSlot(false, false)) 
            tClearChunkCache(); 

    s32 index = BitsetFindFirstEmpty(tTransvoxel.occupiedChunksBitset, (s32)T_MAX_CHUNKS);
    if (index < 0) return UINT32_MAX;

    BitsetSet(tTransvoxel.occupiedChunksBitset, index);
    tTransvoxel.chunkCount++;
    return (u32)index;
}

static void tClearChunkCache(void)
{
	AX_LOG("transvoxel chunk cache reset");
    tDrainBuildJobs();
    RendererSetTerrainChunkDraws(NULL, 0);
    for (u32 i = 0; i < tTransvoxel.chunkCount; i++)
    {
        tDestroyChunkPhysics(&tTransvoxel.chunks[i]);
        tFreeMeshHandle(&tTransvoxel.chunks[i].mesh);
        tFreePendingMesh(&tTransvoxel.chunks[i]);
    }
    tTransvoxel.chunkCount = 0;
    tTransvoxel.cacheVertices = 0u;
    tTransvoxel.cacheIndices = 0u;
    tTransvoxel.physicsSyncCursor = 0;
    if (tTransvoxel.occupiedChunksBitset)
        MemsetZero(tTransvoxel.occupiedChunksBitset, T_CHUNK_BITSET_WORDS * sizeof(u64));
    HMClear(&tTransvoxel.chunkLookup);
}

static void tPruneChunkCache(u32 targetVertices, u32 targetIndices, bool respectKeepFrames)
{
    while (tTransvoxel.cacheVertices > targetVertices || tTransvoxel.cacheIndices > targetIndices)
        if (!tFreeOldestChunkSlot(true, respectKeepFrames))
            break;
}

static void tResolveHeapPressure(void)
{
    if (SDL_GetAtomicInt(&tTransvoxel.heapPressure) == 0)
        return;
    SDL_SetAtomicInt(&tTransvoxel.heapPressure, 0);
    AX_WARN("transvoxel terrain heap pressure; pruning chunk cache");
    tPruneChunkCache(T_VERTEX_CACHE_TARGET / 2u, T_INDEX_CACHE_TARGET / 2u, false);
}

static void tPromotePendingMeshes(void)
{
    for (u32 i = 0; i < tTransvoxel.chunkCount; i++)
    {
        tChunk* chunk = &tTransvoxel.chunks[i];
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
    if (SDL_GetAtomicInt(&tTransvoxel.physicsInvalidated) != 0)
    {
        SDL_SetAtomicInt(&tTransvoxel.physicsInvalidated, 0);
        for (u32 i = 0; i < tTransvoxel.chunkCount; i++)
        {
            tChunk* chunk = &tTransvoxel.chunks[i];
            if (chunk->physicsSlot >= 0 || (chunk->lod <= 1 && chunk->mesh.vertices.heapPtr && chunk->mesh.vertices.count >= 3u))
                chunk->physicsDirty = true;
        }
    }

    if (tTransvoxel.chunkCount == 0u) return;

    u32 synced = 0u;
    u32 visited = 0u;
    while (visited < tTransvoxel.chunkCount && synced < T_MAX_PHYSICS_SYNCS_PER_FRAME)
    {
        u32 i = tTransvoxel.physicsSyncCursor++;
        if (tTransvoxel.physicsSyncCursor >= tTransvoxel.chunkCount)
            tTransvoxel.physicsSyncCursor = 0u;
        visited++;

        tChunk* chunk = &tTransvoxel.chunks[i];
        if (!chunk->physicsDirty) continue;
        
		tSyncChunkPhysics(chunk);
        chunk->physicsDirty = false;
        synced++;
    }
}

static tChunk* GetOrCreateChunk(int3 min, s32 lod)
{
    s32 neighboursMask = tColumnMask(min, lod);
    u32* found = (u32*)HMFind(&tTransvoxel.chunkLookup, tChunkKey(min, lod));
    tChunk* chunk = found ? &tTransvoxel.chunks[*found] : NULL;
    if (chunk)
    {
        chunk->lastTouchedFrame = tTransvoxel.frameIndex;
        // a neighbour crossed a lod ring: this chunk's boundary shrink + transition
        // strips no longer match, remesh with the new mask (old mesh keeps drawing
        // until the pending rebuild promotes, so the seam heals without a flash).
        // while a build flies the stored mask must stay the job's snapshot, otherwise
        // the stale result would look up to date; the diff re-checks after integration
        if (!ChunkBuildInFlight(*found) && chunk->neighboursMask != neighboursMask)
        {
            chunk->neighboursMask = neighboursMask;
            chunk->dirty = true;
        }
        if (chunk->dirty && !ChunkBuildInFlight(*found) && chunk->pendingState == PENDING_NONE)
            ScheduleChunkBuild(*found, chunk);
        return chunk;
    }

    u32 index = tAllocChunkSlot();
    if (index == UINT32_MAX) {
        AX_WARN("transvoxel chunk allocation failed");
        return NULL;
    }
    chunk = &tTransvoxel.chunks[index];
    *chunk = (tChunk){
        .min = min,
        .lod = lod,
        .neighboursMask = neighboursMask,
        .physicsSlot = -1,
        .buildState = CHUNK_UNBUILT,
        .pendingState = PENDING_NONE,
    };
    chunk->lastTouchedFrame = tTransvoxel.frameIndex;
    s32 worldSize = T_CHUNK_CELLS << lod;
    chunk->aabbMin = ToFloat3(min);
    chunk->aabbMax = F3AddF(chunk->aabbMin, (f32)worldSize);
    HMInsert(&tTransvoxel.chunkLookup, tChunkKey(min, lod), &index);
    chunk->dirty = true;
    ScheduleChunkBuild(index, chunk);
    return chunk;
}

static bool tAABBVisible(float3 aabbMin, float3 aabbMax, const FrustumPlanes* frustum)
{
    v128f min = Vec3Load(&aabbMin.x);
    v128f max = Vec3Load(&aabbMax.x);
    return CheckAABBCulled(min, max, frustum->planes);
}

// returns false only when the draw list is full; empty chunks are a successful no-op so
// one air/solid chunk does not abort the rest of the LOD ring. every chunk submits a heap
// draw range (no vertex copy); the brush highlight is a terrain shader uniform now
static bool tAppendChunkTriangles(const tChunk* chunk)
{
    if (!chunk || chunk->mesh.indices.count == 0 || !chunk->mesh.vertices.heapPtr || !chunk->mesh.indices.heapPtr)
        return true;

    if (tTransvoxel.numChunkDraws >= MAX_TERRAIN_CHUNK_DRAWS)
        return false;
    // indexed draw: first_index into the terrain index heap, base_vertex = vertex heap offset
    TerrainChunkDraw draw = { chunk->mesh.indices.first, chunk->mesh.indices.count, (s32)chunk->mesh.vertices.first };
	tTransvoxel.chunkDraws[tTransvoxel.numChunkDraws++] = draw;
    return true;
}

// a chunk counts as presentable when it either has a live mesh
// or is genuinely empty. a fresh build whose mesh still sits in the pending slot is NOT
// presentable: it has no drawable geometry until tPromotePendingMeshes swaps it
// in, and treating it as ready made the LOD descend draw nothing for the promote frames.
static bool tChunkPresentable(const tChunk* chunk)
{
    if (!chunk) return false;
    if (chunk->mesh.vertices.heapPtr) return true;
    return chunk->buildState == CHUNK_READY && chunk->pendingState == PENDING_NONE;
}

// vertical band of chunks that can hold surface geometry for the engine density field,
// on this lod's grid (step = chunk world size). one place so every traversal query
// floors the range identically
static void tChunkYRange(s32 step, s32* chunkYMin, s32* chunkYMax)
{
    f32 yMin, yMax;
    TerrainDensity_GetYRange(&yMin, &yMax);
    *chunkYMin = (s32)Floorf32(yMin / (f32)step);
    *chunkYMax = (s32)Floorf32(yMax / (f32)step);
}

// child column coords on the next-finer grid: i in [0,4), bit0 = x, bit1 = z
static void tChildColumn(s32 chunkX, s32 chunkZ, u32 i, s32* outX, s32* outZ)
{
    *outX = chunkX * 2 + (s32)(i & 1u);
    *outZ = chunkZ * 2 + (s32)(i >> 1u);
}

// true when every chunk of the column at this lod is presentable. requestBuilds=true
// asks GetOrCreateChunk to schedule missing chunks (within the per-frame budget) and always
// walks the whole column; requestBuilds=false is find-only for the coarser-lod
// fallback checks and bails on the first absent chunk
static bool tColumnReady(s32 chunkX, s32 chunkZ, s32 lod, bool requestBuilds)
{
    s32 step = T_CHUNK_CELLS << lod;
    s32 chunkYMin, chunkYMax;
    tChunkYRange(step, &chunkYMin, &chunkYMax);

    bool ready = true;
    for (s32 y = chunkYMin; y <= chunkYMax; y++)
    {
        int3 min = { chunkX * step, y * step, chunkZ * step };
        const tChunk* chunk;
        if (requestBuilds)  chunk = GetOrCreateChunk(min, lod);
        else                chunk = FindChunk(min, lod);

        if (!tChunkPresentable(chunk))
        {
            ready = false;
            if (!requestBuilds) return false;
        }
    }
    return ready;
}

static bool AreChildColumnsReady(s32 chunkX, s32 chunkZ, s32 lod, bool requestBuilds)
{
    for (u32 i = 0; i < 4u; i++)
    {
        s32 cx, cz;
        tChildColumn(chunkX, chunkZ, i, &cx, &cz);
        if (!tColumnReady(cx, cz, lod - 1, requestBuilds))
            return false;
    }
    return true;
}

static bool SubmitChildNodes(s32 chunkX, s32 chunkZ, s32 lod, f32 lodFactor, const FrustumPlanes* frustum, bool useFrustum)
{
    for (u32 i = 0; i < 4u; i++)
    {
        s32 cx, cz;
        tChildColumn(chunkX, chunkZ, i, &cx, &cz);
        if (!tSubmitNode(cx, cz, lod - 1, lodFactor, frustum, useFrustum))
            return false;
    }
    return true;
}

// chunked-LOD quadtree: split a node when its closest point lies inside the finer
// LOD's range, otherwise emit it at this lod. every split child is always handled,
// so the selection is gap-free — the old per-ring center-distance test dropped
// chunks whose center fell between two rings. returns false when the submit
// buffer is full and traversal must stop.
static bool tSubmitNode(s32 chunkX, s32 chunkZ, s32 lod, f32 lodFactor, const FrustumPlanes* frustum, bool useFrustum)
{
    s32 step = T_CHUNK_CELLS << lod;
    f32 distanceSq = ChunkColumnDistanceSq(chunkX, chunkZ, lod);

    // global draw distance, applied only at the tree root: inner nodes always emit
    // or split so a split parent can never leave uncovered children
    if (lod == (s32)T_LOD_COUNT - 1)
    {
        f32 drawDistance = tLODDistance[lod] * lodFactor;
        if (distanceSq >= drawDistance * drawDistance)
            return true;
    }

    if (lod > 0)
    {
        f32 splitDistance = tLODDistance[lod - 1] * lodFactor;
        if (distanceSq < splitDistance * splitDistance)
        {
            // descend only when every child column is presentable; otherwise keep
            // drawing this coarser node while the children build over the next frames
            // (tColumnBuilt requests the missing builds within the frame budget),
            // so camera motion refines the LOD instead of leaving holes
            if (AreChildColumnsReady(chunkX, chunkZ, lod, true))
            {
                return SubmitChildNodes(chunkX, chunkZ, lod, lodFactor, frustum, useFrustum);
            }
        }
    }

    // own column: request builds within the budget. when it is still building (e.g. the
    // camera moved away and this coarser lod never existed here) but the finer children
    // from previous frames are still cached, draw those instead — the mirror image of
    // the descend fallback above, so lod transitions never flash invisible chunks
    if (!tColumnReady(chunkX, chunkZ, lod, true) && lod > 0)
    {
        if (AreChildColumnsReady(chunkX, chunkZ, lod, false))
        {
            return SubmitChildNodes(chunkX, chunkZ, lod, lodFactor, frustum, useFrustum);
        }
    }

    s32 chunkYMin, chunkYMax;
    tChunkYRange(step, &chunkYMin, &chunkYMax);

    for (s32 y = chunkYMin; y <= chunkYMax; y++)
    {
        int3 min = { chunkX * step, y * step, chunkZ * step };
        float3 aabbMin = ToFloat3(min);
        float3 aabbMax = F3AddF(aabbMin, (f32)step);
        if (useFrustum && !tAABBVisible(aabbMin, aabbMax, frustum))
        {
            tTransvoxel.culledChunks++;
            continue;
        }

        tChunk* chunk = GetOrCreateChunk(min, lod);
        if (!tChunkPresentable(chunk))
            continue;
        if (!tAppendChunkTriangles(chunk))
            return false;
    }

    return true;
}

static void tSubmitTerrain(f32 lodFactor, const FrustumPlanes* frustum, bool useFrustum)
{
    const TerrainGenParams* params = Terrain_GetGenParams();
    const s32 rootLOD = (s32)T_LOD_COUNT - 1;
    s32 rootStep = T_CHUNK_CELLS << rootLOD;
    f32 drawDistance = tLODDistance[rootLOD] * lodFactor;
    s32 centerX = (s32)Floorf32(g_Camera.position.x / (f32)rootStep);
    s32 centerZ = (s32)Floorf32(g_Camera.position.z / (f32)rootStep);
    s32 radius = (s32)(drawDistance / (f32)rootStep) + 1;

    // near shells first so the per-frame build budget fills terrain around the camera
    for (s32 shell = 0; shell <= radius; shell++)
    for (s32 z = -shell; z <= shell; z++)
    for (s32 x = -shell; x <= shell; x++)
	{
		if (Maxs32(Abss32(x), Abss32(z)) != shell)
			continue;
		if (!tSubmitNode(centerX + x, centerZ + z, rootLOD, lodFactor, frustum, useFrustum))
			return;
	}
}

static void tLogStats(void)
{
#if T_ENABLE_STATS
    u64 now = SDL_GetTicks();
    if (now - tTransvoxel.lastStatsTicks < 1000u)
        return;
    tTransvoxel.lastStatsTicks = now;
    AX_LOG("transvoxel : draws=%u built=%u cached=%u culled=%u empty=%u",
           tTransvoxel.numChunkDraws, tTransvoxel.builtThisFrame,
           tTransvoxel.chunkCount, tTransvoxel.culledChunks, tTransvoxel.emptyChunks);
#endif
}

static void BeginTerrainFrame(void)
{
    tTransvoxel.numChunkDraws = 0;
    tTransvoxel.builtThisFrame = 0;
    tTransvoxel.culledChunks = 0;
    tTransvoxel.emptyChunks = 0;
}

static bool tInit(void)
{
    if (tTransvoxel.initialized)
        return true;
    MemSet(&tTransvoxel, 0, sizeof(tTransvoxel));
	
    tTransvoxel.jobSystem = JobSystem_Create(0, 0);
    if (!tTransvoxel.jobSystem)
    {
        AX_WARN("terrain job system create failed!");
        return false;
    }

    tTransvoxel.generator.noise3D = tDensityNoise;
    tTransvoxel.generator.noise3DStrength = 1.0f;

    for (u32 i = 0; i < MAX_TERRAIN_PHYSICS_CHUNKS; i++)
        tTransvoxel.physicsSlotPool[i] = (u16)i;
    tTransvoxel.physicsSlotCount = MAX_TERRAIN_PHYSICS_CHUNKS;

    tTransvoxel.chunkDraws = (TerrainChunkDraw*)AllocateTLSFGlobal(sizeof(TerrainChunkDraw) * MAX_TERRAIN_CHUNK_DRAWS);
    tTransvoxel.occupiedChunksBitset = (u64*)AllocateTLSFGlobal(T_CHUNK_BITSET_WORDS * sizeof(u64));
    tTransvoxel.chunkLookup = HMCreate(T_MAX_CHUNKS, sizeof(u32));
    if (!tTransvoxel.chunkDraws || !tTransvoxel.occupiedChunksBitset)
    {
        AX_WARN("transvoxel allocation failed");
        tDestroy();
        return false;
    }
    MemsetZero(tTransvoxel.occupiedChunksBitset, T_CHUNK_BITSET_WORDS * sizeof(u64));

    // one scratch mesh container and bump arena per job slot so workers never share output
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++)
    {
        if (!tMeshDataContainerInit(&tTransvoxel.buildJobs[i].scratchMesh)) { tDestroy(); return false; }
		if (!ArenaScratchCreate(&tTransvoxel.buildJobs[i].scratchArena, T_BUILD_SCRATCH_SIZE, "terrainChunkBuild")) { tDestroy(); return false; }
    }

    tTransvoxel.initialized = true;
    return true;
}

void tUpdate(void)
{
    if (!tInit()) {
        RendererSetTerrainChunkDraws(NULL, 0);
        return;
    }

    IntegrateFinishedBuilds();
    tResolveHeapPressure();
    tPromotePendingMeshes();
    tTransvoxel.frameIndex++;
    tPruneChunkCache(T_VERTEX_CACHE_TARGET, T_INDEX_CACHE_TARGET, true);
    BeginTerrainFrame();
    mat4x4 viewProj = M44Multiply(g_Camera.view, g_Camera.projection);
    FrustumPlanes frustum = CreateFrustumPlanesRevZ(viewProj);

    f32 lodFactor = Maxf32(g_RenderSettings.terrainLodFactor, 0.25f);
    const TerrainGenParams* genParams = Terrain_GetGenParams();
    tTransvoxel.lodFactor = lodFactor;
    tSubmitTerrain(lodFactor, &frustum, true);
    if (tTransvoxel.numChunkDraws == 0) {
        tSubmitTerrain(lodFactor, &frustum, false);
    }

    tSyncDirtyPhysics();
    tLogStats();
    RendererSetTerrainChunkDraws(tTransvoxel.chunkDraws, tTransvoxel.numChunkDraws);
    RendererSetTerrainBrush(tTransvoxel.brushPos, tTransvoxel.brushActive ? tTransvoxel.brushRadius : 0.0f);
}

void tInvalidateAll(void)
{
    if (tTransvoxel.initialized)
        tClearChunkCache();
}

void tInvalidateRegion(float3 mn, float3 mx)
{
    if (!tTransvoxel.initialized)
        return;

    for (u32 i = 0; i < tTransvoxel.chunkCount; i++)
    {
        tChunk* chunk = &tTransvoxel.chunks[i];
        // the chunk's 19^3 sample grid reaches one voxel below aabbMin and two above
        // aabbMax, and gradients read one more; pad like TerrainRemeshRegion does
        f32 pad = 2.0f * (f32)(1 << chunk->lod);
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
    tTransvoxel.brushPos = position;
    tTransvoxel.brushRadius = radius;
    tTransvoxel.brushActive = active && radius > 0.0f;
}

void tDestroy(void)
{
    if (!tTransvoxel.initialized && !tTransvoxel.jobSystem && !tTransvoxel.chunkDraws &&
        !tTransvoxel.occupiedChunksBitset && !tTransvoxel.chunkLookup.keys) {
        return;
    }

    RendererSetTerrainChunkDraws(NULL, 0);
    tClearChunkCache(); // drains in-flight builds first
    HMDestroy(&tTransvoxel.chunkLookup);
    DeAllocateTLSFGlobal(tTransvoxel.chunkDraws);
    DeAllocateTLSFGlobal(tTransvoxel.occupiedChunksBitset);
    for (u32 i = 0; i < T_MAX_BUILD_JOBS; i++) {
        tMeshDataContainerDestroy(&tTransvoxel.buildJobs[i].scratchMesh);
        ArenaScratchDestroy(&tTransvoxel.buildJobs[i].scratchArena);
    }
    if (tTransvoxel.jobSystem)
        JobSystem_Destroy(tTransvoxel.jobSystem);
    MemSet(&tTransvoxel, 0, sizeof(tTransvoxel));
}
