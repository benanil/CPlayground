#ifndef TRANSVOXEL_UNITY_H
#define TRANSVOXEL_UNITY_H

#include "Math/Bitpack.h"
#include "Include/Memory.h"
#include "Include/JobSystem.h"

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
    u64 position;  // u16 fixed x/y/z local to chunk, w unused
    u32 normal;    // packed normal/tangent (PackNormalTangent)
    u32 materials; 
} tVertexData;
STATIC_ASSERT(sizeof(tVertexData) == 16, "tVertexData must stay 16 bytes");

typedef struct tSecondaryVertexData_
{
    u64   position;
    s32   vertexMask;
    s32   vertexIndex;
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

typedef struct GeometryRange_
{
    void* heapPtr;
    u32   first;
    u32   count;
} GeometryRange;

typedef struct PhysicsMesh_
{
    struct b3Vec3* vertices;
    s32*    indices;
    u32     vertexCount;
    u32     indexCount;
} PhysicsMesh;

typedef struct tMeshHandle_
{
    GeometryRange vertices;
    GeometryRange indices;
    PhysicsMesh   physics;
} tMeshHandle;

// one in-flight chunk build on a JobSystem worker. the main thread fills the inputs,
// launches the job and reads the outputs after JobSystem_IsJobDone; exactly one job
// ever touches a chunk, so no locks on chunk state are needed
typedef struct tBuildJob_
{
    // inputs, main thread
    u32  chunkIndex;
    int3 min;
    s32  lod;
    s32  neighboursMask;
    // per-slot scratch, initialized once (ranges in the TerrainVertNew/Second/Index2 heaps)
    tMeshDataContainer scratchMesh;
    ArenaScratch scratchArena;
    // worker-local append state, valid only while the job runs (thread scratch arena)
    tVertexData* buildVertices;
    u32          buildVertexCount;
    u32*         buildIndices;
    u32          buildIndexCount;
    // output, worker; mesh ranges are zero when empty or failed
    tMeshHandle  mesh;
    JobHandle    handle;
    bool         failed;
    bool         busy;
} tBuildJob;

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

bool tMeshDataInit(tMeshData* data);
void tMeshDataDestroy(tMeshData* data);
void tMeshDataClear(tMeshData* data);
bool tMeshDataPushVertex(tMeshData* data, tVertexData vertex);
bool tMeshDataPushIndex(tMeshData* data, u32 index);
bool tMeshDataPushSecondaryVertex(tMeshData* data, tSecondaryVert vertex);
void tMeshDataApplySecondaryVertices(tMeshData* data, s32 neighboursMask);
u32* tMeshDataBuildValidIndices(const tMeshData* data);

bool tMeshDataContainerInit(tMeshDataContainer* container);
void tMeshDataContainerDestroy(tMeshDataContainer* container);
void tMeshDataContainerClear(tMeshDataContainer* container);
bool tMeshDataContainerHasAnyData(const tMeshDataContainer* container);
void tMeshDataContainerApplySecondaryVertices(tMeshDataContainer* container, s32 neighboursMask);

bool tTransvoxelMesherMesh(const tDensityGenerator* generator, 
						   const f32* densityData,
						   tBuildJob* job);
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
