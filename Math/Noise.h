
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

// Vectorized permutation helpers, same math as NoiseMod289/NoiseMod7/NoisePermute
// but operating on 4 lanes at once (one lane per independent sample point).
purefn v128f VCALL NoiseMod289V(v128f x)
{
    return VecSub(x, VecMulf(VecFloor(VecMulf(x, 1.0f / 289.0f)), 289.0f));
}

purefn v128f VCALL NoiseMod7V(v128f x)
{
    return VecSub(x, VecMulf(VecFloor(VecMulf(x, 1.0f / 7.0f)), 7.0f));
}

purefn v128f VCALL NoisePermuteV(v128f x)
{
    return NoiseMod289V(VecMul(VecAddf(VecMulf(x, 34.0f), 1.0f), x));
}

// 4-wide 2-D cellular (Worley) noise: computes NoiseCellular2D for 4 independent
// (Px, Py) points in parallel, one per SIMD lane. *outF1 / *outF2 receive the
// nearest / second-nearest feature distances (one per lane), mirroring the
// scalar version's (x, y) return.
static inline void VCALL NoiseCellular2Dx4(v128f Px, v128f Py, v128f* outF1, v128f* outF2)
{
    const f32 K      = 1.0f / 7.0f;   // 0.142857
    const f32 Ko     = 3.0f / 7.0f;   // 0.428571 (grid mean offset)
    const f32 jitter = 1.0f;         // lower gives a more regular pattern

    v128f PiX = NoiseMod289V(VecFloor(Px));
    v128f PiY = NoiseMod289V(VecFloor(Py));
    v128f PfX = VecFract(Px);
    v128f PfY = VecFract(Py);

    const f32 oi[3]    = { -1.0f, 0.0f, 1.0f };  // integer cell neighbour offsets
    const f32 of[3]    = { -0.5f, 0.5f, 1.5f };  // row distance offsets
    const f32 colDx[3] = { 0.5f, -0.5f, -1.5f }; // column distance offsets

    v128f f1 = VecSet1(1.0e30f);
    v128f f2 = VecSet1(1.0e30f);

    for (s32 i = 0; i < 3; i++)
    {
        v128f px     = NoisePermuteV(VecAddf(PiX, oi[i]));
        v128f pxPiY  = VecAdd(px, PiY);
        for (s32 r = 0; r < 3; r++)
        {
            v128f p  = NoisePermuteV(VecAddf(pxPiY, oi[r]));
            v128f ox = VecSubf(VecFract(VecMulf(p, K)), Ko);
            v128f oy = VecSubf(VecMulf(NoiseMod7V(VecFloor(VecMulf(p, K))), K), Ko);
            v128f dx = VecAdd(VecAddf(PfX, colDx[i]), VecMulf(ox, jitter));
            v128f dy = VecAdd(VecSubf(PfY, of[r]),    VecMulf(oy, jitter));
            v128f d  = VecAdd(VecMul(dx, dx), VecMul(dy, dy));

            v128f closer  = VecCmpLt(d, f1);      // d < f1 (per lane)
            v128f second  = VecCmpLt(d, f2);      // d < f2 (per lane)
            v128f f2FromD = VecSelect(f2, d, second);  // second ? d : f2
            f2 = VecSelect(f2FromD, f1, closer);       // closer ? old-f1 : f2FromD
            f1 = VecSelect(f1, d, closer);             // closer ? d : f1
        }
    }

    *outF1 = VecSqrt(f1);
    *outF2 = VecSqrt(f2);
}

#endif // Noise_H
