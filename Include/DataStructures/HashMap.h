/*
* HM_HashMap.h - High-performance C99 Dense Hash Map
*
* ----------------------------------------------------------------------------
* CREDITS & ADAPTATION
* Based on ankerl::unordered_dense (https://github.com/martinus/unordered_dense)
* Original C++ Implementation Copyright (c) 2022-2023 Martin Leitner-Ankerl
* Ported to C99 and modified by Anilcan Gulkaya, 2026.
* Additional features include type-safe macro wrappers and custom growth logic.
*
* Licensed under the MIT License <http://opensource.org/licenses/MIT>.
* SPDX-License-Identifier: MIT
* ----------------------------------------------------------------------------
*
* USAGE:
* In exactly ONE source file, define the implementation guard before including:
* #define HM_HASHMAP_IMPLEMENTATION
* #include "HM_HashMap.h"
*
* In all other files, simply include the header normally like STB libraries.
*
* TYPE-SAFE WRAPPERS (HM_DEFINE_TYPE) nice to have not required:
* Use this macro to generate specific helper functions for your types.
* Example:
* typedef struct { float x, y; } Vec2;
* HM_DEFINE_TYPE(Vec2, Vec2)
* HM_DEFINE_TYPE(U64, uint64_t)
* This generates:
* - Vec2* HMFindVec2(const HashMap* hm, uint64_t key);
* - bool  HMTryGet(const HashMap* hm, uint64_t key, void* out);
* - Vec2* HMInsertVec2(HashMap* hm, uint64_t key, Vec2 value);
*
* MEMORY CONFIGURATION:
* You can override the default memory functions by defining HMRealloc, 
* HMFree, HMMemset, and HMMemcpy before including the header.
* ----------------------------------------------------------------------------
*/

#ifndef HM_INCLUDE_HASHMAP
#define HM_INCLUDE_HASHMAP

#include <stdbool.h>
#include <stdint.h>

#ifndef HMRealloc
    #include <stdlib.h>
    #define HMAalloc(mem, size) malloc(mem, size)
    #define HMRealloc(mem, size) realloc(mem, size)
    #define HMFree(mem) free(mem)
#endif

#ifndef HMMemset
    #include <string.h>
    #define HMMemset(mem, val, size) memset(mem, val, size)
    #define HMMemcpy(mem, src, size) memcpy(mem, src, size)
#endif

typedef struct HMBucket_{
    uint32_t distAndFingerprint;
    uint32_t valueIdx;
} HMBucket;

typedef struct HashMap_{
    uint64_t* keys;
    void*     values;
    HMBucket* buckets;
    uint32_t  count;
    uint32_t  capacity;
    uint32_t  numBuckets;
    uint32_t  maxBucketCapacity;
    uint32_t  valueSize;
    int8_t    shifts;
} HashMap;

HashMap HMCreate(uint32_t reserveCount, uint32_t valueSize);

void HMDestroy(HashMap* hm);

void HMClear(HashMap* hm);

HashMap HMCopy(const HashMap* src);

void* HMFind(const HashMap* hm, uint64_t key);

bool HMContains(const HashMap* hm, uint64_t key);

void* HMInsert(HashMap* hm, uint64_t key, const void* value);

void* HMInsertOrAssign(HashMap* hm, uint64_t key, const void* value);

bool HMErase(HashMap* hm, uint64_t key);

void HMReserve(HashMap* hm, uint32_t capacity);

#define HMSize(hashMap)   ((hashMap)->count)

#define HMEmpty(hashMap)  ((hashMap)->count == 0)

#define HM_DEFINE_TYPE(NAME, TYPE)                                              \
static inline TYPE* HMFind##NAME(const HashMap* hm, uint64_t key)               \
{                                                                               \
    return (TYPE*)HMFind(hm, key);                                              \
}                                                                               \
static inline TYPE* HMInsert##NAME(HashMap* hm, uint64_t key, const TYPE value) \
{                                                                               \
    return (TYPE*)HMInsert(hm, key, &value);                                    \
}                                                                               \
static inline TYPE* HMInsertOrAssign##NAME(HashMap* hm, uint64_t key, TYPE value)\
{                                                                               \
    return (TYPE*)HMInsertOrAssign(hm, key, &value);                            \
}

#ifdef HM_HASHMAP_IMPLEMENTATION

#define HM_DIST_INC (1u << 8u)
#define HM_FINGERPRINT_MASK (HM_DIST_INC - 1u)
#define HM_INITIAL_SHIFTS ((int8_t)61u)
#define HM_MAX_LOAD_FACTOR 0.8f

#define HMMaxSize (1u << 31u)

static inline uint64_t HMMurmurHash(uint64_t x) {
    x ^= x >> 30ULL; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27ULL; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31ULL);
}

static inline uint32_t HMCalcNumBuckets(int8_t shifts) {
    uint32_t n = 1u << (64u - shifts);
    return n < HMMaxSize ? n : HMMaxSize;
}

static inline uint32_t HMNext(const HashMap* hm, uint32_t idx) {
    return (idx + 1u == hm->numBuckets) ? 0u : idx + 1u;
}

static inline uint32_t HMDistAndFingerprintFromHash(uint64_t hash) {
    return HM_DIST_INC | (uint32_t)(hash & HM_FINGERPRINT_MASK);
}

static inline uint32_t HMBucketIdxFromHash(const HashMap* hm, uint64_t hash) {
    return (uint32_t)(hash >> hm->shifts);
}

static int8_t HMCalcShiftsForSize(uint32_t s) {
    int8_t shifts = HM_INITIAL_SHIFTS;
    while (shifts > 0 && (uint32_t)((float)HMCalcNumBuckets(shifts) * HM_MAX_LOAD_FACTOR) < s)
        --shifts;
    return shifts;
}

static inline void* HMValueAt(const HashMap* hm, uint32_t idx) {
    return (int8_t*)hm->values + (size_t)idx * hm->valueSize;
}

static void HMReallocateBuckets(HashMap* hm, uint32_t numBuckets) {
    hm->numBuckets = numBuckets;
    hm->buckets = (HMBucket*)HMRealloc(hm->buckets, numBuckets * sizeof(HMBucket));
    if (hm->numBuckets == HMMaxSize)
        hm->maxBucketCapacity = HMMaxSize;
    else
        hm->maxBucketCapacity = (uint32_t)((float)hm->numBuckets * HM_MAX_LOAD_FACTOR);
}

static void HMGrowEntries(HashMap* hm, uint32_t newCap) {
    hm->keys = (uint64_t*)HMRealloc(hm->keys, newCap * sizeof(uint64_t));
    hm->values = HMRealloc(hm->values, (size_t)newCap * hm->valueSize);
    hm->capacity = newCap;
}

static void HMPlaceAndShiftUp(HashMap* hm, HMBucket bucket, uint32_t place) {
    while (hm->buckets[place].distAndFingerprint != 0) {
        HMBucket tmp = hm->buckets[place];
        hm->buckets[place] = bucket;
        bucket = tmp;
        bucket.distAndFingerprint += HM_DIST_INC;
        place = HMNext(hm, place);
    }
    hm->buckets[place] = bucket;
}

static HMBucket HMNextWhileLess(const HashMap* hm, uint64_t key) {
    uint64_t hash = HMMurmurHash(key);
    uint32_t distAndFingerprint = HMDistAndFingerprintFromHash(hash);
    uint32_t bucketIdx = HMBucketIdxFromHash(hm, hash);
    while (distAndFingerprint < hm->buckets[bucketIdx].distAndFingerprint) {
        distAndFingerprint += HM_DIST_INC;
        bucketIdx = HMNext(hm, bucketIdx);
    }
    HMBucket result = { distAndFingerprint, bucketIdx };
    return result;
}

static void HMClearAndFillBuckets(HashMap* hm) {
    HMMemset(hm->buckets, 0, hm->numBuckets * sizeof(HMBucket));
    for (uint32_t i = 0; i < hm->count; ++i) {
        HMBucket b = HMNextWhileLess(hm, hm->keys[i]);
        HMBucket toPlace = { b.distAndFingerprint, i };
        HMPlaceAndShiftUp(hm, toPlace, b.valueIdx);
    }
}

static void HMIncreaseSize(HashMap* hm) {
    --hm->shifts;
    HMReallocateBuckets(hm, HMCalcNumBuckets(hm->shifts));
    HMClearAndFillBuckets(hm);
}

HashMap HMCreate(uint32_t reserveCount, uint32_t valueSize) {
    HashMap hm;
    HMMemset(&hm, 0, sizeof(HashMap));
    hm.shifts = HM_INITIAL_SHIFTS;
    hm.valueSize = valueSize;
    if (reserveCount > 0) {
        uint32_t capped = reserveCount < HMMaxSize ? reserveCount : HMMaxSize;
        HMGrowEntries(&hm, capped);
        int8_t shifts = HMCalcShiftsForSize(capped);
        hm.shifts = shifts;
        HMReallocateBuckets(&hm, HMCalcNumBuckets(shifts));
        HMMemset(hm.buckets, 0, hm.numBuckets * sizeof(HMBucket));
    }
    return hm;
}

static int32_t HMFindIdx(const HashMap* hm, uint64_t key) {
    if (hm->count == 0) return -1;
    uint64_t hash = HMMurmurHash(key);
    uint32_t distAndFingerprint = HMDistAndFingerprintFromHash(hash);
    uint32_t bucketIdx = HMBucketIdxFromHash(hm, hash);

    while (true) 
    {
        const HMBucket* b = &hm->buckets[bucketIdx];
        if (distAndFingerprint == b->distAndFingerprint && key == hm->keys[b->valueIdx])
            return (int32_t)b->valueIdx;
        if (distAndFingerprint > b->distAndFingerprint) return -1;
        distAndFingerprint += HM_DIST_INC;
        bucketIdx = HMNext(hm, bucketIdx);
    }
}

void* HMFind(const HashMap* hm, uint64_t key) {
    int32_t idx = HMFindIdx(hm, key);
    if (idx < 0) return NULL;
    return HMValueAt(hm, (uint32_t)idx);
}

bool HMContains(const HashMap* hm, uint64_t key) {
    return HMFindIdx(hm, key) >= 0;
}

bool HMTryGet(const HashMap* hm, uint64_t key, void* out)
{
    void* fnd = (void*)HMFind(hm, key);
    if (!fnd) return false;
    HMMemcpy(out, fnd, hm->valueSize);
    return true;
}

static void* HMDoInsert(HashMap* hm, uint64_t key, const void* value, bool* outInserted) {
    if (hm->count >= hm->maxBucketCapacity) HMIncreaseSize(hm);
    uint64_t hash = HMMurmurHash(key);
    uint32_t distAndFootprint = HMDistAndFingerprintFromHash(hash);
    uint32_t bucketIdx = HMBucketIdxFromHash(hm, hash);
    while (true)
    {
        HMBucket bucket = hm->buckets[bucketIdx];
        if (distAndFootprint == bucket.distAndFingerprint) 
        {
            if (key == hm->keys[bucket.valueIdx]) 
            {
                if (outInserted) *outInserted = false;
                return HMValueAt(hm, bucket.valueIdx);
            }
        }
        else if (distAndFootprint > bucket.distAndFingerprint) 
        {
            if (hm->count == hm->capacity)
                HMGrowEntries(hm, hm->capacity ? hm->capacity + (hm->capacity>>1) : 8);
            uint32_t idx = hm->count;
            hm->keys[idx] = key;
            HMMemcpy(HMValueAt(hm, idx), value, hm->valueSize);
            hm->count++;
            HMBucket toPlace = { distAndFootprint, idx };
            HMPlaceAndShiftUp(hm, toPlace, bucketIdx);
            if (outInserted) *outInserted = true;
            return HMValueAt(hm, idx);
        }
        distAndFootprint += HM_DIST_INC;
        bucketIdx = HMNext(hm, bucketIdx);
    }
}

void* HMInsert(HashMap* hm, uint64_t key, const void* value) {
    return HMDoInsert(hm, key, value, NULL);
}

void* HMInsertOrAssign(HashMap* hm, uint64_t key, const void* value) {
    bool inserted = false;
    void* slot = HMDoInsert(hm, key, value, &inserted);
    if (!inserted) HMMemcpy(slot, value, hm->valueSize);
    return slot;
}

static void HMDoErase(HashMap* hm, uint32_t bucketIdx) {
    uint32_t valueIdxToRemove = hm->buckets[bucketIdx].valueIdx;
    uint32_t nextBucketIdx = HMNext(hm, bucketIdx);
    while (hm->buckets[nextBucketIdx].distAndFingerprint >= HM_DIST_INC * 2u) 
    {
        HMBucket next = hm->buckets[nextBucketIdx];
        hm->buckets[bucketIdx].distAndFingerprint = next.distAndFingerprint - HM_DIST_INC;
        hm->buckets[bucketIdx].valueIdx = next.valueIdx;
        bucketIdx = nextBucketIdx;
        nextBucketIdx = HMNext(hm, nextBucketIdx);
    }
    hm->buckets[bucketIdx].distAndFingerprint = 0;
    hm->buckets[bucketIdx].valueIdx = 0;
    uint32_t lastIdx = hm->count - 1;
    if (valueIdxToRemove != lastIdx) 
    {
        hm->keys[valueIdxToRemove] = hm->keys[lastIdx];
        HMMemcpy(HMValueAt(hm, valueIdxToRemove), HMValueAt(hm, lastIdx), hm->valueSize);
        uint64_t movedKey = hm->keys[valueIdxToRemove];
        uint64_t mh = HMMurmurHash(movedKey);
        uint32_t bIdx = HMBucketIdxFromHash(hm, mh);
        while (hm->buckets[bIdx].valueIdx != lastIdx) bIdx = HMNext(hm, bIdx);
        hm->buckets[bIdx].valueIdx = valueIdxToRemove;
    }
    hm->count--;
}

bool HMErase(HashMap* hm, uint64_t key) {
    if (hm->count == 0) return false;
    HMBucket b = HMNextWhileLess(hm, key);
    uint32_t distAndFingerprint = b.distAndFingerprint;
    uint32_t bucketIdx = b.valueIdx;
    while (distAndFingerprint == hm->buckets[bucketIdx].distAndFingerprint &&
        key != hm->keys[hm->buckets[bucketIdx].valueIdx]) 
    {
        distAndFingerprint += HM_DIST_INC;
        bucketIdx = HMNext(hm, bucketIdx);
    }
    if (distAndFingerprint != hm->buckets[bucketIdx].distAndFingerprint) return false;
    HMDoErase(hm, bucketIdx);
    return true;
}

void HMReserve(HashMap* hm, uint32_t capacity) {
    if (capacity > HMMaxSize) capacity = HMMaxSize;
    if (hm->capacity < capacity) HMGrowEntries(hm, capacity);
    uint32_t forSize = capacity > hm->count ? capacity : hm->count;
    int8_t shifts = HMCalcShiftsForSize(forSize);
    if (hm->numBuckets == 0 || shifts < hm->shifts) 
    {
        hm->shifts = shifts;
        HMReallocateBuckets(hm, HMCalcNumBuckets(shifts));
        HMClearAndFillBuckets(hm);
    }
}

HashMap HMCopy(const HashMap* src) {
    HashMap dst;
    HMMemset(&dst, 0, sizeof(HashMap));
    dst.count             = src->count;
    dst.capacity          = src->capacity;
    dst.numBuckets        = src->numBuckets;
    dst.maxBucketCapacity = src->maxBucketCapacity;
    dst.valueSize         = src->valueSize;
    dst.shifts            = src->shifts;

    if (src->capacity > 0) {
        dst.keys = (uint64_t*)HMRealloc(NULL, dst.capacity * sizeof(uint64_t));
        dst.values = HMRealloc(NULL, (size_t)dst.capacity * dst.valueSize);
        if (src->count > 0) {
            HMMemcpy(dst.keys, src->keys, dst.count * sizeof(uint64_t));
            HMMemcpy(dst.values, src->values, (size_t)dst.count * dst.valueSize);
        }
    }

    if (src->numBuckets > 0) {
        dst.buckets = (HMBucket*)HMRealloc(NULL, dst.numBuckets * sizeof(HMBucket));
        HMMemcpy(dst.buckets, src->buckets, dst.numBuckets * sizeof(HMBucket));
    }

    return dst;
}

void HMClear(HashMap* hm) {
    hm->count = 0;
    if (hm->buckets)
        HMMemset(hm->buckets, 0, hm->numBuckets * sizeof(HMBucket));
}

void HMDestroy(HashMap* hm) {
    HMFree(hm->keys);
    HMFree(hm->values);
    HMFree(hm->buckets);
    HMMemset(hm, 0, sizeof(HashMap));
}

#endif // HM_HASHMAP_IMPLEMENTATION

#endif // HM_INCLUDE_HASHMAP
