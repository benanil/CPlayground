#include "Source/Terrain/TransvoxelUnity.h"
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

#define TRANSVOXEL_CHUNK_SIZE           16
#define TRANSVOXEL_LOD_COUNT            4u
#define TRANSVOXEL_MAX_CHUNKS          (16384u)
#define TRANSVOXEL_CHUNK_BITSET_WORDS ((TRANSVOXEL_MAX_CHUNKS + 63u) / 64u)
#define TRANSVOXEL_CHUNK_TRIANGLE_CAP  (8192u * 4u)
#define TRANSVOXEL_MAX_BUILDS_PER_FRAME 16u
#define TRANSVOXEL_BUILD_SCRATCH_SIZE  (3ull * 1024ull * 1024ull)
#define TRANSVOXEL_MAX_PHYSICS_SYNCS_PER_FRAME 2u
#define TRANSVOXEL_MAX_BUILD_JOBS 8u
#define TRANSVOXEL_VERTEX_CACHE_BUDGET (TERRAIN_MAX_VERTICES / 2u)
#define TRANSVOXEL_VERTEX_CACHE_TARGET (TERRAIN_MAX_VERTICES / 3u)
#define TRANSVOXEL_INDEX_CACHE_BUDGET  (TERRAIN_MAX_INDICES / 2u)
#define TRANSVOXEL_INDEX_CACHE_TARGET  (TERRAIN_MAX_INDICES / 3u)
#define TRANSVOXEL_HEAP_PRESSURE_EVICT_CHUNKS 32u
#define TRANSVOXEL_CACHE_KEEP_FRAMES 2u

#define TRANSVOXEL_GRASS_LAYER 0u
#define TRANSVOXEL_DIRT_LAYER  1u
#define TRANSVOXEL_ROCK_LAYER  2u
#define TRANSVOXEL_SAND_LAYER  3u
#define TRANSVOXEL_SAND_TOP    2.0f
#define TRANSVOXEL_SAND_FADE   2.0f
#define TRANSVOXEL_GRASS_START 3.0f
#define TRANSVOXEL_GRASS_FADE  8.0f

typedef struct tMeshHandle_
{
    void*   heapPtr;
    u32     heapFirst;
    u32     vertexCount;
    void*   idxHeapPtr; // index range in the TerrainIndex2 geometry heap (chunk-local values)
    u32     idxFirst;
    u32     indexCount;
    b3Vec3* physicsVertices;
    s32*    physicsIndices;
    u32     physicsVertexCount;
    u32     physicsIndexCount;
} tMeshHandle;

typedef struct tRuntimeChunk_
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
    bool  built;
    bool  building;       // a worker job is meshing this chunk right now
    bool  dirty;
    bool  physicsDirty;
    bool  pendingEmpty;
} tChunk;

// one in-flight chunk build on a JobSystem worker. the main thread fills the inputs,
// launches the job and reads the outputs after JobSystem_IsJobDone; exactly one job
// ever touches a chunk (chunk->building), so no locks on chunk state are needed
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
    // output, worker; heapPtr is the parked mesh (NULL when empty or failed)
    tMeshHandle  mesh;
    JobHandle    handle;
    bool         failed;
    bool         busy;
} tBuildJob;

typedef struct tTransvoxelState_
{
    tDensityGenerator generator;
	JobSystem* jobSystem;
    tBuildJob  buildJobs[TRANSVOXEL_MAX_BUILD_JOBS];
    TerrainChunkDraw* chunkDraws; // per-frame heap ranges, one indirect multidraw
    tChunk  chunks[TRANSVOXEL_MAX_CHUNKS];
    HashMap chunkLookup; // key: tChunkKey, value: u32 index into chunks
    u64*    unusedChunksBitset; // bit set means slot is occupied; name kept for old code/searches
    u32     numChunkDraws;
	u32     chunkCount;
    u32     builtThisFrame; // jobs scheduled this frame, capped by TRANSVOXEL_MAX_BUILDS_PER_FRAME
    u32     cacheVertices;  // live + pending chunk vertices resident in GeometryBuffer_TerrainVertNew
    u32     cacheIndices;   // live + pending chunk indices resident in GeometryBuffer_TerrainIndex2
    u32     frameIndex;
    u32     culledChunks;
    u32     emptyChunks;
    u32     physicsSyncCursor;
    SDL_AtomicInt physicsInvalidated;
    SDL_AtomicInt heapPressure;
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

// the engine density field (TerrainDensity.c noise) plugged into the port's generator
// contract: tDensityGeneratorGetValue returns -y + noise3D, and the port treats
// positive values as solid, so returning y - At makes the total exactly -At.
// TerrainDensity_At = SDF + sculpt edits, so editor manipulation shows up in the mesh.
static f32 tDensityNoise(f32 x, f32 y, f32 z, void* userData)
{
    (void)userData;
    return y - TerrainDensity_At(x, y, z);
}

static void tTerrainMaterial(float3 worldPos, float3 normal, u32* materials, u32* blend)
{
    const TerrainGenParams* params = Terrain_GetGenParams();
    f32 seaLevel = params ? params->seaLevel : 0.0f;

    f32 slope = normal.y;
    f32 steepRock = 1.0f - Saturatef32((slope - 0.55f) * 4.0f);
    f32 highBlend = Saturatef32((worldPos.y - 34.0f) * 0.08f);
    f32 rockBlend = Maxf32(steepRock, highBlend);
    f32 grassHeight = Saturatef32((worldPos.y - seaLevel - TRANSVOXEL_GRASS_START) * (1.0f / TRANSVOXEL_GRASS_FADE));
    f32 grassAmount = (1.0f - rockBlend) * grassHeight;
    float3 layerWeight = {
        grassAmount * (1.0f - rockBlend),
        (1.0f - grassAmount) * (1.0f - rockBlend),
        rockBlend
    };

    f32 procWeights[3] = { layerWeight.x, layerWeight.y, layerWeight.z };
    u32 procA = TRANSVOXEL_GRASS_LAYER;
    if (layerWeight.y > layerWeight.x)
        procA = TRANSVOXEL_DIRT_LAYER;
    u32 procB = TRANSVOXEL_DIRT_LAYER;
    if (procA == TRANSVOXEL_DIRT_LAYER)
    {
        procB = TRANSVOXEL_GRASS_LAYER;
        if (layerWeight.z > layerWeight.x)
            procB = TRANSVOXEL_ROCK_LAYER;
    }
    else if (layerWeight.z > layerWeight.y)
    {
        procB = TRANSVOXEL_ROCK_LAYER;
    }

    f32 procWeightAF = procWeights[procA];
    f32 procWeightBF = procWeights[procB];
    f32 procTotal = procWeightAF + procWeightBF;
    u32 procWeightA = (u32)Clampf32(procWeightAF / Maxf32(procTotal, 1.0e-4f) * 255.0f + 0.5f, 0.0f, 255.0f);
    f32 sandBlend = Saturatef32((seaLevel + TRANSVOXEL_SAND_TOP - worldPos.y) * (1.0f / TRANSVOXEL_SAND_FADE));

    u8 matIndex[2];
    u8 matWeight[2];
    TerrainEdit_MaterialWeights(worldPos, matIndex, matWeight);

    u32 procDominant = procWeightA >= 128u ? procA : procB;
    u32 idxA;
    u32 idxB;
    u32 wA;
    if (matIndex[0] == 0u && matIndex[1] == 0u)
    {
        if (sandBlend > 0.002f)
        {
            idxA = TRANSVOXEL_SAND_LAYER;
            idxB = TRANSVOXEL_DIRT_LAYER;
            wA = (u32)(sandBlend * 255.0f + 0.5f);
        }
        else
        {
            idxA = procA;
            idxB = procB;
            wA = procWeightA;
        }
    }
    else
    {
        idxA = procDominant;
        if (matIndex[0] != 0u)
            idxA = Minu32((u32)matIndex[0] - 1u, TRANSVOXEL_SAND_LAYER);
        idxB = procDominant;
        if (matIndex[1] != 0u)
            idxB = Minu32((u32)matIndex[1] - 1u, TRANSVOXEL_SAND_LAYER);
        wA = matWeight[0];
    }

    *materials = idxA | (idxB << 8);
    *blend = wA & 0xFFu;
}

// thread safe: TerrainDensity_SampleChunk is pure + edit overlay is mutex guarded
static void tBuildDensity(int3 chunkMin, s32 lod, f32* out)
{
    s32 densitySize = TRANSVOXEL_CHUNK_SIZE + 3;
    s32 step = TRANSVOXEL_CHUNK_SIZE << lod;
    s32 cx = chunkMin.x / step;
    s32 cy = chunkMin.y / step;
    s32 cz = chunkMin.z / step;
    s8 samples[TERRAIN_SAMPLES_TOTAL];
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

static const f32 tLODDistance[TRANSVOXEL_LOD_COUNT] = { 48.0f, 112.0f, 240.0f, 448.0f };

// true when the quadtree node at (nodeX, nodeZ, lod) splits into finer children:
// the closest point of its XZ box lies inside the next-finer lod's distance ring.
// same test tSubmitNode uses, so the mask stays consistent with traversal
static bool tNodeSplits(s32 nodeX, s32 nodeZ, s32 lod, f32 lodFactor)
{
    if (lod <= 0)
        return false;
    s32 step = TRANSVOXEL_CHUNK_SIZE << lod;
    f32 minX = (f32)(nodeX * step);
    f32 minZ = (f32)(nodeZ * step);
    f32 dx = Clampf32(g_Camera.position.x, minX, minX + (f32)step) - g_Camera.position.x;
    f32 dz = Clampf32(g_Camera.position.z, minZ, minZ + (f32)step) - g_Camera.position.z;
    f32 splitDistance = tLODDistance[lod - 1] * lodFactor;
    return dx * dx + dz * dz < splitDistance * splitDistance;
}

// does the column at (chunkX, chunkZ) on this lod's grid render at a finer lod?
// walks the quadtree top-down: every ancestor must split to reach this lod, and the
// node at this lod itself must split for its children to render instead
static bool tNeighbourFiner(s32 chunkX, s32 chunkZ, s32 lod, f32 lodFactor)
{
    if (lod == 0)
        return false;
    for (s32 l = (s32)TRANSVOXEL_LOD_COUNT - 1; l >= lod; l--)
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
    if (tTransvoxel.fixedArea || lod == 0)
        return 0;
    s32 step = TRANSVOXEL_CHUNK_SIZE << lod;
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

static void tPushTriangleVertex(tBuildJob* job, v128f p, u32 normal, u32 materials, u32 blend)
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
    const tMeshData* mesh = tMeshDataContainerGetConst(&job->scratchMesh, slot);
    if (!mesh || !mesh->vertices || !mesh->indices || !job->buildVertices || !job->buildIndices)
        return;

    size_t vertexCount = (size_t)mesh->numVertices;
    size_t indexCount = (size_t)mesh->numIndices;
    if (vertexCount == 0 || indexCount < 3)
        return;
    // this slot's vertices land contiguously after everything emitted so far; the source
    // indices are local to the slot, so shift them by that base to stay chunk-local
    if (job->buildVertexCount + vertexCount > TRANSVOXEL_CHUNK_TRIANGLE_CAP)
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
        tPushTriangleVertex(job, p, mesh->vertices[v].normal, m, b);
    }

    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
        if (job->buildIndexCount + 3u > TRANSVOXEL_CHUNK_TRIANGLE_CAP)
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

static void tFreeMeshHandle(tMeshHandle* mesh)
{
    if (mesh->heapPtr)
    {
        GeometryHeapFree(GeometryBuffer_TerrainVertNew, mesh->heapPtr);
        if (tTransvoxel.cacheVertices >= mesh->vertexCount)
            tTransvoxel.cacheVertices -= mesh->vertexCount;
        else
            tTransvoxel.cacheVertices = 0u;
    }
    if (mesh->idxHeapPtr)
    {
        GeometryHeapFree(GeometryBuffer_TerrainIndex2, mesh->idxHeapPtr);
        if (tTransvoxel.cacheIndices >= mesh->indexCount)
            tTransvoxel.cacheIndices -= mesh->indexCount;
        else
            tTransvoxel.cacheIndices = 0u;
    }
    DeAllocateTLSFGlobal(mesh->physicsVertices);
    DeAllocateTLSFGlobal(mesh->physicsIndices);
    *mesh = (tMeshHandle){0};
}

static void tFreeChunkMesh(tChunk* chunk)
{
    tFreeMeshHandle(&chunk->mesh);
}

static void tFreePendingMesh(tChunk* chunk)
{
    tFreeMeshHandle(&chunk->pendingMesh);
    chunk->pendingFrames = 0;
    chunk->pendingEmpty = false;
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

static void tSyncChunkPhysics(tChunk* chunk)
{
    Scene* scene = Scene_GetActive();
    if (!scene)
        return;
    if (!chunk || chunk->lod > 1 || !chunk->mesh.heapPtr || chunk->mesh.vertexCount < 3u ||
        !chunk->mesh.physicsVertices || !chunk->mesh.physicsIndices || chunk->mesh.physicsIndexCount < 3u)
    {
        tDestroyChunkPhysics(chunk);
        return;
    }

    if (chunk->physicsSlot < 0)
    {
        if (tTransvoxel.physicsSlotCount == 0u)
        {
            AX_WARN("transvoxel terrain physics slots exhausted");
            return;
        }
        chunk->physicsSlot = (s32)tTransvoxel.physicsSlotPool[--tTransvoxel.physicsSlotCount];
    }

    if (!Scene_PhysicsSyncTerrainChunkMesh(scene, (u32)chunk->physicsSlot,
                                           chunk->mesh.physicsVertices, chunk->mesh.physicsVertexCount,
                                           chunk->mesh.physicsIndices, chunk->mesh.physicsIndexCount))
        tDestroyChunkPhysics(chunk);
}

void tInvalidatePhysics(void)
{
    SDL_SetAtomicInt(&tTransvoxel.physicsInvalidated, 1);
}
// worker-side chunk build: density -> transvoxel mesh -> colored soup -> parked in the
// tVertexData heap. touches only the job slot; the heap, upload queue, density field
// and edit storage are all internally synchronized. CPU scratch (density grid, vertex
// buffer, mesher caches via ArenaPushGlobal) comes from a reusable per-build-slot scratch arena
static void tBuildJobRun(void* userData)
{
    tBuildJob* job = (tBuildJob*)userData;
    job->mesh = (tMeshHandle){0};
    job->failed = false;
    job->buildVertices = NULL;
    job->buildVertexCount = 0;
    job->buildIndices = NULL;
    job->buildIndexCount = 0;

    ArenaScratchBegin(&job->scratchArena);

    s32 densitySize = TRANSVOXEL_CHUNK_SIZE + 3;
    size_t densityCount = (size_t)densitySize * (size_t)densitySize * (size_t)densitySize;
    f32* density = (f32*)ArenaPushGlobal(sizeof(f32) * densityCount);
    job->buildVertices = (tVertexData*)ArenaPushGlobal(sizeof(tVertexData) * TRANSVOXEL_CHUNK_TRIANGLE_CAP);
    job->buildIndices = (u32*)ArenaPushGlobal(sizeof(u32) * TRANSVOXEL_CHUNK_TRIANGLE_CAP);
    if (!density || !job->buildVertices || !job->buildIndices)
    {
        AX_WARN("transvoxel build scratch allocation failed");
        job->failed = true;
        job->buildVertices = NULL;
        job->buildIndices = NULL;
        ArenaScratchEnd(&job->scratchArena);
        return;
    }

    tBuildDensity(job->min, job->lod, density);
    if (!tTransvoxelMesherMesh(&tTransvoxel.generator, job->min, TRANSVOXEL_CHUNK_SIZE, density,
                               job->lod, job->neighboursMask, &job->scratchMesh, NULL))
    {
        AX_WARN("transvoxel chunk mesh build failed");
        job->failed = true;
        job->buildVertices = NULL;
        job->buildIndices = NULL;
        ArenaScratchEnd(&job->scratchArena);
        return;
    }

    // lod stitching: snap boundary vertices to their secondary positions (shrinks the
    // regular mesh half a cell inward on faces with a finer neighbour) and fill the gap
    // with that face's transition cell strip. faces without a finer neighbour keep
    // primary positions and skip their transition slot
    tMeshDataContainerApplySecondaryVertices(&job->scratchMesh, job->neighboursMask);
    tAppendMeshSlotTriangles(job, tMeshDataSlot_Main);
    for (u32 bit = 0; bit < 6u; bit++)
    {
        if (job->neighboursMask & (1 << bit))
            tAppendMeshSlotTriangles(job, (tMeshDataSlot)(tMeshDataSlot_LeftTransition + bit));
    }

    // park the mesh in the tVertexData geometry heap and queue the range for the
    // shared GPU-mirror flush; empty chunks leave
    // heapPtr NULL and the main thread integrates them as pendingEmpty
    u32 vertexCount = job->buildVertexCount;
    u32 indexCount = job->buildIndexCount;
    if (vertexCount > 0 && indexCount > 0)
    {
        void* raw = NULL;
        u32 first = GeometryHeapAlloc(GeometryBuffer_TerrainVertNew, vertexCount, &raw);
        void* idxRaw = NULL;
        u32 idxFirst = first == GEOMETRY_ALLOC_FAIL
            ? GEOMETRY_ALLOC_FAIL
            : GeometryHeapAlloc(GeometryBuffer_TerrainIndex2, indexCount, &idxRaw);
        if (first == GEOMETRY_ALLOC_FAIL || idxFirst == GEOMETRY_ALLOC_FAIL)
        {
            // partial success leaks nothing: free whichever heap succeeded, drop the build
            if (first != GEOMETRY_ALLOC_FAIL)
                GeometryHeapFree(GeometryBuffer_TerrainVertNew, raw);
            SDL_SetAtomicInt(&tTransvoxel.heapPressure, 1);
            job->failed = true;
        }
        else
        {
            MemCopy((tVertexData*)gGFX.TerrainVertNewBuffer + first, job->buildVertices, vertexCount * sizeof(tVertexData));
            Rendering_QueueGeometryUpload(GeometryBuffer_TerrainVertNew, first, first + vertexCount);
            MemCopy((u32*)gGFX.TerrainIndexBuffer2 + idxFirst, job->buildIndices, indexCount * sizeof(u32));
            Rendering_QueueGeometryUpload(GeometryBuffer_TerrainIndex2, idxFirst, idxFirst + indexCount);
            job->mesh.heapPtr = raw;
            job->mesh.heapFirst = first;
            job->mesh.vertexCount = vertexCount;
            job->mesh.idxHeapPtr = idxRaw;
            job->mesh.idxFirst = idxFirst;
            job->mesh.indexCount = indexCount;
            // near chunks (lod<=1) get a collider: the render verts + indices ARE the
            // collision mesh, just narrowed to positions (b3Vec3) and s32 indices
			// box3d wants world-space positions, which buildVertices hold
            if (job->lod <= 1 && indexCount >= 3u)
            {
                job->mesh.physicsVertexCount = vertexCount;
                job->mesh.physicsIndexCount = indexCount;
                job->mesh.physicsVertices = (b3Vec3*)AllocateTLSFGlobal(sizeof(b3Vec3) * vertexCount);
                job->mesh.physicsIndices = (s32*)AllocateTLSFGlobal(sizeof(s32) * indexCount);
                if (job->mesh.physicsVertices && job->mesh.physicsIndices)
                {
                    for (u32 v = 0; v < vertexCount; v++)
                    {
                        v128f p = job->buildVertices[v].position;
                        job->mesh.physicsVertices[v] = (b3Vec3){ VecGetX(p), VecGetY(p), VecGetZ(p) };
                    }
                    for (u32 k = 0; k < indexCount; k++)
                        job->mesh.physicsIndices[k] = (s32)job->buildIndices[k];
                }
                else
                {
                    AX_WARN("transvoxel terrain physics mesh allocation failed");
                    DeAllocateTLSFGlobal(job->mesh.physicsVertices);
                    DeAllocateTLSFGlobal(job->mesh.physicsIndices);
                    job->mesh.physicsVertices = NULL;
                    job->mesh.physicsIndices = NULL;
                    job->mesh.physicsVertexCount = 0u;
                    job->mesh.physicsIndexCount = 0u;
                }
            }
        }
    }

    job->buildVertices = NULL;
    job->buildIndices = NULL;
    ArenaScratchEnd(&job->scratchArena);
}

// picks a free job slot and launches the chunk build on a worker. false when the
// per-frame cap is hit, every slot is busy, or the job queue is full; the chunk keeps
// its dirty flag in those cases and retries next frame
static bool tScheduleBuild(u32 chunkIndex, tChunk* chunk)
{
    if (tTransvoxel.builtThisFrame >= TRANSVOXEL_MAX_BUILDS_PER_FRAME)
        return false;
    if (tTransvoxel.cacheVertices > TRANSVOXEL_VERTEX_CACHE_BUDGET ||
        tTransvoxel.cacheIndices > TRANSVOXEL_INDEX_CACHE_BUDGET)
    {
        tPruneChunkCache(TRANSVOXEL_VERTEX_CACHE_TARGET, TRANSVOXEL_INDEX_CACHE_TARGET, true);
    }

    tBuildJob* job = NULL;
    for (u32 i = 0; i < TRANSVOXEL_MAX_BUILD_JOBS; i++)
    {
        if (!tTransvoxel.buildJobs[i].busy)
        {
            job = &tTransvoxel.buildJobs[i];
            break;
        }
    }
    if (!job)
        return false;

    job->chunkIndex = chunkIndex;
    job->min = chunk->min;
    job->lod = chunk->lod;
    job->neighboursMask = chunk->neighboursMask;
    job->busy = true;
    // dirty clears at schedule time: a sculpt landing while the job flies re-sets it
    // and the chunk reschedules after integration, so stale results self-heal
    chunk->dirty = false;
    chunk->building = true;
    job->handle = JobSystem_Execute(tTransvoxel.jobSystem, tBuildJobRun, job);
    if (job->handle == 0)
    {
        job->busy = false;
        chunk->dirty = true;
        chunk->building = false;
        return false;
    }
    tTransvoxel.builtThisFrame++;
    return true;
}

// main thread, once per frame before the pending promote: moves finished job results
// into the chunks' pending slots so they run through the usual 2-frame promote
static void tIntegrateFinishedBuilds(void)
{
    for (u32 i = 0; i < TRANSVOXEL_MAX_BUILD_JOBS; i++)
    {
        tBuildJob* job = &tTransvoxel.buildJobs[i];
        if (!job->busy || !JobSystem_IsJobDone(tTransvoxel.jobSystem, job->handle))
            continue;

        tChunk* chunk = &tTransvoxel.chunks[job->chunkIndex];
        chunk->building = false;
        chunk->built = true;
        job->busy = false;

        if (job->failed)
        {
            // dropped (mesher failure / heap full); a later invalidation re-dirties it
            tFreeMeshHandle(&job->mesh);
            continue;
        }

        tFreePendingMesh(chunk);
        if (job->mesh.vertexCount == 0)
        {
            if (chunk->mesh.heapPtr)
            {
                // genuinely empty now: retire the live mesh through the promote delay
                chunk->pendingEmpty = true;
                chunk->pendingFrames = 2u;
            }
            else
            {
                tDestroyChunkPhysics(chunk);
            }
            tTransvoxel.emptyChunks++;
            continue;
        }

        chunk->pendingMesh = job->mesh;
        chunk->pendingFrames = 2u;
        tTransvoxel.cacheVertices += job->mesh.vertexCount;
        tTransvoxel.cacheIndices += job->mesh.indexCount;
        job->mesh = (tMeshHandle){0};
    }
}

// waits for every in-flight build and discards results that never got integrated;
// must run before anything invalidates chunk indices or tears the system down
static void tDrainBuildJobs(void)
{
    if (!tTransvoxel.jobSystem)
        return;
    JobSystem_Wait(tTransvoxel.jobSystem);
    for (u32 i = 0; i < TRANSVOXEL_MAX_BUILD_JOBS; i++)
    {
        tBuildJob* job = &tTransvoxel.buildJobs[i];
        if (!job->busy)
            continue;
        tFreeMeshHandle(&job->mesh);
        job->busy = false;
        tTransvoxel.chunks[job->chunkIndex].building = false;
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

static void tRemapBuildJobChunkIndex(u32 oldIndex, u32 newIndex)
{
    for (u32 i = 0; i < TRANSVOXEL_MAX_BUILD_JOBS; i++)
    {
        tBuildJob* job = &tTransvoxel.buildJobs[i];
        if (job->busy && job->chunkIndex == oldIndex)
            job->chunkIndex = newIndex;
    }
}

static void tFreeChunkSlot(u32 index)
{
    if (index >= tTransvoxel.chunkCount)
        return;

    tChunk* chunk = &tTransvoxel.chunks[index];
    HMErase(&tTransvoxel.chunkLookup, tChunkKey(chunk->min, chunk->lod));
    tDestroyChunkPhysics(chunk);
    tFreeChunkMesh(chunk);
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
    BitsetReset(tTransvoxel.unusedChunksBitset, (s32)lastIndex);
    tTransvoxel.chunkCount--;
    if (tTransvoxel.physicsSyncCursor >= tTransvoxel.chunkCount)
        tTransvoxel.physicsSyncCursor = 0u;
}

static bool tFreeOldestChunkSlot(bool requireMesh, bool respectKeepFrames)
{
    u32 evictIndex = UINT32_MAX;
    u32 oldestFrame = UINT32_MAX;
    for (u32 i = 0; i < tTransvoxel.chunkCount; i++)
    {
        tChunk* chunk = &tTransvoxel.chunks[i];
        if (chunk->building)
            continue;
        if (requireMesh && !chunk->mesh.heapPtr && !chunk->pendingMesh.heapPtr)
            continue;
        if (respectKeepFrames && chunk->lastTouchedFrame + TRANSVOXEL_CACHE_KEEP_FRAMES >= tTransvoxel.frameIndex)
            continue;
        if (chunk->lastTouchedFrame < oldestFrame)
        {
            oldestFrame = chunk->lastTouchedFrame;
            evictIndex = i;
        }
    }

    if (evictIndex == UINT32_MAX)
        return false;
    tFreeChunkSlot(evictIndex);
    return true;
}

static u32 tAllocChunkSlot(void)
{
    if (tTransvoxel.chunkCount >= TRANSVOXEL_MAX_CHUNKS)
    {
        if (!tFreeOldestChunkSlot(false, false))
        {
            tClearChunkCache();
            AX_LOG("transvoxel chunk cache reset");
        }
    }

    s32 index = BitsetFindFirstEmpty(tTransvoxel.unusedChunksBitset, (s32)TRANSVOXEL_MAX_CHUNKS);
    if (index < 0)
        return UINT32_MAX;
    if ((u32)index != tTransvoxel.chunkCount)
        AX_WARN("transvoxel chunk slot allocator repaired non-dense slot state");

    index = (s32)tTransvoxel.chunkCount;
    if ((u32)index >= TRANSVOXEL_MAX_CHUNKS)
        return UINT32_MAX;

    BitsetSet(tTransvoxel.unusedChunksBitset, index);
    tTransvoxel.chunkCount++;
    return (u32)index;
}

static void tClearChunkCache(void)
{
    tDrainBuildJobs();
    RendererSetTerrainChunkDraws(NULL, 0);
    for (u32 i = 0; i < tTransvoxel.chunkCount; i++)
    {
        tDestroyChunkPhysics(&tTransvoxel.chunks[i]);
        tFreeChunkMesh(&tTransvoxel.chunks[i]);
        tFreePendingMesh(&tTransvoxel.chunks[i]);
    }
    tTransvoxel.chunkCount = 0;
    tTransvoxel.cacheVertices = 0u;
    tTransvoxel.cacheIndices = 0u;
    tTransvoxel.physicsSyncCursor = 0;
    if (tTransvoxel.unusedChunksBitset)
        MemsetZero(tTransvoxel.unusedChunksBitset, TRANSVOXEL_CHUNK_BITSET_WORDS * sizeof(u64));
    HMClear(&tTransvoxel.chunkLookup);
}

static void tPruneChunkCache(u32 targetVertices, u32 targetIndices, bool respectKeepFrames)
{
    while (tTransvoxel.cacheVertices > targetVertices || tTransvoxel.cacheIndices > targetIndices)
    {
        if (!tFreeOldestChunkSlot(true, respectKeepFrames))
            break;
    }
}

static void tEvictOldestChunkSlots(u32 count, bool respectKeepFrames)
{
    for (u32 i = 0; i < count; i++)
    {
        if (!tFreeOldestChunkSlot(true, respectKeepFrames))
            break;
    }
}

static void tResolveHeapPressure(void)
{
    if (SDL_GetAtomicInt(&tTransvoxel.heapPressure) == 0)
        return;
    SDL_SetAtomicInt(&tTransvoxel.heapPressure, 0);
    AX_WARN("transvoxel terrain heap pressure; pruning chunk cache");
    tPruneChunkCache(TRANSVOXEL_VERTEX_CACHE_TARGET / 2u, TRANSVOXEL_INDEX_CACHE_TARGET / 2u, false);
    tEvictOldestChunkSlots(TRANSVOXEL_HEAP_PRESSURE_EVICT_CHUNKS, false);
}

static void tPromotePendingMeshes(void)
{
    for (u32 i = 0; i < tTransvoxel.chunkCount; i++)
    {
        tChunk* chunk = &tTransvoxel.chunks[i];
        if (!chunk->pendingMesh.heapPtr && !chunk->pendingEmpty)
            continue;
        if (chunk->pendingFrames > 0u)
        {
            chunk->pendingFrames--;
            if (chunk->pendingFrames > 0u)
                continue;
        }

        tFreeChunkMesh(chunk);
        if (chunk->pendingEmpty)
        {
            tDestroyChunkPhysics(chunk);
            chunk->pendingEmpty = false;
            continue;
        }

        chunk->mesh = chunk->pendingMesh;
        chunk->pendingMesh = (tMeshHandle){0};
        chunk->physicsDirty = true;
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
            if (chunk->physicsSlot >= 0 || (chunk->lod <= 1 && chunk->mesh.heapPtr && chunk->mesh.vertexCount >= 3u))
                chunk->physicsDirty = true;
        }
    }

    if (tTransvoxel.chunkCount == 0u)
        return;

    u32 synced = 0u;
    u32 visited = 0u;
    while (visited < tTransvoxel.chunkCount && synced < TRANSVOXEL_MAX_PHYSICS_SYNCS_PER_FRAME)
    {
        u32 i = tTransvoxel.physicsSyncCursor++;
        if (tTransvoxel.physicsSyncCursor >= tTransvoxel.chunkCount)
            tTransvoxel.physicsSyncCursor = 0u;
        visited++;

        tChunk* chunk = &tTransvoxel.chunks[i];
        if (!chunk->physicsDirty)
            continue;

        tSyncChunkPhysics(chunk);
        chunk->physicsDirty = false;
        synced++;
    }
}

static tChunk* tGetChunk(int3 min, s32 lod)
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
        if (!chunk->building && chunk->neighboursMask != neighboursMask)
        {
            chunk->neighboursMask = neighboursMask;
            chunk->dirty = true;
        }
        if (chunk->dirty && !chunk->building && !chunk->pendingMesh.heapPtr && !chunk->pendingEmpty)
            tScheduleBuild(*found, chunk);
        return chunk;
    }

    u32 index = tAllocChunkSlot();
    if (index == UINT32_MAX)
    {
        AX_WARN("transvoxel chunk allocation failed");
        return NULL;
    }
    chunk = &tTransvoxel.chunks[index];
    *chunk = (tChunk){
        .min = min,
        .lod = lod,
        .neighboursMask = neighboursMask,
        .physicsSlot = -1,
    };
    chunk->lastTouchedFrame = tTransvoxel.frameIndex;
    s32 worldSize = TRANSVOXEL_CHUNK_SIZE << lod;
    chunk->aabbMin = ToFloat3(min);
    chunk->aabbMax = F3AddF(chunk->aabbMin, (f32)worldSize);
    HMInsert(&tTransvoxel.chunkLookup, tChunkKey(min, lod), &index);
    // built stays false until the worker result integrates; scheduling is capped per
    // frame and by free job slots, missed chunks retry on later frames
    chunk->dirty = true;
    tScheduleBuild(index, chunk);
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
    // must short-circuit: bitwise | evaluates chunk->mesh.indexCount on a NULL chunk
    if (!chunk || chunk->mesh.indexCount == 0 || !chunk->mesh.heapPtr || !chunk->mesh.idxHeapPtr)
        return true;

    if (tTransvoxel.numChunkDraws >= MAX_TERRAIN_CHUNK_DRAWS)
        return false;
    // indexed draw: first_index into the terrain index heap, base_vertex = vertex heap offset
    TerrainChunkDraw draw = { chunk->mesh.idxFirst, chunk->mesh.indexCount, (s32)chunk->mesh.heapFirst };
	tTransvoxel.chunkDraws[tTransvoxel.numChunkDraws++] = draw;
    return true;
}

// a chunk counts as presentable when it is built and either has a live (drawable) mesh
// or is genuinely empty. a fresh build whose mesh still sits in the pending slot is NOT
// presentable: it has no drawable geometry until tPromotePendingMeshes swaps it
// in, and treating it as ready made the LOD descend draw nothing for the promote frames.
static bool tChunkPresentable(const tChunk* chunk)
{
    if (!chunk || !chunk->built)
        return false;
    if (chunk->mesh.heapPtr)
        return true;
    return !chunk->pendingMesh.heapPtr && !chunk->pendingEmpty;
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
// asks tGetChunk to schedule missing chunks (within the per-frame budget) and always
// walks the whole column; requestBuilds=false is find-only for the coarser-lod
// fallback checks and bails on the first absent chunk
static bool tColumnReady(s32 chunkX, s32 chunkZ, s32 lod, bool requestBuilds)
{
    s32 step = TRANSVOXEL_CHUNK_SIZE << lod;
    s32 chunkYMin, chunkYMax;
    tChunkYRange(step, &chunkYMin, &chunkYMax);

    bool ready = true;
    for (s32 y = chunkYMin; y <= chunkYMax; y++)
    {
        int3 min = { chunkX * step, y * step, chunkZ * step };
        const tChunk* chunk;
        if (requestBuilds)
        {
            chunk = tGetChunk(min, lod);
        }
        else
        {
            u32* found = (u32*)HMFind(&tTransvoxel.chunkLookup, tChunkKey(min, lod));
            chunk = found ? &tTransvoxel.chunks[*found] : NULL;
        }
        if (!tChunkPresentable(chunk))
        {
            ready = false;
            if (!requestBuilds)
                return false;
        }
    }
    return ready;
}

// chunked-LOD quadtree: split a node when its closest point lies inside the finer
// LOD's range, otherwise emit it at this lod. every split child is always handled,
// so the selection is gap-free — the old per-ring center-distance test dropped
// chunks whose center fell between two rings. returns false when the submit
// buffer is full and traversal must stop.
static bool tSubmitNode(s32 chunkX, s32 chunkZ, s32 lod, f32 lodFactor, const FrustumPlanes* frustum, bool useFrustum)
{
    s32 step = TRANSVOXEL_CHUNK_SIZE << lod;
    f32 minX = (f32)(chunkX * step);
    f32 minZ = (f32)(chunkZ * step);
    f32 closestX = Clampf32(g_Camera.position.x, minX, minX + (f32)step);
    f32 closestZ = Clampf32(g_Camera.position.z, minZ, minZ + (f32)step);
    f32 dx = closestX - g_Camera.position.x;
    f32 dz = closestZ - g_Camera.position.z;
    f32 distanceSq = dx * dx + dz * dz;

    // global draw distance, applied only at the tree root: inner nodes always emit
    // or split so a split parent can never leave uncovered children
    if (lod == (s32)TRANSVOXEL_LOD_COUNT - 1)
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
            // descend only when every child column is already built; otherwise keep
            // drawing this coarser node while the children build over the next frames
            // (tColumnBuilt requests the missing builds within the frame budget),
            // so camera motion refines the LOD instead of leaving holes
            bool childrenReady = true;
            for (u32 i = 0; i < 4u; i++)
            {
                s32 cx, cz;
                tChildColumn(chunkX, chunkZ, i, &cx, &cz);
                childrenReady &= tColumnReady(cx, cz, lod - 1, true);
            }

            if (childrenReady)
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
        }
    }

    // own column: request builds within the budget. when it is still building (e.g. the
    // camera moved away and this coarser lod never existed here) but the finer children
    // from previous frames are still cached, draw those instead — the mirror image of
    // the descend fallback above, so lod transitions never flash invisible chunks
    if (!tColumnReady(chunkX, chunkZ, lod, true) && lod > 0)
    {
        bool childrenPresent = true;
        for (u32 i = 0; i < 4u; i++)
        {
            s32 cx, cz;
            tChildColumn(chunkX, chunkZ, i, &cx, &cz);
            childrenPresent &= tColumnReady(cx, cz, lod - 1, false);
        }

        if (childrenPresent)
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

        tChunk* chunk = tGetChunk(min, lod);
        if (!chunk || !chunk->built)
            continue;
        if (!tAppendChunkTriangles(chunk))
            return false;
    }

    return true;
}

static void tSubmitTerrain(f32 lodFactor, const FrustumPlanes* frustum, bool useFrustum)
{
    const TerrainGenParams* params = Terrain_GetGenParams();
    if (params && params->fixedArea)
    {
        s32 fixedSize = Clamps32((s32)params->fixedWorldSize, TERRAIN_FIXED_WORLD_MIN_SIZE, TERRAIN_FIXED_WORLD_MAX_SIZE);
        s32 halfSize = fixedSize / 2;
        s32 minChunkX = FloorDiv(-halfSize, TRANSVOXEL_CHUNK_SIZE);
        s32 maxChunkX = FloorDiv(fixedSize - halfSize - 1, TRANSVOXEL_CHUNK_SIZE);
        s32 minChunkZ = minChunkX;
        s32 maxChunkZ = maxChunkX;

        s32 chunkYMin, chunkYMax;
        tChunkYRange(TRANSVOXEL_CHUNK_SIZE, &chunkYMin, &chunkYMax);

        for (s32 z = minChunkZ; z <= maxChunkZ; z++)
        for (s32 x = minChunkX; x <= maxChunkX; x++)
        for (s32 y = chunkYMin; y <= chunkYMax; y++)
        {
            int3 min = { x * TRANSVOXEL_CHUNK_SIZE, y * TRANSVOXEL_CHUNK_SIZE, z * TRANSVOXEL_CHUNK_SIZE };
            tChunk* chunk = tGetChunk(min, 0);
            if (!chunk || !chunk->built)
                continue;
            if (!tAppendChunkTriangles(chunk))
                return;
        }
        return;
    }

    const s32 rootLOD = (s32)TRANSVOXEL_LOD_COUNT - 1;
    s32 rootStep = TRANSVOXEL_CHUNK_SIZE << rootLOD;
    f32 drawDistance = tLODDistance[rootLOD] * lodFactor;
    s32 centerX = (s32)Floorf32(g_Camera.position.x / (f32)rootStep);
    s32 centerZ = (s32)Floorf32(g_Camera.position.z / (f32)rootStep);
    s32 radius = (s32)(drawDistance / (f32)rootStep) + 1;

    // near shells first so the per-frame build budget fills terrain around the camera
    for (s32 shell = 0; shell <= radius; shell++)
    {
        for (s32 z = -shell; z <= shell; z++)
        {
            for (s32 x = -shell; x <= shell; x++)
            {
                if (Maxs32(Abss32(x), Abss32(z)) != shell)
                    continue;
                if (!tSubmitNode(centerX + x, centerZ + z, rootLOD, lodFactor, frustum, useFrustum))
                    return;
            }
        }
    }
}

static void tLogStats(void)
{
	return;
    u64 now = SDL_GetTicks();
    if (now - tTransvoxel.lastStatsTicks < 1000u)
        return;
    tTransvoxel.lastStatsTicks = now;
    AX_LOG("transvoxel : draws=%u built=%u cached=%u culled=%u empty=%u",
           tTransvoxel.numChunkDraws, tTransvoxel.builtThisFrame,
           tTransvoxel.chunkCount, tTransvoxel.culledChunks, tTransvoxel.emptyChunks);
}

static bool tInit(void)
{
    if (tTransvoxel.initialized)
        return true;
    MemSet(&tTransvoxel, 0, sizeof(tTransvoxel));
	
	tTransvoxel.jobSystem = JobSystem_Create(0, 0);
	if (!tTransvoxel.jobSystem) { AX_WARN("terrain job system create failed!"); return false; } // should never happen though

    tTransvoxel.generator.noise3D = tDensityNoise;
    tTransvoxel.generator.noise3DStrength = 1.0f;

    for (u32 i = 0; i < MAX_TERRAIN_PHYSICS_CHUNKS; i++)
        tTransvoxel.physicsSlotPool[i] = (u16)i;
    tTransvoxel.physicsSlotCount = MAX_TERRAIN_PHYSICS_CHUNKS;

    tTransvoxel.chunkDraws = (TerrainChunkDraw*)AllocateTLSFGlobal(sizeof(TerrainChunkDraw) * MAX_TERRAIN_CHUNK_DRAWS);
    tTransvoxel.unusedChunksBitset = (u64*)AllocateTLSFGlobal(TRANSVOXEL_CHUNK_BITSET_WORDS * sizeof(u64));
    tTransvoxel.chunkLookup = HMCreate(TRANSVOXEL_MAX_CHUNKS, sizeof(u32));
    if (!tTransvoxel.chunkDraws || !tTransvoxel.unusedChunksBitset)
    {
        AX_WARN("transvoxel allocation failed");
        tDestroy();
        return false;
    }
    MemsetZero(tTransvoxel.unusedChunksBitset, TRANSVOXEL_CHUNK_BITSET_WORDS * sizeof(u64));

    // one scratch mesh container and bump arena per job slot so workers never share output
    for (u32 i = 0; i < TRANSVOXEL_MAX_BUILD_JOBS; i++)
    {
        if (!tMeshDataContainerInit(&tTransvoxel.buildJobs[i].scratchMesh, 8192, 24576, 2048))
        {
            tDestroy();
            return false;
        }
        if (!ArenaScratchCreate(&tTransvoxel.buildJobs[i].scratchArena, TRANSVOXEL_BUILD_SCRATCH_SIZE, "terrainChunkBuild"))
        {
            tDestroy();
            return false;
        }
    }

    tTransvoxel.initialized = true;
    return true;
}

void tUpdate(void)
{
    if (!tInit())
    {
        RendererSetTerrainChunkDraws(NULL, 0);
        return;
    }

    tIntegrateFinishedBuilds();
    tResolveHeapPressure();
    tPromotePendingMeshes();
    tTransvoxel.frameIndex++;
    tPruneChunkCache(TRANSVOXEL_VERTEX_CACHE_TARGET, TRANSVOXEL_INDEX_CACHE_TARGET, true);
    tTransvoxel.numChunkDraws = 0;
    tTransvoxel.builtThisFrame = 0;
    tTransvoxel.culledChunks = 0;
    tTransvoxel.emptyChunks = 0;
    mat4x4 viewProj = M44Multiply(g_Camera.view, g_Camera.projection);
    FrustumPlanes frustum = CreateFrustumPlanesRevZ(viewProj);

    f32 lodFactor = Maxf32(g_RenderSettings.terrainLodFactor, 0.25f);
    const TerrainGenParams* genParams = Terrain_GetGenParams();
    tTransvoxel.lodFactor = lodFactor;
    tTransvoxel.fixedArea = genParams && genParams->fixedArea;
    tSubmitTerrain(lodFactor, &frustum, true);
    if (tTransvoxel.numChunkDraws == 0)
    {
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
    RendererSetTerrainChunkDraws(NULL, 0);
    tClearChunkCache(); // drains in-flight builds first
    HMDestroy(&tTransvoxel.chunkLookup);
    DeAllocateTLSFGlobal(tTransvoxel.chunkDraws);
    DeAllocateTLSFGlobal(tTransvoxel.unusedChunksBitset);
    for (u32 i = 0; i < TRANSVOXEL_MAX_BUILD_JOBS; i++)
    {
        tMeshDataContainerDestroy(&tTransvoxel.buildJobs[i].scratchMesh);
        ArenaScratchDestroy(&tTransvoxel.buildJobs[i].scratchArena);
    }
    if (tTransvoxel.jobSystem)
        JobSystem_Destroy(tTransvoxel.jobSystem);
    MemSet(&tTransvoxel, 0, sizeof(tTransvoxel));
}
