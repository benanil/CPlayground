#ifndef A_BITPACK
#define A_BITPACK

#include "Math.h"
#include "Quaternion.h"

#define cOneOverSqrt2 (0.70710678f)
#define cNumBits      (9)
#define c9Mask        ((1u << cNumBits) - 1)
#define c9MaxValue    (c9Mask - 1)
#define c10MaxValue   1023
#define c10Mask       0x3FFu

purefn u32 VCALL PackXY11Z10Snorm(v128f v)
{
    v = VecClamp(v, VecSet1(-1.0f), VecSet1(1.0f));
    v = VecMul(v, VecSetR(1023.f, 1023.f, 511.f, 0.f));
    v = VecRound(v);
    v128u i = VecF32ToI32(v);
    i = VeciAnd(i, VeciSetR(0x7FF, 0x7FF, 0x3FF, 0));
    i = VeciSll(i, VeciSetR(0, 11, 22, 0));
    i = VeciOr(i, VecSwapHalvesU(i));
    i = VeciOr(i, VecSwapPairsU(i));
    return VeciGetX(i);
}

purefn u32 VCALL PackXY11Z10Unorm(v128f v)
{
    v = VecClamp01(v);
    v = VecMul(v, VecSetR(2047.f, 2047.f, 1023.f, 0.f));
    v = VecRound(v);
    v128u i = VecF32ToI32(v);
    i = VeciAnd(i, VeciSetR(0x7FF, 0x7FF, 0x3FF, 0));
    i = VeciSll(i, VeciSetR(0, 11, 22, 0));
    i = VeciOr(i, VecSwapHalvesU(i));
    i = VeciOr(i, VecSwapPairsU(i));
    return VeciGetX(i);
}

purefn u32 VCALL PackXY11Z10UnormFixed(v128f v, float scale) {
	return PackXY11Z10Unorm(VecDivf(v, scale));
}

purefn v128f VCALL UnpackXY11Z10UnormFixed(u32 v, float scale) {
	v128u u = VeciSet1(v);
    u = VeciSrl(u, VeciSetR(0, 11, 22, 0));
	u = VeciAnd(u, VeciSetR(0x7FF, 0x7FF, 0x3FF, 0));
	v128f f = VecI32ToF32(u);
	f = VecDiv(f, VecSetR((f32)0x7FF, (f32)0x7FF, (f32)0x3FF, (f32)0));
	return VecMulf(f, scale);
}

static inline u64 VCALL PackUnorm16x4(v128f val)
{
    u64 result;
    v128u u32 = VecF32ToI32(VecMul(VecClamp01(val), VecSet1((f32)(UINT16_MAX - 1))));
    VecStoreLo64(&result, VecPack16(u32));
    return result;
}

static inline v128f VCALL UnpackUnorm16x4(u64 i16)
{
    const v128f inv = VecSet1(1.0f / (f32)(UINT16_MAX - 1));
    return VecMul(VecI32ToF32(VecUnpackLo32(VeciDup64(i16))), inv);
}

static inline u64 VCALL Pack16x4Fixed(v128f val, float scale)
{
	return PackUnorm16x4(VecDivf(val, scale));
}

static inline v128f VCALL Unpack16x4Fixed(u64 i16, float scale)
{
    return VecMulf(UnpackUnorm16x4(i16), scale);
}

// signed pack/unpack: quaternion components are negative half the time, the unsigned
// VecPack16/VecUnpackLo32 variants clamp or zero-extend them and break rotations
static inline void VCALL PackQuaternionS16Norm(v128f quat, u64* result)
{
    v128u u32 = VecF32ToI32(VecMulf(quat, INT16_MAX-1));
    VecStoreLo64(result, VecPack16S(u32));
}

static inline u64 VCALL PackQuaternionS16NormRet(v128f quat)
{
    u64 result;
    v128u u32 = VecF32ToI32(VecMulf(quat, INT16_MAX-1));
    VecStoreLo64(&result, VecPack16S(u32));
    return result;
}

static inline v128f VCALL UnpackQuaternionS16Norm1(u64 i16)
{
    const v128f inv = VecSet1(1.0f / (INT16_MAX - 1));
    return VecMul(VecI32ToF32(VecUnpackLo32S(VeciDup64(i16))), inv);
}

static inline void VCALL UnpackQuaternionS16Norm2(v128u i16, v128f* q0, v128f* q1)
{
    const v128f inv = VecSet1(1.0f / (INT16_MAX - 1));
    *q0 = VecMul(VecI32ToF32(VecUnpackLo32S(i16)), inv);
    *q1 = VecMul(VecI32ToF32(VecUnpackHi32S(i16)), inv);
}

static inline void PackTBNIntoQuaternion64(v128f normal, v128f tangent, u32* out)
{
    v128f binormal = Vec3Cross(tangent, normal);
    v128f quat = QuaternionFromM33Vec(binormal, tangent, normal);
    PackQuaternionS16Norm(quat, (u64*)out);
}

// https://github.com/Global-Illuminati/Precomputed-Light-Field-Probes/blob/master/src/shaders/octahedral.glsl
static inline v128f OctWrap(v128f v)
{
    v128f absSwap  = VecFabs(VecSwapPairs(v));
    v128f oneMinus = VecSub(VecSet1(1.0f), absSwap);
    v128f sign     = VecOr(VecAnd(v, VecSet1(-0.0f)), VecSet1(1.0f));
    return VecMul(oneMinus, sign);
}

static inline v128f OctEncode(v128f n)
{
    VecSetW(n, 0.0f);
    v128f absSum  = VecHSum(VecFabs(n));
    n             = VecDiv(n, absSum);
    v128f wrapped = OctWrap(n);
    v128i negZ    = VecCmpLt(VecSplatZ(n), VecZero());
    return VecBlend(n, wrapped, negZ);
}

static inline v128f OctDecode(v128f f)
{
    f32 x = VecGetX(f);
    f32 y = VecGetY(f);
    f32 z = 1.0f - Absf32(x) - Absf32(y);
    if (z < 0.0f)
    {
        f32 t = -z;
        if (x >= 0.0f) x -= t;
        else x += t;
        if (y >= 0.0f) y -= t;
        else y += t;
    }
    return Vec3NormVSafe(VecSetR(x, y, z, 0.0f));
}

// https://www.jeremyong.com/graphics/2023/01/09/tangent-spaces-and-diamond-encoding/
static inline float EncodeTangentDiamond(v128f normal, v128f tangent)
{
    v128f t1;
    if (Absf32(VecGetY(normal)) > Absf32(VecGetZ(normal)))
        t1 = VecSetR(VecGetY(normal), -VecGetX(normal), 0.0f, 0.0f);
    else
        t1 = VecSetR(VecGetZ(normal), 0.0f, -VecGetX(normal), 0.0f);
    t1 = Vec3NormV(t1);
    v128f  t2  = Vec3Cross(t1, normal);
    float  tx  = Vec3DotfV(tangent, t1);
    float  ty  = Vec3DotfV(tangent, t2);
    float  x   = tx / (Absf32(tx) + Absf32(ty));
    float  pys = Signf(ty);
    return -pys * 0.25f * x + 0.5f + pys * 0.25f;
}

purefn u32 VCALL PackNormalTangent(v128f normal, v128f tangent)
{
    v128f oct          = OctEncode(normal);
    float diamond      = EncodeTangentDiamond(normal, tangent);
    u32 packedOct      = PackXY11Z10Snorm(VecSetR(VecGetX(oct), VecGetY(oct), 0.0f, 0.0f)) & 0x3FFFFFu;
    u32 packedDiamond  = (u32)(Saturatef32(diamond) * 511.0f + 0.5f) & 0x1FFu;
    u32 handedness     = VecGetW(tangent) < 0.0f ? 1u : 0u;
    return packedOct | (packedDiamond << 22) | (handedness << 31);
}

purefn u32 VCALL PackNormalOCT(v128f normal)
{
    v128f oct = OctEncode(normal);
    return PackXY11Z10Snorm(VecSetR(VecGetX(oct), VecGetY(oct), 0.0f, 0.0f)) & 0x3FFFFFu;
}

// 9 bit per channel xyz, and 2 bit for max index, and 1 bit for max val sign, reconstruct w afterwards
static u32 VCALL PackQuat(v128f v)
{
    static const u32 shifts[4][4] = {
        { 32,  0,  9, 18 },
        {  0, 32,  9, 18 },
        {  0,  9, 32, 18 },
        {  0,  9, 18, 32 }
    };

    v128f cScale  = VecSet1((float)c9MaxValue / (2.0f * cOneOverSqrt2));
    v128f absV    = VecFabs(v);
    u32 maxElement = VecMaxElement(absV);

    v128u signBits = VeciAnd(VecBitcastU32(v), VeciSet1(0x80000000u));
    u32 maxSign    = (u32)((u32*)&signBits)[maxElement & 3];
    v128u flipMask = VeciSet1(maxSign);
    v = VecXor(v, VeciBitcastF32(flipMask));

    u32 value = maxSign | (maxElement << 29);
    v = VecFmadd(v, cScale, VecSet1((float)c9MaxValue * 0.5f + 0.5f));
    v = VecClamp(v, VecZero(), VecSet1((float)c9MaxValue));

    v128u i = VecF32ToU32(v);
    i = VeciSll(i, VeciLoad(shifts[maxElement]));
    i = VeciOr(i, VecSwapHalvesU(i));
    i = VeciOr(i, VecSwapPairsU(i));
    return value | VeciGetX(i);
}

static v128f VCALL UnpackQuat(u32 inValue)
{
    v128f cScale  = VecSet1(2.0f * cOneOverSqrt2 / (float)c9MaxValue);
    v128f cOffset = VecSet1(cOneOverSqrt2);

    u32 maxIndex = (inValue >> 29) & 3;
    u32 signBit  = inValue & 0x80000000u;

    v128u raw     = VeciSet1(inValue);
    v128u laneIdx = VeciSetR(0, 1, 2, 3);
    v128u maxLane = VeciSet1(maxIndex);
    v128u isMax   = VeciCmpEq(laneIdx, maxLane);

    v128u gt     = VeciCmpGt(laneIdx, maxLane);
    v128u order  = VeciSub(laneIdx, VeciAnd(gt, VeciSet1(1u)));
    v128u shAmts = VeciMul(order, VeciSet1(9u));
    shAmts       = VeciBlend(shAmts, VeciSet1(0u), isMax);

    v128u extracted = VeciSrl(raw, shAmts);
    extracted       = VeciAnd(extracted, VeciSet1(c9Mask));
    extracted       = VeciAndNot(isMax, extracted);

    v128f v = VecU32ToF32(extracted);
    v       = VecFmsub(v, cScale, cOffset);
    v       = VecAndNot(VeciBitcastF32(isMax), v);

    v128f sq4     = VecDot(v, v);
    v128f clamped = VecMax(VecSub(VecOne(), sq4), VecZero());
    v128f missing = VecSqrt(clamped);
    v128f blended = VecBlend(v, missing, VeciBitcastF32(isMax));
    return VecXor(blended, VecFromInt1((s32)signBit));
}

// 10 bit per channel xyz, and 2 bit for max index, reconstruct w afterwards
// sign of the quaternion might be inverted, q = -q, with 9 bit this is not the case
static inline u32 VCALL PackQuat10(v128f v)
{
    static const u32 shifts[4][4] = {
        { 32,  0, 10, 20 },
        {  0, 32, 10, 20 },
        {  0, 10, 32, 20 },
        {  0, 10, 20, 32 }
    };

    v128f cScale   = VecSet1((float)c10MaxValue / (2.0f * cOneOverSqrt2));
    u32 maxElement = VecMaxElement(VecFabs(v));

    v128u signBits = VeciAnd(VecBitcastU32(v), VeciSet1(0x80000000u));
    u32 maxSign    = VeciGetN(signBits, maxElement & 3);
    v128u flipMask = VeciSet1(maxSign);
    v = VecXor(v, VeciBitcastF32(flipMask));

    v = VecFmadd(v, cScale, VecSet1((float)c10MaxValue * 0.5f + 0.5f));
    v = VecClamp(v, VecZero(), VecSet1((float)c10MaxValue));

    v128u i = VecF32ToU32(v);
    i = VeciSll(i, VeciLoad(shifts[maxElement]));
    i = VeciOr(i, VecSwapHalvesU(i));
    i = VeciOr(i, VecSwapPairsU(i));
    return (maxElement << 30) | VeciGetX(i);
}

static inline v128f VCALL UnpackQuat10(u32 inValue)
{
    v128f cScale  = VecSet1(2.0f * cOneOverSqrt2 / (float)c10MaxValue);
    
    u32 maxIndex  = (inValue >> 30) & 3;
    v128u raw     = VeciSet1(inValue);
    v128u laneIdx = VeciSetR(0, 1, 2, 3);
    v128u maxLane = VeciSet1(maxIndex);
    v128u isMax   = VeciCmpEq(laneIdx, maxLane);

    v128u gt      = VeciCmpGt(laneIdx, maxLane);
    v128u order   = VeciSub(laneIdx, VeciAnd(gt, VeciSet1(1u)));
    v128u shAmts  = VeciMul(order, VeciSet1(10u));
    shAmts        = VeciBlend(shAmts, VeciSet1(0u), isMax);

    v128u extracted = VeciSrl(raw, shAmts);
    extracted       = VeciAnd(extracted, VeciSet1(c10Mask));
    extracted       = VeciAndNot(isMax, extracted);

    v128f v = VecU32ToF32(extracted);
    v       = VecFmsub(v, cScale, VecSet1(cOneOverSqrt2));
    v       = VecAndNot(VeciBitcastF32(isMax), v);

    v128f sq4     = VecDot(v, v);
    v128f clamped = VecMax(VecSub(VecOne(), sq4), VecZero());
    v128f missing = VecSqrt(clamped);
    return VecBlend(v, missing, VeciBitcastF32(isMax));
}

static inline u32 PackTBNIntoQuaternion32(v128f normal, v128f tangent)
{
    v128f binormal = VecNeg(Vec3Cross(tangent, normal));
    v128f quat = QuaternionFromM33Vec(binormal, tangent, normal);
    return PackQuat(quat);
}

#endif
