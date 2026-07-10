
#ifndef Vector_H
#define Vector_H

#include "Math.h"
#include "Half.h"

typedef struct Vec2_  { f32 x, y;           } float2;
typedef struct Vec3f_ { f32 x, y, z;        } float3;
typedef struct Int2_  { s32 x, y;           } int2;
typedef struct Int3_  { s32 x, y, z;        } int3;
typedef struct uInt2_ { u32 x, y;           } uint2;
typedef struct uInt3_ { u32 x, y, z;        } uint3;
typedef struct Ray_   { float3 origin, dir; } Ray;
typedef struct RayV_  { v128f  origin, dir; } RayV;

typedef const float3* cf3;
purefn float3 F3Add (float3 a, float3 b)  { return (float3) { a.x + b.x, a.y + b.y, a.z + b.z }; }
purefn float3 F3Sub (float3 a, float3 b)  { return (float3) { a.x - b.x, a.y - b.y, a.z - b.z }; }
purefn float3 F3Mul (float3 a, float3 b)  { return (float3) { a.x * b.x, a.y * b.y, a.z * b.z }; }
purefn float3 F3Div (float3 a, float3 b)  { return (float3) { a.x / b.x, a.y / b.y, a.z / b.z }; }
purefn float2 F2Add (float2 a, float2 b)  { return (float2) { a.x + b.x, a.y + b.y }; }
purefn float2 F2Sub (float2 a, float2 b)  { return (float2) { a.x - b.x, a.y - b.y }; }
purefn float2 F2Mul (float2 a, float2 b)  { return (float2) { a.x * b.x, a.y * b.y }; }
purefn float2 F2Div (float2 a, float2 b)  { return (float2) { a.x / b.x, a.y / b.y }; }
purefn float3 F3AddF(float3 a, f32 b)  { return (float3) { a.x + b, a.y + b, a.z + b }; }
purefn float3 F3SubF(float3 a, f32 b)  { return (float3) { a.x - b, a.y - b, a.z - b }; }
purefn float3 F3MulF(float3 a, f32 b)  { return (float3) { a.x * b, a.y * b, a.z * b }; }
purefn float3 F3DivF(float3 a, f32 b)  { return (float3) { a.x / b, a.y / b, a.z / b }; }
purefn float2 F2AddF(float2 a, f32 b)  { return (float2) { a.x + b, a.y + b }; }
purefn float2 F2SubF(float2 a, f32 b)  { return (float2) { a.x - b, a.y - b }; }
purefn float2 F2MulF(float2 a, f32 b)  { return (float2) { a.x * b, a.y * b }; }
purefn float2 F2DivF(float2 a, f32 b)  { return (float2) { a.x / b, a.y / b }; }
purefn float2 F2Neg (float2 a)         { return (float2) { -a.x, -a.y }; }
purefn float3 F3Neg (float3 a)         { return (float3) { -a.x, -a.y, -a.z }; }
purefn int3 I3Add (int3 a, int3 b)     { return (int3) { a.x + b.x, a.y + b.y, a.z + b.z }; }
purefn int3 I3Sub (int3 a, int3 b)     { return (int3) { a.x - b.x, a.y - b.y, a.z - b.z }; }
purefn int3 I3Mul (int3 a, int3 b)     { return (int3) { a.x * b.x, a.y * b.y, a.z * b.z }; }
purefn int3 I3Div (int3 a, int3 b)     { return (int3) { a.x / b.x, a.y / b.y, a.z / b.z }; }
purefn int3 I3AddI(int3 a, s32 b)      { return (int3) { a.x + b, a.y + b, a.z + b }; }
purefn int3 I3SubI(int3 a, s32 b)      { return (int3) { a.x - b, a.y - b, a.z - b }; }
purefn int3 I3MulI(int3 a, s32 b)      { return (int3) { a.x * b, a.y * b, a.z * b }; }
purefn int3 I3DivI(int3 a, s32 b)      { return (int3) { a.x / b, a.y / b, a.z / b }; }
purefn int2 I2Add (int2 a, int2 b)     { return (int2) { a.x + b.x, a.y + b.y }; }
purefn int2 I2Sub (int2 a, int2 b)     { return (int2) { a.x - b.x, a.y - b.y }; }
purefn int2 I2Mul (int2 a, int2 b)     { return (int2) { a.x * b.x, a.y * b.y }; }
purefn int2 I2Div (int2 a, int2 b)     { return (int2) { a.x / b.x, a.y / b.y }; }
purefn int2 I2AddI(int2 a, s32 b)      { return (int2) { a.x + b, a.y + b }; }
purefn int2 I2SubI(int2 a, s32 b)      { return (int2) { a.x - b, a.y - b }; }
purefn int2 I2MulI(int2 a, s32 b)      { return (int2) { a.x * b, a.y * b }; }
purefn int2 I2DivI(int2 a, s32 b)      { return (int2) { a.x / b, a.y / b }; }
purefn int3 I3Neg (int3 a)             { return (int3) { -a.x, -a.y, -a.z }; }
purefn int2 I2Neg (int2 a)             { return (int2) { -a.x, -a.y }; }
purefn f32 F2Len  (float2 a)           { return Sqrtf(a.x * a.x + a.y * a.y); }
purefn f32 F3Len  (float3 a)           { return Vec3LenfV(Vec3Load(&a.x)); }
purefn f32 I2Len  (int2 a)             { return Sqrtf(a.x * a.x + a.y * a.y); }
purefn f32 I3Len  (int3 a)             { return Sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
purefn f32 I2LenSq(int2 a)             { return (a.x * a.x + a.y * a.y); }
purefn float2 F2Frac (float2 a)        { return (float2){ Fractf32(a.x)   , Fractf32(a.y)    }; }
purefn float2 F2Floor(float2 a)        { return (float2){ Floorf32(a.x), Floorf32(a.y) }; }
purefn float2 F2Sin  (float2 a)        { return (float2){ Sin(a.x)     , Sin(a.y)      }; }
purefn float3 F3Frac (float3 a)        { return (float3){ Fractf32(a.x)   , Fractf32(a.y)    , Fractf32(a.z) }; }
purefn float3 F3Floor(float3 a)        { return (float3){ Floorf32(a.x), Floorf32(a.y) , Floorf32(a.z) }; }
purefn float3 F3Sin  (float3 a)        { return (float3){ Sin(a.x)     , Sin(a.y)      , Sin(a.z)   }; }
purefn f32 F2LenSq(float2 a)           { return (a.x * a.x + a.y * a.y); }
purefn f32 F2Dot (float2 a, float2 b)  { return (a.x * b.x + a.y * b.y); }
purefn f32 F2Dist(float2 a, float2 b)  { return F2Len(F2Sub(a, b)); }
purefn f32 I2Dist(int2 a, int2 b)      { return I2Len(I2Sub(a, b)); }
purefn f32 I3Dist(int3 a, int3 b)      { return I3Len(I3Sub(a, b)); }
purefn float3 F3FromPtr(f32* ptr)      { return (float3){ ptr[0], ptr[1], ptr[2] }; }
purefn float3 F3Zero()                 { return (float3){ 0.0f,  0.0f,  0.0f}; }
purefn float3 F3One()                  { return (float3){ 1.0f,  1.0f,  1.0f}; }
purefn float3 F3Up()                   { return (float3){ 0.0f,  1.0f,  0.0f}; }
purefn float3 F3Left()                 { return (float3){-1.0f,  0.0f,  0.0f}; }
purefn float3 F3Down()                 { return (float3){ 0.0f, -1.0f,  0.0f}; }
purefn float3 F3Right()                { return (float3){ 1.0f,  0.0f,  0.0f}; }
purefn float3 F3Forward()              { return (float3){ 0.0f,  0.0f,  1.0f}; }
purefn float3 F3Backward()             { return (float3){ 0.0f,  0.0f, -1.0f}; }
purefn float2 F2Zero()                 { return (float2){ 0.0f,  0.0f }; }
purefn float2 F2One()                  { return (float2){ 1.0f,  1.0f }; }

purefn float3 VCALL Vec3Get(f4 v)             { return (float3) { VecGetX(v), VecGetY(v), VecGetZ(v) }; }
purefn f32    F3Dot     (float3 a, float3 b)  { return VecDotf(Vec3Load(&a.x), Vec3Load(&b.x));  }
purefn float3 F3NormSafe(float3 a)            { return F3DivF(a, MATH_Epsilon + Sqrtf(a.x * a.x + a.y * a.y + a.z * a.z)); }
purefn f32    F3DistSqr (cf3 a, cf3 b)        { return Vec3DistSqrfV(Vec3Load(&a->x), Vec3Load(&b->x));           }
purefn f32    F3Angle   (cf3 a, cf3 b)        { return Vec3Angle(Vec3Load(&a->x), Vec3Load(&b->x));               }
purefn f32    F3Dist    (cf3 a, cf3 b)        { return Vec3DistfV(Vec3Load(&a->x), Vec3Load(&b->x));              }
purefn float3 F3Norm    (float3 a)            { return Vec3Get(Vec3NormV(Vec3Load(&a.x)));                        }
purefn float3 F3NormEst (float3 a)            { return Vec3Get(Vec3NormEstV(Vec3Load(&a.x)));                     }
purefn float3 F3Proj    (cf3 v, cf3 n)        { return Vec3Get(Vec3Proj   (Vec3Load(&v->x), Vec3Load(&n->x)));    }
purefn float3 F3Reflect (cf3 i, cf3 n)        { return Vec3Get(Vec3Reflect(Vec3Load(&i->x), Vec3Load(&n->x)));    }
purefn float3 F3Cross   (cf3 a, cf3 b)        { return Vec3Get(Vec3Cross  (Vec3Load(&a->x), Vec3Load(&b->x)));    }
purefn float3 F3Lerp    (cf3 a, cf3 b, f32 t) { return Vec3Get(VecLerp    (Vec3Load(&a->x), Vec3Load(&b->x), t)); }
purefn float2 F2Lerp    (float2 a, float2 b, f32 t) { return (float2) { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }; }
purefn float3 F3Abs     (float3 a)            { return Vec3Get(VecFabs(Vec3Load(&a.x)));                        }
purefn float3 F3Min     (float3 a, float3 b)  { return Vec3Get(VecMin(Vec3Load(&a.x), Vec3Load(&b.x))); }
purefn float3 F3Max     (float3 a, float3 b)  { return Vec3Get(VecMax(Vec3Load(&a.x), Vec3Load(&b.x))); }
purefn int3   I3Min(int3 a, int3 b) { return (int3){Mins32(a.x, b.x), Mins32(a.y, b.y), Mins32(a.z, b.z)}; }
purefn int3   I3Max(int3 a, int3 b) { return (int3){Maxs32(a.x, b.x), Maxs32(a.y, b.y), Maxs32(a.z, b.z)}; }

purefn float2 F2Min(float2 a, float2 b) { return (float2) { Minf32(a.x, b.x), Minf32(a.y, b.y) }; }
purefn float2 F2Max(float2 a, float2 b) { return (float2) { Maxf32(a.x, b.x), Maxf32(a.y, b.y) }; }
purefn int2   I2Min(int2 a, int2 b) { return (int2) { Mins32(a.x, b.x), Mins32(a.y, b.y) }; }
purefn int2   I2Max(int2 a, int2 b) { return (int2) { Maxs32(a.x, b.x), Maxs32(a.y, b.y) }; }

purefn float2 F2Rotate(float2 vec, f32 angle) {
    f32 s = Sin(angle), c = Cos(angle);
    return (float2){vec.x * c - vec.y * s, vec.x * s + vec.y * c};
}

purefn float2 Tof22(int2 vec)  { return (float2){(f32)vec.x,(f32)vec.y }; }
purefn int2 ToInt2(float2 vec) { return (int2){(s32)vec.x,(s32)vec.y }; }
purefn float3 ToFloat3(int3 vec) { return (float3){ (f32)vec.x, (f32)vec.y, (f32)vec.z }; }
purefn int3 ToInt3(float3 vec)   { return (int3){ (s32)vec.x, (s32)vec.y, (s32)vec.z }; }
purefn bool F3Approx(float3 a, float3 b)
{
    const f32 tolerance = 0.0001f;
    float3 difference = F3Abs(F3Sub(a, b));
    return difference.x < tolerance && difference.y < tolerance && difference.z < tolerance;
}

purefn u64 PointBoxIntersection(float2 min, float2 max, float2 point) {
    return point.x <= max.x && point.y <= max.y && point.x >= min.x && point.y >= min.y;
}

purefn u64 RectPointIntersect(float2 min, float2 scale, float2 point) {
    float2 max = { min.x + scale.x, min.y + scale.y };
    return point.x <= max.x && point.y <= max.y && point.x >= min.x && point.y >= min.y;
}

purefn s32 GreaterThan2(float2 a, float2 b) { return (s32)(a.x > b.x) | ((s32)(a.y > b.y) << 1); }
purefn s32 LessThan2(float2 a, float2 b)    { return (s32)(a.x < b.x) | ((s32)(a.y < b.y) << 1); }
purefn s32 GreaterThan3(float3 a, float3 b) { return (s32)(a.x > b.x) | ((s32)(a.y > b.y) << 1) | ((s32)(a.z > b.z) << 2); }
purefn s32 LessThan3(float3 a, float3 b)    { return (s32)(a.x < b.x) | ((s32)(a.y < b.y) << 1) | ((s32)(a.z < b.z) << 2); }

purefn s32 All2(s32 msk) { return msk == 3u; }
purefn s32 All3(s32 msk) { return msk == 7u; }

purefn s32 Any2(s32 msk) { return msk > 0; }
purefn s32 Any3(s32 msk) { return msk > 0; }

typedef float2   f16_2;
typedef v128f f16_4;

#define F2Set1(val) (float2) { val, val }
#define F3Set1(val) (float3) { val, val, val }

static inline u32 PackHalf2(float2 v)
{
    return Float2ToHalf2(&v.x);
}

static inline f16_2 UnpackHalf2(u32 h)
{
    f16_2 res;
    Half2ToFloat2(&res.x, h);
    return res;
}

static inline f16_2 VecXY(v128f v)
{
    f16_2 res;
    VecStoreLo64(&res.x, VecBitcastU32(v));
    return res;
}

static inline f16_2 VecZW(v128f v)
{
    f16_2 res;
    VecStoreHi64(&res.x, VecBitcastU32(v));
    return res;
}

static inline f16_4 VecCombine(float2 a, float2 b)
{
    return VecSetR(a.x, a.y, b.x, b.y);
}

// procedural noise (cellular / value / gradient) lives in Math/Noise.h

#endif //Vector.h
