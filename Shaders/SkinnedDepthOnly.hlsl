#include "../Include/RenderLimits.h"
#include "TextureSampling.hlsl"
#include "Bitpack.hlsl"
#include "Math.hlsl"
#include "AnimatedTransform.hlsl"

cbuffer vs_params : register(b0, space1)
{
    float4x4 uViewProj;
};

StructuredBuffer<Entity>         sEntities          : register(t0);
StructuredBuffer<PrimitiveGroup> sPrimitiveGroups   : register(t1);
StructuredBuffer<uint>           sDrawSparseIndices : register(t2);
StructuredBuffer<uint>           sAnimatedPosition  : register(t3);

StructuredBuffer<MaterialGPU> sMaterials : register(t1, space2);
StructuredBuffer<TextureDescriptor> sTextureDescriptors : register(t2, space2);

Texture2DArray<float4> AlbedoPages : register(t0, space2);
SamplerState           Sampler     : register(s0, space2);

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
    float4 position : SV_Position;
    f16_2_io texCoords : TEXCOORD0;
    nointerpolation uint materialIndex : TEXCOORD1;
};

VSOutput vert(VSInput input, uint instanceID : SV_InstanceID, 
            [[vk::builtin("DrawIndex")]] uint drawID : DRAWINDEX,
            uint vertexId : SV_VertexID)
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

    VSOutput o;
    o.position = mul(uViewProj, float4(finalWorldPos, 1.0));
    o.texCoords = input.aTexCoords;
    o.materialIndex = group.materialIndex;
    return o;
}

float frag(VSOutput input) : SV_Target0
{
    MaterialGPU material = sMaterials[input.materialIndex];
    if (MaterialIsAlphaMasked(material.flags))
    {
        TextureDescriptor albedo = sTextureDescriptors[material.albedoDescriptor];
        f16_4 albedoSample = SampleTexturePageRGBA(AlbedoPages, Sampler, albedo, float2(input.texCoords), f16_4(1.0, 1.0, 1.0, 1.0));
        AlphaClipMaterial(material, float(albedoSample.a));
    }
    return input.position.z;
}