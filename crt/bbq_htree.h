/*
 * bbq_htree.h — 16-way radix tree (hex trie) over 64-bit keys.
 *
 *   bbq_htree t;
 *   bbq_htree_init(&t);                  // or _init_a(&t, alloc)
 *   bbq_htree_insert(&t, key, value);
 *   void* v = bbq_htree_search(&t, key);
 *   bbq_htree_free(&t);
 *
 * A trie, so there is no hash and no rehash: a key's position is the key. Lookup
 * is bounded by the depth, never by the number of entries, and iteration is in
 * ascending key order for free.
 *
 * ── LAZY EXPANSION ──────────────────────────────────────────────────────────
 *
 * A leaf sits at the SHALLOWEST depth that distinguishes its key from every
 * other key in the tree, not at the bottom. One key in an empty tree is a leaf
 * hanging directly off the root; a second key pushes both down only as far as
 * the first nibble where they differ. This is the first of the two ideas in ART
 * (Leis, Kemper & Neumann, ICDE 2013).
 *
 * It is the difference between a tree whose cost is the KEY WIDTH and one whose
 * cost is the number of keys. Without it, every key of a 64-bit key space costs
 * 16 interior nodes of 16 pointers each, whatever the tree holds — which is what
 * this did before, at a measured 681 bytes per key for sparse keys.
 *
 * ── FAILURE ─────────────────────────────────────────────────────────────────
 *
 * bbq_htree_insert returns false when it could not allocate, and the tree is
 * unchanged — no half-built path, no entry that search can find but iteration
 * cannot. The tree is also poisoned: bbq_htree_oom() reports it, so a caller
 * that ignores a return can still find out at a boundary. Nothing aborts.
 *
 * Not thread-safe; one writer or many readers, never both.
 */
#ifndef BBQ_HTREE_H
#define BBQ_HTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bbq_alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 64-bit keys, so a pointer is a key without narrowing it first. The previous
 * 32-bit key made every pointer-keyed caller write
 * `(uint32_t)(uintptr_t)ptr`, where two objects differing only above bit 31
 * silently became one entry. */
typedef uint64_t bbq_htree_key;

#define BBQ_HTREE_NIBBLES 16          /* 64 bits / 4 */

typedef struct bbq_htree_node bbq_htree_node;

typedef struct bbq_htree_leaf {
    bbq_htree_key key;
    void*         value;
} bbq_htree_leaf;

typedef struct bbq_htree {
    bbq_htree_node* root;      /* NULL when empty — an empty tree allocates nothing */
    size_t          count;
    bbq_alloc*      a;
    bool            oom;
    /* Bumped by every mutation. An iterator captures it and stops if it changes,
     * rather than walking nodes that have been freed underneath it. */
    uint32_t        gen;
} bbq_htree;

/* An iterator is a value: it allocates nothing, so abandoning one leaks nothing
 * and there is nothing to free. The stack is one slot per nibble plus the leaf. */
typedef struct bbq_htree_iter {
    const bbq_htree* t;
    uint32_t         gen;
    bbq_htree_node*  stack[BBQ_HTREE_NIBBLES + 1];
    uint8_t          idx  [BBQ_HTREE_NIBBLES + 1];
    int              depth;
} bbq_htree_iter;

/* --- Lifecycle --- */

/* Caller-owned. Prefer this: the tree can then live inside a struct or an arena
 * with no allocation of its own. */
void bbq_htree_init(bbq_htree* t);
void bbq_htree_init_a(bbq_htree* t, bbq_alloc* a);
void bbq_htree_free(bbq_htree* t);

/* The handle form, for callers that thread a `bbq_htree*` through signatures and
 * struct fields rather than owning the storage. It costs one allocation for the
 * handle — taken from the same allocator, so a budget covers the handle too —
 * and is otherwise identical; destroy accepts NULL. */
bbq_htree* bbq_htree_create(void);
bbq_htree* bbq_htree_create_a(bbq_alloc* a);
void       bbq_htree_destroy(bbq_htree* t);

/* Drop every entry, keep the tree usable. Allocates nothing, so it cannot fail —
 * which is the point: the previous version re-allocated a root node and ignored
 * the result, leaving a NULL root that every later call dereferenced. */
void bbq_htree_clear(bbq_htree* t);

/* Copy every entry into `dst` (which must be initialised). False, with `dst`
 * left holding whatever it managed, if an allocation was refused. */
bool bbq_htree_clone(bbq_htree* dst, const bbq_htree* src);

/* --- Core operations --- */

bool  bbq_htree_insert(bbq_htree* t, bbq_htree_key key, void* value);
void* bbq_htree_search(const bbq_htree* t, bbq_htree_key key);
void* bbq_htree_delete(bbq_htree* t, bbq_htree_key key);

/* True if the key is present, whatever its value. Distinct from search != NULL,
 * because NULL is a legitimate stored value — this is the entry point for
 * visited-set use, where the value is a sentinel nobody reads. */
bool bbq_htree_contains(const bbq_htree* t, bbq_htree_key key);

size_t bbq_htree_size(const bbq_htree* t);      /* O(1) */
bool   bbq_htree_is_empty(const bbq_htree* t);
bool   bbq_htree_oom(const bbq_htree* t);

/* --- Iteration, in ascending key order --- */

void            bbq_htree_iter_init(const bbq_htree* t, bbq_htree_iter* it);
bbq_htree_leaf* bbq_htree_next(bbq_htree_iter* it);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_HTREE_H */
