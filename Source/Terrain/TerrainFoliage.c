#include "Include/RenderSet.h"
#include "Include/FileSystem.h"
#include "Include/Platform.h"
#include "Include/Graphics.h"
#include "Include/JobSystem.h"
#include "Include/TextureSystem.h"
#include "Include/Memory.h"
#include "Include/Scene.h"
#include "Include/Terrain.h"
#include "Include/Random.h"
#include "Source/Terrain/TerrainInternal.h"
#include "Math/Noise.h"
#include "Math/Quaternion.h"
#include "Math/Bitpack.h"
#include <box3d/box3d.h>

// discrete procedural foliage: one entity per placement, placed by worker jobs and
// attached to the chunk that owns it. renders through gFoliage.scene's own render sets
// (not merged into the game scene); colliders attach to the global physics world instead.

#define T_MAX_FOLIAGE_SCENE     64u
#define T_FOLIAGE_MAX_PATH      512
#define T_FOLIAGE_NAME_LEN      64
#define T_FOLIAGE_MAX_PER_CHUNK 192u
#define T_FOLIAGE_MAX_JOBS      4u
#define T_FOLIAGE_SCAN_PER_FRAME     64u
#define T_FOLIAGE_SCHEDULE_PER_FRAME 2u

typedef struct tFoliageType_
{
    char  path[T_FOLIAGE_MAX_PATH];
    char  name[T_FOLIAGE_NAME_LEN]; // display label: file name, no directory/extension
    u32   groupIdx;      // first primitive group in gFoliage.scene.surfaceSet, INVALID_GROUP if unresolved
    u32   groupCount;    // number of primitive groups in the bundle (e.g. trunk+needles as separate
                          // meshes) - every group must get an entity or parts of the model won't render
    f32   localBaseY;    // local-space AABB min.y; shifts placement so the mesh's own base
                          // (not its authored pivot) lands on the ground, whatever the pivot is
    b3HullData* hull;    // native-scale convex hull, built lazily on first collider request
    tFoliageParams params;
} tFoliageType;

extern Graphics gGFX; // cpu mega buffers, declared per translation unit as elsewhere (BVH.c, Physics.c)

typedef struct tFoliagePlacement_
{
    float3 localPos; // chunk-relative meters
    f32    scale;     // final uniform scale (size * variance), pre-multiplied on the worker
    u32    typeIndex;
} tFoliagePlacement;

typedef struct tFoliageJob_
{
    int3  chunkMin;
    u32   generationSnapshot;
    s8    density[T_SAMPLES_TOTAL]; // chunk's own 19^3 grid, sampled once per job so every
                                     // candidate resolves Y via a cheap lookup, not analytic SDF calls
    tFoliagePlacement placements[T_FOLIAGE_MAX_PER_CHUNK];
    u32   count;
    JobHandle handle;
    bool  busy;
} tFoliageJob;

typedef struct tFoliageState_
{
    Scene scene;
    u32   numTypes;
    tFoliageType types[T_MAX_FOLIAGE_SCENE];
    tFoliageJob  jobs[T_FOLIAGE_MAX_JOBS];
    u32   worldGeneration; // bumped by tFoliage_SetParams, chunks rebuild when they fall behind
    u32   scanCursor;      // round-robin index into the resident chunk array
} tFoliageState;

static tFoliageState gFoliage;

static void VisitFile(const char* path, void* data)
{
    (void)data;
    int pathLen = StringLength(path);
    u8 isMesh = FileHasExtension(path, pathLen, ".fbx") || FileHasExtension(path, pathLen, ".gltf") ||
                FileHasExtension(path, pathLen, ".obj") || FileHasExtension(path, pathLen, ".glb");
    if (!isMesh) return;

    if (gFoliage.numTypes >= T_MAX_FOLIAGE_SCENE || pathLen <= 0 || pathLen >= T_FOLIAGE_MAX_PATH) {
        AX_WARN("foliage path invalid");
        return;
    }

    tFoliageType* type = &gFoliage.types[gFoliage.numTypes++];
    MemSet(type, 0, sizeof(*type));
    NormalizePath(path, type->path, T_FOLIAGE_MAX_PATH);
    GetFileNameNoExt(type->path, type->name);
    type->groupIdx = INVALID_GROUP;
    type->params = (tFoliageParams){
        .density = 4.0f,
        .rarity  = 0.5f,
        .size    = 1.0f,
        .enabled = false, // off by default until placement/visuals are verified per type
        .collider = true,
        .sizeVariance = false
    };
}

void tFoliage_Init()
{
    MemSet(&gFoliage, 0, sizeof(gFoliage));
    Scene_Init(&gFoliage.scene);
    if (VisitFolder("Assets/Foliage", VisitFile, NULL, true) == 0) {
        AX_WARN("foliage file traverse failed!");
        return;
    }

    for (u32 i = 0; i < gFoliage.numTypes; i++)
    {
        tFoliageType* type = &gFoliage.types[i];
        AX_LOG("foliage: %s", type->path);
        u32 bundleIdx = Scene_AddBundle(&gFoliage.scene, type->path, false);
        if (bundleIdx == INVALID_BUNDLE) continue;

        Range range = gFoliage.scene.surfaceSet.bundlePrimRange[bundleIdx];
        if (range.count == 0u) continue;

        type->groupIdx = range.start;
        type->groupCount = range.count;
        // ground-snap must use the lowest point across every primitive group in the bundle
        // (needles/trunk/etc are separate groups) - using only the first group's aabbMin
        // buries any group whose local origin sits lower than that first group's base.
        f32 minY = VecGetY(gFoliage.scene.surfaceSet.primitiveGroups[range.start].aabbMin);
        for (u32 g = range.start + 1; g < range.start + range.count; g++)
        {
            f32 groupMinY = VecGetY(gFoliage.scene.surfaceSet.primitiveGroups[g].aabbMin);
            if (groupMinY < minY) minY = groupMinY;
        }
        type->localBaseY = minY;
    }

    gFoliage.worldGeneration = 1u;
}

void tFoliage_Destroy()
{
    for (u32 i = 0; i < gFoliage.numTypes; i++)
        if (gFoliage.types[i].hull) b3DestroyHull(gFoliage.types[i].hull);
    Scene_Destroy(&gFoliage.scene);
    MemSet(&gFoliage, 0, sizeof(gFoliage));
}

Scene* tFoliage_GetScene(void) { return &gFoliage.scene; }

u32 tFoliage_NumTypes(void) { return gFoliage.numTypes; }

const char* tFoliage_TypeName(u32 index)
{
    return index < gFoliage.numTypes ? gFoliage.types[index].name : "";
}

bool tFoliage_GetParams(u32 index, tFoliageParams* out)
{
    if (!out || index >= gFoliage.numTypes) return false;
    *out = gFoliage.types[index].params;
    return true;
}

void tFoliage_SetParams(u32 index, const tFoliageParams* params)
{
    if (!params || index >= gFoliage.numTypes) return;
    gFoliage.types[index].params = *params;
    gFoliage.worldGeneration++; // stales every chunk's foliage; tFoliage_Update rebuilds them, terrain mesh untouched
}

// inverse of TerrainDensity_Quantize (>0 air, <0 solid). NOT T_DENSITY_DECODE_SCALE -
// that's negated on purpose for MarchingMesher's own flipped internal convention
#define T_FOLIAGE_DENSITY_DECODE_SCALE (TERRAIN_SDF_CLAMP / 127.0f)

// trilinear sample of the cached 19^3 density grid at a chunk-local point (0..T_CHUNK_CELLS).
// GetDensityFromTerrainPoint, cheap lookup instead of an analytic field eval. sample
// (ix,iy,iz) = world corner (ix-1,iy-1,iz-1), same layout tMesherDensityAt reads
static f32 tFoliageSampleDensity(const s8* density, float3 localPos)
{
    f32 px = Clampf32(localPos.x, 0.0f, (f32)T_CHUNK_CELLS);
    f32 py = Clampf32(localPos.y, 0.0f, (f32)T_CHUNK_CELLS);
    f32 pz = Clampf32(localPos.z, 0.0f, (f32)T_CHUNK_CELLS);

    f32 fx = Floorf32(px), fy = Floorf32(py), fz = Floorf32(pz);
    s32 x0 = (s32)fx + 1, x1 = x0 + 1;
    s32 y0 = (s32)fy + 1, y1 = y0 + 1;
    s32 z0 = (s32)fz + 1, z1 = z0 + 1;
    f32 xd = px - fx, yd = py - fy, zd = pz - fz;

    const s32 axis = T_SAMPLES_AXIS;
    // TerrainDensity_SampleChunk writes [z][y][x]; mesher transposes its copy before use.
    #define TFOL_AT(ix, iy, iz) ((f32)density[(iz) * axis * axis + (iy) * axis + (ix)] * T_FOLIAGE_DENSITY_DECODE_SCALE)
    f32 c000 = TFOL_AT(x0, y0, z0), c100 = TFOL_AT(x1, y0, z0);
    f32 c010 = TFOL_AT(x0, y1, z0), c110 = TFOL_AT(x1, y1, z0);
    f32 c001 = TFOL_AT(x0, y0, z1), c101 = TFOL_AT(x1, y0, z1);
    f32 c011 = TFOL_AT(x0, y1, z1), c111 = TFOL_AT(x1, y1, z1);
    #undef TFOL_AT

    f32 c00 = c000 * (1.0f - xd) + c100 * xd;
    f32 c10 = c010 * (1.0f - xd) + c110 * xd;
    f32 c01 = c001 * (1.0f - xd) + c101 * xd;
    f32 c11 = c011 * (1.0f - xd) + c111 * xd;
    f32 c0 = c00 * (1.0f - yd) + c10 * yd;
    f32 c1 = c01 * (1.0f - yd) + c11 * yd;
    return c0 * (1.0f - zd) + c1 * zd;
}

// GetNearestSurfaceY, chunk-local: march down from the chunk's own ceiling to the first
// air/solid crossing, top-down only. terrain streams a full vertical stack of chunks per
// column (surface/underground/bedrock); starting mid-chunk and searching either direction
// (the original two-way GetNearestSurfaceY) can latch onto a cave ceiling or overhang
// underside in a chunk that has nothing to do with the walkable surface - that's what made
// foliage float. requiring "start in air, first solid going down" only ever accepts the
// topmost surface exposed to open sky in this chunk; an already-solid ceiling means this
// chunk is buried under more terrain above, rejected outright. also rejects thin/floating
// shells and steep slopes (cliff edges), the same safety margin TerrainGrass already uses.
// out: false when nothing placeable was found
static bool tFoliageFindSurfaceY(const s8* density, f32 x, f32 z, f32* outY)
{
    const f32 step = 0.5f, fineStep = 0.1f, maxY = (f32)T_CHUNK_CELLS;
    float3 p = { x, maxY, z };
    f32 d = tFoliageSampleDensity(density, p);
    if (d < 0.0f) return false; // ceiling already solid: buried, real surface is above

    f32 y = maxY;
    while (d >= 0.0f)
    {
        y -= step;
        if (y < 0.0f) return false; // no ground here, real surface is in a lower chunk
        p.y = y;
        d = tFoliageSampleDensity(density, p);
    }
    while (d < 0.0f && y < maxY) { y += fineStep; p.y = y; d = tFoliageSampleDensity(density, p); }
    y = Clampf32(y - fineStep * 0.5f, 0.0f, maxY);

    if (tFoliageSampleDensity(density, (float3){ x, y - 0.75f, z }) >= 0.0f) return false; // thin/floating shell

    const f32 e = 0.5f;
    float3 grad = {
        tFoliageSampleDensity(density, (float3){ x + e, y, z }) - tFoliageSampleDensity(density, (float3){ x - e, y, z }),
        tFoliageSampleDensity(density, (float3){ x, y + e, z }) - tFoliageSampleDensity(density, (float3){ x, y - e, z }),
        tFoliageSampleDensity(density, (float3){ x, y, z + e }) - tFoliageSampleDensity(density, (float3){ x, y, z - e })
    };
    if (F3NormSafe(grad).y < 0.6f) return false; // too steep, mirrors grass's slope cutoff

    *outY = y;
    return true;
}

// worker thread: pure placement, touches only this job slot's output and the read-only
// foliage type table. no RenderSet/physics access happens here (main-thread only)
static void RunFoliageJob(void* userData)
{
    tFoliageJob* job = (tFoliageJob*)userData;
    job->count = 0u;
    f32 chunkSize = (f32)T_CHUNK_CELLS;

    s32 cx = FloorDiv(job->chunkMin.x, T_CHUNK_CELLS);
    s32 cy = FloorDiv(job->chunkMin.y, T_CHUNK_CELLS);
    s32 cz = FloorDiv(job->chunkMin.z, T_CHUNK_CELLS);
    TerrainDensity_SampleChunk(cx, cy, cz, job->density);

    for (u32 t = 0; t < gFoliage.numTypes && job->count < T_FOLIAGE_MAX_PER_CHUNK; t++)
    {
        const tFoliageType* type = &gFoliage.types[t];
        if (!type->params.enabled || type->groupIdx == INVALID_GROUP) continue;

        f32 density = Maxf32(type->params.density, 0.5f);
        f32 rarityGate = Clampf32(1.0f - type->params.rarity, 0.05f, 1.0f) * 1.5f;
        u32 seed = WangHash((u32)job->chunkMin.x * 73856093u ^ (u32)job->chunkMin.z * 19349663u ^ (t * 83492791u + 1u));

        for (f32 x = density * 0.5f; x < chunkSize; x += density)
        {
            for (f32 z = density * 0.5f; z < chunkSize; z += density)
            {
                if (job->count >= T_FOLIAGE_MAX_PER_CHUNK) return;

                f32 worldX = (f32)job->chunkMin.x + x;
                f32 worldZ = (f32)job->chunkMin.z + z;

                float2 cell = NoiseCellular2D((float2){ worldX * 0.08f, worldZ * 0.08f });
                if (cell.x > rarityGate) continue;
                if (TerrainDensity_IslandMask(worldX, worldZ) > 0.02f) continue;

                f32 jitterX = (NextFloat01(PCG2Next(&seed)) * 2.0f - 1.0f) * density * 0.3333333f;
                f32 jitterZ = (NextFloat01(PCG2Next(&seed)) * 2.0f - 1.0f) * density * 0.3333333f;
                f32 localX = Clampf32(x + jitterX, 0.0f, chunkSize);
                f32 localZ = Clampf32(z + jitterZ, 0.0f, chunkSize);

                f32 localY;
                if (!tFoliageFindSurfaceY(job->density, localX, localZ, &localY)) continue;

                f32 scale = type->params.sizeVariance ? RepeatMinMaxF32(PCG2Next(&seed), 0.7f, 1.3f) : 1.0f;

                tFoliagePlacement* placement = &job->placements[job->count++];
                placement->localPos = (float3){ localX, localY, localZ };
                placement->scale = scale;
                placement->typeIndex = t;
            }
        }
    }
}

static void ScheduleFoliageJob(tChunk* chunk, int3 chunkMin)
{
    JobSystem* js = tGetTerrainJobSystem();
    if (!js) return;

    tFoliageJob* job = NULL;
    for (u32 i = 0; i < T_FOLIAGE_MAX_JOBS; i++)
        if (!gFoliage.jobs[i].busy) { job = &gFoliage.jobs[i]; break; }
    if (!job) return;

    job->chunkMin = chunkMin;
    job->generationSnapshot = gFoliage.worldGeneration;
    job->count = 0u;
    job->busy = true;
    job->handle = JobSystem_Execute(js, RunFoliageJob, job);
    if (job->handle == 0)
    {
        job->busy = false;
        return;
    }
    chunk->foliagePending = true;
}

#define T_FOLIAGE_HULL_MAX_VERTS 64

// builds and caches the type's native-scale convex hull from its primitive's vertex cloud.
// per-instance colliders clone+scale it via b3CreateTransformedHullShape.
static b3HullData* EnsureFoliageTypeHull(tFoliageType* type)
{
    if (type->hull) return type->hull;
    if (type->groupIdx == INVALID_GROUP) return NULL;

    RenderSet* set = &gFoliage.scene.surfaceSet;
    if (type->groupIdx >= set->numGroups) return NULL;
    PrimitiveGroup* group = &set->primitiveGroups[type->groupIdx];
    u32 numVertices = group->lodNumVertices[0];
    if (numVertices < 4u) return NULL;
    if (group->bundleIdx >= gFoliage.scene.numBundles || !gFoliage.scene.bundleRefs[group->bundleIdx].bundle)
        return NULL;

    const SceneBundle* bundle = gFoliage.scene.bundleRefs[group->bundleIdx].bundle;
    const APrimitive* prim = &bundle->meshes[group->meshIndex].primitives[group->primitiveIndex];
    u32 vertexOffset = group->lodVertexOffset[0];
    v128f decodeMin = VecLoad(prim->min);
    v128f decodeExtent = VecMax(VecSub(VecLoad(prim->max), decodeMin), VecSet1(1.0e-6f));

    b3Vec3* points = (b3Vec3*)SDL_malloc(sizeof(b3Vec3) * numVertices);
    if (!points) return NULL;

    const AVertex* vb = gGFX.SurfaceVertexBuffer + vertexOffset;
    for (u32 v = 0; v < numVertices; v++)
        points[v] = ToB3Vec3(VecAdd(decodeMin, VecMul(UnpackUnorm16x4(vb[v].position), decodeExtent)));

    type->hull = b3CreateHull(points, (int)numVertices, T_FOLIAGE_HULL_MAX_VERTS);
    SDL_free(points);
    return type->hull;
}

static u64 CreateFoliageCollider(tFoliageType* type, float3 worldPos, f32 scale)
{
    // global world, not scene-owned, so this still collides with the gameplay scene
    b3WorldId world = Physics_GetWorld();
    if (B3_IS_NULL(world)) return 0u;

    b3HullData* hull = EnsureFoliageTypeHull(type);
    if (!hull) return 0u;

    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_staticBody;
    bd.position = (b3Vec3){ worldPos.x, worldPos.y, worldPos.z };
    b3BodyId body = b3CreateBody(world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    b3Vec3 scaleVec = { scale, scale, scale };
    b3ShapeId shape = b3CreateTransformedHullShape(body, &sd, hull, b3Transform_identity, scaleVec);
    if (B3_IS_NULL(shape)) {
        b3DestroyBody(body);
        return 0u;
    }
    return b3StoreBodyId(body);
}

static void DestroyFoliageCollider(u64 stored)
{
    if (stored == 0u) return;
    b3BodyId body = b3LoadBodyId(stored);
    if (B3_IS_NON_NULL(body)) b3DestroyBody(body);
}

void tFoliage_DestroyChunkFoliage(tChunk* chunk)
{
    if (!chunk) return;
    if (!chunk->foliageEntities) { chunk->foliageCount = 0u; return; }

    RenderSet* set = &gFoliage.scene.surfaceSet;
    for (u32 i = 0; i < chunk->foliageCount; i++)
    {
        tFoliageEntity* fe = &chunk->foliageEntities[i];
        DestroyFoliageCollider(fe->physicsBody);

        if (fe->sparseIdx >= set->maxEntities) continue;
        u32 denseIdx = set->sparseID[fe->sparseIdx];
        if (denseIdx == INVALID_ENTITY) continue;
        u32 groupIdx = fe->packed >> 3;
        if (groupIdx >= set->numGroups) continue;
        u32 localIdx = denseIdx - set->primitiveGroups[groupIdx].entityOffset;
        RenderSet_RemoveEntity(set, groupIdx, localIdx);
    }

    DeAllocateTLSFGlobal(chunk->foliageEntities);
    chunk->foliageEntities = NULL;
    chunk->foliageCount = 0u;
}

static void IntegrateFinishedFoliage(JobSystem* js)
{
    for (u32 i = 0; i < T_FOLIAGE_MAX_JOBS; i++)
    {
        tFoliageJob* job = &gFoliage.jobs[i];
        if (!job->busy || !JobSystem_IsJobDone(js, job->handle)) continue;
        job->busy = false;

        // the chunk may have been evicted (or reused for a different location) while
        // this job was in flight; re-resolve by position and drop stale results
        tChunk* chunk = tFindChunkByMin(job->chunkMin);
        if (!chunk) continue;
        chunk->foliagePending = false;

        tFoliage_DestroyChunkFoliage(chunk);

        if (job->count == 0u) {
            chunk->foliageBuiltGen = job->generationSnapshot;
            continue;
        }

        // a placement's bundle can have several primitive groups (e.g. trunk + needles as
        // separate meshes) - every group needs its own entity or those parts never render,
        // so the entity array must fit one slot per (placement, group) pair, not per placement.
        u32 maxEntities = 0u;
        for (u32 p = 0; p < job->count; p++)
        {
            tFoliageType* type = &gFoliage.types[job->placements[p].typeIndex];
            if (type->groupIdx == INVALID_GROUP) continue;
            maxEntities += type->groupCount;
        }

        tFoliageEntity* entities = (tFoliageEntity*)AllocateTLSFGlobal(sizeof(tFoliageEntity) * maxEntities);
        if (!entities) {
            chunk->foliageBuiltGen = job->generationSnapshot;
            continue;
        }

        RenderSet* set = &gFoliage.scene.surfaceSet;
        u32 written = 0u;
        for (u32 p = 0; p < job->count; p++)
        {
            const tFoliagePlacement* placement = &job->placements[p];
            tFoliageType* type = &gFoliage.types[placement->typeIndex];
            if (type->groupIdx == INVALID_GROUP) continue;

            float3 worldPos = F3Add(ToFloat3(job->chunkMin), placement->localPos);
            f32 finalScale = type->params.size * placement->scale;
            worldPos.y -= type->localBaseY * finalScale; // mesh's own base -> ground, not its authored pivot
            f32 yaw = NextFloat01(WangHash(placement->typeIndex * 2654435761u + p)) * 2.0f * MATH_PI;
            u64 physicsBody = type->params.collider ? CreateFoliageCollider(type, worldPos, finalScale) : 0u;

            bool placementFailed = false;
            bool bodyStored = false;
            for (u32 g = 0; g < type->groupCount; g++)
            {
                u32 sparseIdx = RenderSet_AllocateSparseID(set);
                if (sparseIdx == INVALID_ENTITY) { placementFailed = true; break; }

                Entity e = {0};
                e.position = VecSetR(worldPos.x, worldPos.y, worldPos.z, 0.0f);
                e.rotation = PackQuaternionS16NormRet(QFromAxisAngle(F3Up(), yaw));
                e.scale = EntityPackUniformWorldScale(finalScale);
                e.sparseIdx = sparseIdx;

                u32 groupIdx = type->groupIdx + g;
                if (RenderSet_AddEntity(set, groupIdx, &e) == INVALID_ENTITY)
                {
                    RenderSet_FreeSparseID(set, sparseIdx);
                    placementFailed = true;
                    break;
                }

                // collider follows only the first group's entity so it's destroyed exactly once
                u64 bodyForThisEntity = !bodyStored ? physicsBody : 0u;
                bodyStored = true;
                entities[written++] = (tFoliageEntity){ sparseIdx, (groupIdx << 3), bodyForThisEntity };
            }
            // only free the collider here if no entity ended up owning it - once an entity
            // holds it, tFoliage_DestroyChunkFoliage is the sole owner and will free it
            if (placementFailed && !bodyStored) {
                DestroyFoliageCollider(physicsBody);
                break;
            }
            if (placementFailed) break;
        }

        if (written == 0u) {
            DeAllocateTLSFGlobal(entities);
            chunk->foliageEntities = NULL;
            chunk->foliageCount = 0u;
        }
        else {
            chunk->foliageEntities = entities;
            chunk->foliageCount = (u16)written;
        }
        chunk->foliageBuiltGen = job->generationSnapshot;
    }
}

void tFoliage_Update(void)
{
    JobSystem* js = tGetTerrainJobSystem();
    if (!js) return;

    IntegrateFinishedFoliage(js);

    u32 chunkCount = tGetChunkCount();
    if (chunkCount == 0u) return;

    u32 scheduled = 0u;
    u32 scanned = 0u;
    while (scanned < chunkCount && scanned < T_FOLIAGE_SCAN_PER_FRAME && scheduled < T_FOLIAGE_SCHEDULE_PER_FRAME)
    {
        if (gFoliage.scanCursor >= chunkCount) gFoliage.scanCursor = 0u;
        tChunk* chunk = tGetChunkByIndex(gFoliage.scanCursor++);
        scanned++;
        if (!chunk || chunk->buildState != CHUNK_READY) continue;
        if (!chunk->mesh.vertices.heapPtr)
        {
            // no surface in this chunk, nothing to place; skip until params change again
            chunk->foliageBuiltGen = gFoliage.worldGeneration;
            continue;
        }
        if (chunk->foliagePending || chunk->foliageBuiltGen == gFoliage.worldGeneration) continue;

        ScheduleFoliageJob(chunk, chunk->min);
        scheduled++;
    }
}

void tFoliage_DrainJobs(void)
{
    for (u32 i = 0; i < T_FOLIAGE_MAX_JOBS; i++)
        gFoliage.jobs[i].busy = false;
}
