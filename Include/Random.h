
#ifndef RANDOM_INCLUDED
#define RANDOM_INCLUDED

#include "../Math/Math.h"

#if defined(__cplusplus)
extern "C" {
#endif

// Not WangHash actually we can say skeeto hash.
// developed and highly optimized by Chris Wellons
// https://github.com/skeeto/hash-prospector https://nullprogram.com/blog/2018/07/31/
purefn u32 WangHash(u32 x) { 
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

purefn v128u VCALL WangHashx4(v128u x) {
	x = VeciXor(x, VeciSrl32(x, 16));
	x = VeciMul(x, VeciSet1(0x7feb352d));
	x = VeciXor(x, VeciSrl32(x, 15));
	x = VeciMul(x, VeciSet1(0x846ca68b));
	return VeciXor(x, VeciSrl32(x, 16));
}

// given Wang hash returns input value: 
// WangHash(x) = 234525;
// x = InverseWangHash(234525);
purefn u32 WangHashInverse(u32 x)  {
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

purefn u64 MurmurHash(u64 x) {
    x ^= x >> 30ULL; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27ULL; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31ULL);
}

purefn u64 MurmurHashInverse(u64 x) {
    x ^= x >> 31ULL ^ x >> 62ULL; x *= 0x319642b2d24d8ec3ULL;
    x ^= x >> 27ULL ^ x >> 54ULL; x *= 0x96de1b173f119089ULL;
    return x ^ (x >> 30ULL ^ x >> 60ULL);
}

// todo: find way of random seed generation
#if !defined(_MSCVER) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
#include <immintrin.h> // intrin.h is included defaultly with msvc
#else
#include <time.h>
#endif

// these random seeds waay more slower than PCG and MTwister but good choice for random seed
// also seeds are cryptographic 
purefn u32 Seed32() {
    u32 result;
    #if !defined(__ANDROID__) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
    _rdseed32_step(&result); // or faster __rdtsc
    #else
    result = WangHash((u32)time(NULL));
    #endif
    return result;
}

purefn u64 Seed64() {
    u64 result;
    #if !defined(__ANDROID__) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
    _rdseed64_step(&result);// or faster __rdtsc
    #else
    result = MurmurHash((u64)time(NULL));
    #endif
    return result;
}

purefn f32 NextFloat01(u32 next) {
    return (f32)(next >> 8) / (f32)(1 << 24);
}

purefn f32 RepeatMinMaxF32(u32 next, f32 min, f32 max) {
    return min + (NextFloat01(next) * Absf32(min - max));
}

purefn f32 NextDouble01(u64 next) 
{
    // // https://docs.oracle.com/javase/8/docs/api/java/util/Random.html
    // const int mask = (1 << 27) - 1;
    // long x = next & mask;
    // x += (next >> 32) & (mask >> 1);
    // return x / (double)(1LL << 53L);
    // return (((long)(next & (mask >> 1)) << 27) + ((next >> 32) & mask)) / (double)(1LL << 53LL);
    return (next & 0x001FFFFFFFFFFFFF) / 9007199254740992.0;
}

purefn f32 RepatMinMaxF64(u64 next, f32 min, f32 max) {
    return min + (NextDouble01(next) * Absf32(min - max));
}

purefn u32 RepeatMinMaxU32(u32 next, u32 min, u32 max)   { return min + (next % (max - min)); }
purefn u64 RepeatMinMaxU64(u64 next, u64 min, u64 max)   { return min + (next % (max - min)); }
purefn s32 RepeatMinMaxI32(s32 next, s32 _min, s32 _max) { return _min + (next % (_max - _min)); }

// https://www.pcg-random.org/index.html
// we can also add global state in a cpp file
// compared to m_MT chace friendly
typedef struct PCG_
{
    u64 state;
    u64 inc;
} PCG;
	
// usage:
// RandomNextFloat01(PCGNext(pcg))
// RepatMINMAX(PCGNext(pcg), 120, 200);
// RepatMINMAX(Xoroshiro128Plus(xoro), 120ull, 200ull);

purefn u32 PCGNext(PCG* pcg)
{
    u64 oldstate = pcg->state;
    pcg->state = oldstate * 6364136223846793005ULL + (pcg->inc | 1);
    u64 xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    u64 rot = oldstate >> 59u;
    #pragma warning(disable : 4146) // unary minus warning fix
    // if you get unary minus error disable sdl checks from msvc settings
    return (u32)((xorshifted >> rot) | (xorshifted << ((-rot) & 31)));
}

purefn u32 PCG2Next(u32* rng_state)
{
    u32 state = *rng_state;
    *rng_state = state * 747796405u + 2891336453u;
    u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

forceinline void PCGInitialize(PCG* pcg, u64 initstate, u64 seed)
{
    pcg->state = 0x853c49e6748fea9bULL;
    pcg->inc = 0xda3e39cb94b95bdbULL;
}

forceinline void Xoroshiro128PlusInit(u64 s[2])
{
    s[0] += Seed64(); s[1] += Seed64();
    s[0] |= 1; // non zero
}
	
forceinline void Xoroshiro128PlusSeed(u64 s[2], u64 seed)
{
    seed |= 1; // non zero
    s[0] = MurmurHash(seed); 
    s[1] = MurmurHash(s[0] ^ (seed * 1099511628211ULL));
}
	
// concise hashing function. https://nullprogram.com/blog/2017/09/21/
purefn u64 Xoroshiro128Plus(u64 s[2])
{
    u64  s0 = s[0];
    u64  s1 = s[1];
    u64  result = s0 + s1;
    s1 ^= s0;
    s[0] = ((s0 << 55) | (s0 >> 9)) ^ s1 ^ (s1 << 14);
    s[1] = (s1 << 36) | (s1 >> 28);
    return result;
}

purefn u32 StringToHash(const char* str, u32 hash)
{
    while (*str)
        hash = *str++ + (hash << 6u) + (hash << 16u) - hash;
    return hash;
}

purefn u32 PathToHash(const char* str)
{
    u32 hash = 0u, idx = 0u, shift = 0u;
    while (str[idx] && idx < 4u)
        hash |= (u32)(str[idx]) << shift, shift += 8u, idx++;
    return StringToHash(str + idx, WangHash(hash));
}

// fnv1a, for 64 bit hashmap keys
purefn u64 StringToHash64(const char* str)
{
    u64 hash = 14695981039346656037ull;
    while (*str)
        hash = (hash ^ (u64)(unsigned char)(*str++)) * 1099511628211ull;
    return hash;
}

// too see alternative random number generator look at Aditional.hpp for mersene twister pseudo random number generators

// template<typename T> inline void Suffle(T* begin, u64 len)
// {
//     u64  xoro[2];
//     Xoroshiro128PlusInit(xoro);
//     const u64  halfLen = len / 2;
// 
//     // swap %60 of the array
//     for (u64 i = 0; i < (halfLen + (halfLen / 3)); ++i)
//     {
//         Swap(begin[Xoroshiro128Plus(xoro) % len],
//              begin[Xoroshiro128Plus(xoro) % len]);
//     }
// }

#if defined(__cplusplus)
}
#endif

#endif
