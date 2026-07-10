#include "Source/Terrain/TransvoxelUnity.h"
#include "Source/Terrain/TerrainInternal.h"
#include "Include/Camera.h"
#include "Include/Terrain.h"
#include "Include/Rendering.h"
#include "Include/Platform.h"
#include "Include/Scene.h"
#include "Include/Memory.h"

#define T_EXAMPLE_CHUNK_SIZE 16
#define T_EXAMPLE_LOD_COUNT 4u
#define T_EXAMPLE_MAX_CHUNKS (16384u)
#define T_EXAMPLE_CHUNK_TRIANGLE_CAP (8192u * 4u)
#define T_EXAMPLE_MAX_BUILDS_PER_FRAME 16u
#define T_EXAMPLE_MAX_PHYSICS_SYNCS_PER_FRAME 2u

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
    bool  built;
    bool  dirty;
    bool  physicsDirty;
    bool  pendingEmpty;
} tExampleChunk;

typedef struct tTransvoxelExample_
{
    tDensityGenerator generator;
    tMeshDataContainer scratchMesh;
    f32* densityData;
    ALineVertex* buildVertices;  // per-build scratch, copied into the geometry heap
    TerrainChunkDraw* chunkDraws; // per-frame heap ranges, one indirect multidraw
    tExampleChunk chunks[T_EXAMPLE_MAX_CHUNKS];
    HashMap chunkLookup; // key: tExampleChunkKey, value: u32 index into chunks
    u32 chunkCount;
    u32 builtThisFrame;
    u32 submittedChunks;
    u32 culledChunks;
    u32 emptyChunks;
    u32 physicsSyncCursor;
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
static f32 tExampleDensityNoise(f32 x, f32 y, f32 z, void* userData)
{
    (void)userData;
    return y - TerrainDensity_At(x, y, z);
}

static u32 tExamplePackColor(float3 color)
{
    u32 r = (u32)(Saturatef32(color.x) * 255.0f + 0.5f);
    u32 g = (u32)(Saturatef32(color.y) * 255.0f + 0.5f);
    u32 b = (u32)(Saturatef32(color.z) * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16);
}

// flat colors for painted layers; index is the packed TerrainEdit layer id (stored
// layer + 1, 0 = unpainted/procedural). first four follow the builtin texture layer
// order: grass, dirt, rock, sand; the rest are distinct spare hues
static const float3 tExamplePaintPalette[16] = {
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

static u32 tExampleTerrainColor(float3 worldPos, float3 normal)
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
            paint = F3Add(paint, F3MulF(tExamplePaintPalette[layerIndex[i]], w));
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
    return tExamplePackColor(F3MulF(color, shade));
}

static bool tExampleBuildDensity(int3 chunkMin, s32 lod)
{
    s32 densitySize = T_EXAMPLE_CHUNK_SIZE + 3;
    size_t densityCount = (size_t)densitySize * (size_t)densitySize * (size_t)densitySize;
    if (!tExample.densityData || ArrayCapacity(tExample.densityData) < densityCount)
    {
        AX_WARN("transvoxel example density scratch missing");
        return false;
    }
    ArrayFieldSet(tExample.densityData, ArrayField_Length, densityCount);

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
        tExample.densityData[dst] = (f32)samples[src] * invScale;
    }

    return true;
}

static const f32 tExampleLODDistance[T_EXAMPLE_LOD_COUNT] = { 48.0f, 112.0f, 240.0f, 448.0f };

// true when the quadtree node at (nodeX, nodeZ, lod) splits into finer children:
// the closest point of its XZ box lies inside the next-finer lod's distance ring.
// same test tExampleSubmitNode uses, so the mask stays consistent with traversal
static bool tExampleNodeSplits(s32 nodeX, s32 nodeZ, s32 lod, f32 lodFactor)
{
    if (lod <= 0)
        return false;
    s32 step = T_EXAMPLE_CHUNK_SIZE << lod;
    f32 minX = (f32)(nodeX * step);
    f32 minZ = (f32)(nodeZ * step);
    f32 dx = Clampf32(g_Camera.position.x, minX, minX + (f32)step) - g_Camera.position.x;
    f32 dz = Clampf32(g_Camera.position.z, minZ, minZ + (f32)step) - g_Camera.position.z;
    f32 splitDistance = tExampleLODDistance[lod - 1] * lodFactor;
    return dx * dx + dz * dz < splitDistance * splitDistance;
}

// does the column at (chunkX, chunkZ) on this lod's grid render at a finer lod?
// walks the quadtree top-down: every ancestor must split to reach this lod, and the
// node at this lod itself must split for its children to render instead
static bool tExampleNeighbourFiner(s32 chunkX, s32 chunkZ, s32 lod, f32 lodFactor)
{
    if (lod == 0)
        return false;
    for (s32 l = (s32)T_EXAMPLE_LOD_COUNT - 1; l >= lod; l--)
    {
        // arithmetic shift floors negative chunk coords onto the coarser grid
        s32 nodeX = chunkX >> (l - lod);
        s32 nodeZ = chunkZ >> (l - lod);
        if (!tExampleNodeSplits(nodeX, nodeZ, l, lodFactor))
            return false;
    }
    return true;
}

// transvoxel neighbour mask for a chunk column: transition cells live on the COARSE
// side in this port (the mesher samples the face at half steps, i.e. the finer
// neighbour's resolution), so a bit is set when that face's neighbour renders finer.
// vertical neighbours share the column and lod, so the y bits are never set
static s32 tExampleColumnMask(int3 min, s32 lod)
{
    if (tExample.fixedArea || lod == 0)
        return 0;
    s32 step = T_EXAMPLE_CHUNK_SIZE << lod;
    s32 chunkX = FloorDiv(min.x, step);
    s32 chunkZ = FloorDiv(min.z, step);
    f32 lodFactor = tExample.lodFactor;
    s32 mask = 0;
    if (tExampleNeighbourFiner(chunkX - 1, chunkZ, lod, lodFactor)) mask |= 1;  // -x
    if (tExampleNeighbourFiner(chunkX + 1, chunkZ, lod, lodFactor)) mask |= 8;  // +x
    if (tExampleNeighbourFiner(chunkX, chunkZ - 1, lod, lodFactor)) mask |= 4;  // -z
    if (tExampleNeighbourFiner(chunkX, chunkZ + 1, lod, lodFactor)) mask |= 32; // +z
    return mask;
}

static bool tExamplePushTriangleVertex(ALineVertex** vertices, float3 p, u32 color)
{
    if (!vertices || !*vertices)
        return false;
    if (ArrayLength(*vertices) >= ArrayCapacity(*vertices))
        return false;

    ALineVertex vertex = { p.x, p.y, p.z, color };
    ArrayPush(*vertices, vertex);
    return true;
}

static void tExampleAppendMeshSlotTriangles(const tExampleChunk* chunk, tMeshDataSlot slot)
{
    const tMeshData* mesh = tMeshDataContainerGetConst(&tExample.scratchMesh, slot);
    if (!mesh || !mesh->vertices || !mesh->indices || !tExample.buildVertices)
        return;

    size_t vertexCount = (size_t)mesh->numVertices;
    size_t indexCount = (size_t)mesh->numIndices;
    float3 offset = ToFloat3(chunk->min);
    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
        if (ArrayLength(tExample.buildVertices) + 3u > ArrayCapacity(tExample.buildVertices))
            return;

        u32 ia = mesh->indices[i + 0];
        u32 ib = mesh->indices[i + 1];
        u32 ic = mesh->indices[i + 2];
        if ((size_t)ia >= vertexCount || (size_t)ib >= vertexCount || (size_t)ic >= vertexCount)
            continue;

        float3 a = F3Add(Vec3Get(mesh->vertices[ia].position), offset);
        float3 b = F3Add(Vec3Get(mesh->vertices[ib].position), offset);
        float3 c = F3Add(Vec3Get(mesh->vertices[ic].position), offset);

        // secondary-vertex snapping and transition cells produce degenerate triangles
        // (verts collapse onto each other); skip them here instead of shading zero-area
        float3 ab = F3Sub(b, a);
        float3 ac = F3Sub(c, a);
        float3 cross = F3Cross(&ab, &ac);
        if (F3Dot(cross, cross) <= 1.0e-12f)
            continue;

        float3 na = F3NormSafe(Vec3Get(mesh->vertices[ia].normal));
        float3 nb = F3NormSafe(Vec3Get(mesh->vertices[ib].normal));
        float3 nc = F3NormSafe(Vec3Get(mesh->vertices[ic].normal));
        if (!tExamplePushTriangleVertex(&tExample.buildVertices, a, tExampleTerrainColor(a, na))) return;
        if (!tExamplePushTriangleVertex(&tExample.buildVertices, b, tExampleTerrainColor(b, nb))) return;
        if (!tExamplePushTriangleVertex(&tExample.buildVertices, c, tExampleTerrainColor(c, nc))) return;
    }
}

static void tExampleFreeChunkMesh(tExampleChunk* chunk)
{
    if (chunk->heapPtr)
        GeometryHeapFree(GeometryBuffer_TerrainVertex, chunk->heapPtr);
    chunk->heapPtr = NULL;
    chunk->heapFirst = 0;
    chunk->vertexCount = 0;
}

static void tExampleFreePendingMesh(tExampleChunk* chunk)
{
    if (chunk->pendingHeapPtr)
        GeometryHeapFree(GeometryBuffer_TerrainVertex, chunk->pendingHeapPtr);
    chunk->pendingHeapPtr = NULL;
    chunk->pendingHeapFirst = 0;
    chunk->pendingVertexCount = 0;
    chunk->pendingFrames = 0;
    chunk->pendingEmpty = false;
}

static f32 tExampleTriangleAreaSq(const b3Vec3* vertices, u32 a, u32 b, u32 c)
{
    float3 v0 = { vertices[a].x, vertices[a].y, vertices[a].z };
    float3 v1 = { vertices[b].x, vertices[b].y, vertices[b].z };
    float3 v2 = { vertices[c].x, vertices[c].y, vertices[c].z };
    float3 ab = F3Sub(v1, v0);
    float3 ac = F3Sub(v2, v0);
    float3 cross = F3Cross(&ab, &ac);
    return F3Dot(cross, cross);
}

static void tExampleDestroyChunkPhysics(u32 chunkIndex)
{
    Scene* scene = Scene_GetActive();
    if (scene && chunkIndex < MAX_TERRAIN_PHYSICS_CHUNKS)
        Scene_PhysicsDestroyTerrainChunk(scene, chunkIndex);
}

static void tExampleSyncChunkPhysics(u32 chunkIndex, const tExampleChunk* chunk)
{
    Scene* scene = Scene_GetActive();
    if (!scene || chunkIndex >= MAX_TERRAIN_PHYSICS_CHUNKS)
        return;
    if (!chunk || chunk->lod > 1 || !chunk->heapPtr || chunk->vertexCount < 3u)
    {
        Scene_PhysicsDestroyTerrainChunk(scene, chunkIndex);
        return;
    }

    u32 vertexCount = chunk->vertexCount - (chunk->vertexCount % 3u);
    ArenaMark mark = ArenaSave(&GlobalArena);
    b3Vec3* vertices = (b3Vec3*)ArenaAllocGlobal(vertexCount * sizeof(b3Vec3));
    s32* indices = (s32*)ArenaAllocGlobal(vertexCount * sizeof(s32));
    if (!vertices || !indices)
    {
        ArenaRestore(&GlobalArena, mark);
        AX_WARN("transvoxel example terrain physics scratch allocation failed");
        Scene_PhysicsDestroyTerrainChunk(scene, chunkIndex);
        return;
    }

    const ALineVertex* source = (const ALineVertex*)gGFX.TerrainVertexBuffer + chunk->heapFirst;
    for (u32 i = 0; i < vertexCount; i++)
        vertices[i] = (b3Vec3){ source[i].x, source[i].y, source[i].z };

    u32 outIndexCount = 0u;
    for (u32 i = 0; i + 2u < vertexCount; i += 3u)
    {
        if (tExampleTriangleAreaSq(vertices, i, i + 1u, i + 2u) <= 1.0e-10f)
            continue;
        indices[outIndexCount++] = (s32)i;
        indices[outIndexCount++] = (s32)(i + 1u);
        indices[outIndexCount++] = (s32)(i + 2u);
    }

    if (outIndexCount < 3u || !Scene_PhysicsSyncTerrainChunkMesh(scene, chunkIndex, vertices, vertexCount, indices, outIndexCount))
        Scene_PhysicsDestroyTerrainChunk(scene, chunkIndex);
    ArenaRestore(&GlobalArena, mark);
}

static bool tExampleBuildChunk(u32 chunkIndex, tExampleChunk* chunk)
{
    if (!chunk)
        return false;

    s32 worldSize = T_EXAMPLE_CHUNK_SIZE << chunk->lod;
    chunk->aabbMin = ToFloat3(chunk->min);
    chunk->aabbMax = F3AddF(chunk->aabbMin, (f32)worldSize);

    if (!tExampleBuildDensity(chunk->min, chunk->lod))
        return false;

    if (!tTransvoxelMesherMesh(&tExample.generator, chunk->min, T_EXAMPLE_CHUNK_SIZE, tExample.densityData,
                               chunk->lod, chunk->neighboursMask, &tExample.scratchMesh, NULL))
    {
        AX_WARN("transvoxel example chunk mesh build failed");
        return false;
    }

    // lod stitching: snap boundary vertices to their secondary positions (shrinks the
    // regular mesh half a cell inward on faces with a finer neighbour) and fill the gap
    // with that face's transition cell strip. faces without a finer neighbour keep
    // primary positions and skip their transition slot
    tMeshDataContainerApplySecondaryVertices(&tExample.scratchMesh, chunk->neighboursMask);
    ArrayFieldSet(tExample.buildVertices, ArrayField_Length, 0);
    tExampleAppendMeshSlotTriangles(chunk, tMeshDataSlot_Main);
    for (u32 bit = 0; bit < 6u; bit++)
    {
        if (chunk->neighboursMask & (1 << bit))
            tExampleAppendMeshSlotTriangles(chunk, (tMeshDataSlot)(tMeshDataSlot_LeftTransition + bit));
    }

    u32 vertexCount = (u32)ArrayLength(tExample.buildVertices);
    tExample.builtThisFrame++;
    chunk->built = true;
    if (vertexCount == 0)
    {
        tExampleFreePendingMesh(chunk);
        if (chunk->heapPtr)
        {
            chunk->pendingEmpty = true;
            chunk->pendingFrames = 2u;
        }
        else
        {
            tExampleDestroyChunkPhysics(chunkIndex);
        }
        tExample.emptyChunks++;
        chunk->built = true;
        chunk->dirty = false;
        return true;
    }

    // park the mesh in the TerrainVertex geometry heap (ALineVertex is the same 16 byte
    // stride) and queue the range for the shared GPU-mirror flush
    void* raw = NULL;
    u32 first = GeometryHeapAlloc(GeometryBuffer_TerrainVertex, vertexCount, &raw);
    if (first == GEOMETRY_ALLOC_FAIL)
    {
        AX_WARN("transvoxel example terrain heap full, chunk dropped");
        return false;
    }

    MemCopy((ALineVertex*)gGFX.TerrainVertexBuffer + first, tExample.buildVertices, vertexCount * sizeof(ALineVertex));
    Rendering_QueueGeometryUpload(GeometryBuffer_TerrainVertex, first, first + vertexCount);
    tExampleFreePendingMesh(chunk);
    chunk->pendingHeapPtr = raw;
    chunk->pendingHeapFirst = first;
    chunk->pendingVertexCount = vertexCount;
    chunk->pendingFrames = 2u;
    chunk->built = true;
    chunk->dirty = false;
    return true;
}

// tChunkPositionKey packs the position into bits 3..63, so the 2-bit lod fits in
// the free low bits and one map covers all LOD levels
static u64 tExampleChunkKey(int3 position, s32 lod)
{
	return ((u64)((u32)(position.x + 0x100000) & 0x1FFFFFu) << 40)
		| ((u64)((u32)(position.y + 0x8000)   & 0xFFFFu)   << 24)
		| ((u64)((u32)(position.z + 0x100000) & 0x1FFFFFu) << 3) | (u64)(u32)lod;
}

static void tExampleClearChunkCache(void)
{
    RendererSetTerrainChunkDraws(NULL, 0);
    for (u32 i = 0; i < tExample.chunkCount; i++)
    {
        tExampleDestroyChunkPhysics(i);
        tExampleFreeChunkMesh(&tExample.chunks[i]);
        tExampleFreePendingMesh(&tExample.chunks[i]);
    }
    tExample.chunkCount = 0;
    tExample.physicsSyncCursor = 0;
    HMClear(&tExample.chunkLookup);
}

static void tExamplePromotePendingMeshes(void)
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

        tExampleFreeChunkMesh(chunk);
        if (chunk->pendingEmpty)
        {
            tExampleDestroyChunkPhysics(i);
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

static void tExampleSyncDirtyPhysics(void)
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

        tExampleSyncChunkPhysics(i, chunk);
        chunk->physicsDirty = false;
        synced++;
    }
}

static tExampleChunk* tExampleGetChunk(int3 min, s32 lod)
{
    s32 neighboursMask = tExampleColumnMask(min, lod);
    u32* found = (u32*)HMFind(&tExample.chunkLookup, tExampleChunkKey(min, lod));
    tExampleChunk* chunk = found ? &tExample.chunks[*found] : NULL;
    if (chunk)
    {
        // a neighbour crossed a lod ring: this chunk's boundary shrink + transition
        // strips no longer match, remesh with the new mask (old mesh keeps drawing
        // until the pending rebuild promotes, so the seam heals without a flash)
        if (chunk->neighboursMask != neighboursMask)
        {
            chunk->neighboursMask = neighboursMask;
            chunk->dirty = true;
        }
        if (chunk->dirty && !chunk->pendingHeapPtr && !chunk->pendingEmpty &&
            tExample.builtThisFrame < T_EXAMPLE_MAX_BUILDS_PER_FRAME)
            tExampleBuildChunk(*found, chunk);
        return chunk;
    }

    // TerrainDensity sampling is expensive; cap fresh builds per frame so the map
    // streams in over a few frames instead of hitching for seconds on the first one
    if (tExample.builtThisFrame >= T_EXAMPLE_MAX_BUILDS_PER_FRAME)
        return NULL;

    if (tExample.chunkCount >= T_EXAMPLE_MAX_CHUNKS)
    {
        tExampleClearChunkCache();
        AX_LOG("transvoxel example chunk cache reset");
    }

    u32 index = tExample.chunkCount++;
    chunk = &tExample.chunks[index];
    *chunk = (tExampleChunk){ .min = min, .lod = lod, .neighboursMask = neighboursMask };
    HMInsert(&tExample.chunkLookup, tExampleChunkKey(min, lod), &index);
    if (!tExampleBuildChunk(index, chunk))
        chunk->built = true;
    return chunk;
}

static bool tExampleAABBVisible(float3 aabbMin, float3 aabbMax, const FrustumPlanes* frustum)
{
    v128f min = Vec3Load(&aabbMin.x);
    v128f max = Vec3Load(&aabbMax.x);
    return CheckAABBCulled(min, max, frustum->planes);
}

static s32 tExampleAbss32(s32 value)
{
    return value < 0 ? -value : value;
}

// returns false only when the draw list is full; empty chunks are a successful no-op so
// one air/solid chunk does not abort the rest of the LOD ring. every chunk submits a heap
// draw range (no vertex copy); the brush highlight is a terrain shader uniform now
static bool tExampleAppendChunkTriangles(const tExampleChunk* chunk)
{
    if (!chunk || chunk->vertexCount == 0 || !chunk->heapPtr)
        return true;

    if (ArrayLength(tExample.chunkDraws) >= ArrayCapacity(tExample.chunkDraws))
        return false;
    TerrainChunkDraw draw = { chunk->heapFirst, chunk->vertexCount };
    ArrayPush(tExample.chunkDraws, draw);
    tExample.submittedChunks++;
    return true;
}

// a chunk counts as presentable when it is built and either has a live (drawable) mesh
// or is genuinely empty. a fresh build whose mesh still sits in the pending slot is NOT
// presentable: it has no drawable geometry until tExamplePromotePendingMeshes swaps it
// in, and treating it as ready made the LOD descend draw nothing for the promote frames.
static bool tExampleChunkPresentable(const tExampleChunk* chunk)
{
    if (!chunk || !chunk->built)
        return false;
    if (chunk->heapPtr)
        return true;
    return !chunk->pendingHeapPtr && !chunk->pendingEmpty;
}

// find-only: true when every chunk of the column already exists and is presentable;
// never requests builds, used for the coarser-lod fallback checks
static bool tExampleColumnPresent(s32 chunkX, s32 chunkZ, s32 lod)
{
    s32 step = T_EXAMPLE_CHUNK_SIZE << lod;
    f32 yMin, yMax;
    TerrainDensity_GetYRange(&yMin, &yMax);
    s32 chunkYMin = (s32)Floorf32(yMin / (f32)step);
    s32 chunkYMax = (s32)Floorf32(yMax / (f32)step);

    for (s32 y = chunkYMin; y <= chunkYMax; y++)
    {
        int3 min = { chunkX * step, y * step, chunkZ * step };
        u32* found = (u32*)HMFind(&tExample.chunkLookup, tExampleChunkKey(min, lod));
        if (!found || !tExampleChunkPresentable(&tExample.chunks[*found]))
            return false;
    }
    return true;
}

// true when every chunk of the column at this lod is presentable; missing chunks are
// requested through tExampleGetChunk so they build within the per-frame budget
static bool tExampleColumnBuilt(s32 chunkX, s32 chunkZ, s32 lod)
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
        tExampleChunk* chunk = tExampleGetChunk(min, lod);
        if (!tExampleChunkPresentable(chunk))
            built = false;
    }
    return built;
}

// chunked-LOD quadtree: split a node when its closest point lies inside the finer
// LOD's range, otherwise emit it at this lod. every split child is always handled,
// so the selection is gap-free — the old per-ring center-distance test dropped
// chunks whose center fell between two rings. returns false when the submit
// buffer is full and traversal must stop.
static bool tExampleSubmitNode(s32 chunkX, s32 chunkZ, s32 lod, f32 lodFactor, const FrustumPlanes* frustum, bool useFrustum)
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
        f32 drawDistance = tExampleLODDistance[lod] * lodFactor;
        if (distanceSq >= drawDistance * drawDistance)
            return true;
    }

    if (lod > 0)
    {
        f32 splitDistance = tExampleLODDistance[lod - 1] * lodFactor;
        if (distanceSq < splitDistance * splitDistance)
        {
            // descend only when every child column is already built; otherwise keep
            // drawing this coarser node while the children build over the next frames
            // (tExampleColumnBuilt requests the missing builds within the frame budget),
            // so camera motion refines the LOD instead of leaving holes
            bool childrenReady = true;
            for (u32 i = 0; i < 4u; i++)
                childrenReady &= tExampleColumnBuilt(chunkX * 2 + (s32)(i & 1u), chunkZ * 2 + (s32)(i >> 1u), lod - 1);

            if (childrenReady)
            {
                for (u32 i = 0; i < 4u; i++)
                {
                    if (!tExampleSubmitNode(chunkX * 2 + (s32)(i & 1u), chunkZ * 2 + (s32)(i >> 1u), lod - 1, lodFactor, frustum, useFrustum))
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
    if (!tExampleColumnBuilt(chunkX, chunkZ, lod) && lod > 0)
    {
        bool childrenPresent = true;
        for (u32 i = 0; i < 4u; i++)
            childrenPresent &= tExampleColumnPresent(chunkX * 2 + (s32)(i & 1u), chunkZ * 2 + (s32)(i >> 1u), lod - 1);

        if (childrenPresent)
        {
            for (u32 i = 0; i < 4u; i++)
            {
                if (!tExampleSubmitNode(chunkX * 2 + (s32)(i & 1u), chunkZ * 2 + (s32)(i >> 1u), lod - 1, lodFactor, frustum, useFrustum))
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
        if (useFrustum && !tExampleAABBVisible(aabbMin, aabbMax, frustum))
        {
            tExample.culledChunks++;
            continue;
        }

        tExampleChunk* chunk = tExampleGetChunk(min, lod);
        if (!chunk || !chunk->built)
            continue;
        if (!tExampleAppendChunkTriangles(chunk))
            return false;
    }

    return true;
}

static void tExampleSubmitTerrain(f32 lodFactor, const FrustumPlanes* frustum, bool useFrustum)
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
            tExampleChunk* chunk = tExampleGetChunk(min, 0);
            if (!chunk || !chunk->built)
                continue;
            if (!tExampleAppendChunkTriangles(chunk))
                return;
        }
        return;
    }

    const s32 rootLOD = (s32)T_EXAMPLE_LOD_COUNT - 1;
    s32 rootStep = T_EXAMPLE_CHUNK_SIZE << rootLOD;
    f32 drawDistance = tExampleLODDistance[rootLOD] * lodFactor;
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
                if (Maxs32(tExampleAbss32(x), tExampleAbss32(z)) != shell)
                    continue;
                if (!tExampleSubmitNode(centerX + x, centerZ + z, rootLOD, lodFactor, frustum, useFrustum))
                    return;
            }
        }
    }
}

static void tExampleLogStats(void)
{
    u64 now = SDL_GetTicks();
    if (now - tExample.lastStatsTicks < 1000u)
        return;
    tExample.lastStatsTicks = now;
    AX_LOG("transvoxel example: draws=%u chunks=%u built=%u cached=%u culled=%u empty=%u",
           (u32)ArrayLength(tExample.chunkDraws), tExample.submittedChunks, tExample.builtThisFrame,
           tExample.chunkCount, tExample.culledChunks, tExample.emptyChunks);
}

static bool tExampleInit(void)
{
    if (tExample.initialized)
        return true;

    SDL_memset(&tExample, 0, sizeof(tExample));
    tExample.generator.noise3D = tExampleDensityNoise;
    tExample.generator.noise3DStrength = 1.0f;

    size_t densitySize = T_EXAMPLE_CHUNK_SIZE + 3u;
    size_t densityCount = densitySize * densitySize * densitySize;
    tExample.densityData = ArrayCreatePrealloc(f32, densityCount);
    tExample.buildVertices = ArrayCreatePrealloc(ALineVertex, T_EXAMPLE_CHUNK_TRIANGLE_CAP);
    tExample.chunkDraws = ArrayCreatePrealloc(TerrainChunkDraw, MAX_TERRAIN_CHUNK_DRAWS);
    tExample.chunkLookup = HMCreate(T_EXAMPLE_MAX_CHUNKS, sizeof(u32));
    if (!tExample.densityData || !tExample.buildVertices || !tExample.chunkDraws)
    {
        AX_WARN("transvoxel example allocation failed");
        tTransvoxelExampleDestroy();
        return false;
    }

    if (!tMeshDataContainerInit(&tExample.scratchMesh, 8192, 24576, 2048))
    {
        tTransvoxelExampleDestroy();
        return false;
    }

    tExample.initialized = true;
    return true;
}

void tTransvoxelExampleUpdate(void)
{
    if (!tExampleInit())
    {
        RendererSetTerrainChunkDraws(NULL, 0);
        return;
    }

    tExamplePromotePendingMeshes();
    ArrayFieldSet(tExample.chunkDraws, ArrayField_Length, 0);
    tExample.builtThisFrame = 0;
    tExample.submittedChunks = 0;
    tExample.culledChunks = 0;
    tExample.emptyChunks = 0;
    mat4x4 viewProj = M44Multiply(g_Camera.view, g_Camera.projection);
    FrustumPlanes frustum = CreateFrustumPlanesRevZ(viewProj);

    f32 lodFactor = Maxf32(g_RenderSettings.terrainLodFactor, 0.25f);
    const TerrainGenParams* genParams = Terrain_GetGenParams();
    tExample.lodFactor = lodFactor;
    tExample.fixedArea = genParams && genParams->fixedArea;
    tExampleSubmitTerrain(lodFactor, &frustum, true);
    if (ArrayLength(tExample.chunkDraws) == 0)
        tExampleSubmitTerrain(lodFactor, &frustum, false);

    tExampleSyncDirtyPhysics();
    tExampleLogStats();
    RendererSetTerrainChunkDraws(tExample.chunkDraws, (u32)ArrayLength(tExample.chunkDraws));
    RendererSetTerrainBrush(tExample.brushPos, tExample.brushActive ? tExample.brushRadius : 0.0f);
}

void tTransvoxelExampleInvalidateAll(void)
{
    if (tExample.initialized)
        tExampleClearChunkCache();
}

void tTransvoxelExampleInvalidateRegion(float3 mn, float3 mx)
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

void tTransvoxelExampleSetBrushCursor(float3 position, f32 radius, bool active)
{
    tExample.brushPos = position;
    tExample.brushRadius = radius;
    tExample.brushActive = active && radius > 0.0f;
}

void tTransvoxelExampleDestroy(void)
{
    RendererSetTerrainChunkDraws(NULL, 0);
    tExampleClearChunkCache();
    HMDestroy(&tExample.chunkLookup);
    ArrayDestroy(tExample.densityData);
    ArrayDestroy(tExample.buildVertices);
    ArrayDestroy(tExample.chunkDraws);
    tMeshDataContainerDestroy(&tExample.scratchMesh);
    SDL_memset(&tExample, 0, sizeof(tExample));
}
