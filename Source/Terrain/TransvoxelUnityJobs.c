#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Platform.h"

f32 tDensityGeneratorGetValue(const tDensityGenerator* generator, f32 x, f32 y, f32 z)
{
    if (!generator)
    {
        AX_WARN("transvoxel unity density generator failed: null generator");
        return -y;
    }

    f32 heightMap = 0.0f;
    if (generator->heightMapNoise)
        heightMap = generator->heightMapNoise(x, z, generator->heightMapUserData) * generator->heightMapStrength;

    f32 noise3D = 0.0f;
    if (generator->noise3D)
        noise3D = generator->noise3D(x, y, z, generator->noise3DUserData) * generator->noise3DStrength;

    return -y + heightMap + noise3D;
}

f32 tDensityGeneratorSample(f32 x, f32 y, f32 z, void* userData)
{
    return tDensityGeneratorGetValue((const tDensityGenerator*)userData, x, y, z);
}

void tMaterialGeneratorApply(const tMaterialGenerator* generator, tVertexData* vertexData, int3 chunkMin)
{
    if (!generator || !vertexData)
    {
        AX_WARN("transvoxel unity material generator failed: invalid argument");
        return;
    }

    float3 vertexLocal = Vec3Get(vertexData->position);
    float3 vertexPos = F3Add(ToFloat3(chunkMin), vertexLocal);
    float3 normal = Vec3Get(vertexData->normal);

    f32 dot = generator->grassAmount + F3Dot(normal, F3Up());
    f32 topValue = 0.0f;
    if (dot > 0.0f)
        topValue = Powf(dot, generator->grassBlendStrength);

    f32 topMask = 1.0f - topValue;

    f32 stoneValue = 0.0f;
    if (generator->stoneNoise)
        stoneValue = generator->stoneNoise(vertexPos.x, vertexPos.y, vertexPos.z, generator->stoneNoiseUserData);

    stoneValue = (stoneValue + 1.0f) * 0.5f;
    if (stoneValue > generator->stoneThreshold)
        stoneValue = 1.0f;
    else
        stoneValue = 0.0f;

    f32 noiseMask = 1.0f - stoneValue;
    vertexData->materials = VecSetR(0.0f, 1.0f, 2.0f, 0.0f);
    vertexData->blend = VecSetR(
        Saturatef32(topValue),
        Saturatef32(Minf32(topMask, noiseMask)),
        Saturatef32(Minf32(stoneValue, topMask)),
        0.01f);
}

bool tDensityJobExecuteRange(tDensityJob* job, u32 begin, u32 end)
{
    if (!job || !job->sample || !job->densityData || job->size <= 0 || job->step <= 0)
    {
        AX_WARN("transvoxel unity density job failed: invalid argument");
        return false;
    }

    u32 count = (u32)ArrayLength(job->densityData);
    if (end > count || begin > end)
    {
        AX_WARN("transvoxel unity density job failed: invalid range");
        return false;
    }

    s32 size = job->size;
    s32 sizeSq = size * size;
    for (u32 index = begin; index < end; index++)
    {
        s32 z = (s32)(index % (u32)size);
        s32 y = (s32)((index / (u32)size) % (u32)size);
        s32 x = (s32)(index / (u32)sizeSq);

        job->densityData[index] = job->sample(
            (f32)(job->startX + x * job->step),
            (f32)(job->startY + y * job->step),
            (f32)(job->startZ + z * job->step),
            job->userData);
    }

    return true;
}

bool tDensityJobExecute(tDensityJob* job)
{
    if (!job || !job->densityData)
    {
        AX_WARN("transvoxel unity density job execute failed: invalid argument");
        return false;
    }

    return tDensityJobExecuteRange(job, 0u, (u32)ArrayLength(job->densityData));
}

static void tDensityJobParallelFor(u32 begin, u32 end, void* userData)
{
    tDensityJobExecuteRange((tDensityJob*)userData, begin, end);
}

bool tDensityJobExecuteParallel(tDensityJob* job, u32 minItemsPerWorker)
{
    if (!job || !job->densityData)
    {
        AX_WARN("transvoxel unity density job parallel execute failed: invalid argument");
        return false;
    }

    ParallelFor((u32)ArrayLength(job->densityData), minItemsPerWorker, tDensityJobParallelFor, job);
    return true;
}

bool tUniformityJobExecute(tUniformityJob* job)
{
    if (!job || !job->densityData || !job->chunkUniformState)
    {
        AX_WARN("transvoxel unity uniformity job failed: invalid argument");
        return false;
    }

    size_t densityCount = ArrayLength(job->densityData);
    if (densityCount == 0 || job->index >= ArrayLength(job->chunkUniformState))
    {
        AX_WARN("transvoxel unity uniformity job failed: invalid data size");
        return false;
    }

    bool allAboveZero = job->densityData[0] >= 0.0f;
    bool allBelowZero = job->densityData[0] < 0.0f;
    for (size_t i = 1; i < densityCount; i++)
    {
        f32 densityValue = job->densityData[i];
        allAboveZero = allAboveZero && densityValue >= 0.0f;
        allBelowZero = allBelowZero && densityValue < 0.0f;

        if (!allAboveZero && !allBelowZero)
            break;
    }

    job->chunkUniformState[job->index] = allAboveZero || allBelowZero;
    return true;
}

bool tFilterJobExecute(tFilterJob* job)
{
    if (!job || !job->chunkUpdates || !job->filteredChunkUpdates || !job->chunkUniformState)
    {
        AX_WARN("transvoxel unity filter job failed: invalid argument");
        return false;
    }

    size_t updateCount = ArrayLength(job->chunkUpdates);
    if (ArrayLength(job->chunkUniformState) < updateCount)
    {
        AX_WARN("transvoxel unity filter job failed: uniform state count mismatch");
        return false;
    }

    for (size_t i = 0; i < updateCount; i++)
    {
        tChunkUpdate chunkUpdate = job->chunkUpdates[i];
        if (chunkUpdate.updateType != tChunkUpdateType_Create || !job->chunkUniformState[i])
            ArrayPush(job->filteredChunkUpdates, chunkUpdate);
    }

    return true;
}

static bool tMaterialJobGenerateForMesh(tMaterialJob* job, tMeshData* meshData)
{
    if (!job || !meshData || !meshData->vertices)
    {
        AX_WARN("transvoxel unity material job failed: invalid mesh");
        return false;
    }

    size_t vertexCount = ArrayLength(meshData->vertices);
    for (size_t i = 0; i < vertexCount; i++)
        tMaterialGeneratorApply(job->generator, &meshData->vertices[i], job->chunkMin);

    return true;
}

bool tMaterialJobExecute(tMaterialJob* job)
{
    if (!job || !job->meshData || !job->generator)
    {
        AX_WARN("transvoxel unity material job failed: invalid argument");
        return false;
    }

    bool result = true;
    result = tMaterialJobGenerateForMesh(job, tMeshDataContainerGet(job->meshData, tMeshDataSlot_Main)) && result;
    result = tMaterialJobGenerateForMesh(job, tMeshDataContainerGet(job->meshData, tMeshDataSlot_LeftTransition)) && result;
    result = tMaterialJobGenerateForMesh(job, tMeshDataContainerGet(job->meshData, tMeshDataSlot_RightTransition)) && result;
    result = tMaterialJobGenerateForMesh(job, tMeshDataContainerGet(job->meshData, tMeshDataSlot_ForwardTransition)) && result;
    result = tMaterialJobGenerateForMesh(job, tMeshDataContainerGet(job->meshData, tMeshDataSlot_BackTransition)) && result;
    result = tMaterialJobGenerateForMesh(job, tMeshDataContainerGet(job->meshData, tMeshDataSlot_UpTransition)) && result;
    result = tMaterialJobGenerateForMesh(job, tMeshDataContainerGet(job->meshData, tMeshDataSlot_DownTransition)) && result;
    return result;
}

bool tMesherJobExecute(tMesherJob* job)
{
    if (!job || !job->mesh || !job->meshData || !job->densityData || !job->generator)
    {
        AX_WARN("transvoxel unity mesher job failed: invalid argument");
        return false;
    }

    return job->mesh(job->generator, job->chunkMin, job->chunkSize, job->densityData,
                     job->lod, job->neighboursMask, job->meshData, job->userData);
}
