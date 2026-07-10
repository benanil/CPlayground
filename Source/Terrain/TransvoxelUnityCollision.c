#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Platform.h"

static bool tCollisionSlotHasIndices(const tChunk* chunk, tMeshDataSlot slot)
{
    if (!chunk || slot >= tMeshDataSlot_Count)
        return false;

    const u32* indices = chunk->validIndices[slot];
    return indices && ArrayLength(indices) > 0;
}

bool tCollisionJobCreate(tWorldState* worldState, int3 chunkPosition, tCollisionBakeFn bake, void* userData, tCollisionJob* outJob)
{
    if (!worldState || !bake || !outJob)
    {
        AX_WARN("transvoxel unity collision job create failed: invalid argument");
        return false;
    }

    tChunk* chunk = tChunkDataFindChunk(&worldState->chunkData, chunkPosition);
    if (!chunk)
    {
        AX_WARN("transvoxel unity collision job create failed: missing chunk");
        return false;
    }

    *outJob = (tCollisionJob){0};
    outJob->chunk = chunk;
    outJob->bake = bake;
    outJob->userData = userData;
    outJob->bakeSlot[tMeshDataSlot_Main] = tCollisionSlotHasIndices(chunk, tMeshDataSlot_Main);

    static const s32 transitionBit[tMeshDataSlot_Count] = { 0, 1, 2, 4, 8, 16, 32 };
    for (u32 i = 1; i < tMeshDataSlot_Count; i++)
        outJob->bakeSlot[i] = tCollisionSlotHasIndices(chunk, (tMeshDataSlot)i) && ((chunk->neighboursMask & transitionBit[i]) != 0);

    return true;
}

bool tCollisionJobExecute(tCollisionJob* job)
{
    if (!job || !job->chunk || !job->bake)
    {
        AX_WARN("transvoxel unity collision job failed: invalid argument");
        return false;
    }

    bool result = true;
    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
    {
        if (job->bakeSlot[i])
            result = job->bake((tMeshDataSlot)i, job->chunk, job->userData) && result;
    }

    return result;
}

static void tCollisionTaskRunJob(void* userData)
{
    tCollisionTaskData* data = (tCollisionTaskData*)userData;
    tCollisionJobExecute(&data->job);
}

bool tCollisionTaskInit(tCollisionTask* task, tWorldState* worldState, JobSystem* jobs, tCollisionBakeFn bake,
                        tChunkLifecycleFn removeOldChunks, tChunkLifecycleFn enablePendingChunks,
                        void* userData, size_t taskCapacity)
{
    if (!task || !worldState || !bake)
    {
        AX_WARN("transvoxel unity collision task init failed: invalid argument");
        return false;
    }

    *task = (tCollisionTask){0};
    task->worldState = worldState;
    task->jobs = jobs;
    task->bake = bake;
    task->removeOldChunks = removeOldChunks;
    task->enablePendingChunks = enablePendingChunks;
    task->userData = userData;
    task->tasks = ArrayCreatePrealloc(tCollisionTaskData, taskCapacity);
    if (!task->tasks)
    {
        AX_WARN("transvoxel unity collision task init failed: allocation failed");
        return false;
    }

    return true;
}

void tCollisionTaskDestroy(tCollisionTask* task)
{
    if (!task)
        return;

    tCollisionTaskEnd(task);
    ArrayDestroy(task->tasks);
    *task = (tCollisionTask){0};
}

static void tCollisionTaskStartOne(tCollisionTask* task, tChunkUpdate update)
{
    if (update.updateType != tChunkUpdateType_Update || !tChunkDataFindChunk(&task->worldState->chunkData, update.chunkPosition))
        return;

    size_t oldCount = ArrayLength(task->tasks);
    if (oldCount >= ArrayCapacity(task->tasks))
    {
        AX_WARN("transvoxel unity collision task skipped: task capacity exhausted");
        return;
    }

    tCollisionTaskData data;
    data.chunkPosition = update.chunkPosition;
    data.handle = 0;
    if (!tCollisionJobCreate(task->worldState, update.chunkPosition, task->bake, task->userData, &data.job))
        return;

    task->tasks[oldCount] = data;
    ArrayFieldSet(task->tasks, ArrayField_Length, oldCount + 1);

    tCollisionTaskData* stored = &task->tasks[oldCount];
    if (task->jobs)
        stored->handle = JobSystem_Execute(task->jobs, tCollisionTaskRunJob, stored);
    if (stored->handle == 0)
        tCollisionTaskRunJob(stored);
}

void tCollisionTaskStart(tCollisionTask* task)
{
    if (!task || !task->worldState)
        return;

    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
    tChunkUpdate* filteredUpdates = task->worldState->chunkData.filteredChunkUpdates;
    for (size_t rev = ArrayLength(filteredUpdates); rev > 0; rev--)
        tCollisionTaskStartOne(task, filteredUpdates[rev - 1]);

    ArrayFieldSet(filteredUpdates, ArrayField_Length, 0);
}

void tCollisionTaskExecute(tCollisionTask* task)
{
    if (!task)
        return;

    for (size_t rev = ArrayLength(task->tasks); rev > 0; rev--)
    {
        size_t i = rev - 1;
        if (!JobSystem_IsJobDone(task->jobs, task->tasks[i].handle))
            continue;

        task->tasks[i] = task->tasks[ArrayLength(task->tasks) - 1];
        ArrayFieldSet(task->tasks, ArrayField_Length, ArrayLength(task->tasks) - 1);
    }
}

void tCollisionTaskEnd(tCollisionTask* task)
{
    if (!task || !task->tasks)
        return;

    JobSystem_Wait(task->jobs);
    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
    if (task->removeOldChunks)
        task->removeOldChunks(task->worldState, task->userData);
    if (task->enablePendingChunks)
        task->enablePendingChunks(task->worldState, task->userData);
}

bool tCollisionTaskIsDone(const tCollisionTask* task)
{
    return !task || !task->tasks || ArrayLength(task->tasks) == 0;
}
