// opgen Spec tests — the derived per-opcode facts (length, stack effect,
// flags) computed from a parsed Module.
#include "Parser.h"
#include "spec.h"
#include <gtest/gtest.h>

namespace {

opgen::Module* parse(const char* src) {
    Parser p;
    p.init(src, (int)std::strlen(src));
    bool ok = p.parse();
    auto f = p.furthest();
    EXPECT_TRUE(ok) << "parse failed near line " << f.line << " col " << f.col;
    return ok ? p.ast : nullptr;
}

// `type` table makes i32 one slot, i64 two — so stack deltas follow the spec.
const char* kSpec = R"OPGEN(
type i32 int32_t uint32_t 1 i
type i64 int64_t uint64_t 2 l

iconst 0x10 [ u16 imm ] ( -- i32 r )

ireturn 0x20 ( i32 v -- )

goto 0x30 ( -- )
    flag: branch

invokestatic 0x40 ( -- )
    flag: special_invoke

iload 0x50 [ u8 idx ] ( -- i32 r )
)OPGEN";

TEST(OpgenSpec, LengthAndStackDelta) {
    auto* m = parse(kSpec);
    ASSERT_NE(m, nullptr);
    opgen::Spec spec(m);

    const auto& iconst = spec.derived(0x10);
    EXPECT_TRUE(iconst.present);
    EXPECT_EQ(iconst.length, 3);      // 1 opcode + 2 (u16 operand)
    EXPECT_EQ(iconst.sp_delta, 1);    // pushes one i32 (1 slot), pops nothing
    EXPECT_EQ(iconst.sp_pops, 0);

    const auto& iload = spec.derived(0x50);
    EXPECT_EQ(iload.length, 2);       // 1 opcode + 1 (u8 operand)
}

TEST(OpgenSpec, Flags) {
    auto* m = parse(kSpec);
    ASSERT_NE(m, nullptr);
    opgen::Spec spec(m);

    EXPECT_TRUE(spec.derived(0x20).flags & opgen::OPCF_RETURN);
    EXPECT_TRUE(spec.derived(0x20).flags & opgen::OPCF_ENDS_BB);
    EXPECT_TRUE(spec.derived(0x30).flags & opgen::OPCF_BRANCH);     // via `flag: branch`
    EXPECT_TRUE(spec.derived(0x30).flags & opgen::OPCF_ENDS_BB);
    EXPECT_TRUE(spec.derived(0x40).flags & opgen::OPCF_INVOKE);     // "invoke" prefix
}

TEST(OpgenSpec, SpecialOpcodesGetVariableSentinels) {
    auto* m = parse(kSpec);
    ASSERT_NE(m, nullptr);
    opgen::Spec spec(m);
    // `flag: special_invoke` → stack effect not statically encodable.
    EXPECT_EQ(spec.derived(0x40).sp_delta, opgen::OPCD_VARIABLE);
    EXPECT_EQ(spec.derived(0x40).sp_pops, opgen::OPCD_VARIABLE);
}

TEST(OpgenSpec, UnallocatedSlotsAbsent) {
    auto* m = parse(kSpec);
    ASSERT_NE(m, nullptr);
    opgen::Spec spec(m);
    EXPECT_FALSE(spec.derived(0x00).present);
    EXPECT_FALSE(spec.derived(0xFF).present);
}

TEST(OpgenSpec, TypeTableDrivesSlots) {
    auto* m = parse(kSpec);
    ASSERT_NE(m, nullptr);
    opgen::Spec spec(m);
    EXPECT_EQ(spec.stack_slots(opgen::ValueType::TyI32), 1);   // from `type` table
    EXPECT_EQ(spec.stack_slots(opgen::ValueType::TyI64), 2);
    EXPECT_EQ(opgen::Spec::type_bytes(opgen::ValueType::TyU16), 2);
}

}  // namespace
