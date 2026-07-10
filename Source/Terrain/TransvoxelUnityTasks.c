#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Memory.h"
#include "Include/Platform.h"

static void tDensityTaskRunJob(void* userData)
{
    tDensityTaskData* data = (tDensityTaskData*)userData;
    tDensityJobExecute(&data->job);
}

static void tDensityTaskFinish(tDensityTask* task, tDensityTaskData* data)
{
	tDensityDataStoreDataUnchecked(&task->worldState->densityData, data->chunkPosition, data->job.densityData);
    data->job.densityData = NULL;
}

bool tDensityTaskInit(tDensityTask* task, tWorldSettings settings, tWorldState* worldState,
                      const tDensityGenerator* generator, JobSystem* jobs, size_t taskCapacity)
{
    if (!task || !worldState || !generator)
    {
        AX_WARN("transvoxel unity density task init failed: invalid argument");
        return false;
    }

    *task = (tDensityTask){0};
    task->settings = settings;
    task->worldState = worldState;
    task->generator = generator;
    task->jobs = jobs;
    task->tasks = ArrayCreatePrealloc(tDensityTaskData, taskCapacity);
    if (!task->tasks)
    {
        AX_WARN("transvoxel unity density task init failed: allocation failed");
        return false;
    }

    return true;
}

void tDensityTaskDestroy(tDensityTask* task)
{
    if (!task)
        return;

    tDensityTaskEnd(task);
    ArrayDestroy(task->tasks);
    *task = (tDensityTask){0};
}

static void tDensityTaskStartOne(tDensityTask* task, tChunkUpdate update)
{
    if (update.updateType != tChunkUpdateType_Create)
        return;

    tDensityTaskData data;
    data.chunkPosition = update.chunkPosition;
    data.handle = 0;
    if (!tDensityJobCreate(&task->settings, &task->worldState->pools, task->generator, update, &data.job))
        return;

    size_t oldCount = ArrayLength(task->tasks);
    if (oldCount >= ArrayCapacity(task->tasks))
    {
        AX_WARN("transvoxel unity density task skipped: task capacity exhausted");
        tPoolsAddDensityData(&task->worldState->pools, data.job.densityData);
        return;
    }

    task->tasks[oldCount] = data;
    ArrayFieldSet(task->tasks, ArrayField_Length, oldCount + 1);

    tDensityTaskData* stored = &task->tasks[oldCount];
    if (task->jobs)
        stored->handle = JobSystem_Execute(task->jobs, tDensityTaskRunJob, stored);

    if (stored->handle == 0)
        tDensityTaskRunJob(stored);
}

void tDensityTaskStart(tDensityTask* task)
{
    if (!task || !task->worldState)
    {
        AX_WARN("transvoxel unity density task start failed: invalid task");
        return;
    }

    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
    tChunkUpdate* chunkUpdates = task->worldState->chunkData.chunkUpdates;
    for (size_t rev = ArrayLength(chunkUpdates); rev > 0; rev--)
        tDensityTaskStartOne(task, chunkUpdates[rev - 1]);
}

void tDensityTaskExecute(tDensityTask* task)
{
    if (!task)
        return;

    for (size_t rev = ArrayLength(task->tasks); rev > 0; rev--)
    {
        size_t i = rev - 1;
        tDensityTaskData* data = &task->tasks[i];
        if (!JobSystem_IsJobDone(task->jobs, data->handle))
            continue;

        tDensityTaskFinish(task, data);
        task->tasks[i] = task->tasks[ArrayLength(task->tasks) - 1];
        ArrayFieldSet(task->tasks, ArrayField_Length, ArrayLength(task->tasks) - 1);
    }
}

void tDensityTaskEnd(tDensityTask* task)
{
    if (!task || !task->tasks)
        return;

    JobSystem_Wait(task->jobs);
    for (size_t i = 0; i < ArrayLength(task->tasks); i++)
        tDensityTaskFinish(task, &task->tasks[i]);

    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
}

bool tDensityTaskIsDone(const tDensityTask* task)
{
    return !task || !task->tasks || ArrayLength(task->tasks) == 0;
}

static void tUniformityTaskRunJob(void* userData)
{
    tUniformityTaskData* data = (tUniformityTaskData*)userData;
    tUniformityJobExecute(&data->job);
}

static void tUniformityTaskFinish(tUniformityTask* task, tUniformityTaskData* data)
{
    if (!task || !data)
        return;

    if (data->job.index >= ArrayLength(task->worldState->chunkData.chunkUniformState))
        return;

    if (task->worldState->chunkData.chunkUniformState[data->job.index])
    {
        f32* densityData = NULL;
        if (tDensityDataTakeData(&task->worldState->densityData, data->chunkUpdate.chunkPosition, &densityData) && densityData)
            tPoolsAddDensityData(&task->worldState->pools, densityData);
    }
}

bool tUniformityTaskInit(tUniformityTask* task, tWorldState* worldState, JobSystem* jobs, size_t taskCapacity)
{
    if (!task || !worldState)
    {
        AX_WARN("transvoxel unity uniformity task init failed: invalid argument");
        return false;
    }

    *task = (tUniformityTask){0};
    task->worldState = worldState;
    task->jobs = jobs;
    task->tasks = ArrayCreatePrealloc(tUniformityTaskData, taskCapacity);
    if (!task->tasks)
    {
        AX_WARN("transvoxel unity uniformity task init failed: allocation failed");
        return false;
    }

    return true;
}

void tUniformityTaskDestroy(tUniformityTask* task)
{
    if (!task)
        return;

    tUniformityTaskEnd(task);
    ArrayDestroy(task->tasks);
    *task = (tUniformityTask){0};
}

void tUniformityTaskStart(tUniformityTask* task)
{
    if (!task || !task->worldState)
    {
        AX_WARN("transvoxel unity uniformity task start failed: invalid task");
        return;
    }

    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
    tChunkUpdate* chunkUpdates = task->worldState->chunkData.chunkUpdates;
    size_t updateCount = ArrayLength(chunkUpdates);
    while (ArrayLength(task->worldState->chunkData.chunkUniformState) < updateCount)
    {
        bool value = false;
        ArrayPush(task->worldState->chunkData.chunkUniformState, value);
    }

    for (size_t i = 0; i < updateCount; i++)
    {
        tChunkUpdate chunkUpdate = chunkUpdates[i];
        if (chunkUpdate.updateType != tChunkUpdateType_Create)
            continue;

        tUniformityTaskData data;
        data.chunkUpdate = chunkUpdate;
        data.handle = 0;
        if (!tUniformityJobCreate(task->worldState, chunkUpdate.chunkPosition, (u32)i, &data.job))
            continue;

        size_t oldCount = ArrayLength(task->tasks);
        if (oldCount >= ArrayCapacity(task->tasks))
        {
            AX_WARN("transvoxel unity uniformity task skipped: task capacity exhausted");
            continue;
        }

        task->tasks[oldCount] = data;
        ArrayFieldSet(task->tasks, ArrayField_Length, oldCount + 1);

        tUniformityTaskData* stored = &task->tasks[oldCount];
        if (task->jobs)
            stored->handle = JobSystem_Execute(task->jobs, tUniformityTaskRunJob, stored);
        if (stored->handle == 0)
            tUniformityTaskRunJob(stored);
    }
}

void tUniformityTaskExecute(tUniformityTask* task)
{
    if (!task)
        return;

    for (size_t rev = ArrayLength(task->tasks); rev > 0; rev--)
    {
        size_t i = rev - 1;
        tUniformityTaskData* data = &task->tasks[i];
        if (!JobSystem_IsJobDone(task->jobs, data->handle))
            continue;

        tUniformityTaskFinish(task, data);
        task->tasks[i] = task->tasks[ArrayLength(task->tasks) - 1];
        ArrayFieldSet(task->tasks, ArrayField_Length, ArrayLength(task->tasks) - 1);
    }
}

void tUniformityTaskEnd(tUniformityTask* task)
{
    if (!task || !task->tasks)
        return;

    JobSystem_Wait(task->jobs);
    for (size_t i = 0; i < ArrayLength(task->tasks); i++)
        tUniformityTaskFinish(task, &task->tasks[i]);

    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
}

bool tUniformityTaskIsDone(const tUniformityTask* task)
{
    return !task || !task->tasks || ArrayLength(task->tasks) == 0;
}

static void tFilterTaskRunJob(void* userData)
{
    tFilterTask* task = (tFilterTask*)userData;
    tFilterJobExecute(&task->job);
}

bool tFilterTaskInit(tFilterTask* task, tWorldState* worldState, JobSystem* jobs)
{
    if (!task || !worldState)
    {
        AX_WARN("transvoxel unity filter task init failed: invalid argument");
        return false;
    }

    *task = (tFilterTask){0};
    task->worldState = worldState;
    task->jobs = jobs;
    return tFilterJobCreate(worldState, &task->job);
}

void tFilterTaskStart(tFilterTask* task)
{
    if (!task)
        return;

    task->handle = 0;
    if (task->jobs)
        task->handle = JobSystem_Execute(task->jobs, tFilterTaskRunJob, task);
    if (task->handle == 0)
        tFilterTaskRunJob(task);
}

void tFilterTaskExecute(tFilterTask* task)
{
    (void)task;
}

void tFilterTaskEnd(tFilterTask* task)
{
    if (!task)
        return;

    JobSystem_WaitJob(task->jobs, task->handle);
    if (task->job.chunkUpdates)
        ArrayFieldSet(task->job.chunkUpdates, ArrayField_Length, 0);
    task->handle = 0;
}

bool tFilterTaskIsDone(const tFilterTask* task)
{
    return !task || JobSystem_IsJobDone(task->jobs, task->handle);
}

static void tMaterialTaskRunJob(void* userData)
{
    tMaterialTaskData* data = (tMaterialTaskData*)userData;
    tMaterialJobExecute(&data->job);
}

bool tMaterialTaskInit(tMaterialTask* task, tWorldSettings settings, tWorldState* worldState,
                       const tMaterialGenerator* generator, JobSystem* jobs, size_t taskCapacity)
{
    if (!task || !worldState || !generator)
    {
        AX_WARN("transvoxel unity material task init failed: invalid argument");
        return false;
    }

    *task = (tMaterialTask){0};
    task->settings = settings;
    task->worldState = worldState;
    task->generator = generator;
    task->jobs = jobs;
    task->tasks = ArrayCreatePrealloc(tMaterialTaskData, taskCapacity);
    if (!task->tasks)
    {
        AX_WARN("transvoxel unity material task init failed: allocation failed");
        return false;
    }

    return true;
}

void tMaterialTaskDestroy(tMaterialTask* task)
{
    if (!task)
        return;

    tMaterialTaskEnd(task);
    ArrayDestroy(task->tasks);
    *task = (tMaterialTask){0};
}

static void tMaterialTaskStartOne(tMaterialTask* task, tChunkUpdate update)
{
    if (update.updateType != tChunkUpdateType_Create || !tChunkDataFindChunk(&task->worldState->chunkData, update.chunkPosition))
        return;

    size_t oldCount = ArrayLength(task->tasks);
    if (oldCount >= ArrayCapacity(task->tasks))
    {
        AX_WARN("transvoxel unity material task skipped: task capacity exhausted");
        return;
    }

    tMaterialTaskData data;
    data.handle = 0;
    data.chunkPosition = update.chunkPosition;
    if (!tMaterialJobCreate(&task->settings, task->worldState, task->generator, update, &data.job))
        return;

    task->tasks[oldCount] = data;
    ArrayFieldSet(task->tasks, ArrayField_Length, oldCount + 1);

    tMaterialTaskData* stored = &task->tasks[oldCount];
    if (task->jobs)
        stored->handle = JobSystem_Execute(task->jobs, tMaterialTaskRunJob, stored);
    if (stored->handle == 0)
        tMaterialTaskRunJob(stored);
}

void tMaterialTaskStart(tMaterialTask* task)
{
    if (!task || !task->worldState)
        return;

    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
    tChunkUpdate* filteredUpdates = task->worldState->chunkData.filteredChunkUpdates;
    for (size_t rev = ArrayLength(filteredUpdates); rev > 0; rev--)
        tMaterialTaskStartOne(task, filteredUpdates[rev - 1]);
}

void tMaterialTaskExecute(tMaterialTask* task)
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

void tMaterialTaskEnd(tMaterialTask* task)
{
    if (!task || !task->tasks)
        return;

    JobSystem_Wait(task->jobs);
    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
}

bool tMaterialTaskIsDone(const tMaterialTask* task)
{
    return !task || !task->tasks || ArrayLength(task->tasks) == 0;
}

static void tMeshCopyTaskRunJob(void* userData)
{
    tMeshCopyTaskData* data = (tMeshCopyTaskData*)userData;
    tMeshCopyJobExecute(&data->job);
}

static void tMeshCopyTaskFinish(tMeshCopyTask* task, tMeshCopyTaskData* data)
{
    tChunk* chunk = tChunkDataFindChunk(&task->worldState->chunkData, data->chunkPosition);
    if (!chunk)
    {
        tMeshCopyJobClearResults(&data->job);
        return;
    }

    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
    {
        ArrayDestroy(chunk->validIndices[i]);
        chunk->validIndices[i] = data->job.validIndices[i];
        data->job.validIndices[i] = NULL;
    }

    ArrayPush(task->worldState->chunkData.pendingChunks, *chunk);
}

bool tMeshCopyTaskInit(tMeshCopyTask* task, tWorldState* worldState, JobSystem* jobs, size_t taskCapacity)
{
    if (!task || !worldState)
    {
        AX_WARN("transvoxel unity mesh copy task init failed: invalid argument");
        return false;
    }

    *task = (tMeshCopyTask){0};
    task->worldState = worldState;
    task->jobs = jobs;
    task->tasks = ArrayCreatePrealloc(tMeshCopyTaskData, taskCapacity);
    if (!task->tasks)
    {
        AX_WARN("transvoxel unity mesh copy task init failed: allocation failed");
        return false;
    }

    return true;
}

void tMeshCopyTaskDestroy(tMeshCopyTask* task)
{
    if (!task)
        return;

    tMeshCopyTaskEnd(task);
    ArrayDestroy(task->tasks);
    *task = (tMeshCopyTask){0};
}

static void tMeshCopyTaskStartOne(tMeshCopyTask* task, tChunkUpdate update)
{
    if (update.updateType != tChunkUpdateType_Update)
        return;

    tChunk* chunk = tChunkDataFindChunk(&task->worldState->chunkData, update.chunkPosition);
    if (!chunk)
        return;

    size_t oldCount = ArrayLength(task->tasks);
    if (oldCount >= ArrayCapacity(task->tasks))
    {
        AX_WARN("transvoxel unity mesh copy task skipped: task capacity exhausted");
        return;
    }

    tMeshCopyTaskData data;
    data.chunkPosition = update.chunkPosition;
    data.handle = 0;
    if (!tMeshCopyJobCreate(task->worldState, update, &data.job))
        return;

    chunk->neighboursMask = update.neighboursMask;
    task->tasks[oldCount] = data;
    ArrayFieldSet(task->tasks, ArrayField_Length, oldCount + 1);

    tMeshCopyTaskData* stored = &task->tasks[oldCount];
    if (task->jobs)
        stored->handle = JobSystem_Execute(task->jobs, tMeshCopyTaskRunJob, stored);
    if (stored->handle == 0)
        tMeshCopyTaskRunJob(stored);
}

void tMeshCopyTaskStart(tMeshCopyTask* task)
{
    if (!task || !task->worldState)
        return;

    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
    tChunkUpdate* filteredUpdates = task->worldState->chunkData.filteredChunkUpdates;
    for (size_t i = 0; i < ArrayLength(filteredUpdates); i++)
        tMeshCopyTaskStartOne(task, filteredUpdates[i]);
}

void tMeshCopyTaskExecute(tMeshCopyTask* task)
{
    if (!task)
        return;

    for (size_t rev = ArrayLength(task->tasks); rev > 0; rev--)
    {
        size_t i = rev - 1;
        tMeshCopyTaskData* data = &task->tasks[i];
        if (!JobSystem_IsJobDone(task->jobs, data->handle))
            continue;

        tMeshCopyTaskFinish(task, data);
        task->tasks[i] = task->tasks[ArrayLength(task->tasks) - 1];
        ArrayFieldSet(task->tasks, ArrayField_Length, ArrayLength(task->tasks) - 1);
    }
}

void tMeshCopyTaskEnd(tMeshCopyTask* task)
{
    if (!task || !task->tasks)
        return;

    JobSystem_Wait(task->jobs);
    for (size_t i = 0; i < ArrayLength(task->tasks); i++)
        tMeshCopyTaskFinish(task, &task->tasks[i]);

    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
}

bool tMeshCopyTaskIsDone(const tMeshCopyTask* task)
{
    return !task || !task->tasks || ArrayLength(task->tasks) == 0;
}

static void tMesherTaskRunJob(void* userData)
{
    tMesherTaskData* data = (tMesherTaskData*)userData;
    tMesherJobExecute(&data->job);
}

static void tMesherTaskReleaseMeshShell(tMesherTaskData* data)
{
    if (!data || !data->job.meshData)
        return;

    DeAllocateTLSFGlobal(data->job.meshData);
    data->job.meshData = NULL;
}

static bool tMesherTaskFinish(tMesherTask* task, tMesherTaskData* data)
{
    if (!task || !data || !data->job.meshData)
        return false;

    if (tChunkDataFindChunk(&task->worldState->chunkData, data->chunkPosition))
    {
        AX_WARN("transvoxel unity mesher task finish skipped: chunk already exists");
        tPoolsAddMeshDataContainer(&task->worldState->pools, data->job.meshData);
        data->job.meshData = NULL;
        return false;
    }

    tChunk chunk = {0};
    chunk.meshData = *data->job.meshData;
    chunk.position = data->chunkPosition;
    chunk.lod = data->lod;
    chunk.neighboursMask = data->neighboursMask;

    tMesherTaskReleaseMeshShell(data);
    if (!tChunkDataStoreChunk(&task->worldState->chunkData, chunk))
    {
        tMeshDataContainerDestroy(&chunk.meshData);
        return false;
    }

    tChunkUpdate update;
    update.chunkPosition = data->chunkPosition;
    update.lod = data->lod;
    update.neighboursMask = data->neighboursMask;
    update.updateType = tChunkUpdateType_Update;
    ArrayPush(task->worldState->chunkData.filteredChunkUpdates, update);
    return true;
}

bool tMesherTaskInit(tMesherTask* task, tWorldSettings settings, tWorldState* worldState,
                     const tDensityGenerator* generator, JobSystem* jobs, tMesherFn mesh, void* userData,
                     size_t taskCapacity)
{
    if (!task || !worldState || !generator || !mesh)
    {
        AX_WARN("transvoxel unity mesher task init failed: invalid argument");
        return false;
    }

    *task = (tMesherTask){0};
    task->settings = settings;
    task->worldState = worldState;
    task->generator = generator;
    task->jobs = jobs;
    task->mesh = mesh;
    task->userData = userData;
    task->tasks = ArrayCreatePrealloc(tMesherTaskData, taskCapacity);
    if (!task->tasks)
    {
        AX_WARN("transvoxel unity mesher task init failed: allocation failed");
        return false;
    }

    return true;
}

void tMesherTaskDestroy(tMesherTask* task)
{
    if (!task)
        return;

    tMesherTaskEnd(task);
    ArrayDestroy(task->tasks);
    *task = (tMesherTask){0};
}

static void tMesherTaskStartOne(tMesherTask* task, tChunkUpdate update)
{
    if (update.updateType == tChunkUpdateType_Remove)
    {
        if (tChunkDataFindChunk(&task->worldState->chunkData, update.chunkPosition))
            ArrayPush(task->worldState->chunkData.chunksToRemove, update.chunkPosition);
        return;
    }

    if (update.updateType != tChunkUpdateType_Create)
        return;

    size_t oldCount = ArrayLength(task->tasks);
    if (oldCount >= ArrayCapacity(task->tasks))
    {
        AX_WARN("transvoxel unity mesher task skipped: task capacity exhausted");
        return;
    }

    tMesherTaskData data;
    data.chunkPosition = update.chunkPosition;
    data.chunkMin = tGetChunkMin(update.chunkPosition, task->settings.chunkSize, update.lod);
    data.lod = update.lod;
    data.neighboursMask = update.neighboursMask;
    data.handle = 0;
    if (!tMesherJobCreate(&task->settings, task->worldState, task->generator, update, task->mesh, task->userData, &data.job))
        return;

    task->tasks[oldCount] = data;
    ArrayFieldSet(task->tasks, ArrayField_Length, oldCount + 1);

    tMesherTaskData* stored = &task->tasks[oldCount];
    if (task->jobs)
        stored->handle = JobSystem_Execute(task->jobs, tMesherTaskRunJob, stored);
    if (stored->handle == 0)
        tMesherTaskRunJob(stored);
}

void tMesherTaskStart(tMesherTask* task)
{
    if (!task || !task->worldState)
        return;

    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
    tChunkUpdate* filteredUpdates = task->worldState->chunkData.filteredChunkUpdates;
    for (size_t rev = ArrayLength(filteredUpdates); rev > 0; rev--)
        tMesherTaskStartOne(task, filteredUpdates[rev - 1]);
}

void tMesherTaskExecute(tMesherTask* task)
{
    if (!task)
        return;

    for (size_t rev = ArrayLength(task->tasks); rev > 0; rev--)
    {
        size_t i = rev - 1;
        tMesherTaskData* data = &task->tasks[i];
        if (!JobSystem_IsJobDone(task->jobs, data->handle))
            continue;

        tMesherTaskFinish(task, data);
        task->tasks[i] = task->tasks[ArrayLength(task->tasks) - 1];
        ArrayFieldSet(task->tasks, ArrayField_Length, ArrayLength(task->tasks) - 1);
    }
}

void tMesherTaskEnd(tMesherTask* task)
{
    if (!task || !task->tasks)
        return;

    JobSystem_Wait(task->jobs);
    for (size_t i = 0; i < ArrayLength(task->tasks); i++)
        tMesherTaskFinish(task, &task->tasks[i]);
    ArrayFieldSet(task->tasks, ArrayField_Length, 0);
}

bool tMesherTaskIsDone(const tMesherTask* task)
{
    return !task || !task->tasks || ArrayLength(task->tasks) == 0;
}
