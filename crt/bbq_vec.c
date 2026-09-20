/*
 * bbq_vec.c — the growable array's logic. See bbq_vec.h for the contract.
 *
 * Everything that can fail lives here rather than in the macros, because a
 * statement macro has no value to return and therefore no way to tell its caller
 * that an allocation did not happen. That is exactly how the original wrote one
 * element past the end of every vector whose growth failed.
 */
#include "bbq_vec.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── The poison sentinel ──────────────────────────────────────────────────────
 *
 * When the FIRST allocation for a NULL vector fails there is no header to record
 * the failure in, so the vector becomes this object. Its layout mirrors a real
 * block, so bbq__vec_hdr() on the returned pointer lands on a genuine
 * bbq_vec_hdr — better defined than the heap path, which leans on the weaker
 * effective-type rule for allocated storage.
 *
 * There is exactly ONE of these per process because it lives in a .c file. A
 * `static` in the header would have given one per translation unit, and a vector
 * poisoned in one TU can be freed in another — so identifying it by address, as
 * bbq__vec_release does, would have been wrong there and is correct here.
 *
 * Nothing in this file ever writes it. Every header write below checks for it
 * first, including the ones whose write would be harmless (setting len to 0 over
 * a 0), so that the object is only ever read. That is what makes it thread-safe
 * without atomics: with no writer there is no data race, by definition.
 *
 * `slack` is a crumple zone, not storage: len is 0 so no correct caller indexes
 * it, and a stray v[0] from a caller that ignored the contract lands inside an
 * object we own rather than out of bounds.
 */
#define BBQ__VEC_SLACK 16

typedef struct {
    bbq_vec_hdr hdr;
    char        slack[BBQ__VEC_SLACK];
} bbq__vec_sentinel;

/* bbq__vec_hdr() on the data pointer must land exactly on `hdr`. `char` has
 * alignment 1 and the header's size is already a multiple of its own alignment,
 * so no padding can appear between them — but the whole mechanism rests on it. */
_Static_assert(offsetof(bbq__vec_sentinel, slack) == sizeof(bbq_vec_hdr),
               "poison sentinel: element data must sit exactly one header past hdr");

static bbq__vec_sentinel bbq__vec_poison_obj = { { 0, -1, (bbq_alloc*)0 }, { 0 } };

static void* poison_data(void) { return bbq__vec_poison_obj.slack; }

static int is_sentinel(const void* v) { return v == (const void*)bbq__vec_poison_obj.slack; }

/* The capacity a header really has, poisoned or not. */
static size_t real_cap(const bbq_vec_hdr* h) {
    return (size_t)(h->cap >= 0 ? h->cap : -1 - h->cap);
}

/* Mark a vector as permanently unable to grow, and hand back the pointer the
 * caller must store. A block keeps its elements and its length; a vector that
 * never had a block becomes the sentinel. */
static void* poison(void* v) {
    bbq_vec_hdr* h;
    if (!v) return poison_data();
    h = bbq__vec_hdr(v);
    if (h->cap >= 0) h->cap = -1 - h->cap;      /* sticky, and reversible to read */
    return v;
}

static size_t block_bytes(size_t n, size_t elem_size) {
    size_t body = bbq_mem_array_bytes(n, elem_size);
    if (!body) return 0;                                   /* n*elem overflowed */
    if (body > SIZE_MAX - sizeof(bbq_vec_hdr)) return 0;
    return sizeof(bbq_vec_hdr) + body;
}

/* ── Growth ───────────────────────────────────────────────────────────────── */

void* bbq__vec_resize(void* v, size_t want, size_t elem_size, bbq_alloc* born_with) {
    bbq_vec_hdr* oh = v ? bbq__vec_hdr(v) : NULL;
    bbq_vec_hdr* h;
    bbq_alloc*   a;
    size_t       bytes, old_bytes;
    int          len;

    if (oh && oh->cap < 0) return v;                  /* sticky: stays poisoned */
    if (oh && want <= (size_t)oh->cap) return v;
    if (want > (size_t)BBQ_VEC_MAX_CAP) return poison(v);

    bytes = block_bytes(want, elem_size);
    if (!bytes) return poison(v);

    len       = oh ? oh->len : 0;
    a         = oh ? oh->a : born_with;
    old_bytes = oh ? block_bytes(real_cap(oh), elem_size) : 0;

    h = (bbq_vec_hdr*)bbq_mem_resize(a, oh, old_bytes, bytes);
    if (!h) return poison(v);

    h->len = len;
    h->cap = (int)want;
    h->a   = a;
    return (char*)h + sizeof(bbq_vec_hdr);
}

/* `born_with` comes from the CALLER's BBQ_VEC_ALLOC(), expanded in the caller's
 * translation unit. Resolving it here instead would read this file's copy of the
 * macro — always the libc default — and every vector born through a push would
 * quietly ignore the embedder's choice, which is most of them. */
void* bbq__vec_grow(void* v, size_t elem_size, bbq_alloc* born_with) {
    size_t oldcap, want;

    if (v && bbq__vec_hdr(v)->cap < 0) return v;               /* already poisoned */
    oldcap = v ? (size_t)bbq__vec_hdr(v)->cap : 0;

    /* Doubling is done in size_t. `cap * 2` in int is signed overflow at 2^30 —
     * undefined, not a wrap — which is why the old code's negative capacity then
     * became an exabyte-sized request. */
    want = oldcap ? oldcap * 2 : 8;
    if (want > (size_t)BBQ_VEC_MAX_CAP) want = (size_t)BBQ_VEC_MAX_CAP;
    if (want <= oldcap) return poison(v);                      /* at the ceiling */

    return bbq__vec_resize(v, want, elem_size, born_with);
}

void bbq__vec_release(void* v, size_t elem_size) {
    bbq_vec_hdr* h;
    if (!v || is_sentinel(v)) return;             /* the sentinel is not ours to free */
    h = bbq__vec_hdr(v);
    bbq_mem_release(h->a, h, block_bytes(real_cap(h), elem_size));
}

/* ── Header writes ────────────────────────────────────────────────────────── */

void bbq__vec_setlen(void* v, int n) {
    bbq_vec_hdr* h;
    int cap;
    if (!v || is_sentinel(v)) return;
    h = bbq__vec_hdr(v);
    cap = (int)real_cap(h);
    if (n < 0) n = 0;
    h->len = n < cap ? n : cap;
}

void bbq__vec_truncate(void* v, int n) {
    bbq_vec_hdr* h;
    if (!v || is_sentinel(v)) return;
    h = bbq__vec_hdr(v);
    if (n < 0) n = 0;
    if (n < h->len) h->len = n;
}

int bbq__vec_popi(void* v) {
    bbq_vec_hdr* h;
    if (!v || is_sentinel(v)) return 0;
    h = bbq__vec_hdr(v);
    if (h->len <= 0) return 0;
    return --h->len;
}

int bbq__vec_topi(const void* v) {
    int n = v ? ((const bbq_vec_hdr*)(const void*)
                 ((const char*)v - sizeof(bbq_vec_hdr)))->len : 0;
    return n > 0 ? n - 1 : 0;
}
