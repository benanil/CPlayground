#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <float.h>
#include <stddef.h>
#include <limits.h>
#include "SIMD.h"

#if defined(__cplusplus)
extern "C" {
#endif


#define KB 1024L
#define MB (1024L * 1024L)
#define GB (1024L * 1024L * 1024L)

#if defined(__has_builtin)
    #define AX_COMPILER_HAS_BUILTIN(x) __has_builtin(x)
#else
    #define AX_COMPILER_HAS_BUILTIN(x) 0
#endif

#if defined(_MSC_VER)
    #define ALIGNAS(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
    #define ALIGNAS(n) __attribute__((aligned(n)))
#endif

#if defined(_MSC_VER)
	#define ALIGNSIMD __declspec(align(SIMD_NUM_BYTES))
#elif defined(__GNUC__) || defined(__clang__)
	#define ALIGNSIMD __attribute__((aligned(SIMD_NUM_BYTES)))
#endif

#if AX_COMPILER_HAS_BUILTIN(__builtin_assume)
    #define AX_ASSUME(x) __builtin_assume(x)
#elif defined(_MSC_VER)
    #define AX_ASSUME(x) __assume(x)
#else
    #define AX_ASSUME(x) (void)(x)
#endif

#if defined(__cplusplus) &&  __cplusplus >= 201103L
   #define AX_THREAD_LOCAL       thread_local
#elif defined(__GNUC__) && __GNUC__ < 5
   #define AX_THREAD_LOCAL       __thread
#elif defined(_MSC_VER)
   #define AX_THREAD_LOCAL       __declspec(thread)
#elif defined (__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
   #define AX_THREAD_LOCAL       _Thread_local
#endif

#if defined(__GNUC__)
    #define AX_PACK(decl) decl __attribute__((__packed__))
#elif defined(_MSC_VER)
    #define AX_PACK(decl) __pragma(pack(push, 1)); decl __pragma(pack(pop));
#else
    #error you should define pack function
#endif

#if defined(_MSC_VER)       /* MSVC */
#  define AX_ALIGN(N) __declspec(align(N))
#elif defined(__GNUC__)     /* GCC, Clang */
#  define AX_ALIGN(N) __attribute__((aligned(N)))
#elif defined(__INTEL_COMPILER) /* Intel C Compiler */
#  define AX_ALIGN(N) __attribute__((aligned(N)))
#else                       /* Unknown compiler, no alignment */
#  define AX_ALIGN(N)
#endif

#define ALIGNOF(type) offsetof(struct { char c; type t; }, t)

#if defined(__GNUC__) || defined(__MINGW32__)
    #define RESTRICT __restrict__
#elif defined(_MSC_VER)
    #define RESTRICT __restrict
#else
    #define RESTRICT
#endif

#if _MSC_VER
    #define AXGLOBALCONST extern const __declspec(selectany)
#elif defined(__GNUC__) && !defined(__MINGW32__)
    #define AXGLOBALCONST extern const __attribute__((weak))
#else 
    #define AXGLOBALCONST extern const 
#endif

#if defined(__GNUC__) || defined(__INTEL_COMPILER) || defined(__clang__)
    #define AX_LIKELY(x) __builtin_expect(x, 1)  
    #define AX_UNLIKELY(x) __builtin_expect(x, 0)
#else
    #define AX_LIKELY(x) (x)   
    #define AX_UNLIKELY(x) (x) 
#endif



// https://nullprogram.com/blog/2022/06/26/
#if defined(_DEBUG) || defined(Debug)
    #if __GNUC__
        #define ASSERT(c) if (!(c)) { __builtin_trap(); }
    #elif _MSC_VER
        #define ASSERT(c) if (!(c)) { __debugbreak(); }
    #else
        #define ASSERT(c) if (!(c)) { *(volatile int *)0 = 0; }
    #endif
#else
    #define ASSERT(c)
#endif

#if defined(_DEBUG) || defined(Debug)
    #if __GNUC__
        #define ASSERTR(c, r) if (!(c)) { __builtin_trap(); r; }
    #elif _MSC_VER
        #define ASSERTR(c, r) if (!(c)) { __debugbreak(); r; }
    #else
        #define ASSERTR(c, r) if (!(c)) { *(volatile int *)0 = 0; r; }
    #endif
#else
    #define ASSERTR(c, r) if (!(c)) { r; }
#endif

#ifndef STATIC_ASSERT
    #if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
        #define STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
    #else
        #ifdef __COUNTER__
            #define STATIC_ASSERT(cond, msg) typedef char static_assert_##__COUNTER__[(cond) ? 1 : -1]
        #else
            #define STATIC_ASSERT(cond, msg) typedef char static_assert_##__LINE__[(cond) ? 1 : -1]
        #endif
    #endif
#endif

#if AX_COMPILER_HAS_BUILTIN(__builtin_prefetch)
    #define AX_PREFETCH(x) __builtin_prefetch(x)
#elif defined(_MSC_VER)
    #define AX_PREFETCH(x) _mm_prefetch(x, _MM_HINT_T0)
#else
    #define AX_PREFETCH(x)
#endif

#if AX_COMPILER_HAS_BUILTIN(__builtin_unreachable)
    #define AX_UNREACHABLE() __builtin_unreachable()
#elif _MSC_VER
    #define AX_UNREACHABLE() __assume(0)
#else
    #define AX_UNREACHABLE() 
#endif


//------------------------------------------------------------------------
// Determinate Operating System

// Check windows
#if defined(_WIN32) || defined(_WIN64)
    #define PLATFORM_WINDOWS 1
#endif

// Check unix
#if defined(unix) || defined(__unix__) || defined(__unix) || defined(__APPLE__)
    #define PLATFORM_UNIX 1
#endif

// Check Linux
#if defined(linux) || defined(__linux)
    #define PLATFORM_LINUX 1
#endif

// Check macos
#if defined(__APPLE__)
    #define PLATFORM_UNIX 1
    #define PLATFORM_MACOSX 1
#endif

#if defined(__ANDROID__)
    #define PLATFORM_ANDROID 1
#endif

#ifndef AX_NO_UNROLL
    #if defined(__clang__)
        #define AX_NO_UNROLL _Pragma("clang loop unroll(disable)") _Pragma("clang loop vectorize(disable)")
    #elif defined(__GNUC__) && __GNUC__ >= 8
        #define AX_NO_UNROLL _Pragma("GCC unroll 0")
    #elif defined(_MSC_VER)
        #define AX_NO_UNROLL __pragma(loop(no_vector))
    #else
        #define AX_NO_UNROLL
    #endif
#endif

//------------------------------------------------------------------------
// Memory Operations:  memcpy, memset, unaligned load

// __movsb/__stosb are MSVC/clang-cl intrinsics, plain gcc/clang on x86 lacks them
#if defined(AX_SUPPORT_SSE) && defined(_MSC_VER)
    #define SmallMemCpy(dst, src, size) __movsb((unsigned char*)(dst), (unsigned char*)(src), size)
#else
    #define SmallMemCpy(dst, src, size) __builtin_memcpy(dst, src, size)
#endif

#if defined(AX_SUPPORT_SSE) && defined(_MSC_VER)
    #define SmallMemSet(dst, val, size) __stosb((unsigned char*)(dst), val, size)
#else
    #define SmallMemSet(dst, val, size) __builtin_memset(dst, val, size)
#endif

#define MemsetZero(dst, size) SmallMemSet(dst, 0, size)

#if defined(_MSC_VER) && !defined(__clang__)
    #if defined(_M_IX86) //< The __unaligned modifier isn't valid for the x86 platform.
        #define UnalignedLoad64(ptr) *((uint64_t*)(ptr))
    #else
        #define UnalignedLoad64(ptr) *((__unaligned uint64_t*)(ptr))
    #endif
#else
    purefn uint64_t UnalignedLoad64(void const* ptr) {
        __attribute__((aligned(1))) uint64_t const *result = (uint64_t const *)ptr;
        return *result;
    }
#endif

#if defined(_MSC_VER) && !defined(__clang__)
    #if defined(_M_IX86) //< The __unaligned modifier isn't valid for the x86 platform.
        #define UnalignedLoad32(ptr) *((uint32_t*)(ptr))
    #else
        #define UnalignedLoad32(ptr) *((__unaligned uint32_t*)(ptr))
    #endif
#else
    purefn uint32_t UnalignedLoad32(void const* ptr) {
        __attribute__((aligned(1))) uint32_t const *result = (uint32_t const *)ptr;
        return *result;
    }
#endif

#define UnalignedLoadWord(x) (sizeof(unsigned long long) == 8 ? UnalignedLoad64(x) : UnalignedLoad32(x))


//------------------------------------------------------------------------
// Bit Operations

#if defined(_MSC_VER)     /* Visual Studio */
    #define AX_BSWAP32(x) _byteswap_ulong(x)
#elif (defined (__GNUC__) && (__GNUC__ * 100 + __GNUC_MINOR__ >= 403)) \
|| (defined(__clang__) && __has_builtin(__builtin_bswap32))
    #define AX_BSWAP32(x) __builtin_bswap32(x)
#else
purefn uint32_t AX_BSWAP32(uint32_t x) {
    return ((x << 24) & 0xff000000 ) |
           ((x <<  8) & 0x00ff0000 ) |
           ((x >>  8) & 0x0000ff00 ) |
           ((x >> 24) & 0x000000ff );
}
#endif

#if defined(_MSC_VER)
    #define ByteSwap32(x) _byteswap_ulong(x)
    #define ByteSwap64(x) _byteswap_uint64(x)
#elif (defined (__GNUC__) && (__GNUC__ * 100 + __GNUC_MINOR__ >= 403)) \
|| (defined(__clang__) && __has_builtin(__builtin_bswap32))
    #define ByteSwap32(x) __builtin_bswap32(x)
    #define ByteSwap64(x) __builtin_bswap64(x)
#else
#define ByteSwap32(x) AX_BSWAP32(x)
purefn uint64_t ByteSwap64(uint64_t x) {
    return ((x << 56) & 0xff00000000000000ULL) |
           ((x << 40) & 0x00ff000000000000ULL) |
           ((x << 24) & 0x0000ff0000000000ULL) |
           ((x << 8)  & 0x000000ff00000000ULL) |
           ((x >> 8)  & 0x00000000ff000000ULL) |
           ((x >> 24) & 0x0000000000ff0000ULL) |
           ((x >> 40) & 0x000000000000ff00ULL) |
           ((x >> 56) & 0x00000000000000ffULL);
}
#endif

#define ByteSwapWord(x) (sizeof(unsigned long long) == 8 ? ByteSwap64(x) : ByteSwap32(x))

// according to intel intrinsic, popcnt instruction is 3 cycle (equal to mulps, addps) 
// throughput is even double of mulps and addps which is 1.0 (%100)
// https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html

#if defined(__GNUC__) || defined(__clang__)
    // covers arm/neon too, the compiler picks cnt/popcnt by target
    #define PopCount32(x) ((uint32_t)__builtin_popcount(x))
    #define PopCount64(x) ((uint64_t)__builtin_popcountll(x))
#elif defined(AX_SUPPORT_SSE)
    #define PopCount32(x) _mm_popcnt_u32(x)
    #define PopCount64(x) _mm_popcnt_u64(x)
#else

    purefn uint32_t PopCount32(uint32_t x) {
        x =  x - ((x >> 1) & 0x55555555);        // add pairs of bits
        x = (x & 0x33333333) + ((x >> 2) & 0x33333333);  // quads
        x = (x + (x >> 4)) & 0x0F0F0F0F;        // groups of 8
        return (x * 0x01010101) >> 24;          // horizontal sum of bytes	
    }

    // standard popcount; from wikipedia
    purefn uint64_t PopCount64(uint64_t x) {
        x -= ((x >> 1) & 0x5555555555555555ull);
        x = (x & 0x3333333333333333ull) + (x >> 2 & 0x3333333333333333ull);
        return ((x + (x >> 4)) & 0xf0f0f0f0f0f0f0full) * 0x101010101010101ull >> 56;
    }
#endif

// TrailingZeroCount32/64 are defined by SIMD.h (included above)
#define TrailingZeroCountWord(x) (sizeof(unsigned long long) == 8 ? TrailingZeroCount64(x) : TrailingZeroCount32(x))

#if defined(__GNUC__) || defined(__clang__)
    #define LeadingZeroCount32(x) __builtin_clz(x)
    #define LeadingZeroCount64(x) __builtin_clzll(x)
#elif defined(_MSC_VER) && !defined(AX_ARM)
    #define LeadingZeroCount32(x) _lzcnt_u32(x)
    #define LeadingZeroCount64(x) _lzcnt_u64(x)
#else
    #error "LeadingZeroCount is not defined for this compiler"
#endif


#define LeadingZeroCountWord(x) (sizeof(unsigned long long) == 8 ? LeadingZeroCount64(x) : LeadingZeroCount32(x))

#if !AX_COMPILER_HAS_BUILTIN(__builtin_bit_cast)
    #define BitCast(to, from) (*(to*)(&from))
#else
    #define BitCast(to, from) __builtin_bit_cast(to, (from))
#endif

purefn uint32_t NextSetBit(uint32_t* bits)
{
    *bits &= ~1;
    uint32_t tz = TrailingZeroCount32((uint32_t)*bits);
    *bits >>= tz;
    return tz;
}

purefn uint64_t NextSetBit64(uint64_t* bits)
{
    *bits &= ~1ull;
    uint64_t tz = TrailingZeroCount64((uint64_t)*bits);
    *bits >>= tz;
    return tz;
}

#define MIsPowerOfTwo(x) (((x) != 0) && (((x) & ((x) - 1)) == 0)) 

purefn int NextPowerOf2_32(int x) {
    x--;
    x |= x >> 1; x |= x >> 2; x |= x >> 4;
    x |= x >> 8; x |= x >> 16;
    return ++x;
}

purefn int64_t NextPowerOf2_64(int64_t x) {
    x--;
    x |= x >> 1; x |= x >> 2;  x |= x >> 4;
    x |= x >> 8; x |= x >> 16; x |= x >> 32;
    return ++x;
}


//------------------------------------------------------------------------
// Math Operations

#define MMIN(a, b) ((a) < (b) ? (a) : (b))
#define MMAX(a, b) ((a) > (b) ? (a) : (b))
#define MCLAMP(x, mn, mx) (MMIN((mx), MMAX((x), (mn))))
#define MCLAMP01(x) (MMIN((1.0f), MMAX((x), (0.0f))))

purefn f32 Saturatef32(f32 x) { return MMIN(1.0f, MMAX(x, 0.0f)); }
purefn f32 Clampf32(f32 x, f32 min, f32 max) { return MMIN(max, MMAX(x, min)); }
purefn f64 Clampf64(f64 x, f64 min, f64 max) { return MMIN(max, MMAX(x, min)); }
purefn s32 Clamps32(s32 x, s32 min, s32 max) { return MMIN(max, MMAX(x, min)); }
purefn f32 Minf32(f32 a, f32 b) { return a < b ? a : b; }
purefn f32 Maxf32(f32 a, f32 b) { return a > b ? a : b; }
purefn s32 Mins32(s32 a, s32 b) { return a < b ? a : b; }
purefn s32 Maxs32(s32 a, s32 b) { return a > b ? a : b; }
purefn u32 Minu32(u32 a, u32 b) { return a < b ? a : b; }
purefn u32 Maxu32(u32 a, u32 b) { return a > b ? a : b; }
purefn u64 Minu64(u64 a, u64 b) { return a < b ? a : b; }
purefn u64 Maxu64(u64 a, u64 b) { return a > b ? a : b; }

purefn int64_t Absu64(int64_t x) {
    int64_t temp = x >> 63;
    return (x ^ temp) - temp;
}

purefn int64_t Absi64(int64_t x) {
    int64_t temp = x >> 63;
    return (x ^ temp) - temp;
}

purefn int Abss32(int x) {
    int temp = x >> 31;
    return (x ^ temp) - temp;
}

purefn float Absf32(float x)
{
    int ix = BitCast(int, x) & 0x7FFFFFFF; // every bit except sign mask
    return BitCast(float, ix);
}

purefn double Absf64(double x)
{
    uint64_t  ix = BitCast(uint64_t, x) & (~(1ull << 63ull));// every bit except sign mask
    return BitCast(double, ix);
}

purefn f32 Floorf32(f32 x) {
    f32 whole = (f32)(s32)x;        // truncates toward zero
    return whole - (f32)(whole > x); // step down for negative non integers
}

purefn f64 Floor(f64 x) {
    f64 whole = (f64)(s64)x;        // truncates toward zero
    return whole - (f64)(whole > x); // step down for negative non integers
}

purefn f32 Ceilf(f32 x) {
    f32 whole = (f32)(s32)x;  // truncate quotient to integer
    return whole + (f32)(x > whole);
}

purefn f32 Fractf32(f32 a) {
    return a - Floorf32(a);
}

purefn f64 Fract(f64 a) {
    return a - Floor(a); 
}

purefn bool InRange(float x, float start, float length)
{
    return x > start && x < start + length;
}


//------------------------------------------------------------------------
// Other Util

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define PointerDistance(begin, end) ((u32)((char*)(end) - (char*)(begin)))

purefn bool IsAndroid()
{
    #ifdef __ANDROID__
    return true;
    #else
    return false;
    #endif
}

purefn int CalculateArrayGrowth(int _size)
{
    const int addition = _size >> 1;
    if (_size + addition < 0) {
        return INT32_MAX; // growth would overflow
    }
    return _size + addition; // growth is sufficient
}

// almost same performance with memcpy
static inline void MemCopy(void* dst, const void* RESTRICT src, size_t size) 
{
    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = (uint8_t*)dst;
    size_t simd_count = size >> 4;  // Divide by 16
    
    if (simd_count > 0)
    {
        switch (simd_count & 3)
        {
            case 3: VecStoreU((v128u*)d, VecLoadIU((const v128u*)s)); s += 16; d += 16;
            case 2: VecStoreU((v128u*)d, VecLoadIU((const v128u*)s)); s += 16; d += 16;
            case 1: VecStoreU((v128u*)d, VecLoadIU((const v128u*)s)); s += 16; d += 16;
            case 0: break;
        }
        
        simd_count >>= 2;
        while (simd_count--)
        {
            v128u xmm0 = VecLoadIU((const v128u*)s);
            v128u xmm1 = VecLoadIU((const v128u*)(s + 16));
            v128u xmm2 = VecLoadIU((const v128u*)(s + 32));
            v128u xmm3 = VecLoadIU((const v128u*)(s + 48));
            
            VecStoreU((v128u*)d, xmm0);
            VecStoreU((v128u*)(d + 16), xmm1);
            VecStoreU((v128u*)(d + 32), xmm2);
            VecStoreU((v128u*)(d + 48), xmm3);
            s += 64; d += 64;
        }
    }
    
    size_t r = size & 15;
    if (r >= 8) { *(uint64_t*)d = *(uint64_t*)s; d+=8; s+=8; r-=8; }
    if (r >= 4) { *(uint32_t*)d = *(uint32_t*)s; d+=4; s+=4; r-=4; }
    if (r >= 2) { *(uint16_t*)d = *(uint16_t*)s; d+=2; s+=2; r-=2; }
    if (r)      { *d = *s; }
}

static inline void MemSet(void* dst, uint8_t value, size_t size) 
{
    uint8_t* d = (uint8_t*)dst;
    
    v128u xmm_value = VecSetBytes(value);
    size_t simd_count = size >> 4;  // Divide by 16
    
    if (simd_count > 0) 
    {
        switch (simd_count & 3)
        {
            case 3: VecStoreU((v128u*)d, xmm_value); d += 16;
            case 2: VecStoreU((v128u*)d, xmm_value); d += 16;
            case 1: VecStoreU((v128u*)d, xmm_value); d += 16;
            case 0: break;
        }
        
        simd_count >>= 2;
        while (simd_count--)
        {
            VecStoreU((v128u*)d, xmm_value);
            VecStoreU((v128u*)(d + 16), xmm_value);
            VecStoreU((v128u*)(d + 32), xmm_value);
            VecStoreU((v128u*)(d + 48), xmm_value);
            d += 64;
        }
    }
    
    // Tail bytes
    size_t r = size & 15;
    if (r >= 8) { *(uint64_t*)d = (uint64_t)value * 0x0101010101010101ULL; d += 8; r -= 8; }
    if (r >= 4) { *(uint32_t*)d = (uint32_t)value * 0x01010101U; d += 4; r -= 4; }
    if (r >= 2) { *(uint16_t*)d = (uint16_t)value * 0x0101U; d += 2; r -= 2; }
    if (r) { *d = value; }
}


//------------------------------------------------------------------------
// String

#if (defined(__GNUC__) || defined(__clang__))
    #define StringLength(s) (int)__builtin_strlen(s)
#else
    // http://www.lrdev.com/lr/c/strlen.c
    purefn int StringLength(char const* s)
    {
        char const* p = s;
        const uint64_t m = 0x7efefefefefefeffull; 
        const uint64_t n = ~m;
        uint64_t i;

        for (; (uint64_t)p & (sizeof(uint64_t) - 1); p++) 
            if (!*p)
                return (int)(uint64_t)(p - s);

        for (;;) 
        {
            // memory is aligned from now on
            uint64_t i = *(const uint64_t*)p;

            if (!(((i + m) ^ ~i) & n)) {
                p += sizeof(uint64_t);
            }
            else
            {
                for (i = sizeof(uint64_t); i; p++, i--) 
                    if (!*p) return (int)(uint64_t)(p - s);
            }
        }
        return 0;
    }
 #endif

// Returns 0 if s is NULL or not null-terminated within maxLen
static inline int StringLengthSafe(const char* s, size_t maxLen)
{
    if (!s || maxLen == 0) return 0;

    const char* p = s;
    const size_t wordSize = sizeof(uint64_t);
    const uint64_t m = 0x7efefefefefefeffull;
    const uint64_t n = ~m;
    size_t remaining = maxLen;

    // Align pointer
    while (((uintptr_t)p & (wordSize - 1)) && remaining) {
        if (*p == '\0') return (int)(p - s);
        p++; remaining--;
    }

    // Aligned scanning
    while (remaining >= wordSize) {
        uint64_t chunk = *(const uint64_t*)p;
        if (!(((chunk + m) ^ ~chunk) & n)) {
            p += wordSize; remaining -= wordSize;
        } else {
            for (size_t i = 0; i < wordSize && remaining; i++, p++, remaining--)
                if (*p == '\0') return (int)(p - s);
        }
    }

    // Remaining bytes
    while (remaining--) {
        if (*p == '\0') return (int)(p - s);
        p++;
    }
    return 0; // unterminated
}

purefn const char* GetFileName(const char* path)
{
    int length = StringLength(path);
    while (length > 0 && path[length-1] != '\\' && path[length-1] != '/')
        length--;
    return path + length;
}

static inline bool IsMobilePlatform()
{
    #if defined(__ANDROID__)
    return true;
    #else
    return false;
    #endif
}

static inline bool IsDebugMode()
{
	#if defined(_DEBUG) || defined(DEBUG) || defined(Debug)
	return true; 
	#else 
	return false;
	#endif
}

#ifndef PLATFORM_MACOSX
    #ifndef AX_GPU_SHADER_FORMAT
        #define AX_GPU_SHADER_FORMAT SDL_GPU_SHADERFORMAT_SPIRV
        #define AX_GPU_COMPUTE_ENTRYPOINT "main"
    #endif
#else
    #define AX_GPU_SHADER_FORMAT SDL_GPU_SHADERFORMAT_MSL
    #define AX_GPU_COMPUTE_ENTRYPOINT "main"
#endif

#if defined(__cplusplus)
}
#endif


#endif // COMMON_H