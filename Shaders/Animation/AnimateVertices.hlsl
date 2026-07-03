#define FLOAT16_SUPPORTED 0
#define INT16_SUPPORTED 0
#include "../../Include/RenderLimits.h"
#include "../CommonStructs.hlsl"
#include "../Bitpack.hlsl"
#include "../Math.hlsl"
#include "../LOD.hlsl"
#include "../AnimatedTransform.hlsl"

struct SkinnedVertex
{
    uint positionXY;
    uint positionZW;
    uint tangentSpace;
    uint texCoords;
    uint joints;
    uint weights;
};

cbuffer params : register(b0, space2)
{
    uint numPrimitiveGroups;
    uint2 viewportSize;
    uint shadowLOD;
    float lodDistanceModifier;
    float3 padding;
    float4x4 viewProjection;
}

StructuredBuffer<uint>           sBoneMtx           : register(t0);
StructuredBuffer<Entity>         sEntities          : register(t1);
StructuredBuffer<PrimitiveGroup> sPrimitiveGroups   : register(t2);
StructuredBuffer<uint>           sVisibleSparseIndices : register(t3);
StructuredBuffer<SkinnedVertex>  sVertexBuffer      : register(t4);
StructuredBuffer<uint>           sSparseToDense     : register(t5);
StructuredBuffer<uint>           sVisibleCount      : register(t6);

RWStructuredBuffer<uint> sAnimatedPosition : register(u0, space1);

[numthreads(1, 32, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID)
{
    uint drawIdx = globalID.x;
    if (drawIdx >= numPrimitiveGroups)
        return;

    uint primitiveIdx = drawIdx / MESH_LOD_COUNT;
    uint lod = drawIdx - primitiveIdx * MESH_LOD_COUNT;
    PrimitiveGroup group = sPrimitiveGroups[primitiveIdx];

    uint visibleSlot = groupID.y * 32u + groupThreadID.y;
    if (visibleSlot >= sVisibleCount[0])
        return;

    uint sparse = sVisibleSparseIndices[visibleSlot];
    if (sparse >= uint(MAX_ANIM_INSTANCES))
        return;

    uint baseDenseIdx = sSparseToDense[sparse];
    if (baseDenseIdx == 0xffffffffu)
        return;

    Entity baseEntity = sEntities[baseDenseIdx];
    PrimitiveGroup baseGroup = sPrimitiveGroups[baseEntity.primitiveIdx];
    uint instanceSlot = baseDenseIdx - baseGroup.entityOffset;
    if (instanceSlot >= group.numEntities)
        return;

    uint denseIdx = group.entityOffset + instanceSlot;
    Entity entity = sEntities[denseIdx];
    if (entity.sparse != sparse)
        return;

    uint vertexBase = groupID.z * 32u;
    uint animatedBase = group.lodAnimatedVertexOffset[lod];
    uint numVertices = group.lodNumVertices[lod];
    uint sourceVertexBase = group.lodVertexOffset[lod];

    uint boneStart = sparse * MAX_BONES;

    // Skin in model space (entity rotation/scale are applied per vertex shader) and accumulate in a
    // bounds-local frame: the bone translation is shifted by the bounds center so skinned positions
    // stay near 0. Result is packed as xy11z10 unorm in one uint.
    float3 boundsCenter = AnimatedBoundsCenter(group.aabbMin.xyz, group.aabbMax.xyz);
    float3 boundsExtent = AnimatedBoundsExtent(group.aabbMin.xyz, group.aabbMax.xyz);

    [loop]
    for (uint vertexOffset = 0; vertexOffset < 32u; vertexOffset++)
    {
        uint localVertex = vertexBase + vertexOffset;
        if (localVertex >= numVertices)
            break; 

        uint animatedLocalVertex = animatedBase + localVertex;
        uint outVertex = sparse * uint(MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE) + animatedLocalVertex;
        if (animatedLocalVertex >= uint(MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE) || outVertex >= uint(MAX_ANIMATED_VERTEX))
            break;

        uint sourceVertex = sourceVertexBase + localVertex;

        SkinnedVertex input = sVertexBuffer[sourceVertex];
        f16_4 packedPos = f16_4(UnpackHalf2(input.positionXY), UnpackHalf2(input.positionZW));
        f16_4 inputPos = f16_4(packedPos.xyz, f16(1.0));

        f16_4 weights;
        weights.xyz = UnpackVec3XY11Z10Unorm(input.weights);
        weights.w   = saturate(f16(1.0) - weights.x - weights.y - weights.z);

        f16_3x4 animMat = (f16_3x4)0;
        uint joints = input.joints;

        [unroll]
        for (int i = 0; i < 4; i++)
        {
            uint shift = i * 8;
            f16_3x4 bone = LoadBone(sBoneMtx, boneStart + ((joints >> shift) & 0xFFu));
            animMat[0] = mad(weights[i], bone[0], animMat[0]);
            animMat[1] = mad(weights[i], bone[1], animMat[1]);
            animMat[2] = mad(weights[i], bone[2], animMat[2]);
        }

        // shift the blended translation by the bounds center so mul() yields the skinned position
        // relative to the center (bounds-local accumulation). Tangent frame is no longer cached here.
        animMat[0][3] -= f16(boundsCenter.x);
        animMat[1][3] -= f16(boundsCenter.y);
        animMat[2][3] -= f16(boundsCenter.z);

        // mul(Vector, Transpose(Matrix)) is equivalent to mul(Matrix, Vector) in HLSL.
        // FLOAT16_SUPPORTED is 0 here, so this skinning math is fp32.
        float3 centerRelPos = mul(animMat, inputPos);

        sAnimatedPosition[outVertex] = PackAnimatedCenterRel(centerRelPos, boundsExtent);
    }
}