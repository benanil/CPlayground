#include "TransvoxelUnity.h"
#include "Include/Platform.h"

static void tLinearOctreeCreateRootNode(tOctree* tree)
{
    tOctreeNode rootNode;
    rootNode.position = tree->rootPosition;
    rootNode.extents  = tree->rootSize / 2;
    rootNode.depth    = tree->rootDepth;
    rootNode.locCode  = 1u;
	HMInsertOrAssign(&tree->nodeMap, rootNode.locCode, &rootNode);
}

void tOctreeInit(tOctree* tree, int3 rootPosition, s32 rootSize, s32 rootDepth, u32 reserveCount)
{
    if (rootSize <= 0 || rootDepth < 0 || rootDepth >= 30 || rootSize < (1 << rootDepth))
    {
        AX_WARN("transvoxel unity octree init failed: invalid root size/depth");
		rootDepth = 16;
		rootSize = 1 << rootDepth;
	}

    tree->nodeMap      = HMCreate(Maxu32(reserveCount, 1u), sizeof(tOctreeNode));
    tree->rootPosition = rootPosition;
    tree->rootSize     = rootSize;
    tree->rootDepth    = rootDepth;
    tree->leafSize     = rootSize >> rootDepth;
    tLinearOctreeCreateRootNode(tree);
}

void tOctreeDestroy(tOctree* tree)
{
    HMDestroy(&tree->nodeMap);
    *tree = (tOctree){0};
}

bool tOctreeRootNode(const tOctree* tree, tOctreeNode* outNode)
{
    tOctreeNode* root = (tOctreeNode*)HMFind(&tree->nodeMap, 1u);
    if (!root)
    {
        *outNode = (tOctreeNode){0};
        AX_WARN("transvoxel unity octree root lookup failed: missing root node");
        return false;
    }

    *outNode = *root;
    return true;
}

bool tOctreeNodeHasChildren(const tOctree* tree, tOctreeNode node)
{
    return HMContains(&tree->nodeMap, node.locCode << 3u);
}

bool tOctreeGetChild(const tOctree* tree, tOctreeNode node, u32 index, tOctreeNode* outNode)
{
    if (!tree || !outNode || index >= 8u)
    {
        AX_WARN("transvoxel unity octree child lookup failed: invalid argument");
        return false;
    }

    tOctreeNode* child = (tOctreeNode*)HMFind(&tree->nodeMap, (node.locCode << 3u) | index);
    if (!child)
        return false;

    *outNode = *child;
    return true;
}

bool tOctreeGetNodeAt(const tOctree* tree, int3 position, tOctreeNode* outNode)
{
    tOctreeNode currentNode;
    tOctreeRootNode(tree, &currentNode);

    while (tOctreeNodeHasChildren(tree, currentNode))
    {
        u32 index = 0;
        if (position.x > currentNode.position.x) index |= 1u;
        if (position.y > currentNode.position.y) index |= 2u;
        if (position.z > currentNode.position.z) index |= 4u;

        if (!tOctreeGetChild(tree, currentNode, index, &currentNode))
        {
            AX_WARN("transvoxel unity octree node lookup failed: incomplete child set");
            return false;
        }
    }

    *outNode = currentNode;
    return true;
}

bool tOctreeRemoveNode(tOctree* tree, u64 locCode)
{
    return HMErase(&tree->nodeMap, locCode);
}

bool tOctreeSplitNode(tOctree* tree, tOctreeNode node)
{
    static const s32 childSign[8][3] = {
        {-1, -1, -1}, { 1, -1, -1}, {-1,  1, -1}, { 1,  1, -1},
        {-1, -1,  1}, { 1, -1,  1}, {-1,  1,  1}, { 1,  1,  1}
    };

    if (tOctreeNodeHasChildren(tree, node))
    {
        AX_WARN("transvoxel unity octree split failed: node already has children");
        return false;
    }

    if (node.depth <= 0)
    {
        AX_WARN("transvoxel unity octree split failed: node is already a leaf");
        return false;
    }

    s32 childDepth   = node.depth - 1;
    s32 childExtents = node.extents >> 1;

    for (u32 i = 0; i < 8u; i++)
    {
        tOctreeNode child;
        child.depth   = childDepth;
        child.extents = childExtents;
        child.locCode = (node.locCode << 3u) | i;
        child.position = I3Add(node.position, (int3){
            childExtents * childSign[i][0],
            childExtents * childSign[i][1],
            childExtents * childSign[i][2]
        });

		HMInsert(&tree->nodeMap, child.locCode, &child);
    }

    return true;
}
