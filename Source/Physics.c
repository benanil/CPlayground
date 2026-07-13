#include "Include/Scene.h"
#include "Include/Platform.h"
#include "Include/Terrain.h"
#include "Include/JobSystem.h"
#include "Include/Graphics.h"
#include "Include/Memory.h"
#include "Include/BVH.h"
#include "Include/Algorithm.h"
#include "Include/FileSystem.h"
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

#define PHYSICS_SETTINGS_PATH "PhysicsSettings.txt"

PhysicsSettings g_PhysicsSettings = {
	.gravity = { 0.0f, -9.81f, 0.0f },
	.substepCount = 4,
	.enableSleep = true,
	.enableContinuous = true
};

void PhysicsSettings_Save(void)
{
	const PhysicsSettings* s = &g_PhysicsSettings;
	char text[256];
	int n = SDL_snprintf(text, sizeof(text),
		"gravity %f %f %f\nsubsteps %u\nsleep %d\ncontinuous %d\n",
		s->gravity[0], s->gravity[1], s->gravity[2], s->substepCount,
		s->enableSleep ? 1 : 0, s->enableContinuous ? 1 : 0);
	if (n > 0) WriteAllBytes(PHYSICS_SETTINGS_PATH, text, (unsigned long)n);
}

void PhysicsSettings_Load(void)
{
	static bool loaded;
	if (loaded) return;
	loaded = true;
	if (!FileExist(PHYSICS_SETTINGS_PATH)) return;

	uint64_t size = 0;
	char* text = ReadAllTextAlloc(PHYSICS_SETTINGS_PATH, &size, NULL);
	if (!text) return;

	PhysicsSettings* s = &g_PhysicsSettings;
	int sleep = s->enableSleep, cont = s->enableContinuous;
	for (const char* line = text; *line; )
	{
		// each sscanf is a no-op unless the line begins with its keyword
		SDL_sscanf(line, "gravity %f %f %f", &s->gravity[0], &s->gravity[1], &s->gravity[2]);
		SDL_sscanf(line, "substeps %u", &s->substepCount);
		SDL_sscanf(line, "sleep %d", &sleep);
		SDL_sscanf(line, "continuous %d", &cont);
		while (*line && *line != '\n') line++;
		while (*line == '\n' || *line == '\r') line++;
	}
	s->enableSleep = sleep != 0;
	s->enableContinuous = cont != 0;
	s->substepCount = Minu32(Maxu32(s->substepCount, 1u), 32u);
	FreeAllText(text);
}

void Scene_PhysicsApplyWorldSettings(Scene* scene)
{
	if (!scene || B3_IS_NULL(scene->physicsWorldID)) return;
	const PhysicsSettings* s = &g_PhysicsSettings;
	b3World_SetGravity(scene->physicsWorldID, (b3Vec3){ s->gravity[0], s->gravity[1], s->gravity[2] });
	b3World_EnableSleeping(scene->physicsWorldID, s->enableSleep);
	b3World_EnableContinuous(scene->physicsWorldID, s->enableContinuous);
}

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
	PhysicsSettings_Load();

	JobSystem* physicsJobSystem = JobSystem_Create(0, 0);
	b3WorldDef worldDef      = b3DefaultWorldDef();
	worldDef.workerCount     = JobSystem_GetThreadCount(physicsJobSystem);
	worldDef.enqueueTask     = PhysicsEnqueueTask;
	worldDef.finishTask      = PhysicsFinishTask;
	worldDef.userTaskContext = physicsJobSystem;

	scene->physicsWorldID = b3CreateWorld(&worldDef);
	Scene_PhysicsApplyWorldSettings(scene);
	scene->surfacePhysicsBodies = (b3BodyId*)AllocZeroTLSFGlobal(scene->surfaceSet.maxEntities, sizeof(b3BodyId));
	scene->transparentPhysicsBodies = (b3BodyId*)AllocZeroTLSFGlobal(scene->transparentSet.maxEntities, sizeof(b3BodyId));
	if (!scene->surfacePhysicsBodies || !scene->transparentPhysicsBodies)
		AX_WARN("physics: body slot allocation failed");
}

static void PhysicsDestroyMeshStorage(Scene* scene, bool transparent);
static void PhysicsDestroyTerrainMeshStorage(Scene* scene);

void Scene_PhysicsDestroy(Scene* scene)
{
	// destroy the world first, it releases the shapes that reference the mesh data, then the meshes
	if (B3_IS_NON_NULL(scene->physicsWorldID))
		b3DestroyWorld(scene->physicsWorldID);

	PhysicsDestroyMeshStorage(scene, false);
	PhysicsDestroyMeshStorage(scene, true);
	PhysicsDestroyTerrainMeshStorage(scene);
	if (scene->surfacePhysicsBodies) DeAllocateTLSFGlobal(scene->surfacePhysicsBodies);
	if (scene->transparentPhysicsBodies) DeAllocateTLSFGlobal(scene->transparentPhysicsBodies);
	if (scene->pendingPhysics) DeAllocateTLSFGlobal(scene->pendingPhysics);
	scene->surfacePhysicsBodies = NULL;
	scene->transparentPhysicsBodies = NULL;
	scene->pendingPhysics = NULL;
	scene->numPendingPhysics = 0;
	SDL_SetAtomicInt(&scene->physicsColliderBuildRunning, 0);
	SDL_SetAtomicInt(&scene->physicsColliderBuildDone, 0);
	scene->physicsColliderBuildCallback = NULL;
	scene->physicsColliderBuildResult = 0;
	scene->physicsWorldID = b3_nullWorldId;
	MemSet(scene->terrainPhysicsBodies, 0, sizeof(b3BodyId) * MAX_TERRAIN_PHYSICS_CHUNKS);
	MemSet(scene->terrainPhysicsMeshes, 0, sizeof(b3MeshData*) * MAX_TERRAIN_PHYSICS_CHUNKS);
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
	return transparent ? &scene->transparentSet : &scene->surfaceSet;
}

static u32 PhysicsCountMeshes(Scene* scene, bool transparent);
static void BuildCollidersForSet(Scene* scene, const RenderSet* set, bool transparent);

static b3MeshData** PhysicsMeshes(Scene* scene, bool transparent)
{
	return transparent ? scene->transparentPhysicsMeshes : scene->surfacePhysicsMeshes;
}

static b3BodyId* PhysicsBodies(Scene* scene, bool transparent)
{
	return transparent ? scene->transparentPhysicsBodies : scene->surfacePhysicsBodies;
}

// Resolves the body slot for an entity, or NULL when the entity/slot is out of
// range. The slot's body may still be null (no body created yet).
static b3BodyId* PhysicsEntitySlot(Scene* scene, bool transparent, const Entity* entity)
{
	if (!scene || !entity || entity->sparseIdx == INVALID_ENTITY) return NULL;
	b3BodyId* bodies = PhysicsBodies(scene, transparent);
	if (!bodies || entity->sparseIdx >= PhysicsSet(scene, transparent)->maxEntities) return NULL;
	return &bodies[entity->sparseIdx];
}

static uintptr_t PhysicsBodyUserData(u32 sparseIdx, bool transparent)
{
	return ((uintptr_t)sparseIdx << 1u) | (transparent ? 1u : 0u);
}

static uintptr_t PhysicsTerrainUserData(u32 chunkSlot)
{
	return ((uintptr_t)1u << (sizeof(uintptr_t) * 8u - 1u)) | (uintptr_t)chunkSlot;
}

static bool PhysicsUserDataIsTerrain(void* userData)
{
	return (((uintptr_t)userData) & ((uintptr_t)1u << (sizeof(uintptr_t) * 8u - 1u))) != 0u;
}

static u32 PhysicsUserDataTerrainChunk(void* userData)
{
	return (u32)(((uintptr_t)userData) & ~((uintptr_t)1u << (sizeof(uintptr_t) * 8u - 1u)));
}

static u32 PhysicsUserDataSparse(void* userData)
{
	return (u32)((uintptr_t)userData >> 1u);
}

static bool PhysicsUserDataTransparent(void* userData)
{
	return (((uintptr_t)userData) & 1u) != 0u;
}

void Scene_PhysicsUpdate(Scene* scene, float deltaTime)
{
	if (SDL_GetAtomicInt(&scene->physicsColliderBuildDone))
	{
		SDL_SetAtomicInt(&scene->physicsColliderBuildDone, 0);
		SDL_SetAtomicInt(&scene->physicsColliderBuildRunning, 0);
		if (scene->physicsColliderBuildResult)
		{
			BuildCollidersForSet(scene, &scene->surfaceSet, false);
			BuildCollidersForSet(scene, &scene->transparentSet, true);
			Terrain_InvalidatePhysics();
			AX_LOG("physics: built %u static collider meshes\n", PhysicsCountMeshes(scene, false) + PhysicsCountMeshes(scene, true));
		}

		AsyncCallback callback = scene->physicsColliderBuildCallback;
		scene->physicsColliderBuildCallback = NULL;
		if (callback) callback(scene, scene->physicsColliderBuildResult);
	}

	b3World_Step(scene->physicsWorldID, deltaTime, (int)g_PhysicsSettings.substepCount);

	// Write moved bodies back onto their entities. Move events only report bodies
	// that actually changed this step (dynamic/kinematic), so we skip the static
	// majority instead of scanning every entity. userData encodes the sparse id
	// and which set (surface/transparent) the body belongs to.
	b3BodyEvents events = b3World_GetBodyEvents(scene->physicsWorldID);
	for (int i = 0; i < events.moveCount; i++)
	{
		const b3BodyMoveEvent* move = &events.moveEvents[i];
		RenderSet* set = PhysicsSet(scene, PhysicsUserDataTransparent(move->userData));
		u32 sparseIdx = PhysicsUserDataSparse(move->userData);
		u32 dense = set->sparseID[sparseIdx];
		Entity* entity = &set->entities[dense];
		entity->position = SceneB3PosToVec3(move->transform.p);
		entity->rotation = SceneB3QuatToEntityRotation(move->transform.q);
	}
	scene->renderDataDirty |= events.moveCount > 0;
}

static void PhysicsDestroyBodies(b3BodyId* bodies, u32 maxEntities)
{
	if (!bodies) return;
	for (u32 i = 0; i < maxEntities; i++)
	{
		if (B3_IS_NON_NULL(bodies[i]))
			b3DestroyBody(bodies[i]);
		bodies[i] = b3_nullBodyId;
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
	PhysicsDestroyBodies(scene->transparentPhysicsBodies, scene->transparentSet.maxEntities);
	for (u32 i = 0; i < MAX_TERRAIN_PHYSICS_CHUNKS; i++)
		Scene_PhysicsDestroyTerrainChunk(scene, i);
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
    if (group->lodNumIndices[0] < 3 || group->lodNumVertices[0] == 0) return NULL;
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

	size_t vertBytes = (size_t)numVertices * sizeof(b3Vec3);
	size_t indexBytes = (size_t)numIndices * sizeof(int32_t);
	b3Vec3*  verts = (b3Vec3*)SDL_malloc(vertBytes);
	int32_t* idx   = (int32_t*)SDL_malloc(indexBytes);
	if (!verts || !idx)
	{
		AX_WARN("physics collider temp allocation failed vertices=%u indices=%u", numVertices, numIndices);
		if (verts) SDL_free(verts);
		if (idx) SDL_free(idx);
		return NULL;
	}

	const AVertex* vb = gGFX.SurfaceVertexBuffer + vertexOffset;
	for (u32 v = 0; v < numVertices; v++)
		verts[v] = ToB3Vec3(VecAdd(decodeMin, VecMul(UnpackUnorm16x4(vb[v].position), decodeExtent)));

	const u32* ib = gGFX.IndexBuffer + indexOffset;
	for (u32 k = 0; k < numIndices; k++)
	{
		u32 index = ib[k];
		if (index < vertexOffset || index >= vertexOffset + numVertices)
		{
			AX_WARN("physics collider skipped: invalid index group=%u mesh=%u primitive=%u k=%u vertexRange=%u..%u index=%u indexOffset=%u numIndices=%u",
					groupIdx, group->meshIndex, group->primitiveIndex, k, vertexOffset, vertexOffset + numVertices,
					index, indexOffset, numIndices);
			SDL_free(idx);
			SDL_free(verts);
			return NULL;
		}
		idx[k] = (int32_t)(index - vertexOffset); // mega-absolute -> group-local
	}

	b3MeshDef md      = {0};
	md.vertices       = verts;
	md.indices        = idx;
	md.vertexCount    = (int)numVertices;
	md.triangleCount  = (int)(numIndices / 3);
	md.identifyEdges  = true;
	b3MeshData* mesh  = b3CreateMesh(&md, NULL, 0);
	SDL_free(idx);
	SDL_free(verts);
	if (!mesh) return NULL;

	meshes[groupIdx] = mesh;
	return mesh;
}

static void Scene_PhysicsCreateEntityBody(Scene* scene, bool transparent, u32 groupIdx, const Entity* entity)
{
	b3BodyId* slot = PhysicsEntitySlot(scene, transparent, entity);
	if (!slot || B3_IS_NON_NULL(slot[0])) return;
	if ((entity->parentIdx >> 24) & ENTITY_FLAG_NOMESH) return;
	if (groupIdx >= PhysicsSet(scene, transparent)->numGroups) return;

	b3MeshData* mesh = Scene_PhysicsEnsureGroupMesh(scene, transparent, groupIdx);
	if (!mesh) return;

	b3BodyDef bd  = b3DefaultBodyDef();
	// Scene geometry is a triangle mesh (soup) collider. box3d mesh shapes carry
	// no volume/mass and can only back a static body, so this stays static; a
	// movable prop would need a convex hull/primitive shape instead.
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

	*slot = body;
}

void Scene_PhysicsSyncEntityBody(Scene* scene, bool transparent, u32 groupIdx, const Entity* entity)
{
	b3BodyId* slot = PhysicsEntitySlot(scene, transparent, entity);
	if (!slot || B3_IS_NULL(slot[0]))
	{
		Scene_PhysicsCreateEntityBody(scene, transparent, groupIdx, entity);
		return;
	}
	b3BodyId body = slot[0];

	// Rescale the collider mesh only when the entity scale actually changed. A
	// plain move/rotate must not rebuild the mesh tree, since this runs every
	// frame while a gizmo drag is active. The applied scale is read back from
	// the shape, so no per-entity scale needs to be cached alongside the body.
	b3ShapeId shape;
	if (b3Body_GetShapes(body, &shape, 1) > 0 && b3Shape_GetType(shape) == b3_meshShape)
	{
		b3Vec3 newScale = ToB3Vec3(EntityUnpackWorldScale(entity->scale));
		b3Vec3 curScale = b3Shape_GetMesh(shape).scale;
		if (newScale.x != curScale.x || newScale.y != curScale.y || newScale.z != curScale.z)
		{
			b3MeshData* mesh = Scene_PhysicsEnsureGroupMesh(scene, transparent, groupIdx);
			if (mesh) b3Shape_SetMesh(shape, mesh, newScale);
		}
	}

	b3Body_SetTransform(body, ToB3Vec3(entity->position), ToB3Quat(EntityUnpackRotation(entity->rotation)));
}

void Scene_PhysicsDestroyTerrainChunk(Scene* scene, u32 chunkSlot)
{
	if (!scene || chunkSlot >= MAX_TERRAIN_PHYSICS_CHUNKS) return;
	if (B3_IS_NON_NULL(scene->terrainPhysicsBodies[chunkSlot]))
		b3DestroyBody(scene->terrainPhysicsBodies[chunkSlot]);
	scene->terrainPhysicsBodies[chunkSlot] = b3_nullBodyId;
	if (scene->terrainPhysicsMeshes[chunkSlot])
	{
		b3DestroyMesh(scene->terrainPhysicsMeshes[chunkSlot]);
		scene->terrainPhysicsMeshes[chunkSlot] = NULL;
	}
}

static void PhysicsDestroyTerrainMeshStorage(Scene* scene)
{
	for (u32 i = 0; i < MAX_TERRAIN_PHYSICS_CHUNKS; i++)
	{
		if (!scene->terrainPhysicsMeshes[i]) continue;
		b3DestroyMesh(scene->terrainPhysicsMeshes[i]);
		scene->terrainPhysicsMeshes[i] = NULL;
	}
}

const b3MeshData* Scene_GetTerrainMeshData(Scene* scene, s32 slot)
{
	return scene->terrainPhysicsMeshes[slot];
}

bool Scene_PhysicsSyncTerrainChunkMesh(Scene* scene, u32 chunkSlot,
                                       b3Vec3* vertices, u32 vertexCount,
                                       s32* indices, u32 indexCount)
{
	if (!scene || B3_IS_NULL(scene->physicsWorldID)) return false;
	if (chunkSlot >= MAX_TERRAIN_PHYSICS_CHUNKS) return false;
	if (!vertices || !indices || vertexCount == 0u || indexCount < 3u) return false;

	b3MeshDef md     = {0};
	md.vertices      = vertices;
	md.indices       = indices;
	md.vertexCount   = (int)vertexCount;
	md.triangleCount = (int)(indexCount / 3u);
	md.identifyEdges = true;
	b3MeshData* mesh = b3CreateMesh(&md, NULL, 0);
	if (!mesh)
	{
		AX_WARN("physics: terrain mesh build failed for chunk slot %u", chunkSlot);
		return false;
	}

	b3MeshData* oldMesh = scene->terrainPhysicsMeshes[chunkSlot];
	b3BodyId body = scene->terrainPhysicsBodies[chunkSlot];
	if (B3_IS_NULL(body))
	{
		b3BodyDef bd = b3DefaultBodyDef();
		bd.type     = b3_staticBody;
		bd.userData = (void*)PhysicsTerrainUserData(chunkSlot);
		body = b3CreateBody(scene->physicsWorldID, &bd);

		b3ShapeDef sd = b3DefaultShapeDef();
		sd.filter.categoryBits = PHYS_CAT_SURFACE;
		b3ShapeId shape = b3CreateMeshShape(body, &sd, mesh, b3Vec3_one);
		if (B3_IS_NULL(shape))
		{
			b3DestroyBody(body);
			b3DestroyMesh(mesh);
			AX_WARN("physics: terrain shape build failed for chunk slot %u", chunkSlot);
			return false;
		}

		scene->terrainPhysicsBodies[chunkSlot] = body;
	}
	else
	{
		b3ShapeId shape;
		int numShapes = b3Body_GetShapes(body, &shape, 1);
		b3Shape_SetMesh(shape, mesh, b3Vec3_one);
	}

	scene->terrainPhysicsMeshes[chunkSlot] = mesh;
	if (oldMesh) b3DestroyMesh(oldMesh);
	return true;
}

bool Scene_PhysicsSetEntityShape(Scene* scene, bool transparent, u32 groupIdx, const Entity* entity, b3ShapeType type)
{
	b3BodyId* slot = PhysicsEntitySlot(scene, transparent, entity);
	RenderSet* set = PhysicsSet(scene, transparent);
	if (!slot || B3_IS_NULL(slot[0]) || groupIdx >= set->numGroups) return false;

	b3ShapeId shape;
	if (b3Body_GetShapes(slot[0], &shape, 1) <= 0) return false;

	// Mesh restores the original triangle collider (rebuilt/cached per group).
	if (type == b3_meshShape)
	{
		b3MeshData* mesh = Scene_PhysicsEnsureGroupMesh(scene, transparent, groupIdx);
		if (!mesh) return false;
		b3Shape_SetMesh(shape, mesh, ToB3Vec3(EntityUnpackWorldScale(entity->scale)));
		return true;
	}

	// Primitives are baked in body-local space. The mesh collider lives in the
	// primitive's model space scaled by the entity world scale, and box3d
	// primitives take no scale, so we fold the scale into their dimensions.
	PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
	if (group->bundleIdx >= scene->numBundles || !scene->bundleRefs[group->bundleIdx].bundle) return false;
	const SceneBundle* bundle = scene->bundleRefs[group->bundleIdx].bundle;
	const APrimitive* prim = &bundle->meshes[group->meshIndex].primitives[group->primitiveIndex];

	v128f scaleV = EntityUnpackWorldScale(entity->scale);
	v128f localMin = VecMul(VecLoad(prim->min), scaleV);
	v128f localMax = VecMul(VecLoad(prim->max), scaleV);
	v128f center = VecMulf(VecAdd(localMin, localMax), 0.5f);
	v128f half   = VecMax(VecMulf(VecSub(localMax, localMin), 0.5f), VecSet1(0.01f));
	f32 hx = VecGetX(half), hy = VecGetY(half), hz = VecGetZ(half);
	b3Vec3 c = ToB3Vec3(center);

	switch (type)
	{
		case b3_sphereShape:
		{
			b3Sphere sphere = { .center = c, .radius = Maxf32(Maxf32(hx, hy), hz) };
			b3Shape_SetSphere(shape, &sphere);
			return true;
		}
		case b3_capsuleShape:
		{
			// Capsule along Y, radius from the other two half-extents.
			f32 radius = Maxf32(hx, hz);
			f32 halfSpan = Maxf32(hy - radius, 0.0f);
			b3Capsule capsule = {
				.center1 = { c.x, c.y - halfSpan, c.z },
				.center2 = { c.x, c.y + halfSpan, c.z },
				.radius  = radius
			};
			b3Shape_SetCapsule(shape, &capsule);
			return true;
		}
		case b3_hullShape:
		{
			b3BoxHull box = b3MakeOffsetBoxHull(hx, hy, hz, c);
			b3Shape_SetHull(shape, &box.base);
			return true;
		}
		default:
			return false; // compound / height are not supported from the inspector
	}
}

void Scene_PhysicsApplyDefaultDynamicMass(b3BodyId body, b3ShapeId shape)
{
	b3AABB aabb = b3Shape_GetAABB(shape);
	f32 w = Maxf32(aabb.upperBound.x - aabb.lowerBound.x, 0.01f);
	f32 h = Maxf32(aabb.upperBound.y - aabb.lowerBound.y, 0.01f);
	f32 d = Maxf32(aabb.upperBound.z - aabb.lowerBound.z, 0.01f);
	f32 k = 1.0f / 12.0f;
	b3MassData md = {
		.mass = 1.0f,
		.center = { 0.0f, 0.0f, 0.0f },
		.inertia = {
			.cx = { k * (h * h + d * d), 0.0f, 0.0f },
			.cy = { 0.0f, k * (w * w + d * d), 0.0f },
			.cz = { 0.0f, 0.0f, k * (w * w + h * h) }
		}
	};
	b3Body_SetMassData(body, md);
}

bool Scene_PhysicsGetEntityOverride(const Scene* scene, u32 sparseIdx, ScenePhysicsRecord* out)
{
	if (!scene->surfacePhysicsBodies || sparseIdx >= scene->surfaceSet.maxEntities) return false;
	b3BodyId body = scene->surfacePhysicsBodies[sparseIdx];
	if (B3_IS_NULL(body)) return false;

	b3ShapeId shape;
	bool hasShape = b3Body_GetShapes(body, &shape, 1) > 0;
	u32 bodyType  = (u32)b3Body_GetType(body);
	u32 shapeType = hasShape ? (u32)b3Shape_GetType(shape) : (u32)b3_meshShape;
	// nothing to persist for the default static-mesh collider
	if (bodyType == (u32)b3_staticBody && shapeType == (u32)b3_meshShape) return false;

	b3MotionLocks locks = b3Body_GetMotionLocks(body);
	out->sparseIdx = sparseIdx;
	out->bodyType  = bodyType;
	out->shapeType = shapeType;
	out->lockBits  = (u32)locks.linearX | ((u32)locks.linearY << 1) | ((u32)locks.linearZ << 2)
	               | ((u32)locks.angularX << 3) | ((u32)locks.angularY << 4) | ((u32)locks.angularZ << 5);
	out->friction       = hasShape ? b3Shape_GetFriction(shape) : 0.0f;
	out->restitution    = hasShape ? b3Shape_GetRestitution(shape) : 0.0f;
	out->density        = hasShape ? b3Shape_GetDensity(shape) : 0.0f;
	out->linearDamping  = b3Body_GetLinearDamping(body);
	out->angularDamping = b3Body_GetAngularDamping(body);
	out->gravityScale   = b3Body_GetGravityScale(body);
	out->sleepThreshold = b3Body_GetSleepThreshold(body);
	return true;
}

void Scene_PhysicsApplyPendingOverrides(Scene* scene)
{
	RenderSet* set = &scene->surfaceSet;
	for (u32 i = 0; i < scene->numPendingPhysics; i++)
	{
		const ScenePhysicsRecord* rec = &scene->pendingPhysics[i];
		if (rec->sparseIdx >= set->maxEntities) continue;
		u32 dense = set->sparseID[rec->sparseIdx];
		if (dense >= set->numEntities) continue;
		const Entity* entity = &set->entities[dense];
		b3BodyId* slot = PhysicsEntitySlot(scene, false, entity);
		if (!slot || B3_IS_NULL(slot[0])) continue;
		b3BodyId body = slot[0];

		// swap the shape first so the material/mass below act on the final shape
		if (rec->shapeType != (u32)b3_meshShape)
			Scene_PhysicsSetEntityShape(scene, false, entity->primitiveIdx, entity, (b3ShapeType)rec->shapeType);

		b3ShapeId shape;
		bool hasShape = b3Body_GetShapes(body, &shape, 1) > 0;
		if (hasShape)
		{
			b3Shape_SetFriction(shape, rec->friction);
			b3Shape_SetRestitution(shape, rec->restitution);
			b3Shape_SetDensity(shape, rec->density, true);
		}
		b3Body_SetLinearDamping(body, rec->linearDamping);
		b3Body_SetAngularDamping(body, rec->angularDamping);
		b3Body_SetGravityScale(body, rec->gravityScale);
		b3Body_SetSleepThreshold(body, rec->sleepThreshold);

		b3MotionLocks locks = {
			.linearX  = (rec->lockBits >> 0) & 1u, .linearY  = (rec->lockBits >> 1) & 1u,
			.linearZ  = (rec->lockBits >> 2) & 1u, .angularX = (rec->lockBits >> 3) & 1u,
			.angularY = (rec->lockBits >> 4) & 1u, .angularZ = (rec->lockBits >> 5) & 1u
		};
		b3Body_SetMotionLocks(body, locks);

		// SetType recomputes mass (from density for primitives); a dynamic mesh still
		// reads zero mass, so give it the same default box mass as the inspector.
		if (rec->bodyType != (u32)b3_staticBody)
		{
			b3Body_SetType(body, (b3BodyType)rec->bodyType);
			if (rec->bodyType == (u32)b3_dynamicBody && hasShape && b3Body_GetMass(body) <= 0.0f)
				Scene_PhysicsApplyDefaultDynamicMass(body, shape);
		}
	}
	if (scene->pendingPhysics) DeAllocateTLSFGlobal(scene->pendingPhysics);
	scene->pendingPhysics = NULL;
	scene->numPendingPhysics = 0;
}

static void Scene_PhysicsDestroyEntityBody(Scene* scene, bool transparent, u32 groupIdx, const Entity* entity)
{
	(void)groupIdx;
	b3BodyId* slot = PhysicsEntitySlot(scene, transparent, entity);
	if (!slot) return;
	if (B3_IS_NON_NULL(slot[0])) b3DestroyBody(slot[0]);
	slot[0] = b3_nullBodyId;
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
	groupCount = Minu32(groupCount, set->maxGroups - firstGroup);
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
	*transparent = set->materialFilter == RenderSetMaterialFilter_Transparent;
	return true;
}

// Shared prologue for the add/remove range callbacks: validates the set is a
// hooked physics set, resolves the group, and clamps *count to it. Returns NULL
// when the callback should be skipped.
static PrimitiveGroup* PhysicsRangeGroup(RenderSet* set, u32 groupIdx, u32 localStartIdx, u32* count, bool* transparent)
{
	if (!RenderSetPhysicsKind(set, transparent) || !set->hookScene) return NULL;
	if (groupIdx >= set->numGroups || *count == 0u) return NULL;
	PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
	if (localStartIdx >= group->numEntities) return NULL;
	if (localStartIdx + *count > group->numEntities)
		*count = group->numEntities - localStartIdx;
	return group;
}

void RenderSet_AddEntitiesCallback(RenderSet* set, u32 groupIdx, u32 localStartIdx, u32 count)
{
	bool transparent = false;
	PrimitiveGroup* group = PhysicsRangeGroup(set, groupIdx, localStartIdx, &count, &transparent);
	if (!group) return;
	for (u32 i = 0; i < count; i++)
		Scene_PhysicsCreateEntityBody(set->hookScene, transparent, groupIdx, &set->entities[group->entityOffset + localStartIdx + i]);
}

void RenderSet_RemoveRangeCallback(RenderSet* set, u32 groupIdx, u32 localStartIdx, u32 count)
{
	bool transparent = false;
	PrimitiveGroup* group = PhysicsRangeGroup(set, groupIdx, localStartIdx, &count, &transparent);
	if (!group) return;
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

// Builds one triangle-mesh collider per primitive group. This can run on the loader thread because
// it only creates mesh data, not world bodies.
static void BuildColliderMeshesForSet(Scene* scene, const RenderSet* set, bool transparent)
{
	for (u32 b = 0; b < set->numBundles; b++)
	{
		const SceneBundle* bundle = set->bundles[b];
		if (!bundle) continue;

		Range range = set->bundlePrimRange[b];
		for (u32 g = range.start; g < range.start + range.count; g++)
		{
			PrimitiveGroup* group = &set->primitiveGroups[g];
			if (group->numEntities == 0 || group->lodNumIndices[0] < 3 || group->lodNumVertices[0] == 0)
				continue;
			Scene_PhysicsEnsureGroupMesh(scene, transparent, g);
		}
	}
}

// Creates static bodies for built collider meshes. Must run on the main thread: Box3D asserts when
// bodies are created while the world is locked or from async worker mutation paths.
static void BuildCollidersForSet(Scene* scene, const RenderSet* set, bool transparent)
{
	for (u32 b = 0; b < set->numBundles; b++)
	{
		const SceneBundle* bundle = set->bundles[b];
		if (!bundle) continue;

		Range range = set->bundlePrimRange[b];
		for (u32 g = range.start; g < range.start + range.count; g++)
		{
			PrimitiveGroup* group = &set->primitiveGroups[g];
			if (group->numEntities == 0 || group->lodNumIndices[0] < 3 || group->lodNumVertices[0] == 0)
				continue;
			if (!PhysicsMeshes(scene, transparent)[g]) continue;

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

	u32 maxMeshes = scene->surfaceSet.numGroups + scene->transparentSet.numGroups;
	if (maxMeshes == 0) return;

	BuildColliderMeshesForSet(scene, &scene->surfaceSet, false);
	BuildColliderMeshesForSet(scene, &scene->transparentSet, true);
	BuildCollidersForSet(scene, &scene->surfaceSet, false);
	BuildCollidersForSet(scene, &scene->transparentSet, true);
	// the wholesale destroy above also dropped every terrain chunk collider; tell the
	// terrain to re-create them on its next update (this can run on the async loader)
	Terrain_InvalidatePhysics();
	AX_LOG("physics: built %u static collider meshes\n", PhysicsCountMeshes(scene, false) + PhysicsCountMeshes(scene, true));
}

static s32 BuildStaticColliderMeshesTask(void* data)
{
	Scene* scene = (Scene*)data;
	BuildColliderMeshesForSet(scene, &scene->surfaceSet, false);
	BuildColliderMeshesForSet(scene, &scene->transparentSet, true);
	scene->physicsColliderBuildResult = 1;
	SDL_SetAtomicInt(&scene->physicsColliderBuildDone, 1);
	return 1;
}

void Scene_BuildStaticCollidersAsync(Scene* scene, AsyncCallback callback)
{
	if (SDL_GetAtomicInt(&scene->physicsColliderBuildRunning))
	{
		AX_WARN("physics collider build already running");
		return;
	}

	PhysicsDestroyLiveStaticColliders(scene);
	scene->physicsColliderBuildCallback = callback;
	scene->physicsColliderBuildResult = 0;
	SDL_SetAtomicInt(&scene->physicsColliderBuildDone, 0);
	SDL_SetAtomicInt(&scene->physicsColliderBuildRunning, 1);
	AsyncRun("create static colliders task", BuildStaticColliderMeshesTask, NULL, scene);
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
	if (PhysicsUserDataIsTerrain(userData))
	{
		hit->hit.t      = t;
		hit->hit.u      = 0.0f;
		hit->hit.v      = 0.0f;
		hit->skinnedSet = 0xFFFFFFFFu;
		hit->bundleIdx  = 0xFFFFFFFFu;
		hit->groupIdx   = 0u;
		hit->entityIdx  = PhysicsUserDataTerrainChunk(userData);
		hit->triIndex   = (u32)r.triangleIndex;
		return 1;
	}

	u32 sparse = PhysicsUserDataSparse(userData);
	const RenderSet* set = PhysicsUserDataTransparent(userData) ? &scene->transparentSet : &scene->surfaceSet;
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
