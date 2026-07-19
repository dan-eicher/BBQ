/*
 * coverage_main.cpp — e2e driver for burgc's C++ backend.
 *
 * The twin of coverage_main.c: same grammar, same nodes, same cases. The C++ backend
 * reports failure by throwing BurgError instead of latching an error on a context, so
 * the assertions differ in mechanism and agree in outcome. Keeping both drivers on one
 * fixture is what makes "the two backends behave the same" a tested claim rather than
 * an assumed one.
 */
#include <cstdio>
#include <cstring>
#include <string>

#include "coverage_node.h"
#include "coverage_matcher.h"

static int failures = 0;

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL [%s]: %s\n", __func__, (what));              \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void check_trace(const int* want, int n, const char* what) {
    if (cov_trace_len != n) {
        std::printf("FAIL [%s]: trace length %d, want %d\n", what, cov_trace_len, n);
        failures++;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (cov_trace[i] != want[i]) {
            std::printf("FAIL [%s]: trace[%d] = %d, want %d\n", what, i, cov_trace[i], want[i]);
            failures++;
            return;
        }
    }
}

static CovNode mk(int op, int id) {
    CovNode n;
    std::memset(&n, 0, sizeof(n));
    n.op = op;
    n.id = id;
    return n;
}

static void test_covered_tree() {
    BurgMatcher m;
    CovNode c = mk(COV_CONST, 1);
    cov_trace_reset();
    try {
        m.burg_rewrite(&c);
    } catch (const BurgError& e) {
        CHECK(false, "covered tree must not throw");
        return;
    }
    const int want[] = {1, 10};
    check_trace(want, 2, "covered tree");
}

static void test_covered_tile() {
    BurgMatcher m;
    CovNode l = mk(COV_CONST, 1);
    CovNode r = mk(COV_CONST, 2);
    CovNode a = mk(COV_ADD, 3);
    a.nkids = 2;
    a.kids[0] = &l;
    a.kids[1] = &r;

    cov_trace_reset();
    try {
        m.burg_rewrite(&a);
    } catch (const BurgError&) {
        CHECK(false, "covered tile must not throw");
        return;
    }
    const int want[] = {1, 1, 2, 10};
    check_trace(want, 4, "covered tile");
}

static void test_covered_graph() {
    BurgMatcher m;
    CovNode n1 = mk(COV_CONST, 2);
    CovNode n0 = mk(COV_CONST, 1);
    n0.nsucc = 1;
    n0.succs[0] = &n1;

    cov_trace_reset();
    try {
        m.burg_rewrite(&n0);
    } catch (const BurgError&) {
        CHECK(false, "covered graph must not throw");
        return;
    }
    const int want[] = {1, 10, 1, 10};
    check_trace(want, 4, "covered graph");
}

/* An uncovered root must throw rather than return quietly — the C backend's
 * burg_set_error and this throw are the same decision on two backends. */
static void test_uncovered_tree_root() {
    BurgMatcher m;
    CovNode o = mk(COV_ORPHAN, 1);
    cov_trace_reset();
    bool threw = false;
    try {
        m.burg_rewrite(&o);
    } catch (const BurgError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("no rule at root") != std::string::npos,
              "uncovered root message must name the root case");
        CHECK(e.arg == COV_ORPHAN, "BurgError::arg must carry the offending opcode");
    }
    CHECK(threw, "uncovered root must throw BurgError");
}

static void test_uncovered_graph_node() {
    BurgMatcher m;
    CovNode bad = mk(COV_ORPHAN, 2);
    CovNode n0 = mk(COV_CONST, 1);
    n0.nsucc = 1;
    n0.succs[0] = &bad;

    cov_trace_reset();
    bool threw = false;
    try {
        m.burg_rewrite(&n0);
    } catch (const BurgError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("does not cover graph node") != std::string::npos,
              "graph-path message must not claim the failure was at the root");
        CHECK(e.arg == COV_ORPHAN, "BurgError::arg must carry the offending opcode");
    }
    CHECK(threw, "uncovered graph node must throw BurgError");
}

/* The C backend's sticky-error bug has no C++ analogue — a throw carries no state — but
 * the OUTCOME being asserted is identical, so the case is worth pinning on both: a
 * matcher reused after a failure must behave as if the failure never happened. */
static void test_matcher_reusable_after_failure() {
    BurgMatcher m;
    CovNode o = mk(COV_ORPHAN, 1);
    try {
        m.burg_rewrite(&o);
        CHECK(false, "precondition: the uncovered rewrite must throw");
    } catch (const BurgError&) {
    }

    CovNode c = mk(COV_CONST, 1);
    cov_trace_reset();
    try {
        m.burg_rewrite(&c);
    } catch (const BurgError&) {
        CHECK(false, "a matcher reused after a failure must succeed");
        return;
    }
    const int want[] = {1, 10};
    check_trace(want, 2, "rewrite after failure");
}

/* The throw unwinds out of the RPO loop, so nothing downstream of the uncovered node
 * reduces. The C backend reaches the same stopping point by a different route — its
 * burg_reduce short-circuits on the latched error — and this asserts the two agree. */
static void test_graph_failure_stops_reducing() {
    BurgMatcher m;
    CovNode tail = mk(COV_CONST, 3);
    CovNode bad = mk(COV_ORPHAN, 2);
    bad.nsucc = 1;
    bad.succs[0] = &tail;
    CovNode n0 = mk(COV_CONST, 1);
    n0.nsucc = 1;
    n0.succs[0] = &bad;

    cov_trace_reset();
    bool threw = false;
    try {
        m.burg_rewrite(&n0);
    } catch (const BurgError&) {
        threw = true;
    }
    CHECK(threw, "uncovered spine node must throw BurgError");
    const int want[] = {1, 10};
    check_trace(want, 2, "graph failure stops at the first uncovered node");
}

int main() {
    test_covered_tree();
    test_covered_tile();
    test_covered_graph();
    test_uncovered_tree_root();
    test_uncovered_graph_node();
    test_matcher_reusable_after_failure();
    test_graph_failure_stops_reducing();

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("burgc C++ rewrite-error e2e: all checks passed\n");
    return 0;
}
