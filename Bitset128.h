#ifndef BITSET128_H
#define BITSET128_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*********************************************************************************
    *    Description:                                                               *
    *        128-bit bitset implementation for C                                    *
    *        Supports all standard bitset operations with efficient implementation  *
    *    Author:                                                                    *
    *        Generated for high-performance bit manipulation                        *
    *********************************************************************************/

typedef struct BitSet128_ {
    uint64_t bits[2];  // 128 bits = 2 * 64-bit words
} BitSet128;

// Construction and initialization
static inline BitSet128 BitSet128_Zero(void) {
    BitSet128 bs = {{0, 0}};
    return bs;
}

static inline BitSet128 BitSet128_Ones(void) {
    BitSet128 bs = {{UINT64_MAX, UINT64_MAX}};
    return bs;
}

static inline BitSet128 BitSet128_FromU64(uint64_t low, uint64_t high) {
    BitSet128 bs = {{low, high}};
    return bs;
}

// Bit manipulation
static inline void BitSet128_Set(BitSet128* bs, unsigned int index) {
    if (index < 128) {
        unsigned int word = index >> 6;  // index / 64
        unsigned int bit = index & 63;   // index % 64
        bs->bits[word] |= (1ULL << bit);
    }
}

static inline void BitSet128_Clear(BitSet128* bs, unsigned int index) {
    if (index < 128) {
        unsigned int word = index >> 6;
        unsigned int bit = index & 63;
        bs->bits[word] &= ~(1ULL << bit);
    }
}

static inline void BitSet128_Flip(BitSet128* bs, unsigned int index) {
    if (index < 128) {
        unsigned int word = index >> 6;
        unsigned int bit = index & 63;
        bs->bits[word] ^= (1ULL << bit);
    }
}

static inline bool BitSet128_Test(const BitSet128* bs, unsigned int index) {
    if (index >= 128) return false;
    unsigned int word = index >> 6;
    unsigned int bit = index & 63;
    return (bs->bits[word] & (1ULL << bit)) != 0;
}

// Set operations
static inline BitSet128 BitSet128_And(BitSet128 a, BitSet128 b) {
    BitSet128 result = {{a.bits[0] & b.bits[0], a.bits[1] & b.bits[1]}};
    return result;
}

static inline BitSet128 BitSet128_Or(BitSet128 a, BitSet128 b) {
    BitSet128 result = {{a.bits[0] | b.bits[0], a.bits[1] | b.bits[1]}};
    return result;
}

static inline BitSet128 BitSet128_Xor(BitSet128 a, BitSet128 b) {
    BitSet128 result = {{a.bits[0] ^ b.bits[0], a.bits[1] ^ b.bits[1]}};
    return result;
}

static inline BitSet128 BitSet128_Not(BitSet128 a) {
    BitSet128 result = {{~a.bits[0], ~a.bits[1]}};
    return result;
}

// In-place operations
static inline void BitSet128_AndEq(BitSet128* a, BitSet128 b) {
    a->bits[0] &= b.bits[0];
    a->bits[1] &= b.bits[1];
}

static inline void BitSet128_OrEq(BitSet128* a, BitSet128 b) {
    a->bits[0] |= b.bits[0];
    a->bits[1] |= b.bits[1];
}

static inline void BitSet128_XorEq(BitSet128* a, BitSet128 b) {
    a->bits[0] ^= b.bits[0];
    a->bits[1] ^= b.bits[1];
}

static inline void BitSet128_NotEq(BitSet128* a) {
    a->bits[0] = ~a->bits[0];
    a->bits[1] = ~a->bits[1];
}

// Comparison
static inline bool BitSet128_Equal(BitSet128 a, BitSet128 b) {
    return a.bits[0] == b.bits[0] && a.bits[1] == b.bits[1];
}

static inline bool BitSet128_IsZero(BitSet128 a) {
    return (a.bits[0] | a.bits[1]) == 0;
}

static inline bool BitSet128_IsOnes(BitSet128 a) {
    return a.bits[0] == UINT64_MAX && a.bits[1] == UINT64_MAX;
}

// Utility functions
static inline unsigned int BitSet128_PopCount(BitSet128 a) {
    // Count set bits using builtin popcount if available
    #if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(a.bits[0]) + __builtin_popcountll(a.bits[1]);
    #else
    // Fallback implementation
    unsigned int count = 0;
    uint64_t x = a.bits[0];
    while (x) {
        count++;
        x &= x - 1;  // Clear lowest set bit
    }
    x = a.bits[1];
    while (x) {
        count++;
        x &= x - 1;
    }
    return count;
    #endif
}

static inline unsigned int BitSet128_FindFirstSet(BitSet128 a) {
    if (a.bits[0] != 0) {
        #if defined(__GNUC__) || defined(__clang__)
        return __builtin_ctzll(a.bits[0]);
        #else
        // Fallback implementation
        unsigned int pos = 0;
        uint64_t x = a.bits[0];
        if ((x & 0xFFFFFFFF) == 0) { pos += 32; x >>= 32; }
        if ((x & 0xFFFF) == 0) { pos += 16; x >>= 16; }
        if ((x & 0xFF) == 0) { pos += 8; x >>= 8; }
        if ((x & 0xF) == 0) { pos += 4; x >>= 4; }
        if ((x & 0x3) == 0) { pos += 2; x >>= 2; }
        if ((x & 0x1) == 0) { pos += 1; }
        return pos;
        #endif
    } else if (a.bits[1] != 0) {
        #if defined(__GNUC__) || defined(__clang__)
        return 64 + __builtin_ctzll(a.bits[1]);
        #else
        // Fallback implementation for high word
        unsigned int pos = 64;
        uint64_t x = a.bits[1];
        if ((x & 0xFFFFFFFF) == 0) { pos += 32; x >>= 32; }
        if ((x & 0xFFFF) == 0) { pos += 16; x >>= 16; }
        if ((x & 0xFF) == 0) { pos += 8; x >>= 8; }
        if ((x & 0xF) == 0) { pos += 4; x >>= 4; }
        if ((x & 0x3) == 0) { pos += 2; x >>= 2; }
        if ((x & 0x1) == 0) { pos += 1; }
        return pos;
        #endif
    }
    return 128; // No bits set
}

// Shift operations
static inline BitSet128 BitSet128_ShiftLeft(BitSet128 a, unsigned int shift) {
    if (shift >= 128) {
        return BitSet128_Zero();
    } else if (shift >= 64) {
        BitSet128 result = {{0, a.bits[0] << (shift - 64)}};
        return result;
    } else if (shift == 0) {
        return a;
    } else {
        uint64_t high = (a.bits[1] << shift) | (a.bits[0] >> (64 - shift));
        uint64_t low = a.bits[0] << shift;
        BitSet128 result = {{low, high}};
        return result;
    }
}

static inline BitSet128 BitSet128_ShiftRight(BitSet128 a, unsigned int shift) {
    if (shift >= 128) {
        return BitSet128_Zero();
    } else if (shift >= 64) {
        BitSet128 result = {{a.bits[1] >> (shift - 64), 0}};
        return result;
    } else if (shift == 0) {
        return a;
    } else {
        uint64_t low = (a.bits[0] >> shift) | (a.bits[1] << (64 - shift));
        uint64_t high = a.bits[1] >> shift;
        BitSet128 result = {{low, high}};
        return result;
    }
}

// Range operations
static inline void BitSet128_SetRange(BitSet128* bs, unsigned int start, unsigned int end) {
    for (unsigned int i = start; i <= end && i < 128; i++) {
        BitSet128_Set(bs, i);
    }
}

static inline void BitSet128_ClearRange(BitSet128* bs, unsigned int start, unsigned int end) {
    for (unsigned int i = start; i <= end && i < 128; i++) {
        BitSet128_Clear(bs, i);
    }
}

static inline void BitSet128_FlipRange(BitSet128* bs, unsigned int start, unsigned int end) {
    for (unsigned int i = start; i <= end && i < 128; i++) {
        BitSet128_Flip(bs, i);
    }
}

// Conversion functions
static inline void BitSet128_ToString(BitSet128 a, char* buffer) {
    // Buffer should be at least 129 bytes (128 bits + null terminator)
    for (int i = 127; i >= 0; i--) {
        buffer[127 - i] = BitSet128_Test(&a, i) ? '1' : '0';
    }
    buffer[128] = '\0';
}

static inline BitSet128 BitSet128_FromString(const char* str) {
    BitSet128 result = BitSet128_Zero();
    int len = strlen(str);
    for (int i = 0; i < len && i < 128; i++) {
        if (str[len - 1 - i] == '1') {
            BitSet128_Set(&result, i);
        }
    }
    return result;
}

#endif // BITSET128_H