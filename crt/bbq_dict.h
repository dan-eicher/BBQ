/*
 * bbq_dict.h — string-keyed dictionary over bbq_htree.
 *
 * WHY THIS EXISTS. bbq_htree and bbq_hmap key on integers, so every caller that
 * wants to look something up by NAME has had to hash the bytes itself and then
 * decide what to do about collisions. Three answers to that appeared in one
 * codebase:
 *
 *   const_pool_intern   hash -> memcmp the stored run -> on a mismatch append
 *                       unindexed. Correct: "never hand one client another's
 *                       contents."
 *   the scope sidecar   hash -> chain -> full key compare on every candidate.
 *                       Correct: "a merged chain costs a wasted hop, never a
 *                       wrong row."
 *   sema's scope and    hash -> trust the hit.
 *   class tables        NOT correct. djb2-33 collides structurally on short
 *                       strings (any pair with d1*33 + d2 == 0, e.g. "ar" and
 *                       "c0" both hash to 0x597738), so two distinct locals
 *                       were one entry: `int ar; int c0;` was rejected as a
 *                       redeclaration, and a lookup could answer with the wrong
 *                       variable or the wrong class id.
 *
 * The bug is not the hash. Any 32-bit digest of arbitrary-length keys collides;
 * the bug is treating a digest as an identity. So this container stores the key
 * and compares it on every hit, and there is now one place to get that right
 * instead of a convention each caller has to remember.
 *
 * WHAT IT IS. A thin layer: bbq_htree maps hash -> first entry, entries sharing
 * a hash chain, and every lookup memcmps the full key. The tree keeps the parts
 * that are actually hard (16-way radix, O(1), no rebalancing or resize). A
 * collision costs one extra compare and can never return another key's value.
 *
 * KEYS ARE BYTES + LENGTH, not NUL-terminated strings: callers hold wasm_name_t
 * spans, pooled UTF-16LE runs (a 0x00 every other byte) and parser spans into a
 * source buffer, none of which are C strings. The dict COPIES the key on insert
 * and owns that copy, so a caller may key on a span whose buffer it is about to
 * free or move.
 *
 * NOT thread-safe, in keeping with the rest of the CRT.
 *
 *   bbq_dict* d = bbq_dict_create();
 *   bbq_dict_put(d, "java.lang.String", 16, cls);
 *   void* v = bbq_dict_get(d, name, namelen);
 *   bbq_dict_destroy(d);
 */
#ifndef BBQ_DICT_H
#define BBQ_DICT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bbq_dict bbq_dict;

/* One entry, as handed to the iterator. `key` is the dict's own copy and stays
 * valid until the entry is deleted or the dict destroyed. */
typedef struct {
    const void* key;
    size_t      len;
    void*       value;
} bbq_dict_entry;

/* --- Lifecycle --- */

bbq_dict* bbq_dict_create(void);
void      bbq_dict_destroy(bbq_dict* d);
void      bbq_dict_clear(bbq_dict* d);

/* --- Core operations ---
 *
 * put: insert or overwrite. Copies `len` bytes of `key`. Returns false only on
 *      allocation failure; the dict is unchanged in that case.
 * get: the value for an EXACT key match, else NULL. A key whose hash collides
 *      with a stored one is a miss, not a hit.
 * A zero-length key is legal and distinct from an absent one — use
 * bbq_dict_contains to tell a stored NULL value from a miss. */
bool  bbq_dict_put(bbq_dict* d, const void* key, size_t len, void* value);
void* bbq_dict_get(const bbq_dict* d, const void* key, size_t len);
bool  bbq_dict_contains(const bbq_dict* d, const void* key, size_t len);

/* Removes the entry and answers its value (NULL if absent). */
void* bbq_dict_delete(bbq_dict* d, const void* key, size_t len);

/* Convenience for the NUL-terminated callers (strlen at the boundary). */
bool  bbq_dict_puts(bbq_dict* d, const char* key, void* value);
void* bbq_dict_gets(const bbq_dict* d, const char* key);

size_t bbq_dict_len(const bbq_dict* d);

/* --- Iteration (unspecified order) ---
 *
 * Iterating a dict that is being modified is undefined. */
typedef struct { const bbq_dict* d; size_t slot; void* cur; } bbq_dict_iter;

void bbq_dict_iter_init(const bbq_dict* d, bbq_dict_iter* it);
bool bbq_dict_next(bbq_dict_iter* it, bbq_dict_entry* out);

/* The hash the dict keys its tree on — djb2-33, never 0 (bbq_htree reserves
 * that key). Exposed because a caller that already has the digest can avoid
 * recomputing it, and because a test wants to construct collisions on purpose. */
uint32_t bbq_dict_hash(const void* key, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_DICT_H */
