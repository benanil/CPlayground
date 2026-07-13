#include "../../Include/RenderLimits.h"
#include "../Bitpack.hlsl"
#include "../Math.hlsl"
#include "../CommonStructs.hlsl"
#include "../LOD.hlsl"

#define DEBUG_CULLED_AABBS 0
#define SMALL_OBJECT_CULL_ENABLED   1

#define SMALL_CULL_PIXEL_DIAMETER   1.0f
#define SMALL_CULL_DEPTH_THRESHOLD  0.02f // reversed-Z: far ~= 0

#define HIZ_RECT_PADDING_PIXELS     1.0f

#define CULL_DRAW_FLAG_ENABLE_HIZ          (1u << 0u)
#define CULL_DRAW_FLAG_VISIBILITY_OUTPUT   (1u << 1u)
#define CULL_DRAW_FLAG_RESET_VISIBILITY    (1u << 2u)
#define CULL_DRAW_FLAG_ENABLE_LOD          (1u << 3u)
#define CULL_DRAW_FLAG_CULL_SPHERE         (1u << 4u)
#define CULL_DRAW_FLAG_NO_FRUSTUM          (1u << 5u)

Texture2D<float>                 hiZTexture            : register(t0);
StructuredBuffer<Entity>         entities              : register(t1);
StructuredBuffer<PrimitiveGroup> primitiveGroups       : register(t2);
StructuredBuffer<PrimitiveGroupLOD> primitiveGroupLODs : register(t3);

RWStructuredBuffer<uint>                drawSparseIndices    : register(u0, space1);
RWStructuredBuffer<IndexedDrawCommand>  drawArgs             : register(u1, space1);
RWStructuredBuffer<LineVertex>          lineVertices         : register(u2, space1);
RWStructuredBuffer<IndirectDrawCommand> lineDrawCommand      : register(u3, space1);
RWStructuredBuffer<uint>                visibleSparseIndices : register(u4, space1);
RWStructuredBuffer<uint>                visibilityMask       : register(u5, space1);
RWStructuredBuffer<uint>                visibleCount         : register(u6, space1);
RWStructuredBuffer<IndirectDispatchCommand> dispatchArgs     : register(u7, space1);

cbuffer params : register(b0, space2)
{
    float4 frustumPlanes[6];
    uint   maxEntityID;
    uint   numPrimitiveGroups;
    uint   mode;
    uint   flags;
    float4x4 viewProjection;
    uint2  hiZSize;
    uint   hiZMipCount;
    float  hiZDepthBias;
    uint   lodCount;
    uint   sparseIndexLODStride;
    uint   forcedLOD;
    float  lodDistanceModifier;
    uint   instanceMultiplier;
    uint3  padding;
    float4 cullSphere;
};

uint WangHash(uint x)
{
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

void AddLine(float3 a, float3 b, uint idx, uint color)
{
    lineVertices[idx].x = a.x;
    lineVertices[idx].y = a.y;
    lineVertices[idx].z = a.z;
    lineVertices[idx].color = color;

    lineVertices[idx + 1].x = b.x;
    lineVertices[idx + 1].y = b.y;
    lineVertices[idx + 1].z = b.z;
    lineVertices[idx + 1].color = color;
}

void AddAABBLineColored(float3 worldMin, float3 worldMax, uint color)
{
    uint start;
    InterlockedAdd(lineDrawCommand[1].numVertices, 24, start);

    if (start + 24 > MAX_LINE_COUNT)
    {
        lineDrawCommand[1].numVertices = MAX_LINE_COUNT;
        return;
    }

    float3 p000 = float3(worldMin.x, worldMin.y, worldMin.z);
    float3 p100 = float3(worldMax.x, worldMin.y, worldMin.z);
    float3 p010 = float3(worldMin.x, worldMax.y, worldMin.z);
    float3 p110 = float3(worldMax.x, worldMax.y, worldMin.z);

    float3 p001 = float3(worldMin.x, worldMin.y, worldMax.z);
    float3 p101 = float3(worldMax.x, worldMin.y, worldMax.z);
    float3 p011 = float3(worldMin.x, worldMax.y, worldMax.z);
    float3 p111 = float3(worldMax.x, worldMax.y, worldMax.z);

    AddLine(p000, p100, start + 0,  color);
    AddLine(p100, p110, start + 2,  color);
    AddLine(p110, p010, start + 4,  color);
    AddLine(p010, p000, start + 6,  color);

    AddLine(p001, p101, start + 8,  color);
    AddLine(p101, p111, start + 10, color);
    AddLine(p111, p011, start + 12, color);
    AddLine(p011, p001, start + 14, color);

    AddLine(p000, p001, start + 16, color);
    AddLine(p100, p101, start + 18, color);
    AddLine(p110, p111, start + 20, color);
    AddLine(p010, p011, start + 22, color);
}

void AddAABBLine(float3 worldMin, float3 worldMax)
{
    uint3 d = uint3(worldMin * 100.0f);
    AddAABBLineColored(worldMin, worldMax, WangHash(d.x + d.y + d.z));
}

bool AABBVisible(float3 center, float3 extent)
{
    if ((flags & CULL_DRAW_FLAG_NO_FRUSTUM) != 0u)
        return true;

    if ((flags & CULL_DRAW_FLAG_CULL_SPHERE) != 0u)
    {
        float3 closest = clamp(cullSphere.xyz, center - extent, center + extent);
        float3 delta = cullSphere.xyz - closest;
        return dot(delta, delta) <= cullSphere.w * cullSphere.w;
    }

    [unroll]
    for (uint i = 0; i < 6; i++)
    {
        float4 plane = frustumPlanes[i];
        float d = dot(plane.xyz, center) + plane.w;
        float r = dot(abs(plane.xyz), extent);

        if (d + r < -0.001f)
            return false;
    }

    return true;
}

bool AABBTooSmallAndFar(in ProjectedAABB proj)
{
    #if SMALL_OBJECT_CULL_ENABLED
    if ((flags & CULL_DRAW_FLAG_ENABLE_HIZ) == 0u)
        return false;

    if (proj.anyFront == 0u || proj.anyBehind != 0u)
        return false;

    return proj.screenDiameterPixels < SMALL_CULL_PIXEL_DIAMETER &&
           proj.nearestDepth <= SMALL_CULL_DEPTH_THRESHOLD;
    #else
    return false;
    #endif
}

bool AABBOccludedHiZ(in ProjectedAABB proj)
{
    if ((flags & CULL_DRAW_FLAG_ENABLE_HIZ) == 0u || hiZMipCount == 0u || hiZSize.x == 0u || hiZSize.y == 0u)
        return false;

    if (proj.anyFront == 0u || proj.anyBehind != 0u)
        return false;

    if (ProjectedRectOutside(proj))
        return false;

    if (proj.nearestDepth <= 0.0f) // reversed-Z: at/behind the far plane, nothing to occlude
        return false;

    float2 rectMin = proj.uvMin;
    float2 rectMax = proj.uvMax;

    float2 rectPadding = HIZ_RECT_PADDING_PIXELS / float2(hiZSize);
    rectMin = saturate(rectMin - rectPadding);
    rectMax = saturate(rectMax + rectPadding);

    float2 extentPixels = max((rectMax - rectMin) * float2(hiZSize), 1.0f);

    float mip = clamp(
        ceil(log2(max(extentPixels.x, extentPixels.y))),
        0.0f,
        float(hiZMipCount - 1u));

    float lowerMip = max(mip - 1.0f, 0.0f);

    float2 lowerMipSize = max(floor(float2(hiZSize) / exp2(lowerMip)), 1.0f);
    float2 lowerDims = ceil(rectMax * lowerMipSize) - floor(rectMin * lowerMipSize);

    if (lowerDims.x <= 2.0f && lowerDims.y <= 2.0f)
        mip = lowerMip;

    int selectedMip = int(mip);

    uint2 mipSize = max(hiZSize >> uint(selectedMip), uint2(1u, 1u));
    float2 mipSizeF = float2(mipSize);

    uint2 p0 = uint2(max(floor(rectMin * mipSizeF - 0.001f), 0.0f));
    uint2 p1 = min(uint2(floor(rectMax * mipSizeF + 0.001f)), mipSize - 1u);

    uint2 sampleDims = p1 - p0 + 1u;

    // Avoid accidentally turning one huge object into a slow 64+ sample path.
    if (sampleDims.x > 8u || sampleDims.y > 8u)
        return false;

    // reversed-Z: the Hi-Z stores the farthest occluder as the minimum depth value
    float minOccluderDepth = 1.0f;

    for (uint sy = p0.y; sy <= p1.y; sy++)
    {
        for (uint sx = p0.x; sx <= p1.x; sx++)
        {
            minOccluderDepth = min(minOccluderDepth, hiZTexture.Load(int3(sx, sy, selectedMip)));
        }
    }

    // occluded when the object's nearest point is still behind (smaller depth than) the
    // farthest occluder; the bias keeps it conservative (under-cull) like the original test
    return proj.nearestDepth + hiZDepthBias < minOccluderDepth;
}

uint SelectLOD(in ProjectedAABB proj, float indexCountModifier)
{
    if (lodCount <= 1u)
        return 0u;

    if (forcedLOD < lodCount)
    {
        // forced LOD (shadows): bias by triangle density. Dense meshes drop detail
        // (positive offset), cheap low-poly meshes keep more detail (negative offset, toward LOD0).
        int lodOffset = int(floor(indexCountModifier * 3.0)) - 1; // [0..1] -> [-1..2]
        return (uint)clamp(int(forcedLOD) + lodOffset, 0, int(lodCount) - 1);
    }

    if ((flags & CULL_DRAW_FLAG_ENABLE_LOD) == 0u)
        return 0u;

    if (proj.anyFront == 0u)
        return 0u;

    // >1 keeps high detail longer for cheap (low-poly) meshes, <1 swaps sooner for dense ones.
    // Hits exactly lodDistanceModifier at mid density and 0.25x at max density.
    float polyScale = 1.0 + (0.5 - indexCountModifier) * 1.5; // [0..1] -> [1.75..0.25]
    float lodModifier = lodDistanceModifier * polyScale;
    return SelectLODFromScreenDiameter(proj.screenDiameterPixels * lodModifier, lodCount);
}

void Initialize(uint idx)
{
    uint numDraws = numPrimitiveGroups * lodCount;

    if (idx < numDraws)
    {
        uint primitiveIdx = idx / lodCount;
        uint lod = idx - primitiveIdx * lodCount;

        PrimitiveGroup group = primitiveGroups[primitiveIdx];
        PrimitiveGroupLOD lodGroup = primitiveGroupLODs[primitiveIdx];
        drawArgs[idx].numIndices    = lodGroup.lodNumIndices[lod];
        drawArgs[idx].numInstances  = 0;
        drawArgs[idx].firstIndex    = lodGroup.lodIndexOffset[lod];
        drawArgs[idx].vertexOffset  = 0;
        drawArgs[idx].firstInstance = 0;
    }

    if (idx == 0)
    {
        lineDrawCommand[0].numVertices   = 0;
        lineDrawCommand[0].numInstances  = 1;
        lineDrawCommand[0].firstVertex   = 1;
        lineDrawCommand[0].firstInstance = 0;

        lineDrawCommand[1].numVertices   = 0;
        lineDrawCommand[1].numInstances  = 1;
        lineDrawCommand[1].firstVertex   = 0;
        lineDrawCommand[1].firstInstance = 0;

        if ((flags & (CULL_DRAW_FLAG_VISIBILITY_OUTPUT | CULL_DRAW_FLAG_RESET_VISIBILITY)) == (CULL_DRAW_FLAG_VISIBILITY_OUTPUT | CULL_DRAW_FLAG_RESET_VISIBILITY))
        {
            visibleCount[0] = 0;

            dispatchArgs[0].groupCountX = 0;
            dispatchArgs[0].groupCountY = 1;
            dispatchArgs[0].groupCountZ = 1;

            dispatchArgs[1].groupCountX = numPrimitiveGroups * lodCount;
            dispatchArgs[1].groupCountY = 0;
            dispatchArgs[1].groupCountZ = 0;
        }
    }

    if ((flags & (CULL_DRAW_FLAG_VISIBILITY_OUTPUT | CULL_DRAW_FLAG_RESET_VISIBILITY)) == (CULL_DRAW_FLAG_VISIBILITY_OUTPUT | CULL_DRAW_FLAG_RESET_VISIBILITY) && idx < maxEntityID)
    {
        visibilityMask[idx] = 0;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint idx = tid.x;

    if (mode == 0u)
    {
        Initialize(idx);
        return;
    }

    if (idx >= maxEntityID)
        return;

    uint sparse = idx;
    Entity entity = entities[sparse];
    if (((entity.parentIdx >> 24u) & ENTITY_FLAG_NOMESH) != 0u || entity.primitiveIdx == 0xffffffffu)
        return;

    uint primitiveIdx = entity.primitiveIdx;
    PrimitiveGroup group = primitiveGroups[primitiveIdx];
    PrimitiveGroupLOD lodGroup = primitiveGroupLODs[primitiveIdx];

    float3 worldCenter;
    float3 worldExtent;
    float3 worldMin;
    float3 worldMax;
    BuildWorldAABB(entity, group, worldCenter, worldExtent, worldMin, worldMax);

    bool frustumVisible = AABBVisible(worldCenter, worldExtent);

    bool needProjection = frustumVisible && ((flags & CULL_DRAW_FLAG_ENABLE_HIZ) != 0u ||
                                             (flags & CULL_DRAW_FLAG_ENABLE_LOD) != 0u ||
                                             SMALL_OBJECT_CULL_ENABLED != 0);
    ProjectedAABB proj;
    bool tooSmallFar = false;
    bool hiZOccluded = false;

    if (needProjection)
    {
        ProjectAABB(proj, worldMin, worldMax, viewProjection, hiZSize);
        tooSmallFar = AABBTooSmallAndFar(proj);

        if (!tooSmallFar)
            hiZOccluded = AABBOccludedHiZ(proj);
    }

    bool visible = frustumVisible && !tooSmallFar && !hiZOccluded;

#if DEBUG_CULLED_AABBS
    if (!frustumVisible) AddAABBLineColored(worldMin, worldMax, 0xFF0000FFu);
    else if (tooSmallFar) AddAABBLineColored(worldMin, worldMax, 0x00FFFFFFu);
    else if (hiZOccluded) AddAABBLineColored(worldMin, worldMax, 0xFFFF00FFu);
    else return;
#else
    if (!visible)
        return;
#endif

    uint lod = 0u;

    if (((flags & CULL_DRAW_FLAG_ENABLE_LOD) != 0u || forcedLOD < lodCount) && lodCount > 1u)
    {
        if (forcedLOD >= lodCount && !needProjection)
            ProjectAABB(proj, worldMin, worldMax, viewProjection, hiZSize);

		// objects that has more index will swap lod earlier
		float indexCountModifier = saturate(float(lodGroup.lodNumIndices[0]) / 100000.0);
		lod = SelectLOD(proj, indexCountModifier);
    }

    uint drawIdx = primitiveIdx * lodCount + lod;

    uint visibleInstanceCount = max(instanceMultiplier, 1u);
    uint localVisibleInstance;
    InterlockedAdd(drawArgs[drawIdx].numInstances, visibleInstanceCount, localVisibleInstance);
    uint localVisibleIdx = localVisibleInstance / visibleInstanceCount;

    uint globalVisibleIdx;
    InterlockedAdd(lineDrawCommand[0].firstInstance, 1, globalVisibleIdx);

    drawSparseIndices[lod * sparseIndexLODStride + PrimitiveGroup_EntityOffset(group) + localVisibleIdx] = sparse;

    if ((flags & CULL_DRAW_FLAG_VISIBILITY_OUTPUT) != 0u)
    {
        uint visibleSparse = entities[sparse].sparse;

        uint old;
        InterlockedCompareExchange(visibilityMask[visibleSparse], 0, 1, old);

        InterlockedMax(dispatchArgs[1].groupCountZ, (lodGroup.lodNumVertices[lod] + 31u) / 32u);

        if (old == 0u)
        {
            uint visibleSlot;
            InterlockedAdd(visibleCount[0], 1, visibleSlot);

            visibleSparseIndices[visibleSlot] = visibleSparse;
            InterlockedMax(dispatchArgs[1].groupCountY, (visibleSlot + 32u) / 32u);

            if ((visibleSlot & 31u) == 0u)
            {
                InterlockedAdd(dispatchArgs[0].groupCountX, 1);
            }
        }
    }
}
