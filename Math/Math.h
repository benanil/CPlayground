#ifndef MATH_H
#define MATH_H

// most of the functions are accurate and faster than stl 
// convinient for game programming, be aware of speed and preciseness tradeoffs because cstd has more accurate functions
// https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/

// Sections of this file are:
// Essential    : square, sqrt, exp, pow...
// Trigonometry : sin, cos, tan, atan, atan2...
// Half         : IEEE 16bit float, conversion functions
// Color        : packing and unpacking rgba8 color
// Ease         : easeIn, easeOut...

#include "../Include/Common.h"

#define MATH_PI        (3.14159265358f)
#define MATH_HalfPI    (MATH_PI / 2.0f)
#define MATH_QuarterPI (MATH_PI / 4.0f)
#define MATH_RadToDeg  (180.0f / MATH_PI)
#define MATH_DegToRad  (MATH_PI / 180.0f)
#define MATH_OneDivPI  (1.0f / MATH_PI)
#define MATH_TwoPI     (MATH_PI * 2.0f)
#define MATH_Sqrt2     (1.414213562f)
#define MATH_Epsilon   (0.0001f)

purefn u8 IsNanF32(f32 value)
{
    return value != value;
}

purefn u8 IsInfiniteF32(f32 value)
{
    return !IsNanF32(value) && (value < -FLT_MAX || value > FLT_MAX);
}

purefn u8 IsFiniteF32(f32 value)
{
    return !IsNanF32(value) && !IsInfiniteF32(value);
}

purefn v128f VCALL Vec3Cross(v128f vec0, v128f vec1)
{
#if defined(AX_ARM)
    float32x2_t v1xy = vget_low_f32(vec0);
    float32x2_t v2xy = vget_low_f32(vec1);
    float32x2_t v1yx = vrev64_f32(v1xy);
    float32x2_t v2yx = vrev64_f32(v2xy);
    float32x2_t v1zz = vdup_lane_f32(vget_high_f32(vec0), 0);
    float32x2_t v2zz = vdup_lane_f32(vget_high_f32(vec1), 0);
    uint32x4_t FlipY = ARMCreateVecI(0x00000000u, 0x80000000u, 0x00000000u, 0x00000000u);
    v128f vResult = vmulq_f32(vcombine_f32(v1yx, v1xy), vcombine_f32(v2zz, v2yx));
    vResult = vmlsq_f32(vResult, vcombine_f32(v1zz, v1yx), vcombine_f32(v2yx, v2xy));
    vResult = vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(vResult), FlipY));
    return VecAnd(vResult, VecMask3);
#else
    v128f tmp0 = VecShuffleR(vec0, vec0, 3,0,2,1);
    v128f tmp1 = VecShuffleR(vec1, vec1, 3,1,0,2);
    v128f tmp2 = VecMul(tmp0, vec1);
    v128f tmp3 = VecMul(tmp0, tmp1);
    v128f tmp4 = VecShuffleR(tmp2, tmp2, 3,0,2,1);
    return VecSub(tmp3, tmp4);
#endif
}

purefn f32 VCALL VecMinVal(v128f a)
{
    v128f t = VecSwapPairs(a);
    v128f m = VecMin(a, t);
    return VecGetX(VecMin(m, VecSwapHalves(m)));
}

purefn f32 VCALL VecMaxVal(v128f a)
{
    v128f t = VecSwapPairs(a);
    v128f m = VecMax(a, t);
    return VecGetX(VecMax(m, VecSwapHalves(m)));
}

purefn u32 VCALL VeciMinVal(v128u a)
{
    v128u t = VeciSwapPairs(a);
    v128u m = VeciMin(a, t);
    return VeciGetX(VeciMin(m, VeciSwapHalves(m)));
}

purefn u32 VCALL VeciMaxVal(v128u a)
{
    v128u t = VeciSwapPairs(a);
    v128u m = VeciMax(a, t);
    return VeciGetX(VeciMax(m, VeciSwapHalves(m)));
}

purefn f32 VCALL Min3(v128f a)
{
    VecSetW(a, FLT_MAX);
    return VecMinVal(a);
}

purefn f32 VCALL Max3(v128f a)
{
    VecSetW(a, -FLT_MAX);
    return VecMaxVal(a);
}

purefn u32 VCALL Mini3(v128u a)
{
    return VeciMinVal(VeciSetR(VeciGetX(a), VeciGetY(a), VeciGetZ(a), UINT32_MAX));
}

purefn u32 VCALL Maxi3(v128u a)
{
    return VeciMaxVal(VeciSetR(VeciGetX(a), VeciGetY(a), VeciGetZ(a), 0u));
}

#define VecClamp01(v) VecClamp(v, VecZero(), VecOne())

static forceinline void VCALL Vec3Store(float* f, v128f v)
{
    f[0] = VecGetX(v);
    f[1] = VecGetY(v);
    f[2] = VecGetZ(v);
}

purefn v128f Vec3Proj(v128f v, v128f n)
{
    float num = Vec3DotfV(n, n);
    if (num < MATH_Epsilon) return v;
    float num2 = Vec3DotfV(v, n);
    return VecSub(v, VecMulf(n, num2 / num));
}

purefn v128f Vec3Reflect(v128f in, v128f normal) 
{
    return VecSub(in, VecMul(normal, VecMulf(VecDot(normal, in), 2.0f)));
}

// Valid input range -1..1 output is -pi..pi
purefn v128f ACosV(v128f x)   
{
    // Lagarde 2014, "Inverse trigonometric functions GPU optimization for AMD GCN architecture"
    // This is the approximation of degree 1, with a max absolute error of 9.0x10^-3
    v128f y = VecFabs(x);
    v128f p = VecFmadd(VecSet1(-0.1565827f), y, VecSet1(1.570796f));
    p = VecMul(p, VecSqrt(VecSub(VecOne(), y)));
    // x >= 0 ? p : pi - p, like the scalar ACos
    return VecSelect(VecSub(VecSet1(MATH_PI), p), p, VecCmpGe(x, VecZero()));
}

purefn f32 Vec3Angle(v128f a, v128f b) {
    v128f dot = VecMul(Vec3DotV(a, b), VecRSqrt(VecMul(Vec3DotV(a, a), Vec3DotV(b, b))));
    dot = VecClamp(dot, VecSet1(-1.0f), VecSet1(1.0f));
    return VecGetX(ACosV(dot));
}

purefn v128f VCALL VecHSum(v128f v) {
    v = VecHadd(v, v); // high half -> low half
    return VecHadd(v, v);
}

purefn v128f VCALL VecCopySign(v128f x, v128f y)
{
    v128u clearedX = VeciAnd(VecBitcastU32(x), VeciSet1(0x7fffffff));
    v128u signY    = VeciAnd(VecBitcastU32(y), VeciSet1(0x80000000));
    v128u res      = VeciOr(clearedX, signY);
    return VeciBitcastF32(res);
}

purefn v128f VCALL VecLerp(v128f x, v128f y, f32 t)
{
    return VecFmadd(VecSub(y, x), VecSet1(t), x);
}

purefn v128f VCALL VecStep(v128f edge, v128f x)
{
    return VecBlend(VecZero(), VecOne(), VecCmpGt(x, edge));
}

purefn v128f VCALL VecFract(v128f x)
{
    return VecSub(x, VecFloor(x));
}

//------------------------------------------------------------------------------

purefn v128f VCALL VecModAngles(v128f angles)
{
    v128f twoPi       = VecSet1(2.0f * MATH_PI);
    v128f recipTwoPi  = VecSet1(1.0f / (2.0f * MATH_PI));
    v128f v = VecMul(angles, recipTwoPi); // Multiply by 1/(2*pi)
    v = VecRound(v); 
    return VecSub(angles, VecMul(v, twoPi));
}

purefn v128f VCALL VecSin(const v128f V)
{
    v128f SC0 = VecSetR(-0.16666667f, +0.0083333310f, -0.00019840874f, +2.7525562e-06f);
    v128f SC1 = VecSetR(-2.3889859e-08f, -0.16665852f, +0.0083139502f, -0.00018524670f);
    v128f x       = VecModAngles(V);
    v128f signbit = VecAnd(x, VecNegZero());
    v128f c       = VecOr(VecSet1(MATH_PI), signbit);
    v128f absx    = VecAndNot(signbit, x);
    v128f rflx    = VecSub(c, x);
    v128f comp    = VecCmpLe(absx, VecSet1(MATH_HalfPI));

    x = VecBlend(rflx, x, comp);
    v128f x2 = VecMul(x, x);
    // Horner with explicit splats (same order as DXMath/SSE version)
    v128f Result = VecFmadd(VecSplatX(SC1), x2, VecSplatW(SC0));
    Result = VecFmadd(Result, x2, VecSplatZ(SC0));
    Result = VecFmadd(Result, x2, VecSplatY(SC0));
    Result = VecFmadd(Result, x2, VecSplatX(SC0));
    Result = VecFmadd(Result, x2, VecOne());
    return VecMul(Result, x);
}

purefn v128f VCALL VecCos(const v128f V)
{
    v128f CC0 = VecSetR(-0.5f, +0.041666638f, -0.0013888378f, +2.4760495e-05f);
    v128f CC1 = VecSetR(-2.6051615e-07f, -0.49992746f, +0.041493919f, -0.0012712436f);
    v128f x       = VecModAngles(V);
    v128f signbit = VecAnd(x, VecNegZero());   // sign bit only
    v128f c       = VecOr(VecSet1(MATH_PI), signbit);
    v128f absx    = VecAndNot(signbit, x);
    v128f rflx    = VecSub(c, x);
    v128f comp    = VecCmpLe(absx, VecSet1(MATH_HalfPI));

    x = VecBlend(rflx, x, comp);
    
    v128f sign = VecBlend(VecNegativeOne(), VecOne(), comp);
    v128f x2   = VecMul(x, x);
    v128f R    = VecFmadd(VecSplatX(CC1), x2, VecSplatW(CC0));
    R = VecFmadd(R, x2, VecSplatZ(CC0));
    R = VecFmadd(R, x2, VecSplatY(CC0));
    R = VecFmadd(R, x2, VecSplatX(CC0));
    R = VecFmadd(R, x2, VecOne());
    return VecMul(R, sign);
}

static inline void VCALL VecSinCos(v128f V, v128f* pSin, v128f* pCos)
{
    v128f SC0 = VecSetR( -0.16666667f   , +0.0083333310f, -0.00019840874f, +2.7525562e-06f );
    v128f SC1 = VecSetR( -2.3889859e-08f, -0.16665852f /*Est1*/, +0.0083139502f /*Est2*/, -0.00018524670f /*Est3*/ );
    v128f CC0 = VecSetR( -0.500000000f  , +0.041666638f, -0.0013888378f, +2.4760495e-05f );
    v128f CC1 = VecSetR( -2.6051615e-07f, -0.49992746f /*Est1*/, +0.041493919f /*Est2*/, -0.0012712436f /*Est3*/ );

    v128f x       = VecModAngles(V);
    v128f signbit = VecAnd(x, VecNegZero());
    v128f c       = VecOr(VecSet1(MATH_PI), signbit);
    v128f absx    = VecAndNot(signbit, x);
    v128f rflx    = VecSub(c, x);
    v128f comp    = VecCmpLe(absx, VecSet1(MATH_HalfPI));

    x = VecBlend(rflx, x, comp); // x = comp ? x : rflx
    v128f s0   = VecAnd(comp, VecOne());            // +1
    v128f s1   = VecAndNot(comp, VecNegativeOne()); // -1
    v128f sign = VecOr(s0, s1);

    v128f x2 = VecMul(x, x);
    v128f R;
    R     = VecFmadd(VecSplatX(SC1), x2, VecSplatW(SC0));
    R     = VecFmadd(R, x2, VecSplatZ(SC0));
    R     = VecFmadd(R, x2, VecSplatY(SC0));
    R     = VecFmadd(R, x2, VecSplatX(SC0));
    R     = VecFmadd(R, x2, VecOne());
    *pSin = VecMul(R, x);

    R     = VecFmadd(VecSplatX(CC1), x2, VecSplatW(CC0));
    R     = VecFmadd(R, x2, VecSplatZ(CC0));
    R     = VecFmadd(R, x2, VecSplatY(CC0));
    R     = VecFmadd(R, x2, VecSplatX(CC0));
    R     = VecFmadd(R, x2, VecOne());
    *pCos = VecMul(R, sign);
}

purefn v128f VCALL VecAtan(v128f x)
{
    const f32 sa1 =  0.99997726f, sa3 = -0.33262347f, sa5  = 0.19354346f,
    sa7 = -0.11643287f, sa9 =  0.05265332f, sa11 = -0.01172120f;
      
    const v128f xx = VecMul(x, x);
    v128f res = VecSet1(sa11); 
    res = VecFmadd(xx, res, VecSet1(sa9));
    res = VecFmadd(xx, res, VecSet1(sa7));
    res = VecFmadd(xx, res, VecSet1(sa5));
    res = VecFmadd(xx, res, VecSet1(sa3));
    res = VecFmadd(xx, res, VecSet1(sa1));
    return VecMul(x, res);
}

purefn v128f VCALL VecAtan2(v128f y, v128f x)
{
    v128f ay = VecFabs(y), ax = VecFabs(x);
    v128f swapMask = VecCmpGt(ay, ax);
    v128f z  = VecDiv(VecBlend(ay, ax, swapMask), VecBlend(ax, ay, swapMask));
    v128f th = VecAtan(z);
    th = VecSelect(th, VecSub(VecSet1(MATH_HalfPI), th), swapMask);
    th = VecSelect(th, VecSub(VecSet1(MATH_PI), th), VecCmpLt(x, VecZero()));
    return VecCopySign(th, y);
}

purefn u8 IsPointInsideAABB(v128f point, v128f aabbMin, v128f aabbMax)
{
    v128f cmpMin = VecCmpGe(point, aabbMin);
    v128f cmpMax = VecCmpLe(point, aabbMax);
    u32 movemask = VecMovemask(VecAnd(cmpMin, cmpMax));
    return (movemask & 0b111) == 0b111;
}

purefn f32 VCALL IntersectAABB(v128f origin, v128f invDir, v128f aabbMin, v128f aabbMax, f32 minSoFar)
{
    if (IsPointInsideAABB(origin, aabbMin, aabbMax)) return 0.1f;
    v128f tmin = VecMul(VecSub(aabbMin, origin), invDir);
    v128f tmax = VecMul(VecSub(aabbMax, origin), invDir);
    f32 tnear = Max3(VecMin(tmin, tmax));
    f32 tfar  = Min3(VecMax(tmin, tmax));
    // return tnear < tfar && tnear > 0.0f && tnear < minSoFar;
    if (tnear < tfar && tnear > 0.0f && tnear < minSoFar)
        return tnear; else return 1e30f;
}

typedef struct RayHit_
{
    f32 t;            // ray distance
    f32 u, v;         // barycentric coordinates
} RayHit;

// moller trumbore, ported from the old engine. dir may be unnormalized so t stays
// comparable across differently scaled instances
static inline bool IntersectTriangle(v128f origin, v128f dir, v128f v0, v128f v1, v128f v2, RayHit* hit)
{
    v128f edge1 = VecSub(v1, v0);
    v128f edge2 = VecSub(v2, v0);
    v128f h = Vec3Cross(dir, edge2);
    f32 a = Vec3DotfV(edge1, h);
    if (a > -1.0e-9f && a < 1.0e-9f) return false;

    f32 f = 1.0f / a;
    v128f s = VecSub(origin, v0);
    f32 u = f * Vec3DotfV(s, h);
    bool fail = (u < 0.0f) | (u > 1.0f);

    v128f q = Vec3Cross(s, edge1);
    f32 v = f * Vec3DotfV(dir, q);
    f32 t = f * Vec3DotfV(edge2, q);
    fail |= (v < 0.0f) | (u + v > 1.0f);

    if (!fail & (t > 0.0001f) & (t < hit->t))
    {
        hit->u = u; hit->v = v; hit->t = t;
        return true;
    }
    return false;
}

// ray vs plane through planeOrigin with the given normal. out: false when parallel
static inline bool RayPlaneHit(v128f rayOrigin, v128f rayDir, v128f planeOrigin, v128f normal, v128f* outHit)
{
    f32 dn = Vec3DotfV(rayDir, normal);
    if (Absf32(dn) < 1.0e-6f) return false;
    f32 t = Vec3DotfV(VecSub(planeOrigin, rayOrigin), normal) / dn;
    if (t <= 0.0f) return false;
    *outHit = VecAdd(rayOrigin, VecMulf(rayDir, t));
    return true;
}

purefn f32 Sqrf(f32 x) {
    return x * x;
}

purefn f32 Sqrtf(f32 a) {
    #if defined(AX_SUPPORT_SSE) || defined(AX_SUPPORT_NEON)
    return VecGetX(VecSqrt(VecSet1(a)));
    #else
    return __builtin_sqrt(a);
    #endif
}

purefn f32 Lerpf(f32 x, f32 y, f32 t) {
    return x + (y - x) * t;
}

purefn u8 IsZerof(f32 x) {
    return Absf32(x) <= 0.0001f; 
}

purefn u8 AlmostEqualf(f32 x, f32 y) {
    return Absf32(x-y) <= 0.001f;
}

purefn f32 Signf(f32 x) {
    f32 one = 1.0f;
    s32 res = BitCast(s32, one);
    s32 r1 = BitCast(s32, x);
    res |= r1 & 0x80000000;
    return BitCast(f32, res);
} 

purefn s32 Sign32(s32 x) {
    return x < 0 ? -1 : 1; // equal to above f1 version
} 

purefn f32 CopySignf(f32 x, f32 y) {
    s32 ix = BitCast(s32, x) & 0x7fffffff;
    s32 iy = BitCast(s32, y) & 0x80000000;
    s32 ir = ix | iy;
    return BitCast(f32, ir);
}

purefn u8 IsNanf(f32 f) {
    u32 intValue = BitCast(u32, f);
    u32 exponent = (intValue >> 23) & 0xFF;
    u32 fraction = intValue & 0x7FFFFF;
    return (exponent == 0xFF) && (fraction != 0);
}

purefn s64 Int64MulDiv(s64 value, s64 numer, s64 denom) {
    s64 q = value / denom;
    s64 r = value % denom;
    return q * numer + r * numer / denom;
}

purefn f32 FModf(f32 x, f32 y) 
{
    f32 quotient = x / y;
    s32 whole = (s32)quotient;  // truncate toward zero
    f32 remainder = x - (f32)whole * y;
    if (remainder < 0.0f) remainder += y;
    return remainder;
}

purefn f32 FMod(f32 x, f32 y) 
{
    f32 quotient = x / y;
    s64 whole = (s64)quotient;  // truncate toward zero
    f32 remainder = x - (f32)whole * y;
    if (remainder < 0.0) remainder += y;
    return remainder;
}

purefn s32 FloorDiv(s32 a, s32 b)
{
    s32 q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

// https://github.com/id-Software/DOOM-3/blob/master/neo/idlib/math/Math.h
purefn f32 Expf(f32 f) {
    const s32 IEEE_FLT_MANTISSA_BITS  =	23;
    const s32 IEEE_FLT_EXPONENT_BITS  =	8;
    const s32 IEEE_FLT_EXPONENT_BIAS  =	127;
    const s32 IEEE_FLT_SIGN_BIT       =	31;
    
    f32 x = f * 1.44269504088896340f; // multiply with ( 1 / log( 2 ) )
    
    s32 i = BitCast(s32, x);
    s32 s = ( i >> IEEE_FLT_SIGN_BIT );
    s32 e = ( ( i >> IEEE_FLT_MANTISSA_BITS ) & ( ( 1 << IEEE_FLT_EXPONENT_BITS ) - 1 ) ) - IEEE_FLT_EXPONENT_BIAS;
    s32 m = ( i & ( ( 1 << IEEE_FLT_MANTISSA_BITS ) - 1 ) ) | ( 1 << IEEE_FLT_MANTISSA_BITS );
    i = ( ( m >> ( IEEE_FLT_MANTISSA_BITS - e ) ) & ~( e >> 31 ) ) ^ s;
    
    s32 exponent = ( i + IEEE_FLT_EXPONENT_BIAS ) << IEEE_FLT_MANTISSA_BITS;
    f32 y = BitCast(f32, exponent);
    x -= (f32) i;
    if ( x >= 0.5f ) {
        x -= 0.5f;
        y *= 1.4142135623730950488f;	// multiply with sqrt( 2 )
    }
    f32 x2 = x * x;
    f32 p = x * ( 7.2152891511493f + x2 * 0.0576900723731f );
    f32 q = 20.8189237930062f + x2;
    x = y * ( q + p ) / ( q - p );
    return x;
}

// https://martin.ankerl.com/2012/01/25/optimized-approximative-pow-in-c-and-cpp/
// https://github.com/ekmett/approximate/blob/master/cbits/fast.c#L81
// exact for integer exponents, approximate for the fractional part. a must be > 0
purefn f32 Powf(f32 a, f32 b) {
    // approximate a^fract(b) by scaling the float exponent bits (0x3F800000 = 1.0f)
    s32 e = (s32)b;
    union { f32 f; s32 i; } u = { a };
    u.i = (s32)((b - (f32)e) * (f32)(u.i - 1065353216) + 1065353216.0f);

    // exponentiation by squaring with the exponent's integer part
    u32 n = (u32)(e < 0 ? -e : e);
    f32 r = 1.0f;
    while (n) {
        if (n & 1u) r *= a;
        a *= a;
        n >>= 1;
    }
    return e < 0 ? u.f / r : u.f * r;
}

// https://github.com/ekmett/approximate/blob/master/cbits/fast.c#L81 <--you can find d1 versions
purefn f32 Logf(f32 x) {
    return (BitCast(s32, x) - 1064866805) * 8.262958405176314e-8f;
}

// if you want log10 for integer you can look at Algorithms.hpp
purefn f32 Log10f(f32 x) { 
    return Logf(x) / 2.30258509299f; // ln(x) / ln(10)
} 

// you might look at this link as well: 
// https://tech.ebayinc.com/engineering/fast-approximate-logarithms-part-i-the-basics/
// A Collection of f1 Tricks pdf
purefn f32 Log2f(f32 val) {
    f32 result = (f32)*((s32*)&val);
    result *= 1.0f / (1 << 23);
    result = result - 127.0f;
    f32 tmp = result - Floorf32(result);
    tmp = (tmp - tmp*tmp) * 0.346607f;
    return tmp + result; // ln(x) / ln(2) 
}

// https://graphics.stanford.edu/~seander/bithacks.html#IntegerLog
purefn u32 Log2u32(u32 v) {
    u8 const MultiplyDeBruijnBitPosition[32] = 
    {
        0, 9, 1, 10, 13, 21, 2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
        8, 12, 20, 28, 15, 17, 24, 7, 19, 27, 23, 6, 26, 5, 4, 31
    };
    
    // first round down to one less than a power of 2
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return MultiplyDeBruijnBitPosition[(u32)(v * 0x07C4ACDDU) >> 27];
}

purefn u32 Log10_32(u32 v) {
    // this would work too
    const u32 PowersOf10[] = {             // if (n <= 9) return 1;
        1, 10, 100, 1000, 10000, 100000,            // if (n <= 99) return 2;
        1000000, 10000000, 100000000, 1000000000    // if (n <= 999) return 3;
    };                                              // ...
    u32 t = (Log2u32(v) + 1) * 1233 >> 12;    // if (n <= 2147483647) return 10; // 2147483647 = i32 max
    return t - (v < PowersOf10[t]);                                                      
}                            

static inline u64 Pow10Table64(s32 numDigits)
{
    const u64 Pow10Table[20] = {
        1ULL,
        10ULL,
        100ULL,
        1000ULL,
        10000ULL,
        100000ULL,
        1000000ULL,
        10000000ULL,
        100000000ULL,
        1000000000ULL,
        10000000000ULL,
        100000000000ULL,
        1000000000000ULL,
        10000000000000ULL,
        100000000000000ULL,
        1000000000000000ULL,
        10000000000000000ULL,
        100000000000000000ULL,
        1000000000000000000ULL,
        10000000000000000000ULL
    };
    return Pow10Table[numDigits];
}

static inline s32 Log10_64(u64 v)
{
    s32 l2 = 63 - LeadingZeroCount64(v);
    s32 d = (s32)((l2 + 1) * 0.30103);
    return d + (v >= Pow10Table64(d));
}


/*//////////////////////////////////////////////////////////////////////////*/
/*                      Trigonometric Functions                             */
/*//////////////////////////////////////////////////////////////////////////*/

// https://mazzo.li/posts/vectorized-atan2.html
purefn f32 ATan(f32 x) 
{
    return VecGetX(VecAtan(VecSet1(x)));
}

// Warning! if y and x is zero this will return HalfPI instead of 0.0f unlike cstdlib
purefn f32 ATan2(f32 y, f32 x) {
    return VecGetX(VecAtan2(VecSet1(y), VecSet1(x)));
}

// Valid input range -1..1 output is -pi..pi
purefn f32 ACos(f32 x)   
{
    // Lagarde 2014, "Inverse trigonometric functions GPU optimization for AMD GCN architecture"
    // This is the approximation of degree 1, with a max absolute error of 9.0x10^-3
    f32 y = Absf32(x);
    f32 p = -0.1565827f * y + 1.570796f;
    p *= Sqrtf(1.0f - y);
    return x >= 0.0f ? p : MATH_PI - p;
}

purefn f32 ACosPositive(f32 x)
{
    f32 p = -0.1565827f * x + 1.570796f;
    return p * Sqrtf(1.0f - x);
}

// Same cost as Acos + 1 FR Same error
// input [-1, 1] and output [-PI/2, PI/2]
purefn f32 ASin(f32 x) {
    return MATH_HalfPI - ACos(x);
}

// https://en.wikipedia.org/wiki/Sine_and_cosine
// warning: accepts input between -TwoPi and TwoPi  if (Abs(x) > TwoPi) use x = FMod(x + PI, TwoPI) - PI;

purefn f32 RepeatPI(f32 x) {
    return FModf(x + MATH_PI, MATH_TwoPI) - MATH_PI;
}

// https://en.wikipedia.org/wiki/Fast_inverse_square_root
// https://rrrola.wz.cz/inv_sqrt.html   <- fast and 2.5x accurate
purefn f32 RSqrtf(f32 x) {
    #ifdef AX_SUPPORT_SSE
    // when I compile with godbolt -O1 expands to one instruction vrsqrtss
    // which is approximately equal latency as mulps.
    // RSqrt(float):                             # @RSqrt3(float)
    //       vrsqrtss        xmm0, xmm0, xmm0
    //       ret
    return _mm_cvtss_f32(_mm_rsqrt_ps(_mm_set_ps1(x)));
    #elif defined(AX_SUPPORT_NEON)
    return vget_lane_f32(vrsqrte_f32(vdup_n_f32(x)), 0);
    #else
    f32 f = x;
    u32 i = BitCast(u32, f);
    i = (u32)(0x5F1FFFF9ul - (i >> 1));
    f = BitCast(f32, i);
    return 0.703952253f * f * (2.38924456f - x * f * f);
    #endif
}

// cbrt
purefn f32 CubeRootf(f32 val)
{
    const f32 fPower = 1.0f / 3.0f;
    const f32 fScale = 1.0f;
    const f32 oneRepresentationAsf1 = (f32)(0x3f800000);
    f32 magicValue = (f32)((*((const u32*)&fScale))) - (oneRepresentationAsf1 * fPower);
    f32 tmp = (f32)*((u32*)&val);
    tmp = (tmp * fPower) + magicValue;
    u32 tmp2 = (u32)tmp;
    return *(f32*)&tmp2;
}

purefn f32 Sin0pi(f32 x) {
    x *= 0.63655f; // constant founded using desmos
    x *= 2.0f - x;
    return x * (0.225f * x + 0.775f); 
}

purefn f32 Sin(f32 x) 
{
    return VecGetX(VecSin(VecSet1(x)));
}

// Accepts input between -TwoPi and TwoPi, use CosR if value is bigger than this range  
purefn f32 Cos(f32 x)
{
    return VecGetX(VecCos(VecSet1(x)));
}

// R suffix allows us to use with greater range than -TwoPI, TwoPI
purefn f32 SinR(f32 x) {
    return Sin(FModf(x + MATH_PI, MATH_TwoPI) - MATH_PI);
}

// R suffix allows us to use with greater range than -TwoPI, TwoPI
purefn f32 CosR(f32 x) {
    return Cos(FModf(x + MATH_PI, MATH_TwoPI) - MATH_PI);
}

static forceinline void SinCos(f32 x, f32* sp, f32* cp) 
{
    *sp = Sin(x);
    *cp = Cos(x);
}

// https://github.com/id-Software/DOOM-3/blob/master/neo/idlib/math/Math.h
// tanf equivalent
purefn f32 Tan(f32 a) {
    f32 s = 0.0f;
    u8 reciprocal = false;

    if (( a < 0.0f ) || (a >= MATH_PI)) {
        a -= Floorf32(a / MATH_PI) * MATH_PI;
    }
    
    if ( a < MATH_HalfPI ) {
        u8 greater = a > MATH_QuarterPI;
        reciprocal = greater;
        if (greater) a = MATH_HalfPI - a;
    } else {
        u8 greater = a > MATH_HalfPI + MATH_QuarterPI;
        reciprocal = !greater;
        a = greater ? a - MATH_PI : MATH_HalfPI - a;
    }

    s = a * a;
    s = a * ((((((9.5168091e-03f * s + 2.900525e-03f ) * s + 2.45650893e-02f) * s + 5.33740603e-02f) * s + 1.333923995e-01f) * s + 3.333314036e-01f) * s + 1.0f);
    return reciprocal ? 1.0f / s : s;
}

// inspired from Casey Muratori's performance aware programming
// this functions makes the code more readable. OpenCL and Cuda has the same functions as well
purefn f32 ATan2PI(f32 y, f32 x) { return ATan2(y, x) / MATH_PI; }
purefn f32 ASinPI(f32 z) { return ASin(z) / MATH_PI; }
purefn f32 ACosPI(f32 x) { return ACos(x) / MATH_PI; }
purefn f32 CosPI(f32 x)  { return Cos(x) / MATH_PI; }
purefn f32 SinPI(f32 x)  { return Sin(x) / MATH_PI; }

///////////////////////////////////////////////////////////////////////////
// Packing

// packs -1,1 range f1 to short
purefn u16 PackSnorm16(f32 x) {
    return (u16)(s16)Clampf32(x * (f32)INT16_MAX, (f32)INT16_MIN, (f32)INT16_MAX);
}

purefn f32 UnpackSnorm16(u16 x) {
    return (f32)(s16)x / (f32)INT16_MAX;
}

// packs 0,1 range f1 to short
purefn u16 PackUnorm16(f32 x) {
    return (u16)Clampf32(x * (f32)UINT16_MAX, 0.0f, (f32)UINT16_MAX);
}

purefn f32 UnpackUnorm16(u16 x) {
    return (f32)x / (f32)UINT16_MAX;
}

///////////////////////////////////////////////////////////////////////////
// Easing

// to see visually: https://easings.net/ 
purefn f32 EaseIn(f32 x) {
    return x * x;
}

purefn f32 EaseOut(f32 x) { 
    f32 r = 1.0f - x;
    return 1.0f - (r * r); 
}

purefn f32 EaseInOut(f32 x) {
    return x < 0.5f ? 2.0f * x * x : 1.0f - Sqrf(-2.0f * x + 2.0f) / 2.0f;
}

// integral symbol shaped interpolation, similar to EaseInOut
purefn f32 SmoothStep(f32 edge0, f32 edge1, f32 x) {
    f32 t = Saturatef32((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - t * 2.0f);
}

purefn f32 EaseInSine(f32 x) {
    return 1.0f - Cos((x * MATH_PI) * 0.5f);
}

purefn f32 EaseOutSine(f32 x) {
    return Sin((x * MATH_PI) * 0.5f);
}


///////////////////////////////////////////////////////////////////////////
// Other

// Gradually changes a value towards a desired goal over time.
purefn f32 SmoothDamp(f32 current, f32 target, f32* currentVelocity, f32 smoothTime, f32 maxSpeed, f32 deltaTime)
{
    // Based on Game Programming Gems 4 Chapter 1.10
    smoothTime = MMAX(0.0001f, smoothTime);
    f32 omega = 2.0f / smoothTime;

    f32 x = omega * deltaTime;
    f32 exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    f32 change = current - target;
    f32 originalTo = target;

    // Clamp maximum speed
    f32 maxChange = maxSpeed * smoothTime;
    change = Clampf32(change, -maxChange, maxChange);
    target = current - change;

    f32 temp = (*currentVelocity + omega * change) * deltaTime;
    *currentVelocity = (*currentVelocity - omega * temp) * exp;
    f32 output = target + (change + temp) * exp;

    // Prevent overshooting
    if (originalTo - current > 0.0f == output > originalTo)
    {
        output = originalTo;
        *currentVelocity = (output - originalTo) / deltaTime;
    }

    return output;
}

purefn f32 Remap(f32 in, f32 inMin, f32 inMax, f32 outMin, f32 outMax)
{
    return outMin + (in - inMin) * (outMax - outMin) / (inMax - inMin);
}

purefn f32 Repeat(f32 t, f32 length)
{
    return Clampf32(t - Floorf32(t / length) * length, 0.0f, length);
}

purefn f32 Step(f32 edge, f32 x)
{
    return (f32)(x > edge);
}

// https://en.wikipedia.org/wiki/Distance_from_a_point_to_a_line
// position(x0, y0), linePos1(x1, y1), linePos2(x2, y2) 
purefn f32 LineDistance(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2)
{
    f32 a = ((x2 - x1) * (y0 - y1)) - ((x0 - x1) * (y2 - y1));
    return Absf32(a) * RSqrtf(Sqrf(x2 - x1) + Sqrf(y2 - y1));
}

#endif // MATH_H
