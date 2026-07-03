#ifndef BVH_H
#define BVH_H

#include "GLTFParser.h"
#include "Math/Vector.h"
#include "SIMD.h"

#if defined(__cplusplus)
extern "C" {
#endif

// blas node, ported from the old engine. 32 bytes and tightly packed so the arrays can
// upload to the gpu as is (float4 pairs: aabbMin+leftFirst, aabbMax+triCount)
typedef struct BVHNode_
{
    float3 aabbMin;
    u32    leftFirst; // first child pair index, or first triangle when triCount > 0
    float3 aabbMax;
    u32    triCount;  // leaf when > 0
} BVHNode;

// one triangle record, 16 bytes, vertex indices are mega buffer absolute
typedef struct BVHTri_
{
    u32 v0, v1, v2, padding;
} BVHTri;

typedef struct BVHHit_
{
    RayHit hit;
    u32 triIndex;     // bundle local triangle record
    u32 skinnedSet;   // which render set the entity lives in
    u32 groupIdx;     // primitive group inside the set
    u32 entityIdx;    // group local entity
    u32 bundleIdx;    // scene bundle
} BVHHit;

static inline v128f BVH_HitPositionV(v128f origin, v128f dir, const BVHHit* hit)
{
    return VecAdd(origin, VecMulf(dir, hit->hit.t));
}

static inline float3 BVH_HitPositionF(float3 origin, float3 dir, const BVHHit* hit)
{
    return F3Add(origin, F3MulF(dir, hit->hit.t));
}

// builds the blas of every primitive of the bundle from the lod0 triangles, fills
// APrimitive.bvhNodeIndex and the cache entry's bvhNodes/bvhTris arrays. out: 0 on failure
s32 BVH_BuildBundleCached(SceneBundle* bundle, struct BundleCacheEntry* bundleCache, bool skinned);

void BVH_FreeBundle(struct BundleCacheEntry* bundleCache);

struct Scene_;

// casts a world space ray through every entity of the scene's render sets, the ray is
// transformed into entity local space per instance. out: 1 when *hit is filled
s32 BVH_RaycastScene(const struct Scene_* scene, v128f origin, v128f dir, BVHHit* hit);

// box3d raycast against the static surface colliders, fills *hit in the same BVHHit contract when the
// physics hit is nearer than hit->hit.t. implemented in Physics.c. out: 1 when *hit is written
s32 Scene_PhysicsRaycastPick(const struct Scene_* scene, v128f origin, v128f dir, BVHHit* hit);

#if defined(__cplusplus)
}
#endif

#endif // BVH_H
