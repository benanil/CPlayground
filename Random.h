
#ifndef RANDOM_INCLUDED
#define RANDOM_INCLUDED

#include "Math/Math.h"


// Not WangHash actually we can say skeeto hash.
// developed and highly optimized by Chris Wellons
// https://github.com/skeeto/hash-prospector https://nullprogram.com/blog/2018/07/31/
purefn uint32_t WangHash(uint32_t x) { 
	x ^= x >> 16u; x *= 0x7feb352du;
	x ^= x >> 15u; x *= 0x846ca68bu;
	return x ^ (x >> 16u);
}

// given Wang hash returns input value: 
// WangHash(x) = 234525;
// x = InverseWangHash(234525);
purefn uint32_t WangHashInverse(uint32_t x)  {
	x ^= x >> 16u; x *= 0x7feb352du;
	x ^= x >> 15u; x *= 0x846ca68bu;
	return x ^ (x >> 16u);
}

purefn uint64_t MurmurHash(uint64_t x) {
	x ^= x >> 30ULL; x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27ULL; x *= 0x94d049bb133111ebULL;
	return x ^ (x >> 31ULL);
}

purefn uint64_t MurmurHashInverse(uint64_t x) {
	x ^= x >> 31ULL ^ x >> 62ULL; x *= 0x319642b2d24d8ec3ULL;
	x ^= x >> 27ULL ^ x >> 54ULL; x *= 0x96de1b173f119089ULL;
	return x ^ (x >> 30ULL ^ x >> 60ULL);
}

// todo: find way of random seed generation
#if !defined(_MSCVER) && !defined(__ANDROID__)
#include <immintrin.h> // intrin.h is included defaultly with msvc
#else
#include <time.h>
#endif

// these random seeds waay more slower than PCG and MTwister but good choice for random seed
// also seeds are cryptographic 
purefn uint32_t Seed32() {
    uint32_t result;
    #if !defined(__ANDROID__)
    _rdseed32_step(&result); // or faster __rdtsc
    #else
    result = WangHash(time(nullptr));
    #endif
    return result;
}

purefn uint64_t Seed64() {
    uint64_t result;
    #if !defined(__ANDROID__)
    _rdseed64_step(&result);// or faster __rdtsc
    #else
    result = MurmurHash(time(nullptr));
    #endif
    return result;
}

purefn float NextFloat01(uint32_t next) {
    return float(next >> 8) / (float)(1 << 24);
}
	
purefn float RepatMinMax(uint32_t next, float min, float max) {
    return min + (NextFloat01(next) * Absf(min - max));
}
	
purefn double NextDouble01(uint64_t next) 
{
    // // https://docs.oracle.com/javase/8/docs/api/java/util/Random.html
    // const int mask = (1 << 27) - 1;
    // long x = next & mask;
    // x += (next >> 32) & (mask >> 1);
    // return x / (double)(1LL << 53L);
    // return (((long)(next & (mask >> 1)) << 27) + ((next >> 32) & mask)) / (double)(1LL << 53LL);
    return (next & 0x001FFFFFFFFFFFFF) / 9007199254740992.0;
}
	
purefn double RepatMinMax(uint64_t next, double min, double max) {
    return min + (NextDouble01(next) * Absf(min - max));
}
	
purefn uint32 RepatMinMax(uint32_t next, uint32_t min, uint32_t max) { return min + (next % (max - min)); }
purefn uint64_t RepatMinMax(uint64_t next, uint64_t min, uint64_t max) { return min + (next % (max - min)); }
purefn int    RepatMinMax(int next, int _min, int _max) { return _min + (next % (_max - _min)); }

// https://www.pcg-random.org/index.html
// we can also add global state in a cpp file
// compared to m_MT chace friendly
typedef struct PCG_
{
    uint64_t state;
    uint64_t inc;
} PCG;
	
// usage:
// RandomNextFloat01(PCGNext(pcg))
// RepatMINMAX(PCGNext(pcg), 120, 200);
// RepatMINMAX(Xoroshiro128Plus(xoro), 120ull, 200ull);

purefn uint32_t PCGNext(PCG* pcg)
{
    uint64_t oldstate = pcg->state;
    *pcg->state = oldstate * 6364136223846793005ULL + (pcg->inc | 1);
    uint64_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    uint64_t rot = oldstate >> 59u;
    #pragma warning(disable : 4146) // unary minus warning fix
    // if you get unary minus error disable sdl checks from msvc settings
    return (uint32_t)((xorshifted >> rot) | (xorshifted << ((-rot) & 31)));
}

purefn uint32_t PCG2Next(uint* rng_state)
{
    uint32_t state = *rng_state;
    *rng_state = state * 747796405u + 2891336453u;
    uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

forceinline void PCGInitialize(PCG* pcg, uint64_t initstate, uint64_t seed)
{
    pcg->state = 0x853c49e6748fea9bULL;
    pcg->inc = 0xda3e39cb94b95bdbULL;
}

forceinline void Xoroshiro128PlusInit(uint64_t s[2])
{
    s[0] += Seed64(); s[1] += Seed64();
    s[0] |= 1; // non zero
}
	
forceinline void Xoroshiro128PlusSeed(uint64_t s[2], uint64_t  seed)
{
    seed |= 1; // non zero
    s[0] = MurmurHash(seed); 
    s[1] = MurmurHash(s[0] ^ (seed * 1099511628211ULL));
}
	
// concise hashing function. https://nullprogram.com/blog/2017/09/21/
purefn uint64_t Xoroshiro128Plus(uint64_t s[2])
{
    uint64_t  s0 = s[0];
    uint64_t  s1 = s[1];
    uint64_t  result = s0 + s1;
    s1 ^= s0;
    s[0] = ((s0 << 55) | (s0 >> 9)) ^ s1 ^ (s1 << 14);
    s[1] = (s1 << 36) | (s1 >> 28);
    return result;
}

purefn inline uint32_t StringToHash(const char* str, uint32_t hash)
{
	while (*str)
		hash = *str++ + (hash << 6u) + (hash << 16u) - hash;
	return hash;
}

purefn inline uint32_t PathToHash(const char* str)
{
	uint32_t hash = 0u, idx = 0u, shift = 0u;
	while (str[idx] && idx < 4u)
		hash |= uint(str[idx]) << shift, shift += 8u, idx++;
	return StringToHash(str + idx, WangHash(hash));
}

// too see alternative random number generator look at Aditional.hpp for mersene twister pseudo random number generators

// template<typename T> inline void Suffle(T* begin, uint64_t len)
// {
//     uint64_t  xoro[2];
//     Xoroshiro128PlusInit(xoro);
//     const uint64_t  halfLen = len / 2;
// 
//     // swap %60 of the array
//     for (uint64_t i = 0; i < (halfLen + (halfLen / 3)); ++i)
//     {
//         Swap(begin[Xoroshiro128Plus(xoro) % len],
//              begin[Xoroshiro128Plus(xoro) % len]);
//     }
// }
#endif