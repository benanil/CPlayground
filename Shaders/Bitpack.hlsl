#ifndef BITPACK_HLSL
#define BITPACK_HLSL

#include "../Include/RenderLimits.h"
#include "Common.hlsl"

f16_3 UnpackVec3XY11Z10Snorm(uint packed) {
    const f16 scale11 = f16(1.0 / 1023.0);
    const f16 scale10 = f16(1.0 / 511.0);
    return f16_3(
        f16((int)(packed << 21) >> 21) * scale11,
        f16((int)(packed << 10) >> 21) * scale11,
        f16((int)(packed      ) >> 22) * scale10
    );
}

f16 UnpackHalf(uint packed)
{
    #if INT16_SUPPORTED
    return asfloat16(uint16_t(packed & 0xFFFFu));
    #else
    return f16(f16tof32(packed & 0xFFFFu));
    #endif
}

uint PackHalf(f16 v)
{
    #if INT16_SUPPORTED
    return uint(asuint16(v));
    #else
    return f32tof16(float(v));
    #endif
}

f16_2 UnpackHalf2(uint packed) {
    #if INT16_SUPPORTED
    return asfloat16(uint16_t2(u16(packed), u16(packed >> 16)));
    #else
    return f16_2(f16tof32(packed), f16tof32(packed >> 16));
    #endif
}

uint PackHalf2(f16_2 v)
{
    #if INT16_SUPPORTED
    uint16_t2 h = asuint16(v);
    return (uint(h.y) << 16) | uint(h.x);
    #else
    uint lo = f32tof16(v.x);
    uint hi = f32tof16(v.y);
    return (hi << 16) | lo;
    #endif
}

f16_4 UnpackRGBA16Snorm(uint xy, uint zw) {
    return f16_4(
        f16(int(xy << 16u) >> 16),
        f16(int(xy)        >> 16),
        f16(int(zw << 16u) >> 16),
        f16(int(zw)        >> 16)
    ) * f16(1.0 / 32767.0);
}

f16_4 UnpackRGBA16Unorm(uint2 p) {
    uint4 u = (p.xxyy >> uint4(0u, 16u, 0u, 16u)) & 0xFFFFu;
    return f16_4(float4(u) * (1.0 / 65534.0));
}

// full precision unorm16x4, matches CPU PackUnorm16x4 (Math/Bitpack.h, normalized by 1/65534).
// used to de-quantize static AVertex.position against the primitive AABB.
float4 UnpackUnorm16x4(uint2 p) {
    uint4 u = (p.xxyy >> uint4(0u, 16u, 0u, 16u)) & 0xFFFFu;
    return float4(u) * (1.0 / 65534.0);
}

// inverse of UnpackUnorm16x4. fp32 input so meter-scale values keep full 16-bit precision.
uint2 PackUnorm16x4(float4 v) {
    uint4 u = uint4(round(saturate(v) * 65534.0)) & 0xFFFFu;
    return uint2(u.x | (u.y << 16), u.z | (u.w << 16));
}

f16_3 UnpackVec3XY11Z10Unorm(uint packed) {
    const f16 scale11 = f16(1.0 / 2047.0);
    const f16 scale10 = f16(1.0 / 1023.0);
    return f16_3(
        f16( packed        & 0x7FFu) * scale11,
        f16((packed >> 11) & 0x7FFu) * scale11,
        f16( packed >> 22          ) * scale10
    );
}

float4 UnpackColor4Uint(uint color)
{
    return float4(
        float((color >> 0u)  & 0xFFu),
        float((color >> 8u)  & 0xFFu),
        float((color >> 16u) & 0xFFu),
        float((color >> 24u) & 0xFFu)) * (1.0f / 255.0f);
}

f16_4 UnpackColorRGBA5551(uint color)
{
	f16 w = f16(color >> 15) * f16(0.5) + f16(0.5);
    return f16_4(
        f16((color >> 0u)  & 0x1Fu),
        f16((color >> 5u)  & 0x1Fu),
        f16((color >> 10u) & 0x1Fu),
        w * f16(31.0)) * f16(1.0f / 31.0f);
}

f16_4 UnpackAVertexColor(uint2 packedPosition)
{
    return UnpackColorRGBA5551(packedPosition.y >> 16u);
}

f16_4 UnpackColor4UintF16(uint color)
{
    return f16_4(
        f16((color >> 0u)  & 0xFFu),
        f16((color >> 8u)  & 0xFFu),
        f16((color >> 16u) & 0xFFu),
        f16((color >> 24u) & 0xFFu)) * f16(1.0f / 255.0f);
}

f16_3 UnpackColor3Uint(uint color)
{
    const f16 tof1 = f16(1.0 / 255.0);
    return f16_3(
        f16((color >> 0)  & 0xFF),
        f16((color >> 8)  & 0xFF),
        f16((color >> 16) & 0xFF)
    ) * tof1;
}

f16_3 OctDecode(f16_2 f)
{
    // https://twitter.com/Stubbesaurus/status/937994790553227264
    f16_3 n = f16_3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    f16  t  = saturate(-n.z);
    n.xy   += select(n.xy >= 0.0, -t, t);
    return normalize(n);
}

f16_2 DecodeDiamond(f16 p)
{
    f16   p_sign = f16(sign(p - f16(0.5)));
    f16_2 v;
    v.x = mad(p_sign * f16(-4.0), p, f16(1.0) + p_sign * f16(2.0));
    v.y = p_sign * (f16(1.0) - abs(v.x));
    return normalize(v);
}

// https://www.jeremyong.com/graphics/2023/01/09/tangent-spaces-and-diamond-encoding/
f16_3 DecodeTangent(f16_3 normal, f16 diamond_tangent)
{
    f16_3 t1_a = normalize(f16_3( normal.y, -normal.x, f16(0.0)));
    f16_3 t1_b = normalize(f16_3( normal.z,  f16(0.0), -normal.x));
    f16_3 t1   = select(abs(normal.y) > abs(normal.z), t1_a, t1_b);

    f16_2 packed_tangent = DecodeDiamond(diamond_tangent);
    return packed_tangent.x * t1 + packed_tangent.y * cross(t1, normal);
}

void UnpackNormalTangent(uint packed, out f16_3 normal, out f16_3 tangent)
{
    f16_2 oct = f16_2(
        f16((int)(packed << 21) >> 21) * f16(1.0 / 1023.0),
        f16((int)(packed << 10) >> 21) * f16(1.0 / 1023.0));
    f16 diamond = f16((packed >> 22) & 0x1FFu) * f16(1.0 / 511.0);
    normal      = OctDecode(oct);
    tangent     = DecodeTangent(normal, diamond);
}

f16_3 UnpackNormal(uint packed)
{
    f16_2 oct = f16_2(
        f16((int)(packed << 21) >> 21) * f16(1.0 / 1023.0),
        f16((int)(packed << 10) >> 21) * f16(1.0 / 1023.0));
    return OctDecode(oct);
}

f16 UnpackTangentHandedness(uint packed)
{
    return (packed & 0x80000000u) != 0u ? f16(-1.0) : f16(1.0);
}

f16_2 SignNotZero(f16_2 v)
{
    return select(v < f16_2(0.0, 0.0),
                  f16_2(-1.0, -1.0),
                  f16_2( 1.0,  1.0));
}
f16 SignNotZero(f16 v)
{
    return (v < f16(0.0)) ? f16(-1.0) : f16(1.0);
}

f16_2 OctWrap(f16_2 v)
{
    return (f16_2(1.0, 1.0) - abs(v.yx)) * SignNotZero(v);
}

f16_2 OctEncode(f16_3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    f16_2 wrapped = OctWrap(n.xy);
    return (n.z < 0.0) ? wrapped : n.xy;
}

// https://www.jeremyong.com/graphics/2023/01/09/tangent-spaces-and-diamond-encoding/
f16 EncodeTangentDiamond(f16_3 normal, f16_3 tangent)
{
    f16_3 t1;
    if (abs(normal.y) > abs(normal.z))
        t1 = f16_3(normal.y, -normal.x, f16(0.0));
    else
        t1 = f16_3(normal.z, f16(0.0), -normal.x);

    t1 = normalize(t1);
    f16_3 t2 = cross(t1, normal);
    f16 tx = dot(tangent, t1);
    f16 ty = dot(tangent, t2);
    f16 denom = abs(tx) + abs(ty);
    f16 x = (denom > f16(0.0)) ? (tx / denom) : f16(0.0);
    f16 pys = SignNotZero(ty);
    return -pys * f16(0.25) * x + f16(0.5) + pys * f16(0.25);
}

int PackSnormBits(f16 v, float scale)
{
    v = clamp(v, -1.0, 1.0);
    // round away from zero, matching typical CPU pack behavior better than truncation
    return (v >= 0.0) ? int(v * scale + 0.5) : int(v * scale - 0.5);
}

uint PackXY11Z10SnormToU32(f16_3 v)
{
    uint x = uint(PackSnormBits(v.x, 1023.0)) & 0x7FFu;
    uint y = uint(PackSnormBits(v.y, 1023.0)) & 0x7FFu;
    uint z = uint(PackSnormBits(v.z,  511.0)) & 0x3FFu;
    return x | (y << 11) | (z << 22);
}

// inverse of UnpackVec3XY11Z10Unorm (11/11/10 unorm)
uint PackXY11Z10UnormToU32(f16_3 v)
{
    v = saturate(v);
    uint x = uint(round(float(v.x) * 2047.0)) & 0x7FFu;
    uint y = uint(round(float(v.y) * 2047.0)) & 0x7FFu;
    uint z = uint(round(float(v.z) * 1023.0)) & 0x3FFu;
    return x | (y << 11) | (z << 22);
}

uint PackNormalTangent(f16_3 normal, f16_4 tangent)
{
    normal = normalize(normal);
    f16_2 oct = OctEncode(normal);
    f16 diamond  = EncodeTangentDiamond(normal, tangent.xyz);
    uint packedOct = PackXY11Z10SnormToU32(f16_3(oct.x, oct.y, 0.0)) & 0x3FFFFFu;
    uint packedDiamond = uint(saturate(diamond) * 511.0 + 0.5) & 0x1FFu;
    uint handedness = (tangent.w < 0.0) ? 1u : 0u;
    return packedOct | (packedDiamond << 22) | (handedness << 31);
}

uint PackTangentSpace25(f16_3 normal, f16_3 tangent, f16 handedness)
{
    normal = normalize(normal);
    f16_2 oct = OctEncode(normal);
    uint2 n = uint2(saturate(oct * f16(0.5) + f16(0.5)) * 511.0 + 0.5) & 0x1FFu;
    uint td = uint(saturate(EncodeTangentDiamond(normal, normalize(tangent))) * 63.0 + 0.5) & 0x3Fu;
    uint h = handedness < f16(0.0) ? 1u : 0u;
    return n.x | (n.y << 9) | (td << 18) | (h << 24);
}

void UnpackTangentSpace25(uint packed, out f16_3 normal, out f16_3 tangent, out f16 handedness)
{
    f16_2 oct = f16_2(f16(packed & 0x1FFu), f16((packed >> 9) & 0x1FFu)) * f16(1.0 / 511.0) * f16(2.0) - f16(1.0);
    normal = OctDecode(oct);
    tangent = DecodeTangent(normal, f16((packed >> 18) & 0x3Fu) * f16(1.0 / 63.0));
    handedness = (packed & 0x1000000u) != 0u ? f16(-1.0) : f16(1.0);
}

// bone matrices are stored fp16 (12 halves / 6 u32); see WriteBone in AnimationCompute.hlsl.
f16_3x4 LoadBone(StructuredBuffer<uint> boneMtx, uint idx)
{
    uint base = idx * MatrixNumInt32;
    f16_3x4 bone;
    bone[0] = f16_4(UnpackHalf2(boneMtx[base + 0]), UnpackHalf2(boneMtx[base + 1]));
    bone[1] = f16_4(UnpackHalf2(boneMtx[base + 2]), UnpackHalf2(boneMtx[base + 3]));
    bone[2] = f16_4(UnpackHalf2(boneMtx[base + 4]), UnpackHalf2(boneMtx[base + 5]));
    return bone;
}

#endif
