#include "TerrainInternal.h"
#include "Include/Terrain.h"

#define TV_GRASS_LAYER 0u
#define TV_DIRT_LAYER  1u
#define TV_ROCK_LAYER  2u
#define TV_SAND_LAYER  3u
#define TV_SAND_TOP    2.0f
#define TV_SAND_FADE   2.0f
#define TV_GRASS_START 3.0f
#define TV_GRASS_FADE  8.0f

static void CalculateProceduralMaterial(float3 worldPos, float3 normal, f32 weights[4], f32* seaLevel)
{
    const TerrainGenParams* params = Terrain_GetGenParams();
    *seaLevel = params ? params->seaLevel : 0.0f;

    f32 slope = normal.y;
    f32 steepRock = 1.0f - Saturatef32((slope - 0.55f) * 4.0f);
    f32 highBlend = Saturatef32((worldPos.y - 34.0f) * 0.08f);
    f32 rockBlend = Maxf32(steepRock, highBlend);
    f32 grassHeight = Saturatef32((worldPos.y - *seaLevel - TV_GRASS_START) * (1.0f / TV_GRASS_FADE));
    f32 grassAmount = (1.0f - rockBlend) * grassHeight;

    weights[TV_GRASS_LAYER] = grassAmount * (1.0f - rockBlend);
    weights[TV_DIRT_LAYER] = (1.0f - grassAmount) * (1.0f - rockBlend);
    weights[TV_ROCK_LAYER] = rockBlend;
    weights[TV_SAND_LAYER] = 0.0f;
}

static MaterialBlend SelectDominantLayers(const f32 weights[4])
{
    u32 primary = TV_GRASS_LAYER;
    u32 secondary = TV_DIRT_LAYER;
    for (u32 layer = TV_DIRT_LAYER; layer <= TV_ROCK_LAYER; layer++)
    {
        if (weights[layer] > weights[primary])
        {
            secondary = primary;
            primary = layer;
        }
        else if (layer != primary && weights[layer] > weights[secondary])
        {
            secondary = layer;
        }
    }

    f32 total = weights[primary] + weights[secondary];
    u32 primaryWeight = (u32)Clampf32(weights[primary] / Maxf32(total, 1.0e-4f) * 255.0f + 0.5f, 0.0f, 255.0f);
    return (MaterialBlend){ (u8)primary, (u8)secondary, (u8)primaryWeight };
}

static void ApplyMaterialOverrides(float3 worldPos, f32 seaLevel, MaterialBlend* blend)
{
    f32 sandBlend = Saturatef32((seaLevel + TV_SAND_TOP - worldPos.y) * (1.0f / TV_SAND_FADE));

    u8 matIndex[2];
    u8 matWeight[2];
    TerrainEdit_MaterialWeights(worldPos, matIndex, matWeight);

    if (matIndex[0] == 0u && matIndex[1] == 0u)
    {
        if (sandBlend > 0.002f)
        {
            blend->primary = TV_SAND_LAYER;
            blend->secondary = TV_DIRT_LAYER;
            blend->primaryWeight = (u8)Clampf32(sandBlend * 255.0f + 0.5f, 0.0f, 255.0f);
        }
    }
    else
    {
        u32 procDominant = blend->primaryWeight >= 128u ? blend->primary : blend->secondary;
        blend->primary = (u8)procDominant;
        if (matIndex[0] != 0u)
            blend->primary = (u8)Minu32((u32)matIndex[0] - 1u, TV_SAND_LAYER);
        blend->secondary = (u8)procDominant;
        if (matIndex[1] != 0u)
            blend->secondary = (u8)Minu32((u32)matIndex[1] - 1u, TV_SAND_LAYER);
        blend->primaryWeight = matWeight[0];
    }
}

void tTerrainMaterial(float3 worldPos, float3 normal, u32* materials, u32* blend)
{
    f32 weights[4];
    f32 seaLevel;
    CalculateProceduralMaterial(worldPos, normal, weights, &seaLevel);
    MaterialBlend material = SelectDominantLayers(weights);
    ApplyMaterialOverrides(worldPos, seaLevel, &material);

    *materials = (u32)material.primary | ((u32)material.secondary << 8);
    *blend = material.primaryWeight;
}
