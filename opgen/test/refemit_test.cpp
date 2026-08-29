// opgen RefEmitter tests — the pure reference kernels (`<prefix>_ref.h`).
// A kernel is the spec body as a standalone C function: declared stack-effect
// names as by-value ins, outs through pointers, guards as early returns whose
// codes mirror the generated opgen_err_t numbering. Only CLOSED bodies are
// eligible (no locals/globals/stack/pc/sp, no natives, no traps); every
// skipped op is listed in the header with its reason — a silent skip would
// read as "covered".
//
// Assertions are substrings, per the house pattern (vmemit_test.cpp): the
// property under test is the emitted contract, not the whole file.
#include "Parser.h"
#include "refemit.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

opgen::Module* parse(const char* src) {
    Parser p;
    p.init(src, (int)std::strlen(src));
    bool ok = p.parse();
    auto f = p.furthest();
    EXPECT_TRUE(ok) << "parse failed near line " << f.line << " col " << f.col;
    return ok ? p.ast : nullptr;
}

std::string refh(const char* spec) {
    auto* m = parse(spec);
    if (!m) return "";
    char* buf = nullptr; size_t len = 0;
    FILE* f = open_memstream(&buf, &len);
    opgen::RefEmitter ref(m, "calc");
    ref.emit_ref_h(f);
    fclose(f);
    std::string out(buf, len);
    std::free(buf);
    return out;
}

#define HAS(hay, needle)  EXPECT_NE((hay).find(needle), std::string::npos) \
    << "expected to find:\n  " << (needle) << "\nin:\n" << (hay)
#define LACKS(hay, needle) EXPECT_EQ((hay).find(needle), std::string::npos) \
    << "expected NOT to find:\n  " << (needle)

const char* kI32 = "type i32 s4 u4 1 i\n";

// ── A closed body becomes a kernel: ins by value, outs by pointer ──────────

TEST(OpgenRef, ClosedBinopBecomesAKernel) {
    std::string s = refh(std::string(kI32).append(R"(
iadd 0x02 (i32 a, i32 b -- i32 r)
    (. r = a + b; .)
)").c_str());
    HAS(s, "static inline int calc_ref_iadd(s4 a, s4 b, s4* out_r)");
    HAS(s, "r = (s4)((a + b));");
    HAS(s, "*out_r = r;");
    HAS(s, "return CALC_REF_OK;");
}

// ── Guards mirror opgen_err_t: same names, same first-declaration order ────

TEST(OpgenRef, GuardReturnsTheErrorCodeBeforeTheBodyRuns) {
    std::string s = refh(std::string(kI32).append(R"(
idiv 0x0D (i32 a, i32 b -- i32 r)
    error: (. b == 0 .) -> DivByZero
    (. r = a / b; .)
)").c_str());
    HAS(s, "if ((_Bool)(b == 0)) return CALC_REF_ERR_DivByZero;");
    HAS(s, "r = (s4)((a / b));");
    // The code numbering is the opgen_err_t numbering (runtime_api.h):
    // first-declaration order, zero-based; OK is -1 so the spaces never collide.
    HAS(s, "CALC_REF_OK = -1");
    HAS(s, "CALC_REF_ERR_DivByZero = 0");
}

TEST(OpgenRef, ErrorCodesFollowFirstDeclarationOrderAcrossOps) {
    std::string s = refh(std::string(kI32).append(R"(
op_a 0x01 (i32 a, i32 b -- i32 r)
    error: (. b == 0 .) -> First
    (. r = a / b; .)
op_b 0x02 (i32 a, i32 b -- i32 r)
    error: (. b == 1 .) -> Second
    error: (. b == 0 .) -> First
    (. r = a / b; .)
)").c_str());
    HAS(s, "CALC_REF_ERR_First = 0");
    HAS(s, "CALC_REF_ERR_Second = 1");
}

// ── An operand is a decoded scalar parameter ───────────────────────────────

TEST(OpgenRef, OperandBecomesAValueParameter) {
    std::string s = refh(std::string(kI32).append(R"(
iconst 0x01 [ i32 v ] ( -- i32 r )
    (. r = v; .)
)").c_str());
    HAS(s, "static inline int calc_ref_iconst(s4 v, s4* out_r)");
    HAS(s, "r = (s4)(v);");
}

// ── An out that is also an in gets no shadowing local ──────────────────────

TEST(OpgenRef, PassThroughOutDoesNotShadowTheInput) {
    std::string s = refh(std::string(kI32).append(R"(
inop 0x01 (i32 a -- i32 a)
    (. a = a; .)
)").c_str());
    HAS(s, "static inline int calc_ref_inop(s4 a, s4* out_a)");
    LACKS(s, "s4 a = 0;");
    HAS(s, "*out_a = a;");
}

// ── Ineligible bodies are skipped AND listed, never silently dropped ───────

TEST(OpgenRef, LocalsAccessIsSkippedWithItsReason) {
    std::string s = refh(std::string(kI32).append(R"(
istore 0x0B [ i8 idx ] (i32 v -- )
    local_value: int
    (. locals[idx] = v; .)
)").c_str());
    LACKS(s, "calc_ref_istore");
    HAS(s, "istore");
    HAS(s, "accesses locals[]/globals[]/code[]/stack[]");
}

TEST(OpgenRef, NativeCallIsSkippedWithItsReason) {
    std::string s = refh(std::string(kI32).append(R"(
native int helper(int x);
callop 0x01 (i32 a -- i32 r)
    (. r = helper(a); .)
)").c_str());
    LACKS(s, "calc_ref_callop");
    HAS(s, "calls a native");
}

TEST(OpgenRef, BodylessOpIsSkippedWithItsReason) {
    std::string s = refh(std::string(kI32).append(R"(
mystery 0x01 (i32 a -- i32 r)
)").c_str());
    LACKS(s, "calc_ref_mystery");
    HAS(s, "no body");
}

TEST(OpgenRef, VmStateReadIsSkippedWithItsReason) {
    std::string s = refh(std::string(kI32).append(R"(
spread 0x01 ( -- i32 r )
    (. r = sp; .)
)").c_str());
    LACKS(s, "calc_ref_spread");
    HAS(s, "reads VM state (sp/pc/stp/lane)");
}

TEST(OpgenRef, TrapStatementIsSkippedWithItsReason) {
    std::string s = refh(std::string(kI32).append(R"(
boom 0x01 ( -- )
    (. trap; .)
)").c_str());
    LACKS(s, "calc_ref_boom");
    HAS(s, "explicit trap");
}

// ── Self-containment: the header brings its own scalar spellings ───────────

TEST(OpgenRef, HeaderIsSelfContained) {
    std::string s = refh(std::string(kI32).append(R"(
iadd 0x02 (i32 a, i32 b -- i32 r)
    (. r = a + b; .)
)").c_str());
    HAS(s, "#include <stdint.h>");
    HAS(s, "typedef int8_t  s1;");
    LACKS(s, "#include \"runtime_api.h\"");
    // The wrap contract is the interpreter's: consumers must compile the same way.
    HAS(s, "-fwrapv");
}

} // namespace
