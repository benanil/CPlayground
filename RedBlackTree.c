
#include "Common.h"
#include "RedBlackTree.h"
#include "Extern/rpmalloc.h"

static Node m_protect;

static inline bool IsRed(Node* node)  { return ((uint64_t)node->parent) & 0x1ull; }
static inline bool IsBlack(Node* node) { return !IsRed(node); }
static inline void SetRed(Node* node)   { node->parent = (Node*)((uint64_t)node->parent | 0x1ull); }  
static inline void SetBlack(Node* node) { node->parent = (Node*)((uint64_t)node->parent & ~0x1ull); }
static inline bool GetColor(Node* node) { return IsRed(node); }
static inline void SetColor(Node* node, bool isRed) { node->parent = (Node*)((uint64_t)(isRed) | ((uint64_t)node->parent & ~0x1ull)); }
static inline void SetParent(Node* node, Node* newParent) { node->parent = (Node*)((uint64_t)newParent | (((uint64_t)node->parent) & 0x1)); }
static inline Node* GetParent(Node* node) { return (Node*)(((uint64_t)node->parent) & ~0x1); }

RBTree InitRBTree()
{
    RBTree result;
    memset(&m_protect, 0, sizeof(Node));
    // m_protect.SetBlack(); // by default this is black
    m_protect.left = m_protect.right = &m_protect;
    m_protect.parent = &m_protect;
    result.m_root = &m_protect;
    return result;
}

void DestroyRedBlackTree(RBTree* rbTree)
{
    RBFreeNode(rbTree->m_root, true);
}

static Node* Nil() { return &m_protect; }

bool RBEmpty(RBTree* tree) { return tree->m_root == NULL || tree->m_root == &m_protect; }

Node* RBInsert(RBTree* tree, void* value)
{
    Node* node   = tree->m_root;
    Node* parent = &m_protect;
    
    while (node != &m_protect)
    {
        parent = node;
        if (node->value < value) node = node->right;
        else if (value < node->value) node = node->left;
        else return node;
    }

    Node* added = RBAllocateNode(value, parent);
    
    if (parent != &m_protect)
        if (value < parent->value)
            parent->left = added;
    else
        parent->right = added;
    else 
        tree->m_root = added;

    RBInsertFixup(tree, added);
    RBValidate(tree);
    return added;
}

void RBErase(RBTree* tree, Node* node)
{
    Node* replacement;

    if (node->left == &m_protect || node->right == &m_protect)
        replacement = node;
    else
    {
        // get successor
        replacement = node->right;
        while (replacement->left != &m_protect)
            replacement = replacement->left;
    }

    Node* eraseChild = replacement->left != &m_protect ? replacement->left : replacement->right;
    SetParent(eraseChild, GetParent(replacement));
		
    if (GetParent(replacement) != &m_protect)
    {
        if (replacement == GetParent(replacement)->left)
            GetParent(replacement)->left = eraseChild;
        else
            GetParent(replacement)->right = eraseChild;
    }
    else
        tree->m_root = eraseChild;

    node->value = replacement->value;

    if (IsBlack(replacement))
        RBFixupAfterErase(tree, eraseChild);

    RBFreeNode(replacement, 0);

    RBValidate(tree);
}

Node* RBFindNode(RBTree* tree, const void* key)
{
    Node* node = tree->m_root;
    while (node != &m_protect)
    {
        if (node->value < key)
            node = node->right;
        else if (key < node->value)
            node = node->left;
        else 
            return node;
    }
    return NULL;
}

bool RBContains(RBTree* tree, const void* key)
{
    return RBFindNode(tree, key) != NULL;
}

void RBClear(RBTree* tree)
{
    if (tree->m_root)
    {
        RBFreeNode(tree->m_root, 1);
        tree->m_root = &m_protect;
    }
}

void RBTraverseNode(Node* n, void(*func)(void*))
{
    if (n->left != &m_protect)
        RBTraverseNode(n->left, func);
    func(n->value);
    if (n->right != &m_protect)
        RBTraverseNode(n->right, func);
}

void RBTraverse(RBTree* tree, void(*func)(void*))
{
    if (tree->m_root && tree->m_root != &m_protect)
        RBTraverseNode(tree->m_root, func);
}

Node* RBGetBeginNode(RBTree* tree)
{
    Node* iter = &m_protect;
    if (tree->m_root != &m_protect)
    {
        iter = tree->m_root;
        while (iter->left != &m_protect)
            iter = iter->left;
    }
    return iter;
}

Node* RBFindNextNode(RBTree* tree, Node* node)
{
    if (node == &m_protect) return &m_protect;
    Node* next = &m_protect;
    
    if (node->right != &m_protect)
    {
        next = node->right;
        while (next->left != &m_protect)
            next = next->left;
    }
    else if (GetParent(node) != &m_protect)
    {
        if (node == GetParent(node)->left)
            return GetParent(node);
        else
        {
            next = node;
            while (GetParent(next) != &m_protect)
            {
                if (next == GetParent(next)->right)
                    next = GetParent(next);
                else
                    return GetParent(next);
            }
            next = NULL;
        }
    }
    else
    {
        assert(node == tree->m_root);
    }
    return next;
}

int RBNumNodes(const Node* n) 
{
    return n == &m_protect ? 0 : 1 + RBNumNodes(n->left) + RBNumNodes(n->right);
}

void RBInsertFixup(RBTree* tree, Node* new_node)
{
    Node* iter = new_node;

    while (IsRed(GetParent(iter)))
    {
        Node* grandparent = GetParent(GetParent(iter));
        if (GetParent(iter) == grandparent->left)
        {
            Node* uncle = grandparent->right;
            // Both parent and uncle are red.
            // Repaint both, make grandparent red.
            if (IsRed(uncle))
            {
                SetBlack(GetParent(iter));
                SetBlack(uncle);
                SetRed(grandparent);
                iter = grandparent;
            }
            else
            {
                if (iter == GetParent(iter)->right)
                {
                    iter = GetParent(iter);
                    RBRotateLeft(tree, iter);
                }
                grandparent = GetParent(GetParent(iter));
                SetBlack(GetParent(iter));
                SetRed(grandparent);
                RBRotateRight(tree, grandparent);
            }
        }
        else
        {
            Node* uncle = grandparent->left;
            if (IsRed(uncle))
            {
                SetRed(grandparent);
                SetBlack(GetParent(iter));
                SetBlack(uncle);
                iter = grandparent;
            }
            else
            {
                if (iter == GetParent(iter)->left)
                {
                    iter = GetParent(iter);
                    RBRotateRight(tree, iter);
                }
                grandparent = GetParent(GetParent(iter));
                SetBlack(GetParent(iter));
                SetRed(grandparent);
                RBRotateLeft(tree, grandparent);
            }
        }
    }
    SetBlack(tree->m_root);
}

void RBFixupAfterErase(RBTree* tree, Node* n)
{
    Node* iter = n;
    while (iter != tree->m_root && IsBlack(iter))
    {
        if (iter == GetParent(iter)->left)
        {
            Node* sibling = GetParent(iter)->right;
            if (IsRed(sibling))
            {
                SetBlack(sibling);
                Node* parent = GetParent(iter);
                SetRed(parent );
                RBRotateLeft(tree, parent);
                sibling = parent->right;
            }
            if (IsBlack(sibling->left) && IsBlack(sibling->right))
            {
                SetRed(sibling);
                iter = GetParent(iter);
            }
            else
            {
                if (IsBlack(sibling->right))
                {
                    SetBlack(sibling->left);
                    SetRed(sibling);
                    RBRotateRight(tree, sibling);
                    sibling = GetParent(iter)->right;
                }
                SetColor(sibling, GetColor(GetParent(iter)));
                SetBlack(GetParent(iter));
                SetBlack(sibling->right);
                RBRotateLeft(tree, GetParent(iter));
                iter = tree->m_root;
            }
        }
        else // iter == right child
        {
            Node* sibling = GetParent(iter)->left;
            if (IsRed(sibling))
            {
                SetBlack(sibling);
                SetRed(GetParent(iter));
                RBRotateRight(tree, GetParent(iter));
                sibling = GetParent(iter)->left;
            }

            if (IsBlack(sibling->left) && IsBlack(sibling->right))
            {
                SetRed(sibling);
                iter = GetParent(iter);
            }
            else
            {
                if (IsBlack(sibling->left))
                {
                    SetBlack(sibling->right);
                    SetRed(sibling);
                    RBRotateLeft(tree, sibling);
                    sibling = GetParent(iter)->left;
                }
                SetColor(sibling, GetColor(GetParent(iter)));
                SetBlack(GetParent(iter));
                SetBlack(sibling->left);
                RBRotateRight(tree, GetParent(iter));
                iter = tree->m_root;
            }
        }
    }
    SetBlack(iter);
}

void RBValidateNodeRec(RBTree* tree, Node* n)
{
    if (tree->m_root == NULL || tree->m_root == &m_protect) return;
    assert(GetParent(n) == &m_protect ||
           GetParent(n)->left == n || GetParent(n)->right == n);
		
    if (IsRed(n))
    {
        assert(IsBlack(n->left));
        assert(IsBlack(n->right));
    }
    if (n->left != &m_protect)
        RBValidateNodeRec(tree, n->left);
    if (n->right != &m_protect)
        RBValidateNodeRec(tree, n->right);
}

void RBValidate(RBTree* tree) 
{
    assert(IsBlack(tree->m_root));
    RBValidateNodeRec(tree, tree->m_root);
}

void RBRotateLeft(RBTree* tree, Node* node)
{
    Node* rightChild = node->right;
    node->right = rightChild->left;
    
    if (node->right != &m_protect)
        SetParent(node->right, node);

    SetParent(rightChild, GetParent(node));
    
    if (GetParent(node) == &m_protect)
        tree->m_root = rightChild;
    else
    {
        if (node == GetParent(node)->left)
            GetParent(node)->left = rightChild;
        else
            GetParent(node)->right = rightChild;
    }
    rightChild->left = node;
    SetParent(node, rightChild);
}

void RBRotateRight(RBTree* tree, Node* node)
{
    Node* leftChild = node->left;
    node->left = leftChild->right;

    if (node->left != &m_protect)
        SetParent(node->left, node);

    SetParent(leftChild, GetParent(node));
    if (GetParent(node) == &m_protect)
        tree->m_root = leftChild;
    else
        if (node == GetParent(node)->left)
            GetParent(node)->left = leftChild;
    else
        GetParent(node)->right = leftChild;
		
    leftChild->right = node;
    SetParent(node, leftChild);
}

Node* RBAllocateNode(void* data, Node* parent)
{
    Node* node   = rpmalloc(sizeof(Node));
    node->value  = data;
    node->parent = parent;
    node->right  = node->left = &m_protect;
    SetRed(node);
    return node;
}

void RBFreeNode(Node* n, bool recursive)
{
    if (recursive)
    {
        if (n->left != &m_protect)
            RBFreeNode(n->left, 1);
        if (n->right != &m_protect)
            RBFreeNode(n->right, 1);
    }
    if (n != &m_protect)
    {
        rpfree(n);
    }
}
