// Forward+ opaque surface pass. Vertex stage matches Surface.hlsl (the deferred G-buffer
// shader); the fragment stage shades fully: sun PBR + cascade shadow + ambient/AO, then
// accumulates the local lights binned into this pixel's screen tile. Output is a single
// HDR color (no G-buffer). Runs after the depth prepass with depth-test LESS_OR_EQUAL and depth-write off.
#include "TextureSampling.hlsl"
#include "PBR.hlsl"
#include "Bitpack.hlsl"
#include "Math.hlsl"
#include "Shadow/Shadow.hlsl"

#define LOD_VISUALIZE 0

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

StructuredBuffer<Entity>         sEntities          : register(t0);
StructuredBuffer<PrimitiveGroup> sPrimitiveGroups   : register(t1);
StructuredBuffer<uint>           sDrawSparseIndices : register(t2);
StructuredBuffer<ShadowCascadeBuffer> sShadowCascades : register(t3);

// Fragment sampled textures (space2): material pages + shadow atlases + AO/contact.
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

// Fragment storage buffers (space2), packed after the 8 sampled textures.
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
    uint2    aPos          : POSITION0;
    uint     aTangentSpace : TANGENT0;
    f16_2_io aTexCoords    : TEXCOORD0;
};

struct VSOutput
{
    float4   position   : SV_Position;
    f16_2_io texCoords  : TEXCOORD0;
    f16_3_io normal     : NORMAL;
    f16_3_io tangent    : TANGENT0;
    f16_3_io bitangent  : TEXCOORD1;
    f16_4_io vertexColor : COLOR0;
    float4   shadowPos0 : TEXCOORD3;
    float4   shadowPos1 : TEXCOORD4;
    float4   shadowPos2 : TEXCOORD5;
    float    viewDepth  : TEXCOORD6;
    float3   worldPos   : TEXCOORD11;
    nointerpolation float3 cascadeSplits : TEXCOORD7;
    nointerpolation uint materialIndex : TEXCOORD8;
    nointerpolation float handedness : TEXCOORD9;
	#if LOD_VISUALIZE == 1
	nointerpolation uint lod : TEXCOORD10;
    #endif
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

    f16_4 insRot   = normalize(UnpackRGBA16Snorm(entity.rotation[0], entity.rotation[1]));
    f16_3 insScale = UnpackRGBA16Unorm(entity.scale).xyz * f16(10.0);

    f16_3x3 tbn;
    UnpackNormalTangent(input.aTangentSpace, tbn[2], tbn[1]);
    f16 tangentHandedness = UnpackTangentHandedness(input.aTangentSpace);
    tbn[2] = QMulVec3(insRot, tbn[2]);
    tbn[1] = QMulVec3(insRot, tbn[1]);
    tbn[1] = normalize(Orthonormalize(tbn[1], tbn[2]));
    tbn[0] = normalize(cross(tbn[2], tbn[1])) * tangentHandedness;

    float3 localPos = aabbMin + UnpackUnorm16x4(input.aPos).xyz * (aabbMax - aabbMin);
    f16_3 worldPos = QMulVec3(insRot, f16_3(localPos) * insScale);
    float3 finalWorldPos = float3(worldPos) + entity.position.xyz;

    VSOutput o;
    o.position  = mul(uViewProj, float4(finalWorldPos, 1.0));
    o.texCoords = input.aTexCoords;
    o.normal    = normalize(tbn[2]);
    o.tangent   = tbn[1];
    o.bitangent = tbn[0];
    o.vertexColor = f16_4_io(UnpackAVertexColor(input.aPos));
    o.worldPos  = finalWorldPos;
	#if LOD_VISUALIZE == 1
	o.lod = lod;
	#endif
    ShadowCascadeBuffer cascades = sShadowCascades[0];
    o.shadowPos0 = MulShadowCascade(cascades, 0u, float4(finalWorldPos, 1.0));
    o.shadowPos1 = MulShadowCascade(cascades, 1u, float4(finalWorldPos, 1.0));
    o.shadowPos2 = MulShadowCascade(cascades, 2u, float4(finalWorldPos, 1.0));
    o.viewDepth = dot(finalWorldPos - uCameraPosition.xyz, uCameraForward.xyz);
    o.cascadeSplits = cascades.splitDistances.xyz;
    o.materialIndex = PrimitiveGroup_MaterialIndex(group);
    o.handedness = float(tangentHandedness);
    return o;
}

float4 frag(VSOutput input) : SV_Target0
{
    MaterialGPU material = sMaterials[input.materialIndex];
	TextureDescriptor albedo = sTextureDescriptors[material.AlbedoNormalDescriptor & 0xFFFF];
    TextureDescriptor normalDesc = sTextureDescriptors[(material.AlbedoNormalDescriptor >> 16)];
    TextureDescriptor mrDesc = sTextureDescriptors[material.metallicRoughnessDescriptorAndFlags & 0xFFFF];

    // Untextured materials keep the built-in fallback descriptors, whose atlas pages hold no
    // real data. Substitute neutral values so shading falls back to the material factors:
    // white albedo (baseColor == baseFactor), flat tangent normal, and pass-through metal/rough.
    f16_4 albedoSample = SampleTexturePageRGBA(AlbedoPages, Sampler, albedo, float2(input.texCoords), f16_4(1.0, 1.0, 1.0, 1.0));
    f16_4 baseFactor = UnpackColor4UintF16(material.baseColorFactor);
    f16_4 vertexColor = f16_4(input.vertexColor);
    AlphaClipMaterial(material, float(albedoSample.a * vertexColor.a));
    f16_3 baseColor = SRGBToLinear(albedoSample.rgb) * f16_3(baseFactor.rgb) * f16_3(vertexColor.rgb);
    float alpha = float(albedoSample.a * baseFactor.a * vertexColor.a);

	float3 tangentNormal = DecodeNormalRG(float2(SampleTexturePageRG(NormalPages, Sampler, normalDesc, float2(input.texCoords), f16_2(0.5f, 0.5f))));
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
	#if LOD_VISUALIZE == 1
	color[min(input.lod, 3)] += .2;
	#endif
	return float4(color, alpha);
}
