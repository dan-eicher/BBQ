/*
 * egraph.c — see egraph.h for what the structure is and why.
 *
 * Node storage is a flat array; a class owns the indices of its nodes.
 * Union-find canonicalises class ids. The hashcons is a bbq_hmap keyed by a
 * hash of (op, canonical children) — the structure whose own header names
 * this case: "what index did I assign this object?", asked against keys we
 * mint ourselves. A hash is a bucket and not an identity, so each bucket
 * chains its nodes through next_hash and every hit is confirmed by
 * comparing the real operator and children.
 */
#include "egraph.h"
#include "bbq_hmap.h"
#include "bbq_vec.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Children live in one shared growable array rather than a fixed inline
 * slot, so an operator's arity is whatever the consumer's IR says it is.
 * A capped array would have to either reject a wider node or truncate it,
 * and truncation is not an option here: `f(a..h,i)` and `f(a..h,j)` would
 * hashcons to the same node, making the e-graph assert an equality that
 * does not hold — a miscompile the representation itself manufactures. */
struct eg_node {
    int    op;
    int64_t data;      /* part of the key; uninterpreted */
    int    nkids;
    size_t kid_off;    /* into egraph::kids */
    eg_id  cls;        /* the class this node belongs to */
    int    next_hash;  /* next node with the same hash, -1 at the end */
    bool   live;
};

struct eg_class {
    int* node_idx;   /* bbq_vec of node indices */
};

void eg_init(egraph* g) {
    memset(g, 0, sizeof *g);
    bbq_hmap* m = (bbq_hmap*)calloc(1, sizeof *m);
    bbq_hmap_init(m, 0);
    g->hashcons = m;
}

void eg_free(egraph* g) {
    for (int i = 0; i < bbq_vec_len(g->classes); i++)
        bbq_vec_free(g->classes[i].node_idx);
    if (g->hashcons) { bbq_hmap_free((bbq_hmap*)g->hashcons); free(g->hashcons); }
    bbq_vec_free(g->nodes);
    bbq_vec_free(g->kids);
    bbq_vec_free(g->classes);
    bbq_vec_free(g->parent);
    bbq_vec_free(g->worklist);
    bbq_vec_free(g->class_data);
    free(g->analysis);
    memset(g, 0, sizeof *g);
}

/* ── e-class analysis (egg §4.1) ─────────────────────────────────── */

void eg_set_analysis(egraph* g, const eg_analysis* a) {
    free(g->analysis);
    g->analysis = NULL;
    bbq_vec_truncate(g->class_data, 0);
    if (!a || a->size == 0) return;
    g->analysis = (eg_analysis*)malloc(sizeof *g->analysis);
    if (!g->analysis) return;
    *g->analysis = *a;
}

void* eg_class_data(egraph* g, eg_id c) {
    if (!g->analysis) return NULL;
    return g->class_data + (size_t)eg_find(g, c) * g->analysis->size;
}

/* Reserve and zero the fact slot for a freshly created class. Zeroing is
 * what lets facts be compared with memcmp: two equal facts then have
 * equal bytes, padding included. */
static void analysis_reserve(egraph* g, eg_id cls) {
    if (!g->analysis) return;
    size_t sz = g->analysis->size;
    size_t need = ((size_t)cls + 1) * sz;
    while ((size_t)bbq_vec_len(g->class_data) < need)
        bbq_vec_push(g->class_data, (unsigned char)0);
    memset(g->class_data + (size_t)cls * sz, 0, sz);
}

size_t eg_node_count(const egraph* g) { return (size_t)bbq_vec_len(g->nodes); }

/* ── union-find ──────────────────────────────────────────────────── */

eg_id eg_find(egraph* g, eg_id id) {
    while (g->parent[id] != id) {
        g->parent[id] = g->parent[g->parent[id]];   /* halving */
        id = g->parent[id];
    }
    return id;
}

static eg_id eg_new_class(egraph* g) {
    eg_id id = (eg_id)bbq_vec_len(g->classes);
    struct eg_class empty; empty.node_idx = NULL;
    bbq_vec_push(g->classes, empty);
    bbq_vec_push(g->parent, id);          /* a fresh class is its own root */
    return id;
}

static void class_push(egraph* g, eg_id cls, int node_idx) {
    bbq_vec_push(g->classes[cls].node_idx, node_idx);
}

/* ── hashcons ────────────────────────────────────────────────────── */

static eg_id* node_kids(egraph* g, const struct eg_node* n) {
    /* `kids` is unallocated until a node with children is added, and
     * NULL + 0 is undefined. Every caller guards on nkids before
     * reading through the result. */
    return g->kids ? g->kids + n->kid_off : NULL;
}

static void canonicalise(egraph* g, struct eg_node* n) {
    eg_id* k = node_kids(g, n);
    for (int i = 0; i < n->nkids; i++) k[i] = eg_find(g, k[i]);
}

/* Congruence key: the operator and the canonical children. A hash is a
 * bucket, never an identity — every hit is confirmed by comparison. */
static uint64_t node_hash(const egraph* g, int op, int64_t data,
                          const eg_id* kids, int nkids) {
    uint64_t h = 1469598103934665603ull;         /* FNV-1a */
    (void)g;
    h = (h ^ (uint64_t)(uint32_t)op) * 1099511628211ull;
    h = (h ^ (uint64_t)data) * 1099511628211ull;
    h = (h ^ (uint64_t)(uint32_t)nkids) * 1099511628211ull;
    for (int i = 0; i < nkids; i++)
        h = (h ^ (uint64_t)(uint32_t)kids[i]) * 1099511628211ull;
    return h;
}

static bool node_matches(egraph* g, const struct eg_node* n,
                         int op, int64_t data, const eg_id* kids, int nkids) {
    if (n->op != op || n->data != data || n->nkids != nkids) return false;
    const eg_id* k = node_kids(g, n);
    for (int i = 0; i < nkids; i++)
        if (k[i] != kids[i]) return false;
    return true;
}

/* Chain the node at `idx` onto its hash bucket. Values are index+1 so that
 * a present entry is distinguishable without a second contains() call. */
static void hashcons_insert(egraph* g, size_t idx, uint64_t h) {
    bbq_hmap* m = (bbq_hmap*)g->hashcons;
    void* head = bbq_hmap_get(m, h);
    g->nodes[idx].next_hash = head ? (int)((intptr_t)head - 1) : -1;
    bbq_hmap_put(m, h, (void*)(intptr_t)(idx + 1));
}

/* The class of a live node congruent to (op, kids), or -1. */
static eg_id hashcons_lookup(egraph* g, int op, int64_t data,
                             const eg_id* kids, int nkids, uint64_t h) {
    bbq_hmap* m = (bbq_hmap*)g->hashcons;
    void* head = bbq_hmap_get(m, h);
    for (int i = head ? (int)((intptr_t)head - 1) : -1; i >= 0;
         i = g->nodes[i].next_hash) {
        if (!g->nodes[i].live) continue;
        if (node_matches(g, &g->nodes[i], op, data, kids, nkids))
            return eg_find(g, g->nodes[i].cls);
    }
    return -1;
}

eg_id eg_add(egraph* g, int op, int64_t data, const eg_id* kids, int nkids) {
    /* Canonicalise into the shared array first: the key is the canonical
     * form, and the node keeps exactly what was hashed. */
    /* Append the canonical children, then look up: the key IS the canonical
     * form. On a hit the appended tail is abandoned (the vec is truncated
     * back), so a shared term costs no storage. */
    size_t off = (size_t)bbq_vec_len(g->kids);
    for (int i = 0; i < nkids; i++) bbq_vec_push(g->kids, eg_find(g, kids[i]));

    /* A childless node adds nothing, so `g->kids` may still be NULL —
     * and NULL + 0 is undefined even though nkids == 0 means nothing
     * ever reads through it. */
    const eg_id* kbase = g->kids ? g->kids + off : NULL;
    uint64_t h = node_hash(g, op, data, kbase, nkids);
    eg_id found = hashcons_lookup(g, op, data, kbase, nkids, h);
    if (found >= 0) { bbq_vec_truncate(g->kids, (int)off); return found; }

    eg_id cls = eg_new_class(g);
    struct eg_node n;
    n.op = op;
    n.data = data;
    n.nkids = nkids;
    n.kid_off = off;
    n.cls = cls;
    n.next_hash = -1;
    n.live = true;
    int idx = bbq_vec_len(g->nodes);
    bbq_vec_push(g->nodes, n);
    class_push(g, cls, idx);
    hashcons_insert(g, (size_t)idx, h);

    /* egg Fig. 9 lines 10-11: a new singleton's fact is make() of its one
     * e-node, and modify() may then add to the class. */
    analysis_reserve(g, cls);
    if (g->analysis) {
        size_t sz = g->analysis->size;
        g->analysis->make(g, op, data, kbase, nkids,
                          g->class_data + (size_t)cls * sz,
                          g->analysis->user);
        if (g->analysis->modify) {
            /* On a COPY: modify may intern a node, which grows the fact
             * array and moves it, so a pointer into it would dangle the
             * moment modify used the graph it was handed. */
            unsigned char* snap = (unsigned char*)malloc(sz);
            if (snap) {
                memcpy(snap, g->class_data + (size_t)cls * sz, sz);
                g->analysis->modify(g, cls, snap, g->analysis->user);
                free(snap);
            }
        }
    }
    return cls;
}

/* ── merge + congruence repair ───────────────────────────────────── */

static void work_push(egraph* g, eg_id cls) {
    bbq_vec_push(g->worklist, cls);
}

bool eg_merge(egraph* g, eg_id a, eg_id b) {
    a = eg_find(g, a);
    b = eg_find(g, b);
    if (a == b) return false;

    /* Fold the smaller class into the larger so repair touches fewer nodes. */
    if (bbq_vec_len(g->classes[a].node_idx) <
        bbq_vec_len(g->classes[b].node_idx)) { eg_id t = a; a = b; b = t; }
    g->parent[b] = a;

    for (int i = 0; i < bbq_vec_len(g->classes[b].node_idx); i++) {
        int idx = g->classes[b].node_idx[i];
        g->nodes[idx].cls = a;
        class_push(g, a, idx);
    }
    bbq_vec_truncate(g->classes[b].node_idx, 0);

    /* egg Fig. 9 lines 17-18: the survivor's fact is the join of both. */
    if (g->analysis) {
        size_t sz = g->analysis->size;
        unsigned char* tmp = (unsigned char*)calloc(1, sz);
        if (tmp) {
            g->analysis->join(g->class_data + (size_t)a * sz,
                              g->class_data + (size_t)b * sz,
                              tmp, g->analysis->user);
            memcpy(g->class_data + (size_t)a * sz, tmp, sz);
            free(tmp);
        }
    }

    /* Every node in the graph may now have a stale child; the repair pass
     * is what re-canonicalises and re-hashconses them. */
    work_push(g, a);
    return true;
}

/* Restore egg's analysis invariant: `d_c = ⋀ make(n)` over the class's
 * e-nodes, then `modify(c) = c`.
 *
 * egg Fig. 9 lines 37-44 walk each repaired class's PARENTS and re-join
 * them. This file repairs by re-canonicalising and re-hashconsing every
 * node instead of keeping parent lists, so the fact pass takes the same
 * shape: sweep every live node joining its make() into its class, to a
 * fixpoint. It reaches the same answer — the invariant is a property of
 * the finished graph, not of the order it is restored in — and it needs
 * no structure the repair does not already maintain. Terms are
 * applet-sized, so the sweep is cheap; the fixpoint terminates because
 * join is a semilattice operation and the caller's domain has finite
 * height (widening is the caller's, see egraph.h). */
static void analysis_restore(egraph* g) {
    if (!g->analysis) return;
    size_t sz = g->analysis->size;
    unsigned char* made = (unsigned char*)calloc(1, sz);
    unsigned char* joined = (unsigned char*)calloc(1, sz);
    if (!made || !joined) { free(made); free(joined); return; }

    /* Bounded, for the same reason saturation is: a congruence cycle can
     * let a fact keep moving, and a bound is a parameter rather than a
     * hope. A domain of finite height reaches its fixpoint long before
     * this; one that does not has a join that is not widening, which is
     * the caller's contract to keep. */
    int rounds = 64;
    bool changed = true;
    while (changed && rounds-- > 0) {
        changed = false;
        for (int i = 0; i < bbq_vec_len(g->nodes); i++) {
            struct eg_node* n = &g->nodes[i];
            if (!n->live) continue;
            eg_id c = eg_find(g, n->cls);
            memset(made, 0, sz);
            g->analysis->make(g, n->op, n->data, node_kids(g, n), n->nkids,
                              made, g->analysis->user);
            unsigned char* cur = g->class_data + (size_t)c * sz;
            memset(joined, 0, sz);
            g->analysis->join(cur, made, joined, g->analysis->user);
            if (memcmp(cur, joined, sz) != 0) {
                memcpy(cur, joined, sz);
                changed = true;
            }
        }
    }

    if (g->analysis->modify) {
        for (eg_id c = 0; c < (eg_id)bbq_vec_len(g->classes); c++) {
            if (eg_find(g, c) != c) continue;
            /* Same copy as in eg_add, for the same reason: modify interns
             * nodes, and interning moves this array. */
            memcpy(made, g->class_data + (size_t)c * sz, sz);
            g->analysis->modify(g, c, made, g->analysis->user);
        }
    }
    free(made);
    free(joined);
}

void eg_rebuild(egraph* g) {
    while (bbq_vec_len(g->worklist) > 0) {
        bbq_vec_truncate(g->worklist, 0);

        /* A merge changed some class's canonical id, so every node's
         * children may be stale. Re-canonicalise, then rebuild the hashcons
         * from scratch over the new keys — a node's key moved, and the map
         * has no deletion, so re-keying beats patching. Two nodes that land
         * on the same key are congruent: merge their classes and retire the
         * duplicate, which re-arms the worklist and runs this to a fixpoint. */
        bbq_hmap* m = (bbq_hmap*)g->hashcons;
        bbq_hmap_free(m);
        bbq_hmap_init(m, 0);

        for (int i = 0; i < bbq_vec_len(g->nodes); i++) {
            if (!g->nodes[i].live) continue;
            canonicalise(g, &g->nodes[i]);
            g->nodes[i].cls = eg_find(g, g->nodes[i].cls);
            g->nodes[i].next_hash = -1;
        }
        for (int i = 0; i < bbq_vec_len(g->nodes); i++) {
            struct eg_node* n = &g->nodes[i];
            if (!n->live) continue;
            uint64_t h = node_hash(g, n->op, n->data, node_kids(g, n), n->nkids);
            eg_id other = hashcons_lookup(g, n->op, n->data,
                                          node_kids(g, n), n->nkids, h);
            if (other >= 0) {
                eg_id mine = eg_find(g, n->cls);
                if (other != mine) eg_merge(g, other, mine);
                n->live = false;   /* the congruent twin already represents it */
                continue;
            }
            hashcons_insert(g, (size_t)i, h);
        }

        analysis_restore(g);
    }
}

/* ── iteration ───────────────────────────────────────────────────── */

/* A class's live nodes. Merging appends the absorbed class's nodes to the
 * survivor and retires congruent duplicates, so the list is filtered by
 * `live` rather than compacted — an index here is stable for the duration
 * of one matcher pass, which is what a matcher needs. */
static int class_live_node(egraph* g, eg_id cls, int i) {
    struct eg_class* c = &g->classes[eg_find(g, cls)];
    int seen = 0;
    for (int k = 0; k < bbq_vec_len(c->node_idx); k++) {
        int idx = c->node_idx[k];
        if (!g->nodes[idx].live) continue;
        if (seen++ == i) return idx;
    }
    return -1;
}

int eg_class_count(const egraph* g) { return bbq_vec_len(g->classes); }

int eg_class_nodes(egraph* g, eg_id cls) {
    struct eg_class* c = &g->classes[eg_find(g, cls)];
    int n = 0;
    for (int k = 0; k < bbq_vec_len(c->node_idx); k++)
        if (g->nodes[c->node_idx[k]].live) n++;
    return n;
}

int eg_class_node_op(egraph* g, eg_id cls, int i) {
    int idx = class_live_node(g, cls, i);
    return idx < 0 ? -1 : g->nodes[idx].op;
}

int64_t eg_class_node_data(egraph* g, eg_id cls, int i) {
    int idx = class_live_node(g, cls, i);
    return idx < 0 ? 0 : g->nodes[idx].data;
}

int eg_class_node_nkids(egraph* g, eg_id cls, int i) {
    int idx = class_live_node(g, cls, i);
    return idx < 0 ? -1 : g->nodes[idx].nkids;
}

eg_id eg_class_node_kid(egraph* g, eg_id cls, int i, int k) {
    int idx = class_live_node(g, cls, i);
    if (idx < 0 || k < 0 || k >= g->nodes[idx].nkids) return -1;
    return eg_find(g, node_kids(g, &g->nodes[idx])[k]);
}

/* ── saturation ──────────────────────────────────────────────────── */

int eg_saturate(egraph* g, eg_round_fn round, void* user, eg_caps caps) {
    int r = 0;
    while (r < caps.rounds && bbq_vec_len(g->nodes) < caps.node_budget) {
        bool changed = round(g, user);
        r++;
        eg_rebuild(g);
        if (!changed) break;
    }
    return r;
}

/* ── extraction ──────────────────────────────────────────────────── */

/* Cheapest cost per class, and the node achieving it. Costs are computed to
 * a fixpoint: a class's cost can fall when one of its children's does, so
 * the sweep repeats until nothing improves. Cycles never converge to a
 * finite cost and are simply never chosen. */
typedef struct {
    long long* cost;   /* per class; -1 = not yet finite */
    int*       best;   /* node index achieving it */
} ext_state;

static long long node_cost(egraph* g, const ext_state* st,
                           const struct eg_node* n,
                           eg_cost_fn cost, void* user) {
    long long c = cost(n->op, n->data, user);
    for (int i = 0; i < n->nkids; i++) {
        long long kc = st->cost[eg_find(g, node_kids(g, n)[i])];
        if (kc < 0) return -1;   /* a child has no finite term yet */
        c += kc;
    }
    return c;
}

bool eg_extract_prepare(egraph* g, eg_cost_fn cost, void* user,
                        int excl_op, int64_t excl_data,
                        eg_extract_plan* plan) {
    size_t nc = (size_t)bbq_vec_len(g->classes);
    plan->nclasses = (int)nc;
    plan->cost = (long long*)malloc((nc ? nc : 1) * sizeof *plan->cost);
    plan->best = (int*)malloc((nc ? nc : 1) * sizeof *plan->best);
    if (!plan->cost || !plan->best) { eg_extract_plan_free(plan); return false; }
    for (size_t i = 0; i < nc; i++) { plan->cost[i] = -1; plan->best[i] = -1; }

    ext_state st;
    st.cost = plan->cost;
    st.best = plan->best;

    bool improved = true;
    while (improved) {
        improved = false;
        for (int i = 0; i < bbq_vec_len(g->nodes); i++) {
            if (!g->nodes[i].live) continue;
            /* The excluded term is not a candidate anywhere in the result.
             * See eg_extract_excluding's header comment for why a caller
             * needs this and why "anywhere", not just at the root. */
            if (g->nodes[i].op == excl_op && g->nodes[i].data == excl_data)
                continue;
            eg_id c = eg_find(g, g->nodes[i].cls);
            long long nc2 = node_cost(g, &st, &g->nodes[i], cost, user);
            if (nc2 < 0) continue;
            if (st.cost[c] < 0 || nc2 < st.cost[c]) {
                st.cost[c] = nc2;
                st.best[c] = (int)i;
                improved = true;
            }
        }
    }
    return true;
}

void eg_extract_plan_free(eg_extract_plan* plan) {
    free(plan->cost); free(plan->best);
    plan->cost = NULL; plan->best = NULL; plan->nclasses = 0;
}

bool eg_extract_from(egraph* g, const eg_extract_plan* plan, eg_id root,
                     eg_extract_result* out) {
    memset(out, 0, sizeof *out);
    size_t nc = (size_t)plan->nclasses;
    ext_state st;
    st.cost = plan->cost;
    st.best = plan->best;

    root = eg_find(g, root);
    if (st.cost[root] < 0) return false;

    /* Size the output: one node per class on the chosen term. */
    int* memo = (int*)malloc(nc * sizeof *memo);
    for (size_t i = 0; i < nc; i++) memo[i] = -1;

    out->ops     = (int*)malloc(nc * sizeof(int));
    out->data    = (int64_t*)malloc(nc * sizeof(int64_t));
    out->nkids   = (int*)malloc(nc * sizeof(int));
    out->kid_off = (int*)malloc(nc * sizeof(int));
    /* The chosen term takes at most one node per class, and every node's
     * children already sit in g->kids — so that total bounds the output. */
    out->kids    = (int*)malloc(((size_t)bbq_vec_len(g->kids) + 1) * sizeof(int));

    /* Emit children-first so every kid already has an index, filling the
     * kid array as we go. The walk stack is a bbq_vec: the fixed 256-slot
     * array it replaces overflowed silently on any term deeper than that,
     * and a library never smashes its host's stack over input shape. */
    int nkids_total = 0;
    eg_id* stack = NULL; int sp = 0;
    bbq_vec_push(stack, root); sp = 1;
    /* iterative post-order over the chosen nodes */
    while (sp > 0) {
        eg_id c = eg_find(g, stack[sp - 1]);
        if (memo[c] >= 0) { sp--; continue; }
        const struct eg_node* n = &g->nodes[st.best[c]];
        bool pending = false;
        for (int i = 0; i < n->nkids; i++) {
            eg_id kc = eg_find(g, node_kids(g, n)[i]);
            if (memo[kc] < 0) {
                if (sp >= (int)bbq_vec_len(stack)) bbq_vec_push(stack, kc);
                else stack[sp] = kc;
                sp++; pending = true;
            }
        }
        if (pending) continue;
        sp--;
        int idx = out->count++;
        memo[c] = idx;
        out->ops[idx]   = n->op;
        out->data[idx]  = n->data;
        out->nkids[idx] = n->nkids;
        out->kid_off[idx] = nkids_total;
        for (int i = 0; i < n->nkids; i++)
            out->kids[nkids_total++] = memo[eg_find(g, node_kids(g, n)[i])];
    }
    out->root = memo[root];

    free(memo); bbq_vec_free(stack);
    return true;
}

bool eg_extract_excluding(egraph* g, eg_id root, eg_cost_fn cost, void* user,
                          int excl_op, int64_t excl_data,
                          eg_extract_result* out) {
    eg_extract_plan plan;
    if (!eg_extract_prepare(g, cost, user, excl_op, excl_data, &plan)) {
        memset(out, 0, sizeof *out);
        return false;
    }
    bool ok = eg_extract_from(g, &plan, root, out);
    eg_extract_plan_free(&plan);
    return ok;
}

bool eg_extract(egraph* g, eg_id root, eg_cost_fn cost, void* user,
                eg_extract_result* out) {
    /* An op of -1 matches no e-node: every op a consumer interns comes from
     * its own terminal table, and eg_add stores it unchanged. */
    return eg_extract_excluding(g, root, cost, user, -1, 0, out);
}

int eg_extracted_op(const eg_extract_result* r, int node) {
    return r->ops[node];
}

void eg_extract_free(eg_extract_result* r) {
    free(r->ops); free(r->data); free(r->nkids); free(r->kid_off); free(r->kids);
    memset(r, 0, sizeof *r);
}
