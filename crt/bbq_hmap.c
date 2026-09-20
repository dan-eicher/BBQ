#include "bbq_hmap.h"

#include <stdlib.h>
#include <string.h>

#define BBQ_HMAP_DEFAULT_CAP 16

/* The 64-bit finalizer from MurmurHash3 (splitmix-style). Real keys here are POINTERS:
 * 8- or 16-byte aligned and clustered within an arena page, so their low bits are nearly
 * constant and their high bits nearly constant too. The whole point of the mix is to make
 * the *middle* bits — the ones that actually differ — reach the index. Masking a raw
 * pointer instead would put every node from one page into a handful of slots.
 *
 * SEEDED, because the keys are not always pointers. burgc's e-graph hashes e-node
 * CONTENT — op, data and canonical children — and that content comes from the
 * program being compiled. An unseeded mix over attacker-chosen content lets every
 * key be steered onto one probe run, turning the map into a linear scan (Klink &
 * Wälde, 28C3 2011). The seed is drawn from the OS once per process. */
static uint64_t hmap_mix(uint64_t x) {
    x ^= bbq_hash_seed();
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* 0 when there is no power of two at or above n. `c <<= 1` reaches 0 and spins
 * forever for anything above 2^63, which is a hang rather than a refusal. */
static size_t round_pow2(size_t n) {
    size_t c = 1;
    while (c < n) {
        if (c > SIZE_MAX / 2) return 0;
        c <<= 1;
    }
    return c;
}

/* Allocate the three parallel arrays for `cap` slots, all-or-nothing. On partial failure
 * the survivors are freed rather than leaked, and the map is left empty-but-valid. */
static size_t kbytes(size_t cap) { return bbq_mem_array_bytes(cap, sizeof(uint64_t)); }
static size_t vbytes(size_t cap) { return bbq_mem_array_bytes(cap, sizeof(void*)); }
static size_t ubytes(size_t cap) { return bbq_mem_array_bytes(cap, sizeof(uint8_t)); }

static bool hmap_alloc(bbq_hmap* m, size_t cap) {
    uint64_t* keys;
    void**    vals;
    uint8_t*  used;
    if (!cap || !kbytes(cap) || !vbytes(cap) || !ubytes(cap)) return false;
    keys = (uint64_t*)bbq_mem_zalloc(m->a, kbytes(cap));
    vals = (void**)   bbq_mem_zalloc(m->a, vbytes(cap));
    used = (uint8_t*) bbq_mem_zalloc(m->a, ubytes(cap));
    if (!keys || !vals || !used) {
        bbq_mem_release(m->a, keys, kbytes(cap));
        bbq_mem_release(m->a, vals, vbytes(cap));
        bbq_mem_release(m->a, used, ubytes(cap));
        return false;
    }
    m->keys = keys;
    m->vals = vals;
    m->used = used;
    m->cap  = cap;
    return true;
}

bool bbq_hmap_init(bbq_hmap* m, size_t initial_cap) {
    return bbq_hmap_init_a(m, initial_cap, NULL);
}

bool bbq_hmap_init_a(bbq_hmap* m, size_t initial_cap, bbq_alloc* a) {
    size_t cap = round_pow2(initial_cap ? initial_cap : BBQ_HMAP_DEFAULT_CAP);
    m->keys = NULL;
    m->vals = NULL;
    m->used = NULL;
    m->cap  = 0;
    m->len  = 0;
    m->a    = a;
    m->oom  = false;
    if (!cap) { m->oom = true; return false; }     /* no power of two that large */
    if (!hmap_alloc(m, cap)) { m->oom = true; return false; }
    return true;
}

bool bbq_hmap_oom(const bbq_hmap* m) { return m->oom; }

void bbq_hmap_free(bbq_hmap* m) {
    bbq_mem_release(m->a, m->keys, kbytes(m->cap));
    bbq_mem_release(m->a, m->vals, vbytes(m->cap));
    bbq_mem_release(m->a, m->used, ubytes(m->cap));
    m->keys = NULL;
    m->vals = NULL;
    m->used = NULL;
    m->cap  = 0;
    m->len  = 0;
    m->oom  = false;
}

/* The slot `key` occupies, or the first free slot on its probe path. The table is never
 * allowed to fill, so this always terminates. */
static size_t hmap_slot(const bbq_hmap* m, uint64_t key) {
    size_t mask = m->cap - 1;
    size_t i = (size_t)hmap_mix(key) & mask;
    while (m->used[i] && m->keys[i] != key)
        i = (i + 1) & mask;
    return i;
}

/* Double the table (or allocate the first one) and re-place every entry. On allocation
 * failure the map is left EXACTLY as it was — same arrays, same entries, same capacity —
 * so the caller's only obligation is to refuse the put that triggered the grow. */
static bool hmap_grow(bbq_hmap* m) {
    size_t    old_cap  = m->cap;
    uint64_t* old_keys = m->keys;
    void**    old_vals = m->vals;
    uint8_t*  old_used = m->used;

    size_t cap = old_cap ? old_cap * 2 : BBQ_HMAP_DEFAULT_CAP;
    if (old_cap > SIZE_MAX / 2) return false;
    if (!hmap_alloc(m, cap))
        return false;                  /* hmap_alloc writes to `m` only on success */

    /* len is unchanged — the same entries, re-placed. */
    for (size_t i = 0; i < old_cap; i++) {
        if (!old_used[i]) continue;
        size_t s = hmap_slot(m, old_keys[i]);
        m->keys[s] = old_keys[i];
        m->vals[s] = old_vals[i];
        m->used[s] = 1;
    }
    bbq_mem_release(m->a, old_keys, kbytes(old_cap));
    bbq_mem_release(m->a, old_vals, vbytes(old_cap));
    bbq_mem_release(m->a, old_used, ubytes(old_cap));
    return true;
}

bool bbq_hmap_put(bbq_hmap* m, uint64_t key, void* val) {
    /* Grow at 3/4 load. A full (or nearly full) table makes the probe walk unbounded.
     * A cap of 0 — a fresh map whose init failed, or one that has been freed — takes the
     * same path: hmap_grow allocates the first table. If it cannot, we must refuse the
     * put rather than fall through to hmap_slot, which would mask with cap-1 == SIZE_MAX
     * and index a NULL array. */
    if ((m->len + 1) * 4 >= m->cap * 3) {
        if (!hmap_grow(m)) { m->oom = true; return false; }
    }
    size_t i = hmap_slot(m, key);
    if (!m->used[i]) {
        m->used[i] = 1;
        m->keys[i] = key;
        m->len++;
    }
    m->vals[i] = val;                              /* insert OR replace */
    return true;
}

void* bbq_hmap_get(const bbq_hmap* m, uint64_t key) {
    if (m->cap == 0) return NULL;
    size_t i = hmap_slot(m, key);
    return m->used[i] ? m->vals[i] : NULL;
}

bool bbq_hmap_contains(const bbq_hmap* m, uint64_t key) {
    if (m->cap == 0) return false;
    return m->used[hmap_slot(m, key)] != 0;
}

size_t bbq_hmap_len(const bbq_hmap* m) {
    return m->len;
}
