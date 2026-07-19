// opgen SemLowerer tests — lower an opcode's sem_body to C and assert the exact
// emitted text (byte-faithful to the reference VM lowering), in both modes.
#include "Parser.h"
#include "semlower.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
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

// Lower one opcode's whole body to a string, in the given mode.
std::string lower_body(const opgen::Module* m, const opgen::Opcode* op,
                       opgen::SemLowerer::Mode mode) {
    char* buf = nullptr; size_t len = 0;
    FILE* f = open_memstream(&buf, &len);
    opgen::SemLowerer low(m, f, mode, "wasm");
    for (auto* s : op->sem_body) low.lower_stmt(op, s);
    fclose(f);
    std::string out(buf, len);
    std::free(buf);
    return out;
}

const char* kSpec = R"OPGEN(
type i32 int32_t uint32_t 1 i

native int helper(int a);

add 0x10 (i32 a, i32 b -- i32 r)
    (. r = a + b; .)

shl 0x11 (i32 a, i32 b -- i32 r)
    (. r = a << b; .)

cast 0x12 (i32 a -- i32 r)
    (. r = (short)a; .)

callop 0x13 (i32 a -- i32 r)
    (. r = helper(a); .)

ifop 0x14 (i32 a, i32 b -- i32 r)
    (. if (a > b) { r = a; } else { r = b; } .)
)OPGEN";

const opgen::Opcode* op_named(const opgen::Module* m, const char* mn) {
    for (auto* op : m->opcodes) if (!std::strcmp(op->mnemonic, mn)) return op;
    return nullptr;
}

using Mode = opgen::SemLowerer::Mode;

TEST(OpgenLower, Binop) {
    auto* m = parse(kSpec); ASSERT_NE(m, nullptr);
    EXPECT_EQ(lower_body(m, op_named(m, "add"), Mode::Interp),
              "    r = (int32_t)((a + b));\n");
}

TEST(OpgenLower, ShiftMasksToWidth) {
    auto* m = parse(kSpec); ASSERT_NE(m, nullptr);
    // i32 → shift count masked to 31 bits.
    EXPECT_EQ(lower_body(m, op_named(m, "shl"), Mode::Interp),
              "    r = (int32_t)((a << (b & 31)));\n");
}

TEST(OpgenLower, JavaPrimCast) {
    auto* m = parse(kSpec); ASSERT_NE(m, nullptr);
    EXPECT_EQ(lower_body(m, op_named(m, "cast"), Mode::Interp),
              "    r = (int32_t)(((s2)a));\n");
}

TEST(OpgenLower, NativeCallInterpVsStencil) {
    auto* m = parse(kSpec); ASSERT_NE(m, nullptr);
    // INTERP: direct call. The leading runtime context is the NATIVE_ARGS seam
    // (default `vm, vm->heap`; a backend redefines it to change the calling convention).
    EXPECT_EQ(lower_body(m, op_named(m, "callop"), Mode::Interp),
              "    r = (int32_t)((helper(NATIVE_ARGS, a)));\n");
    // STENCIL: indirect call through the _HOLE_ fn-pointer, with the synthesized
    // (ret(*)(vm_t*, heap_t*, params)) cast.
    EXPECT_EQ(lower_body(m, op_named(m, "callop"), Mode::Stencil),
              "    r = (int32_t)((((s4(*)(vm_t*, heap_t*, s4))(uintptr_t)_HOLE_helper)"
              "(NATIVE_ARGS, a)));\n");
}

// The `(_Bool)` is required, not cosmetic: lower_expr parenthesises a binary
// operator, so `if (` + expr + `)` would emit `if ((a > b))` — and for an
// equality that is clang's extraneous-parenthesised-comparison warning.
TEST(OpgenLower, IfElse) {
    auto* m = parse(kSpec); ASSERT_NE(m, nullptr);
    EXPECT_EQ(lower_body(m, op_named(m, "ifop"), Mode::Interp),
              "    if ((_Bool)(a > b)) {\n"
              "    r = (int32_t)(a);\n"
              "    } else {\n"
              "    r = (int32_t)(b);\n"
              "    }\n");
}

}  // namespace
