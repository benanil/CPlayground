#include "TerrainInternal.h"
#include "Source/Terrain/Transvoxel.h"
#include "Include/Memory.h"
#include "Include/Platform.h"
#include "Include/Graphics.h"

bool tMeshDataInit(tMeshData* data)
{
    *data = (tMeshData){0};
	u32 r0 = GeometryHeapAlloc(GeometryBuffer_TerrainVert , T_CHUNK_VERTEX_CAP, (void* *)&data->vertices);
    u32 r1 = GeometryHeapAlloc(GeometryBuffer_TerrainIndex, T_CHUNK_INDEX_CAP , (void**)&data->indices);
	data->secondaryVert = (tSecondaryVert*)AllocateTLSFGlobal(sizeof(tSecondaryVert) * T_CHUNK_SECONDARY_VERTEX_CAP);

    if (r0 == GEOMETRY_ALLOC_FAIL || r1 == GEOMETRY_ALLOC_FAIL || !data->secondaryVert)
    {
        AX_WARN("transvoxel unity mesh data init failed: allocation failed");
        tMeshDataDestroy(data);
        return false;
    }
    data->vertexCapacity    = (s32)T_CHUNK_VERTEX_CAP;
    data->indexCapacity     = (s32)T_CHUNK_INDEX_CAP;
    data->secondaryCapacity = (s32)T_CHUNK_SECONDARY_VERTEX_CAP;
    return true;
}

void tMeshDataDestroy(tMeshData* data)
{
    if (data->vertices) GeometryHeapFree(GeometryBuffer_TerrainVert, data->vertices);
    if (data->indices) GeometryHeapFree(GeometryBuffer_TerrainIndex , data->indices);
    if (data->secondaryVert) DeAllocateTLSFGlobal(data->secondaryVert);
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
	if (!data || !data->vertices) {
		AX_WARN("terrain mesh data push vertex failed: invalid mesh data");
		return false;
	}
	if (data->numVertices >= data->vertexCapacity) {
		AX_WARN("terrain mesh data push vertex failed");
		return false;
	}
	data->vertices[data->numVertices++] = vertex;
	return true;
}

bool tMeshDataPushIndex(tMeshData* data, u32 index)
{
	if (!data || !data->indices) {
		AX_WARN("terrain mesh data push index failed: invalid mesh data");
		return false;
	}
	if (data->numIndices >= data->indexCapacity) {
		AX_WARN("terrain mesh data push indexfailed");
		return false;
	}
	data->indices[data->numIndices++] = index;
	return true;
}

bool tMeshDataPushSecondaryVertex(tMeshData* data, tSecondaryVert vertex)
{
	if (!data || !data->secondaryVert) {
		AX_WARN("terrain mesh data push second index failed: invalid mesh data");
		return false;
	}
	if (data->numSecondaryVert >= data->secondaryCapacity) {
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

        if ((size_t)secondary.vertexIndex >= vertexCount) {
            AX_WARN("transvoxel unity mesh secondary apply skipped: vertex index out of range");
            continue;
        }

        data->vertices[secondary.vertexIndex].position = secondary.position;
    }
}

bool tMeshDataContainerInit(tMeshDataContainer* container)
{
    *container = (tMeshDataContainer){0};
	for (u32 i = 0; i < tMeshDataSlot_Count; i++)
    {
        if (!tMeshDataInit(&container->mesh[i]))
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

void tMeshDataContainerApplySecondaryVertices(tMeshDataContainer* container, s32 neighboursMask)
{
    for (u32 i = 0; i < tMeshDataSlot_Count; i++)
        tMeshDataApplySecondaryVertices(&container->mesh[i], neighboursMask);
}
