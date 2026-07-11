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
	{
		AX_WARN("terrain mesh data push vertex failed");
		return false;
	}
	data->vertices[data->numVertices++] = vertex;
	return true;
}

bool tMeshDataPushIndex(tMeshData* data, u32 index)
{
	if (data->numIndices >= data->indexCapacity)
	{
		AX_WARN("terrain mesh data push indexfailed");
		return false;
	}
	data->indices[data->numIndices++] = index;
	return true;
}

bool tMeshDataPushSecondaryVertex(tMeshData* data, tSecondaryVert vertex)
{
	if (data->numSecondaryVert >= data->secondaryCapacity)
	{
		AX_WARN("terrain mesh data push second index failed:");
		return false;
	}
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
    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
        tMeshDataDestroy(&container->mesh[i]);
}

void tMeshDataContainerClear(tMeshDataContainer* container)
{
    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
        tMeshDataClear(&container->mesh[i]);
}

bool tMeshDataContainerHasAnyData(const tMeshDataContainer* container)
{
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
