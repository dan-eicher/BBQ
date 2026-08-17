/*
 * bbq_dict.c — string-keyed dictionary over bbq_htree. See bbq_dict.h for why
 * this exists rather than each caller hashing its own names.
 *
 * Layout: the tree maps hash -> the head of a chain of entries sharing that
 * hash. Every read walks the chain comparing the FULL key, so a collision is a
 * wasted hop and never a wrong answer. Entries own their key bytes.
 *
 * `bucket_count` is not a table size — the tree has no buckets — it is the
 * number of distinct hashes, kept only so a caller can see chain pressure.
 */
#include "bbq_dict.h"
#include "bbq_htree.h"

#include <stdlib.h>
#include <string.h>

typedef struct bbq_dict_node {
    struct bbq_dict_node* next;    /* next entry with the SAME hash */
    void*                 value;
    size_t                len;
    uint32_t              hash;
    unsigned char         key[];   /* len bytes, the dict's own copy */
} bbq_dict_node;

struct bbq_dict {
    bbq_htree* index;              /* hash -> bbq_dict_node* (chain head) */
    size_t     count;
};

uint32_t bbq_dict_hash(const void* key, size_t len) {
    const unsigned char* b = (const unsigned char*)key;
    uint32_t h = 5381;
    for (size_t i = 0; i < len; i++) h = h * 33 + b[i];
    return h ? h : 1;              /* bbq_htree reserves key 0 */
}

bbq_dict* bbq_dict_create(void) {
    bbq_dict* d = (bbq_dict*)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->index = bbq_htree_create();
    if (!d->index) { free(d); return NULL; }
    return d;
}

static void chain_free(bbq_dict_node* n) {
    while (n) { bbq_dict_node* next = n->next; free(n); n = next; }
}

void bbq_dict_clear(bbq_dict* d) {
    if (!d) return;
    bbq_htree_iter it;
    bbq_htree_iter_init(d->index, &it);
    for (bbq_htree_leaf* l = bbq_htree_next(&it); l; l = bbq_htree_next(&it))
        chain_free((bbq_dict_node*)l->value);
    bbq_htree_clear(d->index);
    d->count = 0;
}

void bbq_dict_destroy(bbq_dict* d) {
    if (!d) return;
    bbq_dict_clear(d);
    bbq_htree_destroy(d->index);
    free(d);
}

/* The one lookup every read goes through: find the chain by hash, then the
 * entry by its FULL key. `prev_out` is for delete, which needs to unlink. */
static bbq_dict_node* find(const bbq_dict* d, const void* key, size_t len,
                           uint32_t h, bbq_dict_node** prev_out) {
    bbq_dict_node* prev = NULL;
    bbq_dict_node* n = (bbq_dict_node*)bbq_htree_search(d->index, h);
    for (; n; prev = n, n = n->next)
        if (n->len == len && (len == 0 || memcmp(n->key, key, len) == 0)) break;
    if (prev_out) *prev_out = prev;
    return n;
}

bool bbq_dict_put(bbq_dict* d, const void* key, size_t len, void* value) {
    if (!d) return false;
    uint32_t h = bbq_dict_hash(key, len);
    bbq_dict_node* hit = find(d, key, len, h, NULL);
    if (hit) { hit->value = value; return true; }        /* overwrite in place */

    bbq_dict_node* n = (bbq_dict_node*)malloc(sizeof *n + len);
    if (!n) return false;
    n->value = value;
    n->len   = len;
    n->hash  = h;
    if (len) memcpy(n->key, key, len);

    /* Push onto the front of this hash's chain. The tree stores the head, so a
     * second key with the same hash replaces the stored pointer rather than
     * colliding in the tree — the chain is what keeps both reachable. */
    n->next = (bbq_dict_node*)bbq_htree_search(d->index, h);
    if (!bbq_htree_insert(d->index, h, n)) { free(n); return false; }
    d->count++;
    return true;
}

void* bbq_dict_get(const bbq_dict* d, const void* key, size_t len) {
    if (!d) return NULL;
    bbq_dict_node* n = find(d, key, len, bbq_dict_hash(key, len), NULL);
    return n ? n->value : NULL;
}

bool bbq_dict_contains(const bbq_dict* d, const void* key, size_t len) {
    if (!d) return false;
    return find(d, key, len, bbq_dict_hash(key, len), NULL) != NULL;
}

void* bbq_dict_delete(bbq_dict* d, const void* key, size_t len) {
    if (!d) return NULL;
    uint32_t h = bbq_dict_hash(key, len);
    bbq_dict_node* prev = NULL;
    bbq_dict_node* n = find(d, key, len, h, &prev);
    if (!n) return NULL;
    void* v = n->value;
    if (prev) prev->next = n->next;
    else if (n->next) bbq_htree_insert(d->index, h, n->next);  /* new chain head */
    else bbq_htree_delete(d->index, h);                        /* chain is empty */
    free(n);
    d->count--;
    return v;
}

bool bbq_dict_puts(bbq_dict* d, const char* key, void* value) {
    return bbq_dict_put(d, key, strlen(key), value);
}

void* bbq_dict_gets(const bbq_dict* d, const char* key) {
    return bbq_dict_get(d, key, strlen(key));
}

size_t bbq_dict_len(const bbq_dict* d) { return d ? d->count : 0; }

/* Iteration walks the tree's leaves and each leaf's chain. `slot` is unused for
 * the tree walk itself — the iterator state lives in `cur` plus a tree iterator
 * held in the dict-iterator's own storage, so this stays a value type. */
typedef struct { bbq_htree_iter tit; bbq_dict_node* n; } dict_iter_state;

void bbq_dict_iter_init(const bbq_dict* d, bbq_dict_iter* it) {
    it->d = d; it->slot = 0; it->cur = NULL;
    if (!d) return;
    dict_iter_state* st = (dict_iter_state*)calloc(1, sizeof *st);
    if (!st) return;
    bbq_htree_iter_init(d->index, &st->tit);
    it->cur = st;
}

bool bbq_dict_next(bbq_dict_iter* it, bbq_dict_entry* out) {
    dict_iter_state* st = (dict_iter_state*)it->cur;
    if (!st) return false;
    while (!st->n) {
        bbq_htree_leaf* l = bbq_htree_next(&st->tit);
        if (!l) { free(st); it->cur = NULL; return false; }
        st->n = (bbq_dict_node*)l->value;
    }
    out->key = st->n->key; out->len = st->n->len; out->value = st->n->value;
    st->n = st->n->next;
    return true;
}
