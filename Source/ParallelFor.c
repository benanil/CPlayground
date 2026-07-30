#include "Include/ParallelFor.h"

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_thread.h>

#define PARALLEL_FOR_MAX_WORKERS 256u

typedef struct ParallelForTask_
{
    ParallelForFn fn;
    void* userData;
    u32 begin;
    u32 end;
} ParallelForTask;

static int ParallelForThreadMain(void* userData)
{
    ParallelForTask* task = (ParallelForTask*)userData;
    task->fn(task->begin, task->end, task->userData);
    return 0;
}

void ParallelFor(u32 itemCount, u32 minItemsPerWorker, ParallelForFn fn, void* userData)
{
    SDL_Thread* threads[PARALLEL_FOR_MAX_WORKERS - 1u];
    ParallelForTask tasks[PARALLEL_FOR_MAX_WORKERS];

    if (!fn || itemCount == 0u) return;
    if (minItemsPerWorker == 0u) minItemsPerWorker = 1u;

    u32 workerCount = (u32)SDL_GetNumLogicalCPUCores();
    if (workerCount < 2u || itemCount < minItemsPerWorker * 2u)
    {
        fn(0u, itemCount, userData);
        return;
    }

    u32 maxWorkersForWork = itemCount / minItemsPerWorker;
    if (workerCount > maxWorkersForWork) workerCount = maxWorkersForWork;
    if (workerCount > PARALLEL_FOR_MAX_WORKERS) workerCount = PARALLEL_FOR_MAX_WORKERS;
    if (workerCount < 2u)
    {
        fn(0u, itemCount, userData);
        return;
    }

    u32 itemsPerWorker = (itemCount + workerCount - 1u) / workerCount;
    u32 threadCount = 0u;
    u32 taskCount = 0u;

    for (u32 begin = 0u; begin < itemCount; begin += itemsPerWorker)
    {
        u32 end = begin + itemsPerWorker;
        if (end > itemCount) end = itemCount;

        tasks[taskCount].fn = fn;
        tasks[taskCount].userData = userData;
        tasks[taskCount].begin = begin;
        tasks[taskCount].end = end;
        taskCount++;
    }

    for (u32 i = 1u; i < taskCount; i++)
    {
        threads[threadCount] = SDL_CreateThread(ParallelForThreadMain, "ParallelFor", &tasks[i]);
        
		if (threads[threadCount]) {
            threadCount++;
        }
        else {
            ParallelForThreadMain(&tasks[i]);
        }
    }

    ParallelForThreadMain(&tasks[0]);

    for (u32 i = 0u; i < threadCount; i++)
    {
        SDL_WaitThread(threads[i], NULL);
    }
}
