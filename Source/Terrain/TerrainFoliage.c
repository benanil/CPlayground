#include "Include/RenderSet.h"
#include "Include/FileSystem.h"
#include "Include/Platform.h"
#include "Include/Graphics.h"
#include "Include/JobSystem.h"
#include "Include/ParallelFor.h"
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
#define T_FOLIAGE_GROUND_SNAP_OFFSET 0.1f

typedef struct tFoliageType_
{
    char  path[T_FOLIAGE_MAX_PATH];
    char  name[T_FOLIAGE_NAME_LEN]; // display label: file name, no directory/extension
    u32   groupIdx;      // first primitive group in gFoliage.scene.surfaceSet, INVALID_GROUP if unresolved
    u32   groupCount;    // number of primitive groups in the bundle (e.g. trunk+needles as separate
                         // meshes) - every group must get an entity or parts of the model won't render
    f32   localBaseY;    // lowest local-space point across all primitive groups
    f32   normalAlign;   // terrain-normal tilt amount; slender meshes stay mostly upright
    b3HullData* hull;    // native-scale convex hull, built lazily on first collider request
    tFoliageParams params;
    bool  paramsDirty;   // set by tFoliage_SetParams; only this type's chunks get torn down
                         // and rebuilt on the next tFoliage_Update, every other type's
                         // instances stay untouched in the RenderSet
} tFoliageType;

extern Graphics gGFX; // cpu mega buffers, declared per translation unit as elsewhere (BVH.c, Physics.c)

typedef struct tFoliagePlacement_
{
    u64 localPos;    // Pack16x4Fixed chunk-relative position
    u64 rotation;    // packed final terrain-aligned rotation
    f32 scale;       // final uniform scale (size * variance), pre-multiplied on the worker
    u32 typeIndex;
} tFoliagePlacement;

typedef struct tFoliageJob_
{
    int3        chunkMin;
    const s8*   density;
    // true for a chunk that has never had foliage decided (see tChunk.foliageBuilt): place
    // every currently-enabled type for it, not just the ones flagged paramsDirty this round.
    bool        placeAllEnabled;
    u32         count;
    tFoliagePlacement placements[T_FOLIAGE_MAX_PER_CHUNK];
} tFoliageJob;

// A resolved placement awaiting batching into RenderSet_AddEntities.
// Transform is computed once per placement; physicsBody attaches only to render group 0.
typedef struct tFoliageBatchItem_
{
    tChunk* chunk;
    v128f   position;
    u64     rotation;
    u64     scale;
} tFoliageBatchItem;

typedef struct tFoliageState_
{
    Scene scene;
    u32   numTypes;
    PCG   pcg;
    tFoliageType types[T_MAX_FOLIAGE_SCENE];
    tFoliageJob  jobs[T_MAX_CHUNKS]; // one slot per resident chunk, all dispatched and
                                     // awaited within a single tFoliage_Update call
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
        .density = RepeatMinMaxF32(PCGNext(&gFoliage.pcg), 7.5f, 10.0),
        .rarity  = RepeatMinMaxF32(PCGNext(&gFoliage.pcg), 0.88f, 0.95),
        .size    = 1.0f,
        .enabled = true, // off by default until placement/visuals are verified per type
        .collider = false,
        .sizeVariance = true
    };
}

static void FoliageStageRange(u32 begin, u32 end, void* userData)
{
    SceneBundleStage* stages = (SceneBundleStage*)userData;
	for (u32 i = begin; i < end; i++) 
	{
		stages[i].storedPath = gFoliage.types[i].path;
		stages[i].skinned    = false;
        Scene_AddBundleStage(&stages[i]);
	}
}

void tFoliage_Init()
{
    MemSet(&gFoliage, 0, sizeof(gFoliage));
	gFoliage.pcg = (PCG){ 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL };
	Scene_Init(&gFoliage.scene);
    if (VisitFolder("Assets/Foliage", VisitFile, NULL, true) == 0) {
        AX_WARN("foliage file traverse failed!");
        return;
    }
    if (gFoliage.numTypes == 0) return;

    // Stage every foliage bundle's mesh+texture load across worker threads via ParallelFor.
    // Finalize (RenderSet/TextureSystem/AnimationSystem mutation) still runs serially on the
    // main thread afterwards, same as the rest of Scene_Init's callers.
	SceneBundleStage* stages = (SceneBundleStage*)AllocateTLSFGlobal(T_MAX_FOLIAGE_SCENE * sizeof(SceneBundleStage));
    ParallelFor(gFoliage.numTypes, 1u, FoliageStageRange, stages);

    for (u32 i = 0; i < gFoliage.numTypes; i++)
    {
        tFoliageType* type = &gFoliage.types[i];
        AX_LOG("foliage: %s", type->path);
        u32 bundleIdx = Scene_AddBundleFinalize(&gFoliage.scene, &stages[i]);
        if (bundleIdx == INVALID_BUNDLE) continue;

        Range range = gFoliage.scene.surfaceSet.bundlePrimRange[bundleIdx];
        if (range.count == 0u) continue;

        type->groupIdx = range.start;
        type->groupCount = range.count;
        PrimitiveGroup* firstGroup = &gFoliage.scene.surfaceSet.primitiveGroups[range.start];
        v128f localBoundsMin = firstGroup->aabbMin;
        v128f localBoundsMax = firstGroup->aabbMax;
        for (u32 g = range.start + 1; g < range.start + range.count; g++)
        {
            PrimitiveGroup* group = &gFoliage.scene.surfaceSet.primitiveGroups[g];
            localBoundsMin = VecMin(localBoundsMin, group->aabbMin);
            localBoundsMax = VecMax(localBoundsMax, group->aabbMax);
        }
        type->localBaseY = VecGetY(localBoundsMin);
        f32 height = VecGetY(localBoundsMax) - VecGetY(localBoundsMin);
        f32 widthX = VecGetX(localBoundsMax) - VecGetX(localBoundsMin);
        f32 widthZ = VecGetZ(localBoundsMax) - VecGetZ(localBoundsMin);
        f32 horizontal = Sqrtf(widthX * widthX + widthZ * widthZ);
        f32 aspect = height / Maxf32(horizontal, 1.0e-3f);
        f32 tallness = Maxf32(aspect, 1.0f);
        type->normalAlign = Clampf32(1.0f / (tallness * tallness * tallness), 0.03f, 0.75f);
        type->paramsDirty = true; // build this type's foliage on the first update
    }
	DeAllocateTLSFGlobal(stages);
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
    return index < gFoliage.numTypes ? gFoliage.types[index].name : "noname-prop";
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
    gFoliage.types[index].paramsDirty = true; // only this type rebuilds next tFoliage_Update
}

void tFoliage_RandomizeParams(void)
{
    for (u32 i = 0; i < gFoliage.numTypes; i++)
    {
        tFoliageType* type = &gFoliage.types[i];
        type->params.density = RepeatMinMaxF32(PCGNext(&gFoliage.pcg), 3.5f, 8.0f);
        type->params.rarity = RepeatMinMaxF32(PCGNext(&gFoliage.pcg), 0.85f, 0.95f);
        type->paramsDirty = true;
    }
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
    // reads tChunk.density directly (see MarchingTerrain.c's tBuildDensity / IntegrateFinishedBuilds),
    // which is already transposed to [x][y][z] for the mesher - no separate raw copy exists anymore.
    #define TFOL_AT(ix, iy, iz) ((f32)density[(ix) * axis * axis + (iy) * axis + (iz)] * T_FOLIAGE_DENSITY_DECODE_SCALE)
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

// GetNearestSurfaceY: March top-down for the first air-to-solid crossing.
// Prevents latching onto cave ceilings/overhangs (fixing floating foliage).
// Rejects buried chunks, thin shells, and steep slopes.
// Out: false if no valid surface found.
static bool tFoliageFindSurfaceY(const s8* density, f32 x, f32 z, f32* outY, float3* outNormal)
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
    while (d < 0.0f && y < maxY) 
	{
		y += fineStep;
		p.y = y;
		d = tFoliageSampleDensity(density, p);
	}
    y = Clampf32(y - fineStep * 0.5f, 0.0f, maxY);

    if (tFoliageSampleDensity(density, (float3){ x, y - 0.75f, z }) >= 0.0f) return false; // thin/floating shell

    const f32 e = 0.5f;
    float3 grad = {
        tFoliageSampleDensity(density, (float3){ x + e, y, z }) - tFoliageSampleDensity(density, (float3){ x - e, y, z }),
        tFoliageSampleDensity(density, (float3){ x, y + e, z }) - tFoliageSampleDensity(density, (float3){ x, y - e, z }),
        tFoliageSampleDensity(density, (float3){ x, y, z + e }) - tFoliageSampleDensity(density, (float3){ x, y, z - e })
    };
    float3 normal = F3NormSafe(grad);
    if (normal.y < 0.6f) return false; // too steep, mirrors grass's slope cutoff

    *outY = y;
    *outNormal = normal;
    return true;
}

// fills rnd[0..count) with a chained WangHashx4 stream, 4 lanes/store. count must be a
// multiple of 4 (noiseAxis is forced to one, and noiseAxis*noiseAxis*4 stays one too).
static void GetRandom(v128u seed, s32 count, s32* rnd)
{
	for (s32 i = 0; i < count; i += 4) {
		seed = WangHashx4(seed);
		VecStoreI(rnd + i, seed);
	}
}
// Fills a noise grid of NoiseCellular2D results (one per row) matching the placement loop's steps.
// Uses SIMD where worldX is broadcast and worldZ varies across consecutive lanes.
static void GetCellular(s32 chunkX, s32 chunkZ, f32 density, s32 axis, f32 frequency,
                        f32 offsetX, f32 offsetZ, f32* cx, f32* cz)
{
	const v128f laneOffset = VecSetR(0.5f, 1.5f, 2.5f, 3.5f); // j+0.5 .. j+3.5

	for (s32 i = 0; i < axis; i++)
	{
		v128f worldX = VecAddf(VecSet1(((f32)chunkX + density * ((f32)i + 0.5f)) * frequency), offsetX);

		for (s32 j = 0; j < axis; j += 4)
		{
			v128f jIdx = VecAddf(laneOffset, (f32)j);                    // j+0.5 .. j+3.5
			v128f worldZ = VecAddf(VecMulf(VecAddf(VecMulf(jIdx, density), (f32)chunkZ), frequency), offsetZ);

			v128f rx, ry;
			NoiseCellular2Dx4(worldX, worldZ, &rx, &ry);
			VecStore(cx + i * axis + j, rx);
			VecStore(cz + i * axis + j, ry);
		}
	}
}

static Quaternion CreateFoliageRotation(float3 normal, f32 yaw, f32 normalAlign)
{
    float3 up = F3Up();
    float3 axis = F3Cross(&up, &normal);
    f32 upDot = Clampf32(F3Dot(up, normal), -1.0f, 1.0f);
    f32 axisLength = F3Dot(axis, axis);
    Quaternion tilt = QIdentity();
    if (axisLength > 1.0e-6f)
        tilt = QFromAxisAngle(F3NormSafe(axis), Minf32(ACos(upDot) * normalAlign, MATH_PI * 0.25f));

    Quaternion aroundUp = QFromAxisAngle(up, yaw);
    return QNormEst(QMul(aroundUp, tilt));
}

#define T_MIN_DENSITY 0.25f
#define T_NOISE_AXIS_MAX (int)(T_CHUNK_CELLS * (1.0f / T_MIN_DENSITY)) 
// worker thread: pure placement, touches only this job slot's output and the read-only
// foliage type table. no RenderSet/physics access happens here (main-thread only)
static void RunFoliageJob(void* userData)
{
    tFoliageJob* job = (tFoliageJob*)userData;
    job->count = 0u;

	ALIGNSIMD s32 rnd[T_NOISE_AXIS_MAX * T_NOISE_AXIS_MAX * 4];
	ALIGNSIMD f32 cx[T_NOISE_AXIS_MAX * T_NOISE_AXIS_MAX];
	ALIGNSIMD f32 cz[T_NOISE_AXIS_MAX * T_NOISE_AXIS_MAX];

    for (u32 t = 0; t < gFoliage.numTypes && job->count < T_FOLIAGE_MAX_PER_CHUNK; t++)
    {
        const tFoliageType* type = &gFoliage.types[t];
        // normally only types whose params just changed get (re)placed - every other
        // type's chunks are left completely alone, both here and in IntegrateFinishedFoliage.
        // a chunk that has never had foliage decided (placeAllEnabled) is the exception:
        // every enabled type gets placed for it once, dirty or not.
        if (!type->params.enabled || type->groupIdx == INVALID_GROUP) continue;
        if (!job->placeAllEnabled && !type->paramsDirty) continue;

        f32 density = Maxf32(type->params.density, T_MIN_DENSITY);
        f32 rarityGate = Clampf32(1.0f - type->params.rarity, 0.05f, 1.0f) * 1.5f;
        u32 seed = WangHash((u32)job->chunkMin.x * 73856093u ^ (u32)job->chunkMin.z * 19349663u ^ (t * 83492791u + 1u));
		v128u s4 = VeciSet(seed, seed + 234097, seed + 12345, seed + 314159265);
		u32 typeNoiseSeed = WangHash((t + 1u) * 0x9e3779b9u);
		f32 noiseFrequency = RepeatMinMaxF32(typeNoiseSeed, 0.065f, 0.095f);
		f32 noiseOffsetX = RepeatMinMaxF32(WangHash(typeNoiseSeed ^ 0xa511e9b3u), 0.0f, 289.0f);
		f32 noiseOffsetZ = RepeatMinMaxF32(WangHash(typeNoiseSeed ^ 0x63d83595u), 0.0f, 289.0f);

        s32 noiseAxis = (s32)((f32)T_CHUNK_CELLS / density);
        noiseAxis = (noiseAxis + 3) & ~3;
		noiseAxis = Clamps32(noiseAxis, 4, T_NOISE_AXIS_MAX);

		GetRandom(s4, noiseAxis * noiseAxis * 4, rnd);
		GetCellular(job->chunkMin.x, job->chunkMin.z, density, noiseAxis, noiseFrequency,
		             noiseOffsetX, noiseOffsetZ, cx, cz);
		s32 rId = 0, ix = 0;
        for (f32 x = density * 0.5f; x < T_CHUNK_CELLS; x += density, ix++)
        {
			s32 iz = 0;
            for (f32 z = density * 0.5f; z < T_CHUNK_CELLS; z += density, iz++)
            {
                if (job->count >= T_FOLIAGE_MAX_PER_CHUNK) return;

                f32 worldX = (f32)job->chunkMin.x + x;
                f32 worldZ = (f32)job->chunkMin.z + z;

				float2 cell = { cx[ix * noiseAxis + iz], cz[ix * noiseAxis + iz] }; // NoiseCellular2D((float2) { worldX * 0.08f, worldZ * 0.08f });
                if (cell.x > rarityGate) continue;
                if (TerrainDensity_IslandMask(worldX, worldZ) > 0.02f) continue;

				f32 jitterX = (NextFloat01(rnd[rId++]) * 2.0f - 1.0f) * density * 0.46f;
				f32 jitterZ = (NextFloat01(rnd[rId++]) * 2.0f - 1.0f) * density * 0.46f;
                f32 localX = Clampf32(x + jitterX, 0.0f, T_CHUNK_CELLS);
                f32 localZ = Clampf32(z + jitterZ, 0.0f, T_CHUNK_CELLS);

                f32 localY;
                float3 normal;
                if (!tFoliageFindSurfaceY(job->density, localX, localZ, &localY, &normal)) continue;

				f32 scale = type->params.sizeVariance ? RepeatMinMaxF32(rnd[rId++], 0.8f, 1.2f) : 1.0f;

                float3 localPos = F3Sub((float3){ localX, localY, localZ }, F3MulF(normal, T_FOLIAGE_GROUND_SNAP_OFFSET));
                f32 yaw = NextFloat01(rnd[rId++]) * 2.0f * MATH_PI;
                tFoliagePlacement* placement = &job->placements[job->count++];
                placement->localPos = Pack16x4Fixed(VecSetR(localPos.x, localPos.y, localPos.z, 0.0f), (f32)T_CHUNK_CELLS);
                placement->rotation = PackQuaternionS16NormRet(CreateFoliageRotation(normal, yaw, type->normalAlign));
                placement->scale = scale;
                placement->typeIndex = t;
            }
        }
    }
}

// Safe because group entries remain contiguous and never reorder.
static void ScheduleFoliageJob(JobSystem* js, tFoliageJob* job, const tChunk* chunk, bool placeAllEnabled)
{
	if (!chunk->density) return; // chunk had no cached density grid yet, nothing to sample
    job->chunkMin = chunk->min;
    job->density = chunk->density;
    job->placeAllEnabled = placeAllEnabled;
    job->count = 0u;
    JobSystem_Execute(js, RunFoliageJob, job);
}

// Destroys chunk->foliageEntities range: colliders individually, RenderSet entities batched by groupIdx.
static void DestroyFoliageEntityRange(tChunk* chunk, u32 start, u32 count)
{
    RenderSet* set = &gFoliage.scene.surfaceSet;
    u32 end = start + count;
    u32 i = start;
    while (i < end)
    {
        u32 groupIdx = chunk->foliageEntities[i].packed >> 3;
        u32 runStart = i;
        while (i < end && (chunk->foliageEntities[i].packed >> 3) == groupIdx) i++;
        u32 runCount = i - runStart;

        tFoliageEntity* first = &chunk->foliageEntities[runStart];
        if (first->sparseIdx >= set->maxEntities || groupIdx >= set->numGroups) continue;
        u32 denseIdx = set->sparseID[first->sparseIdx];
        if (denseIdx == INVALID_ENTITY) continue;
        u32 localStartIdx = denseIdx - set->primitiveGroups[groupIdx].entityOffset;
        RenderSet_RemoveEntities(set, groupIdx, localStartIdx, runCount);
    }
}

void tFoliage_DestroyChunkFoliage(tChunk* chunk)
{
    if (!chunk) return;
    if (!chunk->foliageEntities) { chunk->foliageCount = 0u; return; }

    DestroyFoliageEntityRange(chunk, 0u, chunk->foliageCount);

    DeAllocateTLSFGlobal(chunk->foliageEntities);
    chunk->foliageEntities = NULL;
    chunk->foliageCount = 0u;
}

// which foliage type owns a render set group, INVALID_GROUP if none (every type owns a
// contiguous [groupIdx, groupIdx+groupCount) range, ranges never overlap between types)
static u32 FindFoliageTypeIndexForGroup(u32 groupIdx)
{
    for (u32 t = 0; t < gFoliage.numTypes; t++)
    {
        const tFoliageType* type = &gFoliage.types[t];
        if (groupIdx >= type->groupIdx && groupIdx < type->groupIdx + type->groupCount) return t;
    }
    return ~0u;
}

// Tears down entities of dirty types only, compacting survivors in-place.
// Untouched types are left alone; runs are classified once by groupIdx.
static void RemoveDirtyChunkFoliage(tChunk* chunk)
{
    if (!chunk->foliageEntities) { chunk->foliageCount = 0u; return; }

    u32 total = chunk->foliageCount;
    u32 kept = 0u;
    u32 i = 0;
    while (i < total)
    {
        u32 groupIdx = chunk->foliageEntities[i].packed >> 3;
        u32 runStart = i;
        while (i < total && (chunk->foliageEntities[i].packed >> 3) == groupIdx) i++;
        u32 runCount = i - runStart;

        u32 typeIdx = FindFoliageTypeIndexForGroup(groupIdx);
        if (typeIdx != ~0u && !gFoliage.types[typeIdx].paramsDirty)
        {
            if (kept != runStart)
                MemCopy(&chunk->foliageEntities[kept], &chunk->foliageEntities[runStart], sizeof(tFoliageEntity) * runCount);
            kept += runCount;
            continue;
        }

        DestroyFoliageEntityRange(chunk, runStart, runCount);
    }
    chunk->foliageCount = (u16)kept;
}

// Runs once per tFoliage_Update burst after JobSystem_Wait.
// Bulk-adds dirty placements via RenderSet_AddEntities per (type, render group).
static void IntegrateFinishedFoliage(u32 scheduledCount)
{
    if (scheduledCount == 0u) return;

    tChunk** resolvedChunks = (tChunk**)AllocateTLSFGlobal(sizeof(tChunk*) * scheduledCount);
    if (!resolvedChunks) return;

    u32 typeCounts[T_MAX_FOLIAGE_SCENE] = {0};

    // pass 1: re-resolve each job's chunk (it may have been evicted/reused while the job
    // was in flight), strip only the dirty-type entities out of it, and tally how many new
    // placements landed per type so the batch arrays below can be sized exactly.
    for (u32 i = 0; i < scheduledCount; i++)
    {
        tFoliageJob* job = &gFoliage.jobs[i];
        tChunk* chunk = tFindChunkByMin(job->chunkMin);
        resolvedChunks[i] = chunk;
        if (!chunk) continue;

        RemoveDirtyChunkFoliage(chunk);

        u32 addCount = 0u;
        for (u32 p = 0; p < job->count; p++)
        {
            tFoliageType* type = &gFoliage.types[job->placements[p].typeIndex];
            if (type->groupIdx == INVALID_GROUP) continue;
            addCount += type->groupCount;
            typeCounts[job->placements[p].typeIndex]++;
        }
        if (addCount == 0u) continue; // nothing new; kept entities are already compacted

        u32 keptCount = chunk->foliageCount;
        tFoliageEntity* grown = (tFoliageEntity*)AllocateTLSFGlobal(sizeof(tFoliageEntity) * (keptCount + addCount));
        if (!grown) continue;
        if (keptCount > 0u) MemCopy(grown, chunk->foliageEntities, sizeof(tFoliageEntity) * keptCount);
        if (chunk->foliageEntities) DeAllocateTLSFGlobal(chunk->foliageEntities);
        chunk->foliageEntities = grown; // foliageCount (== keptCount) is now pass 3's write cursor
    }

    // pass 2: bucket every dirty-type placement into that type's batch array. worldPos/
    // rotation/scale are resolved once here and reused for every render group.
    tFoliageBatchItem* typeItems[T_MAX_FOLIAGE_SCENE] = {0};
    u32 typeWriteCursor[T_MAX_FOLIAGE_SCENE] = {0};
    for (u32 t = 0; t < gFoliage.numTypes; t++)
        if (typeCounts[t] > 0u)
            typeItems[t] = (tFoliageBatchItem*)AllocateTLSFGlobal(sizeof(tFoliageBatchItem) * typeCounts[t]);

    for (u32 i = 0; i < scheduledCount; i++)
    {
        tChunk* chunk = resolvedChunks[i];
        if (!chunk || !chunk->foliageEntities) continue;
        tFoliageJob* job = &gFoliage.jobs[i];

        for (u32 p = 0; p < job->count; p++)
        {
            const tFoliagePlacement* placement = &job->placements[p];
            tFoliageType* type = &gFoliage.types[placement->typeIndex];
            if (type->groupIdx == INVALID_GROUP || !typeItems[placement->typeIndex]) continue;

            float3 worldPos = F3Add(ToFloat3(job->chunkMin), Vec3Get(Unpack16x4Fixed(placement->localPos, (f32)T_CHUNK_CELLS)));
            f32 finalScale = type->params.size * placement->scale;
            worldPos.y -= type->localBaseY * finalScale;

            tFoliageBatchItem* item = &typeItems[placement->typeIndex][typeWriteCursor[placement->typeIndex]++];
            item->chunk = chunk;
            item->position = VecSetR(worldPos.x, worldPos.y, worldPos.z, 0.0f);
            item->rotation = placement->rotation;
            item->scale = EntityPackUniformWorldScale(finalScale);
        }
    }

    // pass 3: one bulk RenderSet_AddEntities call per (dirty type, render group).
    RenderSet* set = &gFoliage.scene.surfaceSet;
    for (u32 t = 0; t < gFoliage.numTypes; t++)
    {
        u32 count = typeCounts[t];
        if (count == 0u || !typeItems[t]) continue;
        tFoliageType* type = &gFoliage.types[t];

        Entity* entityBuf = (Entity*)AllocateTLSFGlobal(sizeof(Entity) * count);
        if (!entityBuf) {
            DeAllocateTLSFGlobal(typeItems[t]);
            continue;
        }

        for (u32 g = 0; g < type->groupCount; g++)
        {
            u32 groupIdx = type->groupIdx + g;
            u32 sparseBase = RenderSet_AllocateSparseIDRange(set, (int)count);
            if (sparseBase == INVALID_ENTITY)
                continue;

            for (u32 k = 0; k < count; k++)
            {
                Entity e = {0};
                e.position = typeItems[t][k].position;
                e.rotation = typeItems[t][k].rotation;
                e.scale    = typeItems[t][k].scale;
                e.sparseIdx = sparseBase + k;
                e.hiddenBitAndAmbient = 64 << 1;
                entityBuf[k] = e;
            }

            if (RenderSet_AddEntities(set, groupIdx, count, entityBuf) == INVALID_ENTITY)
            {
                RenderSet_FreeSparseIDRange(set, sparseBase, count);
                continue;
            }

            for (u32 k = 0; k < count; k++)
            {
                tChunk* itemChunk = typeItems[t][k].chunk;
				tFoliageEntity foliage = { sparseBase + k, groupIdx << 3 };
				itemChunk->foliageEntities[itemChunk->foliageCount++] = foliage;
            }
        }

        DeAllocateTLSFGlobal(entityBuf);
        DeAllocateTLSFGlobal(typeItems[t]);
    }

    DeAllocateTLSFGlobal(resolvedChunks);
}

void tFoliage_Update(void)
{
    JobSystem* js = tGetTerrainJobSystem();
    if (!js) return;

    bool anyDirty = false;
    for (u32 t = 0; t < gFoliage.numTypes; t++)
        if (gFoliage.types[t].paramsDirty) { anyDirty = true; break; }

	// Always walk residents (ignoring anyDirty) so streamed-in chunks get an initial foliage pass.
	// Uses the occupied-chunks bitset instead of trusting chunkCount density.
    const u64* occupied = tGetOccupiedChunksBitset();
    u32 scheduledCount = 0u;
    for (u32 w = 0; occupied && w < T_CHUNK_BITSET_WORDS && scheduledCount < T_MAX_CHUNKS; w++)
    {
        u64 word = occupied[w];
        while (word)
        {
            s32 bit = FindFirstSet(word);
            word &= word - 1; // clear the lowest set bit, move to the next resident
            u32 chunkIndex = (w << 6) + (u32)bit;

            tChunk* chunk = tGetChunkByIndex(chunkIndex);
            if (!chunk || chunk->buildState != CHUNK_READY || !chunk->mesh.vertices.heapPtr) continue;
            if (!chunk->density) continue; // no cached density grid yet, nothing to sample

			bool needsInitialBuild = !chunk->foliageBuilt;
            if (!anyDirty && !needsInitialBuild) continue; // nothing to do for this chunk right now

            ScheduleFoliageJob(js, &gFoliage.jobs[scheduledCount], chunk, needsInitialBuild);
            chunk->foliageBuilt = true; // decided (even a zero-placement result counts)
            scheduledCount++;
        }
    }

    if (scheduledCount > 0u)
    {
        JobSystem_Wait(js);
        IntegrateFinishedFoliage(scheduledCount);
    }

    // every type considered dirty at the start of this burst has now been fully
    // re-evaluated across every resident chunk - clear them, leave the rest untouched
    for (u32 t = 0; t < gFoliage.numTypes; t++)
        gFoliage.types[t].paramsDirty = false;
}
