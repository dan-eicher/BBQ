/*
 * bbq_htree.c — 16-way radix tree implementation.
 * Adapted from hextree by Dan Eicher.
 */
#include "bbq_htree.h"
#include <stdlib.h>
#include <string.h>

/* ── Node types ─────────────────────────────────────────── */

typedef struct bbq_htree_node {
    union {
        struct bbq_htree_node* children[16];
        bbq_htree_leaf*        leaf;
    };
    bool is_leaf;
} bbq_htree_node;

struct bbq_htree {
    bbq_htree_node* root;
};

/* bbq_htree_iter is defined in the header with void* stack entries.
 * Internally we cast to/from bbq_htree_node*. */

/* ── Helpers ────────────────────────────────────────────── */

static inline uint8_t get_nibble(uint32_t key, int level) {
    return (key >> (28 - (level * 4))) & 0xF;
}

static void free_node(bbq_htree_node* node) {
    if (!node) return;
    if (node->is_leaf) {
        free(node->leaf);
    } else {
        for (int i = 0; i < 16; i++)
            if (node->children[i]) free_node(node->children[i]);
    }
    free(node);
}

/* ── Lifecycle ──────────────────────────────────────────── */

bbq_htree* bbq_htree_create(void) {
    bbq_htree* t = (bbq_htree*)malloc(sizeof(bbq_htree));
    if (!t) return NULL;
    t->root = (bbq_htree_node*)calloc(1, sizeof(bbq_htree_node));
    if (!t->root) { free(t); return NULL; }
    return t;
}

void bbq_htree_destroy(bbq_htree* t) {
    if (!t) return;
    if (t->root) free_node(t->root);
    free(t);
}

/* ── Insertion ──────────────────────────────────────────── */

static bool split_and_insert(bbq_htree_node* node, bbq_htree_leaf* existing,
                             uint32_t new_key, void* value, int split_level) {
    uint32_t old_key   = existing->key;
    void*    old_value = existing->value;

    int divergence = split_level;
    while (divergence < 8 &&
           get_nibble(old_key, divergence) == get_nibble(new_key, divergence))
        divergence++;

    memset(node, 0, sizeof(bbq_htree_node));
    node->is_leaf = false;

    /* Build common path up to divergence point */
    bbq_htree_node* cur = node;
    for (int level = split_level; level < divergence; level++) {
        uint8_t nib = get_nibble(old_key, level);
        cur->children[nib] = (bbq_htree_node*)calloc(1, sizeof(bbq_htree_node));
        if (!cur->children[nib]) return false;
        cur = cur->children[nib];
        cur->is_leaf = false;
    }

    /* Insert old key from divergence onward */
    uint8_t old_nib = get_nibble(old_key, divergence);
    cur->children[old_nib] = (bbq_htree_node*)calloc(1, sizeof(bbq_htree_node));
    if (!cur->children[old_nib]) return false;
    bbq_htree_node* old_node = cur->children[old_nib];
    for (int level = divergence + 1; level < 8; level++) {
        uint8_t nib = get_nibble(old_key, level);
        old_node->children[nib] = (bbq_htree_node*)calloc(1, sizeof(bbq_htree_node));
        if (!old_node->children[nib]) return false;
        if (level == 7) {
            old_node->children[nib]->is_leaf = true;
            old_node->children[nib]->leaf = (bbq_htree_leaf*)malloc(sizeof(bbq_htree_leaf));
            if (!old_node->children[nib]->leaf) return false;
            old_node->children[nib]->leaf->key   = old_key;
            old_node->children[nib]->leaf->value  = old_value;
        } else {
            old_node = old_node->children[nib];
            old_node->is_leaf = false;
        }
    }

    /* Insert new key from divergence onward */
    uint8_t new_nib = get_nibble(new_key, divergence);
    cur->children[new_nib] = (bbq_htree_node*)calloc(1, sizeof(bbq_htree_node));
    if (!cur->children[new_nib]) return false;
    bbq_htree_node* new_node = cur->children[new_nib];
    for (int level = divergence + 1; level < 8; level++) {
        uint8_t nib = get_nibble(new_key, level);
        new_node->children[nib] = (bbq_htree_node*)calloc(1, sizeof(bbq_htree_node));
        if (!new_node->children[nib]) return false;
        if (level == 7) {
            new_node->children[nib]->is_leaf = true;
            new_node->children[nib]->leaf = (bbq_htree_leaf*)malloc(sizeof(bbq_htree_leaf));
            if (!new_node->children[nib]->leaf) return false;
            new_node->children[nib]->leaf->key   = new_key;
            new_node->children[nib]->leaf->value  = value;
        } else {
            new_node = new_node->children[nib];
            new_node->is_leaf = false;
        }
    }

    return true;
}

bool bbq_htree_insert(bbq_htree* t, uint32_t key, void* value) {
    bbq_htree_node* cur = t->root;

    for (int level = 0; level < 8; level++) {
        uint8_t nib = get_nibble(key, level);

        if (cur->is_leaf) {
            if (cur->leaf->key == key) {
                cur->leaf->value = value;
                return true;
            }
            return split_and_insert(cur, cur->leaf, key, value, level);
        }

        if (!cur->children[nib]) {
            cur->children[nib] = (bbq_htree_node*)calloc(1, sizeof(bbq_htree_node));
            if (!cur->children[nib]) return false;

            if (level == 7) {
                cur->children[nib]->is_leaf = true;
                cur->children[nib]->leaf = (bbq_htree_leaf*)malloc(sizeof(bbq_htree_leaf));
                if (!cur->children[nib]->leaf) {
                    free(cur->children[nib]);
                    cur->children[nib] = NULL;
                    return false;
                }
                cur->children[nib]->leaf->key   = key;
                cur->children[nib]->leaf->value  = value;
                return true;
            }
            cur->children[nib]->is_leaf = false;
        }

        cur = cur->children[nib];

        if (cur->is_leaf) {
            if (cur->leaf->key == key) {
                cur->leaf->value = value;
                return true;
            }
            return split_and_insert(cur, cur->leaf, key, value, level);
        }
    }

    return false;
}

/* ── Deletion ───────────────────────────────────────────── */

void* bbq_htree_delete(bbq_htree* t, uint32_t key) {
    bbq_htree_node* path[8];
    uint8_t         nib_path[8];
    int             depth = 0;
    bbq_htree_node* cur = t->root;
    bbq_htree_node* parent = NULL;
    uint8_t         nib = 0;
    void*           deleted = NULL;

    for (int level = 0; level < 8; level++) {
        nib = get_nibble(key, level);

        if (cur->is_leaf) {
            if (cur->leaf->key == key)
                deleted = cur->leaf->value;
            else
                return NULL;
            break;
        }

        if (!cur->children[nib]) return NULL;

        path[depth]     = cur;
        nib_path[depth] = nib;
        depth++;

        parent = cur;
        cur    = cur->children[nib];
    }

    if (!deleted) {
        if (cur->is_leaf && cur->leaf->key == key)
            deleted = cur->leaf->value;
        else
            return NULL;
    }

    free(cur->leaf);
    free(cur);

    if (depth > 0) {
        parent = path[depth - 1];
        parent->children[nib_path[depth - 1]] = NULL;
    } else {
        memset(t->root, 0, sizeof(bbq_htree_node));
        return deleted;
    }

    /* Collapse empty interior nodes upward */
    for (int i = depth - 1; i >= 0; i--) {
        bbq_htree_node* node = path[i];
        bool has_children = false;
        for (int j = 0; j < 16; j++) {
            if (node->children[j]) { has_children = true; break; }
        }
        if (!has_children && i > 0) {
            free(node);
            path[i - 1]->children[nib_path[i - 1]] = NULL;
        } else {
            break;
        }
    }

    return deleted;
}

void bbq_htree_clear(bbq_htree* t) {
    if (!t || !t->root) return;
    free_node(t->root);
    t->root = (bbq_htree_node*)calloc(1, sizeof(bbq_htree_node));
}

/* ── Search ─────────────────────────────────────────────── */

void* bbq_htree_search(const bbq_htree* t, uint32_t key) {
    bbq_htree_node* cur = t->root;

    for (int level = 0; level < 8; level++) {
        uint8_t nib = get_nibble(key, level);

        if (cur->is_leaf)
            return (cur->leaf->key == key) ? cur->leaf->value : NULL;

        if (!cur->children[nib]) return NULL;
        cur = cur->children[nib];
    }

    if (cur->is_leaf && cur->leaf->key == key)
        return cur->leaf->value;

    return NULL;
}

bool bbq_htree_contains(const bbq_htree* t, uint32_t key) {
    bbq_htree_node* cur = t->root;

    for (int level = 0; level < 8; level++) {
        uint8_t nib = get_nibble(key, level);

        if (cur->is_leaf)
            return cur->leaf->key == key;

        if (!cur->children[nib]) return false;
        cur = cur->children[nib];
    }

    return cur->is_leaf && cur->leaf->key == key;
}

/* ── Utility ────────────────────────────────────────────── */

int bbq_htree_size(const bbq_htree* t) {
    int count = 0;
    bbq_htree_iter it;
    bbq_htree_iter_init(t, &it);
    while (bbq_htree_next(&it)) count++;
    return count;
}

bool bbq_htree_is_empty(const bbq_htree* t) {
    bbq_htree_iter it;
    bbq_htree_iter_init(t, &it);
    return bbq_htree_next(&it) == NULL;
}

bbq_htree* bbq_htree_clone(const bbq_htree* src) {
    bbq_htree* dst = bbq_htree_create();
    if (!dst) return NULL;

    bbq_htree_iter  it;
    bbq_htree_leaf* leaf;
    bbq_htree_iter_init(src, &it);

    while ((leaf = bbq_htree_next(&it))) {
        if (!bbq_htree_insert(dst, leaf->key, leaf->value)) {
            bbq_htree_destroy(dst);
            return NULL;
        }
    }
    return dst;
}

/* ── Iterator ───────────────────────────────────────────── */

bbq_htree_iter* bbq_htree_iter_create(const bbq_htree* t) {
    bbq_htree_iter* it = (bbq_htree_iter*)malloc(sizeof(bbq_htree_iter));
    if (it) bbq_htree_iter_init(t, it);
    return it;
}

void bbq_htree_iter_free(bbq_htree_iter* it) {
    free(it);
}

void bbq_htree_iter_init(const bbq_htree* t, bbq_htree_iter* it) {
    it->stack[0]   = (void*)t->root;
    it->indices[0] = 0;
    it->depth      = 0;
}

bbq_htree_leaf* bbq_htree_next(bbq_htree_iter* it) {
    if (it->depth < 0) return NULL;

    while (it->depth >= 0) {
        bbq_htree_node* cur = (bbq_htree_node*)it->stack[it->depth];
        int idx = it->indices[it->depth];

        if (cur->is_leaf) {
            bbq_htree_leaf* result = cur->leaf;
            it->depth--;
            return result;
        }

        while (idx < 16 && !cur->children[idx])
            idx++;

        it->indices[it->depth] = (uint8_t)(idx + 1);

        if (idx < 16) {
            it->depth++;
            it->stack[it->depth]   = (void*)cur->children[idx];
            it->indices[it->depth] = 0;
        } else {
            it->depth--;
        }
    }

    return NULL;
}
