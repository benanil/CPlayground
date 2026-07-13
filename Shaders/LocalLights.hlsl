#ifndef LOCAL_LIGHTS_HLSL
#define LOCAL_LIGHTS_HLSL

// Shared Forward+ local-light shading. This is meant to be called inside a per-pixel
// loop over a tile's light list, so the out-of-range path returns 0.
//
// The including shader MUST declare these globals (at whatever registers fit its layout)
// before including this file:
//
//   Texture2DArray<float>               PointShadowTexture;
//   Texture2DArray<float>               SpotShadowTexture;
//   SamplerState                        PointShadowSampler;
//   SamplerState                        SpotShadowSampler;
//   StructuredBuffer<PointShadowMatrix> PointShadowMatrices;
//   StructuredBuffer<PointShadowMatrix> SpotShadowMatrices;
//   StructuredBuffer<LightGPU>          sLights;
//   StructuredBuffer<uint2>             sLightGrid;   // per tile {offset, count}
//   StructuredBuffer<uint>              sLightIndex;  // flat light index list

#include "PBR.hlsl"
#include "CommonStructs.hlsl"
#include "Shadow/Shadow.hlsl"

uint LocalLights_PointShadowFace(float3 lightToWorld)
{
    float3 a = abs(lightToWorld);
    if (a.x >= a.y && a.x >= a.z)
        return lightToWorld.x >= 0.0f ? 0u : 1u;
    if (a.y >= a.z)
        return lightToWorld.y >= 0.0f ? 2u : 3u;
    return lightToWorld.z >= 0.0f ? 4u : 5u;
}

float LocalLights_SamplePointShadow(LightGPU light, float3 worldPos, float3 normal, float3 lightDir)
{
    uint shadowIndex = LightGPU_ShadowIndex(light);
    if ((LightGPU_Flags(light) & LIGHT_FLAG_SHADOWED) == 0u || shadowIndex >= POINT_SHADOW_MAX_LIGHTS)
        return 1.0f;

    float ndotl = saturate(dot(normal, lightDir));
    float3 biasedWorldPos = worldPos + (normal * (0.03f * (1.0f - ndotl)));

    // Use the new biasedWorldPos instead of the original worldPos for all matrix math
    float3 lightToWorld = biasedWorldPos - light.positionRadius.xyz;
    uint face = LocalLights_PointShadowFace(lightToWorld);
    uint matrixIndex = shadowIndex * POINT_SHADOW_FACE_COUNT + face;
    float4 shadowPos = MulPointShadowSide(PointShadowMatrices[matrixIndex], float4(biasedWorldPos, 1.0f));
    
    float3 proj = shadowPos.xyz / max(abs(shadowPos.w), 0.00001f);
    float2 uv = proj.xy * float2(0.5f, -0.5f) + 0.5f;
    float depth = proj.z;
    
    if (shadowPos.w <= 0.0f || any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
        return 1.0f;

    uint width, height, layers;
    PointShadowTexture.GetDimensions(width, height, layers);
    if (shadowIndex >= layers)
        return 1.0f;

    float bias = max(0.0035f * (1.0f - ndotl), 0.0008f);
    uint faceWidth = max(width / POINT_SHADOW_FACE_COUNT, 1u);
    float2 texel = 1.0f / float2(faceWidth, max(height, 1u));
    float shadow = 0.0f;

    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float2 sampleUV = clamp(uv + ShadowKernel[i] * texel * 1.5f, texel * 0.5f, 1.0f - texel * 0.5f);
        float2 atlasUV = float2((sampleUV.x + float(face)) / float(POINT_SHADOW_FACE_COUNT), sampleUV.y);
        float mapDepth = PointShadowTexture.SampleLevel(PointShadowSampler, float3(atlasUV, float(shadowIndex)), 0.0f);
        shadow += float(mapDepth >= depth - bias);
    }
    return max(shadow * 0.125f, 0.15f);
}

float LocalLights_SampleSpotShadow(LightGPU light, float3 worldPos, float3 normal, float3 lightDir)
{
    uint shadowIndex = LightGPU_ShadowIndex(light);
    if ((LightGPU_Flags(light) & LIGHT_FLAG_SHADOWED) == 0u || shadowIndex >= SPOT_SHADOW_MAX_LIGHTS)
        return 1.0f;

    uint width, height, layers;
    SpotShadowTexture.GetDimensions(width, height, layers);
    if (shadowIndex >= layers)
        return 1.0f;

    float4 shadowPos = MulPointShadowSide(SpotShadowMatrices[shadowIndex], float4(worldPos, 1.0f));
    float3 proj = shadowPos.xyz / max(abs(shadowPos.w), 0.00001f);
    float2 uv = proj.xy * float2(0.5f, -0.5f) + 0.5f;
    float depth = proj.z;
    if (shadowPos.w <= 0.0f || any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
        return 1.0f;

    float ndotl = saturate(dot(normal, lightDir));
    float bias = max(0.003f * (1.0f - ndotl), 0.0005f);
    float2 texel = 1.0f / float2(max(width, 1u), max(height, 1u));
    float shadow = 0.0f;

    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float2 sampleUV = uv + ShadowKernel[i] * texel * 1.25f;
        float mapDepth = SpotShadowTexture.SampleLevel(SpotShadowSampler, float3(sampleUV, float(shadowIndex)), 0.0f);
        shadow += float(mapDepth >= depth - bias);
    }
    return max(shadow * (1.0f / 8.0f), 0.15f);
}

// discard-free local light: returns 0 contribution when the surface is outside the
// light's radius so it is safe to call in a loop.
float3 ApplyLocalLightForward(float3 albedo, float3 normal, float3 viewDir, float metallic,
                              float perceptualRoughness, LightGPU light, float3 worldPos, float ao)
{
    float3 lightVector = light.positionRadius.xyz - worldPos;
    float distanceSq = dot(lightVector, lightVector);
    float radius = max(light.positionRadius.w, 0.001f);
    if (distanceSq >= radius * radius)
        return float3(0.0f, 0.0f, 0.0f);

    float distanceToLight = sqrt(max(distanceSq, 0.00001f));
    float3 lightDir = lightVector / distanceToLight;
    float attenuation = saturate(1.0f - distanceToLight / radius);
    attenuation *= attenuation;

    uint lightType = LightGPU_Type(light);
    if (lightType == LIGHT_TYPE_SPOT)
    {
        float coneCos = float(light.directionCone.w);
        float spotCos = dot(normalize(-float3(light.directionCone.xyz)), lightDir);
        float spot = saturate((spotCos - coneCos) / max(1.0f - coneCos, 0.0001f));
        attenuation *= spot * spot;
    }

    float shadow = 1.0f;
    if (lightType == LIGHT_TYPE_POINT)
        shadow = LocalLights_SamplePointShadow(light, worldPos, normal, lightDir);
    else if (lightType == LIGHT_TYPE_SPOT)
        shadow = LocalLights_SampleSpotShadow(light, worldPos, normal, lightDir);
    float3 radiance = LightGPU_Color(light) * LightGPU_Intensity(light) * attenuation * shadow;
    return ApplyPBRLight(albedo, normal, viewDir, metallic, perceptualRoughness, radiance, lightDir) * saturate(ao);
}

// Accumulate every light binned into this pixel's screen tile.
float3 AccumulateTileLights(float3 albedo, float3 normal, float3 viewDir, float metallic,
                            float perceptualRoughness, float3 worldPos, float ao,
                            uint2 pixel, uint tilesX, uint tileSize)
{
    uint2 tile = pixel / tileSize;
    uint tileIndex = tile.y * tilesX + tile.x;
    uint2 cell = sLightGrid[tileIndex]; // x = offset into sLightIndex, y = light count
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0u; i < cell.y; i++)
    {
        uint lightIndex = sLightIndex[cell.x + i];
        LightGPU light = sLights[lightIndex];
        sum += ApplyLocalLightForward(albedo, normal, viewDir, metallic, perceptualRoughness, light, worldPos, ao);
    }
    return sum;
}

#endif
