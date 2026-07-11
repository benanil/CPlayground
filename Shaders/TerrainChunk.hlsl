// transvoxel-unity port terrain surface: heap-resident triangle soup, every chunk lives
// in one shared vertex buffer and draws in a single indirect multidraw.
#include "Bitpack.hlsl"
#include "TextureSampling.hlsl"

#define TERRAIN_UV_SCALE  (1.0 / 6.0)
#define TERRAIN_NORMAL_DX 1

Texture2DArray<float4> AlbedoLayers : register(t0, space2);
Texture2DArray<float4> NormalLayers : register(t1, space2);
Texture2DArray<float4> ArmLayers    : register(t2, space2);
SamplerState Sampler : register(s0, space2);

struct VSInput
{
    float4 pos       : POSITION0;
    uint normal      : NORMAL;
    uint materials   : TEXCOORD0;
    uint blend       : TEXCOORD1;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal   : NORMAL;
    nointerpolation uint materials : TEXCOORD1;
    nointerpolation uint blend     : TEXCOORD2;
};

cbuffer vs_params : register(b0, space1)
{
    float4x4 uViewProj;
};

cbuffer ps_params : register(b0, space3)
{
    float4 uBrushPosRadius; // xyz world position, w radius (<= 0 when inactive)
    float4 uSunDirection;
};

VSOutput vert(VSInput i)
{
    VSOutput o;
    o.position = mul(uViewProj, float4(i.pos.xyz, 1.0));
    o.worldPos = i.pos.xyz;
    f16_3 normal = UnpackNormal(i.normal);
    o.normal = float3(normal);
    o.materials = i.materials;
    o.blend = i.blend;
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
    tx.y = -tx.y;
    ty.y = -ty.y;
    tz.y = -tz.y;
#endif
    float3 nx = float3(tx.xy + N.zy, abs(N.x) * tx.z);
    float3 ny = float3(ty.xy + N.xz, abs(N.y) * ty.z);
    float3 nz = float3(tz.xy + N.xy, abs(N.z) * tz.z);
    float3 n = nx.zyx * blend.x + ny.xzy * blend.y + nz.xyz * blend.z;
    return normalize(lerp(N, normalize(n), 0.8));
}

float4 frag(VSOutput i) : SV_Target0
{
    float3 N = normalize(i.normal);
    float3 triBlend = pow(abs(N), 4.0);
    triBlend /= max(triBlend.x + triBlend.y + triBlend.z, 1.0e-4);

    float layerA = float(i.materials & 0xFFu);
    float layerB = float((i.materials >> 8) & 0xFFu);
    float wA = float(i.blend & 0xFFu) * (1.0 / 255.0);
    float wB = 1.0 - wA;

    float4 albedoSample = SampleTriplanarLayer(AlbedoLayers, layerA, i.worldPos, triBlend) * wA
                        + SampleTriplanarLayer(AlbedoLayers, layerB, i.worldPos, triBlend) * wB;
    float4 armSample = SampleTriplanarLayer(ArmLayers, layerA, i.worldPos, triBlend) * wA
                     + SampleTriplanarLayer(ArmLayers, layerB, i.worldPos, triBlend) * wB;

    float normalLayer = wA >= wB ? layerA : layerB;
    float3 shadingN = SampleTriplanarNormal(normalLayer, i.worldPos, triBlend, N);
    float3 color = float3(SRGBToLinear(f16_3(albedoSample.rgb))) * max(armSample.r, 0.08);

    if (uBrushPosRadius.w > 0.0)
    {
        float brushDist = distance(i.worldPos, uBrushPosRadius.xyz);
        float glow = 1.0 - smoothstep(uBrushPosRadius.w * 0.7, uBrushPosRadius.w, brushDist);
        color = lerp(color, float3(1.0, 0.85, 0.45), glow * 0.55);
    }

    float lambert = saturate(dot(shadingN, normalize(uSunDirection.xyz)));
    float sky = saturate(shadingN.y) * 0.18;
    color *= 0.34 + lambert * 0.56 + sky;
    return float4(color, 1.0);
}
