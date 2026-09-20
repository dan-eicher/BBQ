/*
 * bbq_htree.c — 16-way radix tree with lazy expansion. See bbq_htree.h.
 */
#include "bbq_htree.h"

#include <string.h>

struct bbq_htree_node {
    bool is_leaf;
    union {
        bbq_htree_node* children[16];
        bbq_htree_leaf  leaf;
    } u;
};

/* Nibble `level` of `key`, counted from the most significant. Levels run
 * 0..BBQ_HTREE_NIBBLES-1; asking for one past the end would shift by a negative
 * amount, which is undefined — the old code could not reach that case because
 * its leaves were all at the bottom, and lazy expansion is exactly what makes it
 * reachable, so it is asserted by construction here instead. */
static unsigned nibble(bbq_htree_key key, int level) {
    return (unsigned)((key >> (60 - level * 4)) & 0xF);
}

/* The first level at or after `from` where the two keys differ. The caller only
 * asks about keys it already knows are different, so this always finds one
 * before running off the end. */
static int divergence(bbq_htree_key a, bbq_htree_key b, int from) {
    int d = from;
    while (d < BBQ_HTREE_NIBBLES && nibble(a, d) == nibble(b, d)) d++;
    return d;
}

static bbq_htree_node* node_new(bbq_htree* t, bool leaf) {
    bbq_htree_node* n = (bbq_htree_node*)bbq_mem_zalloc(t->a, sizeof *n);
    if (!n) { t->oom = true; return NULL; }
    n->is_leaf = leaf;
    return n;
}

static void node_free_all(bbq_alloc* a, bbq_htree_node* n) {
    if (!n) return;
    if (!n->is_leaf)
        for (int i = 0; i < 16; i++) node_free_all(a, n->u.children[i]);
    bbq_mem_release(a, n, sizeof *n);
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

void bbq_htree_init(bbq_htree* t) { bbq_htree_init_a(t, NULL); }

void bbq_htree_init_a(bbq_htree* t, bbq_alloc* a) {
    t->root  = NULL;
    t->count = 0;
    t->a     = a;
    t->oom   = false;
    t->gen   = 0;
}

void bbq_htree_free(bbq_htree* t) {
    node_free_all(t->a, t->root);
    t->root  = NULL;
    t->count = 0;
    t->oom   = false;
    t->gen++;
}

bbq_htree* bbq_htree_create(void) { return bbq_htree_create_a(NULL); }

bbq_htree* bbq_htree_create_a(bbq_alloc* a) {
    bbq_htree* t = (bbq_htree*)bbq_mem_alloc(a, sizeof *t);
    if (t) bbq_htree_init_a(t, a);
    return t;
}

void bbq_htree_destroy(bbq_htree* t) {
    bbq_alloc* a;
    if (!t) return;
    a = t->a;                       /* free() nulls nothing; read it first */
    bbq_htree_free(t);
    bbq_mem_release(a, t, sizeof *t);
}

void bbq_htree_clear(bbq_htree* t) {
    node_free_all(t->a, t->root);
    t->root  = NULL;
    t->count = 0;
    t->gen++;
}

bool bbq_htree_oom(const bbq_htree* t)      { return t->oom; }
size_t bbq_htree_size(const bbq_htree* t)   { return t->count; }
bool bbq_htree_is_empty(const bbq_htree* t) { return t->count == 0; }

/* ── Insert ───────────────────────────────────────────────────────────────── */

/* Replace the lone leaf at *slot with a chain of interior nodes down to the
 * first nibble where the two keys differ, holding both leaves there. Builds the
 * whole chain into a local first and only publishes it on success, so a refusal
 * partway leaves the tree exactly as it was. */
static bool split_leaf(bbq_htree* t, bbq_htree_node** slot, int level,
                       bbq_htree_key key, void* value) {
    bbq_htree_node* old  = *slot;
    bbq_htree_node* head = NULL;
    bbq_htree_node** tail = &head;
    int d = divergence(old->u.leaf.key, key, level);
    bbq_htree_node* fork;
    bbq_htree_node* fresh;

    /* One interior node per shared nibble, from `level` up to the divergence. */
    for (int l = level; l < d; l++) {
        bbq_htree_node* mid = node_new(t, false);
        if (!mid) { node_free_all(t->a, head); return false; }
        *tail = mid;
        tail  = &mid->u.children[nibble(key, l)];
    }

    /* At the divergence the two keys take different children. */
    fork = node_new(t, false);
    if (!fork) { node_free_all(t->a, head); return false; }
    fresh = node_new(t, true);
    if (!fresh) { node_free_all(t->a, head); bbq_mem_release(t->a, fork, sizeof *fork); return false; }
    fresh->u.leaf.key   = key;
    fresh->u.leaf.value = value;

    fork->u.children[nibble(old->u.leaf.key, d)] = old;
    fork->u.children[nibble(key, d)]             = fresh;

    *tail = fork;
    *slot = head ? head : fork;
    return true;
}

bool bbq_htree_insert(bbq_htree* t, bbq_htree_key key, void* value) {
    bbq_htree_node** slot = &t->root;

    for (int level = 0; ; level++) {
        if (!*slot) {                          /* free position: a leaf lands here */
            bbq_htree_node* n = node_new(t, true);
            if (!n) return false;
            n->u.leaf.key   = key;
            n->u.leaf.value = value;
            *slot = n;
            t->count++;
            t->gen++;
            return true;
        }
        if ((*slot)->is_leaf) {
            if ((*slot)->u.leaf.key == key) {  /* same key: replace the value */
                (*slot)->u.leaf.value = value;
                t->gen++;
                return true;
            }
            /* Two different keys agreeing on nibbles 0..level-1 must differ
             * somewhere in level..15, so the split always terminates. */
            if (!split_leaf(t, slot, level, key, value)) return false;
            t->count++;
            t->gen++;
            return true;
        }
        /* An interior node once every nibble is spent cannot exist: 16 nibbles
         * determine the key completely, so that position holds either this key's
         * leaf or nothing, both handled above. Refusing rather than descending
         * keeps nibble() from being asked for a seventeenth nibble, which would
         * shift by a negative amount. */
        if (level >= BBQ_HTREE_NIBBLES) return false;
        slot = &(*slot)->u.children[nibble(key, level)];
    }
}

/* ── Search ───────────────────────────────────────────────────────────────── */

static const bbq_htree_node* find_leaf(const bbq_htree* t, bbq_htree_key key) {
    const bbq_htree_node* n = t->root;
    for (int level = 0; n && level < BBQ_HTREE_NIBBLES; level++) {
        if (n->is_leaf) return n->u.leaf.key == key ? n : NULL;
        n = n->u.children[nibble(key, level)];
    }
    /* Depth 16 can only hold a leaf (every nibble is spent), and the loop above
     * already returned for one. Anything else is a malformed tree, not a hit. */
    return (n && n->is_leaf && n->u.leaf.key == key) ? n : NULL;
}

void* bbq_htree_search(const bbq_htree* t, bbq_htree_key key) {
    const bbq_htree_node* n = find_leaf(t, key);
    return n ? n->u.leaf.value : NULL;
}

bool bbq_htree_contains(const bbq_htree* t, bbq_htree_key key) {
    return find_leaf(t, key) != NULL;
}

/* ── Delete ───────────────────────────────────────────────────────────────── */

void* bbq_htree_delete(bbq_htree* t, bbq_htree_key key) {
    bbq_htree_node** path[BBQ_HTREE_NIBBLES + 1];
    bbq_htree_node** slot = &t->root;
    int depth = 0;
    void* value;

    while (*slot && !(*slot)->is_leaf) {
        if (depth >= BBQ_HTREE_NIBBLES) return NULL;
        path[depth] = slot;
        slot = &(*slot)->u.children[nibble(key, depth)];
        depth++;
    }
    if (!*slot || (*slot)->u.leaf.key != key) return NULL;

    value = (*slot)->u.leaf.value;
    bbq_mem_release(t->a, *slot, sizeof **slot);
    *slot = NULL;
    t->count--;
    t->gen++;

    /* Walk back up. An interior node with nothing left goes; one with a single
     * remaining LEAF is replaced by that leaf, which is what keeps a leaf at the
     * shallowest depth that distinguishes it — the invariant insert relies on to
     * decide when a split is needed. */
    while (depth > 0) {
        bbq_htree_node** pslot = path[--depth];
        bbq_htree_node*  p     = *pslot;
        bbq_htree_node*  only  = NULL;
        int n = 0;
        for (int i = 0; i < 16 && n < 2; i++)
            if (p->u.children[i]) { only = p->u.children[i]; n++; }

        if (n == 0) {
            bbq_mem_release(t->a, p, sizeof *p);
            *pslot = NULL;
            continue;
        }
        if (n == 1 && only->is_leaf) {
            bbq_mem_release(t->a, p, sizeof *p);
            *pslot = only;
            continue;
        }
        break;
    }
    return value;
}

/* ── Iteration ────────────────────────────────────────────────────────────── */

void bbq_htree_iter_init(const bbq_htree* t, bbq_htree_iter* it) {
    it->t     = t;
    it->gen   = t->gen;
    it->depth = -1;
    if (t->root) {
        it->depth      = 0;
        it->stack[0]   = t->root;
        it->idx[0]     = 0;
    }
}

bbq_htree_leaf* bbq_htree_next(bbq_htree_iter* it) {
    if (it->depth < 0) return NULL;
    /* The tree changed under us. Stop rather than walk nodes that may have been
     * freed — the alternative is a use-after-free that only shows up for the
     * caller who mutates mid-iteration. */
    if (it->gen != it->t->gen) { it->depth = -1; return NULL; }

    while (it->depth >= 0) {
        bbq_htree_node* cur = it->stack[it->depth];
        unsigned i;

        if (cur->is_leaf) {
            it->depth--;
            return &cur->u.leaf;
        }
        i = it->idx[it->depth];
        while (i < 16 && !cur->u.children[i]) i++;
        it->idx[it->depth] = (uint8_t)(i + 1);

        if (i < 16) {
            it->depth++;
            it->stack[it->depth] = cur->u.children[i];
            it->idx[it->depth]   = 0;
        } else {
            it->depth--;
        }
    }
    return NULL;
}

/* ── Clone ────────────────────────────────────────────────────────────────── */

bool bbq_htree_clone(bbq_htree* dst, const bbq_htree* src) {
    bbq_htree_iter  it;
    bbq_htree_leaf* leaf;
    bbq_htree_iter_init(src, &it);
    while ((leaf = bbq_htree_next(&it)))
        if (!bbq_htree_insert(dst, leaf->key, leaf->value)) return false;
    return true;
}
