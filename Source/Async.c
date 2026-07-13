// detached worker thread tasks: async file writes and generic background work.
// tasks are fire and forget, completion is reported through the callback which
// runs on the worker thread
#include "Include/Async.h"
#include "Include/FileSystem.h"
#include "Include/Platform.h"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>

typedef struct AsyncTask_
{
    s32 (*run)(struct AsyncTask_* task);
    AsyncTaskFn   userFn;
    AsyncCallback callback;
    void*         userData;
    const void*   data;
    u64           size;
    char          path[512];
} AsyncTask;

static int AsyncThreadMain(void* param)
{
    AsyncTask* task = (AsyncTask*)param;
    s32 result = task->run(task);
    if (task->callback)
        task->callback(task->userData, result);
    SDL_free(task);
    return 0;
}

// allocates the task with the thread safe allocator, the engine tlsf heap is not
// safe to use across threads
static AsyncTask* AsyncCreateTask(AsyncCallback callback, void* userData)
{
    AsyncTask* task = (AsyncTask*)SDL_calloc(1, sizeof(AsyncTask));
    task->callback = callback;
    task->userData = userData;
    return task;
}

static s32 AsyncStart(AsyncTask* task, const char* name)
{
    SDL_Thread* thread = SDL_CreateThread(AsyncThreadMain, name, task);
    if (!thread)
    {
        AX_ERROR("async thread creation failed: %s", SDL_GetError());
        SDL_free(task);
        return 0;
    }
    SDL_DetachThread(thread);
    return 1;
}

static s32 AsyncRunWriteFile(AsyncTask* task)
{
    AFile file = AFileOpen(task->path, AOpenFlag_WriteBinary);
    if (!AFileExist(file))
    {
        AX_ERROR("async write failed to open: %s", task->path);
        return 0;
    }
    AFileWrite(task->data, task->size, file, 1);
    AFileClose(file);
    return 1;
}

s32 WriteAllBytesAsync(const char* path, const void* data, u64 size, AsyncCallback callback, void* userData)
{
    int pathLen = StringLength(path);
    if (!data || pathLen <= 0 || pathLen >= (int)sizeof(((AsyncTask*)0)->path))
    {
        AX_ERROR("async write invalid arguments: %s", path ? path : "(null)");
        return 0;
    }

    AsyncTask* task = AsyncCreateTask(callback, userData);
    task->run = AsyncRunWriteFile;
    task->data = data;
    task->size = size;
    MemCopy(task->path, path, pathLen + 1);
    return AsyncStart(task, "AsyncWrite");
}

s32 WriteAllTextAsync(const char* path, const char* text, AsyncCallback callback, void* userData)
{
    return WriteAllBytesAsync(path, text, (u64)StringLength(text), callback, userData);
}

static s32 AsyncRunUserFn(AsyncTask* task)
{
    return task->userFn(task->userData);
}

void AsyncRun(const char* name, AsyncTaskFn fn, AsyncCallback callback, void* userData)
{
    AsyncTask* task = AsyncCreateTask(callback, userData);
    task->run = AsyncRunUserFn;
    task->userFn = fn;
	if (!AsyncStart(task, name ? name : "AsyncTask"))
	{
		s32 result = fn(userData);
		if (callback) callback(userData, result);
		AX_WARN("running on another thread failed! %s", name);
	}
}
