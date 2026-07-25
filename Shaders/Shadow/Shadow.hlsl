#ifndef SHADOW_HLSL
#define SHADOW_HLSL

static const float2 ShadowKernel[32] =
{
    float2(0.382143, -0.750672),
    float2(-0.699118, 0.417980),
    float2(0.017911, 0.530034),
    float2(0.461916, -0.497940),
    float2(0.138959, -0.753612),
    float2(-0.619547, 0.523433),
    float2(-0.403566, 0.251649),
    float2(0.501719, -0.631005),
    float2(0.409324, 0.174980),
    float2(-0.649627, -0.377275),
    float2(0.134689, -0.450591),
    float2(-0.686986, 0.133178),
    float2(-0.089575, -0.083734),
    float2(0.007943, 0.826362),
    float2(-0.057125, -0.329970),
    float2(0.078787, -0.175278),
    float2(-0.772931, 0.384923),
    float2(0.551949, -0.669406),
    float2(-0.087713, 0.962403),
    float2(0.094594, -0.576342),
    float2(-0.362402, -0.462390),
    float2(0.714690, -0.311474),
    float2(0.801766, -0.471645),
    float2(0.069921, 0.802410),
    float2(0.422638, -0.300293),
    float2(-0.774194, -0.435089),
    float2(0.359599, -0.633199),
    float2(-0.721211, -0.323427),
    float2(0.242559, 0.850162),
    float2(-0.737375, -0.288707),
    float2(-0.865741, 0.433705),
    float2(-0.191099, -0.707353)
};

struct ShadowCascadeBuffer {
    float4 lightViewProj[12];
    float4 splitDistances;
};

struct PointShadowMatrix {
    float4 lightViewProj[4];
};

float4 MulShadowCascade(ShadowCascadeBuffer cascades, uint cascadeIndex, float4 p)
{
    uint row = cascadeIndex * 4u;
    return float4(dot(p, cascades.lightViewProj[row + 0u]),
                  dot(p, cascades.lightViewProj[row + 1u]),
                  dot(p, cascades.lightViewProj[row + 2u]),
                  dot(p, cascades.lightViewProj[row + 3u]));
}

float4 MulPointShadowSide(PointShadowMatrix side, float4 p)
{
    return float4(dot(p, side.lightViewProj[0u]),
                  dot(p, side.lightViewProj[1u]),
                  dot(p, side.lightViewProj[2u]),
                  dot(p, side.lightViewProj[3u]));
}

float SampleShadow(Texture2D<float> shadowMap, SamplerState samp,
                   float4 shadowPos, uint cascadeIndex,
                   float3 normal, float3 lightDir)
{
    float3 proj  = shadowPos.xyz / shadowPos.w;
    float2 uv    = proj.xy * float2(0.5f, -0.5f) + 0.5f;
    float  depth = proj.z;
    if (shadowPos.w <= 0.0f || any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
        return 1.0f;

    uint width, height;
    shadowMap.GetDimensions(width, height);
    float cascadeSize = float(max(width >> cascadeIndex, 1u));
    float2 texel = 1.0f / float2(cascadeSize, cascadeSize);

    float cascadeBiasScale = 1.0 + (cascadeIndex * 0.15);
    float ndotl = saturate(dot(normal, lightDir));
    float bias  = max(0.0009f * (1.0f - ndotl), 0.00025f) * cascadeBiasScale;

    int kernelSize = 16 - (cascadeIndex * 4);
    float shadow = 0.0f;

    [unroll]
    for (int i = 0; i < 16; i++)
    {
        if (i >= kernelSize) break;
        float2 sampleUV = uv + ShadowKernel[i] * texel * (1.0f + float(i >> 3));
        float  mapDepth = shadowMap.SampleLevel(samp, sampleUV, float(cascadeIndex));
        shadow += float(mapDepth >= (depth - bias));
    }
    return max(shadow / float(kernelSize), 0.3f);
}

#endif