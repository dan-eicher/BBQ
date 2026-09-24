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
 *   bbq_dict d;
 *   bbq_dict_init(&d);                   // or _init_a(&d, alloc)
 *   bbq_dict_put(&d, "java.lang.String", 16, cls);
 *   void* v = bbq_dict_get(&d, name, namelen);
 *   bbq_dict_free(&d);
 */
#ifndef BBQ_DICT_H
#define BBQ_DICT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bbq_alloc.h"
#include "bbq_htree.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bbq_dict_node bbq_dict_node;

/* A hash is handed the dict's own seed with every key, so the seed lives on the container
 * and nowhere else (bbq_alloc.h, "Hash seed"). A hash that ignores it is allowed — the
 * weak-hash tests install one — at the cost of the flooding defence. */
typedef uint64_t (*bbq_dict_hash_fn)(uint64_t seed, const void* key, size_t len);

typedef struct bbq_dict {
    bbq_htree        index;   /* hash -> head of the chain sharing that hash */
    size_t           count;
    bbq_alloc*       a;
    bbq_dict_hash_fn hash;
    uint64_t         seed;    /* this dict's own hash seed, drawn at init */
    bool             oom;
    uint32_t         gen;     /* bumped by every mutation, for iterator safety */
} bbq_dict;

/* One entry, as handed to the iterator. `key` is the dict's own copy and stays
 * valid until the entry is deleted or the dict destroyed. */
typedef struct {
    const void* key;
    size_t      len;
    void*       value;
} bbq_dict_entry;

/* --- Lifecycle ---
 *
 * Caller-owned, like every other container here, so a dict can live inside a
 * struct without a second allocation for its handle. `a` may be NULL for libc. */
void bbq_dict_init(bbq_dict* d);
void bbq_dict_init_a(bbq_dict* d, bbq_alloc* a);
void bbq_dict_free(bbq_dict* d);
void bbq_dict_clear(bbq_dict* d);

/* The handle form, as for bbq_htree: one allocation for the handle from the same
 * allocator, otherwise identical. destroy accepts NULL. */
bbq_dict* bbq_dict_create(void);
bbq_dict* bbq_dict_create_a(bbq_alloc* a);
void      bbq_dict_destroy(bbq_dict* d);

/* A dict keyed by a hash of the caller's choosing. Two reasons this exists:
 *
 *  - a caller that already holds a digest of its names (a class-file constant
 *    pool, say) can key on it instead of hashing the bytes a second time;
 *  - the collision CHAIN — the thing that makes this correct rather than merely
 *    usually-right — cannot otherwise be reached. With a seeded 64-bit hash,
 *    finding two colliding keys is a birthday problem over 2^64, so the unlink
 *    paths would ship untested. A test installs a deliberately weak hash and
 *    exercises them.
 *
 * Whatever is installed, a lookup still compares the FULL key, so a bad hash
 * costs speed and never correctness. */
void bbq_dict_init_hashed(bbq_dict* d, bbq_alloc* a, bbq_dict_hash_fn hash);

/* As _init_a, with the hash seed given rather than drawn: a reproducible bucket layout,
 * for a test and only a test. */
void bbq_dict_init_seeded(bbq_dict* d, bbq_alloc* a, uint64_t seed);

/* Did any insertion get refused? The entries present are correct and complete
 * per key; the dict is just missing some of what was put into it. */
bool bbq_dict_oom(const bbq_dict* d);

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
 * A value type that allocates nothing: abandoning an iteration leaks nothing and
 * there is nothing to free. Mutating the dict mid-iteration ends the iteration —
 * bbq_dict_next then returns false rather than walking freed entries. */
typedef struct {
    const bbq_dict* d;
    bbq_htree_iter  tit;
    void*           cur;      /* next entry in the current hash's chain */
    uint32_t        gen;
} bbq_dict_iter;

void bbq_dict_iter_init(const bbq_dict* d, bbq_dict_iter* it);
bool bbq_dict_next(bbq_dict_iter* it, bbq_dict_entry* out);

/* The hash the dict keys its tree on: SipHash-1-3, keyed by the dict's own seed.
 *
 * Seeded because the keys are names from input this library does not control. An
 * unseeded hash — djb2, which this was — lets an attacker compute keys that all
 * collide, pile every one of them onto a single chain, and turn every lookup
 * into a linear scan. That is hash flooding (Klink & Wälde, 28C3 2011), and
 * SipHash (Aumasson & Bernstein, 2012) is the answer Python, Ruby, Rust and Perl
 * all adopted for it.
 *
 * Exposed so a caller holding the digest can skip recomputing it (with that dict's
 * `seed`). Two dicts give different values for the same key; nothing may persist one. */
uint64_t bbq_dict_hash(uint64_t seed, const void* key, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_DICT_H */
