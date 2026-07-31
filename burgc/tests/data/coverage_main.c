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

/* ── The goal-directed entry: burg_label_root + burg_cost ──────────────────────
 *
 * burg_rewrite can only ever ask for the START nonterminal, and the labeled state it
 * builds is private to it, so a consumer could neither (a) read what the DP claims a
 * cover costs nor (b) tile a tree toward some nonterminal OTHER than START. Both are
 * real needs: (a) is how a consumer checks its rule costs against the bytes its actions
 * actually emit, and (b) is how a consumer that knows a tree's CONTEXT — a branch
 * condition, say — asks the matcher for the cover that context wants. burg_label_root
 * returns the labeled state; burg_rule/burg_cost read it; burg_reduce drives it. */

/* Labeling is pure: it computes the DP's accumulated cost and emits nothing. */
static void test_label_root_reports_dp_cost(burg_ctx_t* ctx) {
    CovNode l = mk(COV_CONST, 1);
    CovNode r = mk(COV_CONST, 2);
    CovNode a = mk(COV_ADD, 3);
    a.nkids = 2;
    a.kids[0] = &l;
    a.kids[1] = &r;

    burg_clear_error(ctx);
    cov_trace_reset();
    burg_state_t* s = burg_label_root(&a, ctx);

    CHECK(s != NULL, "label_root must return a state for a coverable tree");
    CHECK(!burg_has_error(ctx), "labeling a covered tree must not report an error");
    check_trace(NULL, 0, "labeling emits nothing — no action runs");
    /* reg: Add(reg,reg) = 1 over two reg: Const = 1 ⇒ 3. stmt: reg = 0 adds nothing. */
    CHECK(burg_cost(s, reg_NT) == 3, "Add(Const,Const) as reg costs 1+1+1");
    CHECK(burg_cost(s, stmt_NT) == 3, "the stmt chain rule adds 0 to it");
    CHECK(burg_rule(s, stmt_NT) != 0, "and stmt has a rule at this root");
}

/* An uncoverable goal reads back as BURG_MAX_COST — the same "no cover" the state
 * itself encodes — never a 0 a caller could mistake for free. */
static void test_cost_of_uncovered_goal(burg_ctx_t* ctx) {
    CovNode o = mk(COV_ORPHAN, 1);

    burg_clear_error(ctx);
    burg_state_t* s = burg_label_root(&o, ctx);

    CHECK(s != NULL, "an Orphan-rooted tree still labels");
    CHECK(burg_cost(s, junk_NT) == 1, "junk: Orphan = 1");
    CHECK(burg_rule(s, stmt_NT) == 0, "nothing chains junk to stmt");
    CHECK(burg_cost(s, stmt_NT) == BURG_MAX_COST, "an uncovered goal costs BURG_MAX_COST");
    CHECK(burg_cost(s, 0) == BURG_MAX_COST, "an out-of-range goal reads as uncovered");
    CHECK(burg_cost(s, BURG_MAX_NT + 1) == BURG_MAX_COST, "…at both ends of the range");
    CHECK(burg_cost(NULL, stmt_NT) == BURG_MAX_COST, "and so does a null state");
}

/* The point of exposing the state: reduce toward a goal that is NOT the start
 * nonterminal. Only that derivation's actions run — the stmt chain rule's does not. */
static void test_label_root_reduce_to_explicit_goal(burg_ctx_t* ctx) {
    CovNode c = mk(COV_CONST, 1);

    burg_clear_error(ctx);
    cov_trace_reset();
    burg_state_t* s = burg_label_root(&c, ctx);
    CHECK(s != NULL && burg_rule(s, reg_NT) != 0, "precondition: reg covers a Const");
    burg_reduce(&c, s, reg_NT, ctx);

    CHECK(!burg_has_error(ctx), "reducing to an explicit goal must succeed");
    const int want[] = {1};
    check_trace(want, 1, "only the reg derivation fires — not stmt: reg");
}

/* label_root obeys the same short-circuit contract as burg_rewrite: over a latched
 * error it is a no-op returning NULL, so a caller cannot label past a failure and
 * then reduce a state that was never computed. */
static void test_label_root_short_circuits_on_pending_error(burg_ctx_t* ctx) {
    burg_clear_error(ctx);
    CovNode o = mk(COV_ORPHAN, 1);
    burg_rewrite(&o, ctx);
    CHECK(burg_has_error(ctx), "precondition: the uncovered rewrite must fail");

    CovNode c = mk(COV_CONST, 1);
    cov_trace_reset();
    CHECK(burg_label_root(&c, ctx) == NULL,
          "label_root must short-circuit on a pending error");
    check_trace(NULL, 0, "and emit nothing");
}

/* ── The exported rule table, used as it is meant to be used ───────────────────
 *
 * The labeler's claim is that its cover is the CHEAPEST the rules admit. Nothing
 * inside the matcher can check that: the DP is the thing under test, and a bug in
 * chain-rule closure produces a cover that is merely legal. So this searches the
 * exported rules directly — no labeler call anywhere below — and the two numbers
 * have to agree.
 *
 * The search is deliberately naive where the labeler is clever: plain recursive
 * minimization with no memo table and no closure precomputation. Agreeing by
 * accident would take both a bug in the DP and the same bug here. */

/* A preorder pattern is matched in two passes, and the ORDER matters: SHAPE
 * first, then the guard, then the children's own minima. A where-clause reads the
 * node's payload union, so evaluating one before its pattern has matched would
 * read the wrong variant's fields — the labeler never does that (each guard sits
 * inside its terminal's switch arm) and neither may this. */
static int oracle_best(CovNode* n, int nt, burg_ctx_t* ctx, int fuel);

static int oracle_shape(CovNode* n, const burg_pat_node_t* p, int* i) {
    const burg_pat_node_t* here = &p[(*i)++];
    if (!here->is_term) return 1;        /* a nonterminal leaf accepts any node */
    if (n->op != here->sym || n->nkids < here->nkids) return 0;
    for (int k = 0; k < here->nkids; k++)
        if (!oracle_shape(n->kids[k], p, i)) return 0;
    return 1;
}

static int oracle_kids(CovNode* n, const burg_pat_node_t* p, int* i,
                       burg_ctx_t* ctx, int fuel, int* acc) {
    const burg_pat_node_t* here = &p[(*i)++];
    if (!here->is_term) {
        int c = oracle_best(n, here->sym, ctx, fuel - 1);
        if (c < 0) return 0;
        *acc += c;
        return 1;
    }
    for (int k = 0; k < here->nkids; k++)
        if (!oracle_kids(n->kids[k], p, i, ctx, fuel, acc)) return 0;
    return 1;
}

static int oracle_best(CovNode* n, int nt, burg_ctx_t* ctx, int fuel) {
    if (fuel <= 0) return -1;            /* bounds chain-rule cycles */
    int lo = -1;
    for (int r = 0; r < burg_rule_table_len; r++) {
        const burg_rule_row_t* row = &burg_rule_table[r];
        if (row->nonterm != nt) continue;
        int i = 0;
        if (!oracle_shape(n, row->pat, &i)) continue;
        if (!burg_rule_guard(row->rule, n, ctx)) continue;
        i = 0;
        int acc = 0;
        if (!oracle_kids(n, row->pat, &i, ctx, fuel, &acc)) continue;
        /* Variable arity, mirroring the matcher exactly: children beyond the
         * declared pattern are covered with START, so they cost cost[START].
         * (iburg p.4 — a cover's cost sums EVERY child through some nonterminal;
         * the extras' nonterminal is START because that is how they reduce.) */
        if (row->pat[0].is_term) {
            int bad = 0;
            for (int k = row->pat[0].nkids; k < n->nkids; k++) {
                int c = oracle_best(n->kids[k], stmt_NT, ctx, fuel - 1);
                if (c < 0) { bad = 1; break; }
                acc += c;
            }
            if (bad) continue;
        }
        int c = row->cost + acc;
        if (lo < 0 || c < lo) lo = c;
    }
    return lo;
}

/* The table must describe the grammar it was generated from. Checked against
 * coverage.burg's four rules by hand, so a pattern encoded wrongly (a chain rule
 * emitted as a terminal, say) fails here rather than silently making the oracle
 * agree with the labeler about nothing. */
static void test_rule_table_describes_the_grammar(burg_ctx_t* ctx) {
    (void)ctx;
    CHECK(burg_rule_table_len == 5, "coverage.burg has five rules");
    /* reg: Call = 3 — a leaf-terminal rule over a VARIADIC node: npat 1, zero
     * declared children. The extras exist only on the subject node. */
    CHECK(burg_rule_table[4].nonterm == reg_NT && burg_rule_table[4].cost == 3 &&
              burg_rule_table[4].npat == 1 && burg_rule_table[4].pat[0].is_term &&
              burg_rule_table[4].pat[0].sym == COV_CALL &&
              burg_rule_table[4].pat[0].nkids == 0,
          "row 4 is reg: Call = 3 with no declared children");
    /* reg: Const = 1 — a bare terminal, no children. */
    CHECK(burg_rule_table[0].nonterm == reg_NT && burg_rule_table[0].cost == 1 &&
              burg_rule_table[0].npat == 1 && burg_rule_table[0].pat[0].is_term &&
              burg_rule_table[0].pat[0].sym == COV_CONST &&
              burg_rule_table[0].pat[0].nkids == 0,
          "row 0 is reg: Const = 1");
    /* reg: Add(reg, reg) = 1 — a terminal with two nonterminal leaves. */
    CHECK(burg_rule_table[1].npat == 3 && burg_rule_table[1].pat[0].is_term &&
              burg_rule_table[1].pat[0].sym == COV_ADD &&
              burg_rule_table[1].pat[0].nkids == 2 &&
              !burg_rule_table[1].pat[1].is_term && burg_rule_table[1].pat[1].sym == reg_NT &&
              !burg_rule_table[1].pat[2].is_term && burg_rule_table[1].pat[2].sym == reg_NT,
          "row 1 is reg: Add(reg, reg), children encoded as nonterminal leaves");
    /* stmt: reg = 0 — a chain rule: one nonterminal leaf, no terminal at all. */
    CHECK(burg_rule_table[2].nonterm == stmt_NT && burg_rule_table[2].cost == 0 &&
              burg_rule_table[2].npat == 1 && !burg_rule_table[2].pat[0].is_term &&
              burg_rule_table[2].pat[0].sym == reg_NT,
          "row 2 is the chain rule stmt: reg = 0");
    for (int r = 0; r < burg_rule_table_len; r++)
        CHECK(burg_rule_table[r].guarded == 0, "coverage.burg has no where-clauses");
}

/* Variable arity, both halves of the contract. A Call node carries children its
 * rule never declared; the matcher reduces them with START, so it must COST them
 * with START — otherwise the root's cost[start] is not the cover's cost and the
 * DP is minimizing over covers of a smaller tree than it emits (iburg p.4, p.7).
 *
 * Call(Const, Const): reg = 3 (Call) + 1 + 1 (each Const as stmt via the 0-cost
 * chain) = 5; the stmt chain adds 0. Reduction emits both children first, each
 * through its full stmt derivation, then the Call's own action, then the chain's:
 * {1,10, 1,10, 4, 10}. */
static void test_variadic_children_are_costed(burg_ctx_t* ctx) {
    CovNode l = mk(COV_CONST, 1);
    CovNode r = mk(COV_CONST, 2);
    CovNode call = mk(COV_CALL, 3);
    call.nkids = 2;
    call.kids[0] = &l;
    call.kids[1] = &r;

    burg_clear_error(ctx);
    cov_trace_reset();
    burg_state_t* s = burg_label_root(&call, ctx);

    CHECK(s != NULL, "a variadic node labels");
    check_trace(NULL, 0, "labeling emits nothing");
    CHECK(burg_cost(s, reg_NT) == 5,
          "Call(Const,Const) as reg costs 3 + the children's START covers (1+1)");
    CHECK(burg_cost(s, stmt_NT) == 5, "the stmt chain adds 0 to it");

    cov_trace_reset();
    burg_rewrite(&call, ctx);
    CHECK(!burg_has_error(ctx), "the variadic rewrite succeeds");
    const int want[] = {1, 10, 1, 10, 4, 10};
    check_trace(want, 6, "children reduce (with START) before the Call's action");

    /* A childless Call is just its own rule — the loop's base case. */
    CovNode bare = mk(COV_CALL, 4);
    burg_clear_error(ctx);
    burg_state_t* s0 = burg_label_root(&bare, ctx);
    CHECK(burg_cost(s0, reg_NT) == 3, "a nullary Call costs its rule alone");
}

/* The labeler's cover must cost what an independent search says is cheapest. */
static void test_labeler_agrees_with_an_independent_search(burg_ctx_t* ctx) {
    CovNode c  = mk(COV_CONST, 1);
    CovNode l  = mk(COV_CONST, 2);
    CovNode r  = mk(COV_CONST, 3);
    CovNode a  = mk(COV_ADD, 4);   a.nkids = 2;  a.kids[0] = &l;  a.kids[1] = &r;
    CovNode l2 = mk(COV_CONST, 5);
    CovNode a2 = mk(COV_ADD, 6);   a2.nkids = 2; a2.kids[0] = &a; a2.kids[1] = &l2;
    CovNode o  = mk(COV_ORPHAN, 7);

    CovNode ca = mk(COV_CALL, 8);          /* variadic: the oracle must agree here too */
    ca.nkids = 2; ca.kids[0] = &c; ca.kids[1] = &a;
    struct { CovNode* n; const char* what; } cases[] = {
        {&c,  "Const"}, {&a,  "Add(Const,Const)"}, {&a2, "Add(Add(..),Const)"},
        {&ca, "Call(Const, Add(..))"},
    };
    for (int i = 0; i < 4; i++) {
        burg_clear_error(ctx);
        burg_state_t* s = burg_label_root(cases[i].n, ctx);
        int labeled = burg_cost(s, stmt_NT);
        int searched = oracle_best(cases[i].n, stmt_NT, ctx, 32);
        if (labeled != searched) {
            printf("FAIL [%s]: labeler says %d, independent search says %d\n",
                   cases[i].what, labeled, searched);
            failures++;
        }
    }
    /* And they must agree that the uncoverable root has no derivation at all. */
    burg_clear_error(ctx);
    burg_state_t* s = burg_label_root(&o, ctx);
    CHECK(burg_rule(s, stmt_NT) == 0 && oracle_best(&o, stmt_NT, ctx, 32) < 0,
          "both agree an Orphan root has no stmt derivation");
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
    test_label_root_reports_dp_cost(&ctx);
    test_cost_of_uncovered_goal(&ctx);
    test_label_root_reduce_to_explicit_goal(&ctx);
    test_label_root_short_circuits_on_pending_error(&ctx);
    test_rule_table_describes_the_grammar(&ctx);
    test_variadic_children_are_costed(&ctx);
    test_labeler_agrees_with_an_independent_search(&ctx);

    burg_ctx_free(&ctx);

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("burgc C rewrite-error e2e: all checks passed\n");
    return 0;
}
