#ifndef ANIMATED_TRANSFORM_HLSL
#define ANIMATED_TRANSFORM_HLSL

#include "Math.hlsl"
#include "Bitpack.hlsl"

// The animated vertex cache stores ONLY a bounds-normalized skinned position,
// packed as xy11z10 unorm in one uint.
//
// Neither entity rotation nor scale is baked in: the compute pass writes the skinned position in
// *model space* (exactly like a static mesh quantizes against its AABB), and every vertex shader
// applies the entity rotation + scale on read. The tangent frame is not cached; the GBuffer pass
// re-skins it from the source vertex + bones.

float3 AnimatedBoundsCenter(float3 aabbMin, float3 aabbMax) { return (aabbMin + aabbMax) * 0.5f; }

// guard against a zero-extent axis (planar mesh) so the normalize divide can't produce NaN
float3 AnimatedBoundsExtent(float3 aabbMin, float3 aabbMax) { return max((aabbMax - aabbMin) * 0.5f, 1.0e-4f); }

// Compute pass: centerRel = skinnedModelPos - boundsCenter. extent is derived from the group AABB,
// not stored per vertex. Maps [-extent, extent] -> [0, 1] and packs to 11/11/10 unorm.
uint PackAnimatedCenterRel(float3 centerRel, float3 extent)
{
    float3 norm01 = saturate(centerRel / (2.0f * extent) + 0.5f);
    uint x = uint(round(norm01.x * 2047.0f)) & 0x7FFu;
    uint y = uint(round(norm01.y * 2047.0f)) & 0x7FFu;
    uint z = uint(round(norm01.z * 1023.0f)) & 0x3FFu;
    return x | (y << 11u) | (z << 22u);
}

// Vertex shaders: rebuild the model-space skinned position from the packed unorm (fp32).
float3 UnpackAnimatedModelPos(uint packed, float3 aabbMin, float3 aabbMax)
{
    float3 norm01 = float3(
        float( packed        & 0x7FFu) * (1.0f / 2047.0f),
        float((packed >> 11u) & 0x7FFu) * (1.0f / 2047.0f),
        float( packed >> 22u          ) * (1.0f / 1023.0f));
    return aabbMin + norm01 * (aabbMax - aabbMin);
}

// Apply entity rotation + scale to a model-space position (matches the static Surface path, fp32).
float3 AnimatedWorldPos(float3 modelPos, float4 insRot, float3 insScale, float3 entityPos)
{
    return entityPos + QMulVec3F32(insRot, modelPos * insScale);
}

#endif // ANIMATED_TRANSFORM_HLSL