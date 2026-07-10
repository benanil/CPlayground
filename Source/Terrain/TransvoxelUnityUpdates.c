#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Algorithm.h"
#include "Include/Platform.h"

static bool tUpdatesJobPushUpdate(tUpdatesJob* job, tChunkUpdate update)
{
    size_t oldCount = ArrayLength(job->chunkUpdates);
    ArrayPush(job->chunkUpdates, update);
    if (ArrayLength(job->chunkUpdates) != oldCount + 1)
    {
        AX_WARN("transvoxel unity updates job failed: update push failed");
        return false;
    }

    return true;
}

static bool tUpdatesJobAddMapIndex(tUpdatesJob* job, int3 position, size_t index)
{
    if (index > (size_t)INT32_MAX)
    {
        AX_WARN("transvoxel unity updates job failed: too many updates");
        return false;
    }

    s32 indexValue = (s32)index;
    if (!HMInsertOrAssign(&job->chunkUpdatesMap, tChunkPositionKey(position), &indexValue))
    {
        AX_WARN("transvoxel unity updates job failed: map index insert failed");
        return false;
    }

    return true;
}

static bool tUpdatesJobSetActive(tUpdatesJob* job, u64 locCode, bool active)
{
    if (!active)
        return HMErase(&job->activeNodes, locCode);

    u8 value = 1u;
    if (!HMInsertOrAssign(&job->activeNodes, locCode, &value))
    {
        AX_WARN("transvoxel unity updates job failed: active node insert failed");
        return false;
    }

    return true;
}

static bool tUpdatesJobSetNeighbourMask(tUpdatesJob* job, int3 position, s32 mask)
{
    if (!HMInsertOrAssign(&job->activeNodeNeighbours, tChunkPositionKey(position), &mask))
    {
        AX_WARN("transvoxel unity updates job failed: neighbour mask insert failed");
        return false;
    }

    return true;
}

static s32 tUpdatesJobGetNeighbourMask(tUpdatesJob* job, int3 position)
{
    s32* value = (s32*)HMFind(&job->activeNodeNeighbours, tChunkPositionKey(position));
    if (!value)
        return 0;

    return *value;
}

static bool tUpdatesJobCanRender(tUpdatesJob* job, tOctreeNode node)
{
    if (node.depth == 0)
        return true;

    f32 distX = Absf32((f32)node.position.x - job->targetPosition.x);
    f32 distY = Absf32((f32)node.position.y - job->targetPosition.y);
    f32 distZ = Absf32((f32)node.position.z - job->targetPosition.z);
    f32 minDist = Maxf32(distX, Maxf32(distY, distZ));
    f32 compareDist = (f32)job->octree->leafSize * 1.5f * (f32)(1 << node.depth);
    return minDist > compareDist;
}

static bool tUpdatesJobAddMergedLeavesUpdates(tUpdatesJob* job, tOctreeNode fromNode)
{
    for (u32 i = 0; i < 8u; i++)
    {
        tOctreeNode child;
        if (!tOctreeGetChild(job->octree, fromNode, i, &child))
        {
            AX_WARN("transvoxel unity updates job failed: missing child during merge");
            return false;
        }

        if (tOctreeNodeHasChildren(job->octree, child))
        {
            if (!tUpdatesJobAddMergedLeavesUpdates(job, child))
                return false;
        }
        else
        {
            tChunkUpdate update;
            update.updateType = tChunkUpdateType_Remove;
            update.chunkPosition = child.position;
            update.lod = child.depth;
            update.neighboursMask = 0;
            if (!tUpdatesJobPushUpdate(job, update))
                return false;

            tUpdatesJobSetActive(job, child.locCode, false);
            HMErase(&job->activeNodeNeighbours, tChunkPositionKey(child.position));
        }

        if (!tOctreeRemoveNode(job->octree, child.locCode))
            AX_WARN("transvoxel unity updates job failed: child remove failed");
    }

    return true;
}

static bool tUpdatesJobGetChunkUpdates(tUpdatesJob* job, tOctreeNode fromNode)
{
    if (tUpdatesJobCanRender(job, fromNode))
    {
        if (!HMContains(&job->activeNodes, fromNode.locCode))
        {
            tChunkUpdate update;
            update.updateType = tChunkUpdateType_Create;
            update.chunkPosition = fromNode.position;
            update.lod = fromNode.depth;
            update.neighboursMask = 0;

            if (tOctreeNodeHasChildren(job->octree, fromNode))
            {
                if (!tUpdatesJobAddMergedLeavesUpdates(job, fromNode))
                    return false;
            }

            size_t updateIndex = ArrayLength(job->chunkUpdates);
            if (!tUpdatesJobPushUpdate(job, update))
                return false;
            if (!tUpdatesJobAddMapIndex(job, update.chunkPosition, updateIndex))
                return false;
            if (!tUpdatesJobSetActive(job, fromNode.locCode, true))
                return false;
            if (!tUpdatesJobSetNeighbourMask(job, update.chunkPosition, 0))
                return false;
        }

        return true;
    }

    if (HMContains(&job->activeNodes, fromNode.locCode))
    {
        tChunkUpdate update;
        update.updateType = tChunkUpdateType_Remove;
        update.chunkPosition = fromNode.position;
        update.lod = fromNode.depth;
        update.neighboursMask = 0;
        if (!tUpdatesJobPushUpdate(job, update))
            return false;

        tUpdatesJobSetActive(job, fromNode.locCode, false);
        HMErase(&job->activeNodeNeighbours, tChunkPositionKey(fromNode.position));
    }

    if (!tOctreeNodeHasChildren(job->octree, fromNode))
    {
        if (!tOctreeSplitNode(job->octree, fromNode))
            return false;
    }

    for (u32 i = 0; i < 8u; i++)
    {
        tOctreeNode child;
        if (!tOctreeGetChild(job->octree, fromNode, i, &child))
        {
            AX_WARN("transvoxel unity updates job failed: missing child during traversal");
            return false;
        }

        if (!tUpdatesJobGetChunkUpdates(job, child))
            return false;
    }

    return true;
}

static bool tUpdatesJobUpsertTransitionUpdate(tUpdatesJob* job, tOctreeNode otherNode, s32 otherNeighboursMask)
{
    u64 key = tChunkPositionKey(otherNode.position);
    s32* updateIndex = (s32*)HMFind(&job->chunkUpdatesMap, key);
    if (!updateIndex)
    {
        tChunkUpdate transitionUpdate;
        transitionUpdate.chunkPosition = otherNode.position;
        transitionUpdate.lod = otherNode.depth;
        transitionUpdate.updateType = tChunkUpdateType_Update;
        transitionUpdate.neighboursMask = otherNeighboursMask;

        size_t index = ArrayLength(job->chunkUpdates);
        if (!tUpdatesJobPushUpdate(job, transitionUpdate))
            return false;
        return tUpdatesJobAddMapIndex(job, transitionUpdate.chunkPosition, index);
    }

    if (*updateIndex < 0 || (size_t)*updateIndex >= ArrayLength(job->chunkUpdates))
    {
        AX_WARN("transvoxel unity updates job failed: update index out of range");
        return false;
    }

    job->chunkUpdates[*updateIndex].neighboursMask = otherNeighboursMask;
    return true;
}

static bool tUpdatesJobBuildTransitionMasks(tUpdatesJob* job)
{
    static const int3 directions[6] = {
        {-1,  0,  0}, { 0, -1,  0}, { 0,  0, -1},
        { 1,  0,  0}, { 0,  1,  0}, { 0,  0,  1}
    };

    tOctreeNode root;
    if (!tOctreeRootNode(job->octree, &root))
        return false;

    tIntBox worldBox = tIntBoxFromExtents(job->octree->rootPosition, root.extents);
    size_t updateCount = ArrayLength(job->chunkUpdates);
    for (size_t rev = updateCount; rev > 0; rev--)
    {
        size_t i = rev - 1;
        tChunkUpdate update = job->chunkUpdates[i];
        if (update.updateType == tChunkUpdateType_Remove)
            continue;

        s32 neighboursMask = 0;
        s32 halfLeafSize = job->octree->leafSize >> 1;
        for (u32 j = 0; j < 6u; j++)
        {
            int3 offset = I3MulI(directions[j], (halfLeafSize << update.lod) + 5);
            int3 pos = I3Add(update.chunkPosition, offset);
            if (!tIntBoxContains(worldBox, pos))
                continue;

            tOctreeNode otherNode;
            if (!tOctreeGetNodeAt(job->octree, pos, &otherNode))
                return false;

            s32 neighbourBit = 1 << j;
            s32 neighbourBitOpposite = ((neighbourBit << 3) | (neighbourBit >> 3)) & 0x3F;
            if (otherNode.depth < update.lod)
                neighboursMask |= neighbourBit;

            s32 desiredOtherNeighbourBit = 0;
            if (otherNode.depth > update.lod)
                desiredOtherNeighbourBit = neighbourBitOpposite;

            s32 otherNeighboursMask = tUpdatesJobGetNeighbourMask(job, otherNode.position);
            s32 isolatedOtherNeighbourBit = otherNeighboursMask & neighbourBitOpposite;
            if (isolatedOtherNeighbourBit != desiredOtherNeighbourBit)
            {
                otherNeighboursMask ^= neighbourBitOpposite;
                if (!tUpdatesJobUpsertTransitionUpdate(job, otherNode, otherNeighboursMask))
                    return false;
                if (!tUpdatesJobSetNeighbourMask(job, otherNode.position, otherNeighboursMask))
                    return false;
            }
        }

        if (!tUpdatesJobSetNeighbourMask(job, update.chunkPosition, neighboursMask))
            return false;

        update.neighboursMask = neighboursMask;
        job->chunkUpdates[i] = update;
    }

    return true;
}

static int tUpdatesJobCompareUpdates(const void* a, const void* b)
{
    const tChunkUpdate* x = (const tChunkUpdate*)a;
    const tChunkUpdate* y = (const tChunkUpdate*)b;
    return tChunkUpdateCompareLODDescending(*x, *y);
}

bool tUpdatesJobExecute(tUpdatesJob* job)
{
    if (!job || !job->octree || !job->chunkUpdates)
    {
        AX_WARN("transvoxel unity updates job failed: invalid argument");
        return false;
    }

    tOctreeNode root;
    if (!tOctreeRootNode(job->octree, &root))
        return false;

    if (!tUpdatesJobGetChunkUpdates(job, root))
        return false;
    if (!tUpdatesJobBuildTransitionMasks(job))
        return false;

    size_t updateCount = ArrayLength(job->chunkUpdates);
    if (updateCount > 1)
        QuickSort(job->chunkUpdates, 0, (int)updateCount - 1, sizeof(tChunkUpdate), tUpdatesJobCompareUpdates);

    return true;
}

bool tUpdatesJobInit(tUpdatesJob* job, tChunkData* chunkData, u32 reserveCount)
{
    if (!job || !chunkData)
    {
        AX_WARN("transvoxel unity updates job init failed: invalid argument");
        return false;
    }

    *job = (tUpdatesJob){0};
    job->octree = &chunkData->chunkTree;
    job->chunkUpdates = chunkData->chunkUpdates;
    job->activeNodes = HMCreate(reserveCount, sizeof(u8));
    job->activeNodeNeighbours = HMCreate(reserveCount, sizeof(s32));
    job->chunkUpdatesMap = HMCreate(reserveCount, sizeof(s32));
    return true;
}

void tUpdatesJobDestroy(tUpdatesJob* job)
{
    if (!job)
        return;

    HMDestroy(&job->activeNodes);
    HMDestroy(&job->activeNodeNeighbours);
    HMDestroy(&job->chunkUpdatesMap);
    *job = (tUpdatesJob){0};
}

void tUpdatesJobClearFrame(tUpdatesJob* job)
{
    if (!job)
        return;

    if (job->chunkUpdates)
        ArrayFieldSet(job->chunkUpdates, ArrayField_Length, 0);
    HMClear(&job->chunkUpdatesMap);
}
