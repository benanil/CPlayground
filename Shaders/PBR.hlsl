#ifndef PBR_HLSL
#define PBR_HLSL

#include "Math.hlsl"

#ifndef PBR_DEBUG_OUTPUT
    #define PBR_DEBUG_OUTPUT 0
#endif

float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float PerceptualRoughnessToRoughness(float perceptualRoughness)
{
    return perceptualRoughness * perceptualRoughness;
}

float RoughnessToPerceptualRoughness(float roughness)
{
    return sqrt(roughness);
}

#define SPECULAR_AA_VARIANCE  0.15f
#define SPECULAR_AA_THRESHOLD 0.25f

float SpecularAntiAliasing(float perceptualRoughness, float3 normalDx, float3 normalDy)
{
    float variance = SPECULAR_AA_VARIANCE * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
    float roughness = PerceptualRoughnessToRoughness(perceptualRoughness);
    float kernelRoughness = min(2.0f * variance, SPECULAR_AA_THRESHOLD);
    float squareRoughness = saturate(roughness * roughness + kernelRoughness);
    return RoughnessToPerceptualRoughness(sqrt(squareRoughness));
}

float D_GGX(float roughness, float NoH, float3 n, float3 h)
{
    float3 NxH = cross(n, h);
    float oneMinusNoHSquared = dot(NxH, NxH);
    float a = NoH * roughness;
    float k = min(roughness / max(oneMinusNoHSquared + a * a, 0.0000077f), 453.5f);
    return k * k * MATH_OneDivPI;
}

float V_SmithGGXCorrelated(float roughness, float NoV, float NoL)
{
    float a2 = roughness * roughness;
    float lambdaV = NoL * sqrt((NoV - a2 * NoV) * NoV + a2);
    float lambdaL = NoV * sqrt((NoL - a2 * NoL) * NoL + a2);
    return 0.5f / max(lambdaV + lambdaL, 0.0000077f);
}

float V_SmithGGXCorrelatedFast(float roughness, float NoV, float NoL)
{
    return 0.5f / max(lerp(2.0f * NoL * NoV, NoL + NoV, roughness), 0.0000077f);
}

float3 F_Schlick(float3 f0, float f90, float VoH)
{
    return f0 + (f90 - f0) * Pow5(1.0f - VoH);
}

float Distribution(float roughness, float NoH, float3 n, float3 h)
{
    return D_GGX(roughness, NoH, n, h);
}

float Visibility(float roughness, float NoV, float NoL)
{
    return V_SmithGGXCorrelatedFast(roughness, NoV, NoL);
}

float3 Fresnel(float3 f0, float LoH)
{
    float f90 = saturate(dot(f0, float3(16.5f, 16.5f, 16.5f)));
    return F_Schlick(f0, f90, LoH);
}

float Diffuse(float roughness, float NoV, float NoL, float LoH)
{
    return MATH_OneDivPI;
}

float3 DecodeNormalRG(float2 normalRG)
{
    float2 xy = normalRG * 2.0f - 1.0f;
    float z = sqrt(saturate(1.0f - dot(xy, xy)));
    return normalize(float3(xy, z));
}

float3 ApplyPBRLight(float3 albedo, float3 normal, float3 viewDir, float metallic, float perceptualRoughness, float3 radiance, float3 lightDir);

float3 ApplyPBR(float3 albedo, float3 normal, float3 viewDir, float metallic, float perceptualRoughness, float shadow, float ao, float3 lightDir, float ambientBoost = 1.0f)
{
    normal    = normalize(normal);

    #if PBR_DEBUG_OUTPUT == 1
        return normal;
    #elif PBR_DEBUG_OUTPUT == 2
        return albedo;
    #elif PBR_DEBUG_OUTPUT == 3
        return float3(metallic, perceptualRoughness, 0.0f);
    #endif

    lightDir = normalize(lightDir);
    float3 radiance = float3(3.0f, 2.9f, 2.7f) * 4.0f;

    // Shadow sampling keeps a 0.2 floor for non-direct ambient light. Remove that floor
    // before applying visibility to direct BRDF terms, otherwise blocked specular leaks.
    float directShadow = saturate((shadow - 0.2f) * 1.25f);
    float3 direct = ApplyPBRLight(albedo, normal, viewDir, metallic, perceptualRoughness, radiance, lightDir) * directShadow;
	float3 ambient = albedo * 0.10f * saturate(ao) * ambientBoost;
    return ambient + direct;
}

float3 ApplyPBRLight(float3 albedo, float3 normal, float3 viewDir, float metallic, float perceptualRoughness, float3 radiance, float3 lightDir)
{
    // metallic = 0.0f;
    normal    = normalize(normal);
    viewDir   = normalize(viewDir);
    lightDir  = normalize(lightDir);
    // Double-sided foliage can render its back face with the original sun-facing normal.
    // Orient both sides toward the viewer so backfaces do not receive lighting from behind.
    if (dot(normal, viewDir) < 0.0f)
        normal = -normal;
    perceptualRoughness = clamp(perceptualRoughness, 0.045f, 1.0f);
    float roughness = PerceptualRoughnessToRoughness(perceptualRoughness);
    metallic  = saturate(metallic);

    float3 halfVec  = normalize(viewDir + lightDir);

    float NdotL = saturate(dot(normal, lightDir));
    float NdotV = saturate(dot(normal, viewDir));
    float NdotH = saturate(dot(normal, halfVec));
    float HdotV = saturate(dot(halfVec, viewDir));

    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 F = Fresnel(f0, HdotV);
    float D = Distribution(roughness, NdotH, normal, halfVec);
    float V = Visibility(roughness, NdotV, NdotL);
    float diffuse = Diffuse(roughness, NdotV, NdotL, HdotV);

    float3 specular = D * V * F;
    float3 diffuseColor = (1.0f - F) * (1.0f - metallic) * albedo * diffuse;
    return (diffuseColor + specular) * radiance * NdotL;
}

#endif
