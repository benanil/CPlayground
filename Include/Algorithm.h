
#ifndef ALGORITHM_INCLUDED
#define ALGORITHM_INCLUDED

#include "SIMD.h"

#define XSWAP(type, x, y) do { \
    type SWAP_tmp = (x);      \
    (x) = (y);                \
    (y) = SWAP_tmp;           \
} while (0)

#if defined(__cplusplus)
extern "C" {
#endif

purefn bool IsNumber(char a) { return a <= '9' && a >= '0'; };
purefn bool IsLower(char a)  { return a >= 'a' && a <= 'z'; };
purefn bool IsUpper(char a)  { return a >= 'A' && a <= 'Z'; };
purefn char ToLower(char a)  { return IsUpper(a) ? a + ('a' - 'A') : a; }
purefn char ToUpper(char a)  { return IsLower(a) ? a - ('a' - 'A') : a; }
// is alphabetical character?
purefn bool IsChar(char a) { return IsUpper(a) || IsLower(a); };
purefn bool IsWhitespace(char c) { return c <= ' '; }

void SwapMem(void* a, void* b, size_t elemSize);

// quicksort core
void QuickSort(void* arr, int left, int right, size_t elemSize,
                  int (*compareFn)(const void*, const void*));

void Reverse(void** begin, void** end, int elemSize);

int* BinarySearch(int* begin, int len, int value);

// String to number functions
const char* ParseNumber(const char* curr, int* result);

const char* ParseNumberI64(const char* ptr, int64_t* result);

// if you really want to squeeze performance no negative and such checks
const char* ParsePositiveNumber(const char* str, int* outValue);

const char* ParsePositiveNumberU16(const char* str, uint16_t* outValue);

bool IsParsable(const char* curr);

const char* ParseFloat(const char* text, float* out);

int IntToString(char* ptr, int64_t x, int afterPoint);

int Pow10(int x);

// converts floating point to string
// @param afterpoint num digits after friction: 4
// @returns number of characters added
int FloatToString(char* ptr, float f, int afterpoint);

// return index if found, -1 otherwise
int aIndexOf(const void* arr, const void* val, int n, size_t elemSize, int (*cmp)(const void*, const void*));

// count how many times val appears
int aCountIf(const void* arr, const void* val, int n, size_t elemSize, int (*cmp)(const void*, const void*));

#define StrCMP16(_str, _otr) StrCmp16(_str, _otr, sizeof(_otr) - 1)

static inline bool StrCmp16(const char* a, const char* b, uint64_t n)
{
    n = n > 16 ? 16 : n;
    v128u va   = VeciLoad(a);
    v128u vb   = VeciLoad(b);
    v128u cmp  = VeciCmpEq8(va, vb);
    int mask   = VeciMovemask8(cmp);
    int expect = (1 << n) - 1;
    return (mask & expect) == expect;
}

static inline bool StrCmp16Lower(const char* a, const char* b, uint64_t n)
{
    n = n > 16 ? 16 : n;
    v128u va   = VeciToLowerASCII(VeciLoad(a));
    v128u vb   = VeciToLowerASCII(VeciLoad(b));
    v128u cmp  = VeciCmpEq8(va, vb);
    int mask   = VeciMovemask8(cmp);
    int expect = (1 << n) - 1;
    return (mask & expect) == expect;
}

bool StringContains(const char* name, const char* search);

bool StringEqual(const char* RESTRICT a, const char* RESTRICT b, int n);

static inline void CopyString(char* dst, u32 dstSize, const char* src)
{
    u32 len = Minu32((u32)StringLength(src), dstSize - 1u);
    MemCopy(dst, src, len);
    dst[len] = '\0';
}

#if defined(__cplusplus)
}
#endif

#endif
