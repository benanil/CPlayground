// Camera-facing billboard grass, ported from the project's Unity URP grass shader.
// Drawn as one indirect multidraw with a command per visible terrain chunk. The grass
// instance stream (fp16 chunk-relative position + chunk slot index) is an instance-rate
// vertex attribute; the quad corner comes from SV_VertexID; the chunk world origin is
// fetched from a storage buffer indexed by the packed chunk slot. Alpha cutout, so it
// renders as opaque geometry into the HDR forward target.

#include "Common.hlsl"
#include "Bitpack.hlsl"

cbuffer vs_params : register(b0, space1)
{
    float4x4 uView;
    float4x4 uProj;
    float4   uCameraTime;  // xyz: camera world pos, w: time seconds (wind phase)
    float4   uGrassParams; // x: view distance, y/z: scale min/max
};

cbuffer ps_params : register(b0, space3)
{
    float4 uSunDirection;      // xyz sun direction
    float4 uGrassColor;        // rgb base tint
    float4 uGrassColorVariant; // rgb variant tint
};

// one float4 per terrain chunk slot: world origin of the chunk in meters (w unused)
struct GrassChunkInfo { float4 origin; };
StructuredBuffer<GrassChunkInfo> Chunks : register(t0);

Texture2DArray<float4> GrassTex     : register(t0, space2);
SamplerState           GrassSampler : register(s0, space2);

struct VSInput
{
    uint2 packed   : TEXCOORD0; // .x = fp16 x | fp16 y, .y = fp16 z | (chunkIndex << 16)
    uint  vertexID : SV_VertexID;
};

struct VSOutput
{
    float4 position  : SV_Position;
    float2 uv        : TEXCOORD0;
    float  occlusion : TEXCOORD1;
    float  texNoise  : TEXCOORD2;
};

float Rand1(float p)  { return frac(sin(p * 12.9898) * 43758.5453); }
float Rand2(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }
float EaseOutCubic(float x) { float f = 1.0 - x; return 1.0 - f * f * f; }

// value noise, replaces the unity gnoise for the two-texture blend variation
float GNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = Rand2(i + float2(0.0, 0.0));
    float b = Rand2(i + float2(1.0, 0.0));
    float c = Rand2(i + float2(0.0, 1.0));
    float d = Rand2(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// the six quad corners (two triangles), local x in [-0.5,0.5], y in [0,1]
static const float2 kCorners[6] = {
    float2(-0.5, 0.0), float2(0.5, 0.0), float2(-0.5, 1.0),
    float2( 0.5, 0.0), float2(0.5, 1.0), float2(-0.5, 1.0)
};

VSOutput vert(VSInput input)
{
    VSOutput o = (VSOutput)0;

    float2 xy = float2(UnpackHalf2(input.packed.x));
    float  z  = float(UnpackHalf(input.packed.y));
    uint chunkIndex = input.packed.y >> 16;
    float3 worldPos = Chunks[chunkIndex].origin.xyz + float3(xy.x, xy.y, z);

    // cull distant grass: collapse the whole billboard off-screen
    float dist = distance(worldPos, uCameraTime.xyz);
    if (dist > uGrassParams.x)
    {
        o.position = float4(2.0, 2.0, 2.0, 1.0);
        return o;
    }

    float2 corner = kCorners[input.vertexID];
    // corner.y is 0 at the root, 1 at the tip. flip V for sampling so the blade art (tip
    // at the top of the image, v=0) draws upright, and keep geometry on corner.y.
    float2 uv = float2(corner.x + 0.5, 1.0 - corner.y);
    float time = uCameraTime.w;
    float rnd = Rand1(worldPos.x + worldPos.z * 1.37);

    // local billboard geometry, adapted from the unity BillboardVertex
    float2 local = corner;
    local.y += step(0.55, corner.y) * Rand1(worldPos.x) * 0.23;   // tip jitter
    float scaleMul = lerp(uGrassParams.y, uGrassParams.z, EaseOutCubic(rnd));
    local *= scaleMul;
    local.x += corner.y * sin(time + worldPos.x + sin(worldPos.z + 123.33)) * 0.1; // wind sway, tip bends most

    float4 camPos = mul(uView, float4(worldPos, 1.0));
    o.position    = mul(uProj, camPos + float4(local.x, local.y, 0.0, 0.0));
    o.uv          = uv;
    o.occlusion   = corner.y; // 0 root -> darker, 1 tip -> lit
    o.texNoise    = GNoise(worldPos.xz * 0.2);
    return o;
}

// samples the two blade atlases, cuts out on the real alpha channel, tints the blade
float4 SampleGrassColor(float2 uv, float texNoise, out float cutout)
{
    float4 tex0 = GrassTex.Sample(GrassSampler, float3(uv, 0.0));
    float4 tex1 = GrassTex.Sample(GrassSampler, float3(uv, 1.0));
    float4 color = lerp(tex0, tex1, step(0.6, texNoise));
    cutout = color.a - 0.5; // discard the transparent (alpha 0) background pixels
    float3 tint = lerp(uGrassColor.rgb, uGrassColorVariant.rgb, smoothstep(0.6, 0.7, texNoise));
    color.rgb *= tint; // modulate the blade texture with the grass tint (#495727 / #a46d48)
    return color;
}

float4 frag(VSOutput i) : SV_Target0
{
    float cutout;
    float4 color = SampleGrassColor(i.uv, i.texNoise, cutout);
    clip(cutout);

    float NdotL = clamp(-uSunDirection.y, 0.2, 1.0);
    float3 ambient = float3(0.35, 0.42, 0.50);
    float3 diffuse = float3(1.0, 0.98, 0.90) * NdotL;
    float3 lit = color.rgb * (ambient + diffuse);
    lit *= max(EaseOutCubic(i.occlusion), 0.35); // gently darken toward the root

    return float4(lit, 1.0);
}
