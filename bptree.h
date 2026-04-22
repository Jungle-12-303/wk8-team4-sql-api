#ifndef BPTREE_H
#define BPTREE_H

#define BPTREE_ORDER 4
#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)

typedef struct BPTreeNode {
    int is_leaf;
    int num_keys;
    int keys[BPTREE_MAX_KEYS];
    void *values[BPTREE_MAX_KEYS];
    struct BPTreeNode *children[BPTREE_ORDER];
    struct BPTreeNode *parent;
    struct BPTreeNode *next;
} BPTreeNode;

typedef struct BPTree {
    BPTreeNode *root;
} BPTree;

/* Creates an empty B+ tree. */
BPTree *bptree_create(void);

/* Frees all nodes in the tree without freeing record pointers. */
void bptree_destroy(BPTree *tree);

/* Inserts a key/value pair and returns 1 on success or 0 on duplicate key. */
int bptree_insert(BPTree *tree, int key, void *value);

/* Searches for a single key and returns the stored value pointer or NULL. */
void *bptree_search(BPTree *tree, int key);

#endif
