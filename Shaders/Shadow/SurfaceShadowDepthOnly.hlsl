#include "../CommonStructs.hlsl"
#include "../Bitpack.hlsl"
#include "../Math.hlsl"
#include "Shadow.hlsl"

cbuffer vs_params : register(b0, space1)
{
    uint uCascadeIndex;
};

StructuredBuffer<Entity>         sEntities         : register(t0);
StructuredBuffer<PrimitiveGroup> sPrimitiveGroups  : register(t1);
StructuredBuffer<uint>           sDrawSparseIndices : register(t2);
StructuredBuffer<ShadowCascadeBuffer> sShadowCascades : register(t3);

struct VSInput
{
    uint2    aPos          : POSITION0;
    uint     aTangentSpace : TANGENT0;
    f16_2_io aTexCoords    : TEXCOORD0;
};

float4 vert(VSInput input, uint instanceID : SV_InstanceID, [[vk::builtin("DrawIndex")]] uint drawID : DRAWINDEX) : SV_Position
{
    uint primitiveIdx = drawID / MESH_LOD_COUNT;
    uint lod = drawID - primitiveIdx * MESH_LOD_COUNT;
    PrimitiveGroup group = sPrimitiveGroups[primitiveIdx];
    uint denseIdx = sDrawSparseIndices[lod * MAX_ENTITY + PrimitiveGroup_EntityOffset(group) + instanceID];
    Entity entity = sEntities[denseIdx];
    float3 aabbMin = PrimitiveGroup_AABBMin(group);
    float3 aabbMax = PrimitiveGroup_AABBMax(group);

    f16_4 insRot   = normalize(UnpackRGBA16Snorm(entity.rotation[0], entity.rotation[1]));
    f16_3 insScale = UnpackRGBA16Unorm(entity.scale).xyz * f16(10.0);
    float3 localPos = aabbMin + UnpackUnorm16x4(input.aPos).xyz * (aabbMax - aabbMin);
    f16_3 worldPos = QMulVec3(insRot, f16_3(localPos) * insScale);
    float3 finalWorldPos = float3(worldPos) + entity.position.xyz;
    return MulShadowCascade(sShadowCascades[0], uCascadeIndex, float4(finalWorldPos, 1.0));
}

float frag(float4 position : SV_Position) : SV_Target0
{
    return position.z;
}
