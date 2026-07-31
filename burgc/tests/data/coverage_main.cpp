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

/* ── The goal-directed entry: burg_label_root + burg_cost ──────────────────────
 * The twin of coverage_main.c's block. burg_rewrite only ever asks for the START
 * nonterminal and keeps its labeled state to itself; these expose it, so a consumer
 * can read what the DP claims a cover costs and can tile a tree toward a nonterminal
 * its CONTEXT names instead of START. Same cases, same numbers, both backends. */

static void test_label_root_reports_dp_cost() {
    BurgMatcher m;
    CovNode l = mk(COV_CONST, 1);
    CovNode r = mk(COV_CONST, 2);
    CovNode a = mk(COV_ADD, 3);
    a.nkids = 2;
    a.kids[0] = &l;
    a.kids[1] = &r;

    cov_trace_reset();
    BurgMatcher::BurgState* s = m.burg_label_root(&a);

    CHECK(s != nullptr, "label_root must return a state for a coverable tree");
    check_trace(nullptr, 0, "labeling emits nothing — no action runs");
    /* reg: Add(reg,reg) = 1 over two reg: Const = 1 ⇒ 3. stmt: reg = 0 adds nothing. */
    CHECK(BurgMatcher::burg_cost(s, reg_NT) == 3, "Add(Const,Const) as reg costs 1+1+1");
    CHECK(BurgMatcher::burg_cost(s, stmt_NT) == 3, "the stmt chain rule adds 0 to it");
    CHECK(BurgMatcher::burg_rule(s, stmt_NT) != 0, "and stmt has a rule at this root");
}

static void test_cost_of_uncovered_goal() {
    BurgMatcher m;
    CovNode o = mk(COV_ORPHAN, 1);

    BurgMatcher::BurgState* s = m.burg_label_root(&o);

    CHECK(s != nullptr, "an Orphan-rooted tree still labels");
    CHECK(BurgMatcher::burg_cost(s, junk_NT) == 1, "junk: Orphan = 1");
    CHECK(BurgMatcher::burg_rule(s, stmt_NT) == 0, "nothing chains junk to stmt");
    CHECK(BurgMatcher::burg_cost(s, stmt_NT) == BURG_MAX_COST,
          "an uncovered goal costs BURG_MAX_COST");
    CHECK(BurgMatcher::burg_cost(s, 0) == BURG_MAX_COST,
          "an out-of-range goal reads as uncovered");
    CHECK(BurgMatcher::burg_cost(s, BURG_MAX_NT + 1) == BURG_MAX_COST,
          "…at both ends of the range");
    CHECK(BurgMatcher::burg_cost(nullptr, stmt_NT) == BURG_MAX_COST,
          "and so does a null state");
}

static void test_label_root_reduce_to_explicit_goal() {
    BurgMatcher m;
    CovNode c = mk(COV_CONST, 1);

    cov_trace_reset();
    BurgMatcher::BurgState* s = m.burg_label_root(&c);
    CHECK(s != nullptr && BurgMatcher::burg_rule(s, reg_NT) != 0,
          "precondition: reg covers a Const");
    m.burg_reduce(&c, s, reg_NT);

    const int want[] = {1};
    check_trace(want, 1, "only the reg derivation fires — not stmt: reg");
}

/* ── The exported rule table ───────────────────────────────────────────────────
 * The twin of coverage_main.c's oracle. The labeler claims its cover is the
 * cheapest the rules admit; only a search that never calls the labeler can check
 * that. Naive recursive minimization over the exported rows, no memo table — a
 * bug would have to appear identically in the DP and here to go unnoticed. */

/* SHAPE first, then the guard, then the children's minima — a where-clause reads
 * the node's payload union, so it must not run before its pattern has matched. */
static int oracle_best(CovNode* n, int nt, int fuel);

static int oracle_shape(CovNode* n, const BurgMatcher::burg_pat_node_t* p, int* i) {
    const BurgMatcher::burg_pat_node_t* here = &p[(*i)++];
    if (!here->is_term) return 1;
    if (n->op != here->sym || n->nkids < here->nkids) return 0;
    for (int k = 0; k < here->nkids; k++)
        if (!oracle_shape(n->kids[k], p, i)) return 0;
    return 1;
}

static int oracle_kids(CovNode* n, const BurgMatcher::burg_pat_node_t* p, int* i,
                       int fuel, int* acc) {
    const BurgMatcher::burg_pat_node_t* here = &p[(*i)++];
    if (!here->is_term) {
        int c = oracle_best(n, here->sym, fuel - 1);
        if (c < 0) return 0;
        *acc += c;
        return 1;
    }
    for (int k = 0; k < here->nkids; k++)
        if (!oracle_kids(n->kids[k], p, i, fuel, acc)) return 0;
    return 1;
}

static int oracle_best(CovNode* n, int nt, int fuel) {
    if (fuel <= 0) return -1;
    int lo = -1;
    for (int r = 0; r < BurgMatcher::burg_rule_table_len; r++) {
        const BurgMatcher::burg_rule_row_t* row = &BurgMatcher::burg_rule_table[r];
        if (row->nonterm != nt) continue;
        int i = 0;
        if (!oracle_shape(n, row->pat, &i)) continue;
        if (!BurgMatcher::burg_rule_guard(row->rule, n)) continue;
        i = 0;
        int acc = 0;
        if (!oracle_kids(n, row->pat, &i, fuel, &acc)) continue;
        /* Variable arity, mirroring the matcher: children beyond the declared
         * pattern are covered with START, so they cost cost[START] (iburg p.4). */
        if (row->pat[0].is_term) {
            bool bad = false;
            for (int k = row->pat[0].nkids; k < n->nkids; k++) {
                int c = oracle_best(n->kids[k], stmt_NT, fuel - 1);
                if (c < 0) { bad = true; break; }
                acc += c;
            }
            if (bad) continue;
        }
        int c = row->cost + acc;
        if (lo < 0 || c < lo) lo = c;
    }
    return lo;
}

static void test_rule_table_describes_the_grammar() {
    CHECK(BurgMatcher::burg_rule_table_len == 5, "coverage.burg has five rules");
    const auto* t = BurgMatcher::burg_rule_table;
    CHECK(t[0].nonterm == reg_NT && t[0].cost == 1 && t[0].npat == 1 &&
              t[0].pat[0].is_term && t[0].pat[0].sym == COV_CONST && t[0].pat[0].nkids == 0,
          "row 0 is reg: Const = 1");
    CHECK(t[1].npat == 3 && t[1].pat[0].is_term && t[1].pat[0].sym == COV_ADD &&
              t[1].pat[0].nkids == 2 &&
              !t[1].pat[1].is_term && t[1].pat[1].sym == reg_NT &&
              !t[1].pat[2].is_term && t[1].pat[2].sym == reg_NT,
          "row 1 is reg: Add(reg, reg), children encoded as nonterminal leaves");
    CHECK(t[2].nonterm == stmt_NT && t[2].cost == 0 && t[2].npat == 1 &&
              !t[2].pat[0].is_term && t[2].pat[0].sym == reg_NT,
          "row 2 is the chain rule stmt: reg = 0");
    CHECK(t[4].nonterm == reg_NT && t[4].cost == 3 && t[4].npat == 1 &&
              t[4].pat[0].is_term && t[4].pat[0].sym == COV_CALL && t[4].pat[0].nkids == 0,
          "row 4 is reg: Call = 3 with no declared children");
    for (int r = 0; r < BurgMatcher::burg_rule_table_len; r++)
        CHECK(t[r].guarded == 0, "coverage.burg has no where-clauses");
}

/* Variable arity — the C driver's twin. Call(Const,Const): the children reduce
 * with START, so they must be costed with START: 3 + 1 + 1 = 5. */
static void test_variadic_children_are_costed() {
    BurgMatcher m;
    CovNode l = mk(COV_CONST, 1);
    CovNode r = mk(COV_CONST, 2);
    CovNode call = mk(COV_CALL, 3);
    call.nkids = 2;
    call.kids[0] = &l;
    call.kids[1] = &r;

    cov_trace_reset();
    BurgMatcher::BurgState* s = m.burg_label_root(&call);
    CHECK(s != nullptr, "a variadic node labels");
    check_trace(nullptr, 0, "labeling emits nothing");
    CHECK(BurgMatcher::burg_cost(s, reg_NT) == 5,
          "Call(Const,Const) as reg costs 3 + the children's START covers (1+1)");
    CHECK(BurgMatcher::burg_cost(s, stmt_NT) == 5, "the stmt chain adds 0 to it");

    cov_trace_reset();
    m.burg_rewrite(&call);
    const int want[] = {1, 10, 1, 10, 4, 10};
    check_trace(want, 6, "children reduce (with START) before the Call's action");

    CovNode bare = mk(COV_CALL, 4);
    BurgMatcher::BurgState* s0 = m.burg_label_root(&bare);
    CHECK(BurgMatcher::burg_cost(s0, reg_NT) == 3, "a nullary Call costs its rule alone");
}

static void test_labeler_agrees_with_an_independent_search() {
    BurgMatcher m;
    CovNode c  = mk(COV_CONST, 1);
    CovNode l  = mk(COV_CONST, 2);
    CovNode r  = mk(COV_CONST, 3);
    CovNode a  = mk(COV_ADD, 4);   a.nkids = 2;  a.kids[0] = &l;  a.kids[1] = &r;
    CovNode l2 = mk(COV_CONST, 5);
    CovNode a2 = mk(COV_ADD, 6);   a2.nkids = 2; a2.kids[0] = &a; a2.kids[1] = &l2;

    CovNode ca = mk(COV_CALL, 8);          /* variadic: the oracle must agree here too */
    ca.nkids = 2; ca.kids[0] = &c; ca.kids[1] = &a;
    struct { CovNode* n; const char* what; } cases[] = {
        {&c,  "Const"}, {&a,  "Add(Const,Const)"}, {&a2, "Add(Add(..),Const)"},
        {&ca, "Call(Const, Add(..))"},
    };
    for (int i = 0; i < 4; i++) {
        BurgMatcher::BurgState* s = m.burg_label_root(cases[i].n);
        int labeled  = BurgMatcher::burg_cost(s, stmt_NT);
        int searched = oracle_best(cases[i].n, stmt_NT, 32);
        if (labeled != searched) {
            std::printf("FAIL [%s]: labeler says %d, independent search says %d\n",
                        cases[i].what, labeled, searched);
            failures++;
        }
    }
    CovNode o = mk(COV_ORPHAN, 7);
    BurgMatcher::BurgState* s = m.burg_label_root(&o);
    CHECK(BurgMatcher::burg_rule(s, stmt_NT) == 0 && oracle_best(&o, stmt_NT, 32) < 0,
          "both agree an Orphan root has no stmt derivation");
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
    test_label_root_reports_dp_cost();
    test_cost_of_uncovered_goal();
    test_label_root_reduce_to_explicit_goal();
    test_rule_table_describes_the_grammar();
    test_variadic_children_are_costed();
    test_labeler_agrees_with_an_independent_search();

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("burgc C++ rewrite-error e2e: all checks passed\n");
    return 0;
}
