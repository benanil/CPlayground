#include "Include/Memory.h"
#include "Include/Common.h"
#include "Include/Algorithm.h"
#include "Include/Platform.h"

#include "Extern/tlsf.h"

// quick A/B switch for hunting allocator related corruption,
// 0 routes the global allocations through SDL's allocator instead of tlsf
#ifndef USE_TLSF_ALLOCATOR
    #define USE_TLSF_ALLOCATOR 0
#endif

#if !USE_TLSF_ALLOCATOR
    #include <SDL3/SDL_stdinc.h>
#else
    #include <SDL3/SDL_atomic.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef PLATFORM_WINDOWS
    #include <sys/mman.h>
    #include <unistd.h>
    #include <errno.h>
#else
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef VC_EXTRALEAN
    #define VC_EXTRALEAN
#endif

#include <Windows.h>
    #include <intrin.h>
#endif

#ifndef ARRAY_COUNT
    #define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef KiB
    #define KiB(x) ((size_t)(x) * 1024ull)
#endif

#ifndef MiB
    #define MiB(x) ((size_t)(x) * 1024ull * 1024ull)
#endif



#define MEMORY_GUARD_WORDS 16u
#define ARENA_GUARD_BEFORE 0x1111222233334444ull
#define ARENA_GUARD_AFTER  0xAAAABBBBCCCCDDDDull

typedef struct AlignedAllocHeader
{
    void*  raw;
    size_t rawSize;
} AlignedAllocHeader;

static AX_ALIGN(64) char g_ArenaStorage[ARENA_MEMORY_SIZE];
Arena  GlobalArena = { 0, 0, 0 };
tlsf_t GlobalTLSF = NULL;

// When set, ArenaPushGlobal/ArenaPopGlobal target this thread's scratch arena instead of the shared
// GlobalArena. A worker installs one (ArenaBeginScratch) so its import/bake scratch never touches the
// main thread's GlobalArena. The bump pointer is not thread safe, so two threads never share a scratch.
static AX_THREAD_LOCAL ArenaScratch* g_CurrentScratch = NULL;

#if USE_TLSF_ALLOCATOR
// The global TLSF heap is shared across threads (the editor bakes meshes on async worker
// threads). TLSF's segregated free lists are a multi-word data structure that cannot be
// updated atomically, so every heap mutation is serialized behind a spinlock. Each critical
// section is a single tlsf_* call, so contention stays cheap. NOTE: this only makes the
// allocator safe; the LIFO GlobalArena, the GPU upload queue and gBundleCache are still
// single-threaded and must not be touched from worker threads.
static SDL_SpinLock g_TLSFLock;
#define TLSF_LOCK()   SDL_LockSpinlock(&g_TLSFLock)
#define TLSF_UNLOCK() SDL_UnlockSpinlock(&g_TLSFLock)
#else
#define TLSF_LOCK()   ((void)0)
#define TLSF_UNLOCK() ((void)0)
#endif

void*  TLSFMemory = NULL;
size_t TLSFMemorySize = 0;

static bool g_MemoryInitialized = false;

static size_t AlignUpSize(size_t value, size_t align)
{
    const size_t mask = align - 1u;
    ASSERT((align & mask) == 0);
    return (value + mask) & ~mask;
}

static bool AddSizeOverflow(size_t a, size_t b, size_t* out)
{
    if (a > SIZE_MAX - b) return true;
    *out = a + b;
    return false;
}

static bool MulSizeOverflow(size_t a, size_t b, size_t* out)
{
    if (a && b > SIZE_MAX / a) return true;
    *out = a * b;
    return false;
}

size_t OSGetPageSize(void)
{
    #ifdef PLATFORM_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
    #else
    return (size_t)getpagesize();
    #endif
}

size_t OSRoundToPage(size_t size)
{
    const size_t pageSize = OSGetPageSize();
    return AlignUpSize(size, pageSize);
}

void* OSAlloc(size_t size)
{
    if (size == 0) return NULL;
    size = OSRoundToPage(size);
    
    #ifdef PLATFORM_WINDOWS
    void* ptr = NULL;
    SIZE_T largePageSize = GetLargePageMinimum();
    if (largePageSize)
    {
        size_t largeSize = AlignUpSize(size, (size_t)largePageSize);
        ptr = VirtualAlloc(NULL, largeSize, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
    }

    if (!ptr) 
        ptr = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return ptr;
    #else
    void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    return ptr;
    #endif
}

int OSFree(void* ptr, size_t size)
{
    if (!ptr) return -1;
    #ifdef PLATFORM_WINDOWS
    (void)size;
    return VirtualFree(ptr, 0, MEM_RELEASE) ? 0 : (int)GetLastError();
    #else
    size = OSRoundToPage(size);
    return munmap(ptr, size) == 0 ? 0 : errno;
    #endif
}

#if USE_TLSF_ALLOCATOR
static bool InitTLSF(size_t requestedSize)
{
    /* Original TLSF has a maximum representable block size. Also avoid exactly
    block_size_max because some versions map that to one-past the FL table. */
    size_t maxBlock = tlsf_block_size_max();
    if (maxBlock > tlsf_align_size())
        maxBlock -= tlsf_align_size();

    size_t maxTotal = tlsf_size() + tlsf_pool_overhead() + maxBlock;
    size_t tlsfSize = requestedSize;

    if (tlsfSize > maxTotal)
    {
        AX_WARN("TLSF backing size clamped: requested=%llu mb max=%llu mb",
                (u64)requestedSize / 1024ull / 1024ull,
                (u64)maxTotal / 1024ull / 1024ull);
        tlsfSize = maxTotal;
    }

    const size_t minTLSFSize = tlsf_size() + tlsf_pool_overhead() + tlsf_block_size_min();

    while (tlsfSize >= minTLSFSize)
    {
        TLSFMemory = OSAlloc(tlsfSize);
        if (TLSFMemory)
            break;
        tlsfSize >>= 1u;
        tlsfSize = OSRoundToPage(tlsfSize);
    }

    if (!TLSFMemory)
    {
        AX_ERROR("Failed to allocate TLSF backing memory: requested=%llu mb",
                 (u64)requestedSize / 1024ull / 1024ull);
        return false;
    }

    TLSFMemorySize = OSRoundToPage(tlsfSize);

    if (((uintptr_t)TLSFMemory & (uintptr_t)(tlsf_align_size() - 1u)) != 0)
    {
        AX_ERROR("TLSF memory is not aligned: memory=%p align=%llu",
                 TLSFMemory, (u64)tlsf_align_size());
        OSFree(TLSFMemory, TLSFMemorySize);
        TLSFMemory = NULL;
        TLSFMemorySize = 0;
        return false;
    }

    GlobalTLSF = tlsf_create(TLSFMemory);
    if (!GlobalTLSF)
    {
        AX_ERROR("tlsf_create failed memory=%p size=%llu",
                 TLSFMemory, (u64)TLSFMemorySize);
        OSFree(TLSFMemory, TLSFMemorySize);
        TLSFMemory = NULL;
        TLSFMemorySize = 0;
        return false;
    }

    size_t poolOffset = tlsf_size();
    size_t poolSize = TLSFMemorySize - poolOffset;

    pool_t pool = tlsf_add_pool(GlobalTLSF, (char*)TLSFMemory + poolOffset, poolSize);
    if (!pool)
    {
        AX_ERROR("tlsf_add_pool failed tlsf=%p memory=%p total=%llu poolSize=%llu",
                 GlobalTLSF, TLSFMemory, (u64)TLSFMemorySize, (u64)poolSize);

        GlobalTLSF = NULL;
        OSFree(TLSFMemory, TLSFMemorySize);
        TLSFMemory = NULL;
        TLSFMemorySize = 0;
        return false;
    }

    if (TLSFMemorySize != (size_t)TLSF_MEMORY_SIZE)
    {
        AX_WARN("TLSF backing allocation adjusted: requested=%llu mb actual=%llu mb",
                (u64)TLSF_MEMORY_SIZE / 1024ull / 1024ull, (u64)TLSFMemorySize / 1024ull / 1024ull);
    }

    AX_LOG("TLSF initialized tlsf=%p memory=%p total=%llu mb pool=%p poolSize=%llu mb",
            GlobalTLSF, TLSFMemory, (u64)TLSFMemorySize / 1024ull / 1024ull,
            pool, (u64)poolSize / 1024ull / 1024ull);
    return true;
}
#endif // USE_TLSF_ALLOCATOR

void InitGlobalArena(void)
{
    GlobalArena.buf        = g_ArenaStorage;
    GlobalArena.buffLen    = ARENA_MEMORY_SIZE;
    GlobalArena.currOffset = 0;

    GlobalTLSF = NULL;
    TLSFMemory = NULL;
    TLSFMemorySize = 0;

    #if USE_TLSF_ALLOCATOR
    if (!InitTLSF((size_t)TLSF_MEMORY_SIZE))
    {
        AX_ERROR("InitGlobalArena failed");
        return;
    }
    #endif

    g_MemoryInitialized = true;
}

Arena* GetGlobalArena(void)
{
    return &GlobalArena;
}

uint64_t AlignAddress(uint64_t addr, uint64_t align)
{
    const uint64_t mask = align - 1u;
    ASSERT((align & mask) == 0);
    return (addr + mask) & ~mask;
}

void* AlignPointer(void* ptr, uint64_t align)
{
    return (void*)AlignAddress((uint64_t)ptr, align);
}

static void CheckMemorySystem(const char* op)
{
    bool valid = g_MemoryInitialized;
    #if USE_TLSF_ALLOCATOR
    valid = valid && GlobalTLSF && TLSFMemory && TLSFMemorySize != 0;
    #endif
    if (!valid)
    {
        AX_ERROR("Memory system invalid during %s initialized=%d tlsf=%p memory=%p size=%llu",
                 op, g_MemoryInitialized ? 1 : 0, GlobalTLSF, TLSFMemory, (u64)TLSFMemorySize);
    }
}

static bool CheckTLSFFail(void* ptr, size_t requestedSize)
{
    if (ptr) return true;

    AX_ERROR("TLSF allocation failed request=%llu bytes / %.3f mb, total=%llu bytes / %.3f mb, tlsf=%p memory=%p",
             (u64)requestedSize, (double)requestedSize / (1024.0 * 1024.0),
             (u64)TLSFMemorySize, (double)TLSFMemorySize / (1024.0 * 1024.0),
             GlobalTLSF, TLSFMemory);
    return false;
}

void* OSAllocAligned(uint64_t bytes, uint64_t align)
{
    if (bytes == 0) return NULL;

    if (align < sizeof(void*))
        align = sizeof(void*);

    ASSERT((align & (align - 1u)) == 0);
    size_t total = 0;
    if (AddSizeOverflow((size_t)bytes, (size_t)align, &total) ||
        AddSizeOverflow(total, sizeof(AlignedAllocHeader), &total))
    {
        AX_ERROR("OSAllocAligned overflow bytes=%llu align=%llu", (u64)bytes, (u64)align);
        ASSERT(false);
        return NULL;
    }

    void* raw = OSAlloc(total);
    if (!raw)
        return NULL;

    uintptr_t start = (uintptr_t)raw + sizeof(AlignedAllocHeader);
    uintptr_t aligned = AlignAddress(start, align);
    AlignedAllocHeader* header = ((AlignedAllocHeader*)aligned) - 1;
    header->raw = raw;
    header->rawSize = OSRoundToPage(total);
    return (void*)aligned;
}

void OSFreeAligned(void* pMem, size_t size)
{
    (void)size;
    if (!pMem) return;
    AlignedAllocHeader* header = ((AlignedAllocHeader*)pMem) - 1;
    OSFree(header->raw, header->rawSize);
}

void* AllocAligned(uint64_t bytes, uint64_t align)
{
    if (bytes == 0) return NULL;

    ASSERT((align & (align - 1u)) == 0);
    CheckMemorySystem("AllocAligned");
    #if USE_TLSF_ALLOCATOR
    if (align < tlsf_align_size())
        align = tlsf_align_size();
    TLSF_LOCK();
    void* ptr = tlsf_memalign(GlobalTLSF, (size_t)align, (size_t)bytes);
    TLSF_UNLOCK();
    #else
    ASSERT(align <= 16u); // SDL_malloc natural alignment
    void* ptr = SDL_malloc((size_t)bytes);
    #endif

    if (!CheckTLSFFail(ptr, (size_t)bytes))
        return NULL;
    return ptr;
}

void FreeAligned(void* pMem)
{
    if (!pMem) return;
    CheckMemorySystem("FreeAligned");
    #if USE_TLSF_ALLOCATOR
    TLSF_LOCK();
    tlsf_free(GlobalTLSF, pMem);
    TLSF_UNLOCK();
    #else
    SDL_free(pMem);
    #endif
}

void* AllocZeroTLSFGlobal(size_t count, size_t size)
{
    size_t bytes = 0;
    if (MulSizeOverflow(count, size, &bytes))
    {
        AX_ERROR("AllocZeroTLSFGlobal overflow count=%llu size=%llu", (u64)count, (u64)size);
        ASSERT(false);
        return NULL;
    }

    if (bytes == 0)
        return NULL;

    void* ptr = AllocateTLSFGlobal(bytes);
    if (!ptr)
        return NULL;
    MemsetZero(ptr, bytes);
    return ptr;
}

void* AllocateTLSFGlobal(size_t size)
{
    if (size == 0) return NULL;
    CheckMemorySystem("malloc");
    #if USE_TLSF_ALLOCATOR
    TLSF_LOCK();
    void* ptr = tlsf_malloc(GlobalTLSF, size);
    TLSF_UNLOCK();
    #else
    void* ptr = SDL_malloc(size);
    #endif

    if (!CheckTLSFFail(ptr, size))
        return NULL;

    #if defined(_DEBUG) || defined(DEBUG) || defined(Debug)
    MemSet(ptr, 0xCD, size);
    #endif
    return ptr;
}

void* ReAllocateTLSFGlobal(void* ptr, size_t size)
{
    CheckMemorySystem("realloc");
    if (!ptr) return AllocateTLSFGlobal(size);
    if (size == 0)
        return NULL;

    #if USE_TLSF_ALLOCATOR
    TLSF_LOCK();
    void* res = tlsf_realloc(GlobalTLSF, ptr, size);
    TLSF_UNLOCK();
    #else
    void* res = SDL_realloc(ptr, size);
    #endif
    if (!CheckTLSFFail(res, size))
        return NULL;
    return res;
}

void DeAllocateTLSFGlobal(void* buff)
{
    if (!buff) return;
    CheckMemorySystem("free");
    #if USE_TLSF_ALLOCATOR
    TLSF_LOCK();
    tlsf_free(GlobalTLSF, buff);
    TLSF_UNLOCK();
    #else
    SDL_free(buff);
    #endif
}

void RangeAllocator_Init(RangeAllocator* alloc, RangeU32* freeRanges, uint32_t maxFreeRanges, uint32_t capacity)
{
    MemsetZero(alloc, sizeof(*alloc));
    alloc->freeRanges = freeRanges;
    alloc->maxFreeRanges = maxFreeRanges;
    alloc->capacity = capacity;
}

int RangeAllocator_Alloc(RangeAllocator* alloc, uint32_t count, uint32_t* outOffset)
{
    *outOffset = 0u;
    if (count == 0u) return 1;
    for (uint32_t i = 0u; i < alloc->numFreeRanges; i++)
    {
        RangeU32* range = &alloc->freeRanges[i];
        if (range->count < count) continue;
        *outOffset = range->offset;
        range->offset += count;
        range->count -= count;
        if (range->count == 0u)
        {
            alloc->numFreeRanges--;
            alloc->freeRanges[i] = alloc->freeRanges[alloc->numFreeRanges];
        }
        return 1;
    }

    if (alloc->watermark + count > alloc->capacity)
        return 0;
    *outOffset = alloc->watermark;
    alloc->watermark += count;
    return 1;
}

static void RangeAllocator_MergeFreeRanges(RangeAllocator* alloc)
{
merge_ranges:
    for (uint32_t i = 0u; i < alloc->numFreeRanges; i++)
    {
        RangeU32* a = &alloc->freeRanges[i];
        for (uint32_t j = i + 1u; j < alloc->numFreeRanges; j++)
        {
            RangeU32* b = &alloc->freeRanges[j];
            uint32_t aEnd = a->offset + a->count;
            uint32_t bEnd = b->offset + b->count;
            if (aEnd < b->offset || bEnd < a->offset) continue;

            uint32_t begin = Minu32(a->offset, b->offset);
            uint32_t end = Maxu32(aEnd, bEnd);
            *a = (RangeU32){ begin, end - begin };
            alloc->numFreeRanges--;
            alloc->freeRanges[j] = alloc->freeRanges[alloc->numFreeRanges];
            goto merge_ranges;
        }
    }

trim_watermark:
    for (uint32_t i = 0u; i < alloc->numFreeRanges; i++)
    {
        RangeU32 range = alloc->freeRanges[i];
        if (range.offset + range.count != alloc->watermark) continue;
        alloc->watermark = range.offset;
        alloc->numFreeRanges--;
        alloc->freeRanges[i] = alloc->freeRanges[alloc->numFreeRanges];
        goto trim_watermark;
    }
}

void RangeAllocator_Free(RangeAllocator* alloc, uint32_t offset, uint32_t count)
{
    if (count == 0u) return;
    if (alloc->numFreeRanges >= alloc->maxFreeRanges)
    {
        AX_WARN("range allocator free range capacity exceeded");
        return;
    }
    alloc->freeRanges[alloc->numFreeRanges++] = (RangeU32){ offset, count };
    RangeAllocator_MergeFreeRanges(alloc);
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                                  Arena                                   */
/*//////////////////////////////////////////////////////////////////////////*/

static inline void CheckArenaSize(void)
{
    if (GlobalArena.buf == NULL)
    {
        GlobalArena.buf        = g_ArenaStorage;
        GlobalArena.buffLen    = ARENA_MEMORY_SIZE;
        GlobalArena.currOffset = 0;
    }
}

bool ArenaScratchCreate(ArenaScratch* scratch, size_t size, const char* name)
{
    *scratch = (ArenaScratch){0};
    scratch->buf        = (char*)AllocateTLSFGlobal(size);
    scratch->name       = name;
    scratch->buffLen    = size;
    if (!scratch->buf)
    {
        AX_WARN("ArenaScratchCreate failed size=%llu", (u64)size);
        scratch->buffLen = 0;
        return false;
    }
    return true;
}

void ArenaScratchDestroy(ArenaScratch* scratch)
{
    if (!scratch) return;
    while (scratch->spillCount > 0)
        DeAllocateTLSFGlobal(scratch->spills[--scratch->spillCount].ptr);
    if (scratch->buf)
    {
        DeAllocateTLSFGlobal(scratch->buf);
        scratch->buf = NULL;
    }
    *scratch = (ArenaScratch){0};
}

void ArenaScratchBegin(ArenaScratch* scratch)
{
    ASSERT(scratch && scratch->buf);
    scratch->currOffset = 0;
    while (scratch->spillCount > 0)
        DeAllocateTLSFGlobal(scratch->spills[--scratch->spillCount].ptr);
    scratch->previous = g_CurrentScratch;
    g_CurrentScratch = scratch;
}

void ArenaScratchEnd(ArenaScratch* scratch)
{
    // free any spills the scope leaked (balanced push/pop or save/restore should leave none)
    while (scratch->spillCount > 0)
        DeAllocateTLSFGlobal(scratch->spills[--scratch->spillCount].ptr);
    scratch->currOffset = 0;
    g_CurrentScratch = scratch->previous;
    scratch->previous = NULL;
}

bool ArenaBeginScratch(ArenaScratch* scratch, size_t size, const char* name)
{
    if (!ArenaScratchCreate(scratch, size, name))
        return false;
    ArenaScratchBegin(scratch);
    return true;
}

void ArenaEndScratch(ArenaScratch* scratch)
{
    ArenaScratchEnd(scratch);
    ArenaScratchDestroy(scratch);
}

void ArenaInit(Arena* a, size_t backing_buffer_length)
{
    size_t aligned_size = OSRoundToPage(backing_buffer_length);
    a->buf        = (char*)OSAlloc(aligned_size);
    a->buffLen    = aligned_size;
    a->currOffset = 0;

    if (!a->buf)
    {
        AX_ERROR("ArenaInit failed size=%llu", (u64)aligned_size);
        ASSERT(false);
    }
}

void ArenaFree(Arena* a)
{
    if (a->buf)
    {
        OSFree(a->buf, a->buffLen);
        a->buf        = NULL;
        a->buffLen    = 0;
        a->currOffset = 0;
    }
}

void ArenaReset(Arena* a)
{
    a->currOffset = 0;
}

void* ArenaAllocAlign(Arena* a, size_t size, size_t align)
{
    ASSERT(a->buf);
    ASSERT((align & (align - 1u)) == 0);

    size_t curr_ptr = (size_t)a->buf + a->currOffset;
    size_t offset   = AlignAddress(curr_ptr, align);
    offset         -= (size_t)a->buf;

    if (offset > a->buffLen || size > a->buffLen - offset)
    {
        AX_ERROR("ArenaAllocAlign failed size=%llu align=%llu curr=%llu offset=%llu capacity=%llu",
                 (u64)size,
                 (u64)align,
                 (u64)a->currOffset,
                 (u64)offset,
                 (u64)a->buffLen);
        ASSERT(false);
        return NULL;
    }

    void* ptr     = &a->buf[offset];
    a->currOffset = offset + size;
    return ptr;
}

void ArenaPopAligned(Arena* a, void* ptr, size_t size, size_t align)
{
    (void)ptr;
    ASSERT((align & (align - 1u)) == 0);

    size_t curr_ptr = (size_t)a->buf + a->currOffset;
    if (size > a->currOffset)
    {
        AX_ERROR("ArenaPopAligned underflow size=%llu curr=%llu", (u64)size, (u64)a->currOffset);
        a->currOffset = 0;
        return;
    }

    size_t aligned = AlignAddress((size_t)a->buf + (a->currOffset - size), align);
    size_t padded  = curr_ptr - aligned;

    ASSERT(a->currOffset >= padded);
    a->currOffset -= padded;
}

void* ArenaAlloc(Arena* a, size_t size)
{
    return ArenaAllocAlign(a, size, DEFAULT_ALIGN);
}

void* ArenaAllocZero(Arena* a, size_t size)
{
    void* ptr = ArenaAllocAlign(a, size, DEFAULT_ALIGN);
    if (ptr) MemSet(ptr, 0, size);
    return ptr;
}

size_t ArenaRemaining(Arena* a)
{
    ASSERT(a->currOffset <= a->buffLen);
    return a->buffLen - a->currOffset;
}

ArenaMark ArenaSave(Arena* a)
{
    ASSERT(a);
    ArenaMark mark;
    mark.offset = a->currOffset;
    return mark;
}

void ArenaRestore(Arena* a, ArenaMark mark)
{
    ASSERT(mark.offset <= a->currOffset);
    ASSERT(mark.offset <= a->buffLen);
    a->currOffset = mark.offset;
}

uint64_t ArenaRemainingCurrent(void)
{
    ArenaScratch* s = g_CurrentScratch;
    if (s) return s->buffLen - s->currOffset;     // bump space; big allocs spill to tlsf beyond this
    CheckArenaSize();
    ASSERT(GlobalArena.currOffset <= GlobalArena.buffLen);
    return GlobalArena.buffLen - GlobalArena.currOffset;
}

uint64_t ArenaGetCurrentOffset(void)
{
    ArenaScratch* s = g_CurrentScratch;
    if (s) return s->currOffset;
    CheckArenaSize();
    return GlobalArena.currOffset;
}

void ArenaSetCurrentOffset(size_t offset)
{
    ArenaScratch* s = g_CurrentScratch;
    if (s)
    {
        // restore the bump pointer and release every spill made at or after this mark (the most
        // recent suffix of the spill stack), so a save/restore scope frees its spilled allocations too
        while (s->spillCount > 0 && s->spills[s->spillCount - 1].offset >= offset)
            DeAllocateTLSFGlobal(s->spills[--s->spillCount].ptr);
        if (offset > s->buffLen) offset = s->buffLen;
        s->currOffset = offset;
        return;
    }
    CheckArenaSize();
    if (offset > GlobalArena.buffLen)
    {
        AX_ERROR("ArenaSetCurrentOffset invalid offset=%llu capacity=%llu", (u64)offset, (u64)GlobalArena.buffLen);
        offset = GlobalArena.buffLen;
    }
    GlobalArena.currOffset = offset;
}

void* ArenaPushGlobal(uint64_t size)
{
    ArenaScratch* s = g_CurrentScratch;
    if (s)
    {
        // small allocations bump; anything that doesn't fit spills to the (thread-safe) tlsf heap so
        // the scratch buffer can stay small. The spill records the bump offset so pop/restore can
        // distinguish a spill from a bump allocation.
        if (size <= s->buffLen - s->currOffset)
        {
            void* result = s->buf + s->currOffset;
            s->currOffset += size;
            return result;
        }
        if (s->spillCount >= ARENA_SCRATCH_MAX_SPILLS)
        {
            AX_ERROR("Arena scratch spill overflow %s: requested=%llu spills=%u", s->name, (u64)size, s->spillCount);
            return NULL;
        }
        AX_WARN("arena spilled: %s", s->name);
        void* result = AllocateTLSFGlobal(size);
        if (!result) return NULL;
        s->spills[s->spillCount].offset = s->currOffset;
        s->spills[s->spillCount].ptr    = result;
        s->spillCount++;
        return result;
    }

    CheckArenaSize();
    if (GlobalArena.currOffset > GlobalArena.buffLen ||
        size > GlobalArena.buffLen - GlobalArena.currOffset)
    {
        AX_ERROR("Arena push failed: requested=%llu curr=%llu capacity=%llu",
                 (u64)size, (u64)GlobalArena.currOffset, (u64)GlobalArena.buffLen);
        return NULL;
    }

    void* result = GlobalArena.buf + GlobalArena.currOffset;
    GlobalArena.currOffset += size;
    return result;
}

void ArenaPopGlobal(uint64_t size)
{
    ArenaScratch* s = g_CurrentScratch;
    if (s)
    {
        // The most recent allocation was a spill iff the top spill was made at the current offset
        // (a later bump would have advanced the offset past it). Otherwise it was a bump allocation.
        if (s->spillCount > 0 && s->spills[s->spillCount - 1].offset == s->currOffset)
            DeAllocateTLSFGlobal(s->spills[--s->spillCount].ptr);
        else
        {
            if (size > s->currOffset)
            {
                AX_WARN("scratch %s trying to free more than allocated: pop=%llu curr=%llu", s->name, (u64)size, (u64)s->currOffset);
                size = s->currOffset;
            }
            s->currOffset -= size;
        }
        return;
    }

    CheckArenaSize();
    if (GlobalArena.currOffset < size)
    {
        AX_WARN("arena trying to free more than allocated: pop=%llu curr=%llu",
                (u64)size, (u64)GlobalArena.currOffset);
        size = GlobalArena.currOffset;
    }
    GlobalArena.currOffset -= size;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                          FixedPow2Allocator                              */
/*//////////////////////////////////////////////////////////////////////////*/

void FixedPow2Allocator_Init(FixedPow2Allocator* alloc, size_t initialSize)
{
    ASSERT(alloc);
    ASSERT(initialSize);
    ASSERT((initialSize & (initialSize - 1u)) == 0);

    alloc->currentCapacity = initialSize;
    alloc->base = (FixedFragment*)AllocateTLSFGlobal(sizeof(FixedFragment));
    ASSERT(alloc->base);

    alloc->current = alloc->base;
    alloc->base->next = NULL;
    alloc->base->ptr = (char*)AllocateTLSFGlobal(initialSize);
    alloc->base->size = 0;
    ASSERT(alloc->base->ptr);
}

void FixedPow2Allocator_CheckFixGrow(FixedPow2Allocator* alloc, size_t countBytes)
{
    ASSERT(alloc);
    ASSERT(alloc->current);

    size_t newSize = 0;
    if (AddSizeOverflow(alloc->current->size, countBytes, &newSize))
    {
        AX_ERROR("FixedPow2Allocator size overflow current=%llu add=%llu",
                 (u64)alloc->current->size, (u64)countBytes);
        ASSERT(false);
        return;
    }

    if (newSize >= alloc->currentCapacity)
    {
        while (alloc->currentCapacity < newSize)
        {
            if (alloc->currentCapacity > (SIZE_MAX >> 1u))
            {
                AX_ERROR("FixedPow2Allocator capacity overflow capacity=%llu needed=%llu",
                         (u64)alloc->currentCapacity, (u64)newSize);
                ASSERT(false);
                return;
            }

            alloc->currentCapacity <<= 1u;
        }

        alloc->current->next = (FixedFragment*)AllocateTLSFGlobal(sizeof(FixedFragment));
        ASSERT(alloc->current->next);
        alloc->current = alloc->current->next;
        alloc->current->next = NULL;
        alloc->current->ptr = (char*)AllocateTLSFGlobal(alloc->currentCapacity);
        alloc->current->size = 0;
        ASSERT(alloc->current->ptr);
    }
}

void* FixedPow2Allocator_Allocate(FixedPow2Allocator* alloc, size_t countBytes)
{
    FixedPow2Allocator_CheckFixGrow(alloc, countBytes);
    void* ptr = alloc->current->ptr + alloc->current->size;
    alloc->current->size += countBytes;
    MemsetZero(ptr, countBytes);
    return ptr;
}

void* FixedPow2Allocator_AllocateUninitialized(FixedPow2Allocator* alloc, size_t countBytes)
{
    FixedPow2Allocator_CheckFixGrow(alloc, countBytes);
    void* ptr = alloc->current->ptr + alloc->current->size;
    alloc->current->size += countBytes;
    
    #if defined(_DEBUG) || defined(DEBUG) || defined(Debug)
    MemSet(ptr, 0xCD, countBytes);
    #endif
    return ptr;
}

void FixedPow2Allocator_Copy(FixedPow2Allocator* alloc, const FixedPow2Allocator* other)
{
    ASSERT(alloc);

    if (!other || !other->base)
        return;

    size_t totalSize = 0;
    FixedFragment* start = other->base;

    while (start)
    {
        if (AddSizeOverflow(totalSize, start->size, &totalSize))
        {
            AX_ERROR("FixedPow2Allocator_Copy total size overflow");
            ASSERT(false);
            return;
        }

        start = start->next;
    }

    size_t capacity = 1;
    while (capacity < totalSize)
    {
        if (capacity > (SIZE_MAX >> 1u))
        {
            AX_ERROR("FixedPow2Allocator_Copy capacity overflow total=%llu", (u64)totalSize);
            ASSERT(false);
            return;
        }

        capacity <<= 1u;
    }

    alloc->currentCapacity = capacity;
    alloc->base = (FixedFragment*)AllocZeroTLSFGlobal(1, sizeof(FixedFragment));
    ASSERT(alloc->base);

    alloc->base->next = NULL;
    alloc->base->ptr = (char*)AllocateTLSFGlobal(alloc->currentCapacity);
    alloc->base->size = totalSize;
    alloc->current = alloc->base;

    ASSERT(alloc->base->ptr);

    char* curr = alloc->base->ptr;
    start = other->base;

    while (start)
    {
        SmallMemCpy(curr, start->ptr, start->size);
        curr += start->size;
        start = start->next;
    }
}

void* FixedPow2Allocator_TakeOwnership(FixedPow2Allocator* alloc)
{
    ASSERT(alloc);
    void* result = alloc->base;
    alloc->base = NULL;
    alloc->current = NULL;
    alloc->currentCapacity = 0;
    return result;
}

void FixedPow2Allocator_Destroy(FixedPow2Allocator* alloc)
{
    if (!alloc || !alloc->base)
        return;

    while (alloc->base)
    {
        DeAllocateTLSFGlobal(alloc->base->ptr);

        FixedFragment* oldBase = alloc->base;
        alloc->base = alloc->base->next;

        DeAllocateTLSFGlobal(oldBase);
    }

    alloc->current = NULL;
    alloc->currentCapacity = 0;
}
