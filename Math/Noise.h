
#ifndef Noise_H
#define Noise_H

// Procedural noise helpers, moved out of Vector.h. These are the C ports of the
// GLSL "webgl-noise" routines (Stefan Gustavson / Ian McEwan, MIT). The original
// paste targeted HLSL/GLSL (vector swizzles, float4, operator overloads) which do
// not exist in C, so the vectorized bodies are expanded to scalar / engine-vector
// form here. Only the pieces the engine actually needs are kept: the permutation
// polynomial and 2-D cellular (Worley) noise. The 4-D simplex helpers (grad4 /
// rgrad2) were dropped because they require a float4 type the math library has no
// equivalent for and nothing consumes them.

#include "Math.h"
#include "Vector.h"

// Modulo 289 / 7 without a division, matches the GLSL mod289 / mod7 used to keep
// the permutation polynomial from overflowing single precision.
purefn f32 NoiseMod289(f32 x) { return x - Floorf32(x * (1.0f / 289.0f)) * 289.0f; }
purefn f32 NoiseMod7  (f32 x) { return x - Floorf32(x * (1.0f / 7.0f))   * 7.0f; }

// Permutation polynomial (34x^2 + x) mod 289, the shared hash of the noise family.
purefn f32 NoisePermute(f32 x) { return NoiseMod289((34.0f * x + 1.0f) * x); }

// 2-D cellular (Worley) noise over the 3x3 neighbourhood of feature points. Returns
// the distance to the nearest (x = F1) and second nearest (y = F2) feature point,
// both in [0, ~1.5]. Direct port of Gustavson's cellular(): the three columns and
// rows are unrolled and the nine candidate distances reduced to the two smallest.
purefn float2 NoiseCellular2D(float2 P)
{
    const f32 K  = 1.0f / 7.0f; // 0.142857
    const f32 Ko = 3.0f / 7.0f; // 0.428571 (grid mean offset)
    const f32 jitter = 1.0f;    // lower gives a more regular pattern

    float2 Pi = (float2){ NoiseMod289(Floorf32(P.x)), NoiseMod289(Floorf32(P.y)) };
    float2 Pf = (float2){ Fractf32(P.x), Fractf32(P.y) };

    const f32 oi[3] = { -1.0f, 0.0f, 1.0f }; // integer cell neighbour offsets
    const f32 of[3] = { -0.5f, 0.5f, 1.5f }; // row distance offsets
    const f32 colDx[3] = { 0.5f, -0.5f, -1.5f }; // column distance offsets

    f32 f1 = 1.0e30f, f2 = 1.0e30f;
    for (s32 i = 0; i < 3; i++)
    {
        f32 px = NoisePermute(Pi.x + oi[i]);
        for (s32 r = 0; r < 3; r++)
        {
            f32 p  = NoisePermute(px + Pi.y + oi[r]);
            f32 ox = Fractf32(p * K) - Ko;
            f32 oy = NoiseMod7(Floorf32(p * K)) * K - Ko;
            f32 dx = Pf.x + colDx[i] + jitter * ox;
            f32 dy = Pf.y - of[r]    + jitter * oy;
            f32 d  = dx * dx + dy * dy;
            if (d < f1)      { f2 = f1; f1 = d; }
            else if (d < f2) { f2 = d; }
        }
    }
    return (float2){ Sqrtf(f1), Sqrtf(f2) };
}

#endif // Noise_H
