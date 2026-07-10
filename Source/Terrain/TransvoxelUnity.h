#ifndef TRANSVOXEL_UNITY_H
#define TRANSVOXEL_UNITY_H

#include "Include/Common.h"
#include "Include/DataStructures/Array.h"
#include "Include/DataStructures/HashMap.h"
#include "Include/JobSystem.h"
#include "Include/ParallelFor.h"
#include "Math/Vector.h"

#if defined(__cplusplus)
extern "C" {
#endif

// Incremental C port of C:/transvoxel-unity Runtime/Data support types.
// Kept separate from the engine mesher until the port is complete enough to wire in.

typedef struct tIntBox_
{
    int3 min;
    int3 max;
} tIntBox;

typedef struct tOctreeNode_
{
    int3 position;
    s32  extents;
    s32  depth;
    u64  locCode;
} tOctreeNode;

typedef struct tLinearOctree_
{
    HashMap nodeMap; // key: locCode, value: tOctreeNode
    int3    rootPosition;
    s32     rootSize;
    s32     rootDepth;
    s32     leafSize;
} tOctree;

typedef struct tVertexData_
{
    v128f position;
    v128f normal;
    v128f materials;
    v128f blend;
} tVertexData;

typedef struct tSecondaryVertexData_
{
    v128f position;
    u16   vertexMask;
    u16   vertexIndex;
} tSecondaryVert;

typedef struct tMeshData_
{
    tVertexData*  vertices;      // fixed-capacity ranges in the terrain geometry heaps
    u32*          indices;
    tSecondaryVert* secondaryVert;
	s32 numIndices;
	s32 numVertices;
	s32 numSecondaryVert;
	s32 vertexCapacity;          // pushes beyond capacity are dropped (heaps are shared,
	s32 indexCapacity;           // an overrun would corrupt other chunks' meshes)
	s32 secondaryCapacity;
} tMeshData;

typedef enum tMeshDataSlot_
{
    tMeshDataSlot_Main,
    tMeshDataSlot_LeftTransition,
    tMeshDataSlot_DownTransition,
    tMeshDataSlot_BackTransition,
    tMeshDataSlot_RightTransition,
    tMeshDataSlot_UpTransition,
    tMeshDataSlot_ForwardTransition,
    tMeshDataSlot_Count
} tMeshDataSlot;

typedef struct tMeshDataContainer_
{
    tMeshData mesh[tMeshDataSlot_Count];
} tMeshDataContainer;

typedef enum tChunkUpdateType_
{
    tChunkUpdateType_Remove,
    tChunkUpdateType_Create,
    tChunkUpdateType_Update
} tChunkUpdateType;

typedef struct tChunkUpdate_
{
    tChunkUpdateType updateType;
    int3             chunkPosition;
    s32              lod;
    s32              neighboursMask;
} tChunkUpdate;

typedef struct tChunk_
{
    tMeshDataContainer meshData;
    u32*               validIndices[tMeshDataSlot_Count];
    int3               position;
    s32                lod;
    s32                neighboursMask;
} tChunk;

typedef struct tWorldSettings_
{
    s32 worldSize;
    s32 chunkSize;
} tWorldSettings;

typedef struct tDensityDataValue_
{
    f32* values; // Array.h storage, one float density sample per entry
} tDensityDataValue;

typedef struct tDensityData_
{
    HashMap dataByChunkPosition; // key: tChunkPositionKey(position), value: tDensityDataValue
} tDensityData;

typedef struct tChunkData_
{
    tChunkUpdate* chunkUpdates;
    tChunkUpdate* filteredChunkUpdates;
    bool*         chunkUniformState;

    HashMap       chunkMap; // key: tChunkPositionKey(position), value: tChunk
    tOctree chunkTree;
    int3*         chunksToRemove;
    tChunk*       pendingChunks;
} tChunkData;

typedef f32 (*tDensitySampleFn)(f32 x, f32 y, f32 z, void* userData);
typedef f32 (*tNoise2DFn)(f32 x, f32 z, void* userData);
typedef f32 (*tNoise3DFn)(f32 x, f32 y, f32 z, void* userData);

// defined in TransvoxelTables.h; referenced by pointer only so the 61 KB table header
// is not pulled into every TU that includes this one
struct RegularCellData_;
struct TransitionCellData_;

typedef struct tDensityGenerator_
{
    tNoise2DFn heightMapNoise;
    void*      heightMapUserData;
    tNoise3DFn noise3D;
    void*      noise3DUserData;
    f32        heightMapStrength;
    f32        noise3DStrength;
} tDensityGenerator;

typedef struct tMaterialGenerator_
{
    tNoise3DFn stoneNoise;
    void*      stoneNoiseUserData;
    f32        grassBlendStrength;
    f32        grassAmount;
    f32        stoneThreshold;
} tMaterialGenerator;

typedef struct tDensityJob_
{
    tDensitySampleFn sample;
    void*            userData;
    s32              size;
    s32              step;
    s32              startX;
    s32              startY;
    s32              startZ;
    f32*             densityData; // Array.h storage
} tDensityJob;

typedef struct tUniformityJob_
{
    f32*  densityData; // Array.h storage
    bool* chunkUniformState;
    u32   index;
} tUniformityJob;

typedef struct tFilterJob_
{
    tChunkUpdate* chunkUpdates;
    tChunkUpdate* filteredChunkUpdates;
    bool*         chunkUniformState;
} tFilterJob;

typedef struct tMeshCopyJob_
{
    tMeshDataContainer* meshData;
    s32                 neighboursMask;
    u32*                validIndices[tMeshDataSlot_Count];
} tMeshCopyJob;

typedef struct tMaterialJob_
{
    tMeshDataContainer*     meshData;
    const tMaterialGenerator* generator;
    int3                   chunkMin;
} tMaterialJob;

typedef struct tUpdatesJob_
{
    tOctree*       octree;
    float3         targetPosition;
    tChunkUpdate*  chunkUpdates;
    HashMap        activeNodes; // key: locCode, value: u8
    HashMap        activeNodeNeighbours; // key: tChunkPositionKey(position), value: s32
    HashMap        chunkUpdatesMap; // key: tChunkPositionKey(position), value: s32
} tUpdatesJob;

typedef void* (*tPoolSpawnFn)(void* userData);
typedef void  (*tPoolDestroyFn)(void* item, void* userData);

typedef struct tPool_
{
    void**         items;
    tPoolSpawnFn  spawn;
    tPoolDestroyFn destroy;
    void*          userData;
} tPool;

typedef struct tPools_
{
    tPool  meshDataContainers;
    tPool  densityData;
    size_t meshVertexCapacity;
    size_t meshIndexCapacity;
    size_t meshSecondaryVertexCapacity;
    size_t densitySampleCount;
} tPools;

typedef struct tWorldState_
{
    tChunkData  chunkData;
    tDensityData densityData;
    tPools      pools;
} tWorldState;

typedef struct tDensityTaskData_
{
    int3        chunkPosition;
    tDensityJob job;
    JobHandle   handle;
} tDensityTaskData;

typedef struct tDensityTask_
{
    tWorldSettings          settings;
    tWorldState*            worldState;
    const tDensityGenerator* generator;
    JobSystem*              jobs;
    tDensityTaskData*       tasks;
} tDensityTask;

typedef struct tUniformityTaskData_
{
    tChunkUpdate  chunkUpdate;
    tUniformityJob job;
    JobHandle     handle;
} tUniformityTaskData;

typedef struct tUniformityTask_
{
    tWorldState*        worldState;
    JobSystem*          jobs;
    tUniformityTaskData* tasks;
} tUniformityTask;

typedef struct tFilterTask_
{
    tWorldState* worldState;
    JobSystem*   jobs;
    tFilterJob   job;
    JobHandle    handle;
} tFilterTask;

typedef struct tMaterialTaskData_
{
    tMaterialJob job;
    JobHandle    handle;
    int3         chunkPosition;
} tMaterialTaskData;

typedef struct tMaterialTask_
{
    tWorldSettings           settings;
    tWorldState*             worldState;
    const tMaterialGenerator* generator;
    JobSystem*               jobs;
    tMaterialTaskData*       tasks;
} tMaterialTask;

typedef struct tMeshCopyTaskData_
{
    int3         chunkPosition;
    JobHandle    handle;
    tMeshCopyJob job;
} tMeshCopyTaskData;

typedef struct tMeshCopyTask_
{
    tWorldState*      worldState;
    JobSystem*        jobs;
    tMeshCopyTaskData* tasks;
} tMeshCopyTask;

typedef bool (*tMesherFn)(const tDensityGenerator* generator, int3 chunkMin, s32 chunkSize, const f32* densityData,
                          s32 lod, s32 neighboursMask, tMeshDataContainer* meshData, void* userData);

typedef struct tMesherJob_
{
    const tDensityGenerator* generator;
    int3                     chunkMin;
    s32                      chunkSize;
    const f32*                densityData;
    tMeshDataContainer*       meshData;
    s32                      lod;
    s32                      neighboursMask;
    tMesherFn                mesh;
    void*                    userData;
} tMesherJob;

typedef struct tMesherTaskData_
{
    int3       chunkPosition;
    int3       chunkMin;
    s32        lod;
    s32        neighboursMask;
    tMesherJob job;
    JobHandle  handle;
} tMesherTaskData;

typedef struct tMesherTask_
{
    tWorldSettings           settings;
    tWorldState*             worldState;
    const tDensityGenerator*  generator;
    JobSystem*               jobs;
    tMesherFn                mesh;
    void*                    userData;
    tMesherTaskData*         tasks;
} tMesherTask;

typedef bool (*tCollisionBakeFn)(tMeshDataSlot slot, const tChunk* chunk, void* userData);

typedef struct tCollisionJob_
{
    const tChunk*    chunk;
    tCollisionBakeFn bake;
    void*            userData;
    bool             bakeSlot[tMeshDataSlot_Count];
} tCollisionJob;

typedef struct tCollisionTaskData_
{
    int3         chunkPosition;
    JobHandle    handle;
    tCollisionJob job;
} tCollisionTaskData;

typedef void (*tChunkLifecycleFn)(tWorldState* worldState, void* userData);

typedef struct tCollisionTask_
{
    tWorldState*        worldState;
    JobSystem*          jobs;
    tCollisionBakeFn    bake;
    tChunkLifecycleFn   removeOldChunks;
    tChunkLifecycleFn   enablePendingChunks;
    void*               userData;
    tCollisionTaskData* tasks;
} tCollisionTask;

typedef struct tTask_
{
    void* userData;
    void (*start)(void* userData);
    void (*execute)(void* userData);
    void (*end)(void* userData);
    bool (*isDone)(const void* userData);
} tTask;

typedef struct tTaskScheduler_
{
    tTask* tasks;
    u32    currentTask;
    bool   running;
} tTaskScheduler;

typedef float3 (*tTargetPositionFn)(void* userData);

typedef struct tUpdatesTask_
{
    tUpdatesJob       job;
    JobSystem*        jobs;
    JobHandle         handle;
    tTargetPositionFn targetPosition;
    void*             userData;
} tUpdatesTask;

typedef void (*tMemoryInitFn)(void* userData);
typedef void (*tMemoryFreeFn)(void* userData);

typedef struct tMemoryManager_
{
    tMemoryInitFn init;
    tMemoryFreeFn free;
    void*         userData;
} tMemoryManager;

typedef struct tVoxelWorldDesc_
{
    tWorldSettings settings;
    const tDensityGenerator* densityGenerator;
    const tMaterialGenerator* materialGenerator;
    JobSystem* jobs;
    tTargetPositionFn targetPosition;
    tMesherFn mesh;
    tCollisionBakeFn bakeCollision;
    tChunkLifecycleFn removeOldChunks;
    tChunkLifecycleFn enablePendingChunks;
    tMemoryManager memory;
    void* userData;
    u32 reserveChunks;
    size_t updateCapacity;
    u32 poolCount;
    size_t meshVertexCapacity;
    size_t meshIndexCapacity;
    size_t meshSecondaryVertexCapacity;
} tVoxelWorldDesc;

typedef struct tVoxelWorld_
{
    tVoxelWorldDesc desc;
    tWorldState worldState;
    tTaskScheduler scheduler;
    tUpdatesTask updatesTask;
    tDensityTask densityTask;
    tUniformityTask uniformityTask;
    tFilterTask filterTask;
    tMesherTask mesherTask;
    tMaterialTask materialTask;
    tMeshCopyTask meshCopyTask;
    tCollisionTask collisionTask;
} tVoxelWorld;

tIntBox tIntBoxMake(int3 min, int3 max);
tIntBox tIntBoxFromExtents(int3 position, s32 extents);
bool    tIntBoxContainsXYZ(tIntBox box, s32 x, s32 y, s32 z);
bool    tIntBoxContains(tIntBox box, int3 p);
bool    tIntBoxIntersects(tIntBox a, tIntBox b);

void tOctreeInit(tOctree* tree, int3 rootPosition, s32 rootSize, s32 rootDepth, u32 reserveCount);
void tOctreeDestroy(tOctree* tree);

bool tOctreeRootNode(const tOctree* tree, tOctreeNode* outNode);
bool tOctreeNodeHasChildren(const tOctree* tree, tOctreeNode node);
bool tOctreeGetChild(const tOctree* tree, tOctreeNode node, u32 index, tOctreeNode* outNode);
bool tOctreeGetNodeAt(const tOctree* tree, int3 position, tOctreeNode* outNode);
bool tOctreeRemoveNode(tOctree* tree, u64 locCode);
bool tOctreeSplitNode(tOctree* tree, tOctreeNode node);

bool tMeshDataInit(tMeshData* data, size_t vertexCapacity, size_t indexCapacity, size_t secondaryVertexCapacity);
void tMeshDataDestroy(tMeshData* data);
void tMeshDataClear(tMeshData* data);
bool tMeshDataPushVertex(tMeshData* data, tVertexData vertex);
bool tMeshDataPushIndex(tMeshData* data, u32 index);
bool tMeshDataPushSecondaryVertex(tMeshData* data, tSecondaryVert vertex);
void tMeshDataApplySecondaryVertices(tMeshData* data, s32 neighboursMask);
u32* tMeshDataBuildValidIndices(const tMeshData* data);

bool tMeshDataContainerInit(tMeshDataContainer* container, size_t vertexCapacity, size_t indexCapacity, size_t secondaryVertexCapacity);
void tMeshDataContainerDestroy(tMeshDataContainer* container);
void tMeshDataContainerClear(tMeshDataContainer* container);
bool tMeshDataContainerHasAnyData(const tMeshDataContainer* container);
tMeshData* tMeshDataContainerGet(tMeshDataContainer* container, tMeshDataSlot slot);
const tMeshData* tMeshDataContainerGetConst(const tMeshDataContainer* container, tMeshDataSlot slot);
void tMeshDataContainerApplySecondaryVertices(tMeshDataContainer* container, s32 neighboursMask);
void tMeshCopyJobClearResults(tMeshCopyJob* job);
bool tMeshCopyJobExecute(tMeshCopyJob* job);

u64  tChunkPositionKey(int3 position);
s32  tChunkUpdateCompareLODDescending(tChunkUpdate a, tChunkUpdate b);

bool tDensityDataInit(tDensityData* densityData, u32 reserveCount);
void tDensityDataDestroy(tDensityData* densityData);
void tDensityDataStoreDataUnchecked(tDensityData* densityData, int3 chunkPosition, f32* values);
f32* tDensityDataGetDataUnchecked(const tDensityData* densityData, int3 chunkPosition);
bool tDensityDataRemoveData(tDensityData* densityData, int3 chunkPosition);
bool tDensityDataTakeData(tDensityData* densityData, int3 chunkPosition, f32** outValues);

bool    tChunkDataInit(tChunkData* chunkData, tWorldSettings settings, u32 reserveChunks, size_t updateCapacity);
void    tChunkDataDestroy(tChunkData* chunkData);
void    tChunkDataClearUpdates(tChunkData* chunkData);
tChunk* tChunkDataFindChunk(tChunkData* chunkData, int3 chunkPosition);
bool    tChunkDataStoreChunk(tChunkData* chunkData, tChunk chunk);
bool    tChunkDataRemoveChunk(tChunkData* chunkData, int3 chunkPosition);

int3   tGetChunkMin(int3 chunkPosition, s32 chunkSize, s32 lod);
f32    tDensityGeneratorGetValue(const tDensityGenerator* generator, f32 x, f32 y, f32 z);
void   tMaterialGeneratorApply(const tMaterialGenerator* generator, tVertexData* vertexData, int3 chunkMin);

bool tDensityJobExecuteRange(tDensityJob* job, u32 begin, u32 end);
bool tDensityJobExecute(tDensityJob* job);
bool tDensityJobExecuteParallel(tDensityJob* job, u32 minItemsPerWorker);
bool tUniformityJobExecute(tUniformityJob* job);
bool tFilterJobExecute(tFilterJob* job);
bool tMaterialJobExecute(tMaterialJob* job);
bool tUpdatesJobInit(tUpdatesJob* job, tChunkData* chunkData, u32 reserveCount);
void tUpdatesJobDestroy(tUpdatesJob* job);
void tUpdatesJobClearFrame(tUpdatesJob* job);
bool tUpdatesJobExecute(tUpdatesJob* job);
f32  tDensityGeneratorSample(f32 x, f32 y, f32 z, void* userData);

bool  tPoolInit(tPool* pool, u32 numItems, tPoolSpawnFn spawn, tPoolDestroyFn destroy, void* userData);
void  tPoolDestroy(tPool* pool);
void* tPoolGet(tPool* pool);
bool  tPoolAdd(tPool* pool, void* item);

bool tPoolsInit(tPools* pools, tWorldSettings settings, u32 poolCount,
                size_t meshVertexCapacity, size_t meshIndexCapacity, size_t meshSecondaryVertexCapacity);
void tPoolsDestroy(tPools* pools);
tMeshDataContainer* tPoolsGetMeshDataContainer(tPools* pools);
bool tPoolsAddMeshDataContainer(tPools* pools, tMeshDataContainer* container);
f32* tPoolsGetDensityData(tPools* pools);
bool tPoolsAddDensityData(tPools* pools, f32* densityData);

bool tWorldStateInit(tWorldState* worldState, tWorldSettings settings, u32 reserveChunks, size_t updateCapacity,
                     u32 poolCount, size_t meshVertexCapacity, size_t meshIndexCapacity, size_t meshSecondaryVertexCapacity);
void tWorldStateDestroy(tWorldState* worldState);

bool tDensityJobCreate(const tWorldSettings* settings, tPools* pools, const tDensityGenerator* generator,
                       tChunkUpdate update, tDensityJob* outJob);
bool tUniformityJobCreate(tWorldState* worldState, int3 chunkPosition, u32 index, tUniformityJob* outJob);
bool tFilterJobCreate(tWorldState* worldState, tFilterJob* outJob);
bool tMaterialJobCreate(const tWorldSettings* settings, tWorldState* worldState, const tMaterialGenerator* generator,
                        tChunkUpdate update, tMaterialJob* outJob);
bool tMeshCopyJobCreate(tWorldState* worldState, tChunkUpdate update, tMeshCopyJob* outJob);

bool tDensityTaskInit(tDensityTask* task, tWorldSettings settings, tWorldState* worldState,
                      const tDensityGenerator* generator, JobSystem* jobs, size_t taskCapacity);
void tDensityTaskDestroy(tDensityTask* task);
void tDensityTaskStart(tDensityTask* task);
void tDensityTaskExecute(tDensityTask* task);
void tDensityTaskEnd(tDensityTask* task);
bool tDensityTaskIsDone(const tDensityTask* task);

bool tUniformityTaskInit(tUniformityTask* task, tWorldState* worldState, JobSystem* jobs, size_t taskCapacity);
void tUniformityTaskDestroy(tUniformityTask* task);
void tUniformityTaskStart(tUniformityTask* task);
void tUniformityTaskExecute(tUniformityTask* task);
void tUniformityTaskEnd(tUniformityTask* task);
bool tUniformityTaskIsDone(const tUniformityTask* task);

bool tFilterTaskInit(tFilterTask* task, tWorldState* worldState, JobSystem* jobs);
void tFilterTaskStart(tFilterTask* task);
void tFilterTaskExecute(tFilterTask* task);
void tFilterTaskEnd(tFilterTask* task);
bool tFilterTaskIsDone(const tFilterTask* task);

bool tMaterialTaskInit(tMaterialTask* task, tWorldSettings settings, tWorldState* worldState,
                       const tMaterialGenerator* generator, JobSystem* jobs, size_t taskCapacity);
void tMaterialTaskDestroy(tMaterialTask* task);
void tMaterialTaskStart(tMaterialTask* task);
void tMaterialTaskExecute(tMaterialTask* task);
void tMaterialTaskEnd(tMaterialTask* task);
bool tMaterialTaskIsDone(const tMaterialTask* task);

bool tMeshCopyTaskInit(tMeshCopyTask* task, tWorldState* worldState, JobSystem* jobs, size_t taskCapacity);
void tMeshCopyTaskDestroy(tMeshCopyTask* task);
void tMeshCopyTaskStart(tMeshCopyTask* task);
void tMeshCopyTaskExecute(tMeshCopyTask* task);
void tMeshCopyTaskEnd(tMeshCopyTask* task);
bool tMeshCopyTaskIsDone(const tMeshCopyTask* task);

bool tMesherJobCreate(const tWorldSettings* settings, tWorldState* worldState, const tDensityGenerator* generator,
                      tChunkUpdate update, tMesherFn mesh, void* userData, tMesherJob* outJob);
bool tMesherJobExecute(tMesherJob* job);
bool tTransvoxelMesherMesh(const tDensityGenerator* generator, int3 chunkMin, s32 chunkSize, const f32* densityData,
                           s32 lod, s32 neighboursMask, tMeshDataContainer* meshData, void* userData);
bool tMesherTaskInit(tMesherTask* task, tWorldSettings settings, tWorldState* worldState,
                     const tDensityGenerator* generator, JobSystem* jobs, tMesherFn mesh, void* userData,
                     size_t taskCapacity);
void tMesherTaskDestroy(tMesherTask* task);
void tMesherTaskStart(tMesherTask* task);
void tMesherTaskExecute(tMesherTask* task);
void tMesherTaskEnd(tMesherTask* task);
bool tMesherTaskIsDone(const tMesherTask* task);

bool tCollisionJobCreate(tWorldState* worldState, int3 chunkPosition, tCollisionBakeFn bake, void* userData, tCollisionJob* outJob);
bool tCollisionJobExecute(tCollisionJob* job);
bool tCollisionTaskInit(tCollisionTask* task, tWorldState* worldState, JobSystem* jobs, tCollisionBakeFn bake,
                        tChunkLifecycleFn removeOldChunks, tChunkLifecycleFn enablePendingChunks,
                        void* userData, size_t taskCapacity);
void tCollisionTaskDestroy(tCollisionTask* task);
void tCollisionTaskStart(tCollisionTask* task);
void tCollisionTaskExecute(tCollisionTask* task);
void tCollisionTaskEnd(tCollisionTask* task);
bool tCollisionTaskIsDone(const tCollisionTask* task);

bool tTaskSchedulerInit(tTaskScheduler* scheduler, size_t taskCapacity);
void tTaskSchedulerDestroy(tTaskScheduler* scheduler);
bool tTaskSchedulerStartTasks(tTaskScheduler* scheduler, const tTask* tasks, size_t taskCount);
void tTaskSchedulerProcessTasks(tTaskScheduler* scheduler);
void tTaskSchedulerEndTasks(tTaskScheduler* scheduler);

bool tUpdatesTaskInit(tUpdatesTask* task, tChunkData* chunkData, JobSystem* jobs, tTargetPositionFn targetPosition,
                      void* userData, u32 reserveCount);
void tUpdatesTaskDestroy(tUpdatesTask* task);
void tUpdatesTaskStart(tUpdatesTask* task);
void tUpdatesTaskExecute(tUpdatesTask* task);
void tUpdatesTaskEnd(tUpdatesTask* task);
bool tUpdatesTaskIsDone(const tUpdatesTask* task);

void tMemoryManagerInit(tMemoryManager* manager);
void tMemoryManagerFree(tMemoryManager* manager);
void tChunkUtilsRemoveOldChunks(tWorldState* worldState, void* userData);
void tChunkUtilsEnablePendingChunks(tWorldState* worldState, void* userData);

bool tVoxelWorldInit(tVoxelWorld* world, const tVoxelWorldDesc* desc);
void tVoxelWorldDestroy(tVoxelWorld* world);
bool tVoxelWorldStart(tVoxelWorld* world);
void tVoxelWorldUpdate(tVoxelWorld* world);
void tVoxelWorldStop(tVoxelWorld* world);

void tTransvoxelExampleUpdate(void);
void tTransvoxelExampleDestroy(void);
void tTransvoxelExampleInvalidateAll(void);
void tTransvoxelExampleInvalidateRegion(float3 mn, float3 mx);
void tTransvoxelExampleSetBrushCursor(float3 position, f32 radius, bool active);

#if defined(__cplusplus)
}
#endif

#endif // TRANSVOXEL_UNITY_H
