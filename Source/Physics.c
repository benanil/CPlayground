#include "Include/Scene.h"
#include "Include/Platform.h"
#include "Include/JobSystem.h"
#include "Include/Graphics.h"
#include "Include/Memory.h"
#include "Include/BVH.h"
#include "Include/Algorithm.h"
#include "Math/Bitpack.h"

#include <box3d/box3d.h>
#include <SDL3/SDL_stdinc.h>

extern Graphics gGFX; // cpu mega buffers, declared per translation unit as elsewhere (BVH.c)

// collision categories for static geometry. scene picking filters to surface only, matching the
// old cpu-BVH picking (which never cast the transparent set). transparent still collides physically.
#define PHYS_CAT_SURFACE     0x0001ull
#define PHYS_CAT_TRANSPARENT 0x0002ull

// finite segment length for picking rays; the ray dir is unit length so fraction*length is world t
#define PHYS_PICK_MAX_DIST   1.0e5f

// b3EnqueueTaskCallback: box3d's b3TaskCallback (void(void*)) matches JobSystemFn directly, so the
// task is queued as-is. Returns the job handle as an opaque userTask that box3d hands back to the
// finish callback. On a full/failed queue we run the task inline and return NULL so box3d skips finish.
static void* PhysicsEnqueueTask(b3TaskCallback* task, void* taskContext,
						 void* userContext, const char* taskName)
{
    (void)taskName;
    JobSystem* jobSystem = (JobSystem*)userContext;
    JobHandle handle = JobSystem_Execute(jobSystem, (JobSystemFn)task, taskContext);
    if (handle == 0)
    {
        task(taskContext);
        return NULL;
    }
    return (void*)(uintptr_t)(u32)handle;
}

// b3FinishTaskCallback: waits for the job enqueued above to finish.
static void PhysicsFinishTask(void* userTask, void* userContext)
{
    JobSystem* jobSystem = (JobSystem*)userContext;
    JobHandle handle = (JobHandle)(u32)(uintptr_t)userTask;
    JobSystem_WaitJob(jobSystem, handle);
}

void Scene_InitPhysics(Scene* scene)
{
	b3Version version = b3GetVersion();
	AX_LOG("Box3D version %d.%d.%d\n", version.major, version.minor, version.revision);
	
	JobSystem* physicsJobSystem = JobSystem_Create(0, 0);
	b3WorldDef worldDef      = b3DefaultWorldDef();
	worldDef.workerCount     = JobSystem_GetThreadCount(physicsJobSystem);
	worldDef.enqueueTask     = PhysicsEnqueueTask;
	worldDef.finishTask      = PhysicsFinishTask;
	worldDef.userTaskContext = physicsJobSystem;
	
	scene->physicsWorldID = b3CreateWorld(&worldDef);
	scene->surfacePhysicsBodies = (ScenePhysicsBody*)AllocZeroTLSFGlobal(scene->surfaceSet.maxEntities, sizeof(ScenePhysicsBody));
	scene->transparentPhysicsBodies = (ScenePhysicsBody*)AllocZeroTLSFGlobal(scene->transparentSurfaceSet.maxEntities, sizeof(ScenePhysicsBody));
	if (!scene->surfacePhysicsBodies || !scene->transparentPhysicsBodies)
		AX_WARN("physics: body slot allocation failed");
}

void Scene_PhysicsDestroy(Scene* scene)
{
	// destroy the world first, it releases the shapes that reference the mesh data, then the meshes
	if (B3_IS_NON_NULL(scene->physicsWorldID))
		b3DestroyWorld(scene->physicsWorldID);

	for (u32 i = 0; i < scene->surfaceSet.maxGroups; i++)
	{
		if (scene->surfacePhysicsMeshes[i]) b3DestroyMesh(scene->surfacePhysicsMeshes[i]);
		scene->surfacePhysicsMeshes[i] = NULL;
	}
	for (u32 i = 0; i < scene->transparentSurfaceSet.maxGroups; i++)
	{
		if (scene->transparentPhysicsMeshes[i]) b3DestroyMesh(scene->transparentPhysicsMeshes[i]);
		scene->transparentPhysicsMeshes[i] = NULL;
	}
	if (scene->surfacePhysicsBodies) DeAllocateTLSFGlobal(scene->surfacePhysicsBodies);
	if (scene->transparentPhysicsBodies) DeAllocateTLSFGlobal(scene->transparentPhysicsBodies);
	scene->surfacePhysicsBodies = NULL;
	scene->transparentPhysicsBodies = NULL;
	scene->physicsWorldID = b3_nullWorldId;
}

void Scene_PhysicsUpdate(Scene* scene, float deltaTime)
{
	const int physicsStepCount = 4;
	b3World_Step(scene->physicsWorldID, deltaTime, physicsStepCount);
}

b3Vec3 ToB3Vec3(v128f v) { return (b3Vec3){ VecGetX(v), VecGetY(v), VecGetZ(v) }; }

b3Quat ToB3Quat(v128f q)
{
	f32 lenSq = VecGetX(VecLenSq(q));
	if (!(lenSq > 1.0e-12f) || lenSq > 1.0e12f)
		return b3Quat_identity;

	q = VecDivf(q, Sqrtf(lenSq));
	b3Quat res;
	VecStore(&res.v.x, q);
	return res;
}

v128f SceneB3PosToVec3(b3Pos p) { return VecSetR((f32)p.x, (f32)p.y, (f32)p.z, 0.0f); }
u64 SceneB3QuatToEntityRotation(b3Quat q) { return EntityPackRotation(VecSetR(q.v.x, q.v.y, q.v.z, q.s)); }

static RenderSet* PhysicsSet(Scene* scene, bool transparent)
{
	return transparent ? &scene->transparentSurfaceSet : &scene->surfaceSet;
}

static b3MeshData** PhysicsMeshes(Scene* scene, bool transparent)
{
	return transparent ? scene->transparentPhysicsMeshes : scene->surfacePhysicsMeshes;
}

static ScenePhysicsBody* PhysicsBodies(Scene* scene, bool transparent)
{
	return transparent ? scene->transparentPhysicsBodies : scene->surfacePhysicsBodies;
}

ScenePhysicsBody* Scene_PhysicsBodySlot(Scene* scene, bool transparent, u32 sparseIdx)
{
	if (!scene) return NULL;
	RenderSet* set = PhysicsSet(scene, transparent);
	if (sparseIdx >= set->maxEntities) return NULL;
	ScenePhysicsBody* bodies = PhysicsBodies(scene, transparent);
	return bodies ? &bodies[sparseIdx] : NULL;
}

static uintptr_t PhysicsBodyUserData(u32 sparseIdx, bool transparent)
{
	return ((uintptr_t)sparseIdx << 1u) | (transparent ? 1u : 0u);
}

static u32 PhysicsUserDataSparse(void* userData)
{
	return (u32)((uintptr_t)userData >> 1u);
}

static bool PhysicsUserDataTransparent(void* userData)
{
	return (((uintptr_t)userData) & 1u) != 0u;
}

static void PhysicsDestroyBodies(ScenePhysicsBody* bodies, u32 maxEntities)
{
	if (!bodies) return;
	for (u32 i = 0; i < maxEntities; i++)
	{
		if (B3_IS_NON_NULL(bodies[i].body))
			b3DestroyBody(bodies[i].body);
		bodies[i] = (ScenePhysicsBody){0};
	}
}

static void PhysicsDestroyMeshStorage(Scene* scene, bool transparent)
{
	b3MeshData** meshes = PhysicsMeshes(scene, transparent);
	RenderSet* set = PhysicsSet(scene, transparent);
	for (u32 i = 0; i < set->maxGroups; i++)
	{
		if (!meshes[i]) continue;
		b3DestroyMesh(meshes[i]);
		meshes[i] = NULL;
	}
}

static void PhysicsDestroyLiveStaticColliders(Scene* scene)
{
	PhysicsDestroyBodies(scene->surfacePhysicsBodies, scene->surfaceSet.maxEntities);
	PhysicsDestroyBodies(scene->transparentPhysicsBodies, scene->transparentSurfaceSet.maxEntities);
	PhysicsDestroyMeshStorage(scene, false);
	PhysicsDestroyMeshStorage(scene, true);
}

static u32 PhysicsCountMeshes(Scene* scene, bool transparent)
{
	b3MeshData** meshes = PhysicsMeshes(scene, transparent);
	RenderSet* set = PhysicsSet(scene, transparent);
	u32 count = 0u;
	for (u32 i = 0; i < set->maxGroups; i++)
		if (meshes[i]) count++;
	return count;
}

static b3MeshData* Scene_PhysicsEnsureGroupMesh(Scene* scene, bool transparent, u32 groupIdx)
{
	RenderSet* set = PhysicsSet(scene, transparent);
	if (groupIdx >= set->numGroups) return NULL;
	b3MeshData** meshes = PhysicsMeshes(scene, transparent);
	if (meshes[groupIdx]) return meshes[groupIdx];

	PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
	if (group->numIndices < 3 || group->numVertices == 0) return NULL;
	if (group->bundleIdx >= scene->numBundles || !scene->bundleRefs[group->bundleIdx].bundle)
	{
		AX_WARN("physics: missing bundle for collider group %u", groupIdx);
		return NULL;
	}

	const SceneBundle* bundle = scene->bundleRefs[group->bundleIdx].bundle;
	const APrimitive* prim = &bundle->meshes[group->meshIndex].primitives[group->primitiveIndex];
	u32 vertexOffset = group->lodVertexOffset[0];
	u32 numVertices  = group->lodNumVertices[0];
	u32 indexOffset  = group->lodIndexOffset[0];
	u32 numIndices   = group->lodNumIndices[0];
	if (numIndices < 3 || numVertices == 0) return NULL;
	v128f decodeMin    = VecLoad(prim->min);
	v128f decodeExtent = VecMax(VecSub(VecLoad(prim->max), decodeMin), VecSet1(1.0e-6f));

	ArenaMark mark = ArenaSave(&GlobalArena);
	b3Vec3*  verts = (b3Vec3*)ArenaAllocGlobal(numVertices * sizeof(b3Vec3));
	int32_t* idx   = (int32_t*)ArenaAllocGlobal(numIndices * sizeof(int32_t));

	const AVertex* vb = gGFX.SurfaceVertexBuffer + vertexOffset;
	for (u32 v = 0; v < numVertices; v++)
		verts[v] = ToB3Vec3(VecAdd(decodeMin, VecMul(UnpackUnorm16x4(vb[v].position), decodeExtent)));

	const u32* ib = gGFX.IndexBuffer + indexOffset;
	for (u32 k = 0; k < numIndices; k++)
	{
		idx[k] = (int32_t)(ib[k] - vertexOffset); // mega-absolute -> group-local
	}

	b3MeshDef md      = {0};
	md.vertices       = verts;
	md.indices        = idx;
	md.vertexCount    = (int)numVertices;
	md.triangleCount  = (int)(numIndices / 3);
	md.identifyEdges  = true;
	b3MeshData* mesh  = b3CreateMesh(&md, NULL, 0);
	ArenaRestore(&GlobalArena, mark); // b3CreateMesh copied the arrays into its own storage
	if (!mesh) return NULL;

	meshes[groupIdx] = mesh;
	return mesh;
}

static void Scene_PhysicsCreateEntityBody(Scene* scene, bool transparent, u32 groupIdx, const Entity* entity)
{
	if (!scene || !entity || entity->sparseIdx == INVALID_ENTITY) return;
	if ((entity->parentIdx >> 24) & ENTITY_FLAG_NOMESH) return;

	RenderSet* set = PhysicsSet(scene, transparent);
	if (groupIdx >= set->numGroups || entity->sparseIdx >= set->maxEntities) return;
	ScenePhysicsBody* bodies = PhysicsBodies(scene, transparent);
	if (!bodies) return;
	ScenePhysicsBody* slot = &bodies[entity->sparseIdx];
	if (B3_IS_NON_NULL(slot->body)) return;

	b3MeshData* mesh = Scene_PhysicsEnsureGroupMesh(scene, transparent, groupIdx);
	if (!mesh) return;

	b3BodyDef bd  = b3DefaultBodyDef();
	bd.type       = b3_staticBody;
	bd.position   = ToB3Vec3(entity->position);
	bd.rotation   = ToB3Quat(EntityUnpackRotation(entity->rotation));
	bd.userData   = (void*)PhysicsBodyUserData(entity->sparseIdx, transparent);
	b3BodyId body = b3CreateBody(scene->physicsWorldID, &bd);

	b3ShapeDef sd = b3DefaultShapeDef();
	sd.filter.categoryBits = transparent ? PHYS_CAT_TRANSPARENT : PHYS_CAT_SURFACE;
	b3ShapeId shape = b3CreateMeshShape(body, &sd, mesh, ToB3Vec3(EntityUnpackWorldScale(entity->scale)));
	if (B3_IS_NULL(shape))
	{
		b3DestroyBody(body);
		return;
	}

	*slot = (ScenePhysicsBody){
		.scale = entity->scale,
		.body  = body,
		.shape = shape
	};
}

void Scene_PhysicsSyncEntityBody(Scene* scene, bool transparent, u32 groupIdx, const Entity* entity)
{
	if (!scene || !entity || entity->sparseIdx == INVALID_ENTITY) return;
	RenderSet* set = PhysicsSet(scene, transparent);
	if (entity->sparseIdx >= set->maxEntities) return;
	ScenePhysicsBody* bodies = PhysicsBodies(scene, transparent);
	if (!bodies || B3_IS_NULL(bodies[entity->sparseIdx].body))
	{
		Scene_PhysicsCreateEntityBody(scene, transparent, groupIdx, entity);
		return;
	}

	ScenePhysicsBody* slot = &bodies[entity->sparseIdx];
	if (slot->scale != entity->scale)
	{
		b3MeshData* mesh = Scene_PhysicsEnsureGroupMesh(scene, transparent, groupIdx);
		if (mesh)
		{
			b3Shape_SetMesh(slot->shape, mesh, ToB3Vec3(EntityUnpackWorldScale(entity->scale)));
			slot->scale = entity->scale;
		}
	}
	b3Body_SetTransform(slot->body, ToB3Vec3(entity->position), ToB3Quat(EntityUnpackRotation(entity->rotation)));
}

static void Scene_PhysicsDestroyEntityBody(Scene* scene, bool transparent, u32 groupIdx, const Entity* entity)
{
	(void)groupIdx;
	if (!scene || !entity || entity->sparseIdx == INVALID_ENTITY) return;
	ScenePhysicsBody* bodies = PhysicsBodies(scene, transparent);
	if (!bodies) return;
	RenderSet* set = PhysicsSet(scene, transparent);
	if (entity->sparseIdx >= set->maxEntities) return;

	ScenePhysicsBody* slot = &bodies[entity->sparseIdx];
	if (B3_IS_NON_NULL(slot->body)) b3DestroyBody(slot->body);
	*slot = (ScenePhysicsBody){0};
}

static void Scene_PhysicsDestroyBodiesInRange(Scene* scene, bool transparent, u32 firstGroup, u32 groupCount)
{
	if (!scene || groupCount == 0u) return;
	RenderSet* set = PhysicsSet(scene, transparent);
	if (firstGroup >= set->numGroups) return;
	u32 lastGroup = Minu32(firstGroup + groupCount, set->numGroups);
	for (u32 g = firstGroup; g < lastGroup; g++)
	{
		PrimitiveGroup* group = &set->primitiveGroups[g];
		for (u32 e = 0; e < group->numEntities; e++)
			Scene_PhysicsDestroyEntityBody(scene, transparent, g, &set->entities[group->entityOffset + e]);
	}
}

static void Scene_PhysicsRemoveGroupMeshes(Scene* scene, bool transparent, u32 firstGroup, u32 groupCount)
{
	if (!scene || groupCount == 0u) return;
	b3MeshData** meshes = PhysicsMeshes(scene, transparent);
	if (!meshes) return;
	RenderSet* set = PhysicsSet(scene, transparent);
	if (firstGroup >= set->maxGroups) return;
	if (groupCount > set->maxGroups - firstGroup)
		groupCount = set->maxGroups - firstGroup;
	u32 afterRemoved = firstGroup + groupCount;
	u32 last = Minu32(afterRemoved, set->maxGroups);
	for (u32 i = firstGroup; i < last; i++)
	{
		if (!meshes[i]) continue;
		b3DestroyMesh(meshes[i]);
		meshes[i] = NULL;
	}
	if (afterRemoved >= set->maxGroups) return;
	for (u32 i = afterRemoved; i < set->maxGroups; i++)
		meshes[i - groupCount] = meshes[i];
	u32 clearStart = set->maxGroups - groupCount;
	for (u32 i = clearStart; i < set->maxGroups; i++)
		meshes[i] = NULL;
}

static bool RenderSetPhysicsKind(const RenderSet* set, bool* transparent)
{
	if (!set || set->skinned) return false;
	if (set->materialFilter == RenderSetMaterialFilter_Opaque)
		*transparent = false;
	else if (set->materialFilter == RenderSetMaterialFilter_Transparent)
		*transparent = true;
	else
		return false;
	return true;
}

void RenderSet_AddEntitiesCallback(RenderSet* set, u32 groupIdx, u32 localStartIdx, u32 count)
{
	bool transparent = false;
	if (!RenderSetPhysicsKind(set, &transparent) || !set->hookScene) return;
	if (groupIdx >= set->numGroups || count == 0u) return;
	PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
	if (localStartIdx >= group->numEntities) return;
	if (localStartIdx + count > group->numEntities)
		count = group->numEntities - localStartIdx;

	for (u32 i = 0; i < count; i++)
		Scene_PhysicsCreateEntityBody(set->hookScene, transparent, groupIdx, &set->entities[group->entityOffset + localStartIdx + i]);
}

void RenderSet_RemoveRangeCallback(RenderSet* set, u32 groupIdx, u32 localStartIdx, u32 count)
{
	bool transparent = false;
	if (!RenderSetPhysicsKind(set, &transparent) || !set->hookScene) return;
	if (groupIdx >= set->numGroups || count == 0u) return;
	PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
	if (localStartIdx >= group->numEntities) return;
	if (localStartIdx + count > group->numEntities)
		count = group->numEntities - localStartIdx;

	for (u32 i = 0; i < count; i++)
		Scene_PhysicsDestroyEntityBody(set->hookScene, transparent, groupIdx, &set->entities[group->entityOffset + localStartIdx + i]);
}

void RenderSet_RemoveGroupsCallback(RenderSet* set, u32 firstGroup, u32 groupCount)
{
	bool transparent = false;
	if (!RenderSetPhysicsKind(set, &transparent) || !set->hookScene) return;
	Scene_PhysicsDestroyBodiesInRange(set->hookScene, transparent, firstGroup, groupCount);
	Scene_PhysicsRemoveGroupMeshes(set->hookScene, transparent, firstGroup, groupCount);
}

void RenderSet_ClearEntitiesCallback(RenderSet* set)
{
	bool transparent = false;
	if (!RenderSetPhysicsKind(set, &transparent) || !set->hookScene) return;
	Scene_PhysicsDestroyBodiesInRange(set->hookScene, transparent, 0u, set->numGroups);
}

// builds one triangle-mesh collider per primitive group of the set and a static body per instance.
// mirrors the bundle/group iteration used by the picking BVH (BVH_RaycastSet) and the same unorm16
// de-quantization as BVH_PrimitiveDecode so the collider matches the rendered geometry exactly.
static void BuildCollidersForSet(Scene* scene, const RenderSet* set, u64 category)
{
	bool transparent = category == PHYS_CAT_TRANSPARENT;
	for (u32 b = 0; b < set->numBundles; b++)
	{
		const SceneBundle* bundle = set->bundles[b];
		if (!bundle) continue;

		Range range = set->bundlePrimitiveRange[b];
		for (u32 g = range.start; g < range.start + range.count; g++)
		{
			PrimitiveGroup* group = &set->primitiveGroups[g];
			if (group->numEntities == 0 || group->numIndices < 3 || group->numVertices == 0)
				continue;
			if (!Scene_PhysicsEnsureGroupMesh(scene, transparent, g)) continue;

			Entity* ents = set->entities + group->entityOffset;
			for (u32 e = 0; e < group->numEntities; e++)
				Scene_PhysicsCreateEntityBody(scene, transparent, g, &ents[e]);
		}
	}
}

void Scene_BuildStaticColliders(Scene* scene)
{
	PhysicsDestroyLiveStaticColliders(scene);
	if (!scene->surfacePhysicsBodies || !scene->transparentPhysicsBodies) return;

	u32 maxMeshes = scene->surfaceSet.numGroups + scene->transparentSurfaceSet.numGroups;
	if (maxMeshes == 0) return;

	BuildCollidersForSet(scene, &scene->surfaceSet, PHYS_CAT_SURFACE);
	BuildCollidersForSet(scene, &scene->transparentSurfaceSet, PHYS_CAT_TRANSPARENT);
	AX_LOG("physics: built %u static collider meshes\n", PhysicsCountMeshes(scene, false) + PhysicsCountMeshes(scene, true));
}

static s32 BuildStaticCollidersTask(void* data)
{
	Scene_BuildStaticColliders((Scene*)data);
	return 1;
}

void Scene_BuildStaticCollidersAsync(Scene* scene, AsyncCallback callback)
{
	if (!AsyncRun("craete static colliders task", BuildStaticCollidersTask, callback, scene))
	{
		BuildStaticCollidersTask(scene);
	}
}

// box3d picking against the static surface colliders. mirrors the BVHHit contract of the cpu-BVH
// path so BVH_RaycastScene can keep whichever of (static physics, skinned cpu-BVH) hit is nearer.
// only writes *hit when the physics hit is closer than the current hit->hit.t. out: 1 when written.
s32 Scene_PhysicsRaycastPick(const Scene* scene, v128f origin, v128f dir, BVHHit* hit)
{
	b3QueryFilter filter = b3DefaultQueryFilter();
	filter.maskBits = PHYS_CAT_SURFACE; // exclude transparent (and any future non-pickable) colliders

	b3RayResult r = b3World_CastRayClosest(scene->physicsWorldID, ToB3Vec3(origin),
	                                       ToB3Vec3(VecMulf(dir, PHYS_PICK_MAX_DIST)), filter);
	if (!r.hit) return 0;

	f32 t = r.fraction * PHYS_PICK_MAX_DIST; // dir is unit length, so this is world distance
	if (t >= hit->hit.t) return 0;

	// body userData packs the sparse id plus the render-set kind; resolve it to (group, local entity)
	void* userData = b3Body_GetUserData(b3Shape_GetBody(r.shapeId));
	u32 sparse = PhysicsUserDataSparse(userData);
	const RenderSet* set = PhysicsUserDataTransparent(userData) ? &scene->transparentSurfaceSet : &scene->surfaceSet;
	if (sparse >= set->maxEntities) return 0;
	u32 dense = set->sparseID[sparse];
	if (dense == INVALID_ENTITY || dense >= set->numEntities) return 0;

	u32 groupIdx = set->entities[dense].primitiveIdx;
	if (groupIdx >= set->numGroups) return 0;

	hit->hit.t     = t;
	hit->hit.u     = 0.0f;
	hit->hit.v     = 0.0f;
	hit->skinnedSet = 0;
	hit->groupIdx  = groupIdx;
	hit->entityIdx = dense - set->primitiveGroups[groupIdx].entityOffset;
	hit->triIndex  = (u32)r.triangleIndex;
	return 1;
}
