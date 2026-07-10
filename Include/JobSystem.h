#ifndef JOB_SYSTEM_H
#define JOB_SYSTEM_H

#include "Common.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef s32 JobHandle;
typedef void (*JobSystemFn)(void* userData);

// Opaque handle. The queue/worker/handle-slot internals live in JobSystem.c; callers only
// ever hold a JobSystem* and go through the functions below.
typedef struct JobSystem_ JobSystem;

// threadCount 0 uses SDL_GetNumLogicalCPUCores(). queueCapacity 0 uses the default.
JobSystem* JobSystem_Create(u32 threadCount, u32 queueCapacity);
void JobSystem_Destroy(JobSystem* jobs);

// Enqueues one task. Returns 0 if jobs/fn is NULL, the system is shutting down,
// the handle table is full, or all fixed-size queues are full.
JobHandle JobSystem_Execute(JobSystem* jobs, JobSystemFn fn, void* userData);

// Waits until this specific job has finished. Invalid/stale handles return immediately.
void JobSystem_WaitJob(JobSystem* jobs, JobHandle handle);
// Nonblocking status query. Invalid/stale handles are considered done.
s32 JobSystem_IsJobDone(const JobSystem* jobs, JobHandle handle);

// Runs available work on the calling thread while waiting for all queued/running
// tasks in this JobSystem to finish.
void JobSystem_Wait(JobSystem* jobs);

s32 JobSystem_IsBusy(const JobSystem* jobs);
u32 JobSystem_GetThreadCount(const JobSystem* jobs);

#if defined(__cplusplus)
}
#endif

#endif // JOB_SYSTEM_H
