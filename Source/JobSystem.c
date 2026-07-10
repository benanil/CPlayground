#include "Include/JobSystem.h"
#include "Include/Platform.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>

#define JOB_SYSTEM_MAX_THREADS 64u
#define JOB_SYSTEM_DEFAULT_QUEUE_CAPACITY 1024u
#define JOB_SYSTEM_MAX_HANDLE_SLOTS 65535u
#define JOB_SYSTEM_HANDLE_INDEX_MASK 0xFFFFu
#define JOB_SYSTEM_HANDLE_GENERATION_SHIFT 16u

typedef enum JobSystemHandleState_
{
    JobSystemHandleState_Free = 0,
    JobSystemHandleState_Reserving = 1,
    JobSystemHandleState_Active = 2
} JobSystemHandleState;

typedef struct JobSystemTask_
{
    JobSystemFn fn;
    void*       userData;
    JobHandle   handle;
} JobSystemTask;

typedef struct JobSystemQueue_
{
    JobSystemTask* tasks;
    SDL_Mutex*     mutex;
    u32 capacity;
    u32 head;
    u32 tail;
    u32 count;
} JobSystemQueue;

typedef struct JobSystemHandleSlot_
{
    SDL_AtomicInt state;
    SDL_AtomicU32 generation;
} JobSystemHandleSlot;

typedef struct JobSystemWorker_
{
    struct JobSystem_* jobs;
    u32 threadID;
} JobSystemWorker;

struct JobSystem_
{
    SDL_Thread**         threads;
    JobSystemWorker*     workers;
    JobSystemQueue*      queues;
    SDL_Condition*       sleepingCondition;
    SDL_Mutex*           sleepingMutex;
    SDL_Condition*       waitingCondition;
    SDL_Mutex*           waitingMutex;
    JobSystemHandleSlot* handleSlots;
    SDL_AtomicU32        nextQueue;
    SDL_AtomicU32        nextHandleSlot;
    SDL_AtomicU32        counter;
    SDL_AtomicInt        queuedCount;
    SDL_AtomicInt        alive;
    u32                  queueCount;
    u32                  workerThreadCount;
    u32                  handleSlotCount;
};

static JobHandle JobSystem_MakeHandle(u32 slotIndex, u32 generation)
{
    u32 handle = (generation << JOB_SYSTEM_HANDLE_GENERATION_SHIFT) | (slotIndex + 1u);
    return (JobHandle)handle;
}

static u32 JobSystem_HandleSlotIndex(JobHandle handle)
{
    u32 slot = ((u32)handle) & JOB_SYSTEM_HANDLE_INDEX_MASK;
    return slot - 1u;
}

static u32 JobSystem_HandleGeneration(JobHandle handle)
{
    return ((u32)handle) >> JOB_SYSTEM_HANDLE_GENERATION_SHIFT;
}

static JobHandle JobSystem_AllocHandle(JobSystem* jobs)
{
    for (u32 i = 0u; i < jobs->handleSlotCount; i++)
    {
        u32 slotIndex = SDL_AddAtomicU32(&jobs->nextHandleSlot, 1) % jobs->handleSlotCount;
        JobSystemHandleSlot* slot = &jobs->handleSlots[slotIndex];
        if (SDL_CompareAndSwapAtomicInt(&slot->state, JobSystemHandleState_Free, JobSystemHandleState_Reserving))
        {
            u32 generation = (SDL_GetAtomicU32(&slot->generation) & JOB_SYSTEM_HANDLE_INDEX_MASK) + 1u;
            if (generation > JOB_SYSTEM_HANDLE_INDEX_MASK) generation = 1u;
            SDL_SetAtomicU32(&slot->generation, generation);
            SDL_SetAtomicInt(&slot->state, JobSystemHandleState_Active);
            return JobSystem_MakeHandle(slotIndex, generation);
        }
    }

    return 0;
}

static void JobSystem_ReleaseHandle(JobSystem* jobs, JobHandle handle)
{
    u32 slotIndex = JobSystem_HandleSlotIndex(handle);
    if (slotIndex >= jobs->handleSlotCount) return;

    JobSystemHandleSlot* slot = &jobs->handleSlots[slotIndex];
    if ((SDL_GetAtomicU32(&slot->generation) & JOB_SYSTEM_HANDLE_INDEX_MASK) == JobSystem_HandleGeneration(handle))
    {
        SDL_SetAtomicInt(&slot->state, JobSystemHandleState_Free);
    }
}

static s32 JobSystem_IsHandleActive(const JobSystem* jobs, JobHandle handle)
{
    if (!jobs || handle == 0) return 0;

    u32 slotIndex = JobSystem_HandleSlotIndex(handle);
    if (slotIndex >= jobs->handleSlotCount) return 0;

    const JobSystemHandleSlot* slot = &jobs->handleSlots[slotIndex];
    u32 generation = SDL_GetAtomicU32((SDL_AtomicU32*)&slot->generation) & JOB_SYSTEM_HANDLE_INDEX_MASK;
    if (generation != JobSystem_HandleGeneration(handle)) return 0;

    return SDL_GetAtomicInt((SDL_AtomicInt*)&slot->state) == JobSystemHandleState_Active;
}

static s32 JobSystemQueue_PushBack(JobSystem* jobs, JobSystemQueue* queue, JobSystemTask task)
{
    s32 result = 0;
    SDL_LockMutex(queue->mutex);
    if (queue->count < queue->capacity)
    {
        queue->tasks[queue->tail] = task;
        queue->tail++;
        if (queue->tail == queue->capacity) queue->tail = 0u;
        queue->count++;
        SDL_AddAtomicU32(&jobs->counter, 1);
        SDL_AddAtomicInt(&jobs->queuedCount, 1);
        result = 1;
    }
    SDL_UnlockMutex(queue->mutex);
    return result;
}

static void JobSystem_WakeOne(JobSystem* jobs)
{
    SDL_LockMutex(jobs->sleepingMutex);
    SDL_SignalCondition(jobs->sleepingCondition);
    SDL_UnlockMutex(jobs->sleepingMutex);
}

static void JobSystem_WakeAll(JobSystem* jobs)
{
    SDL_LockMutex(jobs->sleepingMutex);
    SDL_BroadcastCondition(jobs->sleepingCondition);
    SDL_UnlockMutex(jobs->sleepingMutex);
}

static s32 JobSystemQueue_PopFront(JobSystemQueue* queue, JobSystemTask* task)
{
    s32 result = 0;
    SDL_LockMutex(queue->mutex);
    if (queue->count > 0u)
    {
        *task = queue->tasks[queue->head];
        queue->head++;
        if (queue->head == queue->capacity) queue->head = 0u;
        queue->count--;
        result = 1;
    }
    SDL_UnlockMutex(queue->mutex);
    return result;
}

static void JobSystem_NotifyFinished(JobSystem* jobs)
{
    SDL_LockMutex(jobs->waitingMutex);
    SDL_BroadcastCondition(jobs->waitingCondition);
    SDL_UnlockMutex(jobs->waitingMutex);
}

static void JobSystem_Work(JobSystem* jobs, u32 startingQueue)
{
    JobSystemTask task;
    for (u32 i = 0u; i < jobs->queueCount; i++)
    {
        JobSystemQueue* queue = &jobs->queues[startingQueue % jobs->queueCount];
        startingQueue++;
        while (JobSystemQueue_PopFront(queue, &task))
        {
            SDL_AddAtomicInt(&jobs->queuedCount, -1);
            task.fn(task.userData);
            JobSystem_ReleaseHandle(jobs, task.handle);
            SDL_AddAtomicU32(&jobs->counter, -1);
            JobSystem_NotifyFinished(jobs);
        }
    }
}

static int JobSystemThreadMain(void* userData)
{
    JobSystemWorker* worker = (JobSystemWorker*)userData;
    JobSystem* jobs = worker->jobs;

    while (SDL_GetAtomicInt(&jobs->alive))
    {
        JobSystem_Work(jobs, worker->threadID);

        SDL_LockMutex(jobs->sleepingMutex);
        while (SDL_GetAtomicInt(&jobs->alive) && SDL_GetAtomicInt(&jobs->queuedCount) == 0)
        {
            SDL_WaitCondition(jobs->sleepingCondition, jobs->sleepingMutex);
        }
        SDL_UnlockMutex(jobs->sleepingMutex);
    }

    return 0;
}

static void JobSystem_FreeQueues(JobSystem* jobs)
{
    if (!jobs->queues) return;

    for (u32 i = 0u; i < jobs->queueCount; i++)
    {
        if (jobs->queues[i].mutex) SDL_DestroyMutex(jobs->queues[i].mutex);
        if (jobs->queues[i].tasks) SDL_free(jobs->queues[i].tasks);
    }
    SDL_free(jobs->queues);
    jobs->queues = NULL;
}

static void JobSystem_Free(JobSystem* jobs)
{
    if (!jobs) return;

    if (jobs->sleepingCondition) SDL_DestroyCondition(jobs->sleepingCondition);
    if (jobs->sleepingMutex)     SDL_DestroyMutex(jobs->sleepingMutex);
    if (jobs->waitingCondition)  SDL_DestroyCondition(jobs->waitingCondition);
    if (jobs->waitingMutex)      SDL_DestroyMutex(jobs->waitingMutex);
    if (jobs->handleSlots) SDL_free(jobs->handleSlots);
    if (jobs->threads) SDL_free(jobs->threads);
    if (jobs->workers) SDL_free(jobs->workers);
    JobSystem_FreeQueues(jobs);
    SDL_free(jobs);
}

JobSystem* JobSystem_Create(u32 threadCount, u32 queueCapacity)
{
    if (threadCount == 0u)
    {
        s32 coreCount = SDL_GetNumLogicalCPUCores();
        threadCount = coreCount > 0 ? (u32)coreCount : 1u;
    }
    if (threadCount > JOB_SYSTEM_MAX_THREADS) threadCount = JOB_SYSTEM_MAX_THREADS;
    if (queueCapacity == 0u) queueCapacity = JOB_SYSTEM_DEFAULT_QUEUE_CAPACITY;

    JobSystem* jobs = (JobSystem*)SDL_calloc(1, sizeof(JobSystem));
    if (!jobs)
    {
        AX_WARN("job system allocation failed");
        return NULL;
    }

    jobs->queueCount = threadCount;
    jobs->handleSlotCount = Minu32(threadCount * queueCapacity, JOB_SYSTEM_MAX_HANDLE_SLOTS);
    SDL_SetAtomicInt(&jobs->alive, 1);

    jobs->queues      = (JobSystemQueue*)SDL_calloc(jobs->queueCount, sizeof(JobSystemQueue));
    jobs->threads     = (SDL_Thread**)SDL_calloc(jobs->queueCount, sizeof(SDL_Thread*));
    jobs->workers     = (JobSystemWorker*)SDL_calloc(jobs->queueCount, sizeof(JobSystemWorker));
    jobs->handleSlots = (JobSystemHandleSlot*)SDL_calloc(jobs->handleSlotCount, sizeof(JobSystemHandleSlot));
    jobs->sleepingCondition = SDL_CreateCondition();
    jobs->sleepingMutex     = SDL_CreateMutex();
    jobs->waitingCondition  = SDL_CreateCondition();
    jobs->waitingMutex      = SDL_CreateMutex();

    if (!jobs->queues || !jobs->threads || !jobs->workers || !jobs->handleSlots || !jobs->sleepingCondition ||
        !jobs->sleepingMutex || !jobs->waitingCondition || !jobs->waitingMutex)
    {
        AX_WARN("job system synchronization allocation failed: %s", SDL_GetError());
        JobSystem_Free(jobs);
        return NULL;
    }

    for (u32 i = 0u; i < jobs->queueCount; i++)
    {
        jobs->queues[i].capacity = queueCapacity;
        jobs->queues[i].tasks    = (JobSystemTask*)SDL_calloc(queueCapacity, sizeof(JobSystemTask));
        jobs->queues[i].mutex    = SDL_CreateMutex();
        if (!jobs->queues[i].tasks || !jobs->queues[i].mutex)
        {
            AX_WARN("job queue allocation failed: %s", SDL_GetError());
            JobSystem_Free(jobs);
            return NULL;
        }
    }

    for (u32 i = 0u; i < jobs->queueCount; i++)
    {
        jobs->workers[i].jobs = jobs;
        jobs->workers[i].threadID = i;
        SDL_Thread* thread = SDL_CreateThread(JobSystemThreadMain, "JobSystem", &jobs->workers[i]);
        if (thread)
        {
            jobs->threads[jobs->workerThreadCount++] = thread;
        }
        else
        {
            AX_WARN("job worker thread creation failed: %s", SDL_GetError());
        }
    }

    return jobs;
}

void JobSystem_Destroy(JobSystem* jobs)
{
    if (!jobs) return;

    SDL_SetAtomicInt(&jobs->alive, 0);
    JobSystem_WakeAll(jobs);

    for (u32 i = 0u; i < jobs->workerThreadCount; i++)
    {
        SDL_WaitThread(jobs->threads[i], NULL);
    }

    JobSystem_Free(jobs);
}

JobHandle JobSystem_Execute(JobSystem* jobs, JobSystemFn fn, void* userData)
{
    if (!jobs || !fn)
    {
        AX_WARN("job execute invalid arguments");
        return 0;
    }
    if (!SDL_GetAtomicInt(&jobs->alive)) return 0;

    JobSystemTask task;
    task.fn = fn;
    task.userData = userData;
    task.handle = JobSystem_AllocHandle(jobs);
    if (task.handle == 0)
    {
        AX_WARN("job handle table is full");
        return 0;
    }

    u32 queueIndex = SDL_AddAtomicU32(&jobs->nextQueue, 1) % jobs->queueCount;
    for (u32 i = 0u; i < jobs->queueCount; i++)
    {
        if (JobSystemQueue_PushBack(jobs, &jobs->queues[queueIndex], task))
        {
            JobSystem_WakeOne(jobs);
            return task.handle;
        }
        queueIndex++;
        if (queueIndex == jobs->queueCount) queueIndex = 0u;
    }

    JobSystem_ReleaseHandle(jobs, task.handle);
    AX_WARN("job queues are full");
    return 0;
}

void JobSystem_WaitJob(JobSystem* jobs, JobHandle handle)
{
    if (!jobs || handle == 0) return;

    while (JobSystem_IsHandleActive(jobs, handle))
    {
        JobSystem_WakeAll(jobs);
        JobSystem_Work(jobs, SDL_AddAtomicU32(&jobs->nextQueue, 1) % jobs->queueCount);
        if (!JobSystem_IsHandleActive(jobs, handle)) return;
        if (jobs->workerThreadCount == 0u) continue;

        SDL_LockMutex(jobs->waitingMutex);
        if (JobSystem_IsHandleActive(jobs, handle)) SDL_WaitCondition(jobs->waitingCondition, jobs->waitingMutex);
        SDL_UnlockMutex(jobs->waitingMutex);
    }
}

s32 JobSystem_IsJobDone(const JobSystem* jobs, JobHandle handle)
{
    return !JobSystem_IsHandleActive(jobs, handle);
}

void JobSystem_Wait(JobSystem* jobs)
{
    if (!jobs) return;

    while (JobSystem_IsBusy(jobs))
    {
        JobSystem_WakeAll(jobs);
        JobSystem_Work(jobs, SDL_AddAtomicU32(&jobs->nextQueue, 1) % jobs->queueCount);
        if (!JobSystem_IsBusy(jobs)) return;
        if (jobs->workerThreadCount == 0u) continue;

        SDL_LockMutex(jobs->waitingMutex);
        if (JobSystem_IsBusy(jobs)) SDL_WaitCondition(jobs->waitingCondition, jobs->waitingMutex);
        SDL_UnlockMutex(jobs->waitingMutex);
    }
}

s32 JobSystem_IsBusy(const JobSystem* jobs)
{
    return jobs && SDL_GetAtomicU32((SDL_AtomicU32*)&jobs->counter) > 0u;
}

u32 JobSystem_GetThreadCount(const JobSystem* jobs)
{
    return jobs ? jobs->workerThreadCount : 0u;
}
