#include "../../Include/RenderLimits.h"
#include "../CommonStructs.hlsl"
#include "../Bitpack.hlsl"
#include "../Math.hlsl"
#include "../AnimatedTransform.hlsl"
#include "Shadow.hlsl"

StructuredBuffer<Entity>              sEntities          : register(t0);
StructuredBuffer<PrimitiveGroup>      sPrimitiveGroups   : register(t1);
StructuredBuffer<uint>                sDrawSparseIndices : register(t2);
StructuredBuffer<uint>                sAnimatedPosition  : register(t3);
StructuredBuffer<ShadowCascadeBuffer> sShadowCascades    : register(t4);

cbuffer vs_params : register(b0, space1)
{
    uint uCascadeIndex;
};

struct VSInput
{
    f16_4_io  aPos          : POSITION0;
    uint      aTangentSpace : TANGENT0;
    f16_2_io  aTexCoords    : TEXCOORD0;
    uint4     aJoints       : BLENDINDICES0;
    uint      aWeights      : BLENDWEIGHT0;
};

float4 vert(VSInput input,
            uint instanceID : SV_InstanceID,
            [[vk::builtin("DrawIndex")]] uint drawID : DRAWINDEX,
            uint vertexId : SV_VertexID) : SV_Position
{
    uint primitiveIdx = drawID / MESH_LOD_COUNT;
    uint lod = drawID - primitiveIdx * MESH_LOD_COUNT;
    PrimitiveGroup group = sPrimitiveGroups[primitiveIdx];
    uint denseIdx  = sDrawSparseIndices[lod * uint(MAX_ANIM_INSTANCES) + group.entityOffset + instanceID];
    uint localVertex = vertexId - group.lodVertexOffset[lod];
    uint sparse = sEntities[denseIdx].sparse;
    uint animatedVertex = sparse * uint(MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE) + group.lodAnimatedVertexOffset[lod] + localVertex;
    uint animatedPos = sAnimatedPosition[animatedVertex];
    Entity entity = sEntities[denseIdx];
    f16_4 insRot = normalize(UnpackRGBA16Snorm(entity.rotation[0], entity.rotation[1]));
    f16_3 insScale = UnpackRGBA16Unorm(entity.scale).xyz * f16(10.0);
    float3 modelPos = UnpackAnimatedModelPos(animatedPos, group.aabbMin.xyz, group.aabbMax.xyz);
    float3 finalWorldPos = AnimatedWorldPos(modelPos, float4(insRot), float3(insScale), entity.position.xyz);
    return MulShadowCascade(sShadowCascades[0], uCascadeIndex, float4(finalWorldPos, 1.0));
}

float frag(float4 position : SV_Position) : SV_Target0
{
    return position.z;
}