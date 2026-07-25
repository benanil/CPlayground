#include "Include/Platform.h"
#include "Include/Memory.h"
#include "Math/Bitpack.h"
#include "Source/Terrain/TerrainInternal.h"
#include "TransvoxelTables.h"

typedef struct tMarchingMesher_
{
    const tDensityGenerator* generator;
    int3                     chunkMin;
    const s8*                densityData;
    tMeshData*      meshData;
    s32*            edgeCache;
} tMarchingMesher;

static f32 tMesherDensityAt(const tMarchingMesher* mesher, s32 x, s32 y, s32 z)
{
    s32 densitySize = T_CHUNK_CELLS + 3;
    s8 raw = mesher->densityData[x * densitySize * densitySize + y * densitySize + z];
    return (f32)raw * T_DENSITY_DECODE_SCALE;
}

static f32 tMesherGeneratorAt(const tMarchingMesher* mesher, float3 pos)
{
    const tDensityGenerator* generator = mesher->generator;
    if (!generator)
    {
        AX_WARN("marching cubes density generator failed: null generator");
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

static s32 tMarchingMesherSign(f32 value)
{
    if (value > 0.0f) return 1;
    if (value < 0.0f) return -1;
    return 0;
}

static float3 tMesherWorldFromLocal(const tMarchingMesher* mesher, float3 localPosition)
{
    return F3Add(ToFloat3(mesher->chunkMin), localPosition);
}

static float3 tMesherNormalAtSample(const tMarchingMesher* mesher, s32 x, s32 y, s32 z)
{
    float3 normal =
    {
        tMesherDensityAt(mesher, x - 1, y, z) - tMesherDensityAt(mesher, x + 1, y, z),
        tMesherDensityAt(mesher, x, y - 1, z) - tMesherDensityAt(mesher, x, y + 1, z),
        tMesherDensityAt(mesher, x, y, z - 1) - tMesherDensityAt(mesher, x, y, z + 1)
    };
    return normal;
}

static void tMesherFillS32(s32* values, size_t count, s32 value)
{
    for (size_t i = 0; i < count; i++)
        values[i] = value;
}

static u32 tMarchingPointIndex(int3 p)
{
    s32 axis = T_CHUNK_CELLS + 1;
    return (u32)(p.x * axis * axis + p.y * axis + p.z);
}

static u32 tMarchingEdgeCacheOffset(u32 edge, int3 cellPos)
{
    u32 corner0 = tMCEdgeCorners[edge][0];
    u32 corner1 = tMCEdgeCorners[edge][1];
    int3 p0 = I3Add(cellPos, tMCCornerOffset[corner0]);
    int3 p1 = I3Add(cellPos, tMCCornerOffset[corner1]);
	int3 mn = I3Min(p0, p1);

    u32 axis = 0;
    if (p0.y != p1.y) axis = 1;
    if (p0.z != p1.z) axis = 2;
    u32 pointCount = (u32)((T_CHUNK_CELLS + 1) * (T_CHUNK_CELLS + 1) * (T_CHUNK_CELLS + 1));
    return axis * pointCount + tMarchingPointIndex(mn);
}

static bool tMarchingEmitVertex(tMeshData* meshData, float3 vertex, float3 normal)
{
    tVertex data = {0};
    v128f localPos = VecSetR(vertex.x, vertex.y, vertex.z, 0.0f);
    data.position = PackXY11Z10UnormFixed(localPos, (f32)T_CHUNK_CELLS);
    v128f normVec = VecSetR(normal.x, normal.y, normal.z, 0.0f);
    data.normal = PackNormalOCT(normVec);
    return tMeshDataPushVertex(meshData, data);
}

static bool tMarchingCachedVertexMatchesEdge(const tMarchingMesher* mesher, const tMeshData* meshData,
                                             s32 vertexIndex, int3 p0i, int3 p1i)
{
    if (vertexIndex < 0 || vertexIndex >= meshData->numVertices)
        return false;

    float3 vertex = Vec3Get(UnpackXY11Z10UnormFixed(meshData->vertices[vertexIndex].position, T_CHUNK_CELLS));
	int3 mn = I3Min(p0i, p1i);
	int3 mx = I3Max(p0i, p1i);
    const f32 epsilon = 0.01f;

    if (vertex.x < mn.x - epsilon || vertex.x > mx.x + epsilon ||
        vertex.y < mn.y - epsilon || vertex.y > mx.y + epsilon ||
        vertex.z < mn.z - epsilon || vertex.z > mx.z + epsilon)
    {
        return false;
    }

    if (p0i.x == p1i.x && Absf32(vertex.x - (f32)p0i.x) > epsilon) return false;
    if (p0i.y == p1i.y && Absf32(vertex.y - (f32)p0i.y) > epsilon) return false;
    if (p0i.z == p1i.z && Absf32(vertex.z - (f32)p0i.z) > epsilon) return false;
    return true;
}

static bool tMarchingVertexForEdge(tMarchingMesher* mesher, tMeshData* meshData,
                                   int3 cellPos, const f32 cellValues[8], u32 edge,
                                   s32* edgeCache, u32* outVertexIndex)
{
    u32 cacheOffset = tMarchingEdgeCacheOffset(edge, cellPos);
    u32 cornerIdx0 = tMCEdgeCorners[edge][0];
    u32 cornerIdx1 = tMCEdgeCorners[edge][1];
    f32 density0 = cellValues[cornerIdx0];
    f32 density1 = cellValues[cornerIdx1];
    int3 p0i = I3Add(cellPos, tMCCornerOffset[cornerIdx0]);
    int3 p1i = I3Add(cellPos, tMCCornerOffset[cornerIdx1]);
    if (tMarchingCachedVertexMatchesEdge(mesher, meshData, edgeCache[cacheOffset], p0i, p1i))
    {
        *outVertexIndex = (u32)edgeCache[cacheOffset];
        return true;
    }

    float3 p0 = ToFloat3(p0i);
    float3 p1 = ToFloat3(p1i);

    float3 midLocal = F3MulF(F3Add(p0, p1), 0.5f);
    float3 midWorld = tMesherWorldFromLocal(mesher, midLocal);
    f32 midDensity = tMesherGeneratorAt(mesher, midWorld);
    if (tMarchingMesherSign(midDensity) == tMarchingMesherSign(density0)) {
        p0 = midLocal;
        density0 = midDensity;
    }
    else {
        p1 = midLocal;
        density1 = midDensity;
    }

    f32 t0 = density1 / (density1 - density0);
    f32 t1 = 1.0f - t0;
    float3 vertex = F3Add(F3MulF(p0, t0), F3MulF(p1, t1));
    float3 normal0 = tMesherNormalAtSample(mesher, p0i.x + 1, p0i.y + 1, p0i.z + 1);
    float3 normal1 = tMesherNormalAtSample(mesher, p1i.x + 1, p1i.y + 1, p1i.z + 1);
    float3 normal = F3NormSafe(F3Add(normal0, normal1));

    s32 vertexIndex = meshData->numVertices;
    if (!tMarchingEmitVertex(meshData, vertex, normal))
        return false;

    edgeCache[cacheOffset] = vertexIndex;
    *outVertexIndex = (u32)vertexIndex;
    return true;
}

static bool tMesherMarchingCubes(tMarchingMesher* mesher)
{
    tMeshData* meshData = mesher->meshData;
    bool result = true;
    for (s32 z = 0; z < T_CHUNK_CELLS && result; z++)
    for (s32 y = 0; y < T_CHUNK_CELLS && result; y++)
    for (s32 x = 0; x < T_CHUNK_CELLS && result; x++)
    {
        int3 cellPos = { x, y, z };
        f32 cellValues[8];
        s32 caseCode = 0;
        for (u32 i = 0; i < 8u; i++)
        {
            int3 sample = I3AddI(I3Add(cellPos, tMCCornerOffset[i]), 1);
            cellValues[i] = tMesherDensityAt(mesher, sample.x, sample.y, sample.z);
            if (cellValues[i] < 0.0f)
                caseCode |= 1 << i;
        }

        if (caseCode == 0 || caseCode == 255)
            continue;

        const s8* triangles = tMCTriTable[caseCode];
        for (u32 i = 0; i < 16u && triangles[i] >= 0; i += 3u)
        {
            u32 vertexIndex[3];
            for (u32 k = 0; k < 3u; k++)
            {
                if (!tMarchingVertexForEdge(mesher, meshData, cellPos, cellValues,
                                            (u32)triangles[i + k], mesher->edgeCache, &vertexIndex[k]))
                {
                    result = false;
                    break;
                }
            }
            if (!result)
                break;

            if (!tMeshDataPushIndex(meshData, vertexIndex[0]) ||
                !tMeshDataPushIndex(meshData, vertexIndex[1]) ||
                !tMeshDataPushIndex(meshData, vertexIndex[2]))
            {
                result = false;
                break;
            }
        }
    }

    return result;
}

bool tMesherMesh(const tDensityGenerator* generator, const s8* density, tBuildJob* job)
{
    tMeshData* meshData = &job->scratchMesh;
    if (!generator || !density || !meshData)
    {
        AX_WARN("marching cubes mesher failed: invalid argument");
        return false;
    }

    tMarchingMesher mesher = {0};
    mesher.generator = generator;
    mesher.chunkMin = job->min;
    mesher.densityData = density;
    mesher.meshData = meshData;
    const u32 pointCount = (u32)((T_CHUNK_CELLS + 1) * (T_CHUNK_CELLS + 1) * (T_CHUNK_CELLS + 1));
    mesher.edgeCache = (s32*)ArenaPushGlobal(sizeof(s32) * pointCount * 3u);
    if (!mesher.edgeCache)
    {
        AX_WARN("marching cubes edge cache allocation failed");
        return false;
    }
    tMesherFillS32(mesher.edgeCache, pointCount * 3u, -1);
    tMeshDataClear(meshData);
    return tMesherMarchingCubes(&mesher);
}
