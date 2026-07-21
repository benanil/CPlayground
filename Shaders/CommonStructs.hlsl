#ifndef COMMON_STRUCTS
#define COMMON_STRUCTS

#include "Common.hlsl"
#include "../Include/RenderLimits.h"

#define ENTITY_FLAG_NOMESH 1u

typedef struct IndexedDrawCommand_
{
    uint numIndices;
    uint numInstances;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
} IndexedDrawCommand;

typedef struct IndirectDrawCommand_
{
    uint numVertices;  
    uint numInstances; 
    uint firstVertex;  
    uint firstInstance;
} IndirectDrawCommand;

typedef struct IndirectDispatchCommand_
{
    uint groupCountX;
    uint groupCountY;
    uint groupCountZ;
} IndirectDispatchCommand;

typedef struct Entity_
{
    float4 position;
    uint2  rotation;
    uint2  scale;
    uint   primitiveIdx; // primitive group
    uint   sparse;
    uint   parentIdx; // sparseIdx
    uint   hiddenBitAndAmbient;
} Entity;

typedef struct PrimitiveGroup_
{
    uint4 aabbMinEntity;
    uint4 aabbMaxMaterial;
} PrimitiveGroup;

typedef struct PrimitiveGroupLOD_
{
    uint4 lodIndexOffset;
    uint4 lodNumIndices;
    uint4 lodVertexOffset;
    uint4 lodNumVertices;
} PrimitiveGroupLOD;

uint PrimitiveGroup_EntityOffset(PrimitiveGroup group)
{
    return group.aabbMinEntity.w;
}

uint PrimitiveGroup_NumEntities(PrimitiveGroup group)
{
    return group.aabbMaxMaterial.w >> 16u;
}

uint PrimitiveGroup_MaterialIndex(PrimitiveGroup group)
{
    return group.aabbMaxMaterial.w & 0xFFFFu;
}

float3 PrimitiveGroup_AABBMin(PrimitiveGroup group)
{
    return asfloat(group.aabbMinEntity.xyz);
}

float3 PrimitiveGroup_AABBMax(PrimitiveGroup group)
{
    return asfloat(group.aabbMaxMaterial.xyz);
}

// Animated vertex cache format, 8 bytes per vertex (position only).
// packed0/packed1: bounds-normalized model-space skinned position, 16/16/16 unorm against the
// primitive group's (whole-skin) AABB. Entity rotation/scale are applied per vertex shader.
typedef struct AnimatedVert_
{
    uint packed0;
    uint packed1;
} AnimatedVert;

typedef struct TextureDescriptor_
{
    float2 uvScale;
    float2 uvBias;
    uint pageIndex;
    uint flags;
    uint2 padding;
} TextureDescriptor;

#define MATERIAL_FLAG_ALPHA_MASK       (1u << 0)
#define MATERIAL_ALPHA_CUTOFF_SHIFT    8u
#define MATERIAL_ALPHA_CUTOFF_MASK     (0xffu << MATERIAL_ALPHA_CUTOFF_SHIFT)

bool MaterialIsAlphaMasked(u16 flags)
{
    return (flags & MATERIAL_FLAG_ALPHA_MASK) != 0u;
}

float MaterialAlphaCutoff(u16 flags)
{
    return float((flags & MATERIAL_ALPHA_CUTOFF_MASK) >> MATERIAL_ALPHA_CUTOFF_SHIFT) * (1.0f / 255.0f);
}

float MaterialBaseAlpha(uint baseColorFactor)
{
    return float(baseColorFactor >> 24u) * (1.0f / 255.0f);
}

typedef struct MaterialGPU_
{
    uint AlbedoNormalDescriptor;
    uint metallicRoughnessDescriptorAndFlags;
    uint baseColorFactor;
    uint metallicRoughnessFactor;
} MaterialGPU;

void AlphaClipMaterial(MaterialGPU material, float albedoAlpha)
{
	u16 flags = (u16)(material.metallicRoughnessDescriptorAndFlags >> 16);
    if (MaterialIsAlphaMasked(flags) && albedoAlpha * MaterialBaseAlpha(material.baseColorFactor) < MaterialAlphaCutoff(flags))
        discard;
}

typedef struct LineVertex_
{
    float x, y, z;
    uint color;
} LineVertex;

#define LIGHT_TYPE_POINT 0u
#define LIGHT_TYPE_SPOT  1u
#define LIGHT_TYPE_RECT  2u

#define LIGHT_FLAG_SHADOWED 1u
#define LIGHT_SHADOW_INDEX_INVALID 0xffu

#define LIGHT_DRAW_FULLSCREEN 1u

typedef struct LightGPU_
{
    float4 positionRadius;
    f16_4 directionCone;
    uint colorShadow;        // r8 | g8 << 8 | b8 << 16 | shadowIndex << 24
    uint intensityTypeFlags; // intensity f16 | type << 16 | flags << 24
} LightGPU;

float3 LightGPU_Color(LightGPU light)
{
    return float3(light.colorShadow & 0xffu,
                  (light.colorShadow >> 8u) & 0xffu,
                  (light.colorShadow >> 16u) & 0xffu) * (1.0f / 255.0f);
}

uint LightGPU_ShadowIndex(LightGPU light)
{
    return (light.colorShadow >> 24u) & 0xffu;
}

float LightGPU_Intensity(LightGPU light)
{
    return f16tof32(light.intensityTypeFlags & 0xffffu);
}

uint LightGPU_Type(LightGPU light)
{
    return (light.intensityTypeFlags >> 16u) & 0xffu;
}

uint LightGPU_Flags(LightGPU light)
{
    return (light.intensityTypeFlags >> 24u) & 0xffu;
}

#endif
