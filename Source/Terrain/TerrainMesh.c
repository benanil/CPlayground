#include "TerrainInternal.h"
#include "Include/Memory.h"
#include "Include/Platform.h"
#include "Include/Graphics.h"

static bool tMeshDataInitCapacity(tMeshData* data, s32 vertexCapacity, s32 indexCapacity)
{
    *data = (tMeshData){0};
	u32 r0 = GeometryHeapAlloc(GeometryBuffer_TerrainVert, (u32)vertexCapacity, (void**)&data->vertices);
    u32 r1 = GeometryHeapAlloc(GeometryBuffer_TerrainIndex, (u32)indexCapacity , (void**)&data->indices);

    if (r0 == GEOMETRY_ALLOC_FAIL || r1 == GEOMETRY_ALLOC_FAIL)
    {
        AX_WARN("terrain mesh data init failed: allocation failed");
        tMeshDataDestroy(data);
        return false;
    }
    data->vertexCapacity    = vertexCapacity;
    data->indexCapacity     = indexCapacity;
    return true;
}

bool tMeshDataInit(tMeshData* data)
{
    return tMeshDataInitCapacity(data, (s32)T_CHUNK_VERTEX_CAP, (s32)T_CHUNK_INDEX_CAP);
}

void tMeshDataDestroy(tMeshData* data)
{
    if (data->vertices) GeometryHeapFree(GeometryBuffer_TerrainVert, data->vertices);
    if (data->indices) GeometryHeapFree(GeometryBuffer_TerrainIndex , data->indices);
    *data = (tMeshData){0};
}

void tMeshDataClear(tMeshData* data)
{
	data->numVertices = 0;
	data->numIndices = 0;
}

bool tMeshDataPushVertex(tMeshData* data, tVertex vertex)
{
	if (!data || !data->vertices || data->numVertices >= data->vertexCapacity) {
		AX_WARN("terrain mesh data push vertex failed");
		return false;
	}
	data->vertices[data->numVertices++] = vertex;
	return true;
}

bool tMeshDataPushIndex(tMeshData* data, u32 index)
{
	if (!data || !data->indices || data->numIndices >= data->indexCapacity) {
		AX_WARN("terrain mesh data push indexfailed");
		return false;
	}
	data->indices[data->numIndices++] = (u16)index;
	return true;
}