/*
 * coverage_main.c — e2e driver for burgc's C backend.
 *
 * The burgc unit tests assert on the GENERATED TEXT. That catches a missing emit; it
 * cannot catch a matcher that compiles, runs, and reports the wrong thing — which is
 * what the C backend did: it emitted a burg_clear_error() nobody called, so the first
 * uncovered root latched an error that every later rewrite kept reporting.
 *
 * This driver compiles the generated matcher and runs it. Its twin, coverage_main.cpp,
 * runs the same cases against the C++ backend.
 */
#include <stdio.h>
#include <string.h>

#include "coverage_node.h"
#include "coverage_burg.h"

static int failures = 0;

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL [%s]: %s\n", __func__, (what));                   \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void check_trace(const int* want, int n, const char* what) {
    if (cov_trace_len != n) {
        printf("FAIL [%s]: trace length %d, want %d\n", what, cov_trace_len, n);
        failures++;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (cov_trace[i] != want[i]) {
            printf("FAIL [%s]: trace[%d] = %d, want %d\n", what, i, cov_trace[i], want[i]);
            failures++;
            return;
        }
    }
}

static CovNode mk(int op, int id) {
    CovNode n;
    memset(&n, 0, sizeof(n));
    n.op = op;
    n.id = id;
    return n;
}

/* A Const-rooted tree: no successor edges, so burg_rewrite takes the fast path. The
 * chain rule stmt:reg reduces the reg derivation before firing its own action. */
static void test_covered_tree(burg_ctx_t* ctx) {
    CovNode c = mk(COV_CONST, 1);
    cov_trace_reset();
    burg_rewrite(&c, ctx);

    CHECK(!burg_has_error(ctx), "covered tree must not report an error");
    const int want[] = {1, 10};
    check_trace(want, 2, "covered tree");
}

/* Add(Const, Const): still the fast path, but exercises a real tile — children reduce
 * postorder before the parent. */
static void test_covered_tile(burg_ctx_t* ctx) {
    CovNode l = mk(COV_CONST, 1);
    CovNode r = mk(COV_CONST, 2);
    CovNode a = mk(COV_ADD, 3);
    a.nkids = 2;
    a.kids[0] = &l;
    a.kids[1] = &r;

    cov_trace_reset();
    burg_rewrite(&a, ctx);

    CHECK(!burg_has_error(ctx), "covered tile must not report an error");
    const int want[] = {1, 1, 2, 10};
    check_trace(want, 4, "covered tile");
}

/* Two Const statements on a spine. A successor edge forces the RPO path, so both nodes
 * are reduced as start-nonterminal roots. */
static void test_covered_graph(burg_ctx_t* ctx) {
    CovNode n1 = mk(COV_CONST, 2);
    CovNode n0 = mk(COV_CONST, 1);
    n0.nsucc = 1;
    n0.succs[0] = &n1;

    cov_trace_reset();
    burg_rewrite(&n0, ctx);

    CHECK(!burg_has_error(ctx), "covered graph must not report an error");
    const int want[] = {1, 10, 1, 10};
    check_trace(want, 4, "covered graph");
}

/* THE case the silent skip hid. An Orphan root labels fine and derives `junk`, which
 * nothing chains to stmt — so there is no rule at the root and nothing to emit. Before
 * the fix burg_rewrite simply returned, and the consumer read uninitialised output. */
static void test_uncovered_tree_root(burg_ctx_t* ctx) {
    CovNode o = mk(COV_ORPHAN, 1);
    cov_trace_reset();
    burg_rewrite(&o, ctx);

    CHECK(burg_has_error(ctx), "uncovered root must set the error");
    CHECK(burg_get_error(ctx) != NULL &&
              strstr(burg_get_error(ctx), "no rule at root") != NULL,
          "uncovered root message must name the root case");
    CHECK(burg_get_error_arg(ctx) == COV_ORPHAN,
          "error arg must carry the offending opcode");
}

/* Same failure, reached through the RPO loop rather than the fast path. The message
 * must NOT claim "at root" — this node is a spine node, not the root. */
static void test_uncovered_graph_node(burg_ctx_t* ctx) {
    CovNode bad = mk(COV_ORPHAN, 2);
    CovNode n0 = mk(COV_CONST, 1);
    n0.nsucc = 1;
    n0.succs[0] = &bad;

    cov_trace_reset();
    burg_rewrite(&n0, ctx);

    CHECK(burg_has_error(ctx), "uncovered graph node must set the error");
    CHECK(burg_get_error(ctx) != NULL &&
              strstr(burg_get_error(ctx), "does not cover graph node") != NULL,
          "graph-path message must not claim the failure was at the root");
    CHECK(burg_get_error_arg(ctx) == COV_ORPHAN,
          "error arg must carry the offending opcode");
}

/* THE regression. burg_set_error latches the first error and drops the rest, so a
 * rewrite that does not clear on entry reports a PREVIOUS call's failure forever. Every
 * rewrite after the first failure looked broken, whatever it was handed. */
static void test_error_does_not_leak_into_next_rewrite(burg_ctx_t* ctx) {
    CovNode o = mk(COV_ORPHAN, 1);
    burg_rewrite(&o, ctx);
    CHECK(burg_has_error(ctx), "precondition: the uncovered rewrite must fail");

    CovNode c = mk(COV_CONST, 1);
    cov_trace_reset();
    burg_rewrite(&c, ctx);

    CHECK(!burg_has_error(ctx),
          "a successful rewrite must not report the previous rewrite's error");
    const int want[] = {1, 10};
    check_trace(want, 2, "rewrite after failure");
}

/* A failed rewrite must not leave half-reduced output behind. burg_reduce opens with an
 * error check, so once the uncovered node latches the error every later node on the
 * spine is a no-op call — reduction stops without the loop jumping out of itself. That
 * makes the guard load-bearing rather than decorative, which is worth pinning: it is
 * also the reason a rewrite that fails to CLEAR the error emits nothing at all. */
static void test_graph_failure_stops_reducing(burg_ctx_t* ctx) {
    CovNode tail = mk(COV_CONST, 3);
    CovNode bad = mk(COV_ORPHAN, 2);
    bad.nsucc = 1;
    bad.succs[0] = &tail;
    CovNode n0 = mk(COV_CONST, 1);
    n0.nsucc = 1;
    n0.succs[0] = &bad;

    cov_trace_reset();
    burg_rewrite(&n0, ctx);

    CHECK(burg_has_error(ctx), "uncovered spine node must set the error");
    /* n0 reduces (1, 10), then Orphan fails and the loop stops — `tail` never fires. */
    const int want[] = {1, 10};
    check_trace(want, 2, "graph failure stops at the first uncovered node");
}

int main(void) {
    burg_ctx_t ctx;
    burg_ctx_init(&ctx);

    test_covered_tree(&ctx);
    test_covered_tile(&ctx);
    test_covered_graph(&ctx);
    test_uncovered_tree_root(&ctx);
    test_uncovered_graph_node(&ctx);
    test_error_does_not_leak_into_next_rewrite(&ctx);
    test_graph_failure_stops_reducing(&ctx);

    burg_ctx_free(&ctx);

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("burgc C rewrite-error e2e: all checks passed\n");
    return 0;
}
