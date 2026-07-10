#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Platform.h"

// Port of Runtime/Core/LoopingTaskScheduler.cs: a round-robin loop over the pipeline
// stages. Only the front task runs; when it reports done it is ended, the ring
// advances, and the next task is started. The loop never terminates on its own.

static void tTaskSchedulerStartTask(tTask* task)
{
    if (task->start)
        task->start(task->userData);
}

bool tTaskSchedulerInit(tTaskScheduler* scheduler, size_t taskCapacity)
{
    if (!scheduler || taskCapacity == 0)
    {
        AX_WARN("transvoxel unity task scheduler init failed: invalid argument");
        return false;
    }

    *scheduler = (tTaskScheduler){0};
    scheduler->tasks = ArrayCreatePrealloc(tTask, taskCapacity);
    if (!scheduler->tasks)
    {
        AX_WARN("transvoxel unity task scheduler init failed: allocation failed");
        return false;
    }

    return true;
}

void tTaskSchedulerDestroy(tTaskScheduler* scheduler)
{
    if (!scheduler)
        return;

    tTaskSchedulerEndTasks(scheduler);
    ArrayDestroy(scheduler->tasks);
    *scheduler = (tTaskScheduler){0};
}

bool tTaskSchedulerStartTasks(tTaskScheduler* scheduler, const tTask* tasks, size_t taskCount)
{
    if (!scheduler || !scheduler->tasks || !tasks || taskCount == 0)
    {
        AX_WARN("transvoxel unity task scheduler start failed: invalid argument");
        return false;
    }

    if (scheduler->running)
        tTaskSchedulerEndTasks(scheduler);

    ArrayFieldSet(scheduler->tasks, ArrayField_Length, 0);
    for (size_t i = 0; i < taskCount; i++)
        ArrayPush(scheduler->tasks, tasks[i]);

    scheduler->currentTask = 0;
    scheduler->running = true;
    tTaskSchedulerStartTask(&scheduler->tasks[0]);
    return true;
}

void tTaskSchedulerProcessTasks(tTaskScheduler* scheduler)
{
    if (!scheduler || !scheduler->running)
        return;

    u32 taskCount = (u32)ArrayLength(scheduler->tasks);
    if (taskCount == 0)
        return;

    tTask* task = &scheduler->tasks[scheduler->currentTask];
    if (task->execute)
        task->execute(task->userData);

    if (!task->isDone || task->isDone(task->userData))
    {
        if (task->end)
            task->end(task->userData);
        scheduler->currentTask = (scheduler->currentTask + 1u) % taskCount;
        tTaskSchedulerStartTask(&scheduler->tasks[scheduler->currentTask]);
    }
}

void tTaskSchedulerEndTasks(tTaskScheduler* scheduler)
{
    if (!scheduler || !scheduler->running)
        return;

    if (ArrayLength(scheduler->tasks) > 0)
    {
        tTask* task = &scheduler->tasks[scheduler->currentTask];
        if (task->end)
            task->end(task->userData);
    }

    ArrayFieldSet(scheduler->tasks, ArrayField_Length, 0);
    scheduler->currentTask = 0;
    scheduler->running = false;
}
