/*
 * Copyright (c) 2019 xieqing. https://github.com/xieqing
 * May be freely redistributed, but copyright notice must be retained.
 */

#ifndef _RB_HEADER
#define _RB_HEADER

#include <stdbool.h>

// rpmalloc has 16 byte alignment, this is perfect
typedef struct Node_
{
    // first bit used to determine red or black
	struct Node_*  parent; 
	struct Node_*  left;
	struct Node_*  right;
	void* value;
} Node;

typedef struct RBTree_
{
    Node* m_root;
} RBTree;


RBTree InitRBTree();

void DestroyRedBlackTree(RBTree* rbTree);

bool RBEmpty(RBTree* tree);

Node* RBInsert(RBTree* tree, void* value);

void RBErase(RBTree* tree, Node* node);

Node* RBFindNode(RBTree* tree, const void* key);

bool RBContains(RBTree* tree, const void* key);

void RBClear(RBTree* tree);

void RBTraverseNode(Node* n, void(*func)(void*));

void RBTraverse(RBTree* tree, void(*func)(void*));

Node* RBGetBeginNode(RBTree* tree);

Node* RBFindNextNode(RBTree* tree, Node* node);

int RBNumNodes(const Node* n);

void RBInsertFixup(RBTree* tree, Node* new_node);

void RBFixupAfterErase(RBTree* tree, Node* n);

void RBValidateNodeRec(RBTree* tree, Node* n);

void RBValidate(RBTree* tree);

void RBRotateLeft(RBTree* tree, Node* node);

void RBRotateRight(RBTree* tree, Node* node);

Node* RBAllocateNode(void* data, Node* parent);
	
void RBFreeNode(Node* n, bool recursive);


#endif /* _RB_HEADER */
