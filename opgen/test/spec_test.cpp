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

// A spec that declares its opcodes without writing bodies for them: the
// semantics live in a hand-written interpreter and the spec exists for the
// tables. Every derived fact must then come from the declarations, because
// there is no body to read an effect out of.
const char* kBodylessSpec = R"OPGEN(
type i16 s2 u2 1 s
type i32 s4 u4 2 i

sload 0x16 [ u8 index ] ( -- i16 result)
    local_value: short

sstore 0x29 [ u8 index ] (i16 value -- )
    local_value: short

sload_2 0x1E ( -- i16 result)
    local_value: short
    local_index: 2

sinc 0x59 [ u8 index, i8 delta ] ( -- )
    local_value: short

sinc_w 0x96 [ u8 index, i16 delta ] ( -- )
    local_value: short
    narrow_form: sinc

ifeq 0x60 [ i8 offset ] (i16 value -- )
    branch: "value == 0" -> "pc - 2 + offset"
)OPGEN";

TEST(OpgenSpec, BodylessSpecDerivesLocalAndBranchFacts) {
    auto* m = parse(kBodylessSpec);
    ASSERT_NE(m, nullptr);
    opgen::Spec spec(m);

    // A local_value opcode that pushes is a load; one that pops is a store;
    // one with no stack effect that falls through is a read-modify-write.
    EXPECT_TRUE(spec.derived(0x16).flags & opgen::OPCF_LOCAL_LOAD);
    EXPECT_TRUE(spec.derived(0x29).flags & opgen::OPCF_LOCAL_STORE);
    EXPECT_TRUE(spec.derived(0x59).flags & opgen::OPCF_LOCAL_STORE);

    // The branch: annotation is what says an opcode branches.
    EXPECT_TRUE(spec.derived(0x60).flags & opgen::OPCF_BRANCH);
    EXPECT_TRUE(spec.derived(0x60).flags & opgen::OPCF_ENDS_BB);

    // local_index and narrow_form reach the derived table so the emitted
    // tables can carry them.
    EXPECT_EQ(spec.derived(0x1E).local_index, 2);
    EXPECT_EQ(spec.derived(0x16).local_index, -1);   // variable form
    EXPECT_EQ(spec.derived(0x96).narrow_form, 0x59); // sinc_w narrows to sinc
    EXPECT_EQ(spec.derived(0x59).narrow_form, 0);    // no narrower form
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
