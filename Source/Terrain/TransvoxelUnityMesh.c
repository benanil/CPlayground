#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Platform.h"
#include "Include/Graphics.h"

bool tMeshDataInit(tMeshData* data, size_t vertexCapacity, size_t indexCapacity, size_t secondaryVertexCapacity)
{
    *data = (tMeshData){0};
    u32 r0 = GeometryHeapAlloc(GeometryBuffer_TerrainVertNew, (u32)vertexCapacity         , (void**)&data->vertices     );
    u32 r1 = GeometryHeapAlloc(GeometryBuffer_TerrainIndex2 , (u32)indexCapacity          , (void**)&data->indices      );
    u32 r2 = GeometryHeapAlloc(GeometryBuffer_TerrainSecond , (u32)secondaryVertexCapacity, (void**)&data->secondaryVert);

    if (!data->vertices || !data->indices || !data->secondaryVert
		|| r0 == GEOMETRY_ALLOC_FAIL || r1 == GEOMETRY_ALLOC_FAIL || r2 == GEOMETRY_ALLOC_FAIL )
    {
        AX_WARN("transvoxel unity mesh data init failed: allocation failed");
        tMeshDataDestroy(data);
        return false;
    }
    data->vertexCapacity    = (s32)vertexCapacity;
    data->indexCapacity     = (s32)indexCapacity;
    data->secondaryCapacity = (s32)secondaryVertexCapacity;
    return true;
}

void tMeshDataDestroy(tMeshData* data)
{
    GeometryHeapFree(GeometryBuffer_TerrainVertNew, data->vertices);
    GeometryHeapFree(GeometryBuffer_TerrainIndex2 , data->indices);
    GeometryHeapFree(GeometryBuffer_TerrainSecond , data->secondaryVert);
    *data = (tMeshData){0};
}

void tMeshDataClear(tMeshData* data)
{
	data->numVertices = 0;
	data->numIndices = 0;
	data->numSecondaryVert = 0;
}

bool tMeshDataPushVertex(tMeshData* data, tVertexData vertex)
{
	if (data->numVertices >= data->vertexCapacity)
		return false;
	data->vertices[data->numVertices++] = vertex;
	return true;
}

bool tMeshDataPushIndex(tMeshData* data, u32 index)
{
	if (data->numIndices >= data->indexCapacity)
		return false;
	data->indices[data->numIndices++] = index;
	return true;
}

bool tMeshDataPushSecondaryVertex(tMeshData* data, tSecondaryVert vertex)
{
	if (data->numSecondaryVert >= data->secondaryCapacity)
		return false;
	data->secondaryVert[data->numSecondaryVert++] = vertex;
	return true;
}

void tMeshDataApplySecondaryVertices(tMeshData* data, s32 neighboursMask)
{
    size_t vertexCount = data->numVertices;
    size_t secondaryCount = data->numSecondaryVert;
    for (size_t i = 0; i < secondaryCount; i++)
    {
        tSecondaryVert secondary = data->secondaryVert[i];
        if ((secondary.vertexMask & (u16)neighboursMask) != secondary.vertexMask)
            continue;

        if ((size_t)secondary.vertexIndex >= vertexCount)
        {
            AX_WARN("transvoxel unity mesh secondary apply skipped: vertex index out of range");
            continue;
        }

        data->vertices[secondary.vertexIndex].position = secondary.position;
    }
}

static bool tMeshDataTriangleValid(const tVertexData* vertices, u32 ia, u32 ib, u32 ic)
{
    float3 a = Vec3Get(vertices[ia].position);
    float3 b = Vec3Get(vertices[ib].position);
    float3 c = Vec3Get(vertices[ic].position);

    if (F3Approx(a, b) || F3Approx(b, c) || F3Approx(c, a))
        return false;

    float3 ab = F3Sub(a, b);
    float3 ac = F3Sub(a, c);
    float3 cross = F3Cross(&ab, &ac);
    return !F3Approx(cross, F3Zero());
}

u32* tMeshDataBuildValidIndices(const tMeshData* data)
{
    size_t sourceIndexCount = data->numIndices;
    size_t vertexCount = data->numVertices;
    u32* result = ArrayCreatePrealloc(u32, sourceIndexCount);
    if (!result)
    {
        AX_WARN("transvoxel unity mesh valid index build failed: allocation failed");
        return NULL;
    }

    for (size_t i = 0; i + 2 < sourceIndexCount; i += 3)
    {
        u32 ia = data->indices[i + 0];
        u32 ib = data->indices[i + 1];
        u32 ic = data->indices[i + 2];
        if ((size_t)ia >= vertexCount || (size_t)ib >= vertexCount || (size_t)ic >= vertexCount)
        {
            AX_WARN("transvoxel unity mesh valid index build skipped: index out of range");
            continue;
        }

        if (!tMeshDataTriangleValid(data->vertices, ia, ib, ic))
            continue;

        ArrayPush(result, ia);
        ArrayPush(result, ib);
        ArrayPush(result, ic);
    }

    return result;
}

bool tMeshDataContainerInit(tMeshDataContainer* container, size_t vertexCapacity, size_t indexCapacity, size_t secondaryVertexCapacity)
{
    *container = (tMeshDataContainer){0};
    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
    {
        if (!tMeshDataInit(&container->mesh[i], vertexCapacity, indexCapacity, secondaryVertexCapacity))
        {
            tMeshDataContainerDestroy(container);
            return false;
        }
    }
    return true;
}

void tMeshDataContainerDestroy(tMeshDataContainer* container)
{
    if (!container) return;

    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
        tMeshDataDestroy(&container->mesh[i]);
}

void tMeshDataContainerClear(tMeshDataContainer* container)
{
    if (!container) return;

    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
        tMeshDataClear(&container->mesh[i]);
}

bool tMeshDataContainerHasAnyData(const tMeshDataContainer* container)
{
    if (!container) return false;

    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
    {
        const tMeshData* data = &container->mesh[i];
        if (data->vertices && data->numVertices > 0)
            return true;
    }
    return false;
}

tMeshData* tMeshDataContainerGet(tMeshDataContainer* container, tMeshDataSlot slot)
{
    return &container->mesh[slot];
}

const tMeshData* tMeshDataContainerGetConst(const tMeshDataContainer* container, tMeshDataSlot slot)
{
    return &container->mesh[slot];
}

void tMeshDataContainerApplySecondaryVertices(tMeshDataContainer* container, s32 neighboursMask)
{
    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
        tMeshDataApplySecondaryVertices(&container->mesh[i], neighboursMask);
}

void tMeshCopyJobClearResults(tMeshCopyJob* job)
{
    if (!job)
        return;

    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
    {
        ArrayDestroy(job->validIndices[i]);
        job->validIndices[i] = NULL;
    }
}

bool tMeshCopyJobExecute(tMeshCopyJob* job)
{
    if (!job || !job->meshData)
    {
        AX_WARN("transvoxel unity mesh copy job failed: invalid argument");
        return false;
    }

    tMeshCopyJobClearResults(job);
    tMeshDataContainerApplySecondaryVertices(job->meshData, job->neighboursMask);

    bool result = true;
    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
    {
        job->validIndices[i] = tMeshDataBuildValidIndices(&job->meshData->mesh[i]);
        result = (job->validIndices[i] != NULL) && result;
    }

    return result;
}
