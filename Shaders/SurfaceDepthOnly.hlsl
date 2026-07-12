#include "TextureSampling.hlsl"
#include "Bitpack.hlsl"
#include "Math.hlsl"

cbuffer vs_params : register(b0, space1)
{
    float4x4 uViewProj;
};

StructuredBuffer<Entity>         sEntities          : register(t0);
StructuredBuffer<PrimitiveGroup> sPrimitiveGroups   : register(t1);
StructuredBuffer<uint>           sDrawSparseIndices : register(t2);
// frag
StructuredBuffer<MaterialGPU>       sMaterials          : register(t1, space2);
StructuredBuffer<TextureDescriptor> sTextureDescriptors : register(t2, space2);
Texture2DArray<float4> AlbedoPages : register(t0, space2);
SamplerState Sampler               : register(s0, space2);

struct VSInput
{
    uint2    aPos          : POSITION0;
    uint     aTangentSpace : TANGENT0;
    f16_2_io aTexCoords    : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    f16_2_io texCoords : TEXCOORD0;
    f16_4_io vertexColor : COLOR0;
    nointerpolation uint materialIndex : TEXCOORD1;
};

VSOutput vert(VSInput input, uint instanceID : SV_InstanceID, [[vk::builtin("DrawIndex")]] uint drawID : DRAWINDEX)
{
    uint primitiveIdx = drawID / MESH_LOD_COUNT;
    uint lod = drawID - primitiveIdx * MESH_LOD_COUNT;
    PrimitiveGroup group = sPrimitiveGroups[primitiveIdx];
    uint denseIdx = sDrawSparseIndices[lod * MAX_ENTITY + PrimitiveGroup_EntityOffset(group) + instanceID];
    Entity entity = sEntities[denseIdx];
    float3 aabbMin = PrimitiveGroup_AABBMin(group);
    float3 aabbMax = PrimitiveGroup_AABBMax(group);

    f16_4  insRot   = normalize(UnpackRGBA16Snorm(entity.rotation[0], entity.rotation[1]));
    f16_3  insScale = UnpackRGBA16Unorm(entity.scale).xyz * f16(10.0);
    float3 localPos = aabbMin + UnpackUnorm16x4(input.aPos).xyz * (aabbMax - aabbMin);
    f16_3  worldPos = QMulVec3(insRot, f16_3(localPos) * insScale);
    float3 finalWorldPos = float3(worldPos) + entity.position.xyz;

    VSOutput o;
    o.position = mul(uViewProj, float4(finalWorldPos, 1.0));
    o.texCoords = input.aTexCoords;
    o.vertexColor = f16_4_io(UnpackAVertexColor(input.aPos));
    o.materialIndex = PrimitiveGroup_MaterialIndex(group);
    return o;
}

float frag(VSOutput input) : SV_Target0
{
    MaterialGPU material = sMaterials[input.materialIndex];
    if (MaterialIsAlphaMasked(material.flags))
    {
        TextureDescriptor albedo = sTextureDescriptors[material.albedoDescriptor];
        f16_4 albedoSample = SampleTexturePageRGBA(AlbedoPages, Sampler, albedo, float2(input.texCoords), f16_4(1.0, 1.0, 1.0, 1.0));
        AlphaClipMaterial(material, float(albedoSample.a * f16_4(input.vertexColor).a));
    }
    return input.position.z;
}
