#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <float.h>

#if defined(__x86_64__) || defined(_M_X64)
    #define AX_X64
#elif defined(__i386) || defined(_M_IX86)
    #define AX_X86
#elif defined(_M_ARM) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC) || __arm__ || __aarch64__
    #define AX_ARM
#endif

#if defined( _M_ARM64 ) || defined( __aarch64__ ) || defined( __arm64__ ) || defined(__ARM_NEON__)
    #define AX_SUPPORT_NEON
    #include <arm_fp16.h>
#endif

#if defined(AX_ARM)
    #if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC) || defined(__aarch64__))
        #include <arm64_neon.h>
    #else
        #include <arm_neon.h>
    #endif
#endif

/* Intrinsics Support */
#if (defined(AX_X64) || defined(AX_X86)) && !defined(AX_ARM)
    #if defined(_MSC_VER) && !defined(__clang__)
        #if _MSC_VER >= 1400 && !defined(AX_NO_SSE2)   /* 2005 */
            #define AX_SUPPORT_SSE
        #endif
        #if _MSC_VER >= 1700 && !defined(AX_NO_AVX2)   /* 2012 */
            #define AX_SUPPORT_AVX2
        #endif
    #else
        #if defined(__SSE2__) && !defined(AX_NO_SSE2)
            #define AX_SUPPORT_SSE
        #endif
        #if defined(__AVX2__) && !defined(AX_NO_AVX2)
            #define AX_SUPPORT_AVX2
        #endif
    #endif
    
    /* If at this point we still haven't determined compiler support for the intrinsics just fall back to __has_include. */
    #if !defined(__GNUC__) && !defined(__clang__) && defined(__has_include)
        #if !defined(AX_SUPPORT_SSE) && !defined(AX_NO_SSE2) && __has_include(<emmintrin.h>)
            #define AX_SUPPORT_SSE
        #endif
        #if !defined(AX_SUPPORT_AVX2) && !defined(AX_NO_AVX2) && __has_include(<immintrin.h>)
            #define AX_SUPPORT_AVX2
        #endif
    #endif

    #if defined(AX_SUPPORT_AVX2) || defined(AX_SUPPORT_AVX)
        #include <immintrin.h>
    #elif defined(AX_SUPPORT_SSE)
        #include <emmintrin.h>
    #endif
#endif

#if defined(__has_builtin)
    #define AX_COMPILER_HAS_BUILTIN(x) __has_builtin(x)
#else
    #define AX_COMPILER_HAS_BUILTIN(x) 0
#endif

#if AX_COMPILER_HAS_BUILTIN(__builtin_assume)
    #define AX_ASSUME(x) __builtin_assume(x)
#elif defined(_MSC_VER)
    #define AX_ASSUME(x) __assume(x)
#else
    #define AX_ASSUME(x) (void)(x)
#endif

#if defined(__clang__) || defined(__GNUC__)
    #define purefn inline __attribute__((pure))
#elif defined(_MSC_VER)
    #define purefn __forceinline __declspec(noalias)
#else
    #define purefn inline __attribute__((always_inline))
#endif

#if defined(__clang__) || defined(__GNUC__)
    #define forceinline inline
#elif defined(_MSC_VER)
    #define forceinline __forceinline 
#else
    #define forceinline inline __attribute__((always_inline))
#endif

#ifdef _MSC_VER
    #include <intrin.h>
    #define VECTORCALL __vectorcall
#elif __CLANG__
    #define VECTORCALL [[clang::vectorcall]] 
#elif __GNUC__
    #define VECTORCALL  
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
#  define ALIGN(N)
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
    #elif defined(__GNUC__) >= 8
        #define AX_NO_UNROLL _Pragma("GCC unroll 0")
    #elif defined(_MSC_VER)
        #define AX_NO_UNROLL __pragma(loop(no_vector))
    #else
        #define AX_NO_UNROLL
    #endif
#endif


//------------------------------------------------------------------------
// Memory Operations:  memcpy, memset, unaligned load

#ifdef _MSC_VER
    #define SmallMemCpy(dst, src, size) __movsb((unsigned char*)(dst), (unsigned char*)(src), size);
#else
    #define SmallMemCpy(dst, src, size) __builtin_memcpy(dst, src, size);
#endif

#ifdef _MSC_VER
    #define SmallMemSet(dst, val, size) __stosb((unsigned char*)(dst), val, size);
#else
    #define SmallMemSet(dst, val, size) __builtin_memset(dst, val, size);
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
    purefn uint64_t UnalignedLoad32(void const* ptr) {
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
inline uint32_t AX_BSWAP32(uint32_t x) {
    return ((in << 24) & 0xff000000 ) |
           ((in <<  8) & 0x00ff0000 ) |
           ((in >>  8) & 0x0000ff00 ) |
           ((in >> 24) & 0x000000ff );
}
#endif

#if defined(_MSC_VER) 
    #define ByteSwap32(x) _byteswap_uint64(x)
#elif (defined (__GNUC__) && (__GNUC__ * 100 + __GNUC_MINOR__ >= 403)) \
|| (defined(__clang__) && __has_builtin(__builtin_bswap32))
    #define ByteSwap64(x) __builtin_bswap64(x)
#else
purefn uint64_t ByteSwap(uint64_t x) {
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

#if defined(__ARM_NEON__)
    #define PopCount32(x) vcnt_u8((int8x8_t)x)
#elif defined(AX_SUPPORT_SSE)
    #define PopCount32(x) _mm_popcnt_u32(x)
    #define PopCount64(x) _mm_popcnt_u64(x)
#elif defined(__GNUC__) || !defined(__MINGW32__)
    #define PopCount32(x) __builtin_popcount(x)
    #define PopCount64(x) __builtin_popcountl(x)
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

#ifdef _MSC_VER
    #define TrailingZeroCount32(x) _tzcnt_u32(x)
    #define TrailingZeroCount64(x) _tzcnt_u64(x)
#elif defined(__GNUC__) || !defined(__MINGW32__)
    #define TrailingZeroCount32(x) __builtin_ctz(x)
    #define TrailingZeroCount64(x) __builtin_ctzll(x)
#else
    #define TrailingZeroCount32(x) PopCount64((x & -x) - 1u)
    #define TrailingZeroCount64(x) PopCount64((x & -x) - 1ull)
#endif

#define TrailingZeroCountWord(x) (sizeof(unsigned long long) == 8 ? TrailingZeroCount64(x) : TrailingZeroCount32(x))

#ifdef _MSC_VER
    #define LeadingZeroCount32(x) _lzcnt_u32(x)
    #define LeadingZeroCount64(x) _lzcnt_u64(x)
#elif AX_COMPILER_HAS_BUILTIN(__builtin_clz) || defined(__GNUC__) || !defined(__MINGW32__)
    #define LeadingZeroCount32(x) __builtin_clz(x)
    #define LeadingZeroCount64(x) __builtin_clzll(x)
#else
    #error "LeadingZeroCount64 is not exist!"
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

purefn float Clamp01f(float x) { return MMIN((1.0f), MMAX((x), (0.0f))); }

purefn float Minf(float a, float b) { return a < b ? a : b; }

purefn float Maxf(float a, float b) { return a > b ? a : b; }

purefn float Min32(int a, int b) { return a < b ? a : b; }

purefn float Max32(int a, int b) { return a > b ? a : b; }

purefn int64_t Abs64(int64_t x) {
    int64_t temp = x >> 63;
    return (x ^ temp) - temp;
}

purefn int Abs32(int x) {
    int temp = x >> 31;
    return (x ^ temp) - temp;
}

purefn float Absf(float x)
{
    int ix = BitCast(int, x) & 0x7FFFFFFF; // every bit except sign mask
    return BitCast(float, ix);
}

purefn double AbsD(double x)
{
    uint64_t  ix = BitCast(uint64_t, x) & (~(1ull << 63ull));// every bit except sign mask
    return BitCast(double, ix);
}

purefn bool InRange(float x, float start, float length)
{
    return x > start && x < start + length;
}


//------------------------------------------------------------------------
// Other Util

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define PointerDistance(begin, end) ((uint)((char*)(end) - (char*)(begin)) / sizeof(T))

purefn int CalculateArrayGrowth(int _size)
{
    const int addition = _size >> 1;
    if (_size + addition < 0) {
        return INT32_MAX; // growth would overflow
    }
    return _size + addition; // growth is sufficient
}


#if (defined(__GNUC__) || defined(__clang__))
    #define StringLength(s) (int)__builtin_strlen(s)
#else
    typedef unsigned long long unsignedLongLong;
    // http://www.lrdev.com/lr/c/strlen.c
    purefn int StringLength(char const* s)
    {
        char const* p = s;
        const unsignedLongLong m = 0x7efefefefefefeffull; 
        const unsignedLongLong n = ~m;

        for (; (unsignedLongLong)p & (sizeof(unsignedLongLong) - 1); p++) 
            if (!*p)
                return (int)(unsignedLongLong)(p - s);

        for (;;) 
        {
            // memory is aligned from now on
            unsignedLongLong i = *(const unsignedLongLong*)p;

            if (!(((i + m) ^ ~i) & n)) {
                p += sizeof(unsignedLongLong);
            }
            else
            {
                for (i = sizeof(unsignedLongLong); i; p++, i--) 
                    if (!*p) return (int)(unsignedLongLong)(p - s);
            }
        }
    }
#endif

purefn const char* GetFileName(const char* path)
{
    int length = StringLength(path);
    while (path[length-1] != '\\' && path[length-1] != '/' && length > 0) 
        length--;
    return path + length;
}

#endif // COMMON_H