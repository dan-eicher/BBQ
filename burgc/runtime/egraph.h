/*
 * egraph.h — the e-graph a burgc --rewrite matcher saturates over.
 *
 * An e-graph is a set of equivalence classes over terms. An e-node is an
 * operator applied to e-class ids; an e-class is a set of e-nodes known to
 * be equal. Two invariants make it work:
 *
 *   hashcons   the same operator over the same (canonical) children is
 *              always the same e-node, so a term built twice is shared.
 *   congruence equal arguments imply equal applications: once a ≡ b, every
 *              f(a) and f(b) already in the graph are in one class.
 *
 * Merging breaks the second one — a node's children may stop being
 * canonical — so a merge records repair work and eg_rebuild() runs it to a
 * fixpoint. Callers may merge freely and rebuild once.
 *
 * The representation only ever GROWS (Tate et al., §1.3: applying an axiom
 * "does not remove the original program from the representation — it only
 * adds information"). Nothing is deleted, which is what makes rewriting
 * order-independent: no rule can disable another.
 *
 * Saturation therefore has to be bounded from outside. eg_saturate() runs a
 * caller-supplied round to fixpoint or to whichever cap binds first —
 * termination is a parameter, not a hope.
 *
 * Extraction reads a per-operator static cost table and picks, bottom-up,
 * the cheapest node in each class. That is a proxy for the global choice a
 * solver would make (the paper uses Pseudo-Boolean optimisation); on a
 * size-first cost model it is close, because a term's size really is the
 * sum of its parts.
 *
 * The runtime interprets no operator: an op is an opaque int it hashes and
 * compares. All target knowledge stays with the consumer.
 */
#ifndef BURGC_EGRAPH_H
#define BURGC_EGRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An e-class id. Ids are stable once handed out; eg_find maps one to the
 * canonical id of its class, which is what equality compares. */
typedef int eg_id;

typedef struct egraph egraph;

void eg_init(egraph* g);
void eg_free(egraph* g);

/* Intern `op[data](kids[0..nkids))` and return its e-class. Building the
 * same term twice returns the same class and adds no node.
 *
 * `data` distinguishes leaves that share an operator: two locals differ by
 * slot, two constants by value, two opaque nodes by identity, and none of
 * them has children to tell them apart. It is part of the KEY, so those
 * stay distinct — carrying it beside the key instead would let the graph
 * assert that local 0 equals local 1. The runtime never interprets it;
 * `0` is the ordinary case. */
eg_id eg_add(egraph* g, int op, int64_t data, const eg_id* kids, int nkids);

/* The `data` an e-node was interned with — how a rule reads a constant's
 * value without the graph having to know what a constant is. */
int64_t eg_class_node_data(egraph* g, eg_id cls, int i);

/* The canonical id of an e-class. */
eg_id eg_find(egraph* g, eg_id id);

/* Assert a ≡ b. Returns true when the two were distinct, i.e. the graph
 * learned something. Congruence is not restored until eg_rebuild(). */
bool eg_merge(egraph* g, eg_id a, eg_id b);

/* Restore the congruence invariant after any number of merges. */
void eg_rebuild(egraph* g);

/* Total e-nodes, which is what the node budget bounds. */
size_t eg_node_count(const egraph* g);

/* ── Iteration ─────────────────────────────────────────────────────
 * What a generated matcher needs to walk: every class, every e-node in a
 * class, and that node's operator and children. A matcher tries its
 * patterns against each node, so this is the whole of its read interface.
 *
 * `eg_class_count` is an upper bound on ids, not a count of live classes;
 * an id whose eg_find is not itself has been merged away and is skipped. */
int   eg_class_count(const egraph* g);
int   eg_class_nodes(egraph* g, eg_id cls);
int   eg_class_node_op(egraph* g, eg_id cls, int i);
int   eg_class_node_nkids(egraph* g, eg_id cls, int i);
eg_id eg_class_node_kid(egraph* g, eg_id cls, int i, int k);

/* Saturation bounds. Both are caller-supplied: the graph cannot know what
 * a reasonable size is for the consumer's terms. */
typedef struct {
    int rounds;        /* maximum passes over the rule set */
    int node_budget;   /* stop once the graph reaches this many e-nodes */
} eg_caps;

/* One pass over the rule set: match, instantiate, merge. Returns true when
 * it changed the graph. The generated matcher supplies this. */
typedef bool (*eg_round_fn)(egraph* g, void* user);

/* Run `round` until it reports no change, or a cap binds. Rebuilds after
 * each round so a rule sees a congruent graph. Returns rounds executed. */
int eg_saturate(egraph* g, eg_round_fn round, void* user, eg_caps caps);

/* ── Extraction ────────────────────────────────────────────────────
 * The chosen term as a flat array: node i has an op and `nkids` children,
 * each an index into the same array. `root` is the top. */
typedef struct {
    int*     ops;
    int64_t* data;      /* the discriminant node i was interned with */
    int*     kid_off;   /* index into `kids` where node i's children start */
    int*     nkids;
    int*     kids;
    int      count;
    int      root;
} eg_extract_result;

/* Extract the cheapest term for `root`'s class under `cost_by_op`, a table
 * indexed by operator. Returns false when the class has no finite-cost
 * term (every candidate cycles back through itself). */
bool eg_extract(egraph* g, eg_id root,
                const int* cost_by_op, int cost_len,
                eg_extract_result* out);

int  eg_extracted_op(const eg_extract_result* r, int node);
void eg_extract_free(eg_extract_result* r);

/* ── Representation ────────────────────────────────────────────────
 * Exposed so a consumer can place an egraph in its own storage; the
 * fields are the runtime's business. Every array is a bbq_vec — a typed
 * pointer whose length lives in a header behind it — so NULL is a valid
 * empty vector and eg_init is a memset. */
struct eg_node;
struct eg_class;

struct egraph {
    struct eg_node*  nodes;
    eg_id*           kids;      /* every node's children, contiguous */
    struct eg_class* classes;
    eg_id*           parent;    /* union-find */
    eg_id*           worklist;  /* classes needing congruence repair */
    /* hash → first node index with that hash; nodes chain via next_hash.
     * The chain exists because a hash is not an identity: a hit is a
     * candidate to compare, never an answer. */
    void*            hashcons;
};

#ifdef __cplusplus
}
#endif

#endif /* BURGC_EGRAPH_H */
