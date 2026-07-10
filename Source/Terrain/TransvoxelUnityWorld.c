#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Memory.h"
#include "Include/Platform.h"

static void* tPoolsSpawnMeshDataContainer(void* userData)
{
    tPools* pools = (tPools*)userData;
    tMeshDataContainer* container = (tMeshDataContainer*)AllocateTLSFGlobal(sizeof(tMeshDataContainer));
    if (!tMeshDataContainerInit(container, pools->meshVertexCapacity, pools->meshIndexCapacity, pools->meshSecondaryVertexCapacity))
    {
        DeAllocateTLSFGlobal(container);
        return NULL;
    }
    return container;
}

static void tPoolsDestroyMeshDataContainer(void* item, void* userData)
{
    (void)userData;
    tMeshDataContainer* container = (tMeshDataContainer*)item;
    if (!container)
        return;

    tMeshDataContainerDestroy(container);
    DeAllocateTLSFGlobal(container);
}

static void* tPoolsSpawnDensityData(void* userData)
{
    tPools* pools = (tPools*)userData;
    f32* densityData = ArrayCreatePrealloc(f32, pools->densitySampleCount);
    if (!densityData)
    {
        AX_WARN("transvoxel unity pools density spawn failed: allocation failed");
        return NULL;
    }

    ArrayFieldSet(densityData, ArrayField_Length, pools->densitySampleCount);
    return densityData;
}

static void tPoolsDestroyDensityData(void* item, void* userData)
{
    (void)userData;
    ArrayDestroy((f32*)item);
}

bool tPoolsInit(tPools* pools, tWorldSettings settings, u32 poolCount,
                size_t meshVertexCapacity, size_t meshIndexCapacity, size_t meshSecondaryVertexCapacity)
{
    if (!pools || settings.chunkSize <= 0)
    {
        AX_WARN("transvoxel unity pools init failed: invalid argument");
        return false;
    }

    *pools = (tPools){0};
    pools->meshVertexCapacity = meshVertexCapacity;
    pools->meshIndexCapacity = meshIndexCapacity;
    pools->meshSecondaryVertexCapacity = meshSecondaryVertexCapacity;

    s32 densitySize = settings.chunkSize + 3;
    pools->densitySampleCount = (size_t)densitySize * (size_t)densitySize * (size_t)densitySize;

    if (!tPoolInit(&pools->meshDataContainers, poolCount, tPoolsSpawnMeshDataContainer, tPoolsDestroyMeshDataContainer, pools))
    {
        tPoolsDestroy(pools);
        return false;
    }

    if (!tPoolInit(&pools->densityData, poolCount, tPoolsSpawnDensityData, tPoolsDestroyDensityData, pools))
    {
        tPoolsDestroy(pools);
        return false;
    }

    return true;
}

void tPoolsDestroy(tPools* pools)
{
    if (!pools)
        return;

    tPoolDestroy(&pools->meshDataContainers);
    tPoolDestroy(&pools->densityData);
    *pools = (tPools){0};
}

tMeshDataContainer* tPoolsGetMeshDataContainer(tPools* pools)
{
    if (!pools)
    {
        AX_WARN("transvoxel unity pools mesh data get failed: null pools");
        return NULL;
    }

    tMeshDataContainer* container = (tMeshDataContainer*)tPoolGet(&pools->meshDataContainers);
    if (container)
        tMeshDataContainerClear(container);

    return container;
}

bool tPoolsAddMeshDataContainer(tPools* pools, tMeshDataContainer* container)
{
    if (!pools || !container)
    {
        AX_WARN("transvoxel unity pools mesh data add failed: invalid argument");
        return false;
    }

    tMeshDataContainerClear(container);
    return tPoolAdd(&pools->meshDataContainers, container);
}

f32* tPoolsGetDensityData(tPools* pools)
{
    if (!pools)
    {
        AX_WARN("transvoxel unity pools density get failed: null pools");
        return NULL;
    }

    f32* densityData = (f32*)tPoolGet(&pools->densityData);
    if (densityData)
        ArrayFieldSet(densityData, ArrayField_Length, pools->densitySampleCount);

    return densityData;
}

bool tPoolsAddDensityData(tPools* pools, f32* densityData)
{
    if (!pools || !densityData)
    {
        AX_WARN("transvoxel unity pools density add failed: invalid argument");
        return false;
    }

    ArrayFieldSet(densityData, ArrayField_Length, pools->densitySampleCount);
    return tPoolAdd(&pools->densityData, densityData);
}

bool tWorldStateInit(tWorldState* worldState, tWorldSettings settings, u32 reserveChunks, size_t updateCapacity,
                     u32 poolCount, size_t meshVertexCapacity, size_t meshIndexCapacity, size_t meshSecondaryVertexCapacity)
{
    if (!worldState)
    {
        AX_WARN("transvoxel unity world state init failed: null state");
        return false;
    }

    *worldState = (tWorldState){0};
    if (!tChunkDataInit(&worldState->chunkData, settings, reserveChunks, updateCapacity))
    {
        tWorldStateDestroy(worldState);
        return false;
    }

	tDensityDataInit(&worldState->densityData, reserveChunks);

    if (!tPoolsInit(&worldState->pools, settings, poolCount, meshVertexCapacity, meshIndexCapacity, meshSecondaryVertexCapacity))
    {
        tWorldStateDestroy(worldState);
        return false;
    }

    return true;
}

void tWorldStateDestroy(tWorldState* worldState)
{
    if (!worldState)
        return;

    tChunkDataDestroy(&worldState->chunkData);
    tDensityDataDestroy(&worldState->densityData);
    tPoolsDestroy(&worldState->pools);
    *worldState = (tWorldState){0};
}

bool tDensityJobCreate(const tWorldSettings* settings, tPools* pools, const tDensityGenerator* generator,
                       tChunkUpdate update, tDensityJob* outJob)
{
    if (!settings || !pools || !generator || !outJob)
    {
        AX_WARN("transvoxel unity density job create failed: invalid argument");
        return false;
    }

    s32 lodScale = 1 << update.lod;
    s32 chunkExtents = (settings->chunkSize >> 1) << update.lod;
    int3 chunkMin = I3SubI(update.chunkPosition, chunkExtents);
    int3 startSamplePos = I3Sub(chunkMin, I3MulI((int3){1, 1, 1}, lodScale));

    f32* densityData = tPoolsGetDensityData(pools);
    if (!densityData)
        return false;

    outJob->sample = tDensityGeneratorSample;
    outJob->userData = (void*)generator;
    outJob->size = settings->chunkSize + 3;
    outJob->step = lodScale;
    outJob->startX = startSamplePos.x;
    outJob->startY = startSamplePos.y;
    outJob->startZ = startSamplePos.z;
    outJob->densityData = densityData;
    return true;
}

bool tUniformityJobCreate(tWorldState* worldState, int3 chunkPosition, u32 index, tUniformityJob* outJob)
{
    if (!worldState || !outJob)
    {
        AX_WARN("transvoxel unity uniformity job create failed: invalid argument");
        return false;
    }

    f32* densityData = tDensityDataGetDataUnchecked(&worldState->densityData, chunkPosition);
    if (!densityData)
    {
        AX_WARN("transvoxel unity uniformity job create failed: missing density data");
        return false;
    }

    outJob->densityData = densityData;
    outJob->chunkUniformState = worldState->chunkData.chunkUniformState;
    outJob->index = index;
    return true;
}

bool tFilterJobCreate(tWorldState* worldState, tFilterJob* outJob)
{
    if (!worldState || !outJob)
    {
        AX_WARN("transvoxel unity filter job create failed: invalid argument");
        return false;
    }

    outJob->chunkUpdates = worldState->chunkData.chunkUpdates;
    outJob->chunkUniformState = worldState->chunkData.chunkUniformState;
    outJob->filteredChunkUpdates = worldState->chunkData.filteredChunkUpdates;
    return true;
}

bool tMaterialJobCreate(const tWorldSettings* settings, tWorldState* worldState, const tMaterialGenerator* generator,
                        tChunkUpdate update, tMaterialJob* outJob)
{
    if (!settings || !worldState || !generator || !outJob)
    {
        AX_WARN("transvoxel unity material job create failed: invalid argument");
        return false;
    }

    tChunk* chunk = tChunkDataFindChunk(&worldState->chunkData, update.chunkPosition);
    if (!chunk)
    {
        AX_WARN("transvoxel unity material job create failed: missing chunk");
        return false;
    }

    outJob->meshData = &chunk->meshData;
    outJob->generator = generator;
    outJob->chunkMin = tGetChunkMin(update.chunkPosition, settings->chunkSize, update.lod);
    return true;
}

bool tMeshCopyJobCreate(tWorldState* worldState, tChunkUpdate update, tMeshCopyJob* outJob)
{
    if (!worldState || !outJob)
    {
        AX_WARN("transvoxel unity mesh copy job create failed: invalid argument");
        return false;
    }

    tChunk* chunk = tChunkDataFindChunk(&worldState->chunkData, update.chunkPosition);
    if (!chunk)
    {
        AX_WARN("transvoxel unity mesh copy job create failed: missing chunk");
        return false;
    }

    *outJob = (tMeshCopyJob){0};
    outJob->meshData = &chunk->meshData;
    outJob->neighboursMask = update.neighboursMask;
    return true;
}

bool tMesherJobCreate(const tWorldSettings* settings, tWorldState* worldState, const tDensityGenerator* generator,
                      tChunkUpdate update, tMesherFn mesh, void* userData, tMesherJob* outJob)
{
    if (!settings || !worldState || !generator || !mesh || !outJob)
    {
        AX_WARN("transvoxel unity mesher job create failed: invalid argument");
        return false;
    }

    f32* densityData = tDensityDataGetDataUnchecked(&worldState->densityData, update.chunkPosition);
    if (!densityData)
    {
        AX_WARN("transvoxel unity mesher job create failed: missing density data");
        return false;
    }

    tMeshDataContainer* meshData = tPoolsGetMeshDataContainer(&worldState->pools);
    if (!meshData)
        return false;

    outJob->generator = generator;
    outJob->chunkMin = tGetChunkMin(update.chunkPosition, settings->chunkSize, update.lod);
    outJob->chunkSize = settings->chunkSize;
    outJob->densityData = densityData;
    outJob->meshData = meshData;
    outJob->lod = update.lod;
    outJob->neighboursMask = update.neighboursMask;
    outJob->mesh = mesh;
    outJob->userData = userData;
    return true;
}
