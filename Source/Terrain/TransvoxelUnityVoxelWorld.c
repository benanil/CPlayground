#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Platform.h"

static void tUpdatesTaskRunJob(void* userData)
{
    tUpdatesTask* task = (tUpdatesTask*)userData;
    tUpdatesJobExecute(&task->job);
}

bool tUpdatesTaskInit(tUpdatesTask* task, tChunkData* chunkData, JobSystem* jobs, tTargetPositionFn targetPosition,
                      void* userData, u32 reserveCount)
{
    if (!task || !chunkData || !targetPosition)
    {
        AX_WARN("transvoxel unity updates task init failed: invalid argument");
        return false;
    }

    *task = (tUpdatesTask){0};
    task->jobs = jobs;
    task->targetPosition = targetPosition;
    task->userData = userData;
    return tUpdatesJobInit(&task->job, chunkData, reserveCount);
}

void tUpdatesTaskDestroy(tUpdatesTask* task)
{
    if (!task)
        return;

    tUpdatesTaskEnd(task);
    tUpdatesJobDestroy(&task->job);
    *task = (tUpdatesTask){0};
}

void tUpdatesTaskStart(tUpdatesTask* task)
{
    if (!task || !task->targetPosition)
        return;

    task->job.targetPosition = task->targetPosition(task->userData);
    tUpdatesJobClearFrame(&task->job);
    task->handle = 0;
    if (task->jobs)
        task->handle = JobSystem_Execute(task->jobs, tUpdatesTaskRunJob, task);
    if (task->handle == 0)
        tUpdatesTaskRunJob(task);
}

void tUpdatesTaskExecute(tUpdatesTask* task)
{
    (void)task;
}

void tUpdatesTaskEnd(tUpdatesTask* task)
{
    if (!task)
        return;

    JobSystem_WaitJob(task->jobs, task->handle);
    task->handle = 0;
}

bool tUpdatesTaskIsDone(const tUpdatesTask* task)
{
    return !task || JobSystem_IsJobDone(task->jobs, task->handle);
}

void tMemoryManagerInit(tMemoryManager* manager)
{
    if (manager && manager->init)
        manager->init(manager->userData);
}

void tMemoryManagerFree(tMemoryManager* manager)
{
    if (manager && manager->free)
        manager->free(manager->userData);
}

void tChunkUtilsRemoveOldChunks(tWorldState* worldState, void* userData)
{
    (void)userData;
    if (!worldState)
        return;

    int3* chunksToRemove = worldState->chunkData.chunksToRemove;
    for (size_t i = 0; i < ArrayLength(chunksToRemove); i++)
    {
        f32* densityData = NULL;
        if (tDensityDataTakeData(&worldState->densityData, chunksToRemove[i], &densityData) && densityData)
            tPoolsAddDensityData(&worldState->pools, densityData);
        tChunkDataRemoveChunk(&worldState->chunkData, chunksToRemove[i]);
    }

    ArrayFieldSet(chunksToRemove, ArrayField_Length, 0);
}

void tChunkUtilsEnablePendingChunks(tWorldState* worldState, void* userData)
{
    (void)userData;
    if (!worldState || !worldState->chunkData.pendingChunks)
        return;

    ArrayFieldSet(worldState->chunkData.pendingChunks, ArrayField_Length, 0);
}

bool tVoxelWorldInit(tVoxelWorld* world, const tVoxelWorldDesc* desc)
{
    if (!world || !desc || !desc->densityGenerator || !desc->materialGenerator || !desc->targetPosition || !desc->bakeCollision)
    {
        AX_WARN("transvoxel unity voxel world init failed: invalid argument");
        return false;
    }

    tMesherFn mesh = desc->mesh;
    if (!mesh)
        mesh = tTransvoxelMesherMesh;

    *world = (tVoxelWorld){0};
    world->desc = *desc;
    world->desc.mesh = mesh;
    tMemoryManagerInit(&world->desc.memory);

    if (!tWorldStateInit(&world->worldState, desc->settings, desc->reserveChunks, desc->updateCapacity,
                         desc->poolCount, desc->meshVertexCapacity, desc->meshIndexCapacity, desc->meshSecondaryVertexCapacity))
        goto fail;

    if (!tTaskSchedulerInit(&world->scheduler, 8)) goto fail;
    if (!tUpdatesTaskInit(&world->updatesTask, &world->worldState.chunkData, desc->jobs, desc->targetPosition, desc->userData, desc->reserveChunks)) goto fail;
    if (!tDensityTaskInit(&world->densityTask, desc->settings, &world->worldState, desc->densityGenerator, desc->jobs, desc->updateCapacity)) goto fail;
    if (!tUniformityTaskInit(&world->uniformityTask, &world->worldState, desc->jobs, desc->updateCapacity)) goto fail;
    if (!tFilterTaskInit(&world->filterTask, &world->worldState, desc->jobs)) goto fail;
    if (!tMesherTaskInit(&world->mesherTask, desc->settings, &world->worldState, desc->densityGenerator, desc->jobs, mesh, desc->userData, desc->updateCapacity)) goto fail;
    if (!tMaterialTaskInit(&world->materialTask, desc->settings, &world->worldState, desc->materialGenerator, desc->jobs, desc->updateCapacity)) goto fail;
    if (!tMeshCopyTaskInit(&world->meshCopyTask, &world->worldState, desc->jobs, desc->updateCapacity)) goto fail;

    tChunkLifecycleFn removeOld = desc->removeOldChunks;
    tChunkLifecycleFn enablePending = desc->enablePendingChunks;
    if (!removeOld) removeOld = tChunkUtilsRemoveOldChunks;
    if (!enablePending) enablePending = tChunkUtilsEnablePendingChunks;
    if (!tCollisionTaskInit(&world->collisionTask, &world->worldState, desc->jobs, desc->bakeCollision,
                            removeOld, enablePending, desc->userData, desc->updateCapacity)) goto fail;

    return true;

fail:
    tVoxelWorldDestroy(world);
    return false;
}

void tVoxelWorldDestroy(tVoxelWorld* world)
{
    if (!world)
        return;

    tVoxelWorldStop(world);
    tCollisionTaskDestroy(&world->collisionTask);
    tMeshCopyTaskDestroy(&world->meshCopyTask);
    tMaterialTaskDestroy(&world->materialTask);
    tMesherTaskDestroy(&world->mesherTask);
    tUniformityTaskDestroy(&world->uniformityTask);
    tDensityTaskDestroy(&world->densityTask);
    tUpdatesTaskDestroy(&world->updatesTask);
    tTaskSchedulerDestroy(&world->scheduler);
    tWorldStateDestroy(&world->worldState);
    tMemoryManagerFree(&world->desc.memory);
    *world = (tVoxelWorld){0};
}

// tTask adapters: the scheduler drives every stage through void*/const void*
// function pointers, so each task type gets a thin cast shim (ITask in the C# original).
#define T_VOXEL_WORLD_TASK_ADAPTERS(name, type)                                                             \
    static void tVW##name##Start(void* task)         { t##name##TaskStart((type*)task); }                   \
    static void tVW##name##Execute(void* task)       { t##name##TaskExecute((type*)task); }                 \
    static void tVW##name##End(void* task)           { t##name##TaskEnd((type*)task); }                     \
    static bool tVW##name##IsDone(const void* task)  { return t##name##TaskIsDone((const type*)task); }

T_VOXEL_WORLD_TASK_ADAPTERS(Updates,    tUpdatesTask)
T_VOXEL_WORLD_TASK_ADAPTERS(Density,    tDensityTask)
T_VOXEL_WORLD_TASK_ADAPTERS(Uniformity, tUniformityTask)
T_VOXEL_WORLD_TASK_ADAPTERS(Filter,     tFilterTask)
T_VOXEL_WORLD_TASK_ADAPTERS(Mesher,     tMesherTask)
T_VOXEL_WORLD_TASK_ADAPTERS(Material,   tMaterialTask)
T_VOXEL_WORLD_TASK_ADAPTERS(MeshCopy,   tMeshCopyTask)
T_VOXEL_WORLD_TASK_ADAPTERS(Collision,  tCollisionTask)

#define T_VOXEL_WORLD_TASK(name, taskPtr) \
    (tTask){ (taskPtr), tVW##name##Start, tVW##name##Execute, tVW##name##End, tVW##name##IsDone }

bool tVoxelWorldStart(tVoxelWorld* world)
{
    if (!world)
        return false;

    tTask tasks[8];
    tasks[0] = T_VOXEL_WORLD_TASK(Updates,    &world->updatesTask);
    tasks[1] = T_VOXEL_WORLD_TASK(Density,    &world->densityTask);
    tasks[2] = T_VOXEL_WORLD_TASK(Uniformity, &world->uniformityTask);
    tasks[3] = T_VOXEL_WORLD_TASK(Filter,     &world->filterTask);
    tasks[4] = T_VOXEL_WORLD_TASK(Mesher,     &world->mesherTask);
    tasks[5] = T_VOXEL_WORLD_TASK(Material,   &world->materialTask);
    tasks[6] = T_VOXEL_WORLD_TASK(MeshCopy,   &world->meshCopyTask);
    tasks[7] = T_VOXEL_WORLD_TASK(Collision,  &world->collisionTask);
    return tTaskSchedulerStartTasks(&world->scheduler, tasks, 8);
}

void tVoxelWorldUpdate(tVoxelWorld* world)
{
    if (world)
        tTaskSchedulerProcessTasks(&world->scheduler);
}

void tVoxelWorldStop(tVoxelWorld* world)
{
    if (world)
        tTaskSchedulerEndTasks(&world->scheduler);
}
