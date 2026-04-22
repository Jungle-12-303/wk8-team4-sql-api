#include "bptree.h"

#include <stdlib.h>

/* Creates a single B+ tree node. */
static BPTreeNode *bptree_create_node(int is_leaf) {
    BPTreeNode *node = (BPTreeNode *)calloc(1, sizeof(BPTreeNode));

    if (node == NULL) {
        return NULL;
    }

    node->is_leaf = is_leaf;
    return node;
}

/* Returns the array position where a key should be inserted. */
static int bptree_find_insert_index(const int *keys, int num_keys, int key) {
    int index = 0;

    while (index < num_keys && keys[index] < key) {
        index++;
    }

    return index;
}

/* Walks from the root to the target leaf for a key. */
static BPTreeNode *bptree_find_leaf(BPTree *tree, int key) {
    BPTreeNode *current;
    int index;

    if (tree == NULL || tree->root == NULL) {
        return NULL;
    }

    current = tree->root;

    while (!current->is_leaf) {
        index = 0;

        while (index < current->num_keys && key >= current->keys[index]) {
            index++;
        }

        current = current->children[index];
    }

    return current;
}

/* Frees nodes recursively from the current subtree root. */
static void bptree_destroy_node(BPTreeNode *node) {
    int index;

    if (node == NULL) {
        return;
    }

    if (!node->is_leaf) {
        for (index = 0; index <= node->num_keys; index++) {
            bptree_destroy_node(node->children[index]);
        }
    }

    free(node);
}

/* Inserts a key/value pair into a non-full leaf node. */
static void bptree_insert_into_leaf(BPTreeNode *leaf, int key, void *value) {
    int index;
    int position = bptree_find_insert_index(leaf->keys, leaf->num_keys, key);

    for (index = leaf->num_keys; index > position; index--) {
        leaf->keys[index] = leaf->keys[index - 1];
        leaf->values[index] = leaf->values[index - 1];
    }

    leaf->keys[position] = key;
    leaf->values[position] = value;
    leaf->num_keys++;
}

/* Builds a new root after the old root split. */
static int bptree_create_new_root(BPTree *tree, BPTreeNode *left, int key, BPTreeNode *right) {
    BPTreeNode *root = bptree_create_node(0);

    if (root == NULL) {
        return 0;
    }

    root->keys[0] = key;
    root->children[0] = left;
    root->children[1] = right;
    root->num_keys = 1;

    left->parent = root;
    right->parent = root;
    tree->root = root;

    return 1;
}

/* Inserts a promoted key and right child into a non-full internal node. */
static void bptree_insert_into_internal(BPTreeNode *parent, int insert_index, int key, BPTreeNode *right_child) {
    int index;

    for (index = parent->num_keys; index > insert_index; index--) {
        parent->keys[index] = parent->keys[index - 1];
    }

    for (index = parent->num_keys + 1; index > insert_index + 1; index--) {
        parent->children[index] = parent->children[index - 1];
    }

    parent->keys[insert_index] = key;
    parent->children[insert_index + 1] = right_child;
    right_child->parent = parent;
    parent->num_keys++;
}

/* Splits an internal node after inserting one extra key and child. */
static int bptree_split_internal_and_insert(BPTree *tree, BPTreeNode *parent, int insert_index, int key, BPTreeNode *right_child);

/* Pushes a promoted key upward after a split. */
static int bptree_insert_into_parent(BPTree *tree, BPTreeNode *left, int key, BPTreeNode *right) {
    BPTreeNode *parent = left->parent;
    int insert_index = 0;

    if (parent == NULL) {
        return bptree_create_new_root(tree, left, key, right);
    }

    while (insert_index <= parent->num_keys && parent->children[insert_index] != left) {
        insert_index++;
    }

    if (parent->num_keys < BPTREE_MAX_KEYS) {
        bptree_insert_into_internal(parent, insert_index, key, right);
        return 1;
    }

    return bptree_split_internal_and_insert(tree, parent, insert_index, key, right);
}

/* Splits a full leaf and returns the new right leaf plus promoted key. */
static int bptree_split_leaf_and_insert(BPTree *tree, BPTreeNode *leaf, int key, void *value) {
    int temp_keys[BPTREE_ORDER];
    void *temp_values[BPTREE_ORDER];
    int insert_index = bptree_find_insert_index(leaf->keys, leaf->num_keys, key);
    int left_count = BPTREE_ORDER / 2;
    int index;
    int temp_index = 0;
    BPTreeNode *right_leaf = bptree_create_node(1);

    if (right_leaf == NULL) {
        return 0;
    }

    /* Merge the existing keys with the new key in sorted order. */
    for (index = 0; index < leaf->num_keys; index++) {
        if (temp_index == insert_index) {
            temp_keys[temp_index] = key;
            temp_values[temp_index] = value;
            temp_index++;
        }

        temp_keys[temp_index] = leaf->keys[index];
        temp_values[temp_index] = leaf->values[index];
        temp_index++;
    }

    if (temp_index == insert_index) {
        temp_keys[temp_index] = key;
        temp_values[temp_index] = value;
    }

    /* Keep the left half in the original leaf. */
    leaf->num_keys = 0;
    for (index = 0; index < left_count; index++) {
        leaf->keys[index] = temp_keys[index];
        leaf->values[index] = temp_values[index];
        leaf->num_keys++;
    }

    /* Move the right half into the new sibling leaf. */
    right_leaf->parent = leaf->parent;
    for (index = left_count; index < BPTREE_ORDER; index++) {
        right_leaf->keys[right_leaf->num_keys] = temp_keys[index];
        right_leaf->values[right_leaf->num_keys] = temp_values[index];
        right_leaf->num_keys++;
    }

    /* Preserve leaf-level linked-list traversal. */
    right_leaf->next = leaf->next;
    leaf->next = right_leaf;

    return bptree_insert_into_parent(tree, leaf, right_leaf->keys[0], right_leaf);
}

/* Splits a full internal node and promotes the middle separator key. */
static int bptree_split_internal_and_insert(BPTree *tree, BPTreeNode *parent, int insert_index, int key, BPTreeNode *right_child) {
    int temp_keys[BPTREE_ORDER];
    BPTreeNode *temp_children[BPTREE_ORDER + 1];
    int promote_index = BPTREE_ORDER / 2;
    int promote_key;
    int index;
    BPTreeNode *right_parent = bptree_create_node(0);

    if (right_parent == NULL) {
        return 0;
    }

    /* Build temporary arrays that include the new key and child. */
    for (index = 0; index < parent->num_keys; index++) {
        temp_keys[index] = parent->keys[index];
    }

    for (index = 0; index <= parent->num_keys; index++) {
        temp_children[index] = parent->children[index];
    }

    for (index = parent->num_keys; index > insert_index; index--) {
        temp_keys[index] = temp_keys[index - 1];
    }

    temp_keys[insert_index] = key;

    for (index = parent->num_keys + 1; index > insert_index + 1; index--) {
        temp_children[index] = temp_children[index - 1];
    }

    temp_children[insert_index + 1] = right_child;

    /* Keep the left half in the original parent node. */
    parent->num_keys = 0;
    for (index = 0; index < promote_index; index++) {
        parent->keys[index] = temp_keys[index];
        parent->children[index] = temp_children[index];
        if (parent->children[index] != NULL) {
            parent->children[index]->parent = parent;
        }
        parent->num_keys++;
    }

    parent->children[promote_index] = temp_children[promote_index];
    if (parent->children[promote_index] != NULL) {
        parent->children[promote_index]->parent = parent;
    }

    promote_key = temp_keys[promote_index];

    /* Put the right half into a new internal sibling node. */
    right_parent->parent = parent->parent;
    for (index = promote_index + 1; index < BPTREE_ORDER; index++) {
        right_parent->keys[right_parent->num_keys] = temp_keys[index];
        right_parent->children[right_parent->num_keys] = temp_children[index];
        if (right_parent->children[right_parent->num_keys] != NULL) {
            right_parent->children[right_parent->num_keys]->parent = right_parent;
        }
        right_parent->num_keys++;
    }

    right_parent->children[right_parent->num_keys] = temp_children[BPTREE_ORDER];
    if (right_parent->children[right_parent->num_keys] != NULL) {
        right_parent->children[right_parent->num_keys]->parent = right_parent;
    }

    return bptree_insert_into_parent(tree, parent, promote_key, right_parent);
}

/* Creates an empty B+ tree. */
BPTree *bptree_create(void) {
    return (BPTree *)calloc(1, sizeof(BPTree));
}

/* Frees all nodes in the tree without freeing record pointers. */
void bptree_destroy(BPTree *tree) {
    if (tree == NULL) {
        return;
    }

    bptree_destroy_node(tree->root);
    free(tree);
}

/* Inserts a key/value pair and returns 1 on success or 0 on duplicate key. */
int bptree_insert(BPTree *tree, int key, void *value) {
    BPTreeNode *leaf;
    int index;

    if (tree == NULL) {
        return 0;
    }

    if (tree->root == NULL) {
        tree->root = bptree_create_node(1);
        if (tree->root == NULL) {
            return 0;
        }

        tree->root->keys[0] = key;
        tree->root->values[0] = value;
        tree->root->num_keys = 1;
        return 1;
    }

    leaf = bptree_find_leaf(tree, key);

    for (index = 0; index < leaf->num_keys; index++) {
        if (leaf->keys[index] == key) {
            return 0;
        }
    }

    if (leaf->num_keys < BPTREE_MAX_KEYS) {
        bptree_insert_into_leaf(leaf, key, value);
        return 1;
    }

    return bptree_split_leaf_and_insert(tree, leaf, key, value);
}

/* Searches for a single key and returns the stored value pointer or NULL. */
void *bptree_search(BPTree *tree, int key) {
    BPTreeNode *leaf = bptree_find_leaf(tree, key);
    int index;

    if (leaf == NULL) {
        return NULL;
    }

    for (index = 0; index < leaf->num_keys; index++) {
        if (leaf->keys[index] == key) {
            return leaf->values[index];
        }
    }

    return NULL;
}
