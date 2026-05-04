// End-to-end tests for the calc compiler pipeline.
//
// `Regression`  — input → numeric result. Drives the full
//                 parse → compile → JIT → execute path via calc_run().
// `IRShape`     — structural assertions on the IR continuation graph
//                 produced by calc_compile(). Used to pin properties
//                 the rule rewrites are supposed to preserve, e.g.
//                 the L==Lnext peephole means fall-through paths emit
//                 zero Goto nodes.

#include "calc_runner.h"

#include <gtest/gtest.h>

namespace {

struct RegressionCase {
    const char* input;
    int64_t     expected;
};

// Each entry gets one TEST_P instantiation. Cover at least one input
// per surface construct so a regression in any rule body shows up
// here, not at runtime in someone's debugger.
const RegressionCase kCases[] = {
    {"5",                                               5},
    {"1+2",                                             3},
    {"2+3*4",                                          14},
    {"1==1",                                            1},
    {"2<3",                                             1},
    {"3<2",                                             0},
    {"let $a = 5; $a + 10",                            15},
    {"if (1==1) 5 else 7",                              5},
    {"if (1==0) 5 else 7",                              7},
    {"let $a = 5; if ($a < 10) $a else 99",             5},
    {"if (1==1) (if (2==2) 100 else 200) else 300",   100},
    {"if (1==0) 999 else (if (3<2) 11 else 22)",       22},
    {"-5 + 7",                                          2},
    {"let $a = 3; let $b = 4; $a * $b",                12},
    // while + break (paper figure 5)
    {"let $a = 0; while ($a < 5) $a = $a + 1; $a",      5},
    {"let $a = 0; while ($a < 3) $a = $a + 1; $a",      3},
    {"let $a = 10; let $b = 0; while ($a > 0) ($a = $a - 1; $b = $b + 1); $b",
                                                       10},
    // return (paper figure 5: γ=ret delivers to ctx.ac via Halt)
    {"return 5",                                        5},
    {"return 1 + 2",                                    3},
    {"let $a = 5; return $a + 10",                     15},
    // and/or/not in test position (paper figure 7)
    {"if (1==1 && 2==2) 100 else 200",                100},
    {"if (1==1 && 2==3) 100 else 200",                200},
    {"if (1==0 || 2==2) 100 else 200",                100},
    {"if (1==0 || 2==3) 100 else 200",                200},
    {"if (!(1==0)) 100 else 200",                     100},
    {"if (!(1==1)) 100 else 200",                     200},
    // Page-12 boolean-as-value normalization. Bare boolean expression
    // in value context wraps to `if Ebool 1 0` before DDCG. Outer
    // parens are required: `=` binds tighter than `||`/`&&`/`!`.
    {"let $a = (1==1); $a",                             1},
    {"let $a = (1==0); $a",                             0},
    {"let $a = ((1==1) && (2==2)); $a",                 1},
    {"let $a = ((1==0) || (2==2)); $a",                 1},
    {"let $a = (!(1==0)); $a",                          1},
    // Page 12 effect+single-label rewrites — boolean producers in
    // sequence-first position are evaluated for effect only; the
    // sequence's value comes from the rest.
    {"(1==1); 5",                                       5},  // rule 4: relop → sequence
    {"((1==1) && (2==2)); 7",                           7},  // rule 2: and → if
    {"((1==0) || (2==2)); 11",                         11},  // rule 3: or → if(not)
    {"(!(1==0)); 13",                                  13},  // rule 5: not → E
    // Nil-else fill — `if (test) then` (no else) returns IntLit(0).
    {"if (1) 100",                                    100},
    {"if (0) 100",                                      0},
    // Let-bindings.
    {"let $a = 5; $a + $a",                            10},
    {"let $a = 3; let $b = 4; $a + $b",                 7},
    // Function declarations + CG[call] (paper page 14).
    {"fn add($x, $y) ($x + $y); add(2, 3)",             5},
    {"fn id($x) $x; id(42)",                           42},
    {"fn k($x, $y) $x; k(7, 99)",                       7},
    // Recursion — return value via ctx.ac, frame discipline via cpu CALL/RET.
    {"fn fact($n) (if ($n == 0) 1 else $n * fact($n - 1)); fact(5)",
                                                      120},
    {"fn fact($n) (if ($n == 0) 1 else $n * fact($n - 1)); fact(10)",
                                                  3628800},
    // Mutual recursion via forward refs.
    {"fn even($n) (if ($n == 0) 1 else odd($n - 1)); "
     "fn odd($n) (if ($n == 0) 0 else even($n - 1)); even(10)",      1},
    {"fn even($n) (if ($n == 0) 1 else odd($n - 1)); "
     "fn odd($n) (if ($n == 0) 0 else even($n - 1)); odd(7)",        1},
    // Let inside a function body.
    {"fn double($x) (let $y = $x; $y + $y); double(7)", 14},
    // Top-level let shared with a function call (fn-decls precede main).
    {"fn add5($x) ($x + 5); let $a = 5; add5($a)",    10},
    // While body: if ($a == 5) break (no else) — Phase 3.5's nil fill
    // turns this into `if ($a == 5) break else 0` so if_break_else's
    // BindPat for `else_: el` binds a real node and the Branch has no
    // null target.
    {"let $a = 5; while (1) (if ($a == 5) break); $a",  5},
    // Paper figure 8 case 3 (RevSub) — `(1+2) - $a` with $a = 5 → -2.
    {"let $a = 5; (1+2) - $a",                         -2},
    // Paper figure 8 case 3 cmp swap — `(1+2) < $a` reuses CmpGt.
    {"let $a = 5; (1+2) < $a",                          1},
    {"let $a = 2; (1+2) < $a",                          0},
    // Nested calls — Call-as-value-producer composing as sub-tree of
    // another Call's args. Pins Phase 9R's value-tree shape end-to-end.
    {"fn add($x, $y) ($x + $y); add(add(1, 2), 3)",     6},
    {"fn id($x) $x; id(id(id(7)))",                     7},
    // Deep recursion — 64-bit math + cpu-stack depth.
    {"fn fact($n) (if ($n == 0) 1 else $n * fact($n - 1)); fact(20)",
                                              2432902008176640000LL},
    // F8 case 3 commutative explicit — Add(LoadConst, LoadLocal) and
    // Mul(LoadConst, LoadLocal) trigger `reg: Op(reg, LoadLocal)` rules
    // that swap the operand position relative to the lhs-local cases.
    {"let $a = 5; 3 + $a",                              8},
    {"let $a = 4; 5 * $a",                             20},
};

class CalcRegression : public ::testing::TestWithParam<RegressionCase> {};

TEST_P(CalcRegression, ProducesExpectedResult) {
    const auto& c = GetParam();
    bool ok = false;
    int64_t got = calc_runner::calc_run(c.input, &ok);
    ASSERT_TRUE(ok) << "parse error on: " << c.input;
    EXPECT_EQ(got, c.expected) << "input: " << c.input;
}

INSTANTIATE_TEST_SUITE_P(All, CalcRegression, ::testing::ValuesIn(kCases));

// ─── IR shape assertions ────────────────────────────────────

TEST(IRShape, IntLiteralEmitsOneLoadConstAndOneLeave) {
    // CEK-tree IR: bare int literal compiles to Leave(LoadConst(5)).
    // Driver compiles main with γ=ret, so cg_deliver wraps the value
    // in make_leave (stencil-identical to Halt).
    auto* ir = calc_runner::calc_compile("5");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::LoadConst), 1);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Leave), 1);
}

TEST(IRShape, AddEmitsExactlyOneAddNode) {
    auto* ir = calc_runner::calc_compile("let $a = 3; $a + 4");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Add), 1);
}

TEST(IRShape, IfEmitsBranchAndPerArmLeave) {
    // CEK-tree IR: if-then-else compiles to Branch(test, then, else)
    // where each arm inherits the outer δ/γ. With main γ=ret, each
    // arm's value sub-tree is wrapped in its own Leave (= 2 Leaves).
    // The (1==1) cmp's both-immediate operand pairing has no spill
    // at the IR level — BURG case 4 catchall handles spilling
    // inside the stencil pipeline at lowering time.
    auto* ir = calc_runner::calc_compile("if (1==1) 5 else 7");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Branch), 1);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Leave), 2);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::StoreLocal), 0);
}

// ─── Figure 8 case dispatch ────────────────────────────────
// Pin the operand-shape specializations: case 1 both-locals avoids
// the spill; case 2 nests recursive case 1 inside local+complex.

TEST(F8, BothImmediatesProduceTreeShape) {
    // CEK-tree IR: `1+2` is Leave(Add(LoadConst(1), LoadConst(2))).
    // Spill (BURG case 4) is stencil-internal — no IR-level
    // StoreLocal emitted for it.
    auto* ir = calc_runner::calc_compile("1+2");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Add), 1);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::LoadConst), 2);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::StoreLocal), 0);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Leave), 1);
}

TEST(F8, LocalPlusImmediateNoSpill) {
    // $a + 1 → LoadConst(1) → Add(0). One StoreLocal from the prior
    // `let $a = 0` setup, none from the binop.
    auto* ir = calc_runner::calc_compile("let $a = 0; $a + 1");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Add), 1);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::StoreLocal), 1);
}

TEST(F8, BothLocalsTreeShape) {
    // CEK-tree IR: `$a + $b` is Add(LoadLocal(0), LoadLocal(1)) with
    // both operand sub-trees as real LoadLocal leaves. Two
    // StoreLocals from the prior lets.
    auto* ir = calc_runner::calc_compile("let $a = 0; let $b = 0; $a + $b");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Add), 1);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::LoadLocal), 2);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::StoreLocal), 2);
}

TEST(F8, LocalPlusComplexCase2) {
    // $a + ($b * $c) → case 2 outer (local+complex), case 1 inner.
    // Three StoreLocals from the prior lets; none from the binops.
    auto* ir = calc_runner::calc_compile(
        "let $a = 0; let $b = 0; let $c = 0; $a + ($b * $c)");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Add), 1);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Mul), 1);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::StoreLocal), 3);
}

TEST(F8, ComplexMinusLocalKeepsSubInIR) {
    // Paper figure 8 case 3 with Sub: `(1+2) - $a`. In CEK-tree IR
    // (Phase 9R), the IR shape is `Sub(Add(LoadConst, LoadConst),
    // LoadLocal)` — no RevSub IR node. BURG matches the shape via
    // `reg: Sub(reg, LoadLocal)` and selects the rev_sub stencil at
    // lowering time. The IR stays paper-figure-8 case 3 directly.
    auto* ir = calc_runner::calc_compile("let $a = 5; (1+2) - $a");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Sub), 1);
}

TEST(IRShape, CallNodeArityMatchesCallSite) {
    // Phase 9R: Call is a value-producer with args as sub-tree
    // children. Per-arity BURG rules (`reg: Call(stack_reg, ...)`)
    // dispatch on Call.args.size(); pin that the IR shape carries
    // the right arity from the AST.
    auto* ir0 = calc_runner::calc_compile("fn z() 0; z()");
    ASSERT_NE(ir0, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir0, calc_ir::NodeTag::Call), 1);

    auto* ir2 = calc_runner::calc_compile("fn add($x, $y) ($x + $y); add(2, 3)");
    ASSERT_NE(ir2, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir2, calc_ir::NodeTag::Call), 1);

    // Nested: outer Call has args = [Call(add(1,2)), LoadConst(3)].
    // Two Calls in the IR (inner + outer); inner is a sub-tree of outer.
    auto* ir_nested = calc_runner::calc_compile(
        "fn add($x, $y) ($x + $y); add(add(1, 2), 3)");
    ASSERT_NE(ir_nested, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir_nested, calc_ir::NodeTag::Call), 2);
}

TEST(F8, ComplexLessThanLocalKeepsCmpLtInIR) {
    // CEK-tree IR: `(1+2) < $a` is CmpLt(Add(LoadConst, LoadConst),
    // LoadLocal). BURG matches `reg: CmpLt(reg, LoadLocal)` and
    // selects the cmp_gt stencil (swap identity) at lowering time;
    // the IR itself stays paper-direct as CmpLt.
    auto* ir = calc_runner::calc_compile("let $a = 5; (1+2) < $a");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::CmpLt), 1);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::CmpGt), 0);
}

TEST(IRShape, Page12Rule4SequenceStripsCmp) {
    // (1==1); 5 — page-12 rule 4 rewrites cmp in effect+single-label
    // to sequence(left, right). The CmpEq node should be GONE from the
    // IR (rewritten away by the normalizer before DDCG sees it).
    auto* ir = calc_runner::calc_compile("(1==1); 5");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::CmpEq), 0);
}

// Pin the L==Lnext peephole. cg_jump's `jump(L) ⇒ L == Lnext ? Lnext :
// make_goto(L)` (paper page 13) means every gen() call whose γ
// matches its Lnext produces no Goto. Across the whole regression set
// — sequence chains, binop spills landing in ac, if arms terminating
// in their inherited γ — every transfer should peephole-elide. If a
// later refactor breaks Lnext threading, Goto count will go non-zero.
struct GotoCountInput { const char* input; };
const GotoCountInput kGotoZeroInputs[] = {
    {"5"}, {"1+2"}, {"2+3*4"}, {"1==1"}, {"2<3"},
    {"let $a = 5; $a + 10"}, {"if (1==1) 5 else 7"},
    {"let $a = 5; if ($a < 10) $a else 99"},
    {"if (1==1) (if (2==2) 100 else 200) else 300"},
    {"-5 + 7"},
    {"let $a = 3; let $b = 4; $a * $b"},
    // While body's γ=jump(Ltest), Lnext=Ltest → peephole fires for the
    // back-edge (no Goto). Test's pair-γ emits a Branch (no Goto).
    {"let $a = 0; while ($a < 5) $a = $a + 1; $a"},
};

class GotoCountZero : public ::testing::TestWithParam<GotoCountInput> {};

TEST_P(GotoCountZero, NoGotoForFallThroughPaths) {
    const auto& c = GetParam();
    auto* ir = calc_runner::calc_compile(c.input);
    ASSERT_NE(ir, nullptr) << "parse error on: " << c.input;
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Goto), 0)
        << "input: " << c.input;
}

INSTANTIATE_TEST_SUITE_P(All, GotoCountZero, ::testing::ValuesIn(kGotoZeroInputs));

// Paper figure 6: `if Etest break Eelse` rewrites the test's pair-γ to
// (Lbreak, Lelse) — the break-arm becomes a direct branch target, not
// a separate cg_break that would allocate a Goto. Without figure 6,
// the break inside an if-arm allocates a Goto(exit_label) because its
// Lnext is Ltest, not exit. With figure 6 firing, zero Gotos for the
// break.
TEST(IRShape, Figure6IfBreakElseEmitsNoGoto) {
    // While body: if ($a != 0) break else $a = $a + 1.
    // Trailing `; $a` puts the loop in effect+SingleLabel context
    // (sequence-first) — paper page 10 requires loop's outer δ to be
    // effect, so a bare loop at main's tail (γ=ret) would hit
    // panic_unreachable in loop_op.
    auto* ir = calc_runner::calc_compile(
        "let $a = 1; while (1) (if ($a != 0) break else $a = $a + 1); $a"
    );
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Goto), 0);
}

// Naïve break (without figure-6 wrapping) DOES allocate a Goto: break's
// cg_jump goes to L_break with Lnext=Ltest, peephole misses, Goto emitted.
TEST(IRShape, NaiveBreakAllocatesOneGoto) {
    auto* ir = calc_runner::calc_compile("while (1) break; 0");
    ASSERT_NE(ir, nullptr);
    EXPECT_EQ(calc_runner::count_node_kind(ir, calc_ir::NodeTag::Goto), 1);
}

} // namespace
