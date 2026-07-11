#include "Source/Terrain/TransvoxelUnity.h"
#include "Source/Terrain/TerrainInternal.h"
#include "Include/Camera.h"
#include "Include/Terrain.h"
#include "Include/Rendering.h"
#include "Include/Platform.h"
#include "Include/Scene.h"
#include "Include/Memory.h"
#include "Include/JobSystem.h"

#define T_EXAMPLE_CHUNK_SIZE 16
#define T_EXAMPLE_LOD_COUNT 4u
#define T_EXAMPLE_MAX_CHUNKS (16384u)
#define T_EXAMPLE_CHUNK_TRIANGLE_CAP (8192u * 4u)
#define T_EXAMPLE_MAX_BUILDS_PER_FRAME 16u
#define T_EXAMPLE_MAX_PHYSICS_SYNCS_PER_FRAME 2u
#define T_EXAMPLE_MAX_BUILD_JOBS 8u

typedef struct tExampleChunk_
{
    int3   min;
    s32    lod;
    float3 aabbMin;
    float3 aabbMax;
    // chunk mesh lives in the TerrainVertex geometry heap (ALineVertex payload, same
    // 16 byte stride); the GPU mirror draws it without any per-frame copy
    void* heapPtr;
    u32   heapFirst;
    u32   vertexCount;
    void* pendingHeapPtr;
    u32   pendingHeapFirst;
    u32   pendingVertexCount;
    u8    pendingFrames;
    s32   neighboursMask; // bit per face (-x,-y,-z,+x,+y,+z): neighbor renders at a finer lod
    s32   physicsSlot;    // scene terrain collider slot, -1 = none. chunk indices can
                          // exceed MAX_TERRAIN_PHYSICS_CHUNKS, so slots are pooled
    bool  built;
    bool  building;       // a worker job is meshing this chunk right now
    bool  dirty;
    bool  physicsDirty;
    bool  pendingEmpty;
} tExampleChunk;

// one in-flight chunk build on a JobSystem worker. the main thread fills the inputs,
// launches the job and reads the outputs after JobSystem_IsJobDone; exactly one job
// ever touches a chunk (chunk->building), so no locks on chunk state are needed
typedef struct tExampleBuildJob_
{
    // inputs, main thread
    u32  chunkIndex;
    int3 min;
    s32  lod;
    s32  neighboursMask;
    // per-slot scratch, initialized once (ranges in the TerrainVertNew/Second/Index2 heaps)
    tMeshDataContainer scratchMesh;
    // worker-local append state, valid only while the job runs (thread scratch arena)
    ALineVertex* buildVertices;
    u32          buildVertexCount;
    // outputs, worker; heapPtr is the parked mesh (NULL when empty or failed)
    void* heapPtr;
    u32   heapFirst;
    u32   vertexCount;
    bool  failed;
    JobHandle handle;
    bool  busy;
} tExampleBuildJob;

typedef struct tTransvoxelExample_
{
    tDensityGenerator generator;
	JobSystem* jobSystem;
    tExampleBuildJob buildJobs[T_EXAMPLE_MAX_BUILD_JOBS];
    TerrainChunkDraw* chunkDraws; // per-frame heap ranges, one indirect multidraw
    tExampleChunk chunks[T_EXAMPLE_MAX_CHUNKS];
    HashMap chunkLookup; // key: tChunkKey, value: u32 index into chunks
	u32 numChunkDraws;
	u32 chunkCount;
    u32 builtThisFrame; // jobs scheduled this frame, capped by T_EXAMPLE_MAX_BUILDS_PER_FRAME
    u32 culledChunks;
    u32 emptyChunks;
    u32 physicsSyncCursor;
    // free-list of scene terrain collider slots; only near lod0/1 chunks hold one
    u16 physicsSlotPool[MAX_TERRAIN_PHYSICS_CHUNKS];
    u32 physicsSlotCount;
    f32 lodFactor;   // this frame's lod distance scale, read by the mask computation
    bool fixedArea;  // fixed-area worlds are lod0-only: no lod seams, masks stay 0
    u64 lastStatsTicks;
    float3 brushPos;
    f32 brushRadius;
    bool brushActive;
    bool initialized;
} tTransvoxelExample;

extern Camera g_Camera;
extern Graphics gGFX; // geometry heap CPU mirrors (defined in Graphics.c)

static tTransvoxelExample tExample;

// the engine density field (TerrainDensity.c noise) plugged into the port's generator
// contract: tDensityGeneratorGetValue returns -y + noise3D, and the port treats
// positive values as solid, so returning y - At makes the total exactly -At.
// TerrainDensity_At = SDF + sculpt edits, so editor manipulation shows up in the mesh.
static f32 tDensityNoise(f32 x, f32 y, f32 z, void* userData)
{
    (void)userData;
    return y - TerrainDensity_At(x, y, z);
}

static u32 tPackColor(float3 color)
{
    u32 r = (u32)(Saturatef32(color.x) * 255.0f + 0.5f);
    u32 g = (u32)(Saturatef32(color.y) * 255.0f + 0.5f);
    u32 b = (u32)(Saturatef32(color.z) * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16);
}

// flat colors for painted layers; index is the packed TerrainEdit layer id (stored
// layer + 1, 0 = unpainted/procedural). first four follow the builtin texture layer
// order: grass, dirt, rock, sand; the rest are distinct spare hues
static const float3 tPaintPalette[16] = {
    { 0.00f, 0.00f, 0.00f }, // 0: unpainted, never sampled
    { 0.16f, 0.42f, 0.14f }, // 1: grass
    { 0.32f, 0.22f, 0.12f }, // 2: dirt
    { 0.42f, 0.42f, 0.40f }, // 3: rock
    { 0.72f, 0.62f, 0.40f }, // 4: sand
    { 0.60f, 0.16f, 0.12f }, // 5
    { 0.16f, 0.36f, 0.58f }, // 6
    { 0.58f, 0.52f, 0.16f }, // 7
    { 0.40f, 0.20f, 0.50f }, // 8
    { 0.16f, 0.52f, 0.48f }, // 9
    { 0.66f, 0.40f, 0.16f }, // 10
    { 0.30f, 0.30f, 0.60f }, // 11
    { 0.55f, 0.30f, 0.35f }, // 12
    { 0.25f, 0.45f, 0.25f }, // 13
    { 0.50f, 0.50f, 0.20f }, // 14
    { 0.70f, 0.70f, 0.70f }  // 15
};

static u32 tTerrainColor(float3 worldPos, float3 normal)
{
    const TerrainGenParams* params = Terrain_GetGenParams();
    f32 seaLevel = params ? params->seaLevel : 0.0f;

    float3 sand  = { 0.62f, 0.53f, 0.34f };
    float3 dirt  = { 0.27f, 0.21f, 0.13f };
    float3 grass = { 0.14f, 0.34f, 0.12f };
    float3 rock  = { 0.34f, 0.34f, 0.32f };

    f32 slope = Saturatef32(normal.y);
    f32 grassBlend = Saturatef32((slope - 0.45f) * 2.5f) * Saturatef32((worldPos.y - seaLevel - 2.0f) * 0.25f);
    f32 sandBlend = Saturatef32((seaLevel + 2.0f - worldPos.y) * 0.35f);
    f32 rockBlend = Maxf32(Saturatef32((0.62f - slope) * 2.4f), Saturatef32((worldPos.y - 34.0f) * 0.08f));

    float3 color = F3Lerp(&dirt, &grass, grassBlend);
    color = F3Lerp(&color, &sand, sandBlend);
    color = F3Lerp(&color, &rock, rockBlend);

    // painted layers override the procedural blend; layer index 0 in a slot keeps
    // that slot's share procedural, so paint fades in with brush pressure
    if (TerrainEdit_NumChunks() != 0u)
    {
        u8 layerIndex[2];
        u8 layerWeight[2];
        TerrainEdit_MaterialWeights(worldPos, layerIndex, layerWeight);
        f32 paintWeight = 0.0f;
        float3 paint = F3Zero();
        for (u32 i = 0; i < 2u; i++)
        {
            if (layerIndex[i] == 0u || layerWeight[i] == 0u)
                continue;
            f32 w = (f32)layerWeight[i] / 255.0f;
            paint = F3Add(paint, F3MulF(tPaintPalette[layerIndex[i]], w));
            paintWeight += w;
        }
        if (paintWeight > 0.0f)
        {
            float3 paintColor = F3MulF(paint, 1.0f / paintWeight);
            color = F3Lerp(&color, &paintColor, Saturatef32(paintWeight));
        }
    }

    float3 lightDir = F3NormSafe((float3){ 0.45f, 0.82f, 0.35f });
    f32 lambert = Saturatef32(F3Dot(normal, lightDir));
    f32 sky = Saturatef32(normal.y) * 0.18f;
    f32 shade = 0.42f + lambert * 0.48f + sky;
    return tPackColor(F3MulF(color, shade));
}

// thread safe: TerrainDensity_SampleChunk is pure + edit overlay is mutex guarded
static void tBuildDensity(int3 chunkMin, s32 lod, f32* out)
{
    s32 densitySize = T_EXAMPLE_CHUNK_SIZE + 3;
    s32 step = T_EXAMPLE_CHUNK_SIZE << lod;
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

static const f32 tLODDistance[T_EXAMPLE_LOD_COUNT] = { 48.0f, 112.0f, 240.0f, 448.0f };

// true when the quadtree node at (nodeX, nodeZ, lod) splits into finer children:
// the closest point of its XZ box lies inside the next-finer lod's distance ring.
// same test tSubmitNode uses, so the mask stays consistent with traversal
static bool tNodeSplits(s32 nodeX, s32 nodeZ, s32 lod, f32 lodFactor)
{
    if (lod <= 0)
        return false;
    s32 step = T_EXAMPLE_CHUNK_SIZE << lod;
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
    for (s32 l = (s32)T_EXAMPLE_LOD_COUNT - 1; l >= lod; l--)
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
    if (tExample.fixedArea || lod == 0)
        return 0;
    s32 step = T_EXAMPLE_CHUNK_SIZE << lod;
    s32 chunkX = FloorDiv(min.x, step);
    s32 chunkZ = FloorDiv(min.z, step);
    f32 lodFactor = tExample.lodFactor;
    s32 mask = 0;
    if (tNeighbourFiner(chunkX - 1, chunkZ, lod, lodFactor)) mask |= 1;  // -x
    if (tNeighbourFiner(chunkX + 1, chunkZ, lod, lodFactor)) mask |= 8;  // +x
    if (tNeighbourFiner(chunkX, chunkZ - 1, lod, lodFactor)) mask |= 4;  // -z
    if (tNeighbourFiner(chunkX, chunkZ + 1, lod, lodFactor)) mask |= 32; // +z
    return mask;
}

static void tPushTriangleVertex(tExampleBuildJob* job, float3 p, u32 color)
{
	ALineVertex vertex = { p.x, p.y, p.z, color };
	job->buildVertices[job->buildVertexCount++] = vertex;
}

// runs on a worker: reads only the job's own scratch mesh and buffers, plus the
// thread safe density/edit/params reads inside tTerrainColor
static void tAppendMeshSlotTriangles(tExampleBuildJob* job, tMeshDataSlot slot)
{
    const tMeshData* mesh = tMeshDataContainerGetConst(&job->scratchMesh, slot);
    if (!mesh || !mesh->vertices || !mesh->indices || !job->buildVertices)
        return;

    size_t vertexCount = (size_t)mesh->numVertices;
    size_t indexCount = (size_t)mesh->numIndices;
    v128f offset = VecI32ToF32(VeciLoad((const u32*)&job->min.x));
    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
		if (job->buildVertexCount + 3u >= T_EXAMPLE_CHUNK_TRIANGLE_CAP)
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

        float3 a = Vec3Get(pa);
        float3 b = Vec3Get(pb);
        float3 c = Vec3Get(pc);
        // safe normalize: secondary-vertex snapping can leave zero-length normals, a
        // plain normalize would spray NaN colors
        float3 na = Vec3Get(Vec3NormVSafe(mesh->vertices[ia].normal));
        float3 nb = Vec3Get(Vec3NormVSafe(mesh->vertices[ib].normal));
        float3 nc = Vec3Get(Vec3NormVSafe(mesh->vertices[ic].normal));
     	// todo: do color sampling in seperate loop because its branchy
		tPushTriangleVertex(job, a, tTerrainColor(a, na));
        tPushTriangleVertex(job, b, tTerrainColor(b, nb));
        tPushTriangleVertex(job, c, tTerrainColor(c, nc));
    }
}

static void tFreeChunkMesh(tExampleChunk* chunk)
{
    if (chunk->heapPtr)
        GeometryHeapFree(GeometryBuffer_TerrainVertex, chunk->heapPtr);
    chunk->heapPtr = NULL;
    chunk->heapFirst = 0;
    chunk->vertexCount = 0;
}

static void tFreePendingMesh(tExampleChunk* chunk)
{
    if (chunk->pendingHeapPtr)
        GeometryHeapFree(GeometryBuffer_TerrainVertex, chunk->pendingHeapPtr);
    chunk->pendingHeapPtr = NULL;
    chunk->pendingHeapFirst = 0;
    chunk->pendingVertexCount = 0;
    chunk->pendingFrames = 0;
    chunk->pendingEmpty = false;
}

static f32 tTriangleAreaSq(const b3Vec3* vertices, u32 a, u32 b, u32 c)
{
    v128f v0 = Vec3Load(&vertices[a].x);
    v128f v1 = Vec3Load(&vertices[b].x);
    v128f v2 = Vec3Load(&vertices[c].x);
    v128f ab = VecSub(v1, v0);
    v128f ac = VecSub(v2, v0);
    v128f cross = Vec3Cross(ab, ac);
    return Vec3DotfV(cross, cross);
}

// releases the chunk's collider slot back to the pool. chunk indices are NOT valid
// collider slots (the chunk pool is 16384, the scene only has MAX_TERRAIN_PHYSICS_CHUNKS
// collider slots), so every physics call goes through the pooled chunk->physicsSlot
static void tDestroyChunkPhysics(tExampleChunk* chunk)
{
    if (!chunk || chunk->physicsSlot < 0)
        return;
    Scene* scene = Scene_GetActive();
    if (scene)
        Scene_PhysicsDestroyTerrainChunk(scene, (u32)chunk->physicsSlot);
    tExample.physicsSlotPool[tExample.physicsSlotCount++] = (u16)chunk->physicsSlot;
    chunk->physicsSlot = -1;
}

static void tSyncChunkPhysics(tExampleChunk* chunk)
{
    Scene* scene = Scene_GetActive();
    if (!scene)
        return;
    if (!chunk || chunk->lod > 1 || !chunk->heapPtr || chunk->vertexCount < 3u)
    {
        tDestroyChunkPhysics(chunk);
        return;
    }

    if (chunk->physicsSlot < 0)
    {
        if (tExample.physicsSlotCount == 0u)
        {
            AX_WARN("transvoxel example terrain physics slots exhausted");
            return;
        }
        chunk->physicsSlot = (s32)tExample.physicsSlotPool[--tExample.physicsSlotCount];
    }

    u32 vertexCount = chunk->vertexCount - (chunk->vertexCount % 3u);
    ArenaMark mark = ArenaSave(&GlobalArena);
    b3Vec3* vertices = (b3Vec3*)ArenaPushGlobal(vertexCount * sizeof(b3Vec3));
    s32* indices = (s32*)ArenaPushGlobal(vertexCount * sizeof(s32));
    if (!vertices || !indices)
    {
        ArenaRestore(&GlobalArena, mark);
        AX_WARN("transvoxel example terrain physics scratch allocation failed");
        tDestroyChunkPhysics(chunk);
        return;
    }

    const ALineVertex* source = (const ALineVertex*)gGFX.TerrainVertexBuffer + chunk->heapFirst;
    for (u32 i = 0; i < vertexCount; i++)
        vertices[i] = (b3Vec3){ source[i].x, source[i].y, source[i].z };

    u32 outIndexCount = 0u;
    for (u32 i = 0; i + 2u < vertexCount; i += 3u)
    {
        if (tTriangleAreaSq(vertices, i, i + 1u, i + 2u) <= 1.0e-10f)
            continue;
        indices[outIndexCount++] = (s32)i;
        indices[outIndexCount++] = (s32)(i + 1u);
        indices[outIndexCount++] = (s32)(i + 2u);
    }

    if (outIndexCount < 3u || !Scene_PhysicsSyncTerrainChunkMesh(scene, (u32)chunk->physicsSlot, vertices, vertexCount, indices, outIndexCount))
        tDestroyChunkPhysics(chunk);
    ArenaRestore(&GlobalArena, mark);
}

// worker-side chunk build: density -> transvoxel mesh -> colored soup -> parked in the
// TerrainVertex heap. touches only the job slot; the heap, upload queue, density field
// and edit storage are all internally synchronized. CPU scratch (density grid, vertex
// buffer, mesher caches via ArenaPushGlobal) comes from a private thread scratch arena
static void tBuildJobRun(void* userData)
{
    tExampleBuildJob* job = (tExampleBuildJob*)userData;
    job->heapPtr = NULL;
    job->heapFirst = 0;
    job->vertexCount = 0;
    job->failed = false;
    job->buildVertices = NULL;
    job->buildVertexCount = 0;

    ArenaScratch scratch;
    if (!ArenaBeginScratch(&scratch, 1u << 20, "terrainChunkBuild"))
    {
        job->failed = true;
        return;
    }

    s32 densitySize = T_EXAMPLE_CHUNK_SIZE + 3;
    size_t densityCount = (size_t)densitySize * (size_t)densitySize * (size_t)densitySize;
    f32* density = (f32*)ArenaPushGlobal(sizeof(f32) * densityCount);
    job->buildVertices = (ALineVertex*)ArenaPushGlobal(sizeof(ALineVertex) * T_EXAMPLE_CHUNK_TRIANGLE_CAP);
    if (!density || !job->buildVertices)
    {
        AX_WARN("transvoxel example build scratch allocation failed");
        job->failed = true;
        job->buildVertices = NULL;
        ArenaEndScratch(&scratch);
        return;
    }

    tBuildDensity(job->min, job->lod, density);
    if (!tTransvoxelMesherMesh(&tExample.generator, job->min, T_EXAMPLE_CHUNK_SIZE, density,
                               job->lod, job->neighboursMask, &job->scratchMesh, NULL))
    {
        AX_WARN("transvoxel example chunk mesh build failed");
        job->failed = true;
        job->buildVertices = NULL;
        ArenaEndScratch(&scratch);
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

    // park the mesh in the TerrainVertex geometry heap (ALineVertex is the same 16 byte
    // stride) and queue the range for the shared GPU-mirror flush; empty chunks leave
    // heapPtr NULL and the main thread integrates them as pendingEmpty
    u32 vertexCount = job->buildVertexCount;
    if (vertexCount > 0)
    {
        void* raw = NULL;
        u32 first = GeometryHeapAlloc(GeometryBuffer_TerrainVertex, vertexCount, &raw);
        if (first == GEOMETRY_ALLOC_FAIL)
        {
            AX_WARN("transvoxel example terrain heap full, chunk dropped");
            job->failed = true;
        }
        else
        {
            MemCopy((ALineVertex*)gGFX.TerrainVertexBuffer + first, job->buildVertices, vertexCount * sizeof(ALineVertex));
            Rendering_QueueGeometryUpload(GeometryBuffer_TerrainVertex, first, first + vertexCount);
            job->heapPtr = raw;
            job->heapFirst = first;
            job->vertexCount = vertexCount;
        }
    }

    job->buildVertices = NULL;
    ArenaEndScratch(&scratch);
}

// picks a free job slot and launches the chunk build on a worker. false when the
// per-frame cap is hit, every slot is busy, or the job queue is full; the chunk keeps
// its dirty flag in those cases and retries next frame
static bool tScheduleBuild(u32 chunkIndex, tExampleChunk* chunk)
{
    if (tExample.builtThisFrame >= T_EXAMPLE_MAX_BUILDS_PER_FRAME)
        return false;

    tExampleBuildJob* job = NULL;
    for (u32 i = 0; i < T_EXAMPLE_MAX_BUILD_JOBS; i++)
    {
        if (!tExample.buildJobs[i].busy)
        {
            job = &tExample.buildJobs[i];
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
    job->handle = JobSystem_Execute(tExample.jobSystem, tBuildJobRun, job);
    if (job->handle == 0)
    {
        job->busy = false;
        chunk->dirty = true;
        chunk->building = false;
        return false;
    }
    tExample.builtThisFrame++;
    return true;
}

// main thread, once per frame before the pending promote: moves finished job results
// into the chunks' pending slots so they run through the usual 2-frame promote
static void tIntegrateFinishedBuilds(void)
{
    for (u32 i = 0; i < T_EXAMPLE_MAX_BUILD_JOBS; i++)
    {
        tExampleBuildJob* job = &tExample.buildJobs[i];
        if (!job->busy || !JobSystem_IsJobDone(tExample.jobSystem, job->handle))
            continue;

        tExampleChunk* chunk = &tExample.chunks[job->chunkIndex];
        chunk->building = false;
        chunk->built = true;
        job->busy = false;

        if (job->failed)
        {
            // dropped (mesher failure / heap full); a later invalidation re-dirties it
            if (job->heapPtr)
                GeometryHeapFree(GeometryBuffer_TerrainVertex, job->heapPtr);
            job->heapPtr = NULL;
            continue;
        }

        tFreePendingMesh(chunk);
        if (job->vertexCount == 0)
        {
            if (chunk->heapPtr)
            {
                // genuinely empty now: retire the live mesh through the promote delay
                chunk->pendingEmpty = true;
                chunk->pendingFrames = 2u;
            }
            else
            {
                tDestroyChunkPhysics(chunk);
            }
            tExample.emptyChunks++;
            continue;
        }

        chunk->pendingHeapPtr = job->heapPtr;
        chunk->pendingHeapFirst = job->heapFirst;
        chunk->pendingVertexCount = job->vertexCount;
        chunk->pendingFrames = 2u;
        job->heapPtr = NULL;
    }
}

// waits for every in-flight build and discards results that never got integrated;
// must run before anything invalidates chunk indices or tears the system down
static void tDrainBuildJobs(void)
{
    if (!tExample.jobSystem)
        return;
    JobSystem_Wait(tExample.jobSystem);
    for (u32 i = 0; i < T_EXAMPLE_MAX_BUILD_JOBS; i++)
    {
        tExampleBuildJob* job = &tExample.buildJobs[i];
        if (!job->busy)
            continue;
        if (job->heapPtr)
            GeometryHeapFree(GeometryBuffer_TerrainVertex, job->heapPtr);
        job->heapPtr = NULL;
        job->busy = false;
        tExample.chunks[job->chunkIndex].building = false;
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

static void tClearChunkCache(void)
{
    tDrainBuildJobs();
    RendererSetTerrainChunkDraws(NULL, 0);
    for (u32 i = 0; i < tExample.chunkCount; i++)
    {
        tDestroyChunkPhysics(&tExample.chunks[i]);
        tFreeChunkMesh(&tExample.chunks[i]);
        tFreePendingMesh(&tExample.chunks[i]);
    }
    tExample.chunkCount = 0;
    tExample.physicsSyncCursor = 0;
    HMClear(&tExample.chunkLookup);
}

static void tPromotePendingMeshes(void)
{
    for (u32 i = 0; i < tExample.chunkCount; i++)
    {
        tExampleChunk* chunk = &tExample.chunks[i];
        if (!chunk->pendingHeapPtr && !chunk->pendingEmpty)
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

        chunk->heapPtr = chunk->pendingHeapPtr;
        chunk->heapFirst = chunk->pendingHeapFirst;
        chunk->vertexCount = chunk->pendingVertexCount;
        chunk->pendingHeapPtr = NULL;
        chunk->pendingHeapFirst = 0;
        chunk->pendingVertexCount = 0;
        chunk->physicsDirty = true;
    }
}

static void tSyncDirtyPhysics(void)
{
    if (tExample.chunkCount == 0u)
        return;

    u32 synced = 0u;
    u32 visited = 0u;
    while (visited < tExample.chunkCount && synced < T_EXAMPLE_MAX_PHYSICS_SYNCS_PER_FRAME)
    {
        u32 i = tExample.physicsSyncCursor++;
        if (tExample.physicsSyncCursor >= tExample.chunkCount)
            tExample.physicsSyncCursor = 0u;
        visited++;

        tExampleChunk* chunk = &tExample.chunks[i];
        if (!chunk->physicsDirty)
            continue;

        tSyncChunkPhysics(chunk);
        chunk->physicsDirty = false;
        synced++;
    }
}

static tExampleChunk* tGetChunk(int3 min, s32 lod)
{
    s32 neighboursMask = tColumnMask(min, lod);
    u32* found = (u32*)HMFind(&tExample.chunkLookup, tChunkKey(min, lod));
    tExampleChunk* chunk = found ? &tExample.chunks[*found] : NULL;
    if (chunk)
    {
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
        if (chunk->dirty && !chunk->building && !chunk->pendingHeapPtr && !chunk->pendingEmpty)
            tScheduleBuild(*found, chunk);
        return chunk;
    }

    if (tExample.chunkCount >= T_EXAMPLE_MAX_CHUNKS)
    {
        tClearChunkCache();
        AX_LOG("transvoxel example chunk cache reset");
    }

    u32 index = tExample.chunkCount++;
    chunk = &tExample.chunks[index];
    *chunk = (tExampleChunk){ .min = min, .lod = lod, .neighboursMask = neighboursMask, .physicsSlot = -1 };
    s32 worldSize = T_EXAMPLE_CHUNK_SIZE << lod;
    chunk->aabbMin = ToFloat3(min);
    chunk->aabbMax = F3AddF(chunk->aabbMin, (f32)worldSize);
    HMInsert(&tExample.chunkLookup, tChunkKey(min, lod), &index);
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

static s32 tAbss32(s32 value)
{
    return value < 0 ? -value : value;
}

// returns false only when the draw list is full; empty chunks are a successful no-op so
// one air/solid chunk does not abort the rest of the LOD ring. every chunk submits a heap
// draw range (no vertex copy); the brush highlight is a terrain shader uniform now
static bool tAppendChunkTriangles(const tExampleChunk* chunk)
{
    // must short-circuit: bitwise | evaluates chunk->vertexCount on a NULL chunk
    if (!chunk || chunk->vertexCount == 0 || !chunk->heapPtr)
        return true;

    if (tExample.numChunkDraws >= MAX_TERRAIN_CHUNK_DRAWS)
        return false;
    TerrainChunkDraw draw = { chunk->heapFirst, chunk->vertexCount };
	tExample.chunkDraws[tExample.numChunkDraws++] = draw;
    return true;
}

// a chunk counts as presentable when it is built and either has a live (drawable) mesh
// or is genuinely empty. a fresh build whose mesh still sits in the pending slot is NOT
// presentable: it has no drawable geometry until tPromotePendingMeshes swaps it
// in, and treating it as ready made the LOD descend draw nothing for the promote frames.
static bool tChunkPresentable(const tExampleChunk* chunk)
{
    if (!chunk || !chunk->built)
        return false;
    if (chunk->heapPtr)
        return true;
    return !chunk->pendingHeapPtr && !chunk->pendingEmpty;
}

// find-only: true when every chunk of the column already exists and is presentable;
// never requests builds, used for the coarser-lod fallback checks
static bool tColumnPresent(s32 chunkX, s32 chunkZ, s32 lod)
{
    s32 step = T_EXAMPLE_CHUNK_SIZE << lod;
    f32 yMin, yMax;
    TerrainDensity_GetYRange(&yMin, &yMax);
    s32 chunkYMin = (s32)Floorf32(yMin / (f32)step);
    s32 chunkYMax = (s32)Floorf32(yMax / (f32)step);

    for (s32 y = chunkYMin; y <= chunkYMax; y++)
    {
        int3 min = { chunkX * step, y * step, chunkZ * step };
        u32* found = (u32*)HMFind(&tExample.chunkLookup, tChunkKey(min, lod));
        if (!found || !tChunkPresentable(&tExample.chunks[*found]))
            return false;
    }
    return true;
}

// true when every chunk of the column at this lod is presentable; missing chunks are
// requested through tGetChunk so they build within the per-frame budget
static bool tColumnBuilt(s32 chunkX, s32 chunkZ, s32 lod)
{
    s32 step = T_EXAMPLE_CHUNK_SIZE << lod;
    f32 yMin, yMax;
    TerrainDensity_GetYRange(&yMin, &yMax);
    s32 chunkYMin = (s32)Floorf32(yMin / (f32)step);
    s32 chunkYMax = (s32)Floorf32(yMax / (f32)step);

    bool built = true;
    for (s32 y = chunkYMin; y <= chunkYMax; y++)
    {
        int3 min = { chunkX * step, y * step, chunkZ * step };
        tExampleChunk* chunk = tGetChunk(min, lod);
        if (!tChunkPresentable(chunk))
            built = false;
    }
    return built;
}

// chunked-LOD quadtree: split a node when its closest point lies inside the finer
// LOD's range, otherwise emit it at this lod. every split child is always handled,
// so the selection is gap-free — the old per-ring center-distance test dropped
// chunks whose center fell between two rings. returns false when the submit
// buffer is full and traversal must stop.
static bool tSubmitNode(s32 chunkX, s32 chunkZ, s32 lod, f32 lodFactor, const FrustumPlanes* frustum, bool useFrustum)
{
    s32 step = T_EXAMPLE_CHUNK_SIZE << lod;
    f32 minX = (f32)(chunkX * step);
    f32 minZ = (f32)(chunkZ * step);
    f32 closestX = Clampf32(g_Camera.position.x, minX, minX + (f32)step);
    f32 closestZ = Clampf32(g_Camera.position.z, minZ, minZ + (f32)step);
    f32 dx = closestX - g_Camera.position.x;
    f32 dz = closestZ - g_Camera.position.z;
    f32 distanceSq = dx * dx + dz * dz;

    // global draw distance, applied only at the tree root: inner nodes always emit
    // or split so a split parent can never leave uncovered children
    if (lod == (s32)T_EXAMPLE_LOD_COUNT - 1)
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
                childrenReady &= tColumnBuilt(chunkX * 2 + (s32)(i & 1u), chunkZ * 2 + (s32)(i >> 1u), lod - 1);

            if (childrenReady)
            {
                for (u32 i = 0; i < 4u; i++)
                {
                    if (!tSubmitNode(chunkX * 2 + (s32)(i & 1u), chunkZ * 2 + (s32)(i >> 1u), lod - 1, lodFactor, frustum, useFrustum))
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
    if (!tColumnBuilt(chunkX, chunkZ, lod) && lod > 0)
    {
        bool childrenPresent = true;
        for (u32 i = 0; i < 4u; i++)
            childrenPresent &= tColumnPresent(chunkX * 2 + (s32)(i & 1u), chunkZ * 2 + (s32)(i >> 1u), lod - 1);

        if (childrenPresent)
        {
            for (u32 i = 0; i < 4u; i++)
            {
                if (!tSubmitNode(chunkX * 2 + (s32)(i & 1u), chunkZ * 2 + (s32)(i >> 1u), lod - 1, lodFactor, frustum, useFrustum))
                    return false;
            }
            return true;
        }
    }

    // vertical band that can contain surface geometry for the engine density field
    f32 yMin, yMax;
    TerrainDensity_GetYRange(&yMin, &yMax);
    s32 chunkYMin = (s32)Floorf32(yMin / (f32)step);
    s32 chunkYMax = (s32)Floorf32(yMax / (f32)step);

    for (s32 y = chunkYMin; y <= chunkYMax; y++)
    {
        int3 min = { chunkX * step, y * step, chunkZ * step };
        float3 aabbMin = ToFloat3(min);
        float3 aabbMax = F3AddF(aabbMin, (f32)step);
        if (useFrustum && !tAABBVisible(aabbMin, aabbMax, frustum))
        {
            tExample.culledChunks++;
            continue;
        }

        tExampleChunk* chunk = tGetChunk(min, lod);
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
        s32 minChunkX = FloorDiv(-halfSize, T_EXAMPLE_CHUNK_SIZE);
        s32 maxChunkX = FloorDiv(fixedSize - halfSize - 1, T_EXAMPLE_CHUNK_SIZE);
        s32 minChunkZ = minChunkX;
        s32 maxChunkZ = maxChunkX;

        f32 yMin, yMax;
        TerrainDensity_GetYRange(&yMin, &yMax);
        s32 chunkYMin = (s32)Floorf32(yMin / (f32)T_EXAMPLE_CHUNK_SIZE);
        s32 chunkYMax = (s32)Floorf32(yMax / (f32)T_EXAMPLE_CHUNK_SIZE);

        for (s32 z = minChunkZ; z <= maxChunkZ; z++)
        for (s32 x = minChunkX; x <= maxChunkX; x++)
        for (s32 y = chunkYMin; y <= chunkYMax; y++)
        {
            int3 min = { x * T_EXAMPLE_CHUNK_SIZE, y * T_EXAMPLE_CHUNK_SIZE, z * T_EXAMPLE_CHUNK_SIZE };
            tExampleChunk* chunk = tGetChunk(min, 0);
            if (!chunk || !chunk->built)
                continue;
            if (!tAppendChunkTriangles(chunk))
                return;
        }
        return;
    }

    const s32 rootLOD = (s32)T_EXAMPLE_LOD_COUNT - 1;
    s32 rootStep = T_EXAMPLE_CHUNK_SIZE << rootLOD;
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
                if (Maxs32(tAbss32(x), tAbss32(z)) != shell)
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
    if (now - tExample.lastStatsTicks < 1000u)
        return;
    tExample.lastStatsTicks = now;
    AX_LOG("transvoxel example: draws=%u built=%u cached=%u culled=%u empty=%u",
           tExample.numChunkDraws, tExample.builtThisFrame,
           tExample.chunkCount, tExample.culledChunks, tExample.emptyChunks);
}

static bool tInit(void)
{
    if (tExample.initialized)
        return true;
    MemSet(&tExample, 0, sizeof(tExample));
	
	tExample.jobSystem = JobSystem_Create(0, 0);
	if (!tExample.jobSystem) { AX_WARN("terrain job system create failed!"); return false; } // should never happen though

    tExample.generator.noise3D = tDensityNoise;
    tExample.generator.noise3DStrength = 1.0f;

    for (u32 i = 0; i < MAX_TERRAIN_PHYSICS_CHUNKS; i++)
        tExample.physicsSlotPool[i] = (u16)i;
    tExample.physicsSlotCount = MAX_TERRAIN_PHYSICS_CHUNKS;

    tExample.chunkDraws    = (TerrainChunkDraw*)AllocateTLSFGlobal(sizeof(TerrainChunkDraw) * MAX_TERRAIN_CHUNK_DRAWS);
    tExample.chunkLookup   = HMCreate(T_EXAMPLE_MAX_CHUNKS, sizeof(u32));
    if (!tExample.chunkDraws)
    {
        AX_WARN("transvoxel example allocation failed");
        tDestroy();
        return false;
    }

    // one scratch mesh container per job slot so workers never share mesher output
    for (u32 i = 0; i < T_EXAMPLE_MAX_BUILD_JOBS; i++)
    {
        if (!tMeshDataContainerInit(&tExample.buildJobs[i].scratchMesh, 8192, 24576, 2048))
        {
            tDestroy();
            return false;
        }
    }

    tExample.initialized = true;
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
    tPromotePendingMeshes();
    tExample.numChunkDraws = 0;
    tExample.builtThisFrame = 0;
    tExample.culledChunks = 0;
    tExample.emptyChunks = 0;
    mat4x4 viewProj = M44Multiply(g_Camera.view, g_Camera.projection);
    FrustumPlanes frustum = CreateFrustumPlanesRevZ(viewProj);

    f32 lodFactor = Maxf32(g_RenderSettings.terrainLodFactor, 0.25f);
    const TerrainGenParams* genParams = Terrain_GetGenParams();
    tExample.lodFactor = lodFactor;
    tExample.fixedArea = genParams && genParams->fixedArea;
    tSubmitTerrain(lodFactor, &frustum, true);
    if (tExample.numChunkDraws == 0)
        tSubmitTerrain(lodFactor, &frustum, false);

    tSyncDirtyPhysics();
    tLogStats();
    RendererSetTerrainChunkDraws(tExample.chunkDraws, tExample.numChunkDraws);
    RendererSetTerrainBrush(tExample.brushPos, tExample.brushActive ? tExample.brushRadius : 0.0f);
}

void tInvalidateAll(void)
{
    if (tExample.initialized)
        tClearChunkCache();
}

void tInvalidateRegion(float3 mn, float3 mx)
{
    if (!tExample.initialized)
        return;

    for (u32 i = 0; i < tExample.chunkCount; i++)
    {
        tExampleChunk* chunk = &tExample.chunks[i];
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
    tExample.brushPos = position;
    tExample.brushRadius = radius;
    tExample.brushActive = active && radius > 0.0f;
}

void tDestroy(void)
{
    RendererSetTerrainChunkDraws(NULL, 0);
    tClearChunkCache(); // drains in-flight builds first
    HMDestroy(&tExample.chunkLookup);
    DeAllocateTLSFGlobal(tExample.chunkDraws);
    for (u32 i = 0; i < T_EXAMPLE_MAX_BUILD_JOBS; i++)
        tMeshDataContainerDestroy(&tExample.buildJobs[i].scratchMesh);
    if (tExample.jobSystem)
        JobSystem_Destroy(tExample.jobSystem);
    MemSet(&tExample, 0, sizeof(tExample));
}
