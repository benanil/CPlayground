// terrain forward pass: chunk-relative vertex format and triplanar material
// selection, shaded directly into the HDR color target used by the current renderer.
#include "TextureSampling.hlsl"
#include "Bitpack.hlsl"
#include "Math.hlsl"
#include "PBR.hlsl"
#include "Shadow/Shadow.hlsl"

#define TERRAIN_UV_SCALE     (1.0 / 6.0)
#define TERRAIN_NORMAL_DX    1

cbuffer vs_params : register(b0, space1)
{
    float4x4 uViewProj;
    float4   uChunkOriginSize;
    float4   uCameraPosition;
    float4   uCameraForward;
};

cbuffer ps_params : register(b0, space3)
{
    float4 uSunDirection;
    float4 uBrushPosRadius;
    float4 uCameraPositionPS;
    uint2  uOutputSize;
    uint2  uPad0;
};

StructuredBuffer<ShadowCascadeBuffer> sShadowCascades : register(t0);

Texture2DArray<float4> AlbedoLayers    : register(t0, space2);
Texture2DArray<float4> NormalLayers    : register(t1, space2);
Texture2DArray<float4> ArmLayers       : register(t2, space2);
Texture2D<float>       ShadowMap       : register(t3, space2);
Texture2D<float>       AmbientOcclusion : register(t4, space2);
Texture2D<float>       ContactShadow   : register(t5, space2);
SamplerState           Sampler         : register(s0, space2);
SamplerState           ShadowSampler   : register(s3, space2);

struct VSInput
{
    uint4 data : TEXCOORD0;
};

struct VSOutput
{
    float4 position   : SV_Position;
    float3 worldPos   : TEXCOORD0;
    float3 normal     : NORMAL;
    float4 shadowPos0 : TEXCOORD1;
    float4 shadowPos1 : TEXCOORD2;
    float4 shadowPos2 : TEXCOORD3;
    float  viewDepth  : TEXCOORD4;
    nointerpolation float3 cascadeSplits : TEXCOORD5;
    nointerpolation uint2 matIndices : TEXCOORD6;
    float2 matWeights : TEXCOORD7;
};

float3 TerrainDecodePosition(uint4 data)
{
    uint qx = data.x & 0x1FFFFFu;
    uint qy = (data.x >> 21) | ((data.y & 0x3FFu) << 11);
    uint qz = (data.y >> 10) & 0x1FFFFFu;
    return float3(qx, qy, qz) * (uChunkOriginSize.w / 524288.0) + uChunkOriginSize.xyz;
}

float3 TerrainDecodeNormal(uint packed)
{
    float2 oct = float2(float(packed & 0xFFFFu), float(packed >> 16)) * (2.0 / 65535.0) - 1.0;
    float3 n = float3(oct.x, oct.y, 1.0 - abs(oct.x) - abs(oct.y));
    float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.0, -t, t);
    return normalize(n);
}

VSOutput vert(VSInput input)
{
    float3 worldPos = TerrainDecodePosition(input.data);

    VSOutput o;
    o.position = mul(uViewProj, float4(worldPos, 1.0));
    o.worldPos = worldPos;
    o.normal   = TerrainDecodeNormal(input.data.z);

    ShadowCascadeBuffer cascades = sShadowCascades[0];
    o.shadowPos0 = MulShadowCascade(cascades, 0u, float4(worldPos, 1.0));
    o.shadowPos1 = MulShadowCascade(cascades, 1u, float4(worldPos, 1.0));
    o.shadowPos2 = MulShadowCascade(cascades, 2u, float4(worldPos, 1.0));
    o.viewDepth  = dot(worldPos - uCameraPosition.xyz, uCameraForward.xyz);
    o.cascadeSplits = cascades.splitDistances.xyz;
    o.matIndices = uint2(input.data.w & 0xFFu, (input.data.w >> 8) & 0xFFu);
    o.matWeights = float2(float((input.data.w >> 16) & 0xFFu), float(input.data.w >> 24)) * (1.0 / 255.0);
    return o;
}

float4 SampleTriplanarLayer(Texture2DArray<float4> tex, float layer, float3 wpos, float3 blend)
{
    float4 sx = tex.Sample(Sampler, float3(wpos.zy * TERRAIN_UV_SCALE, layer));
    float4 sy = tex.Sample(Sampler, float3(wpos.xz * TERRAIN_UV_SCALE, layer));
    float4 sz = tex.Sample(Sampler, float3(wpos.xy * TERRAIN_UV_SCALE, layer));
    return sx * blend.x + sy * blend.y + sz * blend.z;
}

float3 SampleTriplanarNormal(float layer, float3 wpos, float3 blend, float3 N)
{
    float3 tx = NormalLayers.Sample(Sampler, float3(wpos.zy * TERRAIN_UV_SCALE, layer)).xyz * 2.0 - 1.0;
    float3 ty = NormalLayers.Sample(Sampler, float3(wpos.xz * TERRAIN_UV_SCALE, layer)).xyz * 2.0 - 1.0;
    float3 tz = NormalLayers.Sample(Sampler, float3(wpos.xy * TERRAIN_UV_SCALE, layer)).xyz * 2.0 - 1.0;
#if TERRAIN_NORMAL_DX
    tx.y = -tx.y; ty.y = -ty.y; tz.y = -tz.y;
#endif
    float3 nx = float3(tx.xy + N.zy, abs(N.x) * tx.z);
    float3 ny = float3(ty.xy + N.xz, abs(N.y) * ty.z);
    float3 nz = float3(tz.xy + N.xy, abs(N.z) * tz.z);
    float3 n = nx.zyx * blend.x + ny.xzy * blend.y + nz.xyz * blend.z;
    return normalize(lerp(N, normalize(n), 0.8));
}

float4 frag(VSOutput input) : SV_Target0
{
    float3 N = normalize(input.normal);
    float3 blend = pow(abs(N), 4.0);
    blend /= max(blend.x + blend.y + blend.z, 1e-4);

    float2 w = input.matWeights / max(input.matWeights.x + input.matWeights.y, 1e-4);
    float layerA = float(input.matIndices.x);
    float layerB = float(input.matIndices.y);

    float4 albedoSample = SampleTriplanarLayer(AlbedoLayers, layerA, input.worldPos, blend) * w.x
                        + SampleTriplanarLayer(AlbedoLayers, layerB, input.worldPos, blend) * w.y;
    float4 arm          = SampleTriplanarLayer(ArmLayers, layerA, input.worldPos, blend) * w.x
                        + SampleTriplanarLayer(ArmLayers, layerB, input.worldPos, blend) * w.y;

    float normalLayer = w.x >= w.y ? layerA : layerB;
    float3 shadingN = SampleTriplanarNormal(normalLayer, input.worldPos, blend, N);

    float3 baseColor = float3(SRGBToLinear(f16_3(albedoSample.rgb))) * max(arm.r, 0.08);
    if (uBrushPosRadius.w > 0.0)
    {
        float brushDist = distance(input.worldPos, uBrushPosRadius.xyz);
        float glow = 1.0 - smoothstep(uBrushPosRadius.w * 0.7, uBrushPosRadius.w, brushDist);
        baseColor = lerp(baseColor, float3(1.0, 1.0, 1.0), glow * 0.55);
    }

    float metallic  = saturate(arm.b);
    float roughness = saturate(arm.g);

    uint cascadeIndex = input.viewDepth > input.cascadeSplits.x ? 1u : 0u;
    cascadeIndex = input.viewDepth > input.cascadeSplits.y ? 2u : cascadeIndex;
    float4 shadowPos = cascadeIndex == 0u ? input.shadowPos0 : (cascadeIndex == 1u ? input.shadowPos1 : input.shadowPos2);
    float shadow = SampleShadow(ShadowMap, ShadowSampler, shadowPos, cascadeIndex, shadingN, uSunDirection.xyz);

    roughness = SpecularAntiAliasing(roughness, ddx(shadingN), ddy(shadingN));

    float2 uv = saturate(input.position.xy / float2(uOutputSize));
    float ao = AmbientOcclusion.SampleLevel(Sampler, uv, 0.0);
    shadow *= ContactShadow.SampleLevel(Sampler, uv, 0.0);

    float3 viewDir = uCameraPositionPS.xyz - input.worldPos;
    float3 color = ApplyPBR(baseColor, shadingN, viewDir, saturate(metallic), saturate(roughness),
                            saturate(shadow), ao, uSunDirection.xyz);
    return float4(color, 1.0);
}
