// codebuf_test.cpp — the executable code buffer's TWO PHASES.
//
// Stamped code holds PC-relative displacements computed from `base`, so a grow
// that moves the buffer invalidates every one already written. The rule that
// makes growth safe is that nothing depending on `base` is written while the
// layout can still change: copy every stencil, seal, then patch. The buffer
// enforces the split, and that enforcement is what this file is about.
//
// It had no tests. The property was previously bought instead of checked — by
// reserving 64 MB of address space per buffer up front so growth could never
// happen — which held the invariant on a 64-bit host by making the unsafe path
// unreachable, cost 84 GB of executable address space for ~6 MB of code in a
// real run (one buffer per compiled function), failed outright on a 32-bit
// target after some 48 functions, and is inexpressible where there is no MMU.
extern "C" {
#include "jit_codebuf.h"
}
#include <gtest/gtest.h>
#include <vector>

namespace {

struct Buf {
    jit_codebuf_t b;
    explicit Buf(size_t cap = 4096) { EXPECT_EQ(jcb_init(&b, cap), 0); }
    ~Buf() { jcb_free(&b); }
};

size_t page() { return (size_t)sysconf(_SC_PAGESIZE); }

// ── the size you ask for is the size you get ──────────────────────

TEST(CodeBuf, InitHonoursTheRequestedSize) {
    Buf s(4096);
    EXPECT_EQ(s.b.cap, jcb_page_align(4096));
    EXPECT_LT(s.b.cap, (size_t)1u << 20) << "a one-page ask must not reserve megabytes";
}

TEST(CodeBuf, InitRoundsUpToAPageAndZeroMeansOnePage) {
    Buf s(1);
    EXPECT_EQ(s.b.cap, page());
    jit_codebuf_t z;
    ASSERT_EQ(jcb_init(&z, 0), 0);
    EXPECT_EQ(z.cap, jcb_page_align(4096));
    jcb_free(&z);
}

// ── growth, which is the thing the reservation used to forbid ──────

TEST(CodeBuf, GrowsPastItsInitialCapAndKeepsTheBytes) {
    Buf s(page());
    std::vector<uint8_t> pattern(page() * 3);
    for (size_t i = 0; i < pattern.size(); i++) pattern[i] = (uint8_t)(i * 7 + 1);

    jcb_emit(&s.b, pattern.data(), pattern.size());
    ASSERT_TRUE(jcb_ok(&s.b)) << "a grow during the emit phase is legal";
    ASSERT_EQ(s.b.size, pattern.size());
    EXPECT_GE(s.b.cap, pattern.size());
    EXPECT_EQ(memcmp(s.b.base, pattern.data(), pattern.size()), 0)
        << "the copy must carry every byte already stamped";
}

TEST(CodeBuf, GrowthMayMoveTheBufferSoCallersMustNotCacheBase) {
    Buf s(page());
    std::vector<uint8_t> filler(page(), 0xCC);
    jcb_emit(&s.b, filler.data(), filler.size());
    uint8_t* before = s.b.base;
    std::vector<uint8_t> more(page() * 2, 0xDD);
    jcb_emit(&s.b, more.data(), more.size());
    ASSERT_TRUE(jcb_ok(&s.b));
    // Whether it moved is the allocator's business; that the CONTENT survived is
    // the contract. (If it did move, `before` is unmapped — never dereferenced.)
    (void)before;
    EXPECT_EQ(s.b.base[0], 0xCC);
    EXPECT_EQ(s.b.base[page()], 0xDD);
}

// ── the seal, from both sides ──────────────────────────────────────

TEST(CodeBuf, EmitAfterSealIsRefused) {
    Buf s;
    uint8_t code[4] = { 1, 2, 3, 4 };
    jcb_emit(&s.b, code, sizeof code);
    ASSERT_TRUE(jcb_ok(&s.b));
    size_t size_at_seal = s.b.size;

    jcb_seal(&s.b);
    EXPECT_TRUE(jcb_sealed(&s.b));
    jcb_emit(&s.b, code, sizeof code);

    EXPECT_FALSE(jcb_ok(&s.b)) << "the layout is what the displacements were computed against";
    EXPECT_EQ(s.b.size, size_at_seal) << "and nothing was appended";
    EXPECT_EQ(jcb_finalize(&s.b), nullptr);
}

TEST(CodeBuf, RelativePatchBeforeSealIsRefused) {
    Buf s;
    uint8_t code[16] = { 0 };
    jcb_emit(&s.b, code, sizeof code);
    ASSERT_TRUE(jcb_ok(&s.b));

    // A displacement written while the buffer can still move is a displacement
    // to wherever the buffer used to be — silently, which is the whole bug.
    EXPECT_EQ(jcb_patch_rel32(&s.b, 4, (uint64_t)(uintptr_t)(s.b.base + 12)), -1);
    EXPECT_FALSE(jcb_ok(&s.b));
    EXPECT_EQ(jcb_finalize(&s.b), nullptr);
}

TEST(CodeBuf, RelativePatchAfterSealWritesTheDisplacement) {
    Buf s;
    uint8_t code[16] = { 0 };
    jcb_emit(&s.b, code, sizeof code);
    jcb_seal(&s.b);

    uint8_t* target = s.b.base + 12;
    ASSERT_EQ(jcb_patch_rel32(&s.b, 4, (uint64_t)(uintptr_t)target), 0);
    ASSERT_TRUE(jcb_ok(&s.b));

    int32_t disp;
    memcpy(&disp, s.b.base + 4, 4);
    EXPECT_EQ(disp, (int32_t)(target - (s.b.base + 4 + 4)))
        << "target - (address after the field)";
}

// Absolute writes do not depend on where the buffer sits — a native address, an
// immediate — so a move carries them along unchanged and they are legal while
// still emitting. Gating them would forbid the ordinary case.
TEST(CodeBuf, AbsolutePatchesAreLegalBeforeTheSeal) {
    Buf s;
    uint8_t code[16] = { 0 };
    jcb_emit(&s.b, code, sizeof code);

    jcb_patch64(&s.b, 0, 0xDEADBEEFCAFEF00Dull);
    jcb_patch32(&s.b, 8, -12345);
    EXPECT_TRUE(jcb_ok(&s.b));

    uint64_t a; int32_t bv;
    memcpy(&a, s.b.base, 8);
    memcpy(&bv, s.b.base + 8, 4);
    EXPECT_EQ(a, 0xDEADBEEFCAFEF00Dull);
    EXPECT_EQ(bv, -12345);
}

TEST(CodeBuf, AbsolutePatchesSurviveAGrow) {
    Buf s(page());
    std::vector<uint8_t> filler(page() - 16, 0);
    jcb_emit(&s.b, filler.data(), filler.size());
    uint8_t slot[8] = { 0 };
    size_t at = s.b.size;
    jcb_emit(&s.b, slot, sizeof slot);
    jcb_patch64(&s.b, at, 0x0123456789ABCDEFull);

    std::vector<uint8_t> more(page() * 2, 0xEE);   // forces the grow
    jcb_emit(&s.b, more.data(), more.size());
    ASSERT_TRUE(jcb_ok(&s.b));

    uint64_t v;
    memcpy(&v, s.b.base + at, 8);
    EXPECT_EQ(v, 0x0123456789ABCDEFull) << "an absolute value is base-independent";
}

// ── the range check, and failure staying sticky ────────────────────

TEST(CodeBuf, OutOfRangeDisplacementIsRefused) {
    Buf s;
    uint8_t code[16] = { 0 };
    jcb_emit(&s.b, code, sizeof code);
    jcb_seal(&s.b);

    // A JIT buffer sits some 140,000 GB from the host's text; casting that
    // difference to int32 is a branch into nowhere.
    uint64_t far_away = (uint64_t)(uintptr_t)s.b.base + ((uint64_t)1 << 40);
    EXPECT_EQ(jcb_patch_rel32(&s.b, 4, far_away), -1);
    EXPECT_FALSE(jcb_ok(&s.b));
    EXPECT_EQ(jcb_finalize(&s.b), nullptr) << "code that was never stamped is not handed back";
}

TEST(CodeBuf, ResetReturnsToTheEmitPhase) {
    Buf s;
    uint8_t code[4] = { 9, 9, 9, 9 };
    jcb_emit(&s.b, code, sizeof code);
    jcb_seal(&s.b);
    jcb_emit(&s.b, code, sizeof code);          // refused: marks the buffer
    ASSERT_FALSE(jcb_ok(&s.b));

    jcb_reset(&s.b);                            // a driver re-stamping a body
    EXPECT_TRUE(jcb_ok(&s.b));
    EXPECT_FALSE(jcb_sealed(&s.b));
    EXPECT_EQ(s.b.size, 0u);
    jcb_emit(&s.b, code, sizeof code);
    EXPECT_TRUE(jcb_ok(&s.b)) << "re-emitting is what reset is for";
}

TEST(CodeBuf, FinalizeSealsSoNothingMoreCanBeEmitted) {
    Buf s;
    uint8_t ret[1] = { 0xC3 };
    jcb_emit(&s.b, ret, sizeof ret);
    jcb_seal(&s.b);
    ASSERT_NE(jcb_finalize(&s.b), nullptr);
    EXPECT_TRUE(jcb_sealed(&s.b));
    jcb_emit(&s.b, ret, sizeof ret);
    EXPECT_FALSE(jcb_ok(&s.b)) << "the pages are executable now";
}

// The whole point, end to end: stamp two blocks with a growth in between, seal,
// patch a displacement between them, and run it.
TEST(CodeBuf, StampGrowSealPatchAndCall) {
    Buf s(page());
    // Block A: jmp rel32 <B>  (E9 with a hole), then filler to force a grow.
    uint8_t jmp[5] = { 0xE9, 0, 0, 0, 0 };
    jcb_emit(&s.b, jmp, sizeof jmp);
    std::vector<uint8_t> filler(page() * 2, 0x90);   // nops; forces the buffer to grow
    jcb_emit(&s.b, filler.data(), filler.size());
    size_t b_off = s.b.size;
    uint8_t body[4] = { 0xB8, 0x2A, 0x00, 0x00 };    // mov eax, 42 (imm32 low bytes)
    jcb_emit(&s.b, body, sizeof body);
    uint8_t tail[2] = { 0x00, 0xC3 };                // …imm32 high byte, ret
    jcb_emit(&s.b, tail, sizeof tail);
    ASSERT_TRUE(jcb_ok(&s.b));

    jcb_seal(&s.b);
    ASSERT_EQ(jcb_patch_rel32(&s.b, 1, (uint64_t)(uintptr_t)(s.b.base + b_off)), 0);

    void* exec = jcb_finalize(&s.b);
    ASSERT_NE(exec, nullptr);
    int (*fn)(void) = (int (*)(void))exec;
    EXPECT_EQ(fn(), 42) << "the displacement reached the block the grow had moved";
}

}  // namespace
