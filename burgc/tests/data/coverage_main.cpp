/*
 * coverage_main.cpp — e2e driver for burgc's C++ backend.
 *
 * The twin of coverage_main.c: same grammar, same nodes, same cases. The C++ backend
 * reports failure by throwing BurgError instead of latching an error on a context, so
 * the assertions differ in mechanism and agree in outcome. Keeping both drivers on one
 * fixture is what makes "the two backends behave the same" a tested claim rather than
 * an assumed one.
 *
 * The outcome both must produce: a lowered body — many burg_rewrite calls with a single
 * check at the end — stops at its first uncovered statement, emits nothing after it, and
 * still reports THAT failure at the check. C++ gets this from unwinding; C had to be
 * taught it (burg_rewrite short-circuits on a pending error rather than clearing it).
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

/* The C twin of test_explicit_clear_restores. A throw carries no state, so C++ gets for
 * free what C asks for with burg_clear_error: past a caught failure, the next body starts
 * clean. Pinned on both backends because it is the OUTCOME that has to match, not the
 * mechanism. */
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

/* The twin of the C driver's test_body_of_rewrites_keeps_first_failure, and the reason
 * the C backend short-circuits instead of clearing on entry. A lowered body is many
 * burg_rewrite calls with ONE check at the end; statement 2 has no cover. C++ has never
 * been able to get this wrong — the throw unwinds past statement 3 and lands in the
 * end-of-body handler — which is precisely the behaviour C had to be fixed to match. */
static void test_body_of_rewrites_keeps_first_failure() {
    BurgMatcher m;
    CovNode s1 = mk(COV_CONST, 1);
    CovNode s2 = mk(COV_ORPHAN, 2);
    CovNode s3 = mk(COV_CONST, 3);

    cov_trace_reset();
    bool threw = false;
    try {
        m.burg_rewrite(&s1);
        m.burg_rewrite(&s2);
        m.burg_rewrite(&s3);
    } catch (const BurgError& e) {
        threw = true;
        CHECK(e.arg == COV_ORPHAN, "the end-of-body handler must see the offending opcode");
    }
    CHECK(threw, "the body's end-of-body handler must see the failure");
    /* Only statement 1 emitted; statement 3 never ran. */
    const int want[] = {1, 10};
    check_trace(want, 2, "body stops at the first uncovered statement");
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
    test_body_of_rewrites_keeps_first_failure();
    test_graph_failure_stops_reducing();

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("burgc C++ rewrite-error e2e: all checks passed\n");
    return 0;
}
