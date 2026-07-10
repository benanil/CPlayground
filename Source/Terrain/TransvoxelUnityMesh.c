#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Platform.h"

bool tMeshDataInit(tMeshData* data, size_t vertexCapacity, size_t indexCapacity, size_t secondaryVertexCapacity)
{
    data->vertices          = ArrayCreatePrealloc(tVertexData, vertexCapacity);
    data->indices           = ArrayCreatePrealloc(u32, indexCapacity);
    data->secondaryVertices = ArrayCreatePrealloc(tSecondaryVertexData, secondaryVertexCapacity);

    if (!data->vertices || !data->indices || !data->secondaryVertices)
    {
        AX_WARN("transvoxel unity mesh data init failed: allocation failed");
        tMeshDataDestroy(data);
        return false;
    }

    return true;
}

void tMeshDataDestroy(tMeshData* data)
{
    ArrayDestroy(data->vertices);
    ArrayDestroy(data->indices);
    ArrayDestroy(data->secondaryVertices);
    *data = (tMeshData){0};
}

void tMeshDataClear(tMeshData* data)
{
    if (data->vertices) ArrayFieldSet(data->vertices, ArrayField_Length, 0);
    if (data->indices) ArrayFieldSet(data->indices, ArrayField_Length, 0);
    if (data->secondaryVertices) ArrayFieldSet(data->secondaryVertices, ArrayField_Length, 0);
}

bool tMeshDataPushVertex(tMeshData* data, tVertexData vertex)
{
    size_t oldCount = ArrayLength(data->vertices);
    ArrayPush(data->vertices, vertex);
    return ArrayLength(data->vertices) == oldCount + 1;
}

bool tMeshDataPushIndex(tMeshData* data, u32 index)
{
    size_t oldCount = ArrayLength(data->indices);
    ArrayPush(data->indices, index);
    return ArrayLength(data->indices) == oldCount + 1;
}

bool tMeshDataPushSecondaryVertex(tMeshData* data, tSecondaryVertexData vertex)
{
    size_t oldCount = ArrayLength(data->secondaryVertices);
    ArrayPush(data->secondaryVertices, vertex);
    return ArrayLength(data->secondaryVertices) == oldCount + 1;
}

bool tMeshDataApplySecondaryVertices(tMeshData* data, s32 neighboursMask)
{
    size_t vertexCount = ArrayLength(data->vertices);
    size_t secondaryCount = ArrayLength(data->secondaryVertices);
    for (size_t i = 0; i < secondaryCount; i++)
    {
        tSecondaryVertexData secondary = data->secondaryVertices[i];
        if ((secondary.vertexMask & (u16)neighboursMask) != secondary.vertexMask)
            continue;

        if ((size_t)secondary.vertexIndex >= vertexCount)
        {
            AX_WARN("transvoxel unity mesh secondary apply skipped: vertex index out of range");
            continue;
        }

        data->vertices[secondary.vertexIndex].position = secondary.position;
    }
    return true;
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
    size_t sourceIndexCount = ArrayLength(data->indices);
    size_t vertexCount = ArrayLength(data->vertices);
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
        if (data->vertices && ArrayLength(data->vertices) > 0)
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

bool tMeshDataContainerApplySecondaryVertices(tMeshDataContainer* container, s32 neighboursMask)
{
    bool result = true;
    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
        result = tMeshDataApplySecondaryVertices(&container->mesh[i], neighboursMask) && result;
    return result;
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
    if (!tMeshDataContainerApplySecondaryVertices(job->meshData, job->neighboursMask))
        return false;

    bool result = true;
    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
    {
        job->validIndices[i] = tMeshDataBuildValidIndices(&job->meshData->mesh[i]);
        result = (job->validIndices[i] != NULL) && result;
    }

    return result;
}
