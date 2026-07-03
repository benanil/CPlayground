// per primitive blas build and ray traversal, ported from the old engine's BVH.cpp.
// nodes and triangles live in two tightly packed arrays per bundle so they can be
// shared with the gpu later. used by the editor for click picking
#include "Include/BVH.h"
#include "Include/Scene.h"
#include "Include/Graphics.h"
#include "Include/Memory.h"
#include "Include/Platform.h"
#include "Include/Scene.h"
#include "Include/Bitset.h"
#include "Math/Quaternion.h"
#include "Math/Bitpack.h"
#include "Math/Half.h"

extern Graphics gGFX;

enum { BVH_BINS = 8 };
enum { BVH_MAX_DEPTH_STACK = 128 };
#define BVH_MISS 1e30f

/*//////////////////////////////////////////////////////////////////////////*/
/*                                  Build                                   */
/*//////////////////////////////////////////////////////////////////////////*/

typedef struct BVHBuild_
{
    BVHNode* nodes;
    BVHTri*  tris;
    v128f*   centroids; // build only, parallel to tris
    u32      nodesUsed;
    bool     skinned;
    // de-quantization for the static unorm16 positions of the primitive currently building.
    v128f    decodeMin, decodeExtent;
} BVHBuild;

// de-quantization range (min, extent) for a static primitive's unorm16 positions; must match
// the pack in SurfaceVerticesForPrimitive (same 1e-6 zero-extent guard) for an exact roundtrip.
static inline void BVH_PrimitiveDecode(const APrimitive* prim, v128f* outMin, v128f* outExtent)
{
    v128f min = VecLoad(prim->min);
    *outMin = min;
    *outExtent = VecMax(VecSub(VecLoad(prim->max), min), VecSet1(1.0e-6f));
}

// vertex position fetch from the cpu mega buffers, indices are mega absolute.
// static positions are xyz unorm16 vs the primitive AABB; skinned store the bind pose as halves
static v128f BVH_Position(bool skinned, u32 index, v128f decodeMin, v128f decodeExtent)
{
    if (!skinned)
    {
        const AVertex* v = gGFX.SurfaceVertexBuffer + index;
        return VecAdd(decodeMin, VecMul(UnpackUnorm16x4(v->position), decodeExtent));
    }
    const ASkinedVertex* v = gGFX.SkinnedVertexBuffer + index;
    return Half4ToFloat4Vec(&v->positionXY);
}

static void BVH_UpdateNodeBounds(BVHBuild* build, BVHNode* node)
{
    v128f bmin = VecSet1( BVH_MISS);
    v128f bmax = VecSet1(-BVH_MISS);
    for (u32 i = node->leftFirst; i < node->leftFirst + node->triCount; i++)
    {
        const BVHTri* tri = &build->tris[i];
        v128f v0 = BVH_Position(build->skinned, tri->v0, build->decodeMin, build->decodeExtent);
        v128f v1 = BVH_Position(build->skinned, tri->v1, build->decodeMin, build->decodeExtent);
        v128f v2 = BVH_Position(build->skinned, tri->v2, build->decodeMin, build->decodeExtent);
        bmin = VecMin(bmin, VecMin(v0, VecMin(v1, v2)));
        bmax = VecMax(bmax, VecMax(v0, VecMax(v1, v2)));
    }
    Vec3Store(&node->aabbMin.x, bmin);
    Vec3Store(&node->aabbMax.x, bmax);
}

static f32 BVH_AreaOf(v128f bmin, v128f bmax)
{
    v128f e = VecSub(bmax, bmin);
    f32 ex = VecGetX(e), ey = VecGetY(e), ez = VecGetZ(e);
    return ex * ey + ey * ez + ez * ex;
}

typedef struct BVHBin_
{
    v128f bmin, bmax;
    u32 triCount;
} BVHBin;

// sah binned split search, ported from the old engine (8 bins per axis)
static f32 BVH_FindBestSplit(BVHBuild* build, const BVHNode* node, int* outAxis, f32* outPos)
{
    f32 bestCost = BVH_MISS;
    for (int axis = 0; axis < 3; axis++)
    {
        f32 boundsMin = BVH_MISS, boundsMax = -BVH_MISS;
        for (u32 i = node->leftFirst; i < node->leftFirst + node->triCount; i++)
        {
            AX_ALIGN(16) float c[4];
            VecStore(c, build->centroids[i]);
            boundsMin = Minf32(boundsMin, c[axis]);
            boundsMax = Maxf32(boundsMax, c[axis]);
        }
        if (boundsMin == boundsMax) continue;

        BVHBin bins[BVH_BINS];
        for (int b = 0; b < BVH_BINS; b++)
        {
            bins[b].bmin = VecSet1( BVH_MISS);
            bins[b].bmax = VecSet1(-BVH_MISS);
            bins[b].triCount = 0;
        }

        f32 binScale = (f32)BVH_BINS / (boundsMax - boundsMin);
        for (u32 i = node->leftFirst; i < node->leftFirst + node->triCount; i++)
        {
            AX_ALIGN(16) float c[4];
            VecStore(c, build->centroids[i]);
            int binIdx = (int)((c[axis] - boundsMin) * binScale);
            if (binIdx > BVH_BINS - 1) binIdx = BVH_BINS - 1;
            BVHBin* bin = &bins[binIdx];
            bin->triCount++;
            const BVHTri* tri = &build->tris[i];
            v128f v0 = BVH_Position(build->skinned, tri->v0, build->decodeMin, build->decodeExtent);
            v128f v1 = BVH_Position(build->skinned, tri->v1, build->decodeMin, build->decodeExtent);
            v128f v2 = BVH_Position(build->skinned, tri->v2, build->decodeMin, build->decodeExtent);
            bin->bmin = VecMin(bin->bmin, VecMin(v0, VecMin(v1, v2)));
            bin->bmax = VecMax(bin->bmax, VecMax(v0, VecMax(v1, v2)));
        }

        // sweep the bins to get the left/right costs of every split plane
        f32 leftArea[BVH_BINS - 1], rightArea[BVH_BINS - 1];
        u32 leftCount[BVH_BINS - 1], rightCount[BVH_BINS - 1];
        v128f lmin = VecSet1(BVH_MISS), lmax = VecSet1(-BVH_MISS);
        v128f rmin = VecSet1(BVH_MISS), rmax = VecSet1(-BVH_MISS);
        u32 leftSum = 0, rightSum = 0;
        for (int b = 0; b < BVH_BINS - 1; b++)
        {
            leftSum += bins[b].triCount;
            leftCount[b] = leftSum;
            lmin = VecMin(lmin, bins[b].bmin);
            lmax = VecMax(lmax, bins[b].bmax);
            leftArea[b] = BVH_AreaOf(lmin, lmax);

            rightSum += bins[BVH_BINS - 1 - b].triCount;
            rightCount[BVH_BINS - 2 - b] = rightSum;
            rmin = VecMin(rmin, bins[BVH_BINS - 1 - b].bmin);
            rmax = VecMax(rmax, bins[BVH_BINS - 1 - b].bmax);
            rightArea[BVH_BINS - 2 - b] = BVH_AreaOf(rmin, rmax);
        }

        f32 binWidth = (boundsMax - boundsMin) / (f32)BVH_BINS;
        for (int b = 0; b < BVH_BINS - 1; b++)
        {
            f32 cost = (f32)leftCount[b] * leftArea[b] + (f32)rightCount[b] * rightArea[b];
            if (cost > 0.0f && cost < bestCost)
            {
                bestCost = cost;
                *outAxis = axis;
                *outPos = boundsMin + binWidth * (f32)(b + 1);
            }
        }
    }
    return bestCost;
}

static void BVH_Subdivide(BVHBuild* build, u32 nodeIdx)
{
    BVHNode* node = &build->nodes[nodeIdx];
    if (node->triCount <= 2) return;

    int axis = 0;
    f32 splitPos = 0.0f;
    f32 splitCost = BVH_FindBestSplit(build, node, &axis, &splitPos);

    v128f bmin = Vec3Load(&node->aabbMin.x);
    v128f bmax = Vec3Load(&node->aabbMax.x);
    f32 noSplitCost = (f32)node->triCount * BVH_AreaOf(bmin, bmax);
    if (splitCost >= noSplitCost) return;

    // partition the triangles (and their centroids) in place around the split plane
    u32 i = node->leftFirst;
    u32 j = i + node->triCount - 1;
    while (i <= j && j != ~0u)
    {
        AX_ALIGN(16) float c[4];
        VecStore(c, build->centroids[i]);
        if (c[axis] < splitPos)
            i++;
        else
        {
            BVHTri tmpTri = build->tris[i]; build->tris[i] = build->tris[j]; build->tris[j] = tmpTri;
            v128f tmpC = build->centroids[i]; build->centroids[i] = build->centroids[j]; build->centroids[j] = tmpC;
            j--;
        }
    }

    u32 leftCount = i - node->leftFirst;
    if (leftCount == 0 || leftCount == node->triCount) return;

    u32 leftChild = build->nodesUsed;
    build->nodesUsed += 2;
    build->nodes[leftChild].leftFirst      = node->leftFirst;
    build->nodes[leftChild].triCount       = leftCount;
    build->nodes[leftChild + 1].leftFirst  = i;
    build->nodes[leftChild + 1].triCount   = node->triCount - leftCount;
    node->leftFirst = leftChild;
    node->triCount = 0;

    BVH_UpdateNodeBounds(build, &build->nodes[leftChild]);
    BVH_UpdateNodeBounds(build, &build->nodes[leftChild + 1]);
    BVH_Subdivide(build, leftChild);
    BVH_Subdivide(build, leftChild + 1);
}

s32 BVH_BuildBundleCached(SceneBundle* bundle, BundleCacheEntry* bundleCache, bool skinned)
{
    if (bundleCache->bvhNodes || !bundle->allIndices) return bundleCache->bvhNodes != NULL;

    double startTime = TimeSinceStartup();
    u32 totalTris = 0;
    for (int m = 0; m < bundle->numMeshes; m++)
        for (int p = 0; p < bundle->meshes[m].numPrimitives; p++)
            totalTris += (u32)bundle->meshes[m].primitives[p].lodNumIndices[0] / 3u;
    if (totalTris == 0) return 0;

    // worst case two nodes per triangle, the arrays shrink to the used size after the
    // build. heap allocated, the engine tlsf pool is too small for big scenes
    BVHBuild build;
    build.nodes     = (BVHNode*)SDL_malloc((u64)totalTris * 2u * sizeof(BVHNode));
    build.tris      = (BVHTri*)SDL_aligned_alloc(sizeof(v128f), (u64)totalTris * sizeof(BVHTri));
    build.centroids = (v128f*)SDL_aligned_alloc(sizeof(v128f), (u64)totalTris * sizeof(v128f));
    build.nodesUsed = 0;
    build.skinned   = skinned;
    if (!build.nodes || !build.tris || !build.centroids)
    {
        if (build.nodes) SDL_free(build.nodes);
        if (build.tris)  SDL_aligned_free(build.tris);
        if (build.centroids) SDL_aligned_free(build.centroids);
        AX_ERROR("bvh build allocation failed: tris=%d", totalTris);
        return 0;
    }

    const f32 third = 1.0f / 3.0f;
    u32 triCursor = 0;
    for (int m = 0; m < bundle->numMeshes; m++)
    {
        for (int p = 0; p < bundle->meshes[m].numPrimitives; p++)
        {
            APrimitive* prim = &bundle->meshes[m].primitives[p];
            u32 numTris = (u32)prim->lodNumIndices[0] / 3u;
            if (numTris == 0)
            {
                prim->bvhNodeIndex = ~0u;
                continue;
            }

            u32 firstTri = triCursor;
            const u32* indices = gGFX.IndexBuffer + prim->lodIndexOffset[0];
            u32 vertexBegin = (u32)prim->lodVertexOffset[0];
            u32 vertexEnd = vertexBegin + (u32)prim->lodNumVertices[0];
            // de-quantization range for this primitive's static unorm16 positions
            BVH_PrimitiveDecode(prim, &build.decodeMin, &build.decodeExtent);
            for (u32 t = 0; t < numTris; t++)
            {
                BVHTri* tri = &build.tris[triCursor];
                tri->v0 = indices[t * 3u + 0u];
                tri->v1 = indices[t * 3u + 1u];
                tri->v2 = indices[t * 3u + 2u];
                v128u triV = VeciLoad(&tri->v0);
                v128u cmp = VeciOr(VeciCmpLt(triV, VeciSet1(vertexBegin)), VeciCmpGe(triV, VeciSet1(vertexEnd)));

                if (Maxi3(cmp) > 0)
                {
                    AX_WARN("bvh primitive skipped: invalid index mesh=%d primitive=%d tri=%d vertices=%d..%d idx=(%d,%d,%d) indexOffset=%d numIndices=%d",
                            m, p, t, vertexBegin, vertexEnd, tri->v0, tri->v1, tri->v2, prim->lodIndexOffset[0], prim->lodNumIndices[0]);
                    triCursor = firstTri;
                    prim->bvhNodeIndex = ~0u;
                    goto next_primitive;
                }
                tri->padding = 0;
                v128f sum = VecAdd(VecAdd(BVH_Position(skinned, tri->v0, build.decodeMin, build.decodeExtent),
                                          BVH_Position(skinned, tri->v1, build.decodeMin, build.decodeExtent)),
                                   BVH_Position(skinned, tri->v2, build.decodeMin, build.decodeExtent));
                build.centroids[triCursor] = VecMulf(sum, third);
                triCursor++;
            }

            u32 rootIdx = build.nodesUsed++;
            prim->bvhNodeIndex = rootIdx;
            build.nodes[rootIdx].leftFirst = firstTri;
            build.nodes[rootIdx].triCount = numTris;
            BVH_UpdateNodeBounds(&build, &build.nodes[rootIdx]);
            BVH_Subdivide(&build, rootIdx);
next_primitive:;
        }
    }

    SDL_aligned_free(build.centroids);
    bundleCache->bvhNodes = SDL_realloc(build.nodes, (u64)build.nodesUsed * sizeof(BVHNode));
    bundleCache->bvhTris = build.tris;
    bundleCache->numBvhNodes = (int)build.nodesUsed;
    bundleCache->numBvhTris = (int)totalTris;
    AX_LOG("bvh built: tris=%d nodes=%d %.2fs", totalTris, build.nodesUsed, TimeSinceStartup() - startTime);
    return 1;
}

void BVH_FreeBundle(BundleCacheEntry* bundleCache)
{
    if (bundleCache->bvhNodes) SDL_free(bundleCache->bvhNodes);            // SDL_malloc/realloc
    if (bundleCache->bvhTris) SDL_aligned_free(bundleCache->bvhTris);      // SDL_aligned_alloc
    bundleCache->bvhNodes = NULL;
    bundleCache->bvhTris = NULL;
    bundleCache->numBvhNodes = 0;
    bundleCache->numBvhTris = 0;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                                Traversal                                 */
/*//////////////////////////////////////////////////////////////////////////*/

// stack based traversal of one primitive blas, the ray is already in local space
static bool BVH_Intersect(const BundleCacheEntry* bundleCache, bool skinned, u32 rootNode,
                          v128f decodeMin, v128f decodeExtent, v128f origin, v128f dir, BVHHit* hit)
{
    const BVHNode* nodes = (const BVHNode*)bundleCache->bvhNodes;
    const BVHTri* tris = (const BVHTri*)bundleCache->bvhTris;
    if (!nodes || rootNode == ~0u) return false;

    u32 nodesToVisit[BVH_MAX_DEPTH_STACK];
    nodesToVisit[0] = rootNode;
    int stackCount = 1;
    // full precision reciprocal, the approximate VecRcp misses thin aabbs
    v128f invDir = VecDiv(VecSet1(1.0f), dir);
    bool intersection = false;

    while (stackCount > 0)
    {
        const BVHNode* node = nodes + nodesToVisit[--stackCount];
    traverse:;
        u32 triCount = node->triCount;
        u32 leftFirst = node->leftFirst;
        if (triCount > 0) // leaf
        {
            for (u32 i = leftFirst; i < leftFirst + triCount; i++)
            {
                const BVHTri* tri = tris + i;
                v128f v0 = BVH_Position(skinned, tri->v0, decodeMin, decodeExtent);
                v128f v1 = BVH_Position(skinned, tri->v1, decodeMin, decodeExtent);
                v128f v2 = BVH_Position(skinned, tri->v2, decodeMin, decodeExtent);
                if (IntersectTriangle(origin, dir, v0, v1, v2, &hit->hit))
                {
                    hit->triIndex = i;
                    intersection = true;
                }
            }
            continue;
        }

        u32 leftIndex = leftFirst;
        u32 rightIndex = leftIndex + 1;
        const BVHNode* leftNode  = nodes + leftIndex;
        const BVHNode* rightNode = nodes + rightIndex;

        v128f lmin = Vec3Load(&leftNode->aabbMin.x);
        v128f lmax = Vec3Load(&leftNode->aabbMax.x);
        v128f rmin = Vec3Load(&rightNode->aabbMin.x);
        v128f rmax = Vec3Load(&rightNode->aabbMax.x);

        f32 dist1 = IntersectAABB(origin, invDir, lmin, lmax, hit->hit.t);
        f32 dist2 = IntersectAABB(origin, invDir, rmin, rmax, hit->hit.t);

        if (dist1 > dist2)
        {
            f32 tmpD = dist1; dist1 = dist2; dist2 = tmpD;
            u32 tmpI = leftIndex; leftIndex = rightIndex; rightIndex = tmpI;
        }

        if (dist1 == BVH_MISS) continue;
        node = nodes + leftIndex;
        if (dist2 != BVH_MISS && stackCount < BVH_MAX_DEPTH_STACK)
            nodesToVisit[stackCount++] = rightIndex;
        goto traverse;
    }
    return intersection;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                              Scene Raycast                               */
/*//////////////////////////////////////////////////////////////////////////*/

static s32 BVH_RaycastSet(const Scene* scene, const RenderSet* set, bool skinned, v128f origin, v128f dir, BVHHit* hit)
{
    s32 anyHit = 0;
    for (u32 b = 0; b < set->numBundles; b++)
    {
        const SceneBundle* bundle = set->bundles[b];
        if (!bundle) continue;

        Range range = set->bundlePrimitiveRange[b];
        if (range.count == 0) continue;

        // every group of the bundle carries the same owning scene bundle index
        u32 sceneBundleIdx = set->primitiveGroups[range.start].bundleIdx;
        BundleCacheEntry bundleCacheCopy;
        if (!FindCacheForSceneBundle(scene, sceneBundleIdx, &bundleCacheCopy) || !bundleCacheCopy.bvhNodes) continue;
        const BundleCacheEntry* bundleCache = &bundleCacheCopy;
        for (u32 g = range.start; g < range.start + range.count; g++)
        {
            const PrimitiveGroup* group = &set->primitiveGroups[g];
            if (group->numEntities == 0) continue;

            const APrimitive* prim = &bundle->meshes[group->meshIndex].primitives[group->primitiveIndex];
            if (prim->bvhNodeIndex == ~0u) continue;

            for (u32 e = 0; e < group->numEntities; e++)
            {
                const Entity* entity = &set->entities[group->entityOffset + e];

                // transform the ray into entity local space instead of the mesh into
                // world space, the unnormalized direction keeps t in world units
                v128f rotation = VecNorm(UnpackQuaternionS16Norm1(entity->rotation));
                v128f invRot   = QConjugate(rotation);
                v128f scale    = VecMax(EntityUnpackWorldScale(entity->scale), VecSet1(1.0e-6f));
                VecSetW(scale, 1.0f);
                v128f localOrigin = VecDiv(QMulVec3V(VecSub(origin, entity->position), invRot), scale);
                v128f localDir    = VecDiv(QMulVec3V(dir, invRot), scale);

                f32 before = hit->hit.t;
                v128f decodeMin, decodeExtent;
                BVH_PrimitiveDecode(prim, &decodeMin, &decodeExtent);
                if (BVH_Intersect(bundleCache, skinned, prim->bvhNodeIndex, decodeMin, decodeExtent, localOrigin, localDir, hit) && hit->hit.t < before)
                {
                    hit->skinnedSet = skinned;
                    hit->groupIdx = g;
                    hit->entityIdx = e;
                    anyHit = 1;
                }
            }
        }
    }
    return anyHit;
}

s32 BVH_RaycastScene(const Scene* scene, v128f origin, v128f dir, BVHHit* hit)
{
    MemsetZero(hit, sizeof(*hit));
    hit->hit.t = BVH_MISS;

    s32 anyHit = 0;
    // static surface geometry has box3d triangle colliders (built at scene load), use the physics
    // broadphase for it. skinned meshes have no colliders, keep the cpu blas path for those.
    anyHit |= Scene_PhysicsRaycastPick(scene, origin, dir, hit);
    anyHit |= BVH_RaycastSet(scene, &scene->skinnedSet, true, origin, dir, hit);
    if (!anyHit) return 0;

    // resolve the render group back to the scene bundle for reporting. the group carries its
    // owning render bundle slot and the scene keeps a reverse map, so this is O(1).
    hit->bundleIdx = Scene_FindBundleForRenderGroup(scene, hit->skinnedSet != 0, hit->groupIdx);

    return 1;
}
