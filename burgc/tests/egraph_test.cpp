// egraph_test.cpp — the e-graph runtime a --rewrite matcher saturates over.
//
// The four properties the representation has to hold, each red-first:
// hashcons dedup, congruence restored after a merge, saturation stopping at
// its caller-supplied caps, and extraction picking the cheaper of two
// equivalent forms.
extern "C" {
#include "egraph.h"
}
#include <gtest/gtest.h>

namespace {

// A tiny op vocabulary for the tests. The runtime never interprets these —
// an op is an opaque int it hashes and compares.
enum { OP_A = 1, OP_B, OP_F, OP_ADD, OP_MUL, OP_SHL, OP_CONST };

struct Graph {
    egraph g;
    Graph()  { eg_init(&g); }
    ~Graph() { eg_free(&g); }
};

// The op-indexed cost most of these tests want, expressed as what the
// runtime actually takes: a callback over the caller's own table.
static int by_op(int op, int64_t /*data*/, void* user) {
    const int* tbl = (const int*)user;
    return (op >= 0 && op < 64) ? tbl[op] : 1;
}

// Hashconsing: the same op over the same children is the same e-class, so
// building a term twice does not grow the graph.
TEST(EGraph, HashconsDedupsIdenticalNodes) {
    Graph gr;
    eg_id a  = eg_add(&gr.g, OP_A, 0, nullptr, 0);
    eg_id f1 = eg_add(&gr.g, OP_F, 0, &a, 1);
    eg_id f2 = eg_add(&gr.g, OP_F, 0, &a, 1);
    EXPECT_EQ(eg_find(&gr.g, f1), eg_find(&gr.g, f2));
    EXPECT_EQ(eg_node_count(&gr.g), 2u);   // `a` and `f(a)`, nothing more
}

// Arity is the consumer's business, not the runtime's. Two wide nodes that
// differ only in their LAST child must stay distinct — if the kid list were
// capped, both would hashcons to the same node and the e-graph would assert
// an equality that does not hold, silently substituting one term for the
// other at extraction.
TEST(EGraph, WideNodesDifferingInTheLastChildStayDistinct) {
    Graph gr;
    eg_id kids_a[12], kids_b[12];
    for (int i = 0; i < 12; i++) {
        kids_a[i] = eg_add(&gr.g, OP_CONST + i, 0, nullptr, 0);
        kids_b[i] = kids_a[i];
    }
    kids_b[11] = eg_add(&gr.g, OP_CONST + 99, 0, nullptr, 0);

    eg_id wa = eg_add(&gr.g, OP_F, 0, kids_a, 12);
    eg_id wb = eg_add(&gr.g, OP_F, 0, kids_b, 12);
    EXPECT_NE(eg_find(&gr.g, wa), eg_find(&gr.g, wb));

    // And the identical wide term still shares.
    eg_id wa2 = eg_add(&gr.g, OP_F, 0, kids_a, 12);
    EXPECT_EQ(eg_find(&gr.g, wa), eg_find(&gr.g, wa2));
}

// Leaves that share an operator are told apart by their data, not by their
// children — they have none. Local 0 and local 1 are both LoadLocal/0-ary,
// and two constants are both LoadConst/0-ary; without data in the KEY they
// would hashcons together and the graph would assert that different locals
// are the same value. This is the case the calc adoption surfaced.
TEST(EGraph, LeavesSharingAnOperatorAreDistinguishedByData) {
    Graph gr;
    eg_id l0 = eg_add(&gr.g, OP_A, 0, nullptr, 0);
    eg_id l1 = eg_add(&gr.g, OP_A, 1, nullptr, 0);
    EXPECT_NE(eg_find(&gr.g, l0), eg_find(&gr.g, l1));

    // Same operator AND same data is still one class — sharing still works.
    eg_id l0b = eg_add(&gr.g, OP_A, 0, nullptr, 0);
    EXPECT_EQ(eg_find(&gr.g, l0), eg_find(&gr.g, l0b));

    // The value is readable back without the graph interpreting it.
    eg_id c = eg_find(&gr.g, l1);
    ASSERT_EQ(eg_class_nodes(&gr.g, c), 1);
    EXPECT_EQ(eg_class_node_data(&gr.g, c, 0), 1);

    // And data participates in congruence: f(l0) and f(l1) stay apart.
    eg_id fa = eg_add(&gr.g, OP_F, 0, &l0, 1);
    eg_id fb = eg_add(&gr.g, OP_F, 0, &l1, 1);
    EXPECT_NE(eg_find(&gr.g, fa), eg_find(&gr.g, fb));
}

// Congruence: equal arguments make equal applications. After a ≡ b, the
// already-built f(a) and f(b) must land in one class — the merge has to
// re-canonicalise every node whose children moved, transitively.
TEST(EGraph, MergeRestoresCongruence) {
    Graph gr;
    eg_id a  = eg_add(&gr.g, OP_A, 0, nullptr, 0);
    eg_id b  = eg_add(&gr.g, OP_B, 0, nullptr, 0);
    eg_id fa = eg_add(&gr.g, OP_F, 0, &a, 1);
    eg_id fb = eg_add(&gr.g, OP_F, 0, &b, 1);
    ASSERT_NE(eg_find(&gr.g, fa), eg_find(&gr.g, fb));

    eg_merge(&gr.g, a, b);
    eg_rebuild(&gr.g);

    EXPECT_EQ(eg_find(&gr.g, a),  eg_find(&gr.g, b));
    EXPECT_EQ(eg_find(&gr.g, fa), eg_find(&gr.g, fb));
}

// A matcher reads the graph by walking classes and their nodes. After a
// merge the surviving class holds both forms, which is exactly what lets a
// rule see `x*2` and `x<<1` as alternatives of one class.
TEST(EGraph, ClassIterationExposesBothFormsAfterMerge) {
    Graph gr;
    eg_id x = eg_add(&gr.g, OP_A, 0, nullptr, 0);
    eg_id k = eg_add(&gr.g, OP_CONST, 0, nullptr, 0);
    eg_id kids[2] = { x, k };
    eg_id mul = eg_add(&gr.g, OP_MUL, 0, kids, 2);
    eg_id shl = eg_add(&gr.g, OP_SHL, 0, kids, 2);
    eg_merge(&gr.g, mul, shl);
    eg_rebuild(&gr.g);

    eg_id c = eg_find(&gr.g, mul);
    ASSERT_EQ(eg_class_nodes(&gr.g, c), 2);
    bool saw_mul = false, saw_shl = false;
    for (int i = 0; i < eg_class_nodes(&gr.g, c); i++) {
        int op = eg_class_node_op(&gr.g, c, i);
        if (op == OP_MUL) saw_mul = true;
        if (op == OP_SHL) saw_shl = true;
        EXPECT_EQ(eg_class_node_nkids(&gr.g, c, i), 2);
        EXPECT_EQ(eg_class_node_kid(&gr.g, c, i, 0), eg_find(&gr.g, x));
    }
    EXPECT_TRUE(saw_mul);
    EXPECT_TRUE(saw_shl);
}

// Termination is a parameter, not a hope. A round function that always
// reports progress must still stop, at whichever cap binds first.
static bool always_changes(egraph* g, void* user) {
    int* calls = static_cast<int*>(user);
    (*calls)++;
    // Add a fresh node each round so the node budget can also bind.
    eg_add(g, OP_CONST + *calls, 0, nullptr, 0);
    return true;
}

TEST(EGraph, SaturationStopsAtRoundCap) {
    Graph gr;
    int calls = 0;
    eg_caps caps = { /*rounds*/ 5, /*node_budget*/ 1000000 };
    eg_saturate(&gr.g, always_changes, &calls, caps);
    EXPECT_EQ(calls, 5);
}

TEST(EGraph, SaturationStopsAtNodeBudget) {
    Graph gr;
    int calls = 0;
    eg_caps caps = { /*rounds*/ 1000000, /*node_budget*/ 4 };
    eg_saturate(&gr.g, always_changes, &calls, caps);
    EXPECT_LE(eg_node_count(&gr.g), 5u);   // stops as soon as the budget is met
    EXPECT_LT(calls, 1000000);
}

// A round that reports no change ends saturation without burning the cap.
static bool never_changes(egraph*, void* user) {
    (*static_cast<int*>(user))++;
    return false;
}

TEST(EGraph, SaturationStopsWhenNothingChanges) {
    Graph gr;
    int calls = 0;
    eg_caps caps = { 100, 100 };
    eg_saturate(&gr.g, never_changes, &calls, caps);
    EXPECT_EQ(calls, 1);
}

// Extraction: with two equivalent forms in one class, the per-terminal cost
// table decides. `x * 2` and `x << 1` are merged; shl is declared cheaper,
// so the extracted tree is the shl.
TEST(EGraph, ExtractionPicksTheCheaperEquivalentForm) {
    Graph gr;
    eg_id x   = eg_add(&gr.g, OP_A, 0, nullptr, 0);
    eg_id two = eg_add(&gr.g, OP_CONST, 0, nullptr, 0);
    eg_id mul_kids[2] = { x, two };
    eg_id mul = eg_add(&gr.g, OP_MUL, 0, mul_kids, 2);
    eg_id one = eg_add(&gr.g, OP_CONST + 1, 0, nullptr, 0);
    eg_id shl_kids[2] = { x, one };
    eg_id shl = eg_add(&gr.g, OP_SHL, 0, shl_kids, 2);

    eg_merge(&gr.g, mul, shl);
    eg_rebuild(&gr.g);

    // Everything costs 1, but MUL is dear. The table is the caller's; the
    // runtime only calls back.
    int cost[64];
    for (int i = 0; i < 64; i++) cost[i] = 1;
    cost[OP_MUL] = 10;

    eg_extract_result r;
    ASSERT_TRUE(eg_extract(&gr.g, eg_find(&gr.g, mul), by_op, cost, &r));
    EXPECT_EQ(eg_extracted_op(&r, r.root), OP_SHL);

    // And with the costs reversed, the same class yields the mul.
    cost[OP_MUL] = 1;
    cost[OP_SHL] = 10;
    eg_extract_result r2;
    ASSERT_TRUE(eg_extract(&gr.g, eg_find(&gr.g, mul), by_op, cost, &r2));
    EXPECT_EQ(eg_extracted_op(&r2, r2.root), OP_MUL);

    eg_extract_free(&r);
    eg_extract_free(&r2);
}

// A cost that depends on the node's PAYLOAD, not just its operator —
// the case an operator-indexed table cannot express, and the reason
// extraction asks per node. Here a constant costs a byte per magnitude
// step, as a real encoder's immediate does.
TEST(EGraph, CostMayDependOnTheNodePayload) {
    Graph gr;
    eg_id x = eg_add(&gr.g, OP_A, 0, nullptr, 0);
    eg_id big = eg_add(&gr.g, OP_CONST, 1000, nullptr, 0);
    eg_id mk[2] = { x, big };
    eg_id mul = eg_add(&gr.g, OP_MUL, 0, mk, 2);
    eg_id small = eg_add(&gr.g, OP_CONST, 3, nullptr, 0);
    eg_id sk[2] = { x, small };
    eg_id shl = eg_add(&gr.g, OP_SHL, 0, sk, 2);
    eg_merge(&gr.g, mul, shl);
    eg_rebuild(&gr.g);

    // Operators tie at 1 each; only the constants differ, and only by
    // value. An op-indexed table would have to call both the same and
    // could never prefer the shift.
    auto by_value = [](int op, int64_t data, void*) -> int {
        if (op != OP_CONST) return 1;
        int64_t v = data < 0 ? -data : data;
        return v <= 5 ? 1 : (v <= 127 ? 2 : 3);
    };
    eg_extract_result r;
    ASSERT_TRUE(eg_extract(&gr.g, eg_find(&gr.g, mul), by_value, nullptr, &r));
    EXPECT_EQ(eg_extracted_op(&r, r.root), OP_SHL);
    eg_extract_free(&r);
}

// A consumer with let-bindings puts the bound VARIABLE in the same class as
// the expression it names, so a rewrite can see through the binding. The
// binding's own right-hand side then cannot be extracted as that variable —
// `t = t` discards the value — and the variable is the cheapest member of
// the class by construction, so an unguarded extraction always picks it.
TEST(EGraph, ExtractionCanExcludeTheBoundVariable) {
    Graph gr;
    // t = 2 * 3, folded to 6, with `t` itself merged in. OP_A[7] stands for
    // the variable; its data is what tells one variable from another.
    eg_id two = eg_add(&gr.g, OP_CONST, 2, nullptr, 0);
    eg_id three = eg_add(&gr.g, OP_CONST, 3, nullptr, 0);
    eg_id mk[2] = { two, three };
    eg_id mul = eg_add(&gr.g, OP_MUL, 0, mk, 2);
    eg_id six = eg_add(&gr.g, OP_CONST, 6, nullptr, 0);
    eg_id var = eg_add(&gr.g, OP_A, 7, nullptr, 0);
    eg_merge(&gr.g, mul, six);
    eg_merge(&gr.g, mul, var);
    eg_rebuild(&gr.g);

    int cost[64];
    for (int i = 0; i < 64; i++) cost[i] = 1;
    cost[OP_CONST] = 2;    // a constant carries its immediate
    cost[OP_MUL]   = 1;

    // Unguarded, the variable wins on cost — which as the binding's own
    // right-hand side would be `t = t`.
    eg_extract_result r;
    ASSERT_TRUE(eg_extract(&gr.g, eg_find(&gr.g, mul), by_op, cost, &r));
    EXPECT_EQ(eg_extracted_op(&r, r.root), OP_A);
    eg_extract_free(&r);

    // Excluded, the next cheapest is the folded constant — the answer the
    // binding actually wants, and NOT a fallback to the original product.
    eg_extract_result r2;
    ASSERT_TRUE(eg_extract_excluding(&gr.g, eg_find(&gr.g, mul), by_op, cost,
                                     OP_A, 7, &r2));
    EXPECT_EQ(eg_extracted_op(&r2, r2.root), OP_CONST);
    EXPECT_EQ(r2.data[r2.root], 6);
    eg_extract_free(&r2);

    // The exclusion is by (op, data), so a DIFFERENT variable of the same
    // operator is untouched — excluding one binding must not blind the
    // extractor to every other name in scope.
    eg_extract_result r3;
    ASSERT_TRUE(eg_extract_excluding(&gr.g, eg_find(&gr.g, mul), by_op, cost,
                                     OP_A, 9, &r3));
    EXPECT_EQ(eg_extracted_op(&r3, r3.root), OP_A);
    eg_extract_free(&r3);
}

// Exclusion reaches below the root: a term that reached the bound variable
// through any depth of children would be just as circular as naming it
// outright.
TEST(EGraph, ExclusionAppliesBelowTheRoot) {
    Graph gr;
    eg_id var = eg_add(&gr.g, OP_A, 7, nullptr, 0);
    eg_id leaf = eg_add(&gr.g, OP_B, 0, nullptr, 0);
    eg_merge(&gr.g, var, leaf);          // the variable also equals a plain leaf
    eg_rebuild(&gr.g);
    eg_id k[1] = { var };
    eg_id f = eg_add(&gr.g, OP_F, 0, k, 1);

    int cost[64];
    for (int i = 0; i < 64; i++) cost[i] = 1;
    cost[OP_B] = 5;                       // the leaf is dear, the variable cheap

    eg_extract_result r;
    ASSERT_TRUE(eg_extract(&gr.g, eg_find(&gr.g, f), by_op, cost, &r));
    EXPECT_EQ(eg_extracted_op(&r, r.kids[r.kid_off[r.root]]), OP_A);
    eg_extract_free(&r);

    eg_extract_result r2;
    ASSERT_TRUE(eg_extract_excluding(&gr.g, eg_find(&gr.g, f), by_op, cost,
                                     OP_A, 7, &r2));
    EXPECT_EQ(eg_extracted_op(&r2, r2.kids[r2.kid_off[r2.root]]), OP_B);
    eg_extract_free(&r2);
}

// ── E-class analyses (egg §4.1) ──────────────────────────────────
//
// The domain here is the one egg uses to introduce them: "this class is
// a known constant, or it is not". It is a semilattice of finite height,
// so no widening is needed to make these terminate — a domain with
// infinite ascending chains would have to widen inside join.
namespace {
struct KnownConst { bool known; int64_t value; };

void kc_make(egraph* g, int op, int64_t data, const eg_id* kids, int nkids,
             void* out, void*) {
    KnownConst* d = (KnownConst*)out;
    d->known = false;
    d->value = 0;
    if (op == OP_CONST) { d->known = true; d->value = data; return; }
    if (op == OP_ADD && nkids == 2) {
        const KnownConst* a = (const KnownConst*)eg_class_data(g, kids[0]);
        const KnownConst* b = (const KnownConst*)eg_class_data(g, kids[1]);
        if (a && b && a->known && b->known) {
            d->known = true;
            d->value = a->value + b->value;
        }
    }
}

// The strongest fact consistent with both: two e-nodes in one class
// denote the SAME value, so if either is known the class is known.
void kc_join(const void* pa, const void* pb, void* out, void*) {
    const KnownConst* a = (const KnownConst*)pa;
    const KnownConst* b = (const KnownConst*)pb;
    KnownConst* d = (KnownConst*)out;
    *d = a->known ? *a : *b;
}

void kc_modify(egraph* g, eg_id c, const void* pd, void*) {
    const KnownConst* d = (const KnownConst*)pd;
    if (d->known) eg_merge(g, c, eg_add(g, OP_CONST, d->value, nullptr, 0));
}

eg_analysis kc_analysis() {
    eg_analysis a;
    a.size = sizeof(KnownConst);
    a.make = kc_make;
    a.join = kc_join;
    a.modify = kc_modify;
    a.user = nullptr;
    return a;
}
}  // namespace

// make() reads its children's facts, so a fact is computed bottom-up as
// the term is interned — no rule involved.
TEST(EGraph, AnalysisComputesAFactFromChildren) {
    Graph gr;
    eg_analysis a = kc_analysis();
    eg_set_analysis(&gr.g, &a);

    eg_id two = eg_add(&gr.g, OP_CONST, 2, nullptr, 0);
    eg_id three = eg_add(&gr.g, OP_CONST, 3, nullptr, 0);
    eg_id k[2] = { two, three };
    eg_id sum = eg_add(&gr.g, OP_ADD, 0, k, 2);

    const KnownConst* d = (const KnownConst*)eg_class_data(&gr.g, sum);
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->known);
    EXPECT_EQ(d->value, 5);
}

// An opaque leaf has no fact, and neither does anything built over it.
TEST(EGraph, AnalysisStaysUnknownOverAnOpaqueLeaf) {
    Graph gr;
    eg_analysis a = kc_analysis();
    eg_set_analysis(&gr.g, &a);

    eg_id x = eg_add(&gr.g, OP_A, 0, nullptr, 0);
    eg_id two = eg_add(&gr.g, OP_CONST, 2, nullptr, 0);
    eg_id k[2] = { x, two };
    eg_id sum = eg_add(&gr.g, OP_ADD, 0, k, 2);

    EXPECT_FALSE(((const KnownConst*)eg_class_data(&gr.g, sum))->known);
}

// THE POINT OF ANALYSES OVER PER-NODE FACTS: a fact proved about one form
// of a value holds for every form of it. `x` is opaque and `x + 0` says
// nothing; merge it with a known constant and the whole class is known.
TEST(EGraph, AFactFlowsAlongAnEquality) {
    Graph gr;
    eg_analysis a = kc_analysis();
    eg_set_analysis(&gr.g, &a);

    eg_id x = eg_add(&gr.g, OP_A, 0, nullptr, 0);
    eg_id zero = eg_add(&gr.g, OP_CONST, 0, nullptr, 0);
    eg_id k[2] = { x, zero };
    eg_id sum = eg_add(&gr.g, OP_ADD, 0, k, 2);
    EXPECT_FALSE(((const KnownConst*)eg_class_data(&gr.g, sum))->known);

    // Somewhere else, x turns out to be 7.
    eg_merge(&gr.g, x, eg_add(&gr.g, OP_CONST, 7, nullptr, 0));
    eg_rebuild(&gr.g);

    const KnownConst* d = (const KnownConst*)eg_class_data(&gr.g, sum);
    EXPECT_TRUE(d->known);
    EXPECT_EQ(d->value, 7);
}

// modify() is what makes the fact usable downstream: the class gains a
// LoadConst e-node, so extraction can pick it. Idempotent because
// interning a term that exists returns its class.
TEST(EGraph, ModifyAddsTheImpliedConstantAndIsIdempotent) {
    Graph gr;
    eg_analysis a = kc_analysis();
    eg_set_analysis(&gr.g, &a);

    eg_id two = eg_add(&gr.g, OP_CONST, 2, nullptr, 0);
    eg_id three = eg_add(&gr.g, OP_CONST, 3, nullptr, 0);
    eg_id k[2] = { two, three };
    eg_id sum = eg_add(&gr.g, OP_ADD, 0, k, 2);
    eg_rebuild(&gr.g);

    // The class now holds both the sum and the constant 5.
    eg_id c = eg_find(&gr.g, sum);
    bool has_const5 = false;
    for (int i = 0; i < eg_class_nodes(&gr.g, c); i++)
        if (eg_class_node_op(&gr.g, c, i) == OP_CONST &&
            eg_class_node_data(&gr.g, c, i) == 5) has_const5 = true;
    EXPECT_TRUE(has_const5);

    size_t before = eg_node_count(&gr.g);
    eg_rebuild(&gr.g);
    eg_rebuild(&gr.g);
    EXPECT_EQ(eg_node_count(&gr.g), before) << "modify must be idempotent";
}

}  // namespace
