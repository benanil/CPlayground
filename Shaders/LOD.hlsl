#ifndef LOD_HLSL
#define LOD_HLSL

#include "../Include/RenderLimits.h"
#include "CommonStructs.hlsl"
#include "Bitpack.hlsl"
#include "Math.hlsl"

#define LOD0_PIXEL_DIAMETER 128.0f
#define LOD1_PIXEL_DIAMETER 40.0f
#define LOD_PIXEL_QUANTUM   8.0f
#define LOD_CLIP_MIN_W      0.0001f

struct ProjectedAABB
{
    float2 ndcMin;
    float2 ndcMax;
    float2 uvMin;
    float2 uvMax;
    float  nearestDepth;
    float  screenDiameterNDC;
    float  screenDiameterPixels;
    uint   anyFront;
    uint   anyBehind;
};

uint SelectLODFromScreenDiameter(float screenDiameter, uint lodCount)
{
    screenDiameter = floor(screenDiameter / LOD_PIXEL_QUANTUM) * LOD_PIXEL_QUANTUM;

    if (screenDiameter > LOD0_PIXEL_DIAMETER)
        return 0u;
    if (screenDiameter > LOD1_PIXEL_DIAMETER)
        return min(1u, lodCount - 1u);
    return min(2u, lodCount - 1u);
}

void BuildWorldAABB(Entity entity, PrimitiveGroup group, out float3 worldCenter, out float3 worldExtent, out float3 worldMin, out float3 worldMax)
{
    float3 localMin = PrimitiveGroup_AABBMin(group);
    float3 localMax = PrimitiveGroup_AABBMax(group);
    float3 localCenter = (localMin + localMax) * 0.5f;
    float3 localExtent = (localMax - localMin) * 0.5f;

    float4 q = VecNorm(UnpackRGBA16Snorm(entity.rotation.x, entity.rotation.y));
    float3 s = float3(UnpackRGBA16Unorm(entity.scale).xyz) * 10.0f;
    float3x3 rotM = M33FromQuaternionF32(q);

    worldCenter = entity.position.xyz + QMulVec3F32(q, localCenter * s);
    worldExtent = abs(rotM[0] * s.x) * localExtent.x +
                  abs(rotM[1] * s.y) * localExtent.y +
                  abs(rotM[2] * s.z) * localExtent.z;
    worldMin = worldCenter - worldExtent;
    worldMax = worldCenter + worldExtent;
}

void ProjectCorner(float3 p, float4x4 viewProjection, inout ProjectedAABB proj)
{
    float4 clip = mul(viewProjection, float4(p, 1.0f));
    if (clip.w <= LOD_CLIP_MIN_W)
    {
        proj.anyBehind = 1u;
        return;
    }

    float3 ndc = clip.xyz / clip.w;
    proj.ndcMin = min(proj.ndcMin, ndc.xy);
    proj.ndcMax = max(proj.ndcMax, ndc.xy);
    // reversed-Z, D3D 0..1 clip-z: the camera-nearest point has the largest depth value
    proj.nearestDepth = max(proj.nearestDepth, saturate(ndc.z));
    proj.anyFront = 1u;
}

void ProjectAABB(out ProjectedAABB proj, float3 mn, float3 mx, float4x4 viewProjection, uint2 outputSize)
{
    proj.ndcMin = float2( 1e30f,  1e30f);
    proj.ndcMax = float2(-1e30f, -1e30f);
    proj.uvMin = 0.0f;
    proj.uvMax = 0.0f;
    proj.nearestDepth = 0.0f; // reversed-Z: accumulate the max (nearest) depth
    proj.screenDiameterNDC = 0.0f;
    proj.screenDiameterPixels = 0.0f;
    proj.anyFront = 0u;
    proj.anyBehind = 0u;

    ProjectCorner(float3(mn.x, mn.y, mn.z), viewProjection, proj);
    ProjectCorner(float3(mx.x, mn.y, mn.z), viewProjection, proj);
    ProjectCorner(float3(mn.x, mx.y, mn.z), viewProjection, proj);
    ProjectCorner(float3(mx.x, mx.y, mn.z), viewProjection, proj);
    ProjectCorner(float3(mn.x, mn.y, mx.z), viewProjection, proj);
    ProjectCorner(float3(mx.x, mn.y, mx.z), viewProjection, proj);
    ProjectCorner(float3(mn.x, mx.y, mx.z), viewProjection, proj);
    ProjectCorner(float3(mx.x, mx.y, mx.z), viewProjection, proj);

    if (proj.anyFront == 0u)
        return;

    float2 uvA = proj.ndcMin * float2(0.5f, -0.5f) + 0.5f;
    float2 uvB = proj.ndcMax * float2(0.5f, -0.5f) + 0.5f;
    proj.uvMin = saturate(min(uvA, uvB));
    proj.uvMax = saturate(max(uvA, uvB));

    float2 rectPixels = (proj.uvMax - proj.uvMin) * max(float2(outputSize), float2(1.0f, 1.0f));
    proj.screenDiameterNDC = max(proj.ndcMax.x - proj.ndcMin.x,
                                 proj.ndcMax.y - proj.ndcMin.y);
    proj.screenDiameterPixels = max(rectPixels.x, rectPixels.y);
}

bool ProjectedRectOutside(in ProjectedAABB proj)
{
    return proj.ndcMax.x < -1.0f ||
           proj.ndcMin.x >  1.0f ||
           proj.ndcMax.y < -1.0f ||
           proj.ndcMin.y >  1.0f;
}

uint SelectProjectedAABBLOD(in ProjectedAABB proj, float lodDistanceModifier, uint lodCount)
{
    if (proj.anyFront == 0u)
        return min(2u, MESH_LOD_COUNT - 1u);

    return SelectLODFromScreenDiameter(proj.screenDiameterPixels * lodDistanceModifier, lodCount);
}

uint SelectEntityLOD(Entity entity, PrimitiveGroup group, float4x4 viewProjection, uint2 viewportSize, float lodDistanceModifier)
{
    float3 worldCenter;
    float3 worldExtent;
    float3 worldMin;
    float3 worldMax;
    BuildWorldAABB(entity, group, worldCenter, worldExtent, worldMin, worldMax);

    ProjectedAABB proj;
    ProjectAABB(proj, worldMin, worldMax, viewProjection, viewportSize);
    return SelectProjectedAABBLOD(proj, lodDistanceModifier, MESH_LOD_COUNT);
}

#endif // LOD_HLSL
