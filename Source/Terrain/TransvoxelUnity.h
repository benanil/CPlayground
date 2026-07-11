#ifndef TRANSVOXEL_UNITY_H
#define TRANSVOXEL_UNITY_H

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

typedef struct tVertexData_
{
    v128f position;
    u32 normal;    // packed normal/tangent (PackNormalTangent)
    u32 materials; // rgba8 material ids
    u32 blend;     // r = material A weight; shader uses 1-r for B
    u32 padding;
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

bool tTransvoxelMesherMesh(const tDensityGenerator* generator, int3 chunkMin, s32 chunkSize, const f32* densityData,
                           s32 lod, s32 neighboursMask, tMeshDataContainer* meshData, void* userData);

void tUpdate(void);
void tDestroy(void);
void tInvalidateAll(void);
void tInvalidateRegion(float3 mn, float3 mx);
// re-create every live chunk collider: Scene_BuildStaticColliders destroys ALL terrain
// physics bodies (PhysicsDestroyLiveStaticColliders) and runs async on scene load, so
// colliders synced before it lands are silently wiped. safe from any thread, the actual
// resync happens on the next tUpdate
void tInvalidatePhysics(void);
void tSetBrushCursor(float3 position, f32 radius, bool active);

#if defined(__cplusplus)
}
#endif

#endif // TRANSVOXEL_UNITY_H
