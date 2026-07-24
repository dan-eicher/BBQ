/*
 * coverage_main.c — e2e driver for burgc's C backend.
 *
 * The burgc unit tests assert on the GENERATED TEXT. That catches a missing emit; it
 * cannot catch a matcher that compiles, runs, and reports the wrong thing. This driver
 * compiles the generated matcher and runs it. Its twin, coverage_main.cpp, runs the
 * same cases against the C++ backend.
 *
 * THE CONTRACT UNDER TEST. burg_set_error latches the FIRST error and drops later ones,
 * and burg_rewrite SHORT-CIRCUITS on a pending one rather than clearing it. That is what
 * makes a lowered body — which is many burg_rewrite calls over one ctx — abort at its
 * first uncovered statement and still report that failure at the end-of-body check.
 * Clearing on entry instead let the next statement's rewrite swallow the failure, and a
 * truncated body then shipped with its check green.
 *
 * The price is that independent cases are no longer independent: every test below opens
 * with burg_clear_error(ctx), which is exactly the per-call isolation the contract says
 * a caller has to ask for. burg_ctx_init starts clear, so the first one is free.
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
    burg_clear_error(ctx);
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

    burg_clear_error(ctx);
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

    burg_clear_error(ctx);
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
    burg_clear_error(ctx);
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

    burg_clear_error(ctx);
    cov_trace_reset();
    burg_rewrite(&n0, ctx);

    CHECK(burg_has_error(ctx), "uncovered graph node must set the error");
    CHECK(burg_get_error(ctx) != NULL &&
              strstr(burg_get_error(ctx), "does not cover graph node") != NULL,
          "graph-path message must not claim the failure was at the root");
    CHECK(burg_get_error_arg(ctx) == COV_ORPHAN,
          "error arg must carry the offending opcode");
}

/* Half the contract: a pending error makes every later rewrite a no-op, and the FIRST
 * message is the one that survives. A rewrite handed a perfectly coverable tree must
 * still emit nothing — that is what stops a truncated body from looking finished. */
static void test_error_short_circuits_later_rewrites(burg_ctx_t* ctx) {
    burg_clear_error(ctx);
    CovNode o = mk(COV_ORPHAN, 1);
    burg_rewrite(&o, ctx);
    CHECK(burg_has_error(ctx), "precondition: the uncovered rewrite must fail");

    CovNode c = mk(COV_CONST, 1);
    cov_trace_reset();
    burg_rewrite(&c, ctx);

    CHECK(burg_has_error(ctx), "the latched error must survive a later rewrite");
    CHECK(burg_get_error(ctx) != NULL &&
              strstr(burg_get_error(ctx), "no rule at root") != NULL,
          "the FIRST failure's message is the one kept");
    check_trace(NULL, 0, "rewrite after failure emits nothing");
}

/* The other half: isolation is available, it just has to be asked for. This is what a
 * caller does between two independent bodies — and what every test here opens with. */
static void test_explicit_clear_restores(burg_ctx_t* ctx) {
    burg_clear_error(ctx);
    CovNode o = mk(COV_ORPHAN, 1);
    burg_rewrite(&o, ctx);
    CHECK(burg_has_error(ctx), "precondition: the uncovered rewrite must fail");

    burg_clear_error(ctx);
    CovNode c = mk(COV_CONST, 1);
    cov_trace_reset();
    burg_rewrite(&c, ctx);

    CHECK(!burg_has_error(ctx), "an explicit clear must restore the context");
    const int want[] = {1, 10};
    check_trace(want, 2, "rewrite after an explicit clear");
}

/* WHY the contract is this way. A lowered body is many burg_rewrite calls over one ctx
 * with a single check at the end; here statement 2 has no cover. Clearing on entry made
 * statement 3 wipe statement 2's failure, so the end-of-body check read green over a
 * body missing a statement. Short-circuiting makes the failure reach the check, and
 * makes statement 3 emit nothing rather than silently follow a hole. */
static void test_body_of_rewrites_keeps_first_failure(burg_ctx_t* ctx) {
    burg_clear_error(ctx);
    CovNode s1 = mk(COV_CONST, 1);
    CovNode s2 = mk(COV_ORPHAN, 2);
    CovNode s3 = mk(COV_CONST, 3);

    cov_trace_reset();
    burg_rewrite(&s1, ctx);
    burg_rewrite(&s2, ctx);
    burg_rewrite(&s3, ctx);

    CHECK(burg_has_error(ctx), "the body's end-of-body check must see the failure");
    CHECK(burg_get_error_arg(ctx) == COV_ORPHAN,
          "and must see the offending statement's opcode");
    /* Only statement 1 emitted; statement 3 was short-circuited. */
    const int want[] = {1, 10};
    check_trace(want, 2, "body stops at the first uncovered statement");
}

/* A failed rewrite must not leave half-reduced output behind. burg_reduce opens with an
 * error check, so once the uncovered node latches the error every later node on the
 * spine is a no-op call — reduction stops without the loop jumping out of itself. That
 * makes the guard load-bearing rather than decorative. It is the same mechanism the
 * short-circuit uses one level up, applied WITHIN a single rewrite instead of across
 * a body's worth of them. */
static void test_graph_failure_stops_reducing(burg_ctx_t* ctx) {
    burg_clear_error(ctx);
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
    test_error_short_circuits_later_rewrites(&ctx);
    test_explicit_clear_restores(&ctx);
    test_body_of_rewrites_keeps_first_failure(&ctx);
    test_graph_failure_stops_reducing(&ctx);

    burg_ctx_free(&ctx);

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("burgc C rewrite-error e2e: all checks passed\n");
    return 0;
}
