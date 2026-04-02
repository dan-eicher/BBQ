/*
 * bbq_htree.h — 16-way radix tree (hex trie) for 32-bit keys.
 *
 * Adapted from hextree by Dan Eicher. Provides O(1) lookup/insert/delete
 * for 32-bit keys with no hash collisions. Used by BBQ generated code for
 * state caches, visited sets, and symbol tables.
 *
 * Usage:
 *   bbq_htree* t = bbq_htree_create();
 *   bbq_htree_insert(t, key, value);
 *   void* v = bbq_htree_search(t, key);
 *   bbq_htree_destroy(t);
 */
#ifndef BBQ_HTREE_H
#define BBQ_HTREE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bbq_htree bbq_htree;

typedef struct bbq_htree_leaf {
    uint32_t key;
    void*    value;
} bbq_htree_leaf;

typedef struct bbq_htree_iter {
    void*   stack[9];     /* internal traversal stack (bbq_htree_node*) */
    uint8_t indices[9];
    int     depth;
} bbq_htree_iter;

/* --- Lifecycle --- */

bbq_htree* bbq_htree_create(void);
void       bbq_htree_destroy(bbq_htree* t);
bbq_htree* bbq_htree_clone(const bbq_htree* src);

/* --- Core operations --- */

bool  bbq_htree_insert(bbq_htree* t, uint32_t key, void* value);
void* bbq_htree_search(const bbq_htree* t, uint32_t key);
void* bbq_htree_delete(bbq_htree* t, uint32_t key);
void  bbq_htree_clear(bbq_htree* t);

/* --- Convenience --- */

/* Returns true if key is present (equivalent to search != NULL for non-NULL values).
 * For visited-set usage where values are sentinels, this is the preferred API. */
bool bbq_htree_contains(const bbq_htree* t, uint32_t key);

int  bbq_htree_size(const bbq_htree* t);
bool bbq_htree_is_empty(const bbq_htree* t);

/* --- Iteration (ascending key order) --- */

bbq_htree_iter* bbq_htree_iter_create(const bbq_htree* t);
void            bbq_htree_iter_free(bbq_htree_iter* it);
void            bbq_htree_iter_init(const bbq_htree* t, bbq_htree_iter* it);
bbq_htree_leaf* bbq_htree_next(bbq_htree_iter* it);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_HTREE_H */
