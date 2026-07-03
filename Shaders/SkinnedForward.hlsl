// Forward+ opaque skinned pass. Vertex stage matches Skinned.hlsl (re-skins the tangent
// frame from the bone matrices); fragment stage shades fully like SurfaceForward.hlsl.
#include "../Include/RenderLimits.h"
#include "TextureSampling.hlsl"
#include "PBR.hlsl"
#include "Bitpack.hlsl"
#include "Math.hlsl"
#include "AnimatedTransform.hlsl"
#include "Shadow/Shadow.hlsl"

cbuffer vs_params : register(b0, space1)
{
    float4x4 uViewProj;
    float4 uCameraPosition;
    float4 uCameraForward;
};

cbuffer ps_params : register(b0, space3)
{
    float4 uSunDirection;
    float4 uCameraPositionPS;
    uint2  uOutputSize;
    uint   uTilesX;
    uint   uTileSize;
    uint   uLocalLightsEnabled;
    uint3  uPad0;
};

StructuredBuffer<Entity>         sEntities            : register(t0);
StructuredBuffer<PrimitiveGroup> sPrimitiveGroups     : register(t1);
StructuredBuffer<uint>           sDrawSparseIndices   : register(t2);
StructuredBuffer<uint>           sAnimatedPosition    : register(t3);
StructuredBuffer<ShadowCascadeBuffer> sShadowCascades : register(t4);
StructuredBuffer<uint>           sBoneMtx             : register(t5);

Texture2DArray<float4> AlbedoPages            : register(t0, space2);
Texture2DArray<float2> NormalPages            : register(t1, space2);
Texture2DArray<float2> MetallicRoughnessPages : register(t2, space2);
Texture2D<float>       ShadowMap              : register(t3, space2);
Texture2DArray<float>  PointShadowTexture     : register(t4, space2);
Texture2DArray<float>  SpotShadowTexture      : register(t5, space2);
Texture2D<float>       AmbientOcclusion       : register(t6, space2);
Texture2D<float>       ContactShadow          : register(t7, space2);
SamplerState           Sampler                : register(s0, space2);
SamplerState           ShadowSampler          : register(s3, space2);
SamplerState           PointShadowSampler     : register(s4, space2);
SamplerState           SpotShadowSampler      : register(s5, space2);

StructuredBuffer<MaterialGPU>        sMaterials          : register(t8, space2);
StructuredBuffer<TextureDescriptor>  sTextureDescriptors : register(t9, space2);
StructuredBuffer<LightGPU>           sLights             : register(t10, space2);
StructuredBuffer<uint2>              sLightGrid          : register(t11, space2);
StructuredBuffer<uint>               sLightIndex         : register(t12, space2);
StructuredBuffer<PointShadowMatrix>  PointShadowMatrices : register(t13, space2);
StructuredBuffer<PointShadowMatrix>  SpotShadowMatrices  : register(t14, space2);

#include "LocalLights.hlsl"

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
    float4   position   : SV_Position;
    f16_2_io texCoords  : TEXCOORD0;
    f16_3_io normal     : NORMAL;
    f16_3_io tangent    : TANGENT0;
    f16_3_io bitangent  : TEXCOORD1;
    float4   shadowPos0 : TEXCOORD3;
    float4   shadowPos1 : TEXCOORD4;
    float4   shadowPos2 : TEXCOORD5;
    float    viewDepth  : TEXCOORD6;
    float3   worldPos   : TEXCOORD11;
    nointerpolation float3 cascadeSplits : TEXCOORD7;
    nointerpolation uint   materialIndex : TEXCOORD8;
    nointerpolation float  handedness : TEXCOORD9;
};

VSOutput vert(VSInput input, uint instanceID : SV_InstanceID, [[vk::builtin("DrawIndex")]] uint drawID : DRAWINDEX, uint vertexID : SV_VertexID)
{
    uint primitiveIdx = drawID / MESH_LOD_COUNT;
    uint lod = drawID - primitiveIdx * MESH_LOD_COUNT;
    PrimitiveGroup group = sPrimitiveGroups[primitiveIdx];
    uint denseIdx  = sDrawSparseIndices[lod * uint(MAX_ANIM_INSTANCES) + group.entityOffset + instanceID];
    uint localVertex = vertexID - group.lodVertexOffset[lod];
    uint sparse = sEntities[denseIdx].sparse;
    uint animatedVertex = sparse * uint(MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE) + group.lodAnimatedVertexOffset[lod] + localVertex;
    uint animatedPos = sAnimatedPosition[animatedVertex];
    Entity entity = sEntities[denseIdx];
    f16_4 insRot = normalize(UnpackRGBA16Snorm(entity.rotation[0], entity.rotation[1]));
    f16_3 insScale = UnpackRGBA16Unorm(entity.scale).xyz * f16(10.0);
    float3 modelPos = UnpackAnimatedModelPos(animatedPos, group.aabbMin.xyz, group.aabbMax.xyz);
    float3 finalWorldPos = AnimatedWorldPos(modelPos, float4(insRot), float3(insScale), entity.position.xyz);

    uint boneStart = sparse * MAX_BONES;
    f16_4 weights;
    weights.xyz = UnpackVec3XY11Z10Unorm(input.aWeights);
    weights.w   = saturate(f16(1.0) - weights.x - weights.y - weights.z);

    f16_3x4 animMat = (f16_3x4)0;
    [unroll]
    for (int i = 0; i < 4; i++)
    {
        f16_3x4 bone = LoadBone(sBoneMtx, boneStart + input.aJoints[i]);
        animMat[0] = mad(weights[i], bone[0], animMat[0]);
        animMat[1] = mad(weights[i], bone[1], animMat[1]);
        animMat[2] = mad(weights[i], bone[2], animMat[2]);
    }

    f16_3 restNormal, restTangent;
    UnpackNormalTangent(input.aTangentSpace, restNormal, restTangent);
    f16 tangentHandedness = UnpackTangentHandedness(input.aTangentSpace);

    f16_3x3 tbn;
    tbn[2] = normalize(QMulVec3(insRot, mul(animMat, f16_4(restNormal,  f16(0.0)))));
    tbn[1] = normalize(Orthonormalize(QMulVec3(insRot, mul(animMat, f16_4(restTangent, f16(0.0)))), tbn[2]));

    VSOutput o;
    o.position  = mul(uViewProj, float4(finalWorldPos, 1.0));
    o.texCoords = input.aTexCoords;
    o.normal    = tbn[2];
    o.tangent   = tbn[1];
    o.bitangent = cross(tbn[2], tbn[1]) * tangentHandedness;
    o.worldPos  = finalWorldPos;
    ShadowCascadeBuffer cascades = sShadowCascades[0];
    o.shadowPos0 = MulShadowCascade(cascades, 0u, float4(finalWorldPos, 1.0));
    o.shadowPos1 = MulShadowCascade(cascades, 1u, float4(finalWorldPos, 1.0));
    o.shadowPos2 = MulShadowCascade(cascades, 2u, float4(finalWorldPos, 1.0));
    o.viewDepth = dot(finalWorldPos - uCameraPosition.xyz, uCameraForward.xyz);
    o.cascadeSplits = cascades.splitDistances.xyz;
    o.materialIndex = group.materialIndex;
    o.handedness = float(tangentHandedness);
    return o;
}

float4 frag(VSOutput input) : SV_Target0
{
    MaterialGPU material = sMaterials[input.materialIndex];
    TextureDescriptor albedo = sTextureDescriptors[material.albedoDescriptor];
    TextureDescriptor normalDesc = sTextureDescriptors[material.normalDescriptor];
    TextureDescriptor mrDesc = sTextureDescriptors[material.metallicRoughnessDescriptor];

    f16_4 albedoSample = SampleTexturePageRGBA(AlbedoPages, Sampler, albedo, float2(input.texCoords), f16_4(1.0, 1.0, 1.0, 1.0));
    float4 baseFactor = UnpackColor4Uint(material.baseColorFactor);
    AlphaClipMaterial(material, float(albedoSample.a));
    f16_3 baseColor = SRGBToLinear(albedoSample.rgb) * f16_3(baseFactor.rgb);

    float3 tangentNormal = DecodeNormalRG(float2(SampleTexturePageRG(NormalPages, Sampler, normalDesc, float2(input.texCoords), f16_2(0.5, 0.5))));
    f16_2 mr = SampleTexturePageRG(MetallicRoughnessPages, Sampler, mrDesc, float2(input.texCoords), f16_2(1.0, 1.0));

    float3 N = normalize(tangentNormal.x * normalize(float3(input.tangent)) +
                         tangentNormal.y * normalize(float3(input.bitangent)) +
                         tangentNormal.z * normalize(float3(input.normal)));
    float metallicFactor  = float((material.metallicRoughnessFactor >> 16u) & 0xFFFFu) * (1.0f / 65535.0f);
    float roughnessFactor = float(material.metallicRoughnessFactor & 0xFFFFu) * (1.0f / 65535.0f);
    float metallic  = float(mr.x) * metallicFactor;
    float roughness = float(mr.y) * roughnessFactor;

    uint cascadeIndex = input.viewDepth > input.cascadeSplits.x ? 1u : 0u;
    cascadeIndex = input.viewDepth > input.cascadeSplits.y ? 2u : cascadeIndex;
    float4 shadowPos = cascadeIndex == 0u ? input.shadowPos0 : (cascadeIndex == 1u ? input.shadowPos1 : input.shadowPos2);
    float shadow = SampleShadow(ShadowMap, ShadowSampler, shadowPos, cascadeIndex, N, uSunDirection.xyz);

    roughness = SpecularAntiAliasing(roughness, ddx(N), ddy(N));

    float3 worldPos = input.worldPos;
    float3 viewDir = uCameraPositionPS.xyz - worldPos;
    float2 uv = (input.position.xy) / float2(uOutputSize);
    float ao = AmbientOcclusion.SampleLevel(Sampler, uv, 0.0f);
    shadow *= ContactShadow.SampleLevel(Sampler, uv, 0.0f);

    float3 color = ApplyPBR(float3(baseColor), N, viewDir, saturate(metallic), saturate(roughness),
                            saturate(shadow), ao, uSunDirection.xyz);
    if (uLocalLightsEnabled != 0u)
        color += AccumulateTileLights(float3(baseColor), N, viewDir, saturate(metallic), saturate(roughness),
                                      worldPos, ao, uint2(input.position.xy), uTilesX, uTileSize);
    return float4(color, 1.0f);
}