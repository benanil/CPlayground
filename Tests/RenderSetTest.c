// Standalone RenderSet unit tests using placeholder SceneBundle data.
// Run from the repo root with:
//   Tests\RunRenderSetTest.bat

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Extern/miniperf.h"

// RenderSet.c includes Platform.h only for logging. Avoid SDL logging/linking in tests.
#define PLATFORM_H
#define AX_LOG(format, ...)  TestLog("INFO", format, ##__VA_ARGS__)
#define AX_WARN(format, ...) TestLog("WARN", format, ##__VA_ARGS__)
#define AX_ERROR(format, ...) TestLog("ERR", format, ##__VA_ARGS__)

#include "Include/Memory.h"
#include "Include/Graphics.h"

Arena GlobalArena;
Graphics gGFX;

static int gChecks;
static int gFailures;

#define CHECK(cond, ...) do { \
    gChecks++; \
    if (!(cond)) { \
        gFailures++; \
        printf("FAIL %s:%d  ", __FUNCTION__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static void TestLog(const char* level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    printf("%s: ", level);
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

void* AllocateTLSFGlobal(size_t size)
{
    return malloc(size);
}

void* ReAllocateTLSFGlobal(void* ptr, size_t size)
{
    return realloc(ptr, size);
}

void DeAllocateTLSFGlobal(void* ptr)
{
    free(ptr);
}

void* AllocZeroTLSFGlobal(size_t count, size_t size)
{
    return calloc(count, size);
}

void* ArenaAllocAlign(Arena* a, size_t size, size_t align)
{
    (void)a;
    (void)align;
    return malloc(size);
}

void ArenaPopAligned(Arena* a, void* ptr, size_t size, size_t align)
{
    (void)a;
    (void)size;
    (void)align;
    free(ptr);
}

void* ArenaAlloc(Arena* a, size_t size) { return ArenaAllocAlign(a, size, DEFAULT_ALIGN); }
void* ArenaAllocZero(Arena* a, size_t size) { void* p = ArenaAlloc(a, size); if (p) memset(p, 0, size); return p; }
void ArenaPopGlobal(uint64_t size) { (void)size; }
ArenaMark ArenaSave(Arena* a) { (void)a; return (ArenaMark){0}; }
void ArenaRestore(Arena* a, ArenaMark mark) { (void)a; (void)mark; }

#include "Source/Rendering/RenderSet.c"

void RenderSet_AddEntitiesCallback(RenderSet* set, u32 groupIdx, u32 localStartIdx, u32 count)
{
    (void)set; (void)groupIdx; (void)localStartIdx; (void)count;
}

void RenderSet_RemoveRangeCallback(RenderSet* set, u32 groupIdx, u32 localStartIdx, u32 count)
{
    (void)set; (void)groupIdx; (void)localStartIdx; (void)count;
}

void RenderSet_RemoveGroupsCallback(RenderSet* set, u32 firstGroup, u32 groupCount)
{
    (void)set; (void)firstGroup; (void)groupCount;
}

void RenderSet_ClearEntitiesCallback(RenderSet* set)
{
    (void)set;
}

static AVertex gSurfaceVertices[1024];
static ASkinedVertex gSkinnedVertices[1024];
static u32 gIndices[4096];

static APrimitive MakePrimitive(int material, int indexOffset, int numIndices, int vertexOffset, int numVertices)
{
    APrimitive p;
    memset(&p, 0, sizeof(p));
    p.material = (unsigned short)material;
    p.indexOffset = indexOffset;
    p.numIndices = numIndices;
    p.numVertices = numVertices;
    p.min[0] = p.min[1] = p.min[2] = -1.0f;
    p.max[0] = p.max[1] = p.max[2] = 1.0f;
    p.min[3] = 0.0f;
    p.max[3] = 0.0f;
    for (u32 lod = 0; lod < MESH_LOD_COUNT; lod++)
    {
        p.lodIndexOffset[lod] = indexOffset + (int)lod * 100;
        p.lodNumIndices[lod] = lod == 0 ? numIndices : numIndices / 2;
        p.lodVertexOffset[lod] = vertexOffset;
        p.lodNumVertices[lod] = numVertices;
        p.lodAnimatedVertexOffset[lod] = vertexOffset;
    }
    return p;
}

static ANode MakeMeshNode(int meshIndex)
{
    ANode n;
    memset(&n, 0, sizeof(n));
    n.translation[3] = 1.0f;
    n.rotation[3] = 1.0f;
    n.scale[0] = n.scale[1] = n.scale[2] = n.scale[3] = 1.0f;
    n.type = 0;
    n.index = meshIndex;
    n.skin = -1;
    n.parent = -1;
    return n;
}

static ANode MakeEmptyNode(void)
{
    ANode n;
    memset(&n, 0, sizeof(n));
    n.translation[3] = 1.0f;
    n.rotation[3] = 1.0f;
    n.scale[0] = n.scale[1] = n.scale[2] = n.scale[3] = 1.0f;
    n.type = 1;
    n.index = -1;
    n.skin = -1;
    n.parent = -1;
    return n;
}

static SceneBundle MakeBundle(AMesh* meshes, int numMeshes, ANode* nodes, int numNodes, int materialCount, int vertexBase)
{
    SceneBundle b;
    memset(&b, 0, sizeof(b));
    s32 primitiveOffset = 0;
    for (int i = 0; i < numMeshes; i++)
    {
        meshes[i].primitiveOffset = primitiveOffset;
        primitiveOffset += meshes[i].numPrimitives;
    }
    b.meshes = meshes;
    b.numMeshes = numMeshes;
    b.nodes = nodes;
    b.numNodes = numNodes;
    b.numMaterials = materialCount;
    b.allVertices = gSurfaceVertices + vertexBase;
    b.allIndices = gIndices;
    return b;
}

static Entity MakeEntity(u32 sparseIdx)
{
    Entity e;
    memset(&e, 0, sizeof(e));
    e.position = VecZero();
    PackQuaternionS16Norm(QIdentity(), &e.rotation);
    e.scale = EntityPackUniformWorldScale(1.0f);
    e.sparseIdx = sparseIdx;
    return e;
}

static void AddEntitiesToGroup(RenderSet* set, u32 groupIdx, u32 count)
{
    Entity entities[32];

    CHECK(count <= (u32)(sizeof(entities) / sizeof(entities[0])),
          "too many test entities: %u", count);

    if (count > (u32)(sizeof(entities) / sizeof(entities[0])))
        return;

    for (u32 i = 0; i < count; i++)
        entities[i] = MakeEntity(INVALID_ENTITY);

    u32 result = RenderSet_AddEntities(set, groupIdx, count, entities);
    CHECK(result != INVALID_ENTITY, "AddEntities failed group=%u count=%u", groupIdx, count);
}

static void AddSparseEntitiesToGroup(RenderSet* set, u32 groupIdx, u32 count)
{
    Entity entities[32];

    CHECK(count <= (u32)(sizeof(entities) / sizeof(entities[0])),
          "too many test entities: %u", count);

    if (count > (u32)(sizeof(entities) / sizeof(entities[0])))
        return;

    u32 sparseStart = RenderSet_AllocateSparseIDRange(set, (int)count);
    CHECK(sparseStart != INVALID_ENTITY, "sparse allocation failed count=%u", count);
    if (sparseStart == INVALID_ENTITY)
        return;

    for (u32 i = 0; i < count; i++)
        entities[i] = MakeEntity(sparseStart + i);

    u32 result = RenderSet_AddEntities(set, groupIdx, count, entities);
    CHECK(result != INVALID_ENTITY, "AddEntities failed group=%u count=%u", groupIdx, count);
}

static void CheckGroupRange(const RenderSet* set, u32 groupIdx, u32 offset, u32 count, const char* label)
{
    CHECK(groupIdx < set->numGroups, "%s group out of range group=%u numGroups=%u",
          label, groupIdx, set->numGroups);

    if (groupIdx >= set->numGroups)
        return;

    const PrimitiveGroup* group = &set->primitiveGroups[groupIdx];

    CHECK(group->entityOffset == offset && group->numEntities == count,
          "%s group=%u range=%u+%u expected=%u+%u",
          label,
          groupIdx,
          group->entityOffset,
          group->numEntities,
          offset,
          count);
}

static void CheckDensePrimitivePattern(const RenderSet* set, const u32* expected, u32 count, const char* label)
{
    CHECK(set->numEntities == count, "%s numEntities=%u expected=%u", label, set->numEntities, count);

    u32 n = set->numEntities < count ? set->numEntities : count;

    for (u32 i = 0; i < n; i++)
    {
        CHECK(set->entities[i].primitiveIdx == expected[i],
              "%s primitiveIdx[%u]=%u expected=%u",
              label,
              i,
              set->entities[i].primitiveIdx,
              expected[i]);
    }
}

static void CheckDenseMappingsMatchGroups(const RenderSet* set, const char* label)
{
    for (u32 groupIdx = 0; groupIdx < set->numGroups; groupIdx++)
    {
        const PrimitiveGroup* group = &set->primitiveGroups[groupIdx];

        CHECK(group->entityOffset + group->numEntities <= set->numEntities,
              "%s group=%u invalid range %u+%u numEntities=%u",
              label,
              groupIdx,
              group->entityOffset,
              group->numEntities,
              set->numEntities);

        for (u32 i = 0; i < group->numEntities; i++)
        {
            u32 denseIdx = group->entityOffset + i;

            CHECK(set->entities[denseIdx].primitiveIdx == groupIdx,
                  "%s dense=%u maps to group=%u expected=%u",
                  label,
                  denseIdx,
                  set->entities[denseIdx].primitiveIdx,
                  groupIdx);
        }
    }
}

typedef struct RenderSetPerfCase_
{
    RenderSet set;
    SceneBundle bundle;
    AMesh meshes[32];
    APrimitive primitives[64];
    ANode nodes[256];
    u32 bundleIdx;
    u32 iterations;
} RenderSetPerfCase;

static void InitRenderSetPerfCase(RenderSetPerfCase* perf)
{
    memset(perf, 0, sizeof(*perf));
    RenderSet_InitSet(&perf->set, 4096, 128, 4, false);

    for (u32 m = 0; m < 32; m++)
    {
        perf->meshes[m].name = "PerfMesh";
        perf->meshes[m].primitives = perf->primitives + m * 2u;
        perf->meshes[m].numPrimitives = 2;
        perf->meshes[m].primitiveOffset = (int)(m * 2u);
        perf->primitives[m * 2u + 0u] = MakePrimitive(0, (int)(m * 18u),      9, (int)(m * 6u),      3);
        perf->primitives[m * 2u + 1u] = MakePrimitive(1, (int)(m * 18u + 9u), 9, (int)(m * 6u + 3u), 3);
    }

    for (u32 n = 0; n < 256; n++)
    {
        perf->nodes[n] = MakeMeshNode((int)(n & 31u));
    }

    perf->bundle = MakeBundle(perf->meshes, 32, perf->nodes, 256, 2, 0);
    perf->bundleIdx = RenderSet_AddSceneBundle(&perf->set, &perf->bundle, 0);
    perf->iterations = 1000;
}

static void RenderSetPerf_AddScene(void* arg)
{
    RenderSetPerfCase* perf = (RenderSetPerfCase*)arg;
    for (u32 i = 0; i < perf->iterations; i++)
    {
        RenderSet_ClearEntities(&perf->set);
        RenderSet_AddScene(&perf->set, perf->bundleIdx, VecZero(), QIdentity(), VecSet1(1.0f), false);
    }
}

static void PrintMiniPerfResult(const char* label, MiniPerfResult result, u32 iterations)
{
    printf("PERF %s: %.6f sec total, %.3f us/iter, ctxsw=%llu\n",
           label,
           result.ElapsedTime,
           (result.ElapsedTime * 1000000.0) / (double)iterations,
           (unsigned long long)result.ContextSwitches);
    printf("PERF %s: cycles=%llu instructions=%llu branchMiss=%llu branch=%llu dataMiss=%llu dataAccess=%llu\n",
           label,
           (unsigned long long)result.Counters[MP_CycleCount],
           (unsigned long long)result.Counters[MP_Instructions],
           (unsigned long long)result.Counters[MP_BranchMisses],
           (unsigned long long)result.Counters[MP_BranchCount],
           (unsigned long long)result.Counters[MP_DataMisses],
           (unsigned long long)result.Counters[MP_DataAccess]);
}

static void RunRenderSetPerfTests(void)
{
    RenderSetPerfCase perf;
    InitRenderSetPerfCase(&perf);

    RenderSet_ClearEntities(&perf.set);
    MiniPerfResult addScene = MiniPerf(RenderSetPerf_AddScene, &perf);
    PrintMiniPerfResult("RenderSet_AddScene", addScene, perf.iterations);
}

static void TestBundleRegistration(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive primA[2] = { MakePrimitive(0, 10, 30, 0, 8), MakePrimitive(1, 40, 18, 8, 6) };
    AMesh meshesA[1] = { { "A", primA, 2, 0, NULL } };
    SceneBundle bundleA = MakeBundle(meshesA, 1, NULL, 0, 2, 100);

    APrimitive primB[1] = { MakePrimitive(0, 100, 12, 0, 4) };
    AMesh meshesB[1] = { { "B", primB, 1, 0, NULL } };
    SceneBundle bundleB = MakeBundle(meshesB, 1, NULL, 0, 1, 200);

    u32 a = RenderSet_AddSceneBundle(&set, &bundleA, 10);
    u32 b = RenderSet_AddSceneBundle(&set, &bundleB, 20);

    CHECK(a == 0, "first bundle index=%u", a);
    CHECK(b == 1, "second bundle index=%u", b);
    CHECK(set.numGroups == 3, "numGroups=%u", set.numGroups);
    CHECK(set.bundlePrimitiveRange[0].start == 0 && set.bundlePrimitiveRange[0].count == 2, "bundle A range %u+%u", set.bundlePrimitiveRange[0].start, set.bundlePrimitiveRange[0].count);
    CHECK(set.bundlePrimitiveRange[1].start == 2 && set.bundlePrimitiveRange[1].count == 1, "bundle B range %u+%u", set.bundlePrimitiveRange[1].start, set.bundlePrimitiveRange[1].count);
    CHECK(set.primitiveGroups[0].materialIndex == 10, "group0 material=%u", set.primitiveGroups[0].materialIndex);
    CHECK(set.primitiveGroups[1].materialIndex == 11, "group1 material=%u", set.primitiveGroups[1].materialIndex);
    CHECK(set.primitiveGroups[2].materialIndex == 20, "group2 material=%u", set.primitiveGroups[2].materialIndex);
    CHECK(set.primitiveGroups[0].lodVertexOffset[0] == 100, "group0 vertexOffset=%u", set.primitiveGroups[0].lodVertexOffset[0]);
    CHECK(set.primitiveGroups[1].lodVertexOffset[0] == 108, "group1 vertexOffset=%u", set.primitiveGroups[1].lodVertexOffset[0]);
    CHECK(set.primitiveGroups[2].lodVertexOffset[0] == 200, "group2 vertexOffset=%u", set.primitiveGroups[2].lodVertexOffset[0]);
    CHECK(RenderSet_Validate(&set, "registration"), "validation failed");
}

static void TestMiddleInsertionKeepsMappings(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive primA[2] = { MakePrimitive(0, 0, 9, 0, 3), MakePrimitive(0, 9, 9, 3, 3) };
    AMesh meshesA[1] = { { "A", primA, 2, 0, NULL } };
    SceneBundle bundleA = MakeBundle(meshesA, 1, NULL, 0, 1, 0);

    APrimitive primB[2] = { MakePrimitive(0, 18, 9, 0, 3), MakePrimitive(0, 27, 9, 3, 3) };
    AMesh meshesB[1] = { { "B", primB, 2, 0, NULL } };
    SceneBundle bundleB = MakeBundle(meshesB, 1, NULL, 0, 1, 10);

    RenderSet_AddSceneBundle(&set, &bundleA, 0);
    RenderSet_AddSceneBundle(&set, &bundleB, 1);

    Entity e = MakeEntity(INVALID_ENTITY);
    CHECK(RenderSet_AddEntity(&set, 0, &e) != INVALID_ENTITY, "add group 0");
    CHECK(RenderSet_AddEntity(&set, 2, &e) != INVALID_ENTITY, "add group 2 after middle shift");
    CHECK(RenderSet_AddEntity(&set, 1, &e) != INVALID_ENTITY, "add group 1 after group 2 has entity");

    CHECK(set.numEntities == 3, "numEntities=%u", set.numEntities);
    CHECK(set.primitiveGroups[0].entityOffset == 0 && set.primitiveGroups[0].numEntities == 1, "group0 range %u+%u", set.primitiveGroups[0].entityOffset, set.primitiveGroups[0].numEntities);
    CHECK(set.primitiveGroups[1].entityOffset == 1 && set.primitiveGroups[1].numEntities == 1, "group1 range %u+%u", set.primitiveGroups[1].entityOffset, set.primitiveGroups[1].numEntities);
    CHECK(set.primitiveGroups[2].entityOffset == 2 && set.primitiveGroups[2].numEntities == 1, "group2 range %u+%u", set.primitiveGroups[2].entityOffset, set.primitiveGroups[2].numEntities);
    CHECK(set.primitiveGroups[3].entityOffset == 3 && set.primitiveGroups[3].numEntities == 0, "group3 offset=%u", set.primitiveGroups[3].entityOffset);
    for (u32 i = 0; i < set.numEntities; i++)
        CHECK(set.entities[i].primitiveIdx == i, "dense %u primitive=%u", i, set.entities[i].primitiveIdx);
    CHECK(RenderSet_Validate(&set, "middle insertion"), "validation failed");
}

static void TestAddScenePlaceholderHierarchy(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim0[2] = { MakePrimitive(0, 0, 9, 0, 3), MakePrimitive(0, 9, 9, 3, 3) };
    APrimitive prim1[1] = { MakePrimitive(1, 18, 12, 6, 4) };
    AMesh meshes[2] = {
        { "M0", prim0, 2, 0, NULL },
        { "M1", prim1, 1, 0, NULL }
    };
    ANode nodes[2] = { MakeMeshNode(0), MakeMeshNode(1) };
    SceneBundle bundle = MakeBundle(meshes, 2, nodes, 2, 2, 32);

    u32 bundleIdx = RenderSet_AddSceneBundle(&set, &bundle, 5);
    u32 added = RenderSet_AddScene(&set, bundleIdx, VecZero(), QIdentity(), VecSet1(1.0f), false);

    CHECK(added == 3, "added=%u", added);
    CHECK(set.numEntities == 3, "numEntities=%u", set.numEntities);
    CHECK(set.entities[0].sparseIdx != set.entities[1].sparseIdx, "mesh0 primitives should get separate sparse ids");
    CHECK(set.entities[2].sparseIdx != set.entities[0].sparseIdx, "mesh1 should get another sparse id");
    CHECK(((set.entities[0].parentIdx >> 24u) & ENTITY_FLAG_NOMESH) == 0u, "root mesh should not be no-mesh flagged");
    CHECK(RenderSet_Validate(&set, "add scene"), "validation failed");
}

static void TestAddSceneSkipsNoMeshNode(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim[1] = { MakePrimitive(0, 0, 9, 0, 3) };
    AMesh meshes[1] = { { "M0", prim, 1, 0, NULL } };
    ANode nodes[2] = { MakeEmptyNode(), MakeMeshNode(0) };
    nodes[1].parent = 0;
    SceneBundle bundle = MakeBundle(meshes, 1, nodes, 2, 1, 0);

    u32 bundleIdx = RenderSet_AddSceneBundle(&set, &bundle, 0);
    u32 added = RenderSet_AddScene(&set, bundleIdx, VecZero(), QIdentity(), VecSet1(1.0f), false);

    CHECK(added == 1, "added=%u", added);
    CHECK(set.numEntities == 1, "numEntities=%u", set.numEntities);
    CHECK(set.primitiveGroups[0].numEntities == 1, "drawable count=%u", set.primitiveGroups[0].numEntities);
    CHECK(((set.entities[0].parentIdx >> 24u) & ENTITY_FLAG_NOMESH) == 0u, "mesh should not be no-mesh flagged");
    CHECK(RenderSet_Validate(&set, "skip no-mesh scene"), "validation failed");
}

static void TestAddSkinnedSceneSharesSparsePerMeshNode(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, true);

    APrimitive prim[2] = {
        MakePrimitive(0, 0, 9, 0, 3),
        MakePrimitive(0, 9, 12, 3, 4)
    };
    AMesh meshes[1] = { { "M0", prim, 2, 0, NULL } };
    ANode nodes[1] = { MakeMeshNode(0) };
    nodes[0].skin = 0;
    SceneBundle bundle = MakeBundle(meshes, 1, nodes, 1, 1, 0);

    u32 bundleIdx = RenderSet_AddSceneBundle(&set, &bundle, 0);
    u32 added = RenderSet_AddScene(&set, bundleIdx, VecZero(), QIdentity(), VecSet1(1.0f), true);

    CHECK(added == 2, "added=%u", added);
    CHECK(set.numEntities == 2, "numEntities=%u", set.numEntities);
    CHECK(set.entities[0].sparseIdx == set.entities[1].sparseIdx, "skinned mesh node primitives should share sparse id");
    CHECK(RenderSet_Validate(&set, "add skinned shared sparse"), "validation failed");
}

static void TestSparseCapacityFailureDoesNotMutate(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 1, 2, 1, false);

    APrimitive prim[1] = { MakePrimitive(0, 0, 9, 0, 3) };
    AMesh meshes[1] = { { "A", prim, 1, 0, NULL } };
    SceneBundle bundle = MakeBundle(meshes, 1, NULL, 0, 1, 0);
    RenderSet_AddSceneBundle(&set, &bundle, 0);

    Entity two[2] = { MakeEntity(INVALID_ENTITY), MakeEntity(INVALID_ENTITY) };
    u32 result = RenderSet_AddEntities(&set, 0, 2, two);

    CHECK(result == INVALID_ENTITY, "result=%u", result);
    CHECK(set.numEntities == 0, "numEntities mutated to %u", set.numEntities);
    CHECK(set.primitiveGroups[0].numEntities == 0, "group count mutated to %u", set.primitiveGroups[0].numEntities);
    CHECK(RenderSet_Validate(&set, "sparse failure"), "validation failed");
}

static void TestAddEntityAfterSceneKeepsGroups(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim[2] = {
        MakePrimitive(0, 0, 9, 0, 3),
        MakePrimitive(0, 9, 9, 3, 3)
    };
    AMesh meshes[1] = { { "M0", prim, 2, 0, NULL } };
    ANode nodes[2] = { MakeEmptyNode(), MakeMeshNode(0) };
    nodes[1].parent = 0;
    SceneBundle bundle = MakeBundle(meshes, 1, nodes, 2, 1, 0);

    u32 bundleIdx = RenderSet_AddSceneBundle(&set, &bundle, 0);
    RenderSet_AddScene(&set, bundleIdx, VecZero(), QIdentity(), VecSet1(1.0f), false);

    AddEntitiesToGroup(&set, 1, 1);

    CheckGroupRange(&set, 0, 0, 1, "add after scene");
    CheckGroupRange(&set, 1, 1, 2, "add after scene");
    CHECK(set.numEntities == 3, "numEntities=%u", set.numEntities);
    CHECK(RenderSet_Validate(&set, "add after scene"), "validation failed");
}

static void TestRemoveSingleEntityMiddleOfGroup(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim[3] = {
        MakePrimitive(0,  0, 9, 0, 3),
        MakePrimitive(0,  9, 9, 3, 3),
        MakePrimitive(0, 18, 9, 6, 3)
    };

    AMesh meshes[1] = { { "A", prim, 3, 0, NULL } };
    SceneBundle bundle = MakeBundle(meshes, 1, NULL, 0, 1, 0);

    RenderSet_AddSceneBundle(&set, &bundle, 0);

    AddEntitiesToGroup(&set, 0, 2);
    AddEntitiesToGroup(&set, 1, 3);
    AddEntitiesToGroup(&set, 2, 2);

    {
        const u32 expected[] = { 0, 0, 1, 1, 1, 2, 2 };
        CheckDensePrimitivePattern(&set, expected, 7, "before remove single");
    }

    u32 removed = RenderSet_RemoveEntity(&set, 1, 1);
    CHECK(removed != INVALID_ENTITY, "RemoveEntity failed");

    CheckGroupRange(&set, 0, 0, 2, "remove single");
    CheckGroupRange(&set, 1, 2, 2, "remove single");
    CheckGroupRange(&set, 2, 4, 2, "remove single");

    {
        const u32 expected[] = { 0, 0, 1, 1, 2, 2 };
        CheckDensePrimitivePattern(&set, expected, 6, "after remove single");
    }

    CheckDenseMappingsMatchGroups(&set, "after remove single");
    CHECK(RenderSet_Validate(&set, "remove single entity"), "validation failed");
}

static void TestRemoveEntityRangeMiddleOfGroup(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim[3] = {
        MakePrimitive(0,  0, 9, 0, 3),
        MakePrimitive(0,  9, 9, 3, 3),
        MakePrimitive(0, 18, 9, 6, 3)
    };

    AMesh meshes[1] = { { "A", prim, 3, 0, NULL } };
    SceneBundle bundle = MakeBundle(meshes, 1, NULL, 0, 1, 0);

    RenderSet_AddSceneBundle(&set, &bundle, 0);

    AddEntitiesToGroup(&set, 0, 2);
    AddEntitiesToGroup(&set, 1, 4);
    AddEntitiesToGroup(&set, 2, 2);

    u32 removed = RenderSet_RemoveEntities(&set, 1, 1, 2);
    CHECK(removed != INVALID_ENTITY, "RemoveEntities failed");

    CheckGroupRange(&set, 0, 0, 2, "remove range");
    CheckGroupRange(&set, 1, 2, 2, "remove range");
    CheckGroupRange(&set, 2, 4, 2, "remove range");

    {
        const u32 expected[] = { 0, 0, 1, 1, 2, 2 };
        CheckDensePrimitivePattern(&set, expected, 6, "after remove range");
    }

    CheckDenseMappingsMatchGroups(&set, "after remove range");
    CHECK(RenderSet_Validate(&set, "remove entity range"), "validation failed");
}

static void TestRemoveInvalidEntityDoesNotMutate(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim[2] = {
        MakePrimitive(0, 0, 9, 0, 3),
        MakePrimitive(0, 9, 9, 3, 3)
    };

    AMesh meshes[1] = { { "A", prim, 2, 0, NULL } };
    SceneBundle bundle = MakeBundle(meshes, 1, NULL, 0, 1, 0);

    RenderSet_AddSceneBundle(&set, &bundle, 0);

    AddEntitiesToGroup(&set, 0, 2);
    AddEntitiesToGroup(&set, 1, 1);

    u32 numEntities = set.numEntities;
    u32 numGroups = set.numGroups;
    u32 group0Offset = set.primitiveGroups[0].entityOffset;
    u32 group0Count = set.primitiveGroups[0].numEntities;
    u32 group1Offset = set.primitiveGroups[1].entityOffset;
    u32 group1Count = set.primitiveGroups[1].numEntities;

    CHECK(RenderSet_RemoveEntity(&set, 0, 99) == 0,
          "invalid local entity remove should return 0");

    CHECK(RenderSet_RemoveEntity(&set, 99, 0) == 0,
          "invalid group remove should return 0");

    CHECK(RenderSet_RemoveEntities(&set, 0, 99, 1) == 0,
          "invalid range start remove should return 0");

    CHECK(RenderSet_RemoveEntities(&set, 0, 0, 0) == 0,
          "zero count remove should return 0");

    CHECK(set.numEntities == numEntities, "numEntities mutated %u -> %u", numEntities, set.numEntities);
    CHECK(set.numGroups == numGroups, "numGroups mutated %u -> %u", numGroups, set.numGroups);

    CheckGroupRange(&set, 0, group0Offset, group0Count, "invalid remove");
    CheckGroupRange(&set, 1, group1Offset, group1Count, "invalid remove");

    CHECK(RenderSet_Validate(&set, "invalid remove entity"), "validation failed");
}

static void TestClearEntitiesKeepsBundlesAndGroups(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive primA[2] = {
        MakePrimitive(0,  0, 9, 0, 3),
        MakePrimitive(0,  9, 9, 3, 3)
    };

    AMesh meshesA[1] = { { "A", primA, 2, 0, NULL } };
    SceneBundle bundleA = MakeBundle(meshesA, 1, NULL, 0, 1, 0);

    APrimitive primB[1] = {
        MakePrimitive(0, 18, 9, 6, 3)
    };

    AMesh meshesB[1] = { { "B", primB, 1, 0, NULL } };
    SceneBundle bundleB = MakeBundle(meshesB, 1, NULL, 0, 1, 64);

    RenderSet_AddSceneBundle(&set, &bundleA, 10);
    RenderSet_AddSceneBundle(&set, &bundleB, 20);

    AddEntitiesToGroup(&set, 0, 2);
    AddEntitiesToGroup(&set, 1, 1);
    AddEntitiesToGroup(&set, 2, 3);

    RenderSet_ClearEntities(&set);

    CHECK(set.numEntities == 0, "numEntities=%u", set.numEntities);

    CHECK(set.numBundles == 2, "numBundles=%u", set.numBundles);
    CHECK(set.numGroups == 3, "numGroups=%u", set.numGroups);

    CHECK(set.bundles[0] == &bundleA, "bundle 0 pointer changed");
    CHECK(set.bundles[1] == &bundleB, "bundle 1 pointer changed");

    CHECK(set.bundlePrimitiveRange[0].start == 0 && set.bundlePrimitiveRange[0].count == 2,
          "bundle A range %u+%u", set.bundlePrimitiveRange[0].start, set.bundlePrimitiveRange[0].count);

    CHECK(set.bundlePrimitiveRange[1].start == 2 && set.bundlePrimitiveRange[1].count == 1,
          "bundle B range %u+%u", set.bundlePrimitiveRange[1].start, set.bundlePrimitiveRange[1].count);

    CheckGroupRange(&set, 0, 0, 0, "clear entities");
    CheckGroupRange(&set, 1, 0, 0, "clear entities");
    CheckGroupRange(&set, 2, 0, 0, "clear entities");

    AddEntitiesToGroup(&set, 1, 1);

    CheckGroupRange(&set, 0, 0, 0, "add after clear");
    CheckGroupRange(&set, 1, 0, 1, "add after clear");
    CheckGroupRange(&set, 2, 1, 0, "add after clear");

    {
        const u32 expected[] = { 1 };
        CheckDensePrimitivePattern(&set, expected, 1, "after add post clear");
    }

    CHECK(RenderSet_Validate(&set, "clear entities"), "validation failed");
}

static void TestRemoveSceneBundleMiddleWithEntities(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 32, 8, 4, false);

    APrimitive primA[2] = {
        MakePrimitive(0,  0, 9, 0, 3),
        MakePrimitive(1,  9, 9, 3, 3)
    };
    AMesh meshesA[1] = { { "A", primA, 2, 0, NULL } };
    SceneBundle bundleA = MakeBundle(meshesA, 1, NULL, 0, 2, 0);

    APrimitive primB[1] = {
        MakePrimitive(0, 18, 9, 6, 3)
    };
    AMesh meshesB[1] = { { "B", primB, 1, 0, NULL } };
    SceneBundle bundleB = MakeBundle(meshesB, 1, NULL, 0, 1, 64);

    APrimitive primC[2] = {
        MakePrimitive(0, 27, 9,  9, 3),
        MakePrimitive(1, 36, 9, 12, 3)
    };
    AMesh meshesC[1] = { { "C", primC, 2, 0, NULL } };
    SceneBundle bundleC = MakeBundle(meshesC, 1, NULL, 0, 2, 128);

    u32 a = RenderSet_AddSceneBundle(&set, &bundleA, 10);
    u32 b = RenderSet_AddSceneBundle(&set, &bundleB, 20);
    u32 c = RenderSet_AddSceneBundle(&set, &bundleC, 30);

    CHECK(a == 0, "bundle A index=%u", a);
    CHECK(b == 1, "bundle B index=%u", b);
    CHECK(c == 2, "bundle C index=%u", c);

    AddEntitiesToGroup(&set, 0, 1);
    AddEntitiesToGroup(&set, 1, 2);
    AddEntitiesToGroup(&set, 2, 3);
    AddEntitiesToGroup(&set, 3, 1);
    AddEntitiesToGroup(&set, 4, 1);

    u32 removed = RenderSet_RemoveSceneBundle(&set, 1);
    CHECK(removed != INVALID_BUNDLE, "RemoveSceneBundle failed");

    // bundle slots are stable handles: removing B (slot 1) leaves a hole, C keeps slot 2 and
    // numBundles stays the watermark (highest used slot + 1).
    CHECK(set.numBundles == 3, "numBundles=%u", set.numBundles);
    CHECK(set.numGroups == 4, "numGroups=%u", set.numGroups);
    CHECK(set.numEntities == 5, "numEntities=%u", set.numEntities);

    CHECK(set.bundles[0] == &bundleA, "bundle 0 should be A");
    CHECK(set.bundles[1] == NULL, "bundle 1 slot should be empty");
    CHECK(set.bundles[2] == &bundleC, "bundle 2 should be C");

    CHECK(set.bundlePrimitiveRange[0].start == 0 && set.bundlePrimitiveRange[0].count == 2,
          "bundle A range %u+%u", set.bundlePrimitiveRange[0].start, set.bundlePrimitiveRange[0].count);

    CHECK(set.bundlePrimitiveRange[1].start == 0 && set.bundlePrimitiveRange[1].count == 0,
          "freed slot range %u+%u", set.bundlePrimitiveRange[1].start, set.bundlePrimitiveRange[1].count);

    CHECK(set.bundlePrimitiveRange[2].start == 2 && set.bundlePrimitiveRange[2].count == 2,
          "bundle C range %u+%u", set.bundlePrimitiveRange[2].start, set.bundlePrimitiveRange[2].count);

    CHECK(set.primitiveGroups[0].materialIndex == 10, "group0 material=%u", set.primitiveGroups[0].materialIndex);
    CHECK(set.primitiveGroups[1].materialIndex == 11, "group1 material=%u", set.primitiveGroups[1].materialIndex);
    CHECK(set.primitiveGroups[2].materialIndex == 30, "group2 material=%u", set.primitiveGroups[2].materialIndex);
    CHECK(set.primitiveGroups[3].materialIndex == 31, "group3 material=%u", set.primitiveGroups[3].materialIndex);

    CheckGroupRange(&set, 0, 0, 1, "remove bundle middle");
    CheckGroupRange(&set, 1, 1, 2, "remove bundle middle");
    CheckGroupRange(&set, 2, 3, 1, "remove bundle middle");
    CheckGroupRange(&set, 3, 4, 1, "remove bundle middle");

    {
        const u32 expected[] = { 0, 1, 1, 2, 3 };
        CheckDensePrimitivePattern(&set, expected, 5, "after remove bundle middle");
    }

    AddEntitiesToGroup(&set, 2, 2);

    CheckGroupRange(&set, 0, 0, 1, "add after remove bundle");
    CheckGroupRange(&set, 1, 1, 2, "add after remove bundle");
    CheckGroupRange(&set, 2, 3, 3, "add after remove bundle");
    CheckGroupRange(&set, 3, 6, 1, "add after remove bundle");

    {
        const u32 expected[] = { 0, 1, 1, 2, 2, 2, 3 };
        CheckDensePrimitivePattern(&set, expected, 7, "after add post bundle remove");
    }

    CheckDenseMappingsMatchGroups(&set, "after remove bundle middle");
    CHECK(RenderSet_Validate(&set, "remove scene bundle middle"), "validation failed");
}

static void TestRemoveInvalidSceneBundleDoesNotMutate(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim[1] = {
        MakePrimitive(0, 0, 9, 0, 3)
    };

    AMesh meshes[1] = { { "A", prim, 1, 0, NULL } };
    SceneBundle bundle = MakeBundle(meshes, 1, NULL, 0, 1, 0);

    RenderSet_AddSceneBundle(&set, &bundle, 0);
    AddEntitiesToGroup(&set, 0, 2);

    u32 numEntities = set.numEntities;
    u32 numGroups = set.numGroups;
    u32 numBundles = set.numBundles;

    CHECK(RenderSet_RemoveSceneBundle(&set, 9) == 0,
    "invalid bundle remove should return 0");

    CHECK(set.numEntities == numEntities, "numEntities mutated %u -> %u", numEntities, set.numEntities);
    CHECK(set.numGroups == numGroups, "numGroups mutated %u -> %u", numGroups, set.numGroups);
    CHECK(set.numBundles == numBundles, "numBundles mutated %u -> %u", numBundles, set.numBundles);

    CheckGroupRange(&set, 0, 0, 2, "invalid bundle remove");
    CHECK(RenderSet_Validate(&set, "invalid scene bundle remove"), "validation failed");
}

static void TestCompactEntitiesRemovesInvalidSparseHoles(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 32, 8, 4, false);

    APrimitive prim[3] = {
        MakePrimitive(0,  0, 9, 0, 3),
        MakePrimitive(0,  9, 9, 3, 3),
        MakePrimitive(0, 18, 9, 6, 3)
    };
    AMesh meshes[1] = { { "A", prim, 3, 0, NULL } };
    SceneBundle bundle = MakeBundle(meshes, 1, NULL, 0, 1, 0);

    RenderSet_AddSceneBundle(&set, &bundle, 0);

    AddSparseEntitiesToGroup(&set, 0, 3);
    AddSparseEntitiesToGroup(&set, 1, 2);
    AddSparseEntitiesToGroup(&set, 2, 2);

    set.entities[1].sparseIdx = INVALID_ENTITY;
    set.entities[3].sparseIdx = INVALID_ENTITY;
    set.entities[4].sparseIdx = INVALID_ENTITY;

    RenderSet_CompactEntities(&set);

    CheckGroupRange(&set, 0, 0, 2, "compact holes");
    CheckGroupRange(&set, 1, 2, 0, "compact holes");
    CheckGroupRange(&set, 2, 2, 2, "compact holes");

    {
        const u32 expected[] = { 0, 0, 2, 2 };
        CheckDensePrimitivePattern(&set, expected, 4, "after compact holes");
    }

    CHECK(RenderSet_Validate(&set, "compact holes"), "validation failed");
}

static void TestCompactEntitiesAfterSkippedNoMesh(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim[1] = { MakePrimitive(0, 0, 9, 0, 3) };
    AMesh meshes[1] = { { "M0", prim, 1, 0, NULL } };
    ANode nodes[2] = { MakeEmptyNode(), MakeMeshNode(0) };
    nodes[1].parent = 0;
    SceneBundle bundle = MakeBundle(meshes, 1, nodes, 2, 1, 0);

    u32 bundleIdx = RenderSet_AddSceneBundle(&set, &bundle, 0);
    RenderSet_AddScene(&set, bundleIdx, VecZero(), QIdentity(), VecSet1(1.0f), false);

    set.entities[0].sparseIdx = INVALID_ENTITY;

    RenderSet_CompactEntities(&set);

    CheckGroupRange(&set, 0, 0, 0, "compact skipped no-mesh");
    CHECK(set.numEntities == 0, "numEntities=%u", set.numEntities);
    CHECK(RenderSet_Validate(&set, "compact skipped no-mesh"), "validation failed");
}

static void TestRemoveSceneBundleAfterSkippedNoMesh(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim[1] = { MakePrimitive(0, 0, 9, 0, 3) };
    AMesh meshes[1] = { { "M0", prim, 1, 0, NULL } };
    ANode nodes[2] = { MakeEmptyNode(), MakeMeshNode(0) };
    nodes[1].parent = 0;
    SceneBundle bundle = MakeBundle(meshes, 1, nodes, 2, 1, 0);

    u32 bundleIdx = RenderSet_AddSceneBundle(&set, &bundle, 0);
    RenderSet_AddScene(&set, bundleIdx, VecZero(), QIdentity(), VecSet1(1.0f), false);

    u32 removed = RenderSet_RemoveSceneBundle(&set, bundleIdx);

    CHECK(removed == 1, "removed=%u", removed);
    CHECK(set.numEntities == 0, "numEntities=%u", set.numEntities);
    CHECK(set.numGroups == 0, "numGroups=%u", set.numGroups);
    CHECK(set.numBundles == 0, "numBundles=%u", set.numBundles);
    CHECK(RenderSet_Validate(&set, "remove skipped no-mesh bundle"), "validation failed");
}

static void TestRemoveEntityRangeClampsToGroupEnd(void)
{
    RenderSet set;
    RenderSet_InitSet(&set, 16, 8, 4, false);

    APrimitive prim[2] = {
        MakePrimitive(0, 0, 9, 0, 3),
        MakePrimitive(0, 9, 9, 3, 3)
    };

    AMesh meshes[1] = { { "A", prim, 2, 0, NULL } };
    SceneBundle bundle = MakeBundle(meshes, 1, NULL, 0, 1, 0);

    RenderSet_AddSceneBundle(&set, &bundle, 0);

    AddEntitiesToGroup(&set, 0, 3);
    AddEntitiesToGroup(&set, 1, 2);

    u32 removed = RenderSet_RemoveEntities(&set, 0, 1, 99);

    CHECK(removed == 2, "removed=%u expected=2", removed);

    CheckGroupRange(&set, 0, 0, 1, "clamped remove");
    CheckGroupRange(&set, 1, 1, 2, "clamped remove");

    {
        const u32 expected[] = { 0, 1, 1 };
        CheckDensePrimitivePattern(&set, expected, 3, "after clamped remove");
    }

    CheckDenseMappingsMatchGroups(&set, "after clamped remove");
    CHECK(RenderSet_Validate(&set, "clamped remove entity range"), "validation failed");
}
int main(int argc, char** argv)
{
    gGFX.SurfaceVertexBuffer = gSurfaceVertices;
    gGFX.SkinnedVertexBuffer = gSkinnedVertices;
    gGFX.IndexBuffer = gIndices;

    if (argc > 1 && strcmp(argv[1], "--perf") == 0)
    {
        RunRenderSetPerfTests();
        return 0;
    }

    TestBundleRegistration();
    TestMiddleInsertionKeepsMappings();
    TestAddScenePlaceholderHierarchy();
    TestAddSceneSkipsNoMeshNode();
    TestAddSkinnedSceneSharesSparsePerMeshNode();
    TestSparseCapacityFailureDoesNotMutate();
    TestAddEntityAfterSceneKeepsGroups();

    TestRemoveSingleEntityMiddleOfGroup();
    TestRemoveEntityRangeMiddleOfGroup();
    TestRemoveInvalidEntityDoesNotMutate();
    TestClearEntitiesKeepsBundlesAndGroups();
    TestRemoveSceneBundleMiddleWithEntities();
    TestRemoveInvalidSceneBundleDoesNotMutate();
    TestCompactEntitiesRemovesInvalidSparseHoles();
    TestCompactEntitiesAfterSkippedNoMesh();
    TestRemoveSceneBundleAfterSkippedNoMesh();
    TestRemoveEntityRangeClampsToGroupEnd();
    printf("RenderSetTest: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
