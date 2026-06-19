// Unit tests for the generic codegen peephole (opgen/runtime/bbq_emit.h).
//
// Pristine destination-driven codegen rarely produces anything past a
// goto-to-next (the calc only ever fires fallthrough). But an
// optimization pass munging the IR between ddcg and burg — constant
// folding, DCE, GVN, code motion — reintroduces goto-chains,
// goto-to-terminator, and conditional-over-goto. These tests build each
// of those patterns directly over a synthetic opcode set and check the
// matching Tanenbaum pass removes it, so every pass is exercised and
// verified independent of what any particular front end happens to emit.
#include "bbq_emit.h"
#include <gtest/gtest.h>

namespace {

// Synthetic opcode set: a 1-byte filler + terminator + the control ops.
enum : uint8_t { T_NOP = 0x01, T_GOTO = 0x02, T_RET = 0x03, T_COND = 0x04, T_NCOND = 0x05 };

const bbq::Peephole& cfg() {
    static const bbq::Peephole c = [] {
        static uint8_t inv[256] = {};
        inv[T_COND] = T_NCOND;
        inv[T_NCOND] = T_COND;
        return bbq::Peephole{ T_GOTO, inv, [](uint8_t op) { return op == T_RET; } };
    }();
    return c;
}

// Count instruction-boundary occurrences of `target` (branch ops carry a
// BRANCH_WIDTH operand; the synthetic 1-byte ops don't).
int count_op(const std::vector<uint8_t>& code, uint8_t target) {
    int n = 0;
    for (size_t i = 0; i < code.size(); ) {
        uint8_t op = code[i];
        if (op == target) n++;
        bool is_branch = (op == T_GOTO || op == T_COND || op == T_NCOND);
        i += is_branch ? (1 + bbq::Emit::BRANCH_WIDTH) : 1;
    }
    return n;
}

// Distinct label keys (their addresses are the opaque anchors).
int L1, L2, T, F, END;

// Redundant jump removal: a goto to the next instruction vanishes.
TEST(BbqPeephole, Fallthrough) {
    bbq::Emit e;
    e.branch(T_GOTO, &L1);   // goto L1
    e.label(&L1);            // L1: (the very next instruction)
    e.op(T_NOP);             // non-terminator, so tail-jump doesn't preempt
    e.finalize(cfg());
    EXPECT_EQ(e.code.size(), 1u);          // whole goto gone
    EXPECT_EQ(e.code[0], T_NOP);
    EXPECT_EQ(count_op(e.code, T_GOTO), 0);
}

// Tail merging: a goto whose target is a 1-byte terminator becomes it.
TEST(BbqPeephole, TailJump) {
    bbq::Emit e;
    e.branch(T_GOTO, &L1);   // goto L1   (not the next instruction)
    e.op(T_NOP);
    e.label(&L1);            // L1:
    e.op(T_RET);             // a 1-byte terminator
    e.finalize(cfg());
    EXPECT_EQ(count_op(e.code, T_GOTO), 0);  // goto replaced...
    EXPECT_EQ(e.code[0], T_RET);             // ...by the return itself
}

// Branch reversal: cond→T; goto→F; T-adjacent  ⇒  !cond→F (goto gone).
TEST(BbqPeephole, BranchReversal) {
    bbq::Emit e;
    e.branch(T_COND, &T);    // if cond goto T
    e.branch(T_GOTO, &F);    // goto F  (F is a non-terminator, not the next)
    e.label(&T);             // T: (right after the goto)
    e.op(T_NOP);
    e.op(T_NOP);
    e.label(&F);             // F:
    e.op(T_NOP);
    e.finalize(cfg());
    EXPECT_EQ(e.code[0], T_NCOND);           // condition inverted
    EXPECT_EQ(count_op(e.code, T_GOTO), 0);  // bridge goto removed
    EXPECT_EQ(count_op(e.code, T_COND), 0);
}

// Branch chaining: goto→(goto→RET) collapses; no goto survives.
TEST(BbqPeephole, ThreadChain) {
    bbq::Emit e;
    e.branch(T_GOTO, &L1);   // goto L1
    e.op(T_NOP);
    e.label(&L1);            // L1:
    e.branch(T_GOTO, &L2);   // goto L2
    e.op(T_NOP);
    e.label(&L2);            // L2:
    e.op(T_RET);
    e.finalize(cfg());
    EXPECT_EQ(count_op(e.code, T_GOTO), 0);  // both gotos chained away
    EXPECT_GE(count_op(e.code, T_RET), 1);
}

}  // namespace
