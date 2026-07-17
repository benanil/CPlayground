#include "Include/Platform.h"
#include "Include/Memory.h"
#include "Math/Bitpack.h"
#include "Transvoxel.h"
#include "TransvoxelTables.h"
#include "TerrainInternal.h"

#define T_TRANSITION_CELL_WIDTH_PERCENTAGE 0.5f

typedef enum tTransitionDirection_
{
    tTransitionDirection_XMin,
    tTransitionDirection_YMin,
    tTransitionDirection_ZMin,
    tTransitionDirection_XMax,
    tTransitionDirection_YMax,
    tTransitionDirection_ZMax
} tTransitionDirection;

typedef struct tTransvoxelMesher_
{
    const tDensityGenerator* generator;
    int3                     chunkMin;
    const f32*               densityData;
    s32                      lod;
    s32                      lodScale;
    tMeshDataContainer*      meshData;
} tTransvoxelMesher;

static f32 tMesherDensityAt(const tTransvoxelMesher* mesher, s32 x, s32 y, s32 z)
{
    s32 densitySize = T_CHUNK_CELLS + 3;
    return mesher->densityData[x * densitySize * densitySize + y * densitySize + z];
}

static f32 tMesherDensityAtI3(const tTransvoxelMesher* mesher, int3 position)
{
    return tMesherDensityAt(mesher, position.x, position.y, position.z);
}

static f32 tMesherGeneratorAt(const tTransvoxelMesher* mesher, float3 pos)
{
	const tDensityGenerator* generator = mesher->generator;
	if (!generator)
    {
        AX_WARN("transvoxel unity density generator failed: null generator");
        return -pos.y;
    }

    f32 heightMap = 0.0f;
    if (generator->heightMapNoise)
        heightMap = generator->heightMapNoise(pos.x, pos.z, generator->heightMapUserData) * generator->heightMapStrength;

    f32 noise3D = 0.0f;
    if (generator->noise3D)
        noise3D = generator->noise3D(pos.x, pos.y, pos.z, generator->noise3DUserData) * generator->noise3DStrength;

    return -pos.y + heightMap + noise3D;
}

static s32 tTransvoxelMesherSign(f32 value)
{
    if (value > 0.0f) return 1;
    if (value < 0.0f) return -1;
    return 0;
}

static int3 tMesherFaceToLocalI(tTransitionDirection direction, s32 x, s32 y, s32 z)
{
    switch (direction)
    {
        case tTransitionDirection_XMin: return (int3){ z, x, y };
        case tTransitionDirection_XMax: return (int3){ T_CHUNK_CELLS - z, y, x };
        case tTransitionDirection_YMin: return (int3){ y, z, x };
        case tTransitionDirection_YMax: return (int3){ x, T_CHUNK_CELLS - z, y };
        case tTransitionDirection_ZMin: return (int3){ x, y, z };
        case tTransitionDirection_ZMax: return (int3){ y, x, T_CHUNK_CELLS - z };
        default: return (int3){ x, y, z };
    }
}

static float3 tMesherFaceToLocalF(tTransitionDirection direction, f32 x, f32 y, f32 z)
{
    switch (direction)
    {
        case tTransitionDirection_XMin: return (float3){ z, x, y };
        case tTransitionDirection_XMax: return (float3){ (f32)T_CHUNK_CELLS - z, y, x };
        case tTransitionDirection_YMin: return (float3){ y, z, x };
        case tTransitionDirection_YMax: return (float3){ x, (f32)T_CHUNK_CELLS - z, y };
        case tTransitionDirection_ZMin: return (float3){ x, y, z };
        case tTransitionDirection_ZMax: return (float3){ y, x, (f32)T_CHUNK_CELLS - z };
        default: return (float3){ x, y, z };
    }
}

static float3 tMesherWorldFromLocal(const tTransvoxelMesher* mesher, float3 localPosition)
{
    return F3Add(ToFloat3(mesher->chunkMin), F3MulF(localPosition, (f32)mesher->lodScale));
}

static float3 tMesherNormalAtSample(const tTransvoxelMesher* mesher, s32 x, s32 y, s32 z)
{
    float3 normal =
    {
        tMesherDensityAt(mesher, x - 1, y, z) - tMesherDensityAt(mesher, x + 1, y, z),
        tMesherDensityAt(mesher, x, y - 1, z) - tMesherDensityAt(mesher, x, y + 1, z),
        tMesherDensityAt(mesher, x, y, z - 1) - tMesherDensityAt(mesher, x, y, z + 1)
    };
    return normal;
}

static float3 tMesherNormalAtWorld(const tTransvoxelMesher* mesher, float3 worldPosition)
{
    float3 x0 = { worldPosition.x - 1.0f, worldPosition.y, worldPosition.z };
    float3 x1 = { worldPosition.x + 1.0f, worldPosition.y, worldPosition.z };
    float3 y0 = { worldPosition.x, worldPosition.y - 1.0f, worldPosition.z };
    float3 y1 = { worldPosition.x, worldPosition.y + 1.0f, worldPosition.z };
    float3 z0 = { worldPosition.x, worldPosition.y, worldPosition.z - 1.0f };
    float3 z1 = { worldPosition.x, worldPosition.y, worldPosition.z + 1.0f };
    float3 normal =
    {
        tMesherGeneratorAt(mesher, x0) - tMesherGeneratorAt(mesher, x1),
        tMesherGeneratorAt(mesher, y0) - tMesherGeneratorAt(mesher, y1),
        tMesherGeneratorAt(mesher, z0) - tMesherGeneratorAt(mesher, z1)
    };
    return normal;
}

static s32 tMesherBoundaryMaskFromPoint(s32 x, s32 y, s32 z)
{
    s32 mask = 0;
    if (x == 0) mask |= 1;
    if (y == 0) mask |= 2;
    if (z == 0) mask |= 4;
    if (x == T_CHUNK_CELLS) mask |= 8;
    if (y == T_CHUNK_CELLS) mask |= 16;
    if (z == T_CHUNK_CELLS) mask |= 32;
    return mask;
}

static s32 tMesherBoundaryMaskFromEdge(int3 a, int3 b)
{
    s32 mask = 0;
    if (a.x == 0 || b.x == 0) mask |= 1;
    if (a.y == 0 || b.y == 0) mask |= 2;
    if (a.z == 0 || b.z == 0) mask |= 4;
    if (a.x == T_CHUNK_CELLS || b.x == T_CHUNK_CELLS) mask |= 8;
    if (a.y == T_CHUNK_CELLS || b.y == T_CHUNK_CELLS) mask |= 16;
    if (a.z == T_CHUNK_CELLS || b.z == T_CHUNK_CELLS) mask |= 32;
    return mask;
}

static float3 tMesherSecondaryPosition(float3 vertex, float3 normal, s32 boundaryMask)
{
    float3 delta = F3Zero();
    if ((boundaryMask & 1) && vertex.x < 1.0f)
        delta.x = 1.0f - vertex.x;
    else if ((boundaryMask & 8) && vertex.x > (f32)(T_CHUNK_CELLS - 1))
        delta.x = (f32)(T_CHUNK_CELLS - 1) - vertex.x;

    if ((boundaryMask & 2) && vertex.y < 1.0f)
        delta.y = 1.0f - vertex.y;
    else if ((boundaryMask & 16) && vertex.y > (f32)(T_CHUNK_CELLS - 1))
        delta.y = (f32)(T_CHUNK_CELLS - 1) - vertex.y;

    if ((boundaryMask & 4) && vertex.z < 1.0f)
        delta.z = 1.0f - vertex.z;
    else if ((boundaryMask & 32) && vertex.z > (f32)(T_CHUNK_CELLS - 1))
        delta.z = (f32)(T_CHUNK_CELLS - 1) - vertex.z;

    delta = F3MulF(delta, T_TRANSITION_CELL_WIDTH_PERCENTAGE);
    float3 secondary =
    {
        vertex.x + (1.0f - normal.x * normal.x) * delta.x - normal.y * normal.x * delta.y - normal.z * normal.x * delta.z,
        vertex.y - normal.x * normal.y * delta.x + (1.0f - normal.y * normal.y) * delta.y - normal.z * normal.y * delta.z,
        vertex.z - normal.x * normal.z * delta.x - normal.y * normal.z * delta.y + (1.0f - normal.z * normal.z) * delta.z
    };
    return secondary;
}

static void tMesherFillS32(s32* values, size_t count, s32 value)
{
    for (size_t i = 0; i < count; i++)
        values[i] = value;
}

static bool tMesherEmitVertex(tMeshData* meshData, float3 vertex, float3 normal,
                              s32 lodScale, s32 boundaryMask, u32 vertexIndex)
{
    if (boundaryMask > 0)
    {
        if (vertexIndex > 0xFFFFu)
        {
            AX_WARN("transvoxel unity mesher skipped secondary vertex: vertex index out of range");
        }
        else
        {
            float3 secondaryPos = tMesherSecondaryPosition(vertex, normal, boundaryMask);
            tSecondaryVert secondary = {0};
            f32 chunkSize = (f32)(T_CHUNK_CELLS * lodScale);
            v128f localPos = VecSetR(secondaryPos.x * (f32)lodScale, secondaryPos.y * (f32)lodScale, secondaryPos.z * (f32)lodScale, 0.0f);
            secondary.position = Pack16x4Fixed(localPos, chunkSize);
            secondary.vertexMask = boundaryMask;
            secondary.vertexIndex = (s32)vertexIndex;
            if (!tMeshDataPushSecondaryVertex(meshData, secondary))
                return false;
        }
    }

    tVertexData data = {0};
    f32 chunkSize = (f32)(T_CHUNK_CELLS * lodScale);
    v128f localPos = VecSetR(vertex.x * (f32)lodScale, vertex.y * (f32)lodScale, vertex.z * (f32)lodScale, 0.0f);
    data.position = Pack16x4Fixed(localPos, chunkSize);
	v128f normVec = VecSetR(normal.x, normal.y, normal.z, 0.0f);
	data.normal = PackNormalOCT(normVec);
    return tMeshDataPushVertex(meshData, data);
}

static bool tMesherRegular(tTransvoxelMesher* mesher)
{
    const s32 padding = 1;
	tMeshData* meshData = &mesher->meshData->mesh[tMeshDataSlot_Main];
    if (!meshData)
        return false;

    size_t cacheCount = (size_t)T_CHUNK_CELLS * (size_t)T_CHUNK_CELLS * 4u;
    s32* currentCache = (s32*)ArenaPushGlobal(sizeof(s32) * cacheCount);
    s32* previousCache = (s32*)ArenaPushGlobal(sizeof(s32) * cacheCount);
    if (!currentCache || !previousCache)
    {
        AX_WARN("transvoxel unity mesher regular failed: allocation failed");
        if (previousCache) ArenaPopGlobal(sizeof(s32) * cacheCount);
        if (currentCache)  ArenaPopGlobal(sizeof(s32) * cacheCount);
        return false;
    }
    tMesherFillS32(currentCache, cacheCount, -1);
    tMesherFillS32(previousCache, cacheCount, -1);

    bool result = true;
    for (s32 y = 0; y < T_CHUNK_CELLS; y++)
    {
        tMesherFillS32(currentCache, cacheCount, -1);
        for (s32 z = 0; z < T_CHUNK_CELLS && result; z++)
        {
            for (s32 x = 0; x < T_CHUNK_CELLS; x++)
            {
                int3 cellPos = { x, y, z };
                f32 cellValues[8];
                for (u32 i = 0; i < 8; i++)
                {
                    int3 voxelPosition = I3Add(I3AddI(cellPos, padding), tRegularCornerOffset[i]);
                    cellValues[i] = tMesherDensityAtI3(mesher, voxelPosition);
                }

                s32 caseCode = 0;
                for (u32 i = 0; i < 8; i++)
                {
                    if (cellValues[i] < 0.0f)
                        caseCode |= 1 << i;
                }
                if (caseCode == 0 || caseCode == 255)
                    continue;

                s32 cacheValidator = 0;
                if (cellPos.x != 0) cacheValidator |= 1;
                if (cellPos.z != 0) cacheValidator |= 2;
                if (cellPos.y != 0) cacheValidator |= 4;

                u32 cellClass = regularCellClass[caseCode];
				const u16* edgeCodes = regularVertexData[caseCode];
                RegularCellData cellData = regularCellData[cellClass];
                u32 vertexIndices[16] = {0};
                u32 cellVertCount = TVCellVertexCount(cellData);

                for (u32 i = 0; i < cellVertCount; i++)
                {
                    u16 edgeCode = edgeCodes[i];
                    u32 cornerIdx0 = (edgeCode >> 4) & 0x0Fu;
                    u32 cornerIdx1 = edgeCode & 0x0Fu;
                    f32 density0 = cellValues[cornerIdx0];
                    f32 density1 = cellValues[cornerIdx1];
                    u32 cacheIdx = (edgeCode >> 8) & 0x0Fu;
                    u32 cacheDir = edgeCode >> 12;

                    if (density1 == 0.0f)
                    {
                        cacheDir = cornerIdx1 ^ 7u;
                        cacheIdx = 0;
                    }
                    else if (density0 == 0.0f)
                    {
                        cacheDir = cornerIdx0 ^ 7u;
                        cacheIdx = 0;
                    }

                    bool vertexCacheable = ((cacheDir & (u32)cacheValidator) == cacheDir);
                    s32 vertexIndex = -1;
                    s32 cachePosX = x - (s32)(cacheDir & 1u);
                    s32 cachePosZ = z - (s32)((cacheDir >> 1) & 1u);
                    s32* selectedCache = currentCache;
                    if (((cacheDir >> 2) & 1u) == 1u)
                        selectedCache = previousCache;

                    if (vertexCacheable)
                        vertexIndex = selectedCache[cachePosX * T_CHUNK_CELLS * 4 + cachePosZ * 4 + cacheIdx];

                    if (!vertexCacheable || vertexIndex == -1)
                    {
                        float3 vertex = F3Zero();
                        float3 normal = F3Up();
                        vertexIndex = meshData->numVertices;
                        s32 boundaryMask = 0;

                        if (cacheIdx == 0)
                        {
                            const int3* cornerOffsets = tRegularCornerOffset;
                            int3 cornerOffset = cornerOffsets[cornerIdx1];
                            if (density0 == 0.0f)
                                cornerOffset = cornerOffsets[cornerIdx0];

                            s32 vertPosX = x + cornerOffset.x;
                            s32 vertPosY = y + cornerOffset.y;
                            s32 vertPosZ = z + cornerOffset.z;
                            vertex = (float3){ (f32)vertPosX, (f32)vertPosY, (f32)vertPosZ };
                            if (mesher->lod > 0)
                                boundaryMask = tMesherBoundaryMaskFromPoint(vertPosX, vertPosY, vertPosZ);

                            normal = tMesherNormalAtSample(mesher, vertPosX + padding, vertPosY + padding, vertPosZ + padding);
                            if (vertexCacheable)
                                selectedCache[cachePosX * T_CHUNK_CELLS * 4 + cachePosZ * 4 + cacheIdx] = vertexIndex;
                        }
                        else
                        {
                            int3 vertLocalPos0 = I3Add(cellPos, tRegularCornerOffset[cornerIdx0]);
                            int3 vertLocalPos1 = I3Add(cellPos, tRegularCornerOffset[cornerIdx1]);
                            float3 vert0 = ToFloat3(vertLocalPos0);
                            float3 vert1 = ToFloat3(vertLocalPos1);

                            for (s32 j = 0; j < mesher->lod; j++)
                            {
                                float3 midLocal = F3MulF(F3Add(vert0, vert1), 0.5f);
                                float3 midWorld = tMesherWorldFromLocal(mesher, midLocal);
                                f32 midDensity = tMesherGeneratorAt(mesher, midWorld);
                                if (tTransvoxelMesherSign(midDensity) == tTransvoxelMesherSign(density0))
                                {
                                    vert0 = midLocal;
                                    density0 = midDensity;
                                }
                                else
                                {
                                    vert1 = midLocal;
                                    density1 = midDensity;
                                }
                            }

                            f32 t0 = density1 / (density1 - density0);
                            f32 t1 = 1.0f - t0;
                            vertex = F3Add(F3MulF(vert0, t0), F3MulF(vert1, t1));
                            if (mesher->lod > 0)
                                boundaryMask = tMesherBoundaryMaskFromEdge(vertLocalPos0, vertLocalPos1);

                            float3 normal0 = tMesherNormalAtSample(mesher, vertLocalPos0.x + padding, vertLocalPos0.y + padding, vertLocalPos0.z + padding);
                            float3 normal1 = tMesherNormalAtSample(mesher, vertLocalPos1.x + padding, vertLocalPos1.y + padding, vertLocalPos1.z + padding);
                            normal = F3Add(normal0, normal1);
                            if (cornerIdx1 == 7u)
                                currentCache[x * T_CHUNK_CELLS * 4 + z * 4 + cacheIdx] = vertexIndex;
                        }

                        normal = F3NormSafe(normal);
                        if (!tMesherEmitVertex(meshData, vertex, normal, mesher->lodScale, boundaryMask, (u32)vertexIndex))
                        {
                            result = false;
                            break;
                        }
                    }
                    vertexIndices[i] = (u32)vertexIndex;
                }
                if (!result)
                    break;

                u32 indexCount = TVCellTriangleCount(cellData) * 3u;
                for (u32 i = 0; i < indexCount; i += 3)
                {
                    if (!tMeshDataPushIndex(meshData, vertexIndices[cellData.vertexIndex[i + 0]]) ||
                        !tMeshDataPushIndex(meshData, vertexIndices[cellData.vertexIndex[i + 1]]) ||
                        !tMeshDataPushIndex(meshData, vertexIndices[cellData.vertexIndex[i + 2]]))
                    {
                        result = false;
                        break;
                    }
                }
            }
        }

        s32* temp = currentCache;
        currentCache = previousCache;
        previousCache = temp;
    }

    ArenaPopGlobal(cacheCount * sizeof(s32));
    ArenaPopGlobal(cacheCount * sizeof(s32));
    return result;
}

static bool tMesherTransition(tTransvoxelMesher* mesher, tMeshDataSlot slot, tTransitionDirection direction)
{
    const s32 padding = 1;
	tMeshData* meshData = &mesher->meshData->mesh[slot];
    const size_t cacheCount = (size_t)T_CHUNK_CELLS * 10u;
    s32* currentCache  = (s32*)ArenaPushGlobal(sizeof(s32) * cacheCount);
    s32* previousCache = (s32*)ArenaPushGlobal(sizeof(s32) * cacheCount);
    if (!currentCache || !previousCache)
    {
        AX_WARN("transvoxel unity mesher transition failed: allocation failed");
        if (previousCache) ArenaPopGlobal(sizeof(s32) * cacheCount);
        if (currentCache)  ArenaPopGlobal(sizeof(s32) * cacheCount);
        return false;
    }

    bool result = true;
    for (s32 y = 0; y < T_CHUNK_CELLS && result; y++)
    {
        for (s32 x = 0; x < T_CHUNK_CELLS && result; x++)
        {
            f32 cellValues[13];
            int3 pos0 = I3AddI(tMesherFaceToLocalI(direction, x,     y,     0), padding);
            int3 pos2 = I3AddI(tMesherFaceToLocalI(direction, x + 1, y,     0), padding);
            int3 pos6 = I3AddI(tMesherFaceToLocalI(direction, x,     y + 1, 0), padding);
            int3 pos8 = I3AddI(tMesherFaceToLocalI(direction, x + 1, y + 1, 0), padding);

            cellValues[0] = tMesherDensityAtI3(mesher, pos0);
            cellValues[2] = tMesherDensityAtI3(mesher, pos2);
            cellValues[6] = tMesherDensityAtI3(mesher, pos6);
            cellValues[8] = tMesherDensityAtI3(mesher, pos8);

            float3 pos1 = tMesherWorldFromLocal(mesher, tMesherFaceToLocalF(direction, (f32)x + 0.5f, (f32)y,        0.0f));
            float3 pos3 = tMesherWorldFromLocal(mesher, tMesherFaceToLocalF(direction, (f32)x,        (f32)y + 0.5f, 0.0f));
            float3 pos4 = tMesherWorldFromLocal(mesher, tMesherFaceToLocalF(direction, (f32)x + 0.5f, (f32)y + 0.5f, 0.0f));
            float3 pos5 = tMesherWorldFromLocal(mesher, tMesherFaceToLocalF(direction, (f32)x + 1.0f, (f32)y + 0.5f, 0.0f));
            float3 pos7 = tMesherWorldFromLocal(mesher, tMesherFaceToLocalF(direction, (f32)x + 0.5f, (f32)y + 1.0f, 0.0f));
            cellValues[1] = tMesherGeneratorAt(mesher, pos1);
            cellValues[3] = tMesherGeneratorAt(mesher, pos3);
            cellValues[4] = tMesherGeneratorAt(mesher, pos4);
            cellValues[5] = tMesherGeneratorAt(mesher, pos5);
            cellValues[7] = tMesherGeneratorAt(mesher, pos7);
            cellValues[9] = cellValues[0];
            cellValues[10] = cellValues[2];
            cellValues[11] = cellValues[6];
            cellValues[12] = cellValues[8];

            s32 caseCode = 0;
            if (cellValues[0] < 0.0f) caseCode |= 1;
            if (cellValues[1] < 0.0f) caseCode |= 2;
            if (cellValues[2] < 0.0f) caseCode |= 4;
            if (cellValues[5] < 0.0f) caseCode |= 8;
            if (cellValues[8] < 0.0f) caseCode |= 16;
            if (cellValues[7] < 0.0f) caseCode |= 32;
            if (cellValues[6] < 0.0f) caseCode |= 64;
            if (cellValues[3] < 0.0f) caseCode |= 128;
            if (cellValues[4] < 0.0f) caseCode |= 256;
            if (caseCode == 0 || caseCode == 511)
                continue;
			currentCache[0 * T_CHUNK_CELLS + x] = -1;
			currentCache[1 * T_CHUNK_CELLS + x] = -1;
			currentCache[2 * T_CHUNK_CELLS + x] = -1;
			currentCache[7 * T_CHUNK_CELLS + x] = -1;

            s32 cacheValidator = 0;
            if (x != 0) cacheValidator |= 1;
            if (y != 0) cacheValidator |= 2;

            u32 cellClass = transitionCellClass[caseCode];
			const u16* edgeCodes = transitionVertexData[caseCode];
            TransitionCellData cellData = transitionCellData[cellClass & 0x7Fu];
            u32 vertexIndices[36] = {0};
            u32 cellVertCount = TVCellVertexCount(cellData);

            for (u32 i = 0; i < cellVertCount; i++)
            {
                u16 edgeCode = edgeCodes[i];
                u32 cornerIdx0 = (edgeCode >> 4) & 0x0Fu;
                u32 cornerIdx1 = edgeCode & 0x0Fu;
                f32 density0 = cellValues[cornerIdx0];
                f32 density1 = cellValues[cornerIdx1];
                u32 cacheIdx = (edgeCode >> 8) & 0x0Fu;
                u32 cacheDir = edgeCode >> 12;

                if (density1 == 0.0f)
                {
                    u8 cornerData = transitionCornerData[cornerIdx1];
                    cacheDir = (cornerData >> 4) & 0x0Fu;
                    cacheIdx = cornerData & 0x0Fu;
                }
                else if (density0 == 0.0f)
                {
                    u8 cornerData = transitionCornerData[cornerIdx0];
                    cacheDir = (cornerData >> 4) & 0x0Fu;
                    cacheIdx = cornerData & 0x0Fu;
                }

                bool vertexCacheable = ((cacheDir & (u32)cacheValidator) == cacheDir);
                s32 vertexIndex = -1;
                s32 cachePosX = x - (s32)(cacheDir & 1u);
                s32* selectedCache = currentCache;
                if ((cacheDir & 2u) > 0u)
                    selectedCache = previousCache;
                if (vertexCacheable)
                    vertexIndex = selectedCache[cacheIdx * T_CHUNK_CELLS + cachePosX];

                if (!vertexCacheable || vertexIndex == -1)
                {
                    float3 vertex = F3Zero();
                    float3 normal = F3Up();
                    vertexIndex = meshData->numVertices;
                    s32 boundaryMask = 0;
                    bool lowResFace = cacheIdx > 6u;

                    if (density0 == 0.0f || density1 == 0.0f)
                    {
                        u32 cornerIdx = cornerIdx1;
                        if (density0 == 0.0f)
                            cornerIdx = cornerIdx0;
                        int3 cornerOffset = tTransitionCornerOffset[cornerIdx];
                        vertex = tMesherFaceToLocalF(direction, (f32)x + (f32)cornerOffset.x * 0.5f,
                                                                (f32)y + (f32)cornerOffset.y * 0.5f,
                                                                0.0f);

                        if (lowResFace)
                        {
                            int3 localPos = tMesherFaceToLocalI(direction, x + cornerOffset.x / 2, y + cornerOffset.y / 2, 0);
                            boundaryMask = tMesherBoundaryMaskFromPoint(localPos.x, localPos.y, localPos.z);
                            normal = tMesherNormalAtSample(mesher, localPos.x + padding, localPos.y + padding, localPos.z + padding);
                        }
                        else
                        {
                            normal = tMesherNormalAtWorld(mesher, tMesherWorldFromLocal(mesher, vertex));
                        }

                        if (cacheDir == 8u)
                            currentCache[cacheIdx * T_CHUNK_CELLS + x] = vertexIndex;
                        else if (vertexCacheable)
                            selectedCache[cacheIdx * T_CHUNK_CELLS + cachePosX] = vertexIndex;
                    }
                    else
                    {
                        int3 cornerOffset0 = tTransitionCornerOffset[cornerIdx0];
                        int3 cornerOffset1 = tTransitionCornerOffset[cornerIdx1];
                        float3 corner0 = tMesherFaceToLocalF(direction, (f32)x + (f32)cornerOffset0.x * 0.5f, (f32)y + (f32)cornerOffset0.y * 0.5f, 0.0f);
                        float3 corner1 = tMesherFaceToLocalF(direction, (f32)x + (f32)cornerOffset1.x * 0.5f, (f32)y + (f32)cornerOffset1.y * 0.5f, 0.0f);
                        s32 subEdges = mesher->lod - 1;
                        if (lowResFace)
                            subEdges = mesher->lod;

                        for (s32 j = 0; j < subEdges; j++)
                        {
                            float3 midLocal = F3MulF(F3Add(corner0, corner1), 0.5f);
                            float3 midWorld = tMesherWorldFromLocal(mesher, midLocal);
                            f32 midDensity = tMesherGeneratorAt(mesher, midWorld);
                            if (tTransvoxelMesherSign(midDensity) == tTransvoxelMesherSign(density0))
                            {
                                corner0 = midLocal;
                                density0 = midDensity;
                            }
                            else
                            {
                                corner1 = midLocal;
                                density1 = midDensity;
                            }
                        }

                        f32 t0 = density1 / (density1 - density0);
                        f32 t1 = 1.0f - t0;
                        vertex = F3Add(F3MulF(corner0, t0), F3MulF(corner1, t1));

                        float3 normal0;
                        float3 normal1;
                        if (lowResFace)
                        {
                            int3 local0 = tMesherFaceToLocalI(direction, x + cornerOffset0.x / 2, y + cornerOffset0.y / 2, 0);
                            int3 local1 = tMesherFaceToLocalI(direction, x + cornerOffset1.x / 2, y + cornerOffset1.y / 2, 0);
                            boundaryMask = tMesherBoundaryMaskFromEdge(local0, local1);
                            normal0 = tMesherNormalAtSample(mesher, local0.x + padding, local0.y + padding, local0.z + padding);
                            normal1 = tMesherNormalAtSample(mesher, local1.x + padding, local1.y + padding, local1.z + padding);
                        }
                        else
                        {
                            float3 local0 = tMesherFaceToLocalF(direction, (f32)x + (f32)cornerOffset0.x * 0.5f, (f32)y + (f32)cornerOffset0.y * 0.5f, 0.0f);
                            float3 local1 = tMesherFaceToLocalF(direction, (f32)x + (f32)cornerOffset1.x * 0.5f, (f32)y + (f32)cornerOffset1.y * 0.5f, 0.0f);
                            normal0 = tMesherNormalAtWorld(mesher, tMesherWorldFromLocal(mesher, local0));
                            normal1 = tMesherNormalAtWorld(mesher, tMesherWorldFromLocal(mesher, local1));
                        }
                        normal = F3Add(normal0, normal1);

                        if (cacheDir == 8u)
                            currentCache[cacheIdx * T_CHUNK_CELLS + x] = vertexIndex;
                        else if (vertexCacheable && cacheDir != 4u)
                            selectedCache[cacheIdx * T_CHUNK_CELLS + cachePosX] = vertexIndex;
                    }

                    normal = F3NormSafe(normal);
                    if (!tMesherEmitVertex(meshData, vertex, normal, mesher->lodScale, boundaryMask, (u32)vertexIndex))
                    {
                        result = false;
                        break;
                    }
                }
                vertexIndices[i] = (u32)vertexIndex;
            }
            if (!result)
                break;

            bool flipWinding = (cellClass & 0x80u) > 0u;
            u32 indexCount = TVCellTriangleCount(cellData) * 3u;
            for (u32 i = 0; i < indexCount; i += 3)
            {
                u32 ia = vertexIndices[cellData.vertexIndex[i + 0]];
                u32 ib = vertexIndices[cellData.vertexIndex[i + 1]];
                u32 ic = vertexIndices[cellData.vertexIndex[i + 2]];
                if (!flipWinding)
                {
                    if (!tMeshDataPushIndex(meshData, ic) ||
                        !tMeshDataPushIndex(meshData, ib) ||
                        !tMeshDataPushIndex(meshData, ia))
                    {
                        result = false;
                        break;
                    }
                }
                else
                {
                    if (!tMeshDataPushIndex(meshData, ia) ||
                        !tMeshDataPushIndex(meshData, ib) ||
                        !tMeshDataPushIndex(meshData, ic))
                    {
                        result = false;
                        break;
                    }
                }
            }
        }

        s32* temp = currentCache;
        currentCache = previousCache;
        previousCache = temp;
    }

    ArenaPopGlobal(sizeof(s32) * cacheCount);
    ArenaPopGlobal(sizeof(s32) * cacheCount);
    return result;
}

bool tTransvoxelMesherMesh(const tDensityGenerator* generator, const f32* densityData, tBuildJob* job)
{
	tMeshDataContainer* meshData = &job->scratchMesh;
    
    if (!generator || !densityData || !meshData || job->lod < 0)
    {
        AX_WARN("transvoxel unity mesher failed: invalid argument");
        return false;
    }

    tTransvoxelMesher mesher = {0};
    mesher.generator   = generator;
    mesher.chunkMin    = job->min;
    mesher.densityData = densityData;
    mesher.lod         = job->lod;
    mesher.lodScale    = 1 << job->lod;
    mesher.meshData    = meshData;

    tMeshDataContainerClear(meshData);
    bool result = tMesherRegular(&mesher);
    result = result && tMesherTransition(&mesher, tMeshDataSlot_LeftTransition,    tTransitionDirection_XMin);
    result = result && tMesherTransition(&mesher, tMeshDataSlot_DownTransition,    tTransitionDirection_YMin);
    result = result && tMesherTransition(&mesher, tMeshDataSlot_BackTransition,    tTransitionDirection_ZMin);
    result = result && tMesherTransition(&mesher, tMeshDataSlot_RightTransition,   tTransitionDirection_XMax);
    result = result && tMesherTransition(&mesher, tMeshDataSlot_UpTransition,      tTransitionDirection_YMax);
    result = result && tMesherTransition(&mesher, tMeshDataSlot_ForwardTransition, tTransitionDirection_ZMax);
    return result;
}
