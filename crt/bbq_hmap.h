/*
 * bbq_hmap.h — Flat open-addressed map for DENSE 64-bit / pointer keys.
 *
 * WHEN TO USE THIS RATHER THAN bbq_htree:
 *
 *   bbq_htree is a nibble TRIE over a 32-bit key: it descends up to 8 levels, one
 *   dependent pointer load each. That is the right structure for a SPARSE key space you do
 *   not control — a packed (class_id, field_idx) cell key, say — where you want prefix
 *   sharing and no rehash.
 *
 *   It is the WRONG structure for "what index did I assign this object?", asked millions of
 *   times against keys you minted yourself (a pointer). Eight dependent loads cannot be
 *   prefetched — each address depends on the last — so every lookup is a chain of potential
 *   cache misses. And a 64-bit pointer does not fit the trie's 32-bit key, so callers hash
 *   it down and then keep their OWN collision chain on the side, comparing the real
 *   pointers: three structures to answer one question.
 *
 *   bbq_hmap is one contiguous array. A lookup is: mix the key, index the array, compare.
 *   Usually one cache line. Keys are full 64-bit, so there is no truncation and no
 *   collision chain to maintain — the map IS the answer.
 *
 * Open addressing, linear probing, power-of-two capacity, grow at 3/4 load. An empty slot
 * is flagged by a per-slot `used` byte, NOT by a reserved key value — so key 0 and value
 * NULL are both legitimate, and `contains` is distinct from "get returned NULL".
 *
 * No deletion: every caller so far builds a table once and reads it. Adding tombstones for
 * a use case that does not exist would be a bug in waiting.
 *
 *   bbq_hmap m;
 *   bbq_hmap_init(&m, 0);                       // 0 = default initial capacity
 *   bbq_hmap_put(&m, (uint64_t)(uintptr_t)p, v);
 *   void* v = bbq_hmap_get(&m, (uint64_t)(uintptr_t)p);
 *   bbq_hmap_free(&m);
 */
#ifndef BBQ_HMAP_H
#define BBQ_HMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t* keys;     /* [cap]                                   */
    void**    vals;     /* [cap]                                   */
    uint8_t*  used;     /* [cap] — 1 iff the slot holds an entry   */
    size_t    cap;      /* power of two, or 0 when empty           */
    size_t    len;      /* entries in use                          */
} bbq_hmap;

/* Initialize. `initial_cap` is rounded up to a power of two; 0 selects a default.
 * Returns false if the table could not be allocated, leaving the map empty-but-valid:
 * every other entry point works on it, and the first put retries the allocation. */
bool  bbq_hmap_init(bbq_hmap* m, size_t initial_cap);

/* Release the table. Values are NOT owned and are not touched. The map is left valid
 * and reusable — a subsequent put re-allocates from the default capacity. */
void  bbq_hmap_free(bbq_hmap* m);

/* Insert or REPLACE the value for `key`.
 *
 * Returns false, and stores NOTHING, if the table needed to grow and the allocation
 * failed. Rejecting the entry is what keeps the load factor under 3/4, which is what
 * bounds the probe walk — inserting anyway would march the table toward full, and a
 * full table makes hmap_slot's probe loop non-terminating. A false here is "this key is
 * not in the map", never a silently dropped entry that later reads as present. */
bool  bbq_hmap_put(bbq_hmap* m, uint64_t key, void* val);

/* The value for `key`, or NULL if absent. NULL is also a legitimate stored value —
 * use bbq_hmap_contains to tell the two apart. */
void* bbq_hmap_get(const bbq_hmap* m, uint64_t key);

/* Is `key` present, whatever its value? */
bool  bbq_hmap_contains(const bbq_hmap* m, uint64_t key);

/* Number of entries. */
size_t bbq_hmap_len(const bbq_hmap* m);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_HMAP_H */
