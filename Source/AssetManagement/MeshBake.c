#include "Include/AssetManager.h"
#include "Include/Graphics.h"
#include "Include/Memory.h"
#include "Include/Platform.h"
#include "Include/Algorithm.h"
#include "Math/Matrix.h"
#include "Math/Color.h"
#include "Math/Bitpack.h"
#include "Extern/meshoptimizer/src/meshoptimizer.h"

extern Graphics gGFX;

static void JointsForPrimitive(APrimitive* primitive, ASkinedVertex* currVertex)
{
    const u8* joints = (const u8*)primitive->vertexAttribs[AAttribIdx_JOINTS];
    s32 jointSize    = GraphicsTypeToSize(primitive->jointType);
    s32 jointCount   = primitive->jointCount;
    s32 jointStride  = primitive->jointStride ? primitive->jointStride : jointSize * jointCount;

    if (joints == NULL || jointSize <= 0 || jointCount <= 0 || jointStride < jointSize * jointCount)
    {
        AX_LOG("no joints in skinned mesh renderer");
        for (s32 j = 0; j < primitive->numVertices; j++)
            currVertex[j].joints = 0;
        return;
    }

    for (s32 j = 0; j < primitive->numVertices; j++)
    {
        // Some accessors are tightly packed and report stride 0; always address vertices by the resolved stride.
        const u8* vertexJoints = joints + (size_t)j * (size_t)jointStride;
        u32 packedJoints = 0u;
        for (s32 k = 0, shift = 0; k < jointCount && k < 4; k++)
        {
            u32 jointIndex = 0;
            SmallMemCpy(&jointIndex, vertexJoints + (size_t)k * (size_t)jointSize, jointSize);
            ASSERT(jointIndex < 255u && "index has to be smaller than 255");
            packedJoints |= jointIndex << shift;
            shift  += 8;
        }
        currVertex[j].joints = packedJoints;
    }
}

static void WeightsForPrimitive(APrimitive* primitive, ASkinedVertex* currVertex)
{
    const u8* weights = (const u8*)primitive->vertexAttribs[AAttribIdx_WEIGHTS];
    s32 weightSize   = GraphicsTypeToSize(primitive->weightType);
    s32 weightCount  = primitive->jointCount;
    s32 weightStride = primitive->weightStride ? primitive->weightStride : weightSize * weightCount;

    if (weights == NULL || weightSize <= 0 || weightCount <= 0 || weightStride < weightSize * weightCount)
    {
        AX_LOG("no joints in primitive");
        for (s32 j = 0; j < primitive->numVertices; j++)
            currVertex[j].weights = 1023;
        return;
    }

    if (weightSize == 4)
    {
        for (s32 j = 0; j < primitive->numVertices; j++)
        {
            // Read from the current vertex, not by advancing a shared pointer; float VEC4 weights are 16 bytes.
            const f32* vertexWeights = (const f32*)(weights + (size_t)j * (size_t)weightStride);
            u32 packedWeights = PackXY11Z10UnormToU32(Vec3Load(vertexWeights));
            if (packedWeights == 0) packedWeights = 1023;
            currVertex[j].weights = packedWeights;
        }
    }
    else
    {
        for (s32 j = 0; j < primitive->numVertices; j++)
        {
            // Integer weight accessors can be byte/ushort and may also be tightly packed.
            const u8* vertexWeights = weights + (size_t)j * (size_t)weightStride;
            u32 packedWeights = 0;
            const f32 packMax[3] = { 1023.0f, 1023.0f, 511.0f };
            for (s32 k = 0, shift = 0; k < weightCount && k < 3; k++, shift += 11)
            {
                u32 jointWeight = 0u;
                SmallMemCpy(&jointWeight, vertexWeights + (size_t)k * (size_t)weightSize, weightSize);
                f32 weightMax = (f32)((1u << (weightSize * 8)) - 1);
                f32 norm = (f32)jointWeight / weightMax;
                packedWeights |= (u32)(norm * packMax[k]) << shift;
            }
            if (packedWeights == 0) packedWeights = 0XFF000000u;
            currVertex[j].weights = packedWeights;
        }
    }
}

static void IndicesForPrimitive(APrimitive* primitive, u32* currIndices, const u32 vertexCursor)
{
    if (primitive->indices == NULL)
    {
        primitive->indices = currIndices;
        u32* indices = (u32*)primitive->indices;
        for (s32 i = 0; i < primitive->numIndices; i++)
            indices[i] = vertexCursor + (u32)i;
        return;
    }

    const u8* beforeCopy = (const u8*)primitive->indices;
    primitive->indices = currIndices;
    if (primitive->indexType < AComponentType_BYTE || primitive->indexType > AComponentType_FLOAT)
    {
        AX_WARN("primitive index type invalid: %d", primitive->indexType);
        primitive->numIndices = 0;
        return;
    }
    s32 indexSize = GraphicsTypeToSize(primitive->indexType);

    for (s32 i = 0; i < primitive->numIndices; i++)
    {
        u32 index = 0;
        SmallMemCpy(&index, beforeCopy, indexSize);
        currIndices[i] = index + vertexCursor;
        beforeCopy += indexSize;
    }
}

static void VerticesForPrimitive(APrimitive* primitive, ASkinedVertex* currVertex)
{
    primitive->vertices = currVertex;
    const float3* positions  = (const float3*)primitive->vertexAttribs[AAttribIdx_POSITION];
    const float2* texCoords  = (const float2*)primitive->vertexAttribs[AAttribIdx_TEXCOORD_0];
    const float3* normals    = (const float3*)primitive->vertexAttribs[AAttribIdx_NORMAL];
    const v128f* tangents    = (const v128f*)primitive->vertexAttribs[AAttribIdx_TANGENT];

    for (s32 v = 0; v < primitive->numVertices; v++)
    {
        v128f tangent = tangents  ? tangents[v]  : VecZero();
        float2 texCoord  = texCoords ? texCoords[v] : (float2){0.0f, 0.0f};
        float3 normal    = normals   ? normals[v]   : (float3){0.5f, 0.5f, 0.0};

        Float4ToHalf4((f16*)&currVertex[v].positionXY, &positions[v].x);
        currVertex[v].texCoord = Float2ToHalf2(&texCoord.x);
        currVertex[v].octTbn = PackNormalTangent(Vec3Load(&normal.x), tangent);
    }
}

static f32 ReadColorChannel(const u8* src, s32 type)
{
    switch (type)
    {
        case AComponentType_BYTE:           return Saturatef32(((f32)(*(const s8*)src) / 127.0f) * 0.5f + 0.5f);
        case AComponentType_UNSIGNED_BYTE:  return (f32)(*(const u8*)src) * (1.0f / 255.0f);
        case AComponentType_SHORT:          return Saturatef32(((f32)(*(const s16*)src) / 32767.0f) * 0.5f + 0.5f);
        case AComponentType_UNSIGNED_SHORT: return (f32)(*(const u16*)src) * (1.0f / 65535.0f);
        case AComponentType_FLOAT:          return Saturatef32(*(const f32*)src);
        default:                            return 1.0f;
    }
}

static u16 PackVertexColorRGBA4444(const APrimitive* primitive, s32 vertexIndex)
{
    const u8* colors = (const u8*)primitive->vertexAttribs[AAttribIdx_COLOR_0];
    if (!colors || primitive->colorCount < 3) return 0xFFFFu;

    s32 colorType = primitive->colorType;
    s32 componentSize = GraphicsTypeToSize(colorType);
    s32 colorStride = primitive->colorStride ? primitive->colorStride : componentSize * primitive->colorCount;
    const u8* src = colors + (size_t)vertexIndex * (size_t)colorStride;

    u32 rgba[4] = { 31u, 31u, 31u, 1u };
    for (s32 c = 0; c < primitive->colorCount && c < 3; c++)
    {
        f32 channel = ReadColorChannel(src + (size_t)c * (size_t)componentSize, colorType);
        rgba[c] = (u32)(channel * 31.0f + 0.5f) & 0x1Fu;
    }
	// 0:1= 0 -> 0.5 alpha, 1 -> 1.0 alpha
	u16 opacity = primitive->colorCount >= 4 && ReadColorChannel(src + (size_t)3 * (size_t)componentSize, colorType) > 0.5f;
    return (u16)(rgba[0] | (rgba[1] << 5u) | (rgba[2] << 10u) | (opacity << 15u));
}

// Requires primitive->min/max to be set first (BoundsForPrimitive). Positions are quantized
// to xyz unorm16 relative to the primitive AABB; the vertex/depth/shadow shaders and the BVH
// de-quantize with the same AABB (PrimitiveGroup.aabbMin/aabbMax == primitive->min/max).
static void SurfaceVerticesForPrimitive(APrimitive* primitive, AVertex* currVertex)
{
    primitive->vertices = currVertex;
    const float3* positions = (const float3*)primitive->vertexAttribs[AAttribIdx_POSITION];
    const float2* texCoords = (const float2*)primitive->vertexAttribs[AAttribIdx_TEXCOORD_0];
    const float3* normals   = (const float3*)primitive->vertexAttribs[AAttribIdx_NORMAL];
    const v128f*  tangents  = (const v128f*)primitive->vertexAttribs[AAttribIdx_TANGENT];

    v128f aabbMin    = VecLoad(primitive->min);
    // guard against a zero-extent axis (planar mesh) so the divide can't produce NaN
    v128f aabbExtent = VecMax(VecSub(VecLoad(primitive->max), aabbMin), VecSet1(1.0e-6f));
    v128f invExtent  = VecDiv(VecSet1(1.0f), aabbExtent);

    for (s32 v = 0; v < primitive->numVertices; v++)
    {
        v128f tangent   = tangents ? tangents[v] : VecZero();
        float2 texCoord = texCoords ? texCoords[v] : (float2){0.0f, 0.0f};
        float3 normal   = normals ? normals[v] : (float3){0.5f, 0.5f, 0.0f};

        v128f unorm = VecMul(VecSub(VecLoad(&positions[v].x), aabbMin), invExtent);
        u64 packedPosition = PackUnorm16x4(unorm); // PackUnorm16x4 clamps to [0,1]
        currVertex[v].position = packedPosition | ~0x0000FFFFFFFFFFFFull; // make vertex color white
        currVertex[v].texCoord = Float2ToHalf2(&texCoord.x);
        currVertex[v].octTbn = PackNormalTangent(Vec3Load(&normal.x), tangent);
    }
	
	const u8* colors = (const u8*)primitive->vertexAttribs[AAttribIdx_COLOR_0];
    if (!colors || primitive->colorCount < 3) return;

	for (s32 v = 0; v < primitive->numVertices; v++)
	{
        currVertex[v].position &= 0x0000FFFFFFFFFFFFull; // clear vertex color
		currVertex[v].position |= (u64)PackVertexColorRGBA4444(primitive, v) << 48u;
	}
}

static void BoundsForPrimitive(APrimitive* primitive)
{
    const float3* positions = (const float3*)primitive->vertexAttribs[AAttribIdx_POSITION];
    v128f min = VecSet1(FLT_MAX);
    v128f max = VecNeg(min);
    for (s32 i = 0; i < primitive->numVertices; i++)
    {
        v128f v = VecLoad(&positions[i].x);
        min = VecMin(min, v);
        max = VecMax(max, v);
    }
    VecStore(primitive->min, min);
    VecStore(primitive->max, max);
}

static void SetPrimitiveLODFallback(APrimitive* primitive, u32 vertexOffset)
{
    for (u32 lod = 0; lod < ARRAY_SIZE(primitive->lodIndexOffset); lod++)
    {
        primitive->lodIndexOffset[lod] = primitive->indexOffset;
        primitive->lodNumIndices[lod]  = primitive->numIndices;
        primitive->lodVertexOffset[lod] = (int)vertexOffset;
        primitive->lodNumVertices[lod] = primitive->numVertices;
    }
}

// Reorder a primitive's base (LOD0) triangles for the post-transform vertex cache. This only
// permutes triangles within the primitive's own index range, so vertex order, offsets, bounds,
// generated LODs and the BVH triangle set are all unaffected. Indices in the mega-buffer are
// global (vertexBase-relative), so localize before meshopt and re-add the base afterwards.
static void OptimizePrimitiveVertexCache(u32* indices, s32 numIndices, s32 numVertices, u32 vertexBase)
{
    if (numIndices < 3 || numVertices <= 0)
        return;

    u32* localIndices = (u32*)ArenaPushGlobal((u64)numIndices * sizeof(u32));
    for (s32 i = 0; i < numIndices; i++)
        localIndices[i] = indices[i] - vertexBase;

    meshopt_optimizeVertexCache(localIndices, localIndices, (size_t)numIndices, (size_t)numVertices);

    for (s32 i = 0; i < numIndices; i++)
        indices[i] = localIndices[i] + vertexBase;

    ArenaPopGlobal((u64)numIndices * sizeof(u32));
}

static u32 RoundTriangleIndexCount(size_t count)
{
    count -= count % 3u;
    if (count < 3u) count = 3u;
    return (u32)count;
}

static void BuildSkinSimplifyAttributes(const ASkinedVertex* vertices, f32* attributes, size_t numVertices)
{
    for (u32 v = 0; v < (u32)numVertices; v++)
    {
        u32 joints = vertices[v].joints;
        u32 weights = vertices[v].weights;
        f32* attr = attributes + (size_t)v * 7u;

        attr[0] = (f32)(joints & 0xFFu);
        attr[1] = (f32)((joints >> 8u) & 0xFFu);
        attr[2] = (f32)((joints >> 16u) & 0xFFu);
        attr[3] = (f32)((joints >> 24u) & 0xFFu);
        attr[4] = (f32)(weights & 0x7FFu) * (1.0f / 2047.0f);
        attr[5] = (f32)((weights >> 11u) & 0x7FFu) * (1.0f / 2047.0f);
        attr[6] = (f32)((weights >> 22u) & 0x3FFu) * (1.0f / 1023.0f);
    }
}

static void GenerateStaticLODsForPrimitive(APrimitive* primitive, const u32* globalIndices, u32 vertexBase,
                                           u32** lodWrite, u32* lodIndexCursor, u32 indexRangeEnd, f32 lodBudgetScale)
{
    const float3* positions = (const float3*)primitive->vertexAttribs[AAttribIdx_POSITION];
    if (!positions || primitive->numVertices <= 0 || primitive->numIndices < 3)
    {
        AX_WARN("static lod generation skipped: invalid primitive vertices=%d indices=%d", primitive->numVertices, primitive->numIndices);
        return;
    }

    const size_t numIndices = (size_t)primitive->numIndices;
    u32* localIndices = (u32*)ArenaPushGlobal(numIndices * sizeof(u32));
    u32* simplified   = (u32*)ArenaPushGlobal(numIndices * sizeof(u32));
    for (u32 i = 0; i < (u32)numIndices; i++)
        localIndices[i] = globalIndices[i] - vertexBase;

    const float baseTargets[MESH_LOD_COUNT] = { 1.0f, 0.50f, 0.25f };
    const float errors[MESH_LOD_COUNT]  = { 0.0f, 0.20f, 0.50f };
    for (u32 lod = 1; lod < MESH_LOD_COUNT; lod++)
    {
        f32 target = Maxf32(baseTargets[lod] * lodBudgetScale, 0.04f);
        u32 targetIndexCount = RoundTriangleIndexCount((size_t)((f32)numIndices * target));
        f32 resultError = 0.0f;
        size_t simplifiedCount = meshopt_simplify(simplified, localIndices, numIndices,
                                                  &positions[0].x, (size_t)primitive->numVertices, sizeof(float3),
                                                  targetIndexCount, errors[lod], 0, &resultError);
        if (simplifiedCount < 3u)
        {
            AX_WARN("static lod generation failed: lod=%d vertices=%d indices=%d", lod, primitive->numVertices, primitive->numIndices);
            continue;
        }

        // overrunning the bundle's allocation would corrupt a neighboring live bundle
        if (*lodIndexCursor + (u32)simplifiedCount > indexRangeEnd)
        {
            AX_WARN("static lod generation skipped: bundle index range exceeded lod=%d indices=%d/%d", lod,
                    *lodIndexCursor + (u32)simplifiedCount, indexRangeEnd);
            continue;
        }

        primitive->lodIndexOffset[lod] = (int)*lodIndexCursor;
        primitive->lodNumIndices[lod]  = (int)simplifiedCount;
        for (u32 i = 0; i < (u32)simplifiedCount; i++)
            (*lodWrite)[i] = simplified[i] + vertexBase;

        *lodWrite += simplifiedCount;
        *lodIndexCursor += (u32)simplifiedCount;
    }

    ArenaPopGlobal(numIndices * sizeof(u32));
    ArenaPopGlobal(numIndices * sizeof(u32));
}

static void GenerateSkinnedLODsForPrimitive(APrimitive* primitive, const u32* globalIndices, u32 vertexBase,
                                            ASkinedVertex** vertexWrite, u32* vertexCursor, u32 vertexRangeEnd,
                                            u32** lodWrite, u32* lodIndexCursor,
                                            u32 indexRangeEnd, f32 lodBudgetScale)
{
    const float3* positions = (const float3*)primitive->vertexAttribs[AAttribIdx_POSITION];
    if (!positions || primitive->numVertices <= 0 || primitive->numIndices < 3)
    {
        AX_WARN("skinned lod generation skipped: invalid primitive vertices=%d indices=%d", primitive->numVertices, primitive->numIndices);
        return;
    }

    const size_t numIndices = (size_t)primitive->numIndices;
    const size_t numVertices = (size_t)primitive->numVertices;
    u32* localIndices   = (u32*)ArenaPushGlobal(numIndices * sizeof(u32));
    u32* simplified     = (u32*)ArenaPushGlobal(numIndices * sizeof(u32));
    u32* vertexRemap    = (u32*)ArenaPushGlobal(numVertices * sizeof(u32));
    f32* skinAttributes = (f32*)ArenaPushGlobal(numVertices * 7u * sizeof(f32));
    for (u32 i = 0; i < (u32)numIndices; i++)
        localIndices[i] = globalIndices[i] - vertexBase;

    ASkinedVertex* sourceVertices = gGFX.SkinnedVertexBuffer + vertexBase;
    BuildSkinSimplifyAttributes(sourceVertices, skinAttributes, numVertices);

    const float baseTargets[MESH_LOD_COUNT] = { 1.0f, 0.50f, 0.25f };
    const float errors[MESH_LOD_COUNT]  = { 0.0f, 0.20f, 0.50f };
    const float attributeWeights[7] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.25f, 0.25f, 0.25f };
    for (u32 lod = 1; lod < MESH_LOD_COUNT; lod++)
    {
        f32 target = Maxf32(baseTargets[lod] * lodBudgetScale, 0.04f);
        u32 targetIndexCount = RoundTriangleIndexCount((size_t)((f32)numIndices * target));
        f32 resultError = 0.0f;
        size_t simplifiedCount = meshopt_simplifyWithAttributes(simplified, localIndices, numIndices,
                                                                &positions[0].x, numVertices, sizeof(float3),
                                                                skinAttributes, 7u * sizeof(f32), attributeWeights, 7u,
                                                                NULL, targetIndexCount, errors[lod], 0, &resultError);
        if (simplifiedCount < 3u)
        {
            AX_WARN("skinned lod generation failed: lod=%d vertices=%d indices=%d", lod, primitive->numVertices, primitive->numIndices);
            continue;
        }

        if (*lodIndexCursor + (u32)simplifiedCount > indexRangeEnd)
        {
            AX_WARN("skinned lod generation skipped: bundle index range exceeded lod=%d indices=%d/%d", lod,
                    *lodIndexCursor + (u32)simplifiedCount, indexRangeEnd);
            continue;
        }

        const u32 invalidRemap = ~0u;
        for (u32 v = 0; v < (u32)numVertices; v++)
            vertexRemap[v] = invalidRemap;

        u32 compactVertexCount = 0;
        for (u32 i = 0; i < (u32)simplifiedCount; i++)
        {
            u32 localVertex = simplified[i];
            if (localVertex >= (u32)numVertices)
            {
                AX_WARN("skinned lod generation skipped: invalid simplified vertex lod=%d vertex=%d/%d", lod, localVertex, primitive->numVertices);
                compactVertexCount = 0;
                break;
            }

            if (vertexRemap[localVertex] == invalidRemap)
                vertexRemap[localVertex] = compactVertexCount++;
        }

        if (compactVertexCount == 0)
            continue;

        if (*vertexCursor + compactVertexCount > vertexRangeEnd)
        {
            AX_WARN("skinned lod generation skipped: bundle vertex range exceeded lod=%d vertices=%d/%d", lod,
                    *vertexCursor + compactVertexCount, vertexRangeEnd);
            continue;
        }

        u32 lodVertexOffset = *vertexCursor;
        if (lodVertexOffset + compactVertexCount > MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE)
        {
            AX_WARN("skinned lod generation skipped: animated vertex capacity exceeded lod=%d vertices=%d/%llu", lod,
                    lodVertexOffset + compactVertexCount, (unsigned long long)MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE);
            continue;
        }

        for (u32 v = 0; v < (u32)numVertices; v++)
        {
            u32 remapped = vertexRemap[v];
            if (remapped != invalidRemap)
                (*vertexWrite)[remapped] = sourceVertices[v];
        }

        primitive->lodIndexOffset[lod]  = (int)*lodIndexCursor;
        primitive->lodNumIndices[lod]   = (int)simplifiedCount;
        primitive->lodVertexOffset[lod] = (int)lodVertexOffset;
        primitive->lodNumVertices[lod]  = (int)compactVertexCount;

        for (u32 i = 0; i < (u32)simplifiedCount; i++)
            (*lodWrite)[i] = lodVertexOffset + vertexRemap[simplified[i]];

        *vertexWrite += compactVertexCount;
        *vertexCursor += compactVertexCount;
        *lodWrite += simplifiedCount;
        *lodIndexCursor += (u32)simplifiedCount;

        AX_LOG("skinned lod generated: lod=%d indices=%d/%d vertices=%d/%d error=%f", lod,
               (u32)simplifiedCount, primitive->numIndices, compactVertexCount, primitive->numVertices, resultError);
    }

    ArenaPopGlobal(numVertices * 7u * sizeof(f32));
    ArenaPopGlobal(numVertices * sizeof(u32));
    ArenaPopGlobal(numIndices * sizeof(u32));
    ArenaPopGlobal(numIndices * sizeof(u32));
}

static void ValidatePrimitiveLODs(const SceneBundle* gltf, bool isSkinned, u32 vertexBase, u32 indexEnd)
{
    u32 vertexEnd = vertexBase + (u32)gltf->totalVertices;
    for (s32 m = 0; m < gltf->numMeshes; m++)
    {
        const AMesh* mesh = gltf->meshes + m;
        for (s32 p = 0; p < mesh->numPrimitives; p++)
        {
            const APrimitive* primitive = mesh->primitives + p;
            for (u32 lod = 0; lod < MESH_LOD_COUNT; lod++)
            {
                u32 indexOffset = (u32)primitive->lodIndexOffset[lod];
                u32 numIndices = (u32)primitive->lodNumIndices[lod];
                if (indexOffset + numIndices > indexEnd)
                {
                    AX_WARN("lod index range invalid mesh=%d primitive=%d lod=%d range=%d..%d total=%d",
                            m, p, lod, indexOffset, indexOffset + numIndices, indexEnd);
                    continue;
                }

                if (isSkinned)
                {
                    u32 animatedOffset = (u32)primitive->lodVertexOffset[lod];
                    u32 numVertices = (u32)primitive->lodNumVertices[lod];
                    if (animatedOffset + numVertices > MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE)
                    {
                        AX_WARN("lod animated range invalid mesh=%d primitive=%d lod=%d range=%d..%d max=%llu",
                                m, p, lod, animatedOffset, animatedOffset + numVertices,
                                (unsigned long long)MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE);
                    }
                }

                for (u32 i = 0; i < numIndices; i++)
                {
                    u32 index = gGFX.IndexBuffer[indexOffset + i];
                    if (index < vertexBase || index >= vertexEnd)
                    {
                        AX_WARN("lod vertex index invalid mesh=%d primitive=%d lod=%d index=%d vertexRange=%d..%d",
                                m, p, lod, index, vertexBase, vertexEnd);
                        break;
                    }
                }
            }
        }
    }
}

s32 BakeSceneMeshesAndAnimations(SceneBundle* gltf, void** outVertexHeapPtr, void** outIndexHeapPtr)
{
    AX_LOG("mesh bake: meshes=%d vertices=%d indices=%d skins=%d", gltf->numMeshes, gltf->totalVertices, gltf->totalIndices, gltf->numSkins);
    if (gltf->totalVertices <= 0 || gltf->totalIndices <= 0)
    {
        AX_WARN("mesh bake failed: empty geometry vertices=%d indices=%d", gltf->totalVertices, gltf->totalIndices);
        return 0;
    }

    AMesh* meshes    = gltf->meshes;
    bool isSkinned = gltf->numSkins > 0;
    GeometryBufferKind vertexKind = isSkinned ? GeometryBuffer_SkinnedVertex : GeometryBuffer_SurfaceVertex;

    // lods append indices (and vertices for skinned meshes) of unknown size,
    // allocate generous headroom and shrink to the final totals after the bake.
    // meshopt can return more indices than the target, the extra headroom keeps
    // tail primitives from losing their lods when the budget is exactly desired.
    // +1 vertex / +4 index padding for wide copies past the range end
    f32 desiredLODIndices = (f32)gltf->totalIndices * 0.75f;

    u32 vertexCapacity = 0;
    u32 vertexBase = GEOMETRY_ALLOC_FAIL;
    void* vertexRaw = NULL;
    u32 vertexLODCount = isSkinned ? MESH_LOD_COUNT : 1u;
    for (u32 lods = vertexLODCount; lods >= 1u && vertexBase == GEOMETRY_ALLOC_FAIL; lods--)
    {
        vertexCapacity = (u32)gltf->totalVertices * lods + 1u;
        vertexBase = GeometryHeapAlloc(vertexKind, vertexCapacity, &vertexRaw);
    }

    static const f32 lodHeadroom[] = { 2.0f, 1.5f, 1.25f, 1.1f, 1.0f, 0.5f, 0.25f, 0.0f };
    u32 indexCapacity = 0;
    u32 indexBase = GEOMETRY_ALLOC_FAIL;
    void* indexRaw = NULL;
    for (u32 h = 0; h < ARRAY_SIZE(lodHeadroom) && indexBase == GEOMETRY_ALLOC_FAIL; h++)
    {
        indexCapacity = (u32)gltf->totalIndices + (u32)(desiredLODIndices * lodHeadroom[h]) + 4u;
        indexBase = GeometryHeapAlloc(GeometryBuffer_Index, indexCapacity, &indexRaw);
    }

    if (vertexBase == GEOMETRY_ALLOC_FAIL || indexBase == GEOMETRY_ALLOC_FAIL)
    {
        AX_WARN("mesh bake failed: vertex/index buffer space exhausted vertices=%d indices=%d",
                vertexCapacity, indexCapacity);
        GeometryHeapFree(vertexKind, vertexRaw);
        GeometryHeapFree(GeometryBuffer_Index, indexRaw);
        return 0;
    }

    if (isSkinned && vertexBase + (u32)gltf->totalVertices > MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE)
    {
        AX_WARN("mesh bake failed: skinned vertex offsets exceed animated capacity range=%d..%d max=%llu",
                vertexBase, vertexBase + (u32)gltf->totalVertices,
                (unsigned long long)MAX_SKINNED_VERTEX_PER_ANIM_INSTANCE);
        GeometryHeapFree(vertexKind, vertexRaw);
        GeometryHeapFree(GeometryBuffer_Index, indexRaw);
        return 0;
    }

    if (outVertexHeapPtr) *outVertexHeapPtr = vertexRaw;
    if (outIndexHeapPtr)  *outIndexHeapPtr = indexRaw;

    u32 vertexCursor = vertexBase;
    u32 firstVertexCursor = vertexCursor;
    u32 indexCursor = indexBase;
    u32 firstIndexCursor = indexCursor;
    u32 vertexRangeEnd = vertexBase + vertexCapacity - 1u;
    u32 indexRangeEnd  = indexBase + indexCapacity - 4u;

    gltf->allVertices = isSkinned ? (void*)(gGFX.SkinnedVertexBuffer + vertexCursor) : (void*)(gGFX.SurfaceVertexBuffer + vertexCursor);
    gltf->allIndices  = gGFX.IndexBuffer + indexCursor;

    ASkinedVertex* currSkinnedVertex = (ASkinedVertex*)gltf->allVertices;
    AVertex* currSurfaceVertex = (AVertex*)gltf->allVertices;
    u32* currIndices = (u32*)gltf->allIndices;
    u32 lodIndexCursor = indexCursor + (u32)gltf->totalIndices;
    u32* lodWrite = gGFX.IndexBuffer + lodIndexCursor;
    // when the allocator was too tight for the desired budget, scale the lod targets down
    u32 lodBudget = indexCapacity - 4u - (u32)gltf->totalIndices;
    f32 lodBudgetScale = desiredLODIndices > 0.0f ? Minf32((f32)lodBudget / desiredLODIndices, 1.0f) : 1.0f;

    for (s32 m = 0; m < gltf->numMeshes; ++m)
    {
        AMesh mesh = meshes[m];
        for (s32 p = 0; p < mesh.numPrimitives; p++)
        {
            APrimitive* primitive = &mesh.primitives[p];
            if (primitive->numVertices <= 0 || primitive->numIndices <= 0)
            {
                AX_WARN("mesh %d primitive %d has empty geometry vertices=%d indices=%d", m, p, primitive->numVertices, primitive->numIndices);
                continue;
            }
            if (primitive->indices && (primitive->indexType < AComponentType_BYTE || primitive->indexType > AComponentType_FLOAT))
            {
                AX_WARN("mesh %d primitive %d has invalid index type: %d", m, p, primitive->indexType);
                continue;
            }

            u32 primitiveVertexCursor = vertexCursor;
            u32 primitiveIndexCursor = indexCursor;
            IndicesForPrimitive(primitive, currIndices, primitiveVertexCursor);
            // Bounds first: surface vertices quantize their position against this AABB.
            BoundsForPrimitive(primitive);
            if (isSkinned)
            {
                VerticesForPrimitive(primitive, currSkinnedVertex);
                JointsForPrimitive(primitive, currSkinnedVertex);
                WeightsForPrimitive(primitive, currSkinnedVertex);
                currSkinnedVertex += primitive->numVertices;
                vertexCursor += primitive->numVertices;
            }
            else
            {
                SurfaceVerticesForPrimitive(primitive, currSurfaceVertex);
                currSurfaceVertex += primitive->numVertices;
                // Optimize LOD0 index order before LODs are generated from it.
                OptimizePrimitiveVertexCache(currIndices, primitive->numIndices, primitive->numVertices, primitiveVertexCursor);
            }

            primitive->indexOffset = primitiveIndexCursor;
            SetPrimitiveLODFallback(primitive, primitiveVertexCursor);
            if (isSkinned)
            {
                GenerateSkinnedLODsForPrimitive(primitive, currIndices, primitiveVertexCursor, &currSkinnedVertex,
                                                &vertexCursor, vertexRangeEnd, &lodWrite, &lodIndexCursor,
                                                indexRangeEnd, lodBudgetScale);
            }
            else
            {
                GenerateStaticLODsForPrimitive(primitive, currIndices, primitiveVertexCursor, &lodWrite, &lodIndexCursor,
                                               indexRangeEnd, lodBudgetScale);
                vertexCursor += primitive->numVertices;
            }

            currIndices  += primitive->numIndices;
            indexCursor  += primitive->numIndices;
        }
    }

    gltf->totalVertices = (s32)(vertexCursor - firstVertexCursor);
    gltf->totalIndices  = (s32)(lodIndexCursor - firstIndexCursor);

    // return the unused worst case tail, the offsets never move on shrink
    GeometryHeapShrink(vertexKind, vertexRaw, vertexBase, (u32)gltf->totalVertices + 1u);
    GeometryHeapShrink(GeometryBuffer_Index, indexRaw, indexBase, (u32)gltf->totalIndices + 4u);

    if (isSkinned) gGFX.NumSkinnedVertices += (u32)gltf->totalVertices;
    else           gGFX.NumSurfaceVertices += (u32)gltf->totalVertices;
    gGFX.NumIndices += (u32)gltf->totalIndices;

    Rendering_QueueGeometryUpload(isSkinned ? GeometryBuffer_SkinnedVertex : GeometryBuffer_SurfaceVertex,
                                  vertexBase, vertexBase + (u32)gltf->totalVertices);
    Rendering_QueueGeometryUpload(GeometryBuffer_Index, indexBase, indexBase + (u32)gltf->totalIndices);

    ValidatePrimitiveLODs(gltf, isSkinned, firstVertexCursor, firstIndexCursor + (u32)gltf->totalIndices);

    BakeGLTFAnimations(gltf);
    FreeGLTFBuffers(gltf);
    AX_LOG("mesh bake complete: vertices=%d indices=%d", gltf->totalVertices, gltf->totalIndices);
    return 1;
}

void GenerateLOD_50_GLTF(SceneBundle* sceneBundle) { (void)sceneBundle; }

void GenerateLOD_75_GLTF(SceneBundle* sceneBundle) { (void)sceneBundle; }

