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

    // Cost by op: everything 1, but MUL is dear.
    int cost[64];
    for (int i = 0; i < 64; i++) cost[i] = 1;
    cost[OP_MUL] = 10;

    eg_extract_result r;
    ASSERT_TRUE(eg_extract(&gr.g, eg_find(&gr.g, mul), cost, 64, &r));
    EXPECT_EQ(eg_extracted_op(&r, r.root), OP_SHL);

    // And with the costs reversed, the same class yields the mul.
    cost[OP_MUL] = 1;
    cost[OP_SHL] = 10;
    eg_extract_result r2;
    ASSERT_TRUE(eg_extract(&gr.g, eg_find(&gr.g, mul), cost, 64, &r2));
    EXPECT_EQ(eg_extracted_op(&r2, r2.root), OP_MUL);

    eg_extract_free(&r);
    eg_extract_free(&r2);
}

}  // namespace
