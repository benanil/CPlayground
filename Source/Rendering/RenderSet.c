
#include "Include/RenderSet.h"
#include "Include/Graphics.h"
#include "Include/Memory.h"
#include "Include/Algorithm.h"
#include "Include/Random.h"
#include "Include/Platform.h"
#include "Include/Bitset.h"
#include "Math/Half.h"
#include "Math/Matrix.h"
#include "Math/Bitpack.h"

extern Graphics gGFX;

void RenderSet_InitSet(RenderSet* set, u32 maxEntities, u32 maxGroups, u32 maxBundles, bool skinned)
{
    MemsetZero(set, sizeof(*set));
    set->maxEntities = maxEntities;
    set->maxGroups   = maxGroups;
    set->maxBundles  = maxBundles;
    set->skinned     = skinned ? 1u : 0u;
    set->materialFilter = RenderSetMaterialFilter_All;

    set->entities         = (Entity*)AllocAligned(maxEntities * sizeof(Entity), 16);
    set->sparseID         = (u32*)AllocAligned(maxEntities * sizeof(u32), 16);
    set->sparseSlots      = (u64*)AllocZeroTLSFGlobal((maxEntities + 63u) >> 6, sizeof(u64));
    set->primitiveGroups  = (PrimitiveGroup*)AllocAligned(maxGroups * sizeof(PrimitiveGroup), 16);
    set->bundlePrimRange = (Range*)AllocZeroTLSFGlobal(maxBundles, sizeof(Range));
    set->bundles          = (const SceneBundle**)AllocZeroTLSFGlobal(maxBundles, sizeof(SceneBundle*));
    set->bundleSlots      = (u64*)AllocZeroTLSFGlobal((maxBundles + 63u) >> 6, sizeof(u64));
	MemSet(set->entities, 0, maxEntities * sizeof(Entity));
	MemSet(set->primitiveGroups, 0, maxGroups * sizeof(PrimitiveGroup));
	MemSet(set->sparseID, 0xFF, maxEntities * sizeof(u32));
}

void RenderSet_Destroy(RenderSet* set)
{
	FreeAligned(set->entities);
    FreeAligned(set->sparseID); 
    DeAllocateTLSFGlobal(set->sparseSlots);     
    FreeAligned(set->primitiveGroups);
	DeAllocateTLSFGlobal(set->bundlePrimRange);
    DeAllocateTLSFGlobal(set->bundles);
	DeAllocateTLSFGlobal(set->bundleSlots);
	MemsetZero(set, sizeof(RenderSet));
}

void RenderSet_SetMaterialFilter(RenderSet* set, RenderSetMaterialFilter filter)
{
    set->materialFilter = (u32)filter;
}

void RenderSet_SetHookScene(RenderSet* set, struct Scene_* scene)
{
    set->hookScene = scene;
}

static bool PrimitiveMatchesMaterialFilter(const RenderSet* set, const SceneBundle* bundle, const APrimitive* primitive)
{
    if (set->materialFilter == RenderSetMaterialFilter_All) return true;

    AMaterialAlphaMode alphaMode = AMaterialAlphaMode_Opaque;
    if (primitive->material < (u32)bundle->numMaterials)
        alphaMode = bundle->materials[primitive->material].alphaMode;

    bool transparent = alphaMode == AMaterialAlphaMode_Blend;
    if (set->materialFilter == RenderSetMaterialFilter_Transparent) return transparent;
    return !transparent;
}

v128f EntityUnpackRotation(u64 packed)
{
    return UnpackQuaternionS16Norm1(packed);
}

u64 EntityPackRotation(v128f rotation)
{
    return PackQuaternionS16NormRet(rotation);
}

v128f EntityUnpackScale01(u64 packed)
{
    return UnpackUnorm16x4(packed);
}

v128f EntityUnpackWorldScale(u64 packed)
{
    v128f scale = VecMulf(EntityUnpackScale01(packed), 10.0f);
    VecSetW(scale, 1.0f);
    return scale;
}

u64 EntityPackWorldScale(v128f scale)
{
    return PackUnorm16x4(VecClamp01(VecMulf(scale, 0.1f)));
}

u64 EntityPackUniformWorldScale(f32 scale)
{
    f32 packedScale = Saturatef32(scale * 0.1f);
    return PackUnorm16x4(VecSet1(packedScale));
}

v128f RenderSet_GroupLocalCenter(const PrimitiveGroup* group)
{
    return VecMulf(VecAdd(group->aabbMin, group->aabbMax), 0.5f);
}

v128f RenderSet_EntityBoundsCenter(const PrimitiveGroup* group, const Entity* entity, v128f rotation, v128f worldScale)
{
    return VecAdd(QMulVec3V(VecMul(RenderSet_GroupLocalCenter(group), worldScale), rotation), entity->position);
}

bool RenderSet_ResolveEntity(RenderSet* set, u32 groupIdx, u32 entityIdx, PrimitiveGroup** outGroup, Entity** outEntity)
{
    if (!set || groupIdx >= set->numGroups) return false;
    PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
    if (entityIdx >= group->numEntities) return false;
    if (outGroup) *outGroup = group;
    if (outEntity) *outEntity = &set->entities[group->entityOffset + entityIdx];
    return true;
}

s32 RenderSet_NodeSpawnOrdinal(const SceneBundle* bundle, s32 nodeIdx)
{
    s32 ordinal = 0;
    for (s32 i = 0; i < bundle->numNodes; i++)
    {
        const ANode* node = &bundle->nodes[i];
        if (node->type != 0 || node->index < 0) continue;
        if (i == nodeIdx) return ordinal;
        ordinal++;
    }
    return -1;
}

bool RenderSet_FindNodeEntity(const RenderSet* set, Range range, u32 meshIndex, u32 sparseIdx,
                              u32* outGroup, u32* outEntity)
{
    for (u32 g = range.start; g < range.start + range.count; g++)
    {
        const PrimitiveGroup* group = &set->primitiveGroups[g];
        if (group->meshIndex != meshIndex) continue;
        for (u32 e = 0; e < group->numEntities; e++)
        {
            if (set->entities[group->entityOffset + e].sparseIdx == sparseIdx)
            {
                *outGroup = g;
                *outEntity = e;
                return true;
            }
        }
    }
    return false;
}

u32 RenderSet_CountTriangles(const RenderSet* set)
{
    u64 triangles = 0u;
    for (u32 i = 0u; i < set->numGroups; i++)
    {
        const PrimitiveGroup* group = &set->primitiveGroups[i];
        triangles += (u64)(group->lodNumIndices[0] / 3u) * group->numEntities;
    }
    return (u32)Minu64(triangles, 0xFFFFFFFFull);
}

u32 RenderSet_AllocateSparseID(RenderSet* set)
{
    s32 sparseIdx = BitsetFindFirstEmpty(set->sparseSlots, (s32)set->maxEntities);
    if (sparseIdx < 0)
    {
        AX_WARN("maximum sparse id reached: %d", set->maxEntities);
        return INVALID_ENTITY;
    }

    BitsetSet(set->sparseSlots, sparseIdx);
    return (u32)sparseIdx;
}

u32 RenderSet_AllocateSparseIDRange(RenderSet* set, int count)
{
    if (count <= 0) return INVALID_ENTITY;

    s32 sparseIdx = BitsetFindEmptyRange(set->sparseSlots, set->maxEntities, (u32)count);
    if (sparseIdx < 0)
    {
        AX_WARN("RenderSet_AllocateSparseIDRange: maximum sparse id reached: %d", set->maxEntities);
        return INVALID_ENTITY;
    }

    BitsetSetRange(set->sparseSlots, sparseIdx, count, true);
    return (u32)sparseIdx;
}

static void FreeSparseIDRange(RenderSet* set, u32 sparseIdx, u32 count)
{
    if (sparseIdx >= set->maxEntities || count == 0u) return;

    if (sparseIdx + count > set->maxEntities)
        count = set->maxEntities - sparseIdx;
    BitsetSetRange(set->sparseSlots, sparseIdx, count, false);
    MemSet(set->sparseID + sparseIdx, 0xFF, count * sizeof(u32));
}

void RenderSet_FreeSparseID(RenderSet* set, u32 sparseIdx)
{
    if (sparseIdx >= set->maxEntities) return;
    BitsetReset(set->sparseSlots, (s32)sparseIdx);
    set->sparseID[sparseIdx] = INVALID_ENTITY;
}

static s32 GetNumPrimitivesOfSceneBundle(const SceneBundle* sceneBundle)
{
    s32 res = 0;
    for (s32 m = 0; m < sceneBundle->numMeshes; m++)
        res += sceneBundle->meshes[m].numPrimitives;
    return res;
}

u32 RenderSet_AddSceneBundle(RenderSet* set, const SceneBundle* sceneBundle, u32 materialOffset)
{
    u32 numNewGroups = GetNumPrimitivesOfSceneBundle(sceneBundle);
    if (set->numGroups + numNewGroups > set->maxGroups)
    {
        AX_WARN("maximum primitive group count reached: %d + %d > %d", set->numGroups, numNewGroups, set->maxGroups);
        return INVALID_BUNDLE;
    }

    // bundle slots are stable handles, the lowest free slot is reused so removing a bundle
    // never shifts the indices of the others. numBundles tracks the highest used slot + 1.
    s32 slot = BitsetFindFirstEmpty(set->bundleSlots, (s32)set->maxBundles);
    if (slot < 0)
    {
        AX_WARN("maximum render bundle count reached: %d", set->maxBundles);
        return INVALID_BUNDLE;
    }

    u32 bundleIdx = (u32)slot;
    BitsetSet(set->bundleSlots, slot);
    if (bundleIdx + 1u > set->numBundles) set->numBundles = bundleIdx + 1u;
    set->bundles[bundleIdx] = sceneBundle;
    u32 primitiveStart = set->numGroups;
    // Skinned primitives deform far outside their bind-pose AABB when they are rigidly bound
    // to a moving bone (a sword in the hand, a helmet on the head, ...). The animation shaders
    // normalize each vertex against PrimitiveGroup.aabbMin/aabbMax, so a per-primitive bind AABB
    // collapses such accessories to a blob once the bone leaves bind pose. Share one skin-wide
    // bound (the union of every primitive) across all groups so accessories normalize against the
    // whole-character reach, matching how the old absolute ANIMATION_MAX_METERS range behaved.
    v128f skinnedMin = VecSet1(FLT_MAX);
    v128f skinnedMax = VecSet1(-FLT_MAX);
    if (set->skinned)
    {
        for (u32 m = 0; m < (u32)sceneBundle->numMeshes; m++)
        {
            const AMesh* mesh = sceneBundle->meshes + m;
            for (u32 p = 0; p < (u32)mesh->numPrimitives; p++)
            {
                const APrimitive* primitive = mesh->primitives + p;
                skinnedMin = VecMin(skinnedMin, VecLoad(primitive->min));
                skinnedMax = VecMax(skinnedMax, VecLoad(primitive->max));
            }
        }

        // Bind-pose bounds do not cover poses that reach past the bind silhouette (an overhead
        // swing, a kick). Inflate symmetrically so those poses stay inside [-1, 1] and avoid
        // clamping; the 13-bit per-axis precision has ample headroom for the slack.
        v128f boundsMargin = VecSetR(2.5f, 1.5f, 2.5f, 0.0f);
        v128f center = VecMulf(VecAdd(skinnedMin, skinnedMax), 0.5f);
        v128f extent = VecMul(VecSub(skinnedMax, skinnedMin), boundsMargin);
        skinnedMin = VecSub(center, extent);
        skinnedMax = VecAdd(center, extent);
    }

    s32 totalPrimitives = 0;
    for (u32 m = 0; m < (u32)sceneBundle->numMeshes; m++)
    {
        const AMesh* mesh = sceneBundle->meshes + m;
        for (u32 p = 0; p < (u32)mesh->numPrimitives; p++)
        {
            const APrimitive* primitive = mesh->primitives + p;
            PrimitiveGroup* group = set->primitiveGroups + primitiveStart + totalPrimitives + (s32)p;
            group->numEntities    = 0;
            group->capacity       = 0;
            group->meshIndex      = (u16)m;
            group->primitiveIndex = (u16)p;
            group->materialIndex  = (u16)(materialOffset + (u32)primitive->material);
            group->bundleIdx      = (u16)INVALID_BUNDLE; // owner-assigned bundle handle, stamped by the caller
            group->entityOffset   = (u16)set->numEntities;
            
            v128f aabbMin = set->skinned ? skinnedMin : VecLoad(primitive->min);
            v128f aabbMax = set->skinned ? skinnedMax : VecLoad(primitive->max);
            PrimitiveGroup_SetAABB(group, aabbMin, aabbMax);
            
            for (u32 lod = 0; lod < MESH_LOD_COUNT; lod++)
            {
                group->lodIndexOffset[lod] = (u32)primitive->lodIndexOffset[lod];
                group->lodNumIndices[lod]  = (u32)primitive->lodNumIndices[lod];
                group->lodVertexOffset[lod] = (u32)primitive->lodVertexOffset[lod];
                group->lodNumVertices[lod] = (u32)primitive->lodNumVertices[lod];
            }
        }
        totalPrimitives += mesh->numPrimitives;
    }
    set->numGroups += totalPrimitives;
    set->bundlePrimRange[bundleIdx].start = primitiveStart;
    set->bundlePrimRange[bundleIdx].count = set->numGroups - primitiveStart;
    return bundleIdx;
}

static u32 FindPrimitiveGroup(RenderSet* set, Range range, u32 meshIndex, u32 primitiveIndex)
{
    for (u32 i = range.start; i < range.start + range.count; i++)
    {
        const PrimitiveGroup* group = set->primitiveGroups + i;
        if (group->meshIndex == meshIndex && group->primitiveIndex == primitiveIndex)
            return i;
    }
    AX_WARN("primitive group couldnt found: %d", primitiveIndex);
    return INVALID_GROUP;
}

static void RebuildSparseToDense(RenderSet* set)
{
    MemSet(set->sparseID, 0xFF, set->maxEntities * sizeof(u32));
    MemSet(set->sparseSlots, 0, ((set->maxEntities + 63u) >> 6) * sizeof(u64));

    for (u32 e = 0; e < set->numEntities; e++)
    {
        u32 sparseIdx = set->entities[e].sparseIdx;
        if (sparseIdx != INVALID_ENTITY && sparseIdx < set->maxEntities)
        {
            BitsetSet(set->sparseSlots, (s32)sparseIdx);
            if (set->sparseID[sparseIdx] == INVALID_ENTITY)
                set->sparseID[sparseIdx] = e;
        }
    }
}

static void SetSparseToDense(RenderSet* set, u32 sparseIdx, u32 denseIdx)
{
    if (sparseIdx == INVALID_ENTITY || sparseIdx >= set->maxEntities) return;

    BitsetSet(set->sparseSlots, (s32)sparseIdx);
    if (set->sparseID[sparseIdx] == INVALID_ENTITY || denseIdx < set->sparseID[sparseIdx])
        set->sparseID[sparseIdx] = denseIdx;
}

static u32 LeaveSpaceForEntities(RenderSet* set, u32 primitiveIdx, u32 numAdded)
{
    PrimitiveGroup* group = &set->primitiveGroups[primitiveIdx];
    const u32 entityStart = group->entityOffset + group->numEntities;
    if (set->numEntities + numAdded > set->maxEntities)
    {
        AX_WARN("maximum entity reached: %d", set->maxEntities);
        return INVALID_ENTITY;
    }

    for (s32 i = (s32)set->numEntities - 1; i >= (s32)entityStart; i--)
    {
        u32 dst = (u32)i + numAdded;
        set->entities[dst] = set->entities[i];

        u32 sparseIdx = set->entities[dst].sparseIdx;
        if (sparseIdx != INVALID_ENTITY && sparseIdx < set->maxEntities && set->sparseID[sparseIdx] == (u32)i)
            set->sparseID[sparseIdx] = dst;
    }

    for (u32 i = primitiveIdx + 1u; i < set->numGroups; i++)
        set->primitiveGroups[i].entityOffset = (u16)(set->primitiveGroups[i].entityOffset + numAdded);

    set->numEntities += numAdded;
    return entityStart;
}

// warning: assumes consecutive sparseID's (sparseX,X+1, X+2)
u32 RenderSet_AddEntities(RenderSet* set, u32 primitiveIdx, u32 numAdded, const Entity* data)
{
    u32 startIdx = LeaveSpaceForEntities(set, primitiveIdx, numAdded);
    if (startIdx == INVALID_ENTITY)
    {
        FreeSparseIDRange(set, data[0].sparseIdx, numAdded);
        return INVALID_ENTITY;
    }
    PrimitiveGroup* group = &set->primitiveGroups[primitiveIdx];
    for (u32 i = 0; i < numAdded; i++)
    {
        u32 denseIdx = startIdx + i;
        set->entities[denseIdx] = data[i];
        set->entities[denseIdx].primitiveIdx = primitiveIdx;
        SetSparseToDense(set, set->entities[denseIdx].sparseIdx, denseIdx);
    }
    group->numEntities += numAdded;
    group->capacity = group->numEntities;
    RenderSet_AddEntitiesCallback(set, primitiveIdx, group->numEntities - numAdded, numAdded);
    return startIdx;
}

u32 RenderSet_AddEntity(RenderSet* set, u32 primitiveIdx, const Entity* data)
{
    return RenderSet_AddEntities(set, primitiveIdx, 1, data);
}

static bool ANodeIsMesh(const ANode* node, bool skinned)
{
    return !(node->type != 0 || node->index < 0 || (skinned && node->skin < 0));
}

// world: staging entities -1 is root
// rootTemp: temp rootNodeTransform. WARNING make SparseID = INVALID_ENTITY if empty
void RendersetAddANodesAsEntities(RenderSet* rs, const ANode* nodes, s32 numNode, 
                                  Entity* world, s32 sparseStart)
{
    (void)sparseStart;
    for (s32 n = 0; n < numNode; n++)
    {
        const ANode* node = nodes + n;
        v128f localPos   = VecLoad(node->translation);
        v128f localRot   = VecNorm(VecLoad(node->rotation));
        v128f localScale = VecLoad(node->scale);

        const Entity* parent = world + node->parent;
        u32 noMesh = (node->type != 0) | (node->index < 0) | (rs->skinned && node->skin < 0);
        Entity* added = world + n;
        v128f parentRot = EntityUnpackRotation(parent->rotation);
        v128f parentScale = EntityUnpackWorldScale(parent->scale);
        added->position = VecAdd(QMulVec3V(VecMul(localPos, parentScale), parentRot), parent->position);
        added->rotation = EntityPackRotation(VecNorm(QMul(localRot, parentRot)));
        added->scale = EntityPackWorldScale(VecMul(localScale, parentScale));
        added->parentIdx = (parent->sparseIdx & 0x00FFFFFFu) | (noMesh << 24);
        added->sparseIdx = INVALID_ENTITY;
    }
}

u32 RenderSet_AddScene(RenderSet* set, u32 bundleIdx, v128f position, v128f rotation, v128f scale, bool wantSkinned)
{
    if (bundleIdx >= set->numBundles || set->bundles[bundleIdx] == NULL)
    {
        AX_WARN("add scene bundle bounds check failed!");
        return 0;
    }

    const Range range = set->bundlePrimRange[bundleIdx];
    const SceneBundle* bundle = set->bundles[bundleIdx];
    const ANode* nodes = bundle->nodes;
    const int numNodes = bundle->numNodes;
    u32 numPrimitives = range.count;
    if (numNodes <= 0 || numPrimitives == 0u)
    {
        AX_WARN("no nodes in bundle to add!");
        return 0;
    }
 
    u32* primitiveCounts = ArenaAllocGlobal(numPrimitives * sizeof(u32));
    MemSet(primitiveCounts, 0, numPrimitives * sizeof(u32));
    Entity* nodeEntities = ArenaAllocGlobal(((u32)numNodes + 1u) * sizeof(Entity));

    u32 meshNodeCount = 0;
    u32 totalPrimAdded = 0;
    for (u32 m = 0; m < (u32)numNodes; m++)
    {
        const ANode* node = nodes + m;
        if (!ANodeIsMesh(node, wantSkinned))
        {
            nodeEntities[m + 1u].primitiveIdx = range.start;
            continue;
        }

        const AMesh* mesh = bundle->meshes + node->index;
        u32 firstGroupIdx = range.start + (u32)mesh->primitiveOffset;
        meshNodeCount++;

        nodeEntities[m + 1u].primitiveIdx = firstGroupIdx;
        for (u32 p = 0; p < (u32)mesh->numPrimitives; p++)
        {
            const APrimitive* primitive = mesh->primitives + p;
            if (!PrimitiveMatchesMaterialFilter(set, bundle, primitive)) continue;

            primitiveCounts[firstGroupIdx + p - range.start]++;
            totalPrimAdded++;
        }
    }

    if (totalPrimAdded == 0u)
    {
        ArenaPopGlobal(((u32)numNodes + 1u) * sizeof(Entity));
        ArenaPopGlobal(numPrimitives * sizeof(u32));
        return 0;
    }

    u32 sparseCount = set->skinned ? meshNodeCount : totalPrimAdded;
    u32 sparseStart = RenderSet_AllocateSparseIDRange(set, sparseCount);
    if (sparseStart == INVALID_ENTITY)
    {
        AX_WARN("maximum entity reached: %d", set->maxEntities);
        ArenaPopGlobal(((u32)numNodes + 1u) * sizeof(Entity));
        ArenaPopGlobal(numPrimitives * sizeof(u32));
        return INVALID_ENTITY;
    }

    u32 insertedBefore = totalPrimAdded;
    for (s32 g = (s32)set->numGroups - 1; g >= (s32)range.start; g--)
    {
        PrimitiveGroup* group = &set->primitiveGroups[g];
        if ((u32)g < range.start + numPrimitives)
        {
            u32 localIdx = (u32)g - range.start;
            insertedBefore -= primitiveCounts[localIdx];
        }

        u32 oldOffset = group->entityOffset;
        u32 newOffset = oldOffset + insertedBefore;
        for (s32 e = (s32)group->numEntities - 1; e >= 0; e--)
        {
            u32 dst = newOffset + (u32)e;
            set->entities[dst] = set->entities[oldOffset + (u32)e];
            set->entities[dst].primitiveIdx = (u32)g;

            u32 sparseIdx = set->entities[dst].sparseIdx;
            if (sparseIdx != INVALID_ENTITY && sparseIdx < set->maxEntities)
                set->sparseID[sparseIdx] = dst;
        }

        group->entityOffset = (u16)newOffset;
    }

    for (u32 p = 0; p < numPrimitives; p++)
        set->primitiveGroups[range.start + p].capacity += primitiveCounts[p];

    set->numEntities += totalPrimAdded;

    rotation = VecNorm(rotation);
    nodeEntities[0].position     = position;
    nodeEntities[0].rotation     = PackQuaternionS16NormRet(rotation);
    nodeEntities[0].scale        = EntityPackWorldScale(scale);
    nodeEntities[0].primitiveIdx = 0;
    nodeEntities[0].sparseIdx    = INVALID_ENTITY;
    RendersetAddANodesAsEntities(set, bundle->nodes, bundle->numNodes, nodeEntities + 1, sparseStart);

    // insert entities
    u32 sparseCursor = 0;
    for (u32 n = 0; n < (u32)numNodes; n++)
    {
        const ANode* node = nodes + n;
        if (!ANodeIsMesh(node, wantSkinned)) continue;

        const AMesh* mesh = bundle->meshes + node->index;
        u32 firstGroupIdx = nodeEntities[n + 1u].primitiveIdx;
        u32 nodeSparseIdx = INVALID_ENTITY;
        if (set->skinned)
            nodeSparseIdx = sparseStart + sparseCursor++;

        for (u32 p = 0; p < (u32)mesh->numPrimitives; p++)
        {
            const APrimitive* primitive = mesh->primitives + p;
            if (!PrimitiveMatchesMaterialFilter(set, bundle, primitive)) continue;

            Entity added = nodeEntities[n + 1u];
            added.primitiveIdx = firstGroupIdx + p;
            added.sparseIdx = set->skinned ? nodeSparseIdx : sparseStart + sparseCursor++;
            PrimitiveGroup* group = &set->primitiveGroups[added.primitiveIdx];
            u32 localIdx = group->numEntities++;
            u32 denseIdx = group->entityOffset + localIdx;
            set->entities[denseIdx] = added;
            SetSparseToDense(set, added.sparseIdx, denseIdx);
            RenderSet_AddEntitiesCallback(set, added.primitiveIdx, localIdx, 1u);
        }
    }

    ArenaPopGlobal(((u32)numNodes + 1u) * sizeof(Entity));
    ArenaPopGlobal(numPrimitives * sizeof(u32));
    return totalPrimAdded;
}

void RenderSet_CompactEntities(RenderSet* set)
{
    u32 oldNumEntities = set->numEntities;
    Entity* noMeshEntities = (Entity*)ArenaAllocGlobal(oldNumEntities * sizeof(Entity));
    u32* groupCounts = (u32*)ArenaAllocGlobal(set->numGroups * sizeof(u32));
    MemSet(groupCounts, 0, set->numGroups * sizeof(u32));

    u32 noMeshCount = 0;
    for (u32 i = 0; i < oldNumEntities; i++)
    {
        Entity entity = set->entities[i];
        bool noMesh = ((entity.parentIdx >> 24u) & ENTITY_FLAG_NOMESH) != 0u || entity.primitiveIdx == INVALID_GROUP;
        if (noMesh && entity.sparseIdx != INVALID_ENTITY)
        {
            noMeshEntities[noMeshCount++] = entity;
        }
    }

    u32 totalDrawable = 0;
    for (u32 groupIdx = 0; groupIdx < set->numGroups; groupIdx++)
    {
        PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
        for (u32 i = 0; i < group->numEntities; i++)
        {
            Entity entity = set->entities[group->entityOffset + i];
            if (entity.sparseIdx != INVALID_ENTITY)
            {
                groupCounts[groupIdx]++;
                totalDrawable++;
            }
        }
    }

    u32 writeEntity = 0;
    for (u32 groupIdx = 0; groupIdx < set->numGroups; groupIdx++)
    {
        PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
        u32 oldOffset = group->entityOffset;
        u32 oldCount = group->numEntities;

        group->entityOffset = (u16)writeEntity;
        for (u32 i = 0; i < oldCount; i++)
        {
            Entity entity = set->entities[oldOffset + i];
            if (entity.sparseIdx == INVALID_ENTITY)
                continue;

            set->entities[writeEntity] = entity;
            set->entities[writeEntity].primitiveIdx = groupIdx;
            writeEntity++;
        }
    }

    for (u32 groupIdx = 0; groupIdx < set->numGroups; groupIdx++)
    {
        PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
        group->numEntities = groupCounts[groupIdx];
        group->capacity = groupCounts[groupIdx];
    }

    MemCopy(&set->entities[writeEntity], noMeshEntities, noMeshCount * sizeof(Entity));
    writeEntity += noMeshCount;
    set->numEntities = writeEntity;
    MemSet(&set->entities[writeEntity], 0, (oldNumEntities - writeEntity) * sizeof(Entity));
    RebuildSparseToDense(set);
    ArenaPopGlobal(set->numGroups * sizeof(u32));
    ArenaPopGlobal(oldNumEntities * sizeof(Entity));
}

void RenderSet_ClearEntities(RenderSet* set)
{
    RenderSet_ClearEntitiesCallback(set);
    MemsetZero(set->entities, set->maxEntities * sizeof(Entity));
    MemSet(set->sparseID, 0xFF, set->maxEntities * sizeof(u32));
    MemsetZero(set->sparseSlots, ((set->maxEntities + 63u) >> 6) * sizeof(u64));

    for (u32 g = 0; g < set->numGroups; g++)
    {
        PrimitiveGroup* group = set->primitiveGroups + g;
        group->entityOffset = 0;
        group->numEntities = 0;
        group->capacity = 0;
    }

    set->numEntities = 0;
}

void RenderSet_Clear(RenderSet* set)
{
    RenderSet_RemoveGroupsCallback(set, 0u, set->numGroups);
    set->numEntities = 0;
    set->numGroups = 0;
    set->numBundles = 0;
    
    MemSet(set->sparseID, 0xFF, set->maxEntities * sizeof(u32));
    MemsetZero(set->sparseSlots, ((set->maxEntities + 63u) >> 6) * sizeof(u64));
    MemsetZero(set->entities, set->maxEntities * sizeof(Entity));
    MemsetZero(set->primitiveGroups, set->maxGroups * sizeof(PrimitiveGroup));
    MemsetZero(set->bundlePrimRange, set->maxBundles * sizeof(Range));
    MemsetZero(set->bundles, set->maxBundles * sizeof(SceneBundle*));
    MemsetZero(set->bundleSlots, ((set->maxBundles + 63u) >> 6) * sizeof(u64));
}

static void ShiftEntitiesLeft(RenderSet* set, u32 firstRemoved, u32 count)
{
    if (count == 0) return;

    u32 oldNumEntities = set->numEntities;
    u32 endRemoved = firstRemoved + count;
    for (u32 i = firstRemoved; i < endRemoved && i < oldNumEntities; i++)
    {
        u32 sparseIdx = set->entities[i].sparseIdx;
        bool validSparse = sparseIdx != INVALID_ENTITY && sparseIdx < set->maxEntities;
        bool sparseMapsToRemoved = validSparse &&
                                   set->sparseID[sparseIdx] >= firstRemoved &&
                                   set->sparseID[sparseIdx] < endRemoved;
        if (sparseMapsToRemoved)
        {
            set->sparseID[sparseIdx] = INVALID_ENTITY;
            BitsetReset(set->sparseSlots, (s32)sparseIdx);
        }
    }

    for (u32 i = endRemoved; i < oldNumEntities; i++)
    {
        u32 dst = i - count;
        set->entities[dst] = set->entities[i];

        u32 sparseIdx = set->entities[dst].sparseIdx;
        bool validSparse = sparseIdx != INVALID_ENTITY && sparseIdx < set->maxEntities;
        bool sparseMapsToMovedSource = validSparse && set->sparseID[sparseIdx] == i;
        bool sparseNeedsReplacement = validSparse && set->sparseID[sparseIdx] == INVALID_ENTITY;
        if (sparseMapsToMovedSource || sparseNeedsReplacement)
        {
            set->sparseID[sparseIdx] = dst;
            BitsetSet(set->sparseSlots, (s32)sparseIdx);
        }
    }

    set->numEntities -= count;
}

u32 RenderSet_RemoveEntities(RenderSet* set, u32 groupIdx, u32 localStartIdx, u32 count)
{
    if (groupIdx >= set->numGroups || count == 0) return 0;

    PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
    if (localStartIdx >= group->numEntities) return 0;

    if (localStartIdx + count > group->numEntities)
        count = group->numEntities - localStartIdx;

    RenderSet_RemoveRangeCallback(set, groupIdx, localStartIdx, count);
    u32 firstRemoved = group->entityOffset + localStartIdx;
    ShiftEntitiesLeft(set, firstRemoved, count);

    group->numEntities -= count;
    group->capacity = group->numEntities;

    for (u32 i = groupIdx + 1; i < set->numGroups; i++)
        set->primitiveGroups[i].entityOffset = (u16)(set->primitiveGroups[i].entityOffset - count);

    return count;
}

u32 RenderSet_RemoveEntity(RenderSet* set, u32 groupIdx, u32 localEntityIdx)
{
    return RenderSet_RemoveEntities(set, groupIdx, localEntityIdx, 1);
}

u32 RenderSet_RemoveSceneBundle(RenderSet* set, u32 bundleIdx)
{
    if (bundleIdx >= set->numBundles) return 0;

    Range range = set->bundlePrimRange[bundleIdx];
    if (range.count == 0) return 0;

    u32 firstGroup = range.start;
    u32 lastGroup = firstGroup + range.count - 1;
    if (firstGroup >= set->numGroups || lastGroup >= set->numGroups) return 0;

    RenderSet_RemoveGroupsCallback(set, firstGroup, range.count);

    u32 firstEntity = set->primitiveGroups[firstGroup].entityOffset;
    PrimitiveGroup* last = &set->primitiveGroups[lastGroup];
    u32 entityCount = (last->entityOffset + last->numEntities) - firstEntity;

    ShiftEntitiesLeft(set, firstEntity, entityCount);

    u32 noMeshRemoved = 0;
    for (s32 e = (s32)set->numEntities - 1; e >= 0; e--)
    {
        Entity* entity = &set->entities[e];
        bool noMesh = ((entity->parentIdx >> 24u) & ENTITY_FLAG_NOMESH) != 0u;
        if (noMesh && entity->primitiveIdx >= firstGroup && entity->primitiveIdx <= lastGroup)
        {
            ShiftEntitiesLeft(set, (u32)e, 1u);
            noMeshRemoved++;
        }
    }

    u32 groupCount = range.count;
    for (u32 i = firstGroup + groupCount; i < set->numGroups; i++)
    {
        PrimitiveGroup moved = set->primitiveGroups[i];
        moved.entityOffset = (u16)(moved.entityOffset - entityCount);
        set->primitiveGroups[i - groupCount] = moved;
        for (u32 e = 0; e < moved.numEntities; e++)
            set->entities[moved.entityOffset + e].primitiveIdx = i - groupCount;
    }
    set->numGroups -= groupCount;

    u32 zeroedGroups = Minu32(groupCount, set->maxGroups - groupCount);
    MemSet(&set->primitiveGroups[set->numGroups], 0, zeroedGroups * sizeof(PrimitiveGroup));

    // free the slot without shifting the others; the primitive groups compacted above, so any
    // live bundle whose range started after the removed one slides down by groupCount.
    BitsetReset(set->bundleSlots, (s32)bundleIdx);
    set->bundles[bundleIdx] = NULL;
    set->bundlePrimRange[bundleIdx] = (Range){0};

    for (u32 i = 0; i < set->numBundles; i++)
    {
        if (i == bundleIdx || set->bundles[i] == NULL) continue;
        if (set->bundlePrimRange[i].start > firstGroup)
            set->bundlePrimRange[i].start -= groupCount;
    }

    while (set->numBundles > 0 && !BitsetGet(set->bundleSlots, (s32)(set->numBundles - 1u)))
        set->numBundles--;

    return entityCount + noMeshRemoved;
}

bool RenderSet_Validate(const RenderSet* set, const char* label)
{
    if (!set) return false;

    bool ok = true;
    u32 countedEntities = 0;
    u32 prevEnd = 0;

    for (u32 b = 0; b < set->numBundles; b++)
    {
        if (set->bundles[b] == NULL) continue;
        Range range = set->bundlePrimRange[b];
        if (range.start + range.count > set->numGroups)
        {
            AX_WARN("RenderSet invalid %s: bundle %d range start=%d count=%d groups=%d",
                    label ? label : "", b, range.start, range.count, set->numGroups);
            ok = false;
        }
    }

    for (u32 g = 0; g < set->numGroups; g++)
    {
        const PrimitiveGroup* group = &set->primitiveGroups[g];
        u32 end = group->entityOffset + group->numEntities;
        if (group->entityOffset < prevEnd || end > set->numEntities)
        {
            AX_WARN("RenderSet invalid %s: group %d entity range offset=%d count=%d prevEnd=%d entities=%d mesh=%d prim=%d mat=%d idx=%d/%d",
                    label ? label : "", g, group->entityOffset, group->numEntities, prevEnd, set->numEntities,
                    group->meshIndex, group->primitiveIndex, group->materialIndex, group->lodIndexOffset[0], group->lodNumIndices[0]);
            ok = false;
        }
        prevEnd = Maxu32(prevEnd, end);
        countedEntities += group->numEntities;

        if (group->numEntities > 0 && group->lodNumIndices[0] == 0)
        {
            AX_WARN("RenderSet invalid %s: group %d has entities but no lod0 indices mesh=%d prim=%d",
                    label ? label : "", g, group->meshIndex, group->primitiveIndex);
            ok = false;
        }
    }

    if (countedEntities > set->numEntities)
    {
        AX_WARN("RenderSet invalid %s: counted drawable entities=%d set entities=%d groups=%d bundles=%d",
                label ? label : "", countedEntities, set->numEntities, set->numGroups, set->numBundles);
        ok = false;
    }

    for (u32 e = 0; e < set->numEntities; e++)
    {
        bool noMesh = ((set->entities[e].parentIdx >> 24u) & ENTITY_FLAG_NOMESH) != 0u ||
                      set->entities[e].primitiveIdx == INVALID_GROUP;
        u32 groupIdx = set->entities[e].primitiveIdx;
        if (noMesh)
        {
            u32 sparseIdx = set->entities[e].sparseIdx;
            if (sparseIdx != INVALID_ENTITY && sparseIdx < set->maxEntities && set->sparseID[sparseIdx] == INVALID_ENTITY)
            {
                AX_WARN("RenderSet invalid %s: no-mesh dense %d sparse %d missing sparseToDense", label ? label : "", e, sparseIdx);
                ok = false;
            }
            continue;
        }

        if (groupIdx >= set->numGroups)
        {
            AX_WARN("RenderSet invalid %s: dense %d primitive=%d groups=%d", label ? label : "", e, groupIdx, set->numGroups);
            ok = false;
            continue;
        }

        const PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
        if (e < group->entityOffset || e >= group->entityOffset + group->numEntities)
        {
            AX_WARN("RenderSet invalid %s: dense %d points group %d range=%d..%d",
                    label ? label : "", e, groupIdx, group->entityOffset, group->entityOffset + group->numEntities);
            ok = false;
        }

        u32 sparseIdx = set->entities[e].sparseIdx;
        if (sparseIdx != INVALID_ENTITY && sparseIdx < set->maxEntities && set->sparseID[sparseIdx] == INVALID_ENTITY)
        {
            AX_WARN("RenderSet invalid %s: dense %d sparse %d missing sparseToDense", label ? label : "", e, sparseIdx);
            ok = false;
        }
    }

    return ok;
}
