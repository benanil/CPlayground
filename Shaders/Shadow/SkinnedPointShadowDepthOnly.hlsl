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
StructuredBuffer<PointShadowMatrix>   sPointShadowSides  : register(t4);

cbuffer vs_params : register(b0, space1)
{
    uint uShadowSideIndex;
};

struct VSInput
{
    f16_4_io  aPos          : POSITION0;
    uint      aTangentSpace : TANGENT0;
    f16_2_io  aTexCoords    : TEXCOORD0;
    uint4     aJoints       : BLENDINDICES0;
    uint      aWeights      : BLENDWEIGHT0;
};

struct VSOutput
{
    float4 position     : SV_Position;
    float4 clipDistance : SV_ClipDistance;
};

float4 AtlasPointShadowClip(float4 clip, uint face)
{
    clip.x = (clip.x + (float(face) * 2.0f - 5.0f) * clip.w) * (1.0f / 6.0f);
    return clip;
}

VSOutput MakeAtlasOutput(float4 clip, uint face)
{
    VSOutput output;
    output.position = AtlasPointShadowClip(clip, face);
    output.clipDistance = float4(clip.x + clip.w, clip.w - clip.x, clip.y + clip.w, clip.w - clip.y);
    return output;
}

VSOutput vert(VSInput input,
              uint instanceID : SV_InstanceID,
              [[vk::builtin("DrawIndex")]] uint drawID : DRAWINDEX,
              uint vertexId : SV_VertexID)
{
    uint entityInstanceID = instanceID / POINT_SHADOW_FACE_COUNT;
    uint face         = instanceID % POINT_SHADOW_FACE_COUNT;
    uint primitiveIdx = drawID / MESH_LOD_COUNT;
    uint lod          = drawID - primitiveIdx * MESH_LOD_COUNT;

    PrimitiveGroup group  = sPrimitiveGroups[primitiveIdx];
    uint denseIdx         = sDrawSparseIndices[lod * uint(MAX_ANIM_INSTANCES) + group.entityOffset + entityInstanceID];
    uint localVertex      = vertexId - group.lodVertexOffset[lod];
    uint sparse           = sEntities[denseIdx].sparse;
    uint animatedVertex   = sparse * uint(MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE) + group.lodAnimatedVertexOffset[lod] + localVertex;
    uint animatedPos      = sAnimatedPosition[animatedVertex];
    Entity entity  = sEntities[denseIdx];
    f16_4 insRot = normalize(UnpackRGBA16Snorm(entity.rotation[0], entity.rotation[1]));
    f16_3 insScale = UnpackRGBA16Unorm(entity.scale).xyz * f16(10.0);
    float3 modelPos = UnpackAnimatedModelPos(animatedPos, group.aabbMin.xyz, group.aabbMax.xyz);
    float3 finalWorldPos = AnimatedWorldPos(modelPos, float4(insRot), float3(insScale), entity.position.xyz);
    float4 clip = MulPointShadowSide(sPointShadowSides[uShadowSideIndex + face], float4(finalWorldPos, 1.0));
    return MakeAtlasOutput(clip, face);
}

float frag(float4 position : SV_Position) : SV_Target0
{
    return position.z;
}