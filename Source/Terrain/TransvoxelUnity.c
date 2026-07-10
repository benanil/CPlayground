#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Platform.h"

u64 tChunkPositionKey(int3 position)
{
    return ((u64)((u32)(position.x + 0x100000) & 0x1FFFFFu) << 40)
         | ((u64)((u32)(position.y + 0x8000)   & 0xFFFFu)   << 24)
         | ((u64)((u32)(position.z + 0x100000) & 0x1FFFFFu) << 3);
}

s32 tChunkUpdateCompareLODDescending(tChunkUpdate a, tChunkUpdate b)
{
    if (a.lod < b.lod) return 1;
    return a.lod > b.lod ? -1 : 0;
}

tIntBox tIntBoxMake(int3 min, int3 max)
{
    return (tIntBox){ min, max };
}

tIntBox tIntBoxFromExtents(int3 position, s32 extents)
{
    return (tIntBox){ I3SubI(position, extents), I3AddI(position, extents) };
}

bool tIntBoxContainsXYZ(tIntBox box, s32 x, s32 y, s32 z)
{
    return (x >= box.min.x && x < box.max.x)
        && (y >= box.min.y && y < box.max.y)
        && (z >= box.min.z && z < box.max.z);
}

bool tIntBoxContains(tIntBox box, int3 p)
{
    return tIntBoxContainsXYZ(box, p.x, p.y, p.z);
}

bool tIntBoxIntersects(tIntBox a, tIntBox b)
{
    return (a.min.x < b.max.x && a.max.x > b.min.x)
        && (a.min.y < b.max.y && a.max.y > b.min.y)
        && (a.min.z < b.max.z && a.max.z > b.min.z);
}

bool tDensityDataInit(tDensityData* densityData, u32 reserveCount)
{
    densityData->dataByChunkPosition = HMCreate(reserveCount, sizeof(tDensityDataValue));
    return true;
}

void tDensityDataDestroy(tDensityData* densityData)
{
    tDensityDataValue* values = (tDensityDataValue*)densityData->dataByChunkPosition.values;
    for (u32 i = 0; i < densityData->dataByChunkPosition.count; i++)
        ArrayDestroy(values[i].values);

    HMDestroy(&densityData->dataByChunkPosition);
}

void tDensityDataStoreDataUnchecked(tDensityData* densityData, int3 chunkPosition, f32* values)
{
    tDensityDataValue value = { values };
	HMInsert(&densityData->dataByChunkPosition, tChunkPositionKey(chunkPosition), &value);
}

f32* tDensityDataGetDataUnchecked(const tDensityData* densityData, int3 chunkPosition)
{
    tDensityDataValue* value = (tDensityDataValue*)HMFind(&densityData->dataByChunkPosition, tChunkPositionKey(chunkPosition));
	return value->values;
}

bool tDensityDataRemoveData(tDensityData* densityData, int3 chunkPosition)
{
    return HMErase(&densityData->dataByChunkPosition, tChunkPositionKey(chunkPosition));
}

bool tDensityDataTakeData(tDensityData* densityData, int3 chunkPosition, f32** outValues)
{
    u64 key = tChunkPositionKey(chunkPosition);
    tDensityDataValue* value = (tDensityDataValue*)HMFind(&densityData->dataByChunkPosition, key);
    if (!value)
        return false;

    *outValues = value->values;
    value->values = NULL;
    return HMErase(&densityData->dataByChunkPosition, key);
}

static s32 tChunkDataRootDepth(tWorldSettings* settings)
{
    s32 chunkCount = settings->worldSize / settings->chunkSize;
	if (chunkCount * settings->chunkSize != settings->worldSize)
		settings->worldSize = chunkCount * settings->chunkSize;

    return Log2u32((u32)chunkCount);
}

bool tChunkDataInit(tChunkData* chunkData, tWorldSettings settings, u32 reserveChunks, size_t updateCapacity)
{
	settings.chunkSize = Maxs32(settings.chunkSize, 1);
	settings.worldSize = Maxs32(settings.worldSize, settings.chunkSize);
    s32 rootDepth = tChunkDataRootDepth(&settings);

    *chunkData = (tChunkData){0};
    chunkData->chunkMap = HMCreate(reserveChunks, sizeof(tChunk));
	tOctreeInit(&chunkData->chunkTree, (int3) { 0, 0, 0 }, settings.worldSize, rootDepth, reserveChunks);

    chunkData->pendingChunks        = ArrayCreatePrealloc(tChunk, updateCapacity);
    chunkData->chunksToRemove       = ArrayCreatePrealloc(int3, updateCapacity);
    chunkData->chunkUpdates         = ArrayCreatePrealloc(tChunkUpdate, updateCapacity);
    chunkData->filteredChunkUpdates = ArrayCreatePrealloc(tChunkUpdate, updateCapacity);
    chunkData->chunkUniformState    = ArrayCreatePrealloc(bool, updateCapacity);
	// todo meshes to clear here
    if (!chunkData->chunkUpdates || !chunkData->filteredChunkUpdates || !chunkData->chunkUniformState ||
        !chunkData->chunksToRemove || !chunkData->pendingChunks)
    {
        AX_WARN("transvoxel unity chunk data init failed: allocation failed");
        tChunkDataDestroy(chunkData);
        return false;
    }

    return true;
}

void tChunkDataDestroy(tChunkData* chunkData)
{
    tChunk* chunks = (tChunk*)chunkData->chunkMap.values;
    for (u32 i = 0; i < chunkData->chunkMap.count; i++)
    {
        tMeshDataContainerDestroy(&chunks[i].meshData);
        for (u32 j = 0; j < tMeshDataSlot_Count; j++)
            ArrayDestroy(chunks[i].validIndices[j]);
    }

    HMDestroy(&chunkData->chunkMap);
    tOctreeDestroy(&chunkData->chunkTree);
    ArrayDestroy(chunkData->chunkUpdates);
    ArrayDestroy(chunkData->filteredChunkUpdates);
    ArrayDestroy(chunkData->chunkUniformState);
    ArrayDestroy(chunkData->chunksToRemove);
    ArrayDestroy(chunkData->pendingChunks);
    *chunkData = (tChunkData){0};
}

void tChunkDataClearUpdates(tChunkData* chunkData)
{
    if (chunkData->chunkUpdates) ArrayFieldSet(chunkData->chunkUpdates, ArrayField_Length, 0);
    if (chunkData->filteredChunkUpdates) ArrayFieldSet(chunkData->filteredChunkUpdates, ArrayField_Length, 0);
    if (chunkData->chunkUniformState) ArrayFieldSet(chunkData->chunkUniformState, ArrayField_Length, 0);
}

tChunk* tChunkDataFindChunk(tChunkData* chunkData, int3 chunkPosition)
{
    return (tChunk*)HMFind(&chunkData->chunkMap, tChunkPositionKey(chunkPosition));
}

bool tChunkDataStoreChunk(tChunkData* chunkData, tChunk chunk)
{
    u64 key = tChunkPositionKey(chunk.position);
    if (HMContains(&chunkData->chunkMap, key))
    {
        AX_WARN("transvoxel unity chunk store failed: chunk already exists");
        return false;
    }

    if (!HMInsert(&chunkData->chunkMap, key, &chunk))
    {
        AX_WARN("transvoxel unity chunk store failed: map insert failed");
        return false;
    }

    return true;
}

bool tChunkDataRemoveChunk(tChunkData* chunkData, int3 chunkPosition)
{
    tChunk* chunk = tChunkDataFindChunk(chunkData, chunkPosition);
    if (chunk)
    {
        tMeshDataContainerDestroy(&chunk->meshData);
        for (u32 i = 0; i < tMeshDataSlot_Count; i++)
            ArrayDestroy(chunk->validIndices[i]);
    }

    return HMErase(&chunkData->chunkMap, tChunkPositionKey(chunkPosition));
}

int3 tGetChunkMin(int3 chunkPosition, s32 chunkSize, s32 lod)
{
    s32 chunkExtents = (chunkSize >> 1) << lod;
    return I3SubI(chunkPosition, chunkExtents);
}
