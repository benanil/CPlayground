#ifndef MEMORY_INCLUDED
#define MEMORY_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef TLSF_MEMORY_SIZE
    #define TLSF_MEMORY_SIZE (2048ull * 1024ull * 1024ull)
#endif

#ifndef ARENA_MEMORY_SIZE
    #define ARENA_MEMORY_SIZE (128 * 1024 * 1024) /* 128 mb */ 
#endif

#ifndef DEFAULT_ALIGN
    #define DEFAULT_ALIGN (2 * sizeof(void*))
#endif

#define DEFER(_end_) for(int (_defer_##__LINE__) = 0; (_defer_##__LINE__) < 1; (_defer_##__LINE__)++, _end_)

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct Arena_ {
    char*  buf;
    size_t buffLen;
    size_t currOffset;
} Arena;

typedef struct ArenaMark_ {
    size_t offset;
} ArenaMark;

// Per-import bump buffer; allocations larger than what's left spill to TLSF instead of forcing a
// huge fixed arena. Sized so the common small import/bake scratch never spills.
#ifndef ARENA_SCRATCH_SIZE
    #define ARENA_SCRATCH_SIZE (32 * 1024 * 1024) /* 32 mb */
#endif
// Max simultaneously-live TLSF spill allocations. Bounded by code nesting depth (a handful), not by
// data size, so a small fixed count is safe.
#define ARENA_SCRATCH_MAX_SPILLS 24

typedef struct ArenaScratchSpill_ {
    size_t offset;   // bump offset when this spill was made; lets pop/restore tell spill from bump
    void*  ptr;      // TLSF allocation
} ArenaScratchSpill;

// A scratch arena that temporarily replaces the calling thread's "current" arena (the one
// ArenaPushGlobal/ArenaPopGlobal operate on). Small bump buffer + TLSF spill. Lives on the caller's stack.
typedef struct ArenaScratch_ {
    struct ArenaScratch_* previous;
    const char* name;
    char*    buf;
    size_t   buffLen;
    size_t   currOffset;
    uint32_t spillCount;
    ArenaScratchSpill spills[ARENA_SCRATCH_MAX_SPILLS];
} ArenaScratch;

typedef struct FixedFragment_ {
    struct FixedFragment_* next;
    char*  ptr;
    size_t size;
} FixedFragment;

typedef struct FixedPow2Allocator_ {
    size_t         currentCapacity;
    FixedFragment* base;
    FixedFragment* current;
} FixedPow2Allocator;

typedef struct RangeU32_
{
    uint32_t offset;
    uint32_t count;
} RangeU32;

typedef struct RangeAllocator_
{
    RangeU32* freeRanges;
    uint32_t  numFreeRanges;
    uint32_t  maxFreeRanges;
    uint32_t  watermark;
    uint32_t  capacity;
} RangeAllocator;

extern Arena GlobalArena;

uint64_t  AlignAddress(uint64_t addr, uint64_t align);
void*     AlignPointer(void* ptr, uint64_t align);
void*     AllocAligned(uint64_t bytes, uint64_t align);
void*     OSAllocAligned(uint64_t bytes, uint64_t align);
void      OSFreeAligned(void* pMem, size_t size);
void      FreeAligned(void* pMem);

void      ArenaInit(Arena* a, size_t backing_buffer_length);
void      ArenaFree(Arena* a);
void      ArenaReset(Arena* a);
void*     ArenaAllocAlign(Arena* a, size_t size, size_t align);
void      ArenaPopAligned(Arena* a, void* ptr, size_t size, size_t align);
void*     ArenaAlloc(Arena* a, size_t size);
void*     ArenaAllocZero(Arena* a, size_t size);
size_t    ArenaRemaining(Arena* a);
ArenaMark ArenaSave(Arena* a);
void      ArenaRestore(Arena* a, ArenaMark mark);

void      InitGlobalArena();
Arena*    GetGlobalArena();
void*     ArenaPushGlobal(uint64_t size);
void      ArenaPopGlobal(uint64_t size);

// Installs a fresh TLSF-backed scratch arena as the calling thread's current arena: every
// ArenaPushGlobal/ArenaPopGlobal until the matching ArenaEndScratch comes from it instead of
// the shared GlobalArena. This lets a worker thread run the import/bake scratch allocations
// without racing the main thread's GlobalArena. Nestable. out: false on allocation failure
// (current arena is left unchanged).
bool      ArenaBeginScratch(ArenaScratch* scratch, size_t size, const char* name);
void      ArenaEndScratch(ArenaScratch* scratch);
bool      ArenaScratchCreate(ArenaScratch* scratch, size_t size, const char* name);
void      ArenaScratchDestroy(ArenaScratch* scratch);
void      ArenaScratchBegin(ArenaScratch* scratch);
void      ArenaScratchEnd(ArenaScratch* scratch);
uint64_t  ArenaRemainingCurrent(void);
uint64_t  ArenaGetCurrentOffset(void);
void      ArenaSetCurrentOffset(size_t offset);

size_t OSGetPageSize(void);
size_t OSRoundToPage(size_t size);
void*  OSAlloc(size_t size);
int    OSFree(void *ptr, size_t size);

#define ArenaStruct(arena, type)     ((type*)ArenaAllocAlign(arena, sizeof(type), _Alignof(type)))
#define ArenaArray(arena, type, cnt) ((type*)ArenaAllocAlign(arena, sizeof(type) * (cnt), _Alignof(type)))
#define ArenaAllocGlobal(cnt)        (ArenaAllocAlign(&GlobalArena, (cnt), DEFAULT_ALIGN))

void* AllocateTLSFGlobal(size_t size);
void* ReAllocateTLSFGlobal(void* ptr, size_t size);
void  DeAllocateTLSFGlobal(void* ptr);
void* AllocZeroTLSFGlobal(size_t count, size_t size);

void RangeAllocator_Init(RangeAllocator* alloc, RangeU32* freeRanges, uint32_t maxFreeRanges, uint32_t capacity);
int  RangeAllocator_Alloc(RangeAllocator* alloc, uint32_t count, uint32_t* outOffset);
void RangeAllocator_Free(RangeAllocator* alloc, uint32_t offset, uint32_t count);

// FixedPow2Allocator
void  FixedPow2Allocator_Init(FixedPow2Allocator* alloc, size_t initialSize);
void  FixedPow2Allocator_CheckFixGrow(FixedPow2Allocator* alloc, size_t countBytes);
void* FixedPow2Allocator_Allocate(FixedPow2Allocator* alloc, size_t countBytes);
void* FixedPow2Allocator_AllocateUninitialized(FixedPow2Allocator* alloc, size_t countBytes);
void  FixedPow2Allocator_Copy(FixedPow2Allocator* alloc, const FixedPow2Allocator* other);
void* FixedPow2Allocator_TakeOwnership(FixedPow2Allocator* alloc);
void  FixedPow2Allocator_Destroy(FixedPow2Allocator* alloc);
    
#if defined(__cplusplus)
}
#endif
#endif

