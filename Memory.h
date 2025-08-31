#ifndef MEMORY_INCLUDED
#define MEMORY_INCLUDED

#include "Common.h"
#include "Algorithm.h"
#include "Extern/rpmalloc.h"

// Shift the given address upwards if/as necessary to// ensure it is aligned to the given number of bytes.
static inline uint64_t AlignAddress(uint64_t addr, uint64_t align)
{
    const uint64_t mask = align - 1;
    ASSERT((align & mask) == 0); // pwr of 2
    return (addr + mask) & ~mask;
}

// Shift the given pointer upwards if/as necessary to// ensure it is aligned to the given number of bytes.
static inline void* AlignPointer(void* ptr, uint64_t align)
{
    const uint64_t addr = (uint64_t)ptr;
    const uint64_t addrAligned = AlignAddress(addr, align);
    return (void*)addrAligned;
}

// Aligned allocation function. IMPORTANT: 'align'// must be a power of 2 (typically 4, 8 or 16).
static inline void* AllocAligned(uint64_t bytes, uint64_t align)
{
    // Allocate 'align' more bytes than we need.
    uint64_t  actualBytes = bytes + align;
    // Allocate unaligned block.
    uint8_t* pRawMem = rpmalloc(actualBytes);
    // Align the block. If no alignment occurred,// shift it up the full 'align' bytes so we// always have room to store the shift.
    uint8_t* pAlignedMem = AlignPointer(pRawMem, align);
    
    if (pAlignedMem == pRawMem)
        pAlignedMem += align;
    // Determine the shift, and store it.// (This works for up to 256-byte alignment.)
    uint8_t shift = (uint8_t)(pAlignedMem - pRawMem);
    ASSERT(shift > 0 && shift <= 256);
    pAlignedMem[-1] = (uint8_t)(shift & 0xFF);
    return pAlignedMem;
}

static inline void FreeAligned(void* pMem)
{
    ASSERT(pMem);
    // Convert to U8 pointer.
    uint8_t* pAlignedMem = (uint8_t*)pMem;
    // Extract the shift.
    uint64_t  shift = pAlignedMem[-1];
    if (shift == 0)
        shift = 256;
    // Back up to the actual allocated address,
    uint8_t* pRawMem = pAlignedMem - shift;
    rpfree(pRawMem);
}


// if you want memmove it is here with simd version: https://hackmd.io/@AndybnA/0410
static inline void MemSetAligned64(void* RESTRICT dst, unsigned char val, uint64_t sizeInBytes)
{
    // we want an offset because the while loop below iterates over 4 uint64_t at a time
    const uint64_t * end = (uint64_t *)((char*)dst + (sizeInBytes >> 3));
    uint64_t * dp = (uint64_t *)dst;
    uint64_t d8 = val * 0x0101010101010101ull; // replicate value to each byte.

    while (dp < end)
    {
        dp[0] = dp[1] = dp[2] = dp[3] = d8;
        dp += 4;
    }
   
    switch (sizeInBytes & 7)
    {
        case 7: *dp++ = d8;
        case 6: *dp++ = d8;
        case 5: *dp++ = d8;
        case 4: *dp++ = d8;
        case 3: *dp++ = d8;
        case 2: *dp++ = d8;
        case 1: *dp   = d8;
    };
}

static inline void MemSet32(uint32_t* RESTRICT dst, uint32_t val, uint32_t numValues)
{
    for (uint32_t i = 0; i < numValues; i++)
        dst[i] = val;
}

static inline void MemSet64(uint64_t* RESTRICT dst, uint64_t val, uint32_t numValues)
{
    for (uint32_t i = 0; i < numValues; i++)
        dst[i] = val;
}

static inline void MemSetAligned32(uint32_t* RESTRICT dst, unsigned char val, uint64_t sizeInBytes)
{
    const uint32_t* end = (uint32_t*)((char*)dst + (sizeInBytes >> 2));
    uint32_t* dp = (uint32_t*)dst;
    uint32_t  d4 = val * 0x01010101u; // replicate value to each byte
    
    while (dp < end)
    {
        dp[0] = dp[1] = dp[2] = dp[3] = d4;
        dp += 4;
    }
    
    switch (sizeInBytes & 3)
    {
        case 3: *dp++ = d4;
        case 2: *dp++ = d4;
        case 1: *dp   = d4;
    };
}

// use size for struct/class types such as Vector3 and Matrix4, 
// leave as zero size constant for big arrays or unknown size arrays
static inline void MemSet(void* dst, unsigned char val, uint64_t sizeInBytes, int alignment)
{
    if (alignment == 8)
        MemSetAligned64(dst, val, sizeInBytes);
    else if (alignment == 4)
        MemSetAligned32((uint32_t*)dst, val, sizeInBytes);
    else
    {
        uint64_t  uptr = (uint64_t )dst;
        if (!(uptr & 7) && uptr >= 8) MemSetAligned64(dst, val, sizeInBytes);
        else if (!(uptr & 3) && uptr >= 4)  MemSetAligned32((uint32_t*)dst, val, sizeInBytes);
        else
        {
            unsigned char* dp = (unsigned char*)dst;
            while (sizeInBytes--)
                *dp++ = val;
        }
    }
}

static inline void MemCpyAligned64(void* dst, const void* RESTRICT src, uint64_t sizeInBytes)
{
    uint64_t *       dp  = (uint64_t *)dst;
    const uint64_t * sp  = (const uint64_t *)src;
    const uint64_t * end = (const uint64_t *)((char*)src) + (sizeInBytes >> 3);
        
    while (sp < end)
    {
        dp[0] = sp[0];
        dp[1] = sp[1];
        dp[2] = sp[2];
        dp[3] = sp[3];
        dp += 4, sp += 4;
    }

    SmallMemCpy(dp, sp, sizeInBytes & 7);
}

static inline void MemCpyAligned32(uint32_t* dst, const uint32_t* RESTRICT src, uint64_t sizeInBytes)
{
    uint32_t*       dp  = (uint32_t*)dst;
    const uint32_t* sp  = (const uint32_t*)src;
    const uint32_t* end = (const uint32_t*)((char*)src) + (sizeInBytes >> 2);
        
    while (sp < end)
    {
        dp[0] = sp[0];
        dp[1] = sp[1];
        dp[2] = sp[2];
        dp[3] = sp[3];
        dp += 4, sp += 4;
    }
    
    switch (sizeInBytes & 3)
    {
        case 3: *dp++ = *sp++;
        case 2: *dp++ = *sp++;
        case 1: *dp++ = *sp++;
    };
}

// use size for structs and classes such as Vector3 and Matrix4,
// and use MemCpy for big arrays or unknown size arrays
static inline void MemCpy(void* dst, const void* RESTRICT src, uint64_t sizeInBytes, int alignment)
{
    if (alignment == 8)
        MemCpyAligned64(dst, src, sizeInBytes);
    else if (alignment == 4)
        MemCpyAligned32((uint32_t*)dst, (const uint32_t*)src, sizeInBytes);
    else if (alignment == 16)
    {
        #if defined(AX_SUPPORT_SSE) 
        const __m128* srcV = (__m128* )src;
        __m128* dstV = (__m128* )dst;
        #elif defined(AX_ARM)
        const uint32x4_t* srcV = (uint32x4_t* )src;
        uint32x4_t* dstV = (uint32x4_t* )dst;
        #else
        struct vType { uint64_t a, b; };
        const vType* srcV = (vType* )src;
        vType* dstV = (vType* )dst;
        #endif

        while (sizeInBytes >= 16)
        {
            *dstV++ = *srcV++;
            sizeInBytes -= 16;
        }
    }
    else
    {
        const char* cend = (char*)((char*)src + sizeInBytes);
        const char* scp = (const char*)src;
        char* dcp = (char*)dst;
        
        while (scp < cend) *dcp++ = *scp++;
    }
}

typedef struct FixedFragment_
{
    struct FixedFragment_* next;
    char* ptr;
    size_t size; // used like an index until we fill the fragment (in bytes)
} FixedFragment;

typedef struct FixedPow2Allocator_
{
    size_t currentCapacity;   // capacity in bytes
    FixedFragment* base;
    FixedFragment* current;
} FixedPow2Allocator;

static inline void FixedPow2Allocator_Init(FixedPow2Allocator* alloc, size_t initialSize)
{
    // WARNING initial size must be power of two
    ASSERT((initialSize & (initialSize - 1)) == 0);
    alloc->currentCapacity = initialSize;
    alloc->base = rpmalloc(sizeof(FixedFragment));
    alloc->current = alloc->base;
    alloc->base->next = NULL;
    alloc->base->ptr = rpmalloc(initialSize);
    alloc->base->size = 0;
}

static inline void FixedPow2Allocator_CheckFixGrow(FixedPow2Allocator* alloc, size_t countBytes)
{
    int64_t newSize = alloc->current->size + countBytes;
    if (newSize >= alloc->currentCapacity)
    {
        while (alloc->currentCapacity < newSize)
            alloc->currentCapacity <<= 1;

        alloc->current->next = rpmalloc(sizeof(FixedFragment));
        alloc->current = alloc->current->next;
        alloc->current->next = NULL;
        alloc->current->ptr = rpmalloc(alloc->currentCapacity);
        alloc->current->size = 0;
    }
}

static inline void* FixedPow2Allocator_Allocate(FixedPow2Allocator* alloc, size_t countBytes)
{
    FixedPow2Allocator_CheckFixGrow(alloc, countBytes);
    void* ptr = alloc->current->ptr + alloc->current->size;
    alloc->current->size += countBytes;
    MemsetZero(ptr, countBytes);
    return ptr;
}

static inline void* FixedPow2Allocator_AllocateUninitialized(FixedPow2Allocator* alloc, size_t countBytes)
{
    FixedPow2Allocator_CheckFixGrow(alloc, countBytes);
    void* ptr = alloc->current->ptr + alloc->current->size;
    alloc->current->size += countBytes;
    return ptr;
}

static inline void FixedPow2Allocator_Copy(FixedPow2Allocator* alloc, FixedPow2Allocator* other)
{
    if (!other->base) return;

    size_t totalSize = 0;
    FixedFragment* start = other->base;
    while (start)
    {
        totalSize += start->size;
        start = start->next;
    }

    alloc->currentCapacity = 1ULL << (64 - LeadingZeroCount64(totalSize));
    alloc->base = rpcalloc(1, sizeof(FixedFragment));
    alloc->base->next = NULL;
    alloc->base->ptr = rpmalloc(alloc->currentCapacity);
    alloc->base->size = totalSize;
    alloc->current = alloc->base;

    // copy other's memory
    char* curr = alloc->base->ptr;
    start = other->base;
    while (start)
    {
        SmallMemCpy(curr, start->ptr, start->size);
        curr += start->size;
        start = start->next;
    }
}

static inline void* FixedPow2Allocator_TakeOwnership(FixedPow2Allocator* alloc)
{
    void* result = alloc->base;
    alloc->base = NULL;
    return result;
}

static inline void FixedPow2Allocator_Destroy(FixedPow2Allocator* alloc)
{
    if (!alloc->base) return;
    while (alloc->base)
    {
        rpfree(alloc->base->ptr);
        FixedFragment* oldBase = alloc->base;
        alloc->base = alloc->base->next;
        rpfree(oldBase);
    }
}

#endif