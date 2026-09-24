/*
 * bbq_dict.c — string-keyed dictionary over bbq_htree. See bbq_dict.h for why
 * this exists rather than each caller hashing its own names.
 *
 * Layout: the tree maps hash -> the head of a chain of entries sharing that
 * hash. Every read walks the chain comparing the FULL key, so a collision is a
 * wasted hop and never a wrong answer. Entries own their key bytes.
 */
#include "bbq_dict.h"

#include <string.h>

struct bbq_dict_node {
    struct bbq_dict_node* next;    /* next entry with the SAME hash */
    void*                 value;
    size_t                len;
    uint64_t              hash;
    unsigned char         key[];   /* len bytes, the dict's own copy */
};

static size_t node_bytes(size_t len) {
    return sizeof(struct bbq_dict_node) + len;
}

/* ── SipHash-1-3 ──────────────────────────────────────────────────────────────
 *
 * Keyed by the dict's own seed. One compression round per 8-byte block and three
 * finalisation rounds: the variant Rust and the Linux kernel use for hash tables,
 * chosen because the property needed here is that an attacker cannot COMPUTE
 * colliding keys, not that the digest be cryptographic.
 */
#define SIP_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_ROUND()                                                  \
    do {                                                             \
        v0 += v1; v1 = SIP_ROTL(v1, 13); v1 ^= v0; v0 = SIP_ROTL(v0, 32); \
        v2 += v3; v3 = SIP_ROTL(v3, 16); v3 ^= v2;                   \
        v0 += v3; v3 = SIP_ROTL(v3, 21); v3 ^= v0;                   \
        v2 += v1; v1 = SIP_ROTL(v1, 17); v1 ^= v2; v2 = SIP_ROTL(v2, 32); \
    } while (0)

uint64_t bbq_dict_hash(uint64_t seed, const void* key, size_t len) {
    const unsigned char* in = (const unsigned char*)key;
    uint64_t k0 = seed, k1 = seed ^ 0x9E3779B97F4A7C15ULL;
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;
    uint64_t b = ((uint64_t)len) << 56;
    size_t left = len & 7;
    size_t i;

    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t m = (uint64_t)in[i]            | ((uint64_t)in[i+1] << 8)  |
                     ((uint64_t)in[i+2] << 16)  | ((uint64_t)in[i+3] << 24) |
                     ((uint64_t)in[i+4] << 32)  | ((uint64_t)in[i+5] << 40) |
                     ((uint64_t)in[i+6] << 48)  | ((uint64_t)in[i+7] << 56);
        v3 ^= m;
        SIP_ROUND();
        v0 ^= m;
    }
    switch (left) {
    case 7: b |= (uint64_t)in[i+6] << 48; /* fall through */
    case 6: b |= (uint64_t)in[i+5] << 40; /* fall through */
    case 5: b |= (uint64_t)in[i+4] << 32; /* fall through */
    case 4: b |= (uint64_t)in[i+3] << 24; /* fall through */
    case 3: b |= (uint64_t)in[i+2] << 16; /* fall through */
    case 2: b |= (uint64_t)in[i+1] << 8;  /* fall through */
    case 1: b |= (uint64_t)in[i+0];       /* fall through */
    case 0: break;
    }
    v3 ^= b;
    SIP_ROUND();
    v0 ^= b;
    v2 ^= 0xff;
    SIP_ROUND(); SIP_ROUND(); SIP_ROUND();
    return v0 ^ v1 ^ v2 ^ v3;
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

void bbq_dict_init_hashed(bbq_dict* d, bbq_alloc* a, bbq_dict_hash_fn hash) {
    bbq_htree_init_a(&d->index, a);
    d->count = 0;
    d->a     = a;
    d->hash  = hash ? hash : bbq_dict_hash;
    d->seed  = bbq_hash_seed();
    d->oom   = false;
    d->gen   = 0;
}

void bbq_dict_init_seeded(bbq_dict* d, bbq_alloc* a, uint64_t seed) {
    bbq_dict_init_hashed(d, a, bbq_dict_hash);
    d->seed = seed;
}

void bbq_dict_init(bbq_dict* d) { bbq_dict_init_hashed(d, NULL, bbq_dict_hash); }

void bbq_dict_init_a(bbq_dict* d, bbq_alloc* a) {
    bbq_dict_init_hashed(d, a, bbq_dict_hash);
}

bbq_dict* bbq_dict_create(void) { return bbq_dict_create_a(NULL); }

bbq_dict* bbq_dict_create_a(bbq_alloc* a) {
    bbq_dict* d = (bbq_dict*)bbq_mem_alloc(a, sizeof *d);
    if (d) bbq_dict_init_a(d, a);
    return d;
}

void bbq_dict_destroy(bbq_dict* d) {
    bbq_alloc* a;
    if (!d) return;
    a = d->a;                       /* free() nulls nothing; read it first */
    bbq_dict_free(d);
    bbq_mem_release(a, d, sizeof *d);
}

static void chain_free(bbq_dict* d, bbq_dict_node* n) {
    while (n) {
        bbq_dict_node* next = n->next;
        bbq_mem_release(d->a, n, node_bytes(n->len));
        n = next;
    }
}

void bbq_dict_clear(bbq_dict* d) {
    bbq_htree_iter it;
    bbq_htree_leaf* l;
    bbq_htree_iter_init(&d->index, &it);
    for (l = bbq_htree_next(&it); l; l = bbq_htree_next(&it))
        chain_free(d, (bbq_dict_node*)l->value);
    bbq_htree_clear(&d->index);
    d->count = 0;
    d->gen++;
}

void bbq_dict_free(bbq_dict* d) {
    bbq_dict_clear(d);
    bbq_htree_free(&d->index);
    d->oom = false;
}

bool bbq_dict_oom(const bbq_dict* d) { return d->oom || bbq_htree_oom(&d->index); }

/* The one lookup every read goes through: find the chain by hash, then the
 * entry by its FULL key. `prev_out` is for delete, which needs to unlink. */
static bbq_dict_node* find(const bbq_dict* d, const void* key, size_t len,
                           uint64_t h, bbq_dict_node** prev_out) {
    bbq_dict_node* prev = NULL;
    bbq_dict_node* n = (bbq_dict_node*)bbq_htree_search(&d->index, h);
    for (; n; prev = n, n = n->next)
        if (n->len == len && (len == 0 || memcmp(n->key, key, len) == 0)) break;
    if (prev_out) *prev_out = prev;
    return n;
}

bool bbq_dict_put(bbq_dict* d, const void* key, size_t len, void* value) {
    uint64_t h = d->hash(d->seed, key, len);
    bbq_dict_node* hit = find(d, key, len, h, NULL);
    bbq_dict_node* n;

    if (hit) { hit->value = value; d->gen++; return true; }   /* overwrite in place */

    if (len > SIZE_MAX - sizeof *n) { d->oom = true; return false; }
    n = (bbq_dict_node*)bbq_mem_alloc(d->a, node_bytes(len));
    if (!n) { d->oom = true; return false; }
    n->value = value;
    n->len   = len;
    n->hash  = h;
    if (len) memcpy(n->key, key, len);

    /* Push onto the front of this hash's chain. The tree stores the head, so a
     * second key with the same hash replaces the stored pointer rather than
     * colliding in the tree — the chain is what keeps both reachable. */
    n->next = (bbq_dict_node*)bbq_htree_search(&d->index, h);
    if (!bbq_htree_insert(&d->index, h, n)) {
        bbq_mem_release(d->a, n, node_bytes(len));
        d->oom = true;
        return false;
    }
    d->count++;
    d->gen++;
    return true;
}

void* bbq_dict_get(const bbq_dict* d, const void* key, size_t len) {
    bbq_dict_node* n = find(d, key, len, d->hash(d->seed, key, len), NULL);
    return n ? n->value : NULL;
}

bool bbq_dict_contains(const bbq_dict* d, const void* key, size_t len) {
    return find(d, key, len, d->hash(d->seed, key, len), NULL) != NULL;
}

void* bbq_dict_delete(bbq_dict* d, const void* key, size_t len) {
    uint64_t h = d->hash(d->seed, key, len);
    bbq_dict_node* prev = NULL;
    bbq_dict_node* n = find(d, key, len, h, &prev);
    void* v;
    if (!n) return NULL;
    v = n->value;

    if (prev) {
        prev->next = n->next;
    } else if (n->next) {
        /* A new chain head. This insert cannot allocate — the key is already in
         * the tree, so it only rewrites a leaf's value — but it is checked
         * anyway, because dropping it would strand the whole rest of the chain
         * while count fell by one: entries reachable by nothing and a length
         * that disagrees with the contents. */
        if (!bbq_htree_insert(&d->index, h, n->next)) { d->oom = true; return NULL; }
    } else {
        bbq_htree_delete(&d->index, h);
    }
    bbq_mem_release(d->a, n, node_bytes(n->len));
    d->count--;
    d->gen++;
    return v;
}

bool bbq_dict_puts(bbq_dict* d, const char* key, void* value) {
    return bbq_dict_put(d, key, strlen(key), value);
}

void* bbq_dict_gets(const bbq_dict* d, const char* key) {
    return bbq_dict_get(d, key, strlen(key));
}

size_t bbq_dict_len(const bbq_dict* d) { return d->count; }

/* ── Iteration ────────────────────────────────────────────────────────────── */

void bbq_dict_iter_init(const bbq_dict* d, bbq_dict_iter* it) {
    it->d   = d;
    it->cur = NULL;
    it->gen = d->gen;
    bbq_htree_iter_init(&d->index, &it->tit);
}

bool bbq_dict_next(bbq_dict_iter* it, bbq_dict_entry* out) {
    bbq_dict_node* n = (bbq_dict_node*)it->cur;
    if (it->gen != it->d->gen) return false;     /* mutated: stop, do not walk */
    while (!n) {
        bbq_htree_leaf* l = bbq_htree_next(&it->tit);
        if (!l) return false;
        n = (bbq_dict_node*)l->value;
    }
    out->key = n->key; out->len = n->len; out->value = n->value;
    it->cur = n->next;
    return true;
}
