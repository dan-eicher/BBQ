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

/* ── E-class analyses ──────────────────────────────────────────────
 * egg §4.1 (Willsey et al., POPL 2021), whose interface this is.
 *
 * A rewrite rule can only say that two terms are EQUAL. It cannot say
 * that a value lies in [0, 255], which is what a width-narrowing or a
 * bounds-check elimination needs — those are abstract interpretation,
 * not rewriting. An e-class analysis attaches a fact from a domain D to
 * each e-class and maintains egg's invariant:
 *
 *     ∀c ∈ G .  d_c = ⋀ make(n)  for n ∈ c,  and  modify(c) = c
 *
 * i.e. a class's fact is the join of `make` over every e-node in it, and
 * `modify`'s additions are driven to a fixed point.
 *
 * The payoff over a per-node analysis is that facts flow ALONG
 * equalities: when two e-nodes are known equal, the class carries the
 * strongest fact consistent with both, so proving something about one
 * form of a value sharpens every other form of it.
 *
 * TERMINATION IS THE CALLER'S. egg requires a join-semilattice and says
 * nothing about chain height; a domain with infinite ascending chains
 * (intervals: [0,1] ⊑ [0,2] ⊑ …) will not stabilise here any more than
 * under Kleene iteration, because saturation IS that iteration. Such a
 * domain must widen inside `join`.
 *
 * D is a fixed-size blob the runtime never interprets. Its bytes are
 * zeroed before every `make` and `join`, so the runtime can compare two
 * facts with memcmp without padding making equal facts look different. */
typedef struct {
    size_t size;                  /* bytes of analysis data per e-class */

    /* The fact for an e-node, read as if it alone were in its class. A
     * child's fact is available through eg_class_data. */
    void (*make)(egraph* g, int op, int64_t data,
                 const eg_id* kids, int nkids, void* out, void* user);

    /* Combine the facts of two classes being merged, or of a class and
     * one of its members. Must be commutative, associative and
     * idempotent, and must widen if D has infinite ascending chains. */
    void (*join)(const void* a, const void* b, void* out, void* user);

    /* Optionally add an e-node implied by the fact — a constant, say,
     * once the fact says the class is that constant. Must be idempotent;
     * interning an existing term already is, so adding through eg_add
     * satisfies this for free. May be NULL. */
    void (*modify)(egraph* g, eg_id c, const void* d, void* user);

    void* user;
} eg_analysis;

/* Install an analysis. Call before the first eg_add: facts are computed
 * as nodes are added, and a class created earlier would have none. The
 * struct is copied. */
void eg_set_analysis(egraph* g, const eg_analysis* a);

/* The fact for `c`, or NULL when no analysis is installed. Valid until
 * the next merge or rebuild. */
void* eg_class_data(egraph* g, eg_id c);

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

/* What one e-node costs, asked of the consumer per NODE rather than read
 * from a table indexed by operator.
 *
 * Per-node because a cost is frequently not a property of the operator
 * alone: a target that encodes a small constant in one byte and a large
 * one in three prices `Const` by its VALUE, and an operator-indexed table
 * has to pick an average that is wrong in both directions. Under a
 * size-first model that average is the difference between choosing a
 * shift and choosing a multiply. The runtime still interprets nothing —
 * `op` and `data` are the consumer's own, handed straight back.
 *
 * Costs are summed over the chosen term, so they must be non-negative;
 * a negative one would make a cycle look cheap. */
typedef int (*eg_cost_fn)(int op, int64_t data, void* user);

/* Extract the cheapest term for `root`'s class under `cost`. Returns false
 * when the class has no finite-cost term (every candidate cycles back
 * through itself). */
bool eg_extract(egraph* g, eg_id root,
                eg_cost_fn cost, void* user,
                eg_extract_result* out);

/* As eg_extract, but the e-node `excl_op[excl_data]` is not a candidate.
 *
 * WHY A CALLER NEEDS THIS. A consumer whose IR has let-bindings puts the
 * bound variable in the SAME class as the expression it is bound to —
 * that is what lets a rewrite see through the binding, and it is pure
 * added information. But the binding's own right-hand side may not then
 * be extracted AS that variable: `t = t` is not a definition of t, it
 * discards the value. The variable is the cheapest member of the class
 * almost by construction (a name is smaller than what it names), so
 * without an exclusion the binding could never be rewritten at all.
 *
 * Excluded EVERYWHERE in the result, not merely at the root: a term that
 * reached the bound variable through any depth of children would be just
 * as circular. Extraction still interprets nothing — it compares an int
 * and an int64, both of which the consumer chose. */
bool eg_extract_excluding(egraph* g, eg_id root,
                          eg_cost_fn cost, void* user,
                          int excl_op, int64_t excl_data,
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
    /* The installed e-class analysis, or NULL. `class_data` is a bbq_vec
     * of bytes holding analysis->size per class, indexed by class id. */
    eg_analysis*     analysis;
    unsigned char*   class_data;
    /* hash → first node index with that hash; nodes chain via next_hash.
     * The chain exists because a hash is not an identity: a hit is a
     * candidate to compare, never an answer. */
    void*            hashcons;
};

#ifdef __cplusplus
}
#endif

#endif /* BURGC_EGRAPH_H */
