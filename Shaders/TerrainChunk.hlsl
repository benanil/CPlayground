// transvoxel-unity port terrain surface: heap-resident triangle soup, every chunk lives
// in one shared vertex buffer and draws in a single indirect multidraw.
#include "Bitpack.hlsl"
#include "TextureSampling.hlsl"
#include "PBR.hlsl"
#include "Shadow/Shadow.hlsl"
#include "CommonStructs.hlsl"

#define TERRAIN_UV_SCALE  (1.0 / 6.0)
#define TERRAIN_NORMAL_DX 1
#define TERRAIN_NORMAL_STRENGTH 0.35
#define T_CHUNK_CELLS 16.0 /* equal to T_CHUNK_CELLS */

Texture2DArray<float4> AlbedoLayers : register(t0, space2);
Texture2DArray<float4> NormalLayers : register(t1, space2);
Texture2DArray<float4> ArmLayers    : register(t2, space2);
Texture2D<float> ShadowMap           : register(t3, space2);
Texture2DArray<float> PointShadowTexture : register(t4, space2);
Texture2DArray<float> SpotShadowTexture  : register(t5, space2);
Texture2D<float> AmbientOcclusion    : register(t6, space2);
Texture2D<float> ContactShadow       : register(t7, space2);
SamplerState Sampler                 : register(s0, space2);
SamplerState ShadowSampler           : register(s3, space2);
SamplerState PointShadowSampler      : register(s4, space2);
SamplerState SpotShadowSampler       : register(s5, space2);

StructuredBuffer<LightGPU>          sLights             : register(t8, space2);
StructuredBuffer<uint2>             sLightGrid          : register(t9, space2);
StructuredBuffer<uint>              sLightIndex         : register(t10, space2);
StructuredBuffer<PointShadowMatrix> PointShadowMatrices : register(t11, space2);
StructuredBuffer<PointShadowMatrix> SpotShadowMatrices  : register(t12, space2);

#include "LocalLights.hlsl"

StructuredBuffer<uint2> ChunkLocations : register(t0);
StructuredBuffer<ShadowCascadeBuffer> sShadowCascades : register(t1);

struct VSInput
{
    uint position   : POSITION0;
    uint normal      : NORMAL;
    uint materials   : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal   : NORMAL;
    nointerpolation uint materials : TEXCOORD1;
    float4 shadowPos0 : TEXCOORD2;
    float4 shadowPos1 : TEXCOORD3;
    float4 shadowPos2 : TEXCOORD4;
    float  viewDepth : TEXCOORD5;
    nointerpolation float3 cascadeSplits : TEXCOORD6;
};

cbuffer vs_params : register(b0, space1)
{
    float4x4 uViewProj;
    float4   uCameraPosition;
    float4   uCameraForward;
};

cbuffer ps_params : register(b0, space3)
{
    float4 uBrushPosRadius; // xyz world position, w radius (<= 0 when inactive)
    float4 uSunDirection;
    float4 uCameraPositionPS;
    uint2  uOutputSize;
    uint   uTilesX;
    uint   uTileSize;
    uint   uLocalLightsEnabled;
    uint3  uPad0;
};

int DecodeS16(uint v) {
    return int(v << 16) >> 16;
}

float3 DecodeTerrainPosition(uint packedPosition, uint drawID)
{
    uint2 chunkLocation = ChunkLocations[drawID];
    int3 chunkCoord = int3(DecodeS16(chunkLocation.x), int(chunkLocation.x) >> 16, DecodeS16(chunkLocation.y));
    float3 local = UnpackVec3XY11Z10Unorm(packedPosition) * (f32)T_CHUNK_CELLS;
    return float3(chunkCoord) * (f32)T_CHUNK_CELLS + local;
}

VSOutput vert(VSInput i, [[vk::builtin("DrawIndex")]] uint drawID : DRAWINDEX)
{
    VSOutput o;
    float3 worldPos = DecodeTerrainPosition(i.position, drawID);
    o.position = mul(uViewProj, float4(worldPos, 1.0));
    o.worldPos = worldPos;
    f16_3 normal = UnpackNormal(i.normal);
    o.normal = float3(normal);
    o.materials = i.materials;
    ShadowCascadeBuffer cascades = sShadowCascades[0];
    o.shadowPos0 = MulShadowCascade(cascades, 0u, float4(worldPos, 1.0));
    o.shadowPos1 = MulShadowCascade(cascades, 1u, float4(worldPos, 1.0));
    o.shadowPos2 = MulShadowCascade(cascades, 2u, float4(worldPos, 1.0));
    o.viewDepth = dot(worldPos - uCameraPosition.xyz, uCameraForward.xyz);
    o.cascadeSplits = cascades.splitDistances.xyz;
    return o;
}

float4 SampleTriplanarLayer(Texture2DArray<float4> tex, float layer, float3 wpos, float3 blend)
{
    float4 sx = tex.Sample(Sampler, float3(wpos.zy * TERRAIN_UV_SCALE, layer));
    float4 sy = tex.Sample(Sampler, float3(wpos.xz * TERRAIN_UV_SCALE, layer));
    float4 sz = tex.Sample(Sampler, float3(wpos.xy * TERRAIN_UV_SCALE, layer));
    return sx * blend.x + sy * blend.y + sz * blend.z;
}
/*
// https://bgolus.medium.com/normal-mapping-for-a-triplanar-shader-10bf39dca05a
float3 SampleTriplanarNormal(float layer, float3 wpos, float3 blend, float3 N)
{
    float3 tx = NormalLayers.Sample(Sampler, float3(wpos.zy * TERRAIN_UV_SCALE, layer)).xyz * 2.0 - 1.0;
    float3 ty = NormalLayers.Sample(Sampler, float3(wpos.xz * TERRAIN_UV_SCALE, layer)).xyz * 2.0 - 1.0;
    float3 tz = NormalLayers.Sample(Sampler, float3(wpos.xy * TERRAIN_UV_SCALE, layer)).xyz * 2.0 - 1.0;
#if TERRAIN_NORMAL_DX
    tx.y = -tx.y;
    ty.y = -ty.y;
    tz.y = -tz.y;
#endif
    tx.xy *= TERRAIN_NORMAL_STRENGTH;
    ty.xy *= TERRAIN_NORMAL_STRENGTH;
    tz.xy *= TERRAIN_NORMAL_STRENGTH;
    tx.z = sqrt(saturate(1.0 - dot(tx.xy, tx.xy)));
    ty.z = sqrt(saturate(1.0 - dot(ty.xy, ty.xy)));
    tz.z = sqrt(saturate(1.0 - dot(tz.xy, tz.xy)));

    float signX = N.x < 0.0 ? -1.0 : 1.0;
    float signY = N.y < 0.0 ? -1.0 : 1.0;
    float signZ = N.z < 0.0 ? -1.0 : 1.0;
    float3 nx = float3(tx.z * signX, tx.y, tx.x);
    float3 ny = float3(ty.x, ty.z * signY, ty.y);
    float3 nz = float3(tz.x, tz.y, tz.z * signZ);
    float3 detailN = normalize(nx * blend.x + ny * blend.y + nz * blend.z);
    if (dot(detailN, N) < 0.0) detailN = -detailN;
    return normalize(lerp(N, detailN, 0.8));
}
*/
float4 frag(VSOutput i) : SV_Target0
{
    float3 N = normalize(i.normal);
    float3 triBlend = pow(abs(N), 4.0);
    triBlend /= max(triBlend.x + triBlend.y + triBlend.z, 1.0e-4);

    float layerA = float(i.materials & 0xFFu);
    float layerB = float((i.materials >> 8) & 0xFFu);
    float wA = float((i.materials >> 16) & 0xFFu) * (1.0 / 255.0);
    float wB = 1.0 - wA;

    float4 albedoSample = SampleTriplanarLayer(AlbedoLayers, layerA, i.worldPos, triBlend) * wA
                        + SampleTriplanarLayer(AlbedoLayers, layerB, i.worldPos, triBlend) * wB;
    float4 armSample = SampleTriplanarLayer(ArmLayers, layerA, i.worldPos, triBlend) * wA
                     + SampleTriplanarLayer(ArmLayers, layerB, i.worldPos, triBlend) * wB;

    // float normalLayer = wA >= wB ? layerA : layerB;
	float3 shadingN = N; // SampleTriplanarNormal(normalLayer, i.worldPos, triBlend, N);
    float3 baseColor = float3(SRGBToLinear(f16_3(albedoSample.rgb))) * max(armSample.r, 0.08);

    if (uBrushPosRadius.w > 0.0)
    {
        float brushDist = distance(i.worldPos, uBrushPosRadius.xyz);
        float glow = 1.0 - smoothstep(uBrushPosRadius.w * 0.7, uBrushPosRadius.w, brushDist);
        baseColor = lerp(baseColor, float3(1.0, 0.85, 0.45), glow * 0.55);
    }

    float3 viewDir = uCameraPositionPS.xyz - i.worldPos;
    if (dot(shadingN, viewDir) < 0.0) shadingN = -shadingN;
    float metallic = saturate(armSample.b);
    float roughness = SpecularAntiAliasing(saturate(armSample.g), ddx(shadingN), ddy(shadingN));

    uint cascadeIndex = 0u;
    if (i.viewDepth > i.cascadeSplits.x) cascadeIndex = 1u;
    if (i.viewDepth > i.cascadeSplits.y) cascadeIndex = 2u;
    float4 shadowPos = i.shadowPos0;
    if (cascadeIndex == 1u) shadowPos = i.shadowPos1;
    if (cascadeIndex == 2u) shadowPos = i.shadowPos2;
    float shadow = SampleShadow(ShadowMap, ShadowSampler, shadowPos, cascadeIndex, shadingN, uSunDirection.xyz);
	shadow = max(shadow, 0.4);
    float2 uv = saturate(i.position.xy / float2(uOutputSize));
    float ao = AmbientOcclusion.SampleLevel(Sampler, uv, 0.0);
    shadow *= ContactShadow.SampleLevel(Sampler, uv, 0.0);

    float3 color = ApplyPBR(baseColor, shadingN, viewDir, metallic, roughness,
                            shadow, ao, uSunDirection.xyz);
    color += baseColor * (0.08 + saturate(shadingN.y) * 0.08);
    if (uLocalLightsEnabled != 0u)
        color += AccumulateTileLights(baseColor, shadingN, viewDir, metallic, roughness,
                                      i.worldPos, ao, uint2(i.position.xy), uTilesX, uTileSize);
    return float4(color, 1.0);
}
