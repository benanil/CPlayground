// transform gizmo for the picked objects. all primitives of a spawned node share one
// sparse id (a skinned spawn shares one for the whole character), so a gizmo target is
// "every entity with that sparse id": the object and its parts move as one. multiple
// targets (ctrl click) manipulate together around the merged aabb center.
//
// keys: 1 translate, 2 rotate, 3 scale, g local/global axes, ctrl+f focus the camera,
// escape deselects. rotation uses a linear tangent drag (reference: unity runtime
// transform gizmo), handles render as camera facing quads so the lines have thickness
#include "EditorInternal.h"
#include "Include/Scene.h"
#include "Include/Camera.h"
#include "Include/Rendering.h"
#include "Include/Platform.h"
#include "Math/Quaternion.h"
#include "Math/Bitpack.h"

typedef enum GizmoMode_
{
    GizmoMode_Translate,
    GizmoMode_Rotate,
    GizmoMode_Scale
} GizmoMode;

enum
{
    GizmoAxis_None    = -1,
    GizmoAxis_X       = 0,
    GizmoAxis_Y       = 1,
    GizmoAxis_Z       = 2,
    GizmoAxis_Uniform = 3,
    GizmoAxis_PlaneX  = 4, // plane normal x, drags on yz
    GizmoAxis_PlaneY  = 5,
    GizmoAxis_PlaneZ  = 6
};

// one picked object, the sparse id resolves through groupIdx/entityIdx every frame
typedef struct GizmoTarget_
{
    u32 skinned;
    u32 groupIdx;
    u32 entityIdx;
} GizmoTarget;

enum { GIZMO_MAX_TARGETS = 2048 };
static GizmoTarget gizmoTargets[GIZMO_MAX_TARGETS];
static u32 gizmoNumTargets;

// one entity of the manipulated objects, captured fresh every idle frame and frozen
// during a drag
typedef struct GizmoMember_
{
    u32 skinned;
    u32 groupIdx;
    u32 entityIdx;
    v128f startPos;
    v128f startRot;
    v128f startScale;  // stored 0..1 form
    v128f localCenter; // group aabb center in entity local space
    v128f startCenter; // world aabb center
} GizmoMember;

enum { GIZMO_MAX_MEMBERS = 2048 };
static GizmoMember gizmoMembers[GIZMO_MAX_MEMBERS];
static u32 gizmoNumMembers;
static f32 gizmoFocusRadius; // rough world radius of the selection, for camera focus

typedef struct GizmoState_
{
    GizmoMode mode;
    bool localSpace;      // g toggles, translate/rotate axes follow the picked rotation
    s32  hotAxis;
    bool dragging;
    f32  dragStart;       // delta to apply: axis offset, accumulated angle or scale factor
    f32  dragReference;   // axis param / mouse y at the drag start
    v128f startPivot;
    v128f startPlaneHit; // plane translate start, also the previous drag-plane hit for rotation
    v128f planeDelta;
    v128f rotateTangent; // ring tangent at the grab point, world space
} GizmoState;

static GizmoState gizmo = { .hotAxis = GizmoAxis_None };

static const u32 kGizmoAxisColors[3] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u }; // r g b
#define GIZMO_HOT_COLOR    0xFF00FFFFu
#define GIZMO_CENTER_COLOR 0xFFCCCCCCu

typedef struct GizmoDuplicateRecord_
{
    u32 skinned;
    u32 groupIdx;
    Entity entity;
} GizmoDuplicateRecord;

/*//////////////////////////////////////////////////////////////////////////*/
/*                                Targets                                   */
/*//////////////////////////////////////////////////////////////////////////*/

void EditorGizmoClear(void)
{
    gizmoNumTargets = 0;
    gizmo.dragging = false;
    gizmo.hotAxis = GizmoAxis_None;
    RendererSetGizmoLines(NULL, 0);
    RendererClearOutlineTarget();
}

void EditorGizmoSetTarget(u32 skinned, u32 groupIdx, u32 entityIdx)
{
    gizmoNumTargets = 1;
    gizmoTargets[0] = (GizmoTarget){ skinned, groupIdx, entityIdx };
    gizmo.dragging = false;
    gizmo.hotAxis = GizmoAxis_None;
}

// out: the sparse id of a target, INVALID_ENTITY when it no longer resolves
static u32 GizmoTargetSparseId(const Scene* scene, const GizmoTarget* target)
{
    const RenderSet* set = target->skinned ? &scene->skinnedSet : &scene->surfaceSet;
    if (target->groupIdx >= set->numGroups) return INVALID_ENTITY;
    const PrimitiveGroup* group = &set->primitiveGroups[target->groupIdx];
    if (target->entityIdx >= group->numEntities) return INVALID_ENTITY;
    return set->entities[group->entityOffset + target->entityIdx].sparseIdx;
}

// ctrl click: toggles the object in the selection
void EditorGizmoAddTarget(u32 skinned, u32 groupIdx, u32 entityIdx)
{
    Scene* scene = Scene_GetActive();
    if (!scene) return;

    GizmoTarget added = (GizmoTarget){ skinned, groupIdx, entityIdx };
    u32 addedSparse = GizmoTargetSparseId(scene, &added);
    if (addedSparse == INVALID_ENTITY) return;

    for (u32 i = 0; i < gizmoNumTargets; i++)
    {
        if (gizmoTargets[i].skinned == skinned && GizmoTargetSparseId(scene, &gizmoTargets[i]) == addedSparse)
        {
            gizmoTargets[i] = gizmoTargets[--gizmoNumTargets]; // toggle off
            return;
        }
    }
    if (gizmoNumTargets < GIZMO_MAX_TARGETS)
        gizmoTargets[gizmoNumTargets++] = added;
}

bool EditorGizmoDuplicateSelected(void)
{
    Scene* scene = Scene_GetActive();
    if (!scene || gizmoNumTargets == 0u) return false;

    static GizmoDuplicateRecord records[GIZMO_MAX_MEMBERS];
    static GizmoTarget newTargets[GIZMO_MAX_TARGETS];
    u32 numNewTargets = 0u;
    bool duplicated = false;

    for (u32 t = 0u; t < gizmoNumTargets && numNewTargets < GIZMO_MAX_TARGETS; t++)
    {
        GizmoTarget target = gizmoTargets[t];
        RenderSet* set = target.skinned ? &scene->skinnedSet : &scene->surfaceSet;
        u32 sparseIdx = GizmoTargetSparseId(scene, &target);
        if (sparseIdx == INVALID_ENTITY) continue;

        u32 numRecords = 0u;
        for (u32 g = 0u; g < set->numGroups && numRecords < GIZMO_MAX_MEMBERS; g++)
        {
            const PrimitiveGroup* group = &set->primitiveGroups[g];
            for (u32 e = 0u; e < group->numEntities && numRecords < GIZMO_MAX_MEMBERS; e++)
            {
                const Entity* entity = &set->entities[group->entityOffset + e];
                if (entity->sparseIdx != sparseIdx) continue;
                records[numRecords++] = (GizmoDuplicateRecord){ target.skinned, g, *entity };
            }
        }
        if (numRecords == 0u) continue;
        u32 newSparse = RenderSet_AllocateSparseID(set);
        if (newSparse == INVALID_ENTITY) continue;
        bool targetSet = false;
        for (u32 i = 0u; i < numRecords; i++)
        {
            GizmoDuplicateRecord* record = &records[i];
            record->entity.sparseIdx = newSparse;
            record->entity.position = VecAdd(record->entity.position, VecSetR(1.0f, 0.0f, 0.0f, 0.0f));
            u32 denseIdx = RenderSet_AddEntity(set, record->groupIdx, &record->entity);
            if (denseIdx == INVALID_ENTITY) continue;
            PrimitiveGroup* group = &set->primitiveGroups[record->groupIdx];
            if (!targetSet && denseIdx >= group->entityOffset)
            {
                newTargets[numNewTargets++] = (GizmoTarget){ target.skinned, record->groupIdx, denseIdx - group->entityOffset };
                targetSet = true;
            }
        }
        duplicated |= targetSet;
        if (!targetSet) RenderSet_FreeSparseID(set, newSparse);

        if (target.skinned)
        {
            // Ensure the new sparse slot has a deterministic animation assignment.
            GPUAnimationInstance instance = { 0 };
            AnimationSystem_SetInstance(&scene->animSystem, newSparse, instance);
        }
    }

    if (!duplicated) return false;
    MemCopy(gizmoTargets, newTargets, numNewTargets * sizeof(GizmoTarget));
    gizmoNumTargets = numNewTargets;
    gizmo.hotAxis = GizmoAxis_None;
    scene->renderDataDirty = 1;
    return true;
}

bool EditorGizmoDeleteSelected(void)
{
    Scene* scene = Scene_GetActive();
    if (!scene || gizmoNumTargets == 0u) return false;

    u32 surfaceSparse[GIZMO_MAX_TARGETS];
    u32 skinnedSparse[GIZMO_MAX_TARGETS];
    u32 numSurface = 0u;
    u32 numSkinned = 0u;

    for (u32 t = 0u; t < gizmoNumTargets; t++)
    {
        GizmoTarget* target = &gizmoTargets[t];
        u32 sparseIdx = GizmoTargetSparseId(scene, target);
        if (sparseIdx == INVALID_ENTITY) continue;

        u32* list = target->skinned ? skinnedSparse : surfaceSparse;
        u32* count = target->skinned ? &numSkinned : &numSurface;
        bool exists = false;
        for (u32 i = 0u; i < *count; i++)
            if (list[i] == sparseIdx) exists = true;
        if (!exists && *count < GIZMO_MAX_TARGETS)
            list[(*count)++] = sparseIdx;
    }

    bool removed = false;
    for (u32 pass = 0u; pass < 2u; pass++)
    {
        RenderSet* set = pass == 0u ? &scene->surfaceSet : &scene->skinnedSet;
        u32* sparseList = pass == 0u ? surfaceSparse : skinnedSparse;
        u32 sparseCount = pass == 0u ? numSurface : numSkinned;
        if (sparseCount == 0u) continue;

        for (s32 g = (s32)set->numGroups - 1; g >= 0; g--)
        {
            PrimitiveGroup* group = &set->primitiveGroups[g];
            for (s32 e = (s32)group->numEntities - 1; e >= 0; e--)
            {
                Entity* entity = &set->entities[group->entityOffset + (u32)e];
                for (u32 i = 0u; i < sparseCount; i++)
                {
                    if (entity->sparseIdx == sparseList[i])
                    {
                        RenderSet_RemoveEntity(set, (u32)g, (u32)e);
                        removed = true;
                        break;
                    }
                }
            }
        }
    }

    if (!removed) return false;
    gizmoNumTargets = 0u;
    gizmo.dragging = false;
    gizmo.hotAxis = GizmoAxis_None;
    scene->renderDataDirty = 1;
    RendererSetGizmoLines(NULL, 0);
    RendererClearOutlineTarget();
    return true;
}

// gathers every entity of every target and the merged center of their aabb centers.
// out: number of members, 0 when nothing resolves anymore
static u32 GizmoCollectMembers(Scene* scene, v128f* outCenter)
{
    gizmoNumMembers = 0;
    gizmoFocusRadius = 0.0f;
    v128f bmin = VecSet1(1.0e30f);
    v128f bmax = VecSet1(-1.0e30f);

    for (u32 t = 0; t < gizmoNumTargets; t++)
    {
        const GizmoTarget* target = &gizmoTargets[t];
        RenderSet* set = target->skinned ? &scene->skinnedSet : &scene->surfaceSet;
        u32 sparseIdx = GizmoTargetSparseId(scene, target);
        if (sparseIdx == INVALID_ENTITY) continue;

        for (u32 g = 0; g < set->numGroups && gizmoNumMembers < GIZMO_MAX_MEMBERS; g++)
        {
            const PrimitiveGroup* group = &set->primitiveGroups[g];
            v128f mn = group->aabbMin;
            v128f mx = group->aabbMax;
            v128f localCenter = VecMulf(VecAdd(mn, mx), 0.5f);
            v128f localExtent = VecMulf(VecSub(mx, mn), 0.5f);
            for (u32 e = 0; e < group->numEntities && gizmoNumMembers < GIZMO_MAX_MEMBERS; e++)
            {
                const Entity* entity = &set->entities[group->entityOffset + e];
                if (entity->sparseIdx != sparseIdx) continue;

                GizmoMember* member = &gizmoMembers[gizmoNumMembers++];
                member->skinned = target->skinned;
                member->groupIdx = g;
                member->entityIdx = e;
                member->startPos = entity->position;
                member->startRot = VecNorm(UnpackQuaternionS16Norm1(entity->rotation));
                member->startScale = UnpackUnorm16x4(entity->scale);
                member->localCenter = localCenter;

                v128f worldScale = VecMulf(member->startScale, 10.0f);
                v128f rotation = member->startRot;
                v128f center = VecAdd(QMulVec3V(VecMul(localCenter, worldScale), rotation), entity->position);
                member->startCenter = center;
                bmin = VecMin(bmin, center);
                bmax = VecMax(bmax, center);

                v128f worldExtent = VecMul(localExtent, worldScale);
                gizmoFocusRadius = Maxf32(gizmoFocusRadius, Sqrtf(Vec3DotfV(worldExtent, worldExtent)));
            }
        }
    }
    if (gizmoNumMembers == 0) return 0;

    v128f spread = VecMulf(VecSub(bmax, bmin), 0.5f);
    gizmoFocusRadius += Sqrtf(Vec3DotfV(spread, spread));
    *outCenter = VecMulf(VecAdd(bmin, bmax), 0.5f);
    return gizmoNumMembers;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                              Intersection                                */
/*//////////////////////////////////////////////////////////////////////////*/

// closest parameter on the axis line to the ray, both directions unit length
static f32 GizmoAxisParam(v128f rayOrigin, v128f rayDir, v128f axisOrigin, v128f axisDir)
{
    v128f w0 = VecSub(rayOrigin, axisOrigin);
    f32 b = Vec3DotfV(rayDir, axisDir);
    f32 d = Vec3DotfV(rayDir, w0);
    f32 e = Vec3DotfV(axisDir, w0);
    f32 denom = 1.0f - b * b;
    if (denom < 1.0e-6f) return e;
    return (e - b * d) / denom;
}

// distance between the ray and the closest point of the axis segment [0, length]
static f32 GizmoAxisDistance(v128f rayOrigin, v128f rayDir, v128f axisOrigin, v128f axisDir, f32 length, f32* outParam)
{
    f32 t = Clampf32(GizmoAxisParam(rayOrigin, rayDir, axisOrigin, axisDir), 0.0f, length);
    v128f axisPoint = VecAdd(axisOrigin, VecMulf(axisDir, t));
    f32 s = Maxf32(Vec3DotfV(VecSub(axisPoint, rayOrigin), rayDir), 0.0f);
    v128f rayPoint = VecAdd(rayOrigin, VecMulf(rayDir, s));
    v128f delta = VecSub(axisPoint, rayPoint);
    *outParam = t;
    return Sqrtf(Vec3DotfV(delta, delta));
}

// stable perpendicular basis of a rotation ring
static void GizmoRingBasis(v128f axis, v128f* u, v128f* v)
{
    v128f up = Absf32(VecGetY(axis)) > 0.9f ? VecSetR(1.0f, 0.0f, 0.0f, 0.0f) : VecSetR(0.0f, 1.0f, 0.0f, 0.0f);
    *u = Vec3NormV(Vec3Cross(axis, up));
    *v = Vec3NormV(Vec3Cross(axis, *u));
}

// ray vs the ring plane. out: angle of the hit around the axis and distance to the ring
static bool GizmoRingHit(v128f rayOrigin, v128f rayDir, v128f center, v128f axis, f32 radius, f32* outAngle, f32* outRingDist)
{
    v128f hitPoint;
    if (!RayPlaneHit(rayOrigin, rayDir, center, axis, &hitPoint)) return false;
    v128f rel = VecSub(hitPoint, center);
    v128f u, v;
    GizmoRingBasis(axis, &u, &v);
    *outAngle = ATan2(Vec3DotfV(rel, v), Vec3DotfV(rel, u));
    *outRingDist = Absf32(Sqrtf(Vec3DotfV(rel, rel)) - radius);
    return true;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                                Drawing                                   */
/*//////////////////////////////////////////////////////////////////////////*/

static ALineVertex gizmoVerts[MAX_GIZMO_VERTICES];
static OutlineTarget gizmoOutlines[GIZMO_MAX_MEMBERS];
static u32 gizmoNumVerts;
static v128f gizmoCamPos;
static f32 gizmoLineWidth;

static void GizmoVertex(v128f p, u32 color)
{
    gizmoVerts[gizmoNumVerts++] = (ALineVertex){ VecGetX(p), VecGetY(p), VecGetZ(p), color };
}

static void GizmoQuad(v128f a, v128f b, v128f c, v128f d, u32 color)
{
    if (gizmoNumVerts + 6u > MAX_GIZMO_VERTICES) return;
    GizmoVertex(a, color); GizmoVertex(b, color); GizmoVertex(c, color);
    GizmoVertex(a, color); GizmoVertex(c, color); GizmoVertex(d, color);
}

// camera facing thick line, two triangles
static void GizmoLine(v128f a, v128f b, u32 color)
{
    v128f dir = VecSub(b, a);
    f32 len = Sqrtf(Vec3DotfV(dir, dir));
    if (len < 1.0e-6f) return;
    dir = VecMulf(dir, 1.0f / len);

    v128f view = VecSub(VecMulf(VecAdd(a, b), 0.5f), gizmoCamPos);
    v128f side = Vec3Cross(dir, view);
    f32 sideLen = Sqrtf(Vec3DotfV(side, side));
    if (sideLen < 1.0e-6f) return;
    side = VecMulf(side, gizmoLineWidth * 0.5f / sideLen);

    GizmoQuad(VecAdd(a, side), VecAdd(b, side), VecSub(b, side), VecSub(a, side), color);
}

static void GizmoDrawArrow(v128f center, v128f axisDir, f32 size, u32 color)
{
    v128f tip = VecAdd(center, VecMulf(axisDir, size));
    GizmoLine(center, tip, color);

    v128f u, v;
    GizmoRingBasis(axisDir, &u, &v);
    v128f back = VecSub(tip, VecMulf(axisDir, size * 0.15f));
    GizmoLine(tip, VecAdd(back, VecMulf(u, size * 0.06f)), color);
    GizmoLine(tip, VecSub(back, VecMulf(u, size * 0.06f)), color);
    GizmoLine(tip, VecAdd(back, VecMulf(v, size * 0.06f)), color);
    GizmoLine(tip, VecSub(back, VecMulf(v, size * 0.06f)), color);
}

// filled quad between [start, end] on the two axes spanning the plane
static void GizmoDrawPlane(v128f center, v128f spanA, v128f spanB, f32 start, f32 end, u32 color)
{
    v128f a = VecAdd(center, VecAdd(VecMulf(spanA, start), VecMulf(spanB, start)));
    v128f b = VecAdd(center, VecAdd(VecMulf(spanA, end),   VecMulf(spanB, start)));
    v128f c = VecAdd(center, VecAdd(VecMulf(spanA, end),   VecMulf(spanB, end)));
    v128f d = VecAdd(center, VecAdd(VecMulf(spanA, start), VecMulf(spanB, end)));
    GizmoQuad(a, b, c, d, color);
}

static void GizmoDrawScaleHandle(v128f center, v128f axisDir, f32 size, u32 color)
{
    v128f tip = VecAdd(center, VecMulf(axisDir, size));
    GizmoLine(center, tip, color);

    v128f u, v;
    GizmoRingBasis(axisDir, &u, &v);
    f32 s = size * 0.06f;
    GizmoQuad(VecAdd(tip, VecAdd(VecMulf(u,  s), VecMulf(v,  s))),
              VecAdd(tip, VecAdd(VecMulf(u,  s), VecMulf(v, -s))),
              VecAdd(tip, VecAdd(VecMulf(u, -s), VecMulf(v, -s))),
              VecAdd(tip, VecAdd(VecMulf(u, -s), VecMulf(v,  s))), color);
}

static void GizmoDrawRing(v128f center, v128f axis, f32 radius, u32 color)
{
    enum { Segments = 32 };
    v128f u, v;
    GizmoRingBasis(axis, &u, &v);
    v128f prev = VecAdd(center, VecMulf(u, radius));
    for (int i = 1; i <= Segments; i++)
    {
        f32 angle = (f32)i * (MATH_TwoPI / (f32)Segments);
        v128f point = VecAdd(center, VecAdd(VecMulf(u, Cos(angle) * radius), VecMulf(v, Sin(angle) * radius)));
        GizmoLine(prev, point, color);
        prev = point;
    }
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                                 Update                                   */
/*//////////////////////////////////////////////////////////////////////////*/

// handle directions: world axes, or the picked object's local axes when local space is
// on. scale is always local, entity scale is stored per local axis
static void GizmoHandleAxes(v128f rotation, v128f axes[3])
{
    axes[0] = VecSetR(1.0f, 0.0f, 0.0f, 0.0f);
    axes[1] = VecSetR(0.0f, 1.0f, 0.0f, 0.0f);
    axes[2] = VecSetR(0.0f, 0.0f, 1.0f, 0.0f);
    if (gizmo.localSpace || gizmo.mode == GizmoMode_Scale)
        for (int i = 0; i < 3; i++)
            axes[i] = Vec3NormV(QMulVec3V(axes[i], rotation));
}

// applies the current drag delta to every member around the shared pivot. each member
// keeps its own aabb center pinned to its transformed location
static void GizmoApplyMembers(Scene* scene, const v128f axes[3], f32 mouseY)
{
    v128f pivot = gizmo.startPivot;

    for (u32 m = 0; m < gizmoNumMembers; m++)
    {
        GizmoMember* member = &gizmoMembers[m];
        RenderSet* set = member->skinned ? &scene->skinnedSet : &scene->surfaceSet;
        const PrimitiveGroup* group = &set->primitiveGroups[member->groupIdx];
        Entity* entity = &set->entities[group->entityOffset + member->entityIdx];

        v128f startPos    = member->startPos;
        v128f startRot    = member->startRot;
        v128f startCenter = member->startCenter;
        v128f localCenter = member->localCenter;

        if (gizmo.mode == GizmoMode_Translate)
        {
            v128f delta = gizmo.hotAxis >= GizmoAxis_PlaneX
                ? gizmo.planeDelta
                : VecMulf(axes[gizmo.hotAxis], gizmo.dragStart);
            entity->position = VecAdd(startPos, delta);
        }
        else if (gizmo.mode == GizmoMode_Rotate)
        {
            v128f deltaQ = QFromAxisAngleV(axes[gizmo.hotAxis], gizmo.dragStart);
            v128f newQ = VecNorm(QMul(startRot, deltaQ)); // extra world rotation
            PackQuaternionS16Norm(newQ, &entity->rotation);

            // the member's center orbits the shared pivot, then its own center pins
            // to that location: T = C' - R' * (localCenter * worldScale)
            v128f newCenter = VecAdd(QMulVec3V(VecSub(startCenter, pivot), deltaQ), pivot);
            v128f worldScale = VecMulf(member->startScale, 10.0f);
            entity->position = VecSub(newCenter, QMulVec3V(VecMul(localCenter, worldScale), newQ));
        }
        else // scale
        {
            f32 factor;
            if (gizmo.hotAxis == GizmoAxis_Uniform)
                factor = 1.0f + (gizmo.dragReference - mouseY) * 0.005f;
            else
                factor = gizmo.dragStart;
            factor = Clampf32(factor, 0.02f, 100.0f);

            v128f scaleFactor = gizmo.hotAxis == GizmoAxis_Uniform
                ? VecSet1(factor)
                : VecSetR(gizmo.hotAxis == 0 ? factor : 1.0f,
                          gizmo.hotAxis == 1 ? factor : 1.0f,
                          gizmo.hotAxis == 2 ? factor : 1.0f,
                          0.0f);
            v128f scale = VecClamp(VecMul(member->startScale, scaleFactor), VecSet1(0.0001f), VecSet1(1.0f));
            VecSetW(scale, 0.0f);
            entity->scale = EntityPackWorldScale(VecMulf(scale, ENTITY_MAX_SCALE));

            // the member center moves with the scale: radially for uniform, along the
            // handle's world direction for a single axis
            v128f rel = VecSub(startCenter, pivot);
            if (gizmo.hotAxis == GizmoAxis_Uniform)
                rel = VecMulf(rel, factor);
            else
            {
                f32 along = Vec3DotfV(rel, axes[gizmo.hotAxis]);
                rel = VecAdd(rel, VecMulf(axes[gizmo.hotAxis], along * (factor - 1.0f)));
            }
            v128f newCenter = VecAdd(pivot, rel);
            v128f worldScale = VecMulf(scale, 10.0f);
            entity->position = VecSub(newCenter, QMulVec3V(VecMul(localCenter, worldScale), startRot));
        }

        if (!member->skinned)
            Scene_PhysicsSyncEntityBody(scene, false, member->groupIdx, entity);
    }
}

// out: true while the gizmo owns the mouse, the caller skips picking then
bool EditorGizmoUpdate(Camera* camera)
{
    Scene* scene = Scene_GetActive();
    if (gizmoNumTargets == 0 || !scene)
        return false;

    if (GetKeyPressed(27) && !gizmo.dragging) // escape deselects
    {
        EditorGizmoClear();
        return false;
    }

    if (GetKeyPressed('1')) gizmo.mode = GizmoMode_Translate;
    if (GetKeyPressed('2')) gizmo.mode = GizmoMode_Rotate;
    if (GetKeyPressed('3')) gizmo.mode = GizmoMode_Scale;
    if (GetKeyPressed('g'))
    {
        gizmo.localSpace = !gizmo.localSpace;
        AX_LOG("gizmo space: %s", gizmo.localSpace ? "local" : "global");
    }

    // while idle re-collect the members and the merged center every frame, a drag works
    // on the captured snapshot and keeps the pivot fixed
    v128f center;
    if (!gizmo.dragging)
    {
        if (!GizmoCollectMembers(scene, &center))
        {
            EditorGizmoClear();
            return false;
        }
        gizmo.startPivot = center;
    }
    else
        center = gizmo.startPivot;

    // every member outlines, the whole selection highlights
    {
        for (u32 m = 0; m < gizmoNumMembers; m++)
            gizmoOutlines[m] = (OutlineTarget){ gizmoMembers[m].skinned, gizmoMembers[m].groupIdx, gizmoMembers[m].entityIdx };
        RendererSetOutlineTargets(gizmoOutlines, gizmoNumMembers);
    }

    // ctrl+f focuses the camera on the selection
    if (GetKeyDown(SDLK_LCTRL) && GetKeyPressed('f'))
    {
        f32 focusDistance = Maxf32(gizmoFocusRadius * 2.5f, 1.0f);
        camera->position.x = VecGetX(center) - camera->Front.x * focusDistance;
        camera->position.y = VecGetY(center) - camera->Front.y * focusDistance;
        camera->position.z = VecGetZ(center) - camera->Front.z * focusDistance;
        Camera_RecalculateView(camera);
    }

    v128f pickedRotation = gizmoMembers[0].startRot;
    v128f camPos = VecLoad(&camera->position.x);
    v128f toCenter = VecSub(center, camPos);
    f32 size = Maxf32(Sqrtf(Vec3DotfV(toCenter, toCenter)) * 0.15f, 0.05f);

    v128f axes[3];
    GizmoHandleAxes(pickedRotation, axes);

    float2 sceneMouse = EditorSceneMouse();
    f32 mx = sceneMouse.x, my = sceneMouse.y;
    RayV ray = ScreenPointToRay(camera, sceneMouse);

    bool consumed = false;
    if (gizmo.dragging)
    {
        if (!GetMouseDown(MouseButton_Left))
        {
            gizmo.dragging = false;
        }
        else
        {
            v128f startPivot = gizmo.startPivot;
            bool apply = false;

            if (gizmo.hotAxis >= GizmoAxis_PlaneX) // plane translate, free in two axes
            {
                v128f normal = axes[gizmo.hotAxis - GizmoAxis_PlaneX];
                v128f hitPoint;
                if (RayPlaneHit(ray.origin, ray.dir, startPivot, normal, &hitPoint))
                {
                    gizmo.planeDelta = VecSub(hitPoint, gizmo.startPlaneHit);
                    apply = true;
                }
            }
            else if (gizmo.mode == GizmoMode_Translate || (gizmo.mode == GizmoMode_Scale && gizmo.hotAxis != GizmoAxis_Uniform))
            {
                f32 t = GizmoAxisParam(ray.origin, ray.dir, startPivot, axes[gizmo.hotAxis]);
                if (gizmo.mode == GizmoMode_Translate)
                    gizmo.dragStart = t - gizmo.dragReference;
                else
                    gizmo.dragStart = Clampf32(1.0f + (t - gizmo.dragReference) / Maxf32(size * 0.5f, 1.0e-4f), 0.02f, 100.0f);
                apply = true;
            }
            else if (gizmo.mode == GizmoMode_Rotate)
            {
                // linear rotation drag: the mouse hit on a camera facing plane (never
                // edge on) moves along the ring tangent of the grab point, each frame
                // adds a small increment to the accumulated angle
                v128f planeNormal = Vec3NormV(VecSub(camPos, startPivot));
                v128f hitPoint;
                if (RayPlaneHit(ray.origin, ray.dir, startPivot, planeNormal, &hitPoint))
                {
                    v128f hitDelta = VecSub(hitPoint, gizmo.startPlaneHit);
                    gizmo.startPlaneHit = hitPoint;
                    f32 along = Vec3DotfV(hitDelta, gizmo.rotateTangent);
                    gizmo.dragStart += along / Maxf32(size, 1.0e-4f) * 2.5f;
                    apply = true;
                }
            }
            else // uniform scale, vertical mouse delta
                apply = true;

            if (apply) GizmoApplyMembers(scene, axes, my);
            consumed = true;
        }
    }
    else
    {
        // hover test, nearest handle wins. axis handles win over the uniform center
        gizmo.hotAxis = GizmoAxis_None;
        f32 best = size * 0.1f; // pick threshold
        f32 bestRef = 0.0f;
        v128f planeHit = VecZero();

        for (int i = 0; i < 3; i++)
        {
            if (gizmo.mode == GizmoMode_Rotate)
            {
                f32 angle, ringDist;
                if (GizmoRingHit(ray.origin, ray.dir, center, axes[i], size, &angle, &ringDist) && ringDist < best)
                {
                    best = ringDist;
                    bestRef = angle;
                    gizmo.hotAxis = i;
                }
            }
            else
            {
                f32 t, dist;
                dist = GizmoAxisDistance(ray.origin, ray.dir, center, axes[i], size, &t);
                if (dist < best)
                {
                    best = dist;
                    bestRef = t;
                    gizmo.hotAxis = i;
                }
            }
        }

        if (gizmo.mode == GizmoMode_Translate && gizmo.hotAxis == GizmoAxis_None)
        {
            // plane handles between 30% and 60% of the gizmo size on both spanning axes
            for (int i = 0; i < 3; i++)
            {
                v128f hitPoint;
                if (!RayPlaneHit(ray.origin, ray.dir, center, axes[i], &hitPoint)) continue;
                v128f rel = VecSub(hitPoint, center);
                f32 a = Vec3DotfV(rel, axes[(i + 1) % 3]);
                f32 b = Vec3DotfV(rel, axes[(i + 2) % 3]);
                if (a >= size * 0.3f && a <= size * 0.6f && b >= size * 0.3f && b <= size * 0.6f)
                {
                    gizmo.hotAxis = GizmoAxis_PlaneX + i;
                    planeHit = hitPoint;
                    break;
                }
            }
        }

        if (gizmo.mode == GizmoMode_Scale && gizmo.hotAxis == GizmoAxis_None)
        {
            // center handle: distance of the ray to the gizmo center
            v128f toC = VecSub(center, ray.origin);
            v128f closest = VecAdd(ray.origin, VecMulf(ray.dir, Maxf32(Vec3DotfV(toC, ray.dir), 0.0f)));
            v128f d = VecSub(center, closest);
            if (Sqrtf(Vec3DotfV(d, d)) < size * 0.15f)
                gizmo.hotAxis = GizmoAxis_Uniform;
        }

        if (gizmo.hotAxis != GizmoAxis_None && GetMousePressed(MouseButton_Left) && EditorSceneInteractAllowed())
        {
            // members and pivot were captured by the collect above this frame
            gizmo.dragging = true;
            gizmo.startPlaneHit = planeHit;
            gizmo.dragReference = gizmo.hotAxis == GizmoAxis_Uniform ? my : bestRef;
            gizmo.dragStart = 0.0f;

            if (gizmo.mode == GizmoMode_Rotate)
            {
                // ring tangent at the grab point drives the linear rotation drag
                v128f u, v;
                GizmoRingBasis(axes[gizmo.hotAxis], &u, &v);
                v128f grabDir = VecAdd(VecMulf(u, Cos(bestRef)), VecMulf(v, Sin(bestRef)));
                gizmo.rotateTangent = Vec3Cross(axes[gizmo.hotAxis], grabDir);

                v128f planeNormal = Vec3NormV(VecSub(camPos, center));
                v128f hitPoint = center;
                RayPlaneHit(ray.origin, ray.dir, center, planeNormal, &hitPoint);
                gizmo.startPlaneHit = hitPoint;
            }
            consumed = true;
        }
    }

    // draw, the active or hovered handle highlights
    gizmoNumVerts = 0;
    gizmoCamPos = camPos;
    gizmoLineWidth = size * 0.02f;
    for (int i = 0; i < 3; i++)
    {
        u32 color = gizmo.hotAxis == i ? GIZMO_HOT_COLOR : kGizmoAxisColors[i];
        if (gizmo.mode == GizmoMode_Translate)   GizmoDrawArrow(center, axes[i], size, color);
        else if (gizmo.mode == GizmoMode_Rotate) GizmoDrawRing(center, axes[i], size, color);
        else                                     GizmoDrawScaleHandle(center, axes[i], size, color);
    }
    if (gizmo.mode == GizmoMode_Translate)
    {
        for (int i = 0; i < 3; i++)
        {
            u32 color = gizmo.hotAxis == GizmoAxis_PlaneX + i ? GIZMO_HOT_COLOR : kGizmoAxisColors[i];
            GizmoDrawPlane(center, axes[(i + 1) % 3], axes[(i + 2) % 3], size * 0.3f, size * 0.6f, color);
        }
    }
    if (gizmo.mode == GizmoMode_Scale)
    {
        v128f camForward = Vec3NormV(VecSub(center, camPos));
        GizmoDrawRing(center, camForward, size * 0.15f,
                      gizmo.hotAxis == GizmoAxis_Uniform ? GIZMO_HOT_COLOR : GIZMO_CENTER_COLOR);
    }
    RendererSetGizmoLines(gizmoVerts, gizmoNumVerts);

    return consumed;
}
