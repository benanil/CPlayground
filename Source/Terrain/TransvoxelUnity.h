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

// Kept separate from the engine mesher until the port is complete enough to wire in.
	
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

typedef enum tChunkUpdateType_
{
    tChunkUpdateType_Remove,
    tChunkUpdateType_Create,
    tChunkUpdateType_Update
} tChunkUpdateType;

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

typedef struct tMeshDataContainer_
{
    tMeshData mesh[tMeshDataSlot_Count];
} tMeshDataContainer;

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

u64  tChunkPositionKey(int3 position);

bool tDensityDataInit(tDensityData* densityData, u32 reserveCount);
void tDensityDataDestroy(tDensityData* densityData);
void tDensityDataStoreDataUnchecked(tDensityData* densityData, int3 chunkPosition, f32* values);
f32* tDensityDataGetDataUnchecked(const tDensityData* densityData, int3 chunkPosition);
bool tDensityDataRemoveData(tDensityData* densityData, int3 chunkPosition);
bool tDensityDataTakeData(tDensityData* densityData, int3 chunkPosition, f32** outValues);

bool tTransvoxelMesherMesh(const tDensityGenerator* generator, int3 chunkMin, s32 chunkSize, const f32* densityData,
                           s32 lod, s32 neighboursMask, tMeshDataContainer* meshData, void* userData);

void tTransvoxelExampleUpdate(void);
void tTransvoxelExampleDestroy(void);
void tTransvoxelExampleInvalidateAll(void);
void tTransvoxelExampleInvalidateRegion(float3 mn, float3 mx);
void tTransvoxelExampleSetBrushCursor(float3 position, f32 radius, bool active);

#if defined(__cplusplus)
}
#endif

#endif // TRANSVOXEL_UNITY_H
