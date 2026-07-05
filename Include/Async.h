#ifndef ASYNC_H
#define ASYNC_H

#include "Common.h"

#if defined(__cplusplus)
extern "C" {
#endif

// callbacks run on the worker thread, not the main thread: use thread safe allocators
// (SDL_malloc/SDL_free) and do not touch gpu or scene state from them. result: 1 success
typedef void (*AsyncCallback)(void* userData, s32 result);

// a task body run on the worker thread. out: result passed to the callback
typedef s32 (*AsyncTaskFn)(void* userData);

// writes size bytes to path on a detached worker thread. data must stay valid until the
// callback fires, free it there. callback may be NULL. out: 0 when the thread could not
// start (no callback fires in that case)
s32 WriteAllBytesAsync(const char* path, const void* data, u64 size, AsyncCallback callback, void* userData);

// WriteAllBytesAsync for null terminated text, written raw without a BOM
s32 WriteAllTextAsync(const char* path, const char* text, AsyncCallback callback, void* userData);

// runs fn(userData) on a detached worker thread, then callback(userData, fn result).
// out: 0 when the thread could not start
void AsyncRun(const char* name, AsyncTaskFn fn, AsyncCallback callback, void* userData);

#if defined(__cplusplus)
}
#endif

#endif // ASYNC_H
