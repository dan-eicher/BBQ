#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "ParseArena.h"
#include "StringPool.h"
#include "Capture.h"
#include "KontNode.h"
#include "Environment.h"
#include "Frame.h"
#include "Machine.h"

using namespace bbq;        // index runtime: ParseArena, FieldCapture, CaptureType, ...
using namespace bbq::cek;   // machine IR: Value, CEKMachine, ...

// Test helper: wrap an integer in an IntValue allocated in `arena`.
// Used by Binding initializers and add_field/add_computed calls in
// the migrated tests, since the new IR types those slots as `Value*`
// instead of the old `int64_t`.
static inline bbq::cek::Value* iv(bbq::ParseArena& arena, int64_t v) {
    auto* x = arena.alloc<bbq::cek::IntValue>();
    x->v = v;
    return x;
}

// Neutral computed scalar for the index builder. The index runtime stores a
// ComputedValue (no machine-IR dependency), not the machine's Value.
static inline bbq::ComputedValue* cv(bbq::ParseArena& arena, int64_t v) {
    auto* c = arena.alloc<bbq::ComputedValue>();
    c->kind = bbq::ComputedValue::Kind::Int;
    c->i = v;
    return c;
}

// ── ParseArena ─────────────────────────────────────────────

TEST(CEKParseArena, BasicAlloc) {
    ParseArena arena(1024);
    auto* p = arena.alloc<int>(42);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 42);
    EXPECT_GT(arena.bytes_used(), 0u);
}

TEST(CEKParseArena, MultipleAllocs) {
    ParseArena arena(1024);
    auto* a = arena.alloc<int>(1);
    auto* b = arena.alloc<int>(2);
    auto* c = arena.alloc<int>(3);
    EXPECT_EQ(*a, 1);
    EXPECT_EQ(*b, 2);
    EXPECT_EQ(*c, 3);
    // Distinct addresses
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
}

TEST(CEKParseArena, Alignment) {
    ParseArena arena(1024);
    arena.alloc<char>('x');  // 1-byte alloc to shift offset
    auto* p = arena.alloc<double>(3.14);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(double), 0u);
    EXPECT_EQ(*p, 3.14);
}

TEST(CEKParseArena, Reset) {
    ParseArena arena(1024);
    arena.alloc<int>(1);
    arena.alloc<int>(2);
    EXPECT_GT(arena.bytes_used(), 0u);
    arena.reset();
    EXPECT_EQ(arena.bytes_used(), 0u);
    // Can still alloc after reset
    auto* p = arena.alloc<int>(99);
    EXPECT_EQ(*p, 99);
}

TEST(CEKParseArena, Growth) {
    ParseArena arena(64);  // Small block size to force growth
    // Allocate more than one block's worth
    for (int i = 0; i < 100; i++) {
        auto* p = arena.alloc<int>(i);
        EXPECT_EQ(*p, i);
    }
    EXPECT_GE(arena.bytes_used(), 100 * sizeof(int));
}

// ── StringPool ─────────────────────────────────────────────

TEST(CEKStringPool, BasicIntern) {
    StringPool pool;
    const char* s = pool.intern("hello");
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "hello");
}

TEST(CEKStringPool, Dedup) {
    StringPool pool;
    const char* a = pool.intern("hello");
    const char* b = pool.intern("hello");
    EXPECT_EQ(a, b);  // Same pointer
}

TEST(CEKStringPool, DistinctStrings) {
    StringPool pool;
    const char* a = pool.intern("alpha");
    const char* b = pool.intern("beta");
    EXPECT_NE(a, b);
    EXPECT_STREQ(a, "alpha");
    EXPECT_STREQ(b, "beta");
}

TEST(CEKStringPool, PointerEquality) {
    StringPool pool;
    const char* x1 = pool.intern("field_x");
    const char* x2 = pool.intern(std::string("field_") + "x");
    const char* y = pool.intern("field_y");
    EXPECT_EQ(x1, x2);   // Same string, same pointer
    EXPECT_NE(x1, y);     // Different string, different pointer
}

// ── KontNode ───────────────────────────────────────────────

TEST(CEKKontNode, PrimitiveInfoByteCount) {
    // Integer widths
    PrimitiveInfo p8{PrimKind::Integer, PrimWidth::W8};
    EXPECT_EQ(p8.byte_count(), 1u);
    PrimitiveInfo p16{PrimKind::Integer, PrimWidth::W16};
    EXPECT_EQ(p16.byte_count(), 2u);
    PrimitiveInfo p32{PrimKind::Integer, PrimWidth::W32};
    EXPECT_EQ(p32.byte_count(), 4u);
    PrimitiveInfo p64{PrimKind::Integer, PrimWidth::W64};
    EXPECT_EQ(p64.byte_count(), 8u);

    // Float
    PrimitiveInfo f32{PrimKind::Float, PrimWidth::W32};
    EXPECT_EQ(f32.byte_count(), 4u);
    PrimitiveInfo f64{PrimKind::Float, PrimWidth::W64};
    EXPECT_EQ(f64.byte_count(), 8u);

    // Bool
    PrimitiveInfo b{PrimKind::Bool};
    EXPECT_EQ(b.byte_count(), 1u);
}

// ── Environment ────────────────────────────────────────────

// CEKEnvironment tests — re-enabled with iv() wrapping for the typed
// Value* in Binding (was int64_t pre-rebuild).

TEST(CEKEnvironment, EmptyLookup) {
    // Null environment: lookup returns nullptr
    const Environment* env = nullptr;
    // Can't call member on nullptr; design handles this in usage
    // Verify a single-entry env doesn't find a wrong name
    ParseArena arena;
    StringPool pool;
    auto* name = pool.intern("x");
    auto* other = pool.intern("y");

    auto* e = arena.alloc<Environment>();
    e->binding = {name, iv(arena, 42), 0, 1, CaptureType::UInt8};
    e->parent = nullptr;

    EXPECT_EQ(e->lookup(other), nullptr);
    (void)env;
}

TEST(CEKEnvironment, SingleBinding) {
    ParseArena arena;
    StringPool pool;
    auto* name = pool.intern("value");

    auto* e = arena.alloc<Environment>();
    e->binding = {name, iv(arena, 100), 0, 4, CaptureType::UInt32LE};
    e->parent = nullptr;

    auto* found = e->lookup(name);
    ASSERT_NE(found, nullptr);
    ASSERT_NE(found->value, nullptr);
    EXPECT_EQ(found->value->tag, ValueTag::IntValue);
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(found->value)->v, 100);
    EXPECT_EQ(found->start_offset, 0u);
    EXPECT_EQ(found->end_offset, 4u);
}

TEST(CEKEnvironment, ChainLookup) {
    ParseArena arena;
    StringPool pool;
    auto* x = pool.intern("x");
    auto* y = pool.intern("y");

    auto* e1 = arena.alloc<Environment>();
    e1->binding = {x, iv(arena, 10), 0, 1, CaptureType::UInt8};
    e1->parent = nullptr;

    auto* e2 = arena.alloc<Environment>();
    e2->binding = {y, iv(arena, 20), 1, 3, CaptureType::UInt16LE};
    e2->parent = e1;

    // Find y in current
    auto* fy = e2->lookup(y);
    ASSERT_NE(fy, nullptr);
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(fy->value)->v, 20);

    // Find x in parent
    auto* fx = e2->lookup(x);
    ASSERT_NE(fx, nullptr);
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(fx->value)->v, 10);
}

TEST(CEKEnvironment, Shadowing) {
    ParseArena arena;
    StringPool pool;
    auto* name = pool.intern("x");

    auto* e1 = arena.alloc<Environment>();
    e1->binding = {name, iv(arena, 1), 0, 1, CaptureType::UInt8};
    e1->parent = nullptr;

    auto* e2 = arena.alloc<Environment>();
    e2->binding = {name, iv(arena, 2), 1, 2, CaptureType::UInt8};
    e2->parent = e1;

    // Should find the most recent binding
    auto* found = e2->lookup(name);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(found->value)->v, 2);
}

// ── CaptureBuilder ─────────────────────────────────────────
// Re-enabled with iv() wrapping for the typed Value* in computed_value
// (was int64_t pre-rebuild).

TEST(CEKCaptureBuilder, AddField) {
    ParseArena arena;
    CaptureBuilder builder;
    builder.add_field(nullptr, 0, 1, CaptureType::UInt8, cv(arena, 42));
    builder.add_field(nullptr, 1, 3, CaptureType::UInt16LE, cv(arena, 0x0201));
    EXPECT_EQ(builder.fields().size(), 2u);
    EXPECT_EQ(builder.fields()[0].computed_value->i, 42);
    EXPECT_EQ(builder.fields()[1].computed_value->i, 0x0201);
}

TEST(CEKCaptureBuilder, MarkRestore) {
    ParseArena arena;
    CaptureBuilder builder;
    builder.add_field(nullptr, 0, 1, CaptureType::UInt8, cv(arena, 1));
    auto m = builder.mark();
    builder.add_field(nullptr, 1, 2, CaptureType::UInt8, cv(arena, 2));
    builder.add_field(nullptr, 2, 3, CaptureType::UInt8, cv(arena, 3));
    EXPECT_EQ(builder.fields().size(), 3u);

    builder.restore(m);
    EXPECT_EQ(builder.fields().size(), 1u);
    EXPECT_EQ(builder.fields()[0].computed_value->i, 1);
}

TEST(CEKCaptureBuilder, AddComputed) {
    ParseArena arena;
    StringPool pool;
    CaptureBuilder builder;
    const char* nm = pool.intern("derived");
    builder.add_computed(nm, cv(arena, 0xABCD), /*pos=*/8);
    ASSERT_EQ(builder.fields().size(), 1u);
    const auto& f = builder.fields()[0];
    EXPECT_EQ(f.name, nm);
    EXPECT_EQ(f.type, CaptureType::Computed);
    EXPECT_EQ(f.start_offset, 8u);
    EXPECT_EQ(f.end_offset, 8u);   // zero-width at pos
    ASSERT_NE(f.computed_value, nullptr);
    EXPECT_EQ(f.computed_value->i, 0xABCD);
    EXPECT_EQ(f.children, nullptr);
    EXPECT_EQ(f.child_count, 0);
}


// ─────────────────────────────────────────────────────────────────────
//  Layer 1 invariant tests for the rebuilt CEK IR.
//
//  These exercise the asdl-generated header shape: Value sum + tag
//  dispatch, KontNode root + sum bases, sub-tree ownership, the
//  expect<T> helper. They do NOT exercise runtime invokes (those are
//  stubs through Layer 1; Layer 2 fills them in).
// ─────────────────────────────────────────────────────────────────────

TEST(CEKLayer1IR, ValueTagDispatch) {
    ParseArena arena;
    auto* iv = arena.alloc<IntValue>();
    iv->v = 42;
    EXPECT_EQ(iv->tag, ValueTag::IntValue);
    EXPECT_EQ(iv->v, 42);

    auto* fv = arena.alloc<FloatValue>();
    fv->v = 3.14f;
    EXPECT_EQ(fv->tag, ValueTag::FloatValue);

    auto* sv = arena.alloc<StringValue>();
    sv->v = "hello";
    EXPECT_EQ(sv->tag, ValueTag::StringValue);

    auto* bv = arena.alloc<BoolValue>();
    bv->v = true;
    EXPECT_EQ(bv->tag, ValueTag::BoolValue);

    auto* cv = arena.alloc<FieldCaptureValue>();
    cv->capture = nullptr;
    EXPECT_EQ(cv->tag, ValueTag::FieldCaptureValue);
}

TEST(CEKLayer1IR, ValueKindConstant) {
    EXPECT_EQ(IntValue::kind, ValueTag::IntValue);
    EXPECT_EQ(FloatValue::kind, ValueTag::FloatValue);
    EXPECT_EQ(StringValue::kind, ValueTag::StringValue);
    EXPECT_EQ(BoolValue::kind, ValueTag::BoolValue);
    EXPECT_EQ(FieldCaptureValue::kind, ValueTag::FieldCaptureValue);
}

TEST(CEKLayer1IR, KontNodeIsBaseOfStaticAndDynamic) {
    ParseArena arena;
    auto* sk = arena.alloc<HaltNode>();
    auto* dk = arena.alloc<NormalizeKont>();
    KontNode* kn1 = sk;
    KontNode* kn2 = dk;
    EXPECT_NE(kn1, nullptr);
    EXPECT_NE(kn2, nullptr);
    // Both live under KontNode — the trampoline only sees this root.
    StaticKont* check_static = sk;
    DynamicKont* check_dynamic = dk;
    (void)check_static;
    (void)check_dynamic;
}

TEST(CEKLayer1IR, OperatorSubTreeOwnership) {
    ParseArena arena;
    // AddKont(IntLitKont(2), IntLitKont(3)) — operator carries
    // operand sub-trees by pointer; mutating one leaf's value
    // doesn't touch the other.
    auto* l = arena.alloc<IntLitKont>();
    l->v = 2;
    auto* r = arena.alloc<IntLitKont>();
    r->v = 3;
    auto* add = arena.alloc<AddKont>();
    add->left = l;
    add->right = r;

    EXPECT_EQ(static_cast<IntLitKont*>(add->left)->v, 2);
    EXPECT_EQ(static_cast<IntLitKont*>(add->right)->v, 3);
    l->v = 99;
    EXPECT_EQ(static_cast<IntLitKont*>(add->left)->v, 99);
    EXPECT_EQ(static_cast<IntLitKont*>(add->right)->v, 3);
}

TEST(CEKLayer1IR, SpineKontHasNextField) {
    ParseArena arena;
    auto* halt = arena.alloc<HaltNode>();
    auto* match = arena.alloc<MatchPrimitiveNode>();
    match->next = halt;
    EXPECT_EQ(match->next, halt);
}

TEST(CEKLayer1IR, ExpressionKontHasNoNextField) {
    // Compile-time: AddKont has `left` and `right` but NO `next`. If
    // a `next` field crept back in, this would be the wrong shape.
    // Verified at compile time: any attempt to access add->next would
    // be a compile error. We exercise the actual fields here.
    ParseArena arena;
    auto* add = arena.alloc<AddKont>();
    add->left = nullptr;
    add->right = nullptr;
    EXPECT_EQ(add->left, nullptr);
    EXPECT_EQ(add->right, nullptr);
}

// ─────────────────────────────────────────────────────────────────────
//  Layer 2a tests: leaf konts + K-pop trampoline + backtracking.
//
//  Each leaf invoked through execute_from() should produce the right
//  typed Value in m->result. Trampoline should drain K then exit. fail()
//  with no AlternativeFrame should set failed=true.
// ─────────────────────────────────────────────────────────────────────

namespace {
// Helper: run a single kont through the trampoline and return the
// machine's final state. Caller pre-builds the IR in `arena`.
struct LeafResult {
    Value* result;
    bool failed;
    const char* error_msg;
};

LeafResult run_leaf(KontNode* entry, ParseArena& arena,
                    const uint8_t* input = nullptr, size_t length = 0,
                    Environment* env = nullptr) {
    CEKMachine m;
    m.arena = &arena;
    m.env = env;
    auto meta = m.execute_from(entry, input, length, /*little_endian=*/true);
    (void)meta;
    return {m.result, m.failed, m.best_error_msg};
}
}

TEST(CEKLayer2Leaves, IntLit) {
    ParseArena arena;
    auto* k = arena.alloc<IntLitKont>();
    k->v = 42;
    auto r = run_leaf(k, arena);
    ASSERT_NE(r.result, nullptr);
    ASSERT_EQ(r.result->tag, ValueTag::IntValue);
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, 42);
    EXPECT_FALSE(r.failed);
}

TEST(CEKLayer2Leaves, BoolLit) {
    ParseArena arena;
    auto* k = arena.alloc<BoolLitKont>();
    k->v = true;
    auto r = run_leaf(k, arena);
    ASSERT_NE(r.result, nullptr);
    ASSERT_EQ(r.result->tag, ValueTag::BoolValue);
    EXPECT_TRUE(static_cast<BoolValue*>(r.result)->v);
}

TEST(CEKLayer2Leaves, FloatLit) {
    ParseArena arena;
    auto* k = arena.alloc<FloatLitKont>();
    k->v = 3.14f;
    auto r = run_leaf(k, arena);
    ASSERT_NE(r.result, nullptr);
    ASSERT_EQ(r.result->tag, ValueTag::FloatValue);
    EXPECT_FLOAT_EQ(static_cast<FloatValue*>(r.result)->v, 3.14f);
}

TEST(CEKLayer2Leaves, StrLit) {
    ParseArena arena;
    auto* k = arena.alloc<StrLitKont>();
    k->v = "hello";
    auto r = run_leaf(k, arena);
    ASSERT_NE(r.result, nullptr);
    ASSERT_EQ(r.result->tag, ValueTag::StringValue);
    EXPECT_STREQ(static_cast<StringValue*>(r.result)->v, "hello");
}

TEST(CEKLayer2Leaves, RefHits) {
    ParseArena arena;
    StringPool pool;
    const char* nm = pool.intern("x");

    // Build env: x → IntValue(7)
    auto* iv = arena.alloc<IntValue>();
    iv->v = 7;
    auto* env = arena.alloc<Environment>();
    env->binding.name = nm;
    env->binding.value = iv;
    env->parent = nullptr;

    auto* k = arena.alloc<RefKont>();
    k->name = nm;
    auto r = run_leaf(k, arena, nullptr, 0, env);
    ASSERT_NE(r.result, nullptr);
    EXPECT_EQ(r.result, iv);  // RefKont copies the pointer
    EXPECT_FALSE(r.failed);
}

TEST(CEKLayer2Leaves, RefMissesFails) {
    ParseArena arena;
    StringPool pool;
    auto* k = arena.alloc<RefKont>();
    k->name = pool.intern("missing");
    auto r = run_leaf(k, arena);
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.error_msg, nullptr);
}

TEST(CEKLayer2Leaves, Pos) {
    ParseArena arena;
    auto* k = arena.alloc<PosKont>();
    uint8_t buf[8] = {};
    auto r = run_leaf(k, arena, buf, 8);
    ASSERT_NE(r.result, nullptr);
    ASSERT_EQ(r.result->tag, ValueTag::IntValue);
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, 0);  // pos starts at 0
}

TEST(CEKLayer2Leaves, Remaining) {
    ParseArena arena;
    auto* k = arena.alloc<RemainingKont>();
    uint8_t buf[16] = {};
    auto r = run_leaf(k, arena, buf, 16);
    ASSERT_NE(r.result, nullptr);
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, 16);
}

TEST(CEKLayer2Leaves, EOI) {
    ParseArena arena;
    auto* k = arena.alloc<EOIKont>();
    uint8_t buf[7] = {};
    auto r = run_leaf(k, arena, buf, 7);
    ASSERT_NE(r.result, nullptr);
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, 7);
}

TEST(CEKLayer2Leaves, LoopVarOutsideLoopFails) {
    ParseArena arena;
    auto* k = arena.alloc<LoopVarKont>();
    auto r = run_leaf(k, arena);
    EXPECT_TRUE(r.failed);
}

TEST(CEKLayer2Leaves, LoopVarReadsInnermostLoop) {
    ParseArena arena;
    auto* outer = arena.alloc<LoopFrame>();
    outer->counter = 5;
    outer->prev = nullptr;
    auto* inner = arena.alloc<LoopFrame>();
    inner->counter = 9;
    inner->prev = outer;

    auto* k = arena.alloc<LoopVarKont>();
    CEKMachine m;
    m.arena = &arena;
    m.frame_top = inner;
    auto meta = m.execute_from(k, nullptr, 0, true);
    (void)meta;

    ASSERT_NE(m.result, nullptr);
    ASSERT_EQ(m.result->tag, ValueTag::IntValue);
    EXPECT_EQ(static_cast<IntValue*>(m.result)->v, 9);
}

TEST(CEKLayer2Trampoline, HaltExitsLoopCleanly) {
    ParseArena arena;
    auto* halt = arena.alloc<HaltNode>();
    CEKMachine m;
    m.arena = &arena;
    auto meta = m.execute_from(halt, nullptr, 0, true);
    EXPECT_FALSE(m.failed);
    EXPECT_TRUE(meta.success);
}

TEST(CEKLayer2Spine, BeginEndStructNoFields) {
    // Build IR: BeginStruct("S") → EndStruct → Halt
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* end_st = arena.alloc<EndStructNode>();
    end_st->next = halt;
    auto* begin_st = arena.alloc<BeginStructNode>();
    begin_st->struct_name = pool.intern("S");
    begin_st->next = end_st;

    CEKMachine m;
    m.arena = &arena;
    auto meta = m.execute_from(begin_st, nullptr, 0, true);
    EXPECT_FALSE(m.failed);
    EXPECT_TRUE(meta.success);
    ASSERT_NE(meta.doc.root(), nullptr);
    ASSERT_EQ(meta.doc.root()->kids.size(), 1);
    EXPECT_STREQ(meta.doc.root()->kids[0]->name, "S");
    EXPECT_EQ(meta.doc.root()->kids[0]->type, CaptureType::Struct);
}

TEST(CEKLayer2Spine, KPushOrder) {
    // Verify that BeginStruct → next → EndStruct → next → Halt
    // sequence executes via K push/pop in the right order.
    // After three konts, K should be empty.
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* end_st = arena.alloc<EndStructNode>();
    end_st->next = halt;
    auto* begin_st = arena.alloc<BeginStructNode>();
    begin_st->struct_name = pool.intern("Z");
    begin_st->next = end_st;

    CEKMachine m;
    m.arena = &arena;
    auto meta = m.execute_from(begin_st, nullptr, 0, true);
    EXPECT_TRUE(meta.success);
}

// ── MatchPrimitive ─────────────────────────────────────────

namespace {
// Helper: build MatchPrimitive(name, prim) → Halt and run it on `data`.
struct MatchResult {
    bool success;
    Value* binding_value;
    Environment* env;
    const char* error_msg;
    zcow::parse_result meta;
};

MatchResult run_match_prim(ParseArena& arena, const char* name,
                           PrimitiveInfo prim,
                           const uint8_t* data, size_t length,
                           bool little_endian = true) {
    auto* halt = arena.alloc<HaltNode>();
    // producer → Capture → StoreEnv → halt
    auto* st_env = arena.alloc<StoreEnvKont>();
    st_env->name = name;
    st_env->next = halt;
    auto* cap = arena.alloc<CaptureKont>();
    cap->name = name;
    cap->next = st_env;
    auto* match = arena.alloc<MatchPrimitiveNode>();
    match->prim = prim;
    match->next = cap;

    CEKMachine m;
    m.arena = &arena;
    auto meta = m.execute_from(match, data, length, little_endian);
    Value* bv = (m.env && m.env->binding.name == name) ? m.env->binding.value : nullptr;
    return {!m.failed, bv, m.env, m.best_error_msg, meta};
}
}

TEST(CEKLayer2MatchPrim, Uint8) {
    ParseArena arena;
    StringPool pool;
    PrimitiveInfo prim{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Native};
    uint8_t data[] = {0x42};
    auto r = run_match_prim(arena, pool.intern("x"), prim, data, sizeof(data));
    ASSERT_TRUE(r.success) << (r.error_msg ? r.error_msg : "");
    ASSERT_NE(r.binding_value, nullptr);
    ASSERT_EQ(r.binding_value->tag, ValueTag::IntValue);
    EXPECT_EQ(static_cast<IntValue*>(r.binding_value)->v, 0x42);

    // Capture recorded
    ASSERT_NE(r.meta.doc.root(), nullptr);
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 1);
    EXPECT_EQ(r.meta.doc.root()->kids[0]->start_offset, 0u);
    EXPECT_EQ(r.meta.doc.root()->kids[0]->end_offset, 1u);
    EXPECT_EQ(r.meta.doc.root()->kids[0]->type, CaptureType::UInt8);
}

TEST(CEKLayer2MatchPrim, Uint32LE) {
    ParseArena arena;
    StringPool pool;
    PrimitiveInfo prim{PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Little};
    uint8_t data[] = {0x78, 0x56, 0x34, 0x12};   // 0x12345678 LE
    auto r = run_match_prim(arena, pool.intern("u"), prim, data, sizeof(data));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(static_cast<IntValue*>(r.binding_value)->v, 0x12345678);
}

TEST(CEKLayer2MatchPrim, Uint32BE) {
    ParseArena arena;
    StringPool pool;
    PrimitiveInfo prim{PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Big};
    uint8_t data[] = {0x12, 0x34, 0x56, 0x78};   // 0x12345678 BE
    auto r = run_match_prim(arena, pool.intern("u"), prim, data, sizeof(data));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(static_cast<IntValue*>(r.binding_value)->v, 0x12345678);
}

TEST(CEKLayer2MatchPrim, FloatPreservedNotTruncated) {
    // Crucial test for the new design: float fields stay typed as
    // FloatValue, not silently truncated to IntValue.
    ParseArena arena;
    StringPool pool;
    PrimitiveInfo prim{PrimKind::Float, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Little};
    // 3.14f as little-endian bytes
    float src = 3.14f;
    uint8_t data[4];
    std::memcpy(data, &src, 4);
    auto r = run_match_prim(arena, pool.intern("f"), prim, data, sizeof(data));
    ASSERT_TRUE(r.success);
    ASSERT_NE(r.binding_value, nullptr);
    ASSERT_EQ(r.binding_value->tag, ValueTag::FloatValue);
    EXPECT_DOUBLE_EQ(static_cast<FloatValue*>(r.binding_value)->v, static_cast<double>(3.14f));
}

TEST(CEKLayer2MatchPrim, BoolFromByte) {
    ParseArena arena;
    StringPool pool;
    PrimitiveInfo prim{PrimKind::Bool, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Native};
    uint8_t data[] = {0x01};
    auto r = run_match_prim(arena, pool.intern("b"), prim, data, sizeof(data));
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.binding_value->tag, ValueTag::BoolValue);
    EXPECT_TRUE(static_cast<BoolValue*>(r.binding_value)->v);
}

TEST(CEKLayer2MatchPrim, BoundsCheckFails) {
    ParseArena arena;
    StringPool pool;
    PrimitiveInfo prim{PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Little};
    uint8_t data[] = {0x01, 0x02};   // Only 2 bytes; need 4
    auto r = run_match_prim(arena, pool.intern("u"), prim, data, sizeof(data));
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error_msg, nullptr);
}

// ── Expression operator/apply tests ───────────────────────

namespace {
// Helper: evaluate an expression IR through the trampoline, return ac.
struct ExprResult {
    Value* result;
    bool failed;
    const char* error_msg;
};

ExprResult run_expr(KontNode* expr_root, ParseArena& arena, Environment* env = nullptr) {
    CEKMachine m;
    m.arena = &arena;
    m.env = env;
    auto meta = m.execute_from(expr_root, nullptr, 0, true);
    (void)meta;
    return {m.result, m.failed, m.best_error_msg};
}

// Variant for tests that exercise built-in function calls (abs/min/...).
// Built-in dispatch is by interned-pointer equality, so the test's
// StringPool — the one used to intern the call's func_name — must also
// be where the BuiltinFnTable's names are interned.
ExprResult run_expr_with_builtins(KontNode* expr_root, ParseArena& arena,
                                   StringPool& pool, Environment* env = nullptr) {
    BuiltinFnTable table;
    populate_builtins_into(table, arena, pool);

    CEKMachine m;
    m.arena = &arena;
    m.env = env;
    m.builtins = &table;
    auto meta = m.execute_from(expr_root, nullptr, 0, true);
    (void)meta;
    return {m.result, m.failed, m.best_error_msg};
}

IntLitKont* int_lit(ParseArena& a, int64_t v) {
    auto* k = a.alloc<IntLitKont>();
    k->v = v;
    return k;
}

BoolLitKont* bool_lit(ParseArena& a, bool v) {
    auto* k = a.alloc<BoolLitKont>();
    k->v = v;
    return k;
}

FloatLitKont* float_lit(ParseArena& a, double v) {
    auto* k = a.alloc<FloatLitKont>();
    k->v = v;
    return k;
}

StrLitKont* str_lit(ParseArena& a, const char* v) {
    auto* k = a.alloc<StrLitKont>();
    k->v = v;
    return k;
}

template<typename OpKont>
OpKont* binop(ParseArena& a, KontNode* l, KontNode* r) {
    auto* k = a.alloc<OpKont>();
    k->left = l;
    k->right = r;
    return k;
}
}

TEST(CEKLayer2Binop, AddInt) {
    ParseArena arena;
    auto* k = binop<AddKont>(arena, int_lit(arena, 2), int_lit(arena, 3));
    auto r = run_expr(k, arena);
    ASSERT_FALSE(r.failed) << (r.error_msg ? r.error_msg : "");
    ASSERT_EQ(r.result->tag, ValueTag::IntValue);
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, 5);
}

TEST(CEKLayer2Binop, AddFloat) {
    ParseArena arena;
    auto* k = binop<AddKont>(arena, float_lit(arena, 1.5), float_lit(arena, 2.25));
    auto r = run_expr(k, arena);
    ASSERT_FALSE(r.failed);
    ASSERT_EQ(r.result->tag, ValueTag::FloatValue);
    EXPECT_DOUBLE_EQ(static_cast<FloatValue*>(r.result)->v, 3.75);
}

TEST(CEKLayer2Binop, AddTypeMismatchFails) {
    ParseArena arena;
    auto* k = binop<AddKont>(arena, int_lit(arena, 1), str_lit(arena, "x"));
    auto r = run_expr(k, arena);
    EXPECT_TRUE(r.failed);
}

// CEK arithmetic must match the SAME C++ expression. C's usual arithmetic conversions
// are the standard (the C/C++ backends emit native C; the CEK is the conformer). The C++
// literal on the right is constant-folded = the C-correct answer. Edge cases, not happy path:
// negative-division/mod truncation, non-commutative ops in both operand orders, comparison
// boundaries, mixed int/float promotion.
TEST(CEKLayer2Binop, ArithConformsToC) {
    ParseArena arena;
    auto V = [&](KontNode* k) { auto r = run_expr(k, arena); EXPECT_FALSE(r.failed); return r.result; };
#define II(op, a, b) static_cast<IntValue*>(V(binop<op##Kont>(arena, int_lit(arena, a), int_lit(arena, b))))->v
#define IF(op, a, b) static_cast<FloatValue*>(V(binop<op##Kont>(arena, int_lit(arena, a), float_lit(arena, b))))->v
#define FI(op, a, b) static_cast<FloatValue*>(V(binop<op##Kont>(arena, float_lit(arena, a), int_lit(arena, b))))->v
#define BIF(op, a, b) static_cast<BoolValue*>(V(binop<op##Kont>(arena, int_lit(arena, a), float_lit(arena, b))))->v
#define BFI(op, a, b) static_cast<BoolValue*>(V(binop<op##Kont>(arena, float_lit(arena, a), int_lit(arena, b))))->v

    // int/int: division truncates toward zero, % sign follows the dividend (C semantics)
    EXPECT_EQ(II(Div, 7, 2),   7 / 2);
    EXPECT_EQ(II(Div, -7, 2),  -7 / 2);
    EXPECT_EQ(II(Mod, -7, 3),  -7 % 3);
    EXPECT_EQ(II(Mod, 7, -3),  7 % -3);

    // mixed int/float promotes to float; both operand orders for non-commutative ops
    EXPECT_DOUBLE_EQ(IF(Div, 5, 2.0),  5 / 2.0);
    EXPECT_DOUBLE_EQ(FI(Div, 5.0, 2),  5.0 / 2);
    EXPECT_DOUBLE_EQ(IF(Sub, 1, 0.25), 1 - 0.25);
    EXPECT_DOUBLE_EQ(FI(Sub, 0.25, 1), 0.25 - 1);
    EXPECT_DOUBLE_EQ(IF(Add, 3, 1.5),  3 + 1.5);
    EXPECT_DOUBLE_EQ(IF(Mul, -4, 2.5), -4 * 2.5);

    // comparisons across the int/float boundary
    EXPECT_EQ(BIF(Eq, 3, 3.0), 3 == 3.0);
    EXPECT_EQ(BIF(Ne, 3, 3.0), 3 != 3.0);
    EXPECT_EQ(BIF(Lt, 3, 3.0), 3 < 3.0);
    EXPECT_EQ(BIF(Le, 3, 3.0), 3 <= 3.0);
    EXPECT_EQ(BFI(Gt, 3.5, 3), 3.5 > 3);
    EXPECT_EQ(BIF(Ge, 3, 3.5), 3 >= 3.5);

    // genuinely incompatible operands still fail (not coerced)
    EXPECT_TRUE(run_expr(binop<AddKont>(arena, int_lit(arena, 1), str_lit(arena, "x")), arena).failed);
#undef II
#undef IF
#undef FI
#undef BIF
#undef BFI
}

TEST(CEKLayer2Binop, MulSubDivMod) {
    ParseArena arena;
    EXPECT_EQ(static_cast<IntValue*>(run_expr(
        binop<MulKont>(arena, int_lit(arena, 4), int_lit(arena, 5)), arena).result)->v, 20);
    EXPECT_EQ(static_cast<IntValue*>(run_expr(
        binop<SubKont>(arena, int_lit(arena, 10), int_lit(arena, 3)), arena).result)->v, 7);
    EXPECT_EQ(static_cast<IntValue*>(run_expr(
        binop<DivKont>(arena, int_lit(arena, 17), int_lit(arena, 5)), arena).result)->v, 3);
    EXPECT_EQ(static_cast<IntValue*>(run_expr(
        binop<ModKont>(arena, int_lit(arena, 17), int_lit(arena, 5)), arena).result)->v, 2);
}

TEST(CEKLayer2Binop, DivByZeroFails) {
    ParseArena arena;
    auto r = run_expr(binop<DivKont>(arena, int_lit(arena, 1), int_lit(arena, 0)), arena);
    EXPECT_TRUE(r.failed);
}

TEST(CEKLayer2Binop, BitwiseOps) {
    ParseArena arena;
    EXPECT_EQ(static_cast<IntValue*>(run_expr(
        binop<BitAndKont>(arena, int_lit(arena, 0xF0), int_lit(arena, 0xFF)), arena).result)->v, 0xF0);
    EXPECT_EQ(static_cast<IntValue*>(run_expr(
        binop<BitOrKont>(arena, int_lit(arena, 0x0F), int_lit(arena, 0xF0)), arena).result)->v, 0xFF);
    EXPECT_EQ(static_cast<IntValue*>(run_expr(
        binop<BitXorKont>(arena, int_lit(arena, 0xFF), int_lit(arena, 0x0F)), arena).result)->v, 0xF0);
    EXPECT_EQ(static_cast<IntValue*>(run_expr(
        binop<ShlKont>(arena, int_lit(arena, 1), int_lit(arena, 4)), arena).result)->v, 16);
    EXPECT_EQ(static_cast<IntValue*>(run_expr(
        binop<ShrKont>(arena, int_lit(arena, 16), int_lit(arena, 2)), arena).result)->v, 4);
}

TEST(CEKLayer2Binop, EqualityCrossTypeReturnsFalse) {
    ParseArena arena;
    auto r = run_expr(binop<EqKont>(arena, int_lit(arena, 1), str_lit(arena, "1")), arena);
    ASSERT_FALSE(r.failed);
    ASSERT_EQ(r.result->tag, ValueTag::BoolValue);
    EXPECT_FALSE(static_cast<BoolValue*>(r.result)->v);
}

TEST(CEKLayer2Binop, EqualitySameTypeWorks) {
    ParseArena arena;
    EXPECT_TRUE(static_cast<BoolValue*>(run_expr(
        binop<EqKont>(arena, int_lit(arena, 5), int_lit(arena, 5)), arena).result)->v);
    EXPECT_FALSE(static_cast<BoolValue*>(run_expr(
        binop<EqKont>(arena, int_lit(arena, 5), int_lit(arena, 6)), arena).result)->v);
}

TEST(CEKLayer2Binop, OrderedCompareFailsCrossType) {
    ParseArena arena;
    auto r = run_expr(binop<LtKont>(arena, int_lit(arena, 1), str_lit(arena, "x")), arena);
    EXPECT_TRUE(r.failed);
}

TEST(CEKLayer2Binop, OrderedCompareSameType) {
    ParseArena arena;
    EXPECT_TRUE(static_cast<BoolValue*>(run_expr(
        binop<LtKont>(arena, int_lit(arena, 1), int_lit(arena, 2)), arena).result)->v);
    EXPECT_TRUE(static_cast<BoolValue*>(run_expr(
        binop<GtKont>(arena, int_lit(arena, 5), int_lit(arena, 3)), arena).result)->v);
}

TEST(CEKLayer2Logical, AndShortCircuit) {
    ParseArena arena;
    // false && (anything) = false; right operand should not be evaluated.
    // Use a kont whose invoke would fail to detect inadvertent eval.
    auto* divzero = binop<DivKont>(arena, int_lit(arena, 1), int_lit(arena, 0));
    auto* and_kont = arena.alloc<LogicalAndKont>();
    and_kont->left = bool_lit(arena, false);
    and_kont->right = divzero;
    auto r = run_expr(and_kont, arena);
    ASSERT_FALSE(r.failed);
    ASSERT_EQ(r.result->tag, ValueTag::BoolValue);
    EXPECT_FALSE(static_cast<BoolValue*>(r.result)->v);
}

TEST(CEKLayer2Logical, OrShortCircuit) {
    ParseArena arena;
    auto* divzero = binop<DivKont>(arena, int_lit(arena, 1), int_lit(arena, 0));
    auto* or_kont = arena.alloc<LogicalOrKont>();
    or_kont->left = bool_lit(arena, true);
    or_kont->right = divzero;
    auto r = run_expr(or_kont, arena);
    ASSERT_FALSE(r.failed);
    EXPECT_TRUE(static_cast<BoolValue*>(r.result)->v);
}

TEST(CEKLayer2Logical, AndDoesEvaluateRightWhenLeftTrue) {
    ParseArena arena;
    auto* and_kont = arena.alloc<LogicalAndKont>();
    and_kont->left = bool_lit(arena, true);
    and_kont->right = bool_lit(arena, true);
    auto r = run_expr(and_kont, arena);
    ASSERT_FALSE(r.failed);
    EXPECT_TRUE(static_cast<BoolValue*>(r.result)->v);
}

TEST(CEKLayer2Ternary, PicksThenWhenTrue) {
    ParseArena arena;
    auto* t = arena.alloc<TernaryKont>();
    t->cond = bool_lit(arena, true);
    t->then_ = int_lit(arena, 100);
    t->else_ = int_lit(arena, 200);
    auto r = run_expr(t, arena);
    ASSERT_FALSE(r.failed);
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, 100);
}

TEST(CEKLayer2Ternary, PicksElseWhenFalse) {
    ParseArena arena;
    auto* t = arena.alloc<TernaryKont>();
    t->cond = bool_lit(arena, false);
    t->then_ = int_lit(arena, 100);
    t->else_ = int_lit(arena, 200);
    auto r = run_expr(t, arena);
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, 200);
}

TEST(CEKLayer2Unary, NegInt) {
    ParseArena arena;
    auto* k = arena.alloc<NegKont>();
    k->operand = int_lit(arena, 42);
    auto r = run_expr(k, arena);
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, -42);
}

TEST(CEKLayer2Unary, NotBool) {
    ParseArena arena;
    auto* k = arena.alloc<NotKont>();
    k->operand = bool_lit(arena, true);
    auto r = run_expr(k, arena);
    EXPECT_FALSE(static_cast<BoolValue*>(r.result)->v);
}

TEST(CEKLayer2Call, AbsInt) {
    ParseArena arena;
    StringPool pool;
    auto* call = arena.alloc<CallKont>();
    call->func_name = pool.intern("abs");
    auto** args = static_cast<KontNode**>(arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    args[0] = int_lit(arena, -7);
    call->args = args;
    call->args_count = 1;
    auto r = run_expr_with_builtins(call, arena, pool);
    ASSERT_FALSE(r.failed) << (r.error_msg ? r.error_msg : "");
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, 7);
}

TEST(CEKLayer2Call, MinTwoInts) {
    ParseArena arena;
    StringPool pool;
    auto* call = arena.alloc<CallKont>();
    call->func_name = pool.intern("min");
    auto** args = static_cast<KontNode**>(arena.allocate(sizeof(KontNode*) * 2, alignof(KontNode*)));
    args[0] = int_lit(arena, 5);
    args[1] = int_lit(arena, 3);
    call->args = args;
    call->args_count = 2;
    auto r = run_expr_with_builtins(call, arena, pool);
    EXPECT_EQ(static_cast<IntValue*>(r.result)->v, 3);
}

// ── Layer 3: bbq.ddcg-driven compilation end-to-end ──────────────

#include "Parser.h"
#include "Sema.h"
#include "bbq_compile.h"

namespace {
struct CompileAndRunResult {
    bool compiled;
    bool success;
    zcow::parse_result meta;
    std::string error;
    CompiledGrammar* grammar;
};

CompileAndRunResult layer3_compile_and_run(const char* bbq_src,
                                            const char* rule_name,
                                            const std::vector<uint8_t>& bytes) {
    auto* parser = new ::Parser();
    parser->init(bbq_src, static_cast<int>(strlen(bbq_src)));
    if (!parser->parse() || parser->ast == nullptr) {
        return {false, false, {}, "parse failed", nullptr};
    }
    bbqgen::ErrorReporter reporter;
    bbqgen::Sema sema(reporter);
    sema.analyze(parser->ast);
    if (reporter.has_errors()) {
        std::ostringstream oss;
        reporter.print_all(oss);
        return {false, false, {}, oss.str(), nullptr};
    }

    ::bbq::Compiler compiler;
    CompiledGrammar* grammar = compiler.compile_grammar(parser->ast);
    if (!grammar) {
        return {true, false, {}, "compile returned null", nullptr};
    }

    // Use the std::string overload — the const char* one uses
    // interned-pointer equality which won't match a bare literal.
    KontNode* entry = grammar->lookup(std::string(rule_name));
    if (!entry) {
        return {true, false, {}, "rule not found", grammar};
    }

    // A fixed-width external, so grammars carrying `extern(...)` fields can run.
    // Harmless for grammars without one: the table is consulted only at an
    // ExternalCallNode. The name must be interned in THIS grammar's pool, since
    // lookup is by pointer equality.
    static ExternalParserTable::Entry ext_entries[1];
    static ExternalParserTable ext_table;
    ext_entries[0].name = grammar->strings.intern("readit");
    ext_entries[0].fn = [](const uint8_t*, size_t length, size_t* consumed,
                           ParseArena*, void*) -> bool {
        if (length < 4) return false;
        *consumed = 4;
        return true;
    };
    ext_entries[0].user_data = nullptr;
    ext_table.entries = ext_entries;
    ext_table.count = 1;

    CEKMachine m;
    m.arena = &grammar->arena;
    m.builtins = &grammar->builtins;   // peek/abs/min/max/clamp
    m.ext_parsers = &ext_table;
    auto meta = m.execute_from(entry, bytes.data(), bytes.size(),
                               grammar->default_little_endian);
    return {true, !m.failed && meta.success, meta,
            m.best_error_msg ? m.best_error_msg : "", grammar};
}
}

TEST(CEKLayer3, MinimalStruct) {
    auto r = layer3_compile_and_run(
        "Hdr = struct { a: uint8, b: uint16le }",
        "Hdr",
        {0x07, 0x42, 0x01});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 3u);
    ASSERT_NE(r.meta.doc.root(), nullptr);
    // Top-level struct fields land directly under root (no Hdr wrap)
    // — matches the user-facing convention `result.a` / `result.b`.
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 2);
    EXPECT_STREQ(r.meta.doc.root()->kids[0]->name, "a");
    EXPECT_STREQ(r.meta.doc.root()->kids[1]->name, "b");
    delete r.grammar;
}

TEST(CEKLayer3, StructWithFloat) {
    // Float field stays typed as FloatValue end-to-end through compile.
    auto r = layer3_compile_and_run(
        "Sample = struct { x: float32 }",
        "Sample",
        {0xc3, 0xf5, 0x48, 0x40});  // 3.14f LE
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 4u);
    delete r.grammar;
}

TEST(CEKLayer3, StructWithConstraint) {
    auto r = layer3_compile_and_run(
        "Magic = struct { ver: uint8 where ver == 1 }",
        "Magic",
        {0x01});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    delete r.grammar;
}

TEST(CEKLayer3, UnderscoreDropsFromOutput) {
    // `_` is the effect destination: parsed (bytes consumed) but not stored.
    // Multiple `_` are fine — none of them lands in the output tree.
    auto r = layer3_compile_and_run(
        "Hdr = struct { a: uint8, _: uint8, b: uint8, _: uint16le }",
        "Hdr",
        {0x01, 0x02, 0x03, 0x04, 0x05});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 5u);    // all five bytes consumed
    ASSERT_NE(r.meta.doc.root(), nullptr);
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 2);  // a, b — both `_` dropped
    EXPECT_STREQ(r.meta.doc.root()->kids[0]->name, "a");
    EXPECT_STREQ(r.meta.doc.root()->kids[1]->name, "b");
    delete r.grammar;
}

TEST(CEKLayer3, UnderscoreVerifyThenDrop) {
    // The magic-number case: parse, VERIFY via where, consume the bytes, but
    // keep it out of the struct. A bad value must still fail the parse.
    const char* spec = "Hdr = struct { _: uint8 where _ == 0x42, x: uint8 }";
    auto ok = layer3_compile_and_run(spec, "Hdr", {0x42, 0x07});
    ASSERT_TRUE(ok.compiled) << ok.error;
    ASSERT_TRUE(ok.success) << ok.error;           // 0x42 verifies
    EXPECT_EQ(ok.meta.bytes_consumed, 2u);
    ASSERT_EQ(ok.meta.doc.root()->kids.size(), 1);       // only x; the `_` is dropped
    EXPECT_STREQ(ok.meta.doc.root()->kids[0]->name, "x");
    delete ok.grammar;

    auto bad = layer3_compile_and_run(spec, "Hdr", {0x99, 0x07});
    ASSERT_TRUE(bad.compiled) << bad.error;
    EXPECT_FALSE(bad.success);                      // 0x99 fails the where
    delete bad.grammar;
}

TEST(CEKLayer3, ArrayWithTypeSeparator) {
    // `array<uint8>(uint8, count(3))`: 3 u8 elements, each pair split by a u8
    // separator. Bytes [10, FF, 20, FF, 30] → elements at 0,2,4; the FF
    // separators at 1,3 are parsed and discarded (δ=discard → Drop), never
    // captured. (Separators are a PEG/CSV bolt-on — not in IPG.)
    auto r = layer3_compile_and_run(
        "Arr = array<uint8>(uint8, count(3))",
        "Arr",
        {0x10, 0xFF, 0x20, 0xFF, 0x30});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 5u);          // elements + separators
    auto& arr = *r.meta.doc.root()->kids[0];
    EXPECT_EQ(arr.type, CaptureType::Array);
    ASSERT_EQ(arr.kids.size(), 3u);                // 3 elements; separators dropped
    EXPECT_EQ(arr.kids[0]->start_offset, 0u);
    EXPECT_EQ(arr.kids[1]->start_offset, 2u);      // skipped separator at 1
    EXPECT_EQ(arr.kids[2]->start_offset, 4u);      // skipped separator at 3
    delete r.grammar;
}

TEST(CEKLayer3, ArrayWithVerifiedSeparator) {
    // A real CSV-style delimiter: `uint8 where _ == 0x2C` (a comma). Each gap is
    // parsed, VERIFIED to be the delimiter via its where, and dropped from the
    // array — the separator is `_` for arrays, so it binds under `_` for the
    // check (pure Drop couldn't resolve `_`) but never captures.
    const char* spec = "Arr = array<uint8>(uint8 where _ == 0x2C, count(3))";
    auto ok = layer3_compile_and_run(spec, "Arr", {0x10, 0x2C, 0x20, 0x2C, 0x30});
    ASSERT_TRUE(ok.compiled) << ok.error;
    ASSERT_TRUE(ok.success) << ok.error;
    EXPECT_EQ(ok.meta.bytes_consumed, 5u);
    auto& arr = *ok.meta.doc.root()->kids[0];
    ASSERT_EQ(arr.kids.size(), 3u);                 // 3 elements; commas dropped
    EXPECT_EQ(arr.kids[1]->start_offset, 2u);
    delete ok.grammar;

    // A non-comma delimiter (0x3B) fails the separator's where → parse fails.
    auto bad = layer3_compile_and_run(spec, "Arr", {0x10, 0x3B, 0x20, 0x2C, 0x30});
    ASSERT_TRUE(bad.compiled) << bad.error;
    EXPECT_FALSE(bad.success);
    delete bad.grammar;
}

// @rest: the size field scopes the remainder of the struct to exactly
// `size` bytes (a confined, exactly-consumed window).
static const char* kRestSpec =
    "Section = struct { id: uint8, size: uint8 @rest, a: uint8, b: uint8 }";

TEST(CEKLayer3, RestExactConsumption) {
    // size = 2; the two fields after it consume exactly the window.
    auto r = layer3_compile_and_run(kRestSpec, "Section",
                                    {0x10, 0x02, 0xAA, 0xBB});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 4u);
    ASSERT_NE(r.meta.doc.root(), nullptr);
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 4);
    delete r.grammar;
}

TEST(CEKLayer3, RestUnderConsumptionFails) {
    // size = 3 but the struct's fields consume only 2 bytes of the window —
    // a size prefix means exactly, so the leftover byte is a parse error.
    auto r = layer3_compile_and_run(kRestSpec, "Section",
                                    {0x10, 0x03, 0xAA, 0xBB, 0xCC});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error.find("size mismatch"), std::string::npos) << r.error;
    delete r.grammar;
}

TEST(CEKLayer3, RestWindowEscapingInputFails) {
    // size = 5 declares a window past the end of the 4-byte input — confinement
    // fails it at the size field, never silently clamped.
    auto r = layer3_compile_and_run(kRestSpec, "Section",
                                    {0x10, 0x05, 0xAA, 0xBB});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error.find("out of range"), std::string::npos) << r.error;
    delete r.grammar;
}

TEST(CEKLayer3, ConstraintFails) {
    auto r = layer3_compile_and_run(
        "Magic = struct { ver: uint8 where ver == 1 }",
        "Magic",
        {0x02});  // != 1
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);  // constraint violation
    delete r.grammar;
}

TEST(CEKLayer3, ComputeField) {
    auto r = layer3_compile_and_run(
        "Doubled = struct {\n"
        "    base: uint8,\n"
        "    twice: compute(base * 2 : uint16)\n"
        "}",
        "Doubled",
        {0x07});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    ASSERT_NE(r.meta.doc.root(), nullptr);
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 2);
    EXPECT_STREQ(r.meta.doc.root()->kids[1]->name, "twice");
    EXPECT_EQ(r.meta.doc.root()->kids[1]->type, CaptureType::Computed);
    ASSERT_NE(r.meta.doc.root()->kids[1]->computed_value, nullptr);
    EXPECT_EQ(r.meta.doc.root()->kids[1]->computed_value->kind, bbq::ComputedValue::Kind::Int);
    EXPECT_EQ(r.meta.doc.root()->kids[1]->computed_value->i, 14);
    delete r.grammar;
}

TEST(CEKLayer3, Optional) {
    // Optional with `where` constraint: present when condition holds,
    // absent otherwise.
    auto r = layer3_compile_and_run(
        "Hdr = struct {\n"
        "    flag: uint8,\n"
        "    extra: optional<uint16le> where flag != 0\n"
        "}",
        "Hdr",
        {0x01, 0x42, 0x01});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 3u);
    delete r.grammar;
}

TEST(CEKLayer3, OptionalAbsent) {
    auto r = layer3_compile_and_run(
        "Hdr = struct {\n"
        "    flag: uint8,\n"
        "    extra: optional<uint16le> where flag != 0\n"
        "}",
        "Hdr",
        {0x00});  // flag == 0, so optional absent
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 1u);
    delete r.grammar;
}

TEST(CEKLayer3, OptionalPredicatedSkipsPresentBytes) {
    // The discriminating case: flag == 0 with element bytes available. A
    // predicated optional skips the element because the predicate is false —
    // it does NOT greedily try-parse it. So `tail` reads the byte right after
    // `flag`, `extra` is absent, and only flag+tail are consumed. (A
    // constraint-ignoring try/restore would have consumed the uint16le and
    // left `tail` reading the fourth byte.)
    auto r = layer3_compile_and_run(
        "Hdr = struct {\n"
        "    flag: uint8,\n"
        "    extra: optional<uint16le> where flag != 0,\n"
        "    tail: uint8\n"
        "}",
        "Hdr",
        {0x00, 0x11, 0x22, 0x33});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 2u);  // flag + tail; extra skipped
    ASSERT_NE(r.meta.doc.root(), nullptr);
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 2);  // flag, tail (extra absent)
    EXPECT_STREQ(r.meta.doc.root()->kids[0]->name, "flag");
    EXPECT_STREQ(r.meta.doc.root()->kids[1]->name, "tail");
    delete r.grammar;
}

TEST(CEKLayer3, OptionalPredicatedElementMandatory) {
    // When the predicate holds the element is mandatory: flag != 0 but no bytes
    // for `extra` is a hard parse failure, not a silent absence. (A try/restore
    // optional would restore and succeed with extra absent — the wrong answer
    // once the predicate has committed to the element being present.)
    auto r = layer3_compile_and_run(
        "Hdr = struct {\n"
        "    flag: uint8,\n"
        "    extra: optional<uint16le> where flag != 0\n"
        "}",
        "Hdr",
        {0x01});  // flag != 0 but no bytes for the uint16le
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
    delete r.grammar;
}

TEST(CEKLayer3, ArrayFixedCount) {
    auto r = layer3_compile_and_run(
        "Buf = struct {\n"
        "    n: uint8,\n"
        "    items: array<uint8>[n]\n"
        "}",
        "Buf",
        {0x03, 0xAA, 0xBB, 0xCC});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 4u);
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 2);
    EXPECT_EQ(r.meta.doc.root()->kids[1]->type, CaptureType::Array);
    EXPECT_EQ(r.meta.doc.root()->kids[1]->kids.size(), 3);
    delete r.grammar;
}
TEST(CEKLayer3, ArrayUntilEof) {
    auto r = layer3_compile_and_run(
        "Bytes = array<uint8>(none, eof)",
        "Bytes",
        {0x01, 0x02, 0x03, 0x04, 0x05});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 5u);
    auto& s = *r.meta.doc.root()->kids[0];
    EXPECT_EQ(s.type, CaptureType::Array);
    EXPECT_EQ(s.kids.size(), 5u);
    delete r.grammar;
}

TEST(CEKLayer3, ArrayUntilCondition) {
    auto r = layer3_compile_and_run(
        "Stream = array<uint8>(none, until(@remaining < 2))",
        "Stream",
        {0xAA, 0xBB, 0xCC, 0xDD});  // stops when remaining < 2 → after 3rd byte
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto& s = *r.meta.doc.root()->kids[0];
    EXPECT_EQ(s.type, CaptureType::Array);
    EXPECT_GE(s.kids.size(), 1u);   // some elements consumed
    delete r.grammar;
}

TEST(CEKLayer3, BitfieldBE) {
    auto r = layer3_compile_and_run(
        "Flags = bitfield<uint8> { hi: 4, lo: 4 }",
        "Flags",
        {0xAB});  // 0xA in hi, 0xB in lo (BE: MSB-first)
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 1u);
    delete r.grammar;
}

TEST(CEKLayer3, BitfieldEntryConstraint) {
    // Per-member `where`: a member's constraint runs after its own
    // ExtractBits/StoreEnv, against the just-extracted value (resolved by name
    // through E). Full-width entry → shift 0 either endianness, so the test is
    // about the constraint, not bit order.
    const char* spec = "Flags = bitfield<uint8> { v: 8 where v == 0x42 }";
    auto ok = layer3_compile_and_run(spec, "Flags", {0x42});
    ASSERT_TRUE(ok.compiled) << ok.error;
    ASSERT_TRUE(ok.success) << ok.error;
    EXPECT_EQ(ok.meta.bytes_consumed, 1u);
    delete ok.grammar;

    auto bad = layer3_compile_and_run(spec, "Flags", {0x99});  // v=0x99 fails the where
    ASSERT_TRUE(bad.compiled) << bad.error;
    EXPECT_FALSE(bad.success);
    delete bad.grammar;
}

TEST(CEKLayer3, SwitchDefaultRejectMessage) {
    // `default: reject` and no-default both fail on an unmatched discriminant,
    // but the no-match message names the cause distinctly.
    auto rej = layer3_compile_and_run(
        "Msg = struct {\n"
        "    tag: uint8,\n"
        "    body: switch(tag) {\n"
        "        1: uint16le;\n"
        "        default: reject;\n"
        "    }\n"
        "}",
        "Msg", {0x99, 0x00, 0x00});       // tag=0x99 → no case, explicit reject
    ASSERT_TRUE(rej.compiled) << rej.error;
    EXPECT_FALSE(rej.success);
    EXPECT_NE(rej.error.find("default: reject"), std::string::npos) << rej.error;
    delete rej.grammar;

    auto nod = layer3_compile_and_run(
        "Msg = struct {\n"
        "    tag: uint8,\n"
        "    body: switch(tag) {\n"
        "        1: uint16le;\n"
        "    }\n"
        "}",
        "Msg", {0x99, 0x00, 0x00});       // tag=0x99 → no case, no default
    ASSERT_TRUE(nod.compiled) << nod.error;
    EXPECT_FALSE(nod.success);
    EXPECT_NE(nod.error.find("no default"), std::string::npos) << nod.error;
    delete nod.grammar;
}

// Collect RefKonts reachable from a rule body (down the spine + into a `where`
// expression). Enough node types for the simple grammar below.
static void collect_refkonts(KontNode* k, std::vector<const KontNode*>& seen,
                             std::vector<RefKont*>& out) {
    if (!k) return;
    for (auto* s : seen) if (s == k) return;
    seen.push_back(k);
    if (auto* r = dynamic_cast<RefKont*>(k)) { out.push_back(r); return; }
    if (auto* e = dynamic_cast<EvalConstraintNode*>(k)) {
        collect_refkonts(e->constraint_expr, seen, out);
        collect_refkonts(e->next, seen, out);
        return;
    }
    if (auto* l = dynamic_cast<LtKont*>(k)) {
        collect_refkonts(l->left, seen, out);
        collect_refkonts(l->right, seen, out);
        return;
    }
    if (auto* b = dynamic_cast<BeginStructNode*>(k)) { collect_refkonts(b->next, seen, out); return; }
    if (auto* es = dynamic_cast<EndStructNode*>(k)) { collect_refkonts(es->next, seen, out); return; }
    if (auto* m = dynamic_cast<MatchPrimitiveNode*>(k)) { collect_refkonts(m->next, seen, out); return; }
    if (auto* c = dynamic_cast<CaptureKont*>(k)) { collect_refkonts(c->next, seen, out); return; }
    if (auto* s = dynamic_cast<StoreEnvKont*>(k)) { collect_refkonts(s->next, seen, out); return; }
}

TEST(CEKLayer3, RefKontCarriesCrossRuleResolution) {
    // `Inner`'s `where data < len` references `len` from the enclosing `Outer`.
    // Sema's cross-rule resolution (parent rule + path + depth) must ride on the
    // RefKont for the static emitter; the same-scope `data` ref stays clean.
    auto r = layer3_compile_and_run(
        "Outer = struct { len: uint8, inner: Inner }\n"
        "Inner = struct { data: uint8 where data < len }",
        "Outer", {0x05, 0x03});           // len=5; inner.data=3, 3<5 passes
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;

    KontNode* inner = r.grammar->lookup(std::string("Inner"));
    ASSERT_NE(inner, nullptr);
    std::vector<const KontNode*> seen;
    std::vector<RefKont*> refs;
    collect_refkonts(inner, seen, refs);

    RefKont* len_ref = nullptr;
    RefKont* data_ref = nullptr;
    for (auto* rk : refs) {
        if (std::string(rk->name) == "len") len_ref = rk;
        if (std::string(rk->name) == "data") data_ref = rk;
    }
    ASSERT_NE(len_ref, nullptr) << "no RefKont(len) found";
    EXPECT_STREQ(len_ref->resolved_parent, "Outer");   // cross-rule: names the parent rule
    EXPECT_NE(len_ref->resolved_path, nullptr);        // ...and the field path within it

    ASSERT_NE(data_ref, nullptr) << "no RefKont(data) found";
    EXPECT_EQ(data_ref->resolved_parent, nullptr);     // same-scope: clean, no resolution
    EXPECT_EQ(data_ref->resolved_path, nullptr);

    delete r.grammar;
}

TEST(CEKLayer3, MultiRuleCrossReference) {
    // Two rules where one references the other via a field.
    auto r = layer3_compile_and_run(
        "Header = struct { magic: uint8, len: uint8 }\n"
        "Outer = struct { hdr: Header, payload: uint16le }",
        "Outer",
        {0x42, 0x05, 0x34, 0x12});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 4u);
    delete r.grammar;
}

TEST(CEKLayer3, SwitchIntCases) {
    auto r = layer3_compile_and_run(
        "Msg = struct {\n"
        "    tag: uint8,\n"
        "    body: switch(tag) {\n"
        "        1: uint16le;\n"
        "        2: uint32le;\n"
        "    }\n"
        "}",
        "Msg",
        {0x01, 0x42, 0x01});  // tag=1 → 2-byte body
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 3u);
    delete r.grammar;
}

TEST(CEKLayer3, SwitchWithDefault) {
    // Verifies the ddcgc safe_field_name fix — `default` is a C++
    // keyword and asdl-emitted AST uses `default_`. The fix in
    // ddcgc/src/cpp_backend.cpp's safe_field_name now matches.
    auto r = layer3_compile_and_run(
        "Msg = struct {\n"
        "    tag: uint8,\n"
        "    body: switch(tag) {\n"
        "        1: uint16le;\n"
        "        default: uint8;\n"
        "    }\n"
        "}",
        "Msg",
        {0x05, 0xFF});  // tag=5 → default branch
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 2u);
    delete r.grammar;
}

TEST(CEKLayer3, Leb128Varints) {
    // uleb128/sleb128 decode to the carrier width and land as Computed captures
    // holding the decoded IntValue; the byte span consumed is data-dependent.
    auto r = layer3_compile_and_run(
        "Msg = struct {\n"
        "    id: uleb128,\n"      // 0xAC 0x02      → 300
        "    delta: sleb128,\n"   // 0x7F           → -1
        "    wide: uleb64,\n"     // 0xE5 0x8E 0x26 → 624485
        "    tail: uint8\n"       // 0x2A
        "}",
        "Msg",
        {0xAC, 0x02, 0x7F, 0xE5, 0x8E, 0x26, 0x2A});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 7u);   // exact variable spans consumed
    ASSERT_NE(r.meta.doc.root(), nullptr);
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 4);
    auto ival = [](const bbq::zcow::node_ptr& n) {
        return n->computed_value->i;
    };
    EXPECT_EQ(ival(r.meta.doc.root()->kids[0]), 300);
    EXPECT_EQ(ival(r.meta.doc.root()->kids[1]), -1);
    EXPECT_EQ(ival(r.meta.doc.root()->kids[2]), 624485);
    // The id field spans 2 bytes [0,2); delta 1 byte [2,3).
    EXPECT_EQ(r.meta.doc.root()->kids[0]->start_offset, 0u);
    EXPECT_EQ(r.meta.doc.root()->kids[0]->end_offset, 2u);
    EXPECT_EQ(r.meta.doc.root()->kids[1]->end_offset, 3u);
    delete r.grammar;
}

TEST(CEKLayer3, Leb128Rejected) {
    // Overlong/over-wide uleb32: five continuation bytes exceed the 32-bit carrier.
    auto r = layer3_compile_and_run(
        "Msg = struct { id: uleb32 }",
        "Msg",
        {0x80, 0x80, 0x80, 0x80, 0x80, 0x00});  // 6 bytes, all-continuation → invalid
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);                     // strict decoder rejects
    delete r.grammar;
}

TEST(CEKLayer3, SwitchRangeCases) {
    // A range case `1..3` is expanded into one match entry per discriminant,
    // all sharing a single compiled target.
    const char* spec =
        "Small = struct { v: uint8 }\n"
        "Wide  = struct { v: uint16le }\n"
        "Msg = struct {\n"
        "    tag: uint8,\n"
        "    body: switch(tag) {\n"
        "        1..3: Small;\n"
        "        10:   Wide;\n"
        "    }\n"
        "}";
    // tag=2 falls inside 1..3 → Small (1-byte body).
    auto r = layer3_compile_and_run(spec, "Msg", {0x02, 0x2A});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 2u);
    delete r.grammar;

    // tag=3 (upper bound of the range) also routes to Small.
    auto r3 = layer3_compile_and_run(spec, "Msg", {0x03, 0x2A});
    ASSERT_TRUE(r3.success) << r3.error;
    EXPECT_EQ(r3.meta.bytes_consumed, 2u);
    delete r3.grammar;

    // tag=10 routes to Wide (2-byte body); proves the non-range case still matches.
    auto r10 = layer3_compile_and_run(spec, "Msg", {0x0A, 0x34, 0x12});
    ASSERT_TRUE(r10.success) << r10.error;
    EXPECT_EQ(r10.meta.bytes_consumed, 3u);
    delete r10.grammar;
}

TEST(CEKLayer3, ArrayElementInterval) {
    // Per-element random access: element i seeks to base-@index, so the array
    // reads the input BACKWARD — provable random access, not sequential reads.
    auto r = layer3_compile_and_run(
        "M = struct {\n"
        "    base: uint8,\n"
        "    items: array<uint8>[3] @ [base - @index, base - @index + 1]\n"
        "}",
        "M",
        {0x03, 0xAA, 0xBB, 0xCC});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    ASSERT_NE(r.meta.doc.root(), nullptr);
    // root.children = { base, items }; items is an array of 3.
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 2);
    auto* items = r.meta.doc.root()->kids[1].get();
    ASSERT_EQ(items->kids.size(), 3);
    // Each element parsed at base-i: 3, 2, 1 — it sought backward each iteration.
    EXPECT_EQ(items->kids[0]->start_offset, 3u);
    EXPECT_EQ(items->kids[1]->start_offset, 2u);
    EXPECT_EQ(items->kids[2]->start_offset, 1u);
    delete r.grammar;

    // Confinement: an element interval that escapes the input fails the parse.
    auto bad = layer3_compile_and_run(
        "M2 = struct {\n"
        "    base: uint8,\n"
        "    items: array<uint8>[3] @ [base + @index, base + @index + 1]\n"
        "}",
        "M2",
        {0x02, 0xAA, 0xBB});   // base=2; element 2 seeks to [4,5), past the 3-byte input
    ASSERT_TRUE(bad.compiled) << bad.error;
    EXPECT_FALSE(bad.success);
    delete bad.grammar;
}

TEST(CEKLayer3, StructLevelWhere) {
    // A struct-level `where` runs after all fields are parsed and can reference
    // any of them. Here it requires the two fields to match.
    const char* spec = "Msg = struct { a: uint8, b: uint8 } where a == b";

    auto ok = layer3_compile_and_run(spec, "Msg", {0x05, 0x05});
    ASSERT_TRUE(ok.compiled) << ok.error;
    EXPECT_TRUE(ok.success) << ok.error;          // a == b → passes
    EXPECT_EQ(ok.meta.bytes_consumed, 2u);
    delete ok.grammar;

    auto bad = layer3_compile_and_run(spec, "Msg", {0x05, 0x06});
    ASSERT_TRUE(bad.compiled) << bad.error;
    EXPECT_FALSE(bad.success);                     // a != b → struct where rejects
    delete bad.grammar;

    // Probe: does a FIELD-level where reject at the top level?
    auto fbad = layer3_compile_and_run(
        "M2 = struct { a: uint8 where a == 1 }", "M2", {0x02});
    ASSERT_TRUE(fbad.compiled) << fbad.error;
    EXPECT_FALSE(fbad.success);                    // field where rejection
    delete fbad.grammar;

    // Probe: a struct where on a builtin (pos), no field refs.
    auto pbad = layer3_compile_and_run(
        "M3 = struct { a: uint8, b: uint8 } where pos > 100", "M3", {0x01, 0x02});
    ASSERT_TRUE(pbad.compiled) << pbad.error;
    EXPECT_FALSE(pbad.success);                    // pos==2, not >100
    delete pbad.grammar;

    // Probe: single-field struct-level where (closest to field-level shape).
    auto s1 = layer3_compile_and_run(
        "M4 = struct { a: uint8 } where a == 1", "M4", {0x02});
    ASSERT_TRUE(s1.compiled) << s1.error;
    EXPECT_FALSE(s1.success);                       // a==2 != 1
    delete s1.grammar;

    // Probe: literal false — does the struct EvalConstraint fire at all?
    auto lf = layer3_compile_and_run(
        "M5 = struct { a: uint8 } where false", "M5", {0x01});
    ASSERT_TRUE(lf.compiled) << lf.error;
    EXPECT_FALSE(lf.success);
    delete lf.grammar;
}

// A registered `where` verification function called with the same arguments as the
// codegen backends — `buffer` (the construct's byte interval, a FieldCaptureValue
// here) and the parsed `checksum`. Reads the header bytes over buffer's [start, end)
// and validates the IPv4 checksum. The .bbq spec is byte-identical across backends.
static void cek_ipv4_checksum_ok(bbq::cek::CEKMachine* m, bbq::cek::Value** args) {
    auto* buf = static_cast<bbq::cek::FieldCaptureValue*>(args[0]);
    size_t start = buf->capture->start_offset;
    size_t end   = buf->capture->end_offset;
    int64_t stored = static_cast<bbq::cek::IntValue*>(args[1])->v;
    uint32_t sum = 0;
    for (size_t i = start; i + 1 < end; i += 2) {
        uint32_t w = ((uint32_t)m->input[i] << 8) | m->input[i + 1];
        if (i - start == 10) w = 0;    // skip the checksum field itself
        sum += w;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    auto* r = m->arena->alloc<bbq::cek::BoolValue>();
    r->v = ((uint16_t)(~sum) == (uint16_t)stored);
    m->result = r;
}

TEST(CEKLayer3, StructWhereCallsFunction) {
    // A struct-level `where` that calls a user-registered function — the CEK
    // counterpart of the C/C++ IPv4 checksum tests.
    const char* spec =
        "@endian big\n"
        "IPv4Header = struct {\n"
        "    ver_ihl: uint8, dscp_ecn: uint8, total_length: uint16,\n"
        "    identification: uint16, flags_offset: uint16, ttl: uint8,\n"
        "    protocol: uint8, checksum: uint16, src_addr: uint32, dst_addr: uint32\n"
        "} where ipv4_checksum_ok(@buffer, checksum)";

    auto* parser = new ::Parser();
    parser->init(spec, static_cast<int>(strlen(spec)));
    ASSERT_TRUE(parser->parse());
    bbqgen::ErrorReporter rep;
    bbqgen::Sema sema(rep);
    sema.analyze(parser->ast);
    ASSERT_FALSE(rep.has_errors());
    ::bbq::Compiler compiler;
    CompiledGrammar* g = compiler.compile_grammar(parser->ast);
    ASSERT_NE(g, nullptr);

    // Register the verifier in a builtin table (name interned in the grammar's pool
    // so the call site matches by pointer).
    bbq::cek::BuiltinFnTable table;
    auto* entries = static_cast<bbq::cek::BuiltinFnTable::Entry*>(
        g->arena.allocate(sizeof(bbq::cek::BuiltinFnTable::Entry),
                          alignof(bbq::cek::BuiltinFnTable::Entry)));
    entries[0] = {g->strings.intern("ipv4_checksum_ok"), 2, &cek_ipv4_checksum_ok};
    table.entries = entries;
    table.count = 1;

    KontNode* entry = g->lookup(std::string("IPv4Header"));
    ASSERT_NE(entry, nullptr);
    auto run = [&](const std::vector<uint8_t>& bytes) {
        CEKMachine m;
        m.arena = &g->arena;
        m.builtins = &table;
        auto meta = m.execute_from(entry, bytes.data(), bytes.size(),
                                   g->default_little_endian);
        return !m.failed && meta.success;
    };

    std::vector<uint8_t> good = {
        0x45,0x00,0x00,0x73,0x00,0x00,0x40,0x00,
        0x40,0x11,0xb8,0x61,0xc0,0xa8,0x00,0x01,
        0xc0,0xa8,0x00,0xc7
    };
    EXPECT_TRUE(run(good));           // valid checksum → where passes

    std::vector<uint8_t> bad = good;
    bad[12] ^= 0xFF;                  // corrupt → checksum fails
    EXPECT_FALSE(run(bad));           // where rejects

    delete g;
}

TEST(CEKLayer3, RuleAlternatives) {
    // Rule-level `A | B` alternatives with backtracking. First alt
    // requires magic 0x42; second alt accepts any byte.
    auto r = layer3_compile_and_run(
        "Item = struct { magic: uint8 where magic == 0x42 }\n"
        "      | struct { other: uint8 }",
        "Item",
        {0x99});  // doesn't match magic, falls back to second alt
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 1u);
    delete r.grammar;
}

TEST(CEKLayer3, ThreeArmAlternativesChainAdvance) {
    // Three arms: each guarded by a where-check on the same byte.
    // Tests OnFailKont chain advancement — arm 0 fails (where mismatch),
    // chain advances to arm 1's RestoreThenJump link, arm 1 also fails,
    // chain advances to arm 2's link, arm 2 finally succeeds.
    auto r = layer3_compile_and_run(
        "Item = struct { a: uint8 where a == 0x10 }\n"
        "      | struct { b: uint8 where b == 0x20 }\n"
        "      | struct { c: uint8 where c == 0x30 }",
        "Item",
        {0x30});  // matches only the third arm
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 1u);
    delete r.grammar;
}

TEST(CEKLayer3, CrossRuleWhereFailBacktracksToCallerArm) {
    // A where-check inside Inner's top level fails. Inner is invoked
    // from Outer's first alt arm. Failure should cross the rule
    // boundary (via AbortRuleKont reading ReturnFrame.rule_fail_target,
    // which was baked at the InvokeRule emit site) and dispatch to the
    // caller's chain — Outer should successfully fall back to its
    // second arm.
    auto r = layer3_compile_and_run(
        "Inner = struct { magic: uint8 where magic == 0x42 }\n"
        "Outer = struct { hdr: Inner }\n"
        "      | struct { other: uint8 }",
        "Outer",
        {0x99});  // doesn't match Inner's magic; falls back to second alt
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 1u);
    delete r.grammar;
}

TEST(CEKLayer3, ThreeArmAlternativesAllFail) {
    // Same shape as above, but the input doesn't match any arm.
    // After the chain exhausts, the savepoint pops and the failure
    // propagates outward — at the rule's top level there's no
    // enclosing scope, so parse fails.
    auto r = layer3_compile_and_run(
        "Item = struct { a: uint8 where a == 0x10 }\n"
        "      | struct { b: uint8 where b == 0x20 }\n"
        "      | struct { c: uint8 where c == 0x30 }",
        "Item",
        {0xFF});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
    delete r.grammar;
}

TEST(CEKLayer3, ResyncArrayScansThroughCorruption) {
    // `array<T> resync` recovers from per-element parse failure by
    // advancing pos one byte and retrying. Element here demands a
    // 0x42 magic; the input has three valid records sandwiched
    // between corrupted bytes, so all three should be captured.
    auto r = layer3_compile_and_run(
        "Item = struct { magic: uint8 where magic == 0x42, val: uint8 }\n"
        "Doc  = array<Item> (none, eof) resync",
        "Doc",
        {0xFF, 0x42, 0x01,        // junk, then record (0x42 0x01)
         0xAA, 0xBB,              // junk
         0x42, 0x02,              // record (0x42 0x02)
         0x99,                    // junk
         0x42, 0x03});            // record (0x42 0x03)
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto& arr = *r.meta.doc.root()->kids[0];
    EXPECT_EQ(arr.type, CaptureType::Array);
    EXPECT_EQ(arr.kids.size(), 3u);
    delete r.grammar;
}

TEST(CEKLayer3, ResyncArrayEmptyOnAllJunk) {
    // No bytes match the where-check; resync skips through everything,
    // exits at EOF, and the array ends with zero records.
    auto r = layer3_compile_and_run(
        "Item = struct { magic: uint8 where magic == 0x42 }\n"
        "Doc  = array<Item> (none, eof) resync",
        "Doc",
        {0xAA, 0xBB, 0xCC, 0xDD});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto& arr = *r.meta.doc.root()->kids[0];
    EXPECT_EQ(arr.type, CaptureType::Array);
    EXPECT_EQ(arr.kids.size(), 0u);
    delete r.grammar;
}

TEST(CEKLayer3, ResyncWithUntilIsRejected) {
    // Sema rejects `resync` combined with `until` — the two have
    // contradictory roles around per-element semantics.
    auto r = layer3_compile_and_run(
        "Item = struct { x: uint8 }\n"
        "Doc  = array<Item> (none, until(false)) resync",
        "Doc",
        {0x00});
    EXPECT_FALSE(r.compiled);
    if (r.grammar) delete r.grammar;
}

TEST(CEKLayer3, ResyncCountedCollectsExactly) {
    // `array<Item>[n] resync` collects exactly n elements, skipping junk between
    // them. n=2 over {0x42, junk, 0x42} captures the two records.
    auto r = layer3_compile_and_run(
        "Item = struct { magic: uint8 where magic == 0x42 }\n"
        "Doc  = struct { n: uint8, items: array<Item>[n] resync }",
        "Doc",
        {0x02, 0x42, 0xFF, 0x42});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.doc.root()->kids[1]->kids.size(), 2);
    delete r.grammar;
}

TEST(CEKLayer3, ResyncCountedFailsUnderCount) {
    // A declared count `[n]` that cannot be met before EOF is a malformed structure
    // (IPG correct-by-construction): the parse FAILS, never silently returns fewer.
    // n=3 but only two 0x42 records exist → fail.
    auto r = layer3_compile_and_run(
        "Item = struct { magic: uint8 where magic == 0x42 }\n"
        "Doc  = struct { n: uint8, items: array<Item>[n] resync }",
        "Doc",
        {0x03, 0x42, 0xFF, 0x42});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success) << "under-count resync must fail, not return fewer";
    delete r.grammar;
}

TEST(CEKLayer3, BytesWithLength) {
    auto r = layer3_compile_and_run(
        "TLV = struct {\n"
        "    len: uint8,\n"
        "    data: bytes[len]\n"
        "}",
        "TLV",
        {0x03, 0xAA, 0xBB, 0xCC});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 4u);
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 2);
    EXPECT_EQ(r.meta.doc.root()->kids[1]->type, CaptureType::Bytes);
    EXPECT_EQ(r.meta.doc.root()->kids[1]->end_offset - r.meta.doc.root()->kids[1]->start_offset, 3u);
    delete r.grammar;
}

TEST(CEKLayer3, RecursiveTreeGrammar) {
    // Self-recursive grammar: a tree where each node carries N children
    // of the same Tree shape. The recursion is *guarded* by the array —
    // when n=0, the array stops and parsing terminates.
    //
    // Structure encoded:
    //   root(val=0x10, n=2)
    //     ├─ kid0(val=0x20, n=0)
    //     └─ kid1(val=0x30, n=1)
    //          └─ leaf(val=0x40, n=0)
    auto r = layer3_compile_and_run(
        "Tree = struct {\n"
        "    val: uint8,\n"
        "    n:   uint8,\n"
        "    kids: array<Tree>[n]\n"
        "}",
        "Tree",
        {0x10, 0x02,
            0x20, 0x00,
            0x30, 0x01,
                0x40, 0x00});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 8u);

    // Top-level Tree inlines into root: val, n, kids
    ASSERT_NE(r.meta.doc.root(), nullptr);
    ASSERT_EQ(r.meta.doc.root()->kids.size(), 3);
    EXPECT_STREQ(r.meta.doc.root()->kids[0]->name, "val");
    EXPECT_STREQ(r.meta.doc.root()->kids[1]->name, "n");
    EXPECT_STREQ(r.meta.doc.root()->kids[2]->name, "kids");

    auto& arr = *r.meta.doc.root()->kids[2];
    EXPECT_EQ(arr.type, CaptureType::Array);
    ASSERT_EQ(arr.kids.size(), 2u);

    // First child: leaf
    auto& kid0 = *arr.kids[0];
    ASSERT_EQ(kid0.kids.size(), 3u);
    auto& kid0_kids = *kid0.kids[2];
    EXPECT_EQ(kid0_kids.type, CaptureType::Array);
    EXPECT_EQ(kid0_kids.kids.size(), 0u);

    // Second child: has one grandchild
    auto& kid1 = *arr.kids[1];
    ASSERT_EQ(kid1.kids.size(), 3u);
    auto& kid1_kids = *kid1.kids[2];
    EXPECT_EQ(kid1_kids.type, CaptureType::Array);
    ASSERT_EQ(kid1_kids.kids.size(), 1u);

    auto& leaf = *kid1_kids.kids[0];
    ASSERT_EQ(leaf.kids.size(), 3u);
    auto& leaf_kids = *leaf.kids[2];
    EXPECT_EQ(leaf_kids.kids.size(), 0u);

    delete r.grammar;
}

TEST(CEKLayer3, UnguardedRecursionRejected) {
    // Direct self-ref with no guard would be an infinite type — Sema
    // must reject it.
    auto r = layer3_compile_and_run(
        "Bad = struct { x: uint8, b: Bad }",
        "Bad",
        {0x00});
    EXPECT_FALSE(r.compiled);  // expect Sema error, not a runtime failure
}

TEST(CEKLayer2EndToEnd, StructWithThreeFields) {
    // Full struct parse: BeginStruct → MatchPrim x → MatchPrim y → MatchPrim z → EndStruct → Halt
    // Verifies: K push order, env extends per-field, capture builder
    // produces nested captures, position advances, types are right.
    ParseArena arena;
    StringPool pool;
    PrimitiveInfo p_u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Native};
    PrimitiveInfo p_u16le{PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Little};
    PrimitiveInfo p_bool{PrimKind::Bool, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Native};

    auto* halt = arena.alloc<HaltNode>();
    auto* end_st = arena.alloc<EndStructNode>();
    end_st->next = halt;
    // Each field: producer → Capture → StoreEnv. z chains to end_st.
    auto* se_z = arena.alloc<StoreEnvKont>(); se_z->name = pool.intern("z"); se_z->next = end_st;
    auto* cap_z = arena.alloc<CaptureKont>(); cap_z->name = pool.intern("z"); cap_z->next = se_z;
    auto* m_z = arena.alloc<MatchPrimitiveNode>(); m_z->prim = p_bool; m_z->next = cap_z;
    auto* se_y = arena.alloc<StoreEnvKont>(); se_y->name = pool.intern("y"); se_y->next = m_z;
    auto* cap_y = arena.alloc<CaptureKont>(); cap_y->name = pool.intern("y"); cap_y->next = se_y;
    auto* m_y = arena.alloc<MatchPrimitiveNode>(); m_y->prim = p_u16le; m_y->next = cap_y;
    auto* se_x = arena.alloc<StoreEnvKont>(); se_x->name = pool.intern("x"); se_x->next = m_y;
    auto* cap_x = arena.alloc<CaptureKont>(); cap_x->name = pool.intern("x"); cap_x->next = se_x;
    auto* m_x = arena.alloc<MatchPrimitiveNode>(); m_x->prim = p_u8; m_x->next = cap_x;
    auto* begin_st = arena.alloc<BeginStructNode>();
    begin_st->struct_name = pool.intern("S");
    begin_st->next = m_x;

    uint8_t data[] = {0x07, 0x42, 0x01, 0x01};   // x=7 (u8), y=0x0142 (u16le), z=true (bool)
    CEKMachine m;
    m.arena = &arena;
    auto meta = m.execute_from(begin_st, data, sizeof(data), true);
    ASSERT_TRUE(meta.success) << (m.best_error_msg ? m.best_error_msg : "");
    EXPECT_EQ(meta.bytes_consumed, 4u);

    // Capture tree: root → struct S → [x, y, z]
    ASSERT_NE(meta.doc.root(), nullptr);
    ASSERT_EQ(meta.doc.root()->kids.size(), 1);
    auto& s = *meta.doc.root()->kids[0];
    EXPECT_EQ(s.type, CaptureType::Struct);
    EXPECT_STREQ(s.name, "S");
    ASSERT_EQ(s.kids.size(), 3u);
    EXPECT_STREQ(s.kids[0]->name, "x");
    EXPECT_STREQ(s.kids[1]->name, "y");
    EXPECT_STREQ(s.kids[2]->name, "z");

    // Env extended in order: most recent (z) is the head
    ASSERT_NE(m.env, nullptr);
    EXPECT_STREQ(m.env->binding.name, "z");
    ASSERT_EQ(m.env->binding.value->tag, ValueTag::BoolValue);
    EXPECT_TRUE(static_cast<BoolValue*>(m.env->binding.value)->v);

    ASSERT_NE(m.env->parent, nullptr);
    EXPECT_STREQ(m.env->parent->binding.name, "y");
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(m.env->parent->binding.value)->v, 0x0142);

    ASSERT_NE(m.env->parent->parent, nullptr);
    EXPECT_STREQ(m.env->parent->parent->binding.name, "x");
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(m.env->parent->parent->binding.value)->v, 0x07);
}

// ── Round trip ──────────────────────────────────────────────────────────────
//
// The correctness a consumer checks first: parse a real file with a real grammar,
// write it back, and get the same bytes. Then edit one field and check that the
// output differs in exactly that field and re-parses to the value written.
//
// NOTE: layer3_compile_and_run's result BORROWS the bytes it was given — spans
// point into them and serialize() reads them — so the input must outlive the
// result. Every test below binds it to a named vector rather than a temporary.

namespace {

// Every node of a fresh parse should still be described by its span: a document
// nobody wrote to owns no values, which is what makes serializing it a memcpy.
bool all_span_backed(const bbq::zcow::node* n) {
    if (!n) return true;
    if (!n->parsed) return false;
    for (const auto& k : n->kids) if (!all_span_backed(k.get())) return false;
    return true;
}

}  // namespace

TEST(CEKRoundTrip, UnmodifiedParseIsByteIdentical) {
    std::vector<uint8_t> bytes = {0x07, 0x42, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    auto r = layer3_compile_and_run(
        "Hdr = struct { a: uint8, b: uint16le, c: uint32be }", "Hdr", bytes);
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;

    EXPECT_TRUE(all_span_backed(r.meta.doc.root()));
    EXPECT_EQ(r.meta.doc.serialize(), bytes);
    delete r.grammar;
}

TEST(CEKRoundTrip, ArraysAndNestedStructsAreByteIdentical) {
    std::vector<uint8_t> bytes = {0x03, 0x0A, 0x14, 0x1E, 0x99};
    auto r = layer3_compile_and_run(
        "Doc = struct { n: uint8, xs: array<uint8>[n], tail: uint8 }", "Doc", bytes);
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 5u);

    EXPECT_TRUE(all_span_backed(r.meta.doc.root()));
    EXPECT_EQ(r.meta.doc.serialize(), bytes);
    delete r.grammar;
}

TEST(CEKRoundTrip, VarintDocumentIsByteIdentical) {
    // Variable-width leaves are the case a naive re-serializer gets wrong.
    std::vector<uint8_t> bytes = {0xAC, 0x02, 0x7F, 0xE5, 0x8E, 0x26, 0x2A};
    auto r = layer3_compile_and_run(
        "Msg = struct { id: uleb128, delta: sleb128, big: uleb128, tag: uint8 }",
        "Msg", bytes);
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.doc.serialize(), bytes);
    delete r.grammar;
}

TEST(CEKRoundTrip, EditingOneFieldChangesOnlyThatField) {
    std::vector<uint8_t> bytes = {0x07, 0x42, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    auto r = layer3_compile_and_run(
        "Hdr = struct { a: uint8, b: uint16le, c: uint32be }", "Hdr", bytes);
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;

    bbq::zcow::transient t = r.meta.doc.begin_edit();
    const char* path[] = {"c"};
    bbq::zcow::node* c = t.own_path(path, nullptr, 1);
    ASSERT_NE(c, nullptr);
    bbq::zcow::set_int(c, 0x11223344);
    bbq::zcow::document edited = std::move(t).commit();

    EXPECT_EQ(edited.serialize(),
              (std::vector<uint8_t>{0x07, 0x42, 0x01, 0x11, 0x22, 0x33, 0x44}));
    // The document it came from is untouched.
    EXPECT_EQ(r.meta.doc.serialize(), bytes);
    delete r.grammar;
}

// The check a consumer actually cares about: what you wrote is what comes back
// when the bytes you produced are parsed again.
TEST(CEKRoundTrip, AnEditReParsesToTheValueWritten) {
    std::vector<uint8_t> bytes = {0x07, 0x42, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    const char* src = "Hdr = struct { a: uint8, b: uint16le, c: uint32be }";

    auto r = layer3_compile_and_run(src, "Hdr", bytes);
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;

    bbq::zcow::transient t = r.meta.doc.begin_edit();
    const char* pa[] = {"a"};
    const char* pc[] = {"c"};
    bbq::zcow::set_int(t.own_path(pa, nullptr, 1), 0x5A);
    bbq::zcow::set_int(t.own_path(pc, nullptr, 1), 0x11223344);
    std::vector<uint8_t> out = std::move(t).commit().serialize();

    auto again = layer3_compile_and_run(src, "Hdr", out);
    ASSERT_TRUE(again.compiled) << again.error;
    ASSERT_TRUE(again.success) << again.error;

    const bbq::zcow::node* root = again.meta.doc.root();
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(root, "a"), again.meta.doc.src()), 0x5A);
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(root, "b"), again.meta.doc.src()), 0x0142);
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(root, "c"), again.meta.doc.src()), 0x11223344);
    // ...and re-serializing the re-parse is byte-identical to what produced it.
    EXPECT_EQ(again.meta.doc.serialize(), out);

    delete again.grammar;
    delete r.grammar;
}

// ── The whole grammar, both directions ──────────────────────────────────────
//
// The fixture covers every `type_expr` constructor in grammar/BBQ.asdl — struct,
// array (counted / eof / until / resync / separated), optional, union,
// alternatives, switch (value and range), compute, ruleref, extern, bitfield
// (named and inline), endian switch, intervals and @rest, varints, every
// primitive width and both endiannesses. Round-tripping all of it is the coverage
// claim; testing a handful of hand-written grammars is not.

namespace {

struct MatrixCase { const char* rule; std::vector<uint8_t> bytes; };

const std::vector<MatrixCase>& matrix_cases() {
    static const std::vector<MatrixCase> cases = {
        {"Flat", {0x12,0x56,0x34,0x00,0x01,0x00,0x00,0x01,0x02,0xFC,0xFF,0xFF,0xFF}},
        {"Nest", {0x07,0x09,0x00,0x0B}},
        {"Arr", {0x03,0x10,0x00,0x20,0x00,0x30,0x00}},
        {"Pair", {0x07,0x08}},
        {"Outer", {0x55,0x07,0x08}},
        {"RArr", {0x02,1,2,3,4}},
        {"Bytes", {0x03,0xAA,0xBB,0xCC}},
        {"Str", {0x61,0x62,0x63}},
        {"Wh", {0x01,0x2A}},
        {"WhTwice", {0x05,0x0A,0x2A}},
        {"Comp", {0xA5}},
        {"Iv", {0x02,0xFF,0x2A}},
        {"Pos", {0x0A,0x0B}},
        {"Op", {0x01,0x34,0x12,0x05,0x06}},
        {"Bare", {0x0A,0x2A}},
        {"Sw", {0x01,0x34,0x12}},
        {"VA", {0x01,0x0A}},
        {"VB", {0x02,0x0B}},
        {"U", {0x01,0x0A}},
        {"Alts", {0x02,0x0B}},
        {"Bf", {0xA5}},
        {"InlineBf", {0x11,0xA5,0x22}},
        {"Eof", {1,2,3,4}},
        {"Unt", {1,2,3}},
        {"Es", {0x34,0x12,0x12,0x34}},
        {"RsItem", {0x05}},
        {"Resync", {0x02,0x00,0x05,0x00,0x07}},
        {"Ext", {0xAA,0x01,0x02,0x03,0x04,0xBB}},
        {"SwRange", {0x02,0x34,0x12}},
        {"Tern", {0x05}},
        {"Bstart", {0x0A,0x0B}},
        {"Rest", {0x02,0x34,0x12}},
        {"RestEof", {0x03,10,20,30}},
        {"Cnt", {0x03,10,20,30}},
        {"TopSw", {0x01,0x22}},
        {"Np", {0x03,0xAA,0xBB,0xCC}},
        {"OScope", {0x03,0x03,0xFF,0x10,0x20,0x30}},
        {"AllPrim", {0x12,0xFE,0x56,0x34,0x01,0x02,0xFD,0xFF,
            0x00,0x01,0x00,0x00,0x01,0x02,0x03,0x04,0xFC,0xFF,0xFF,0xFF,
            0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xFB,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0x00,0x00,0xC0,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x40,0x01}},
        {"Leb", {0xAC,0x02,0x7B,0x42}},
        {"LebArr", {0x03,0x01,0xAC,0x02,0x05}},
        {"LebOpt", {0x01,0xAC,0x02,0x42}},
        {"Mat", {0x02,0x03,1,2,3,4,5,6}},
        {"OptSpan", {0x01,0xAA,0xBB,0x61,0x62,0x63}},
        {"TyByte", {0x2A}},
        {"TyRule", {0x07,0x08}},
        {"OptTop", {0x2A}},
        {"NestGrp", {0x02,0xAA,0xBB}},
        {"NestArr", {0x02, 0x01,0x11, 0x02,0x22,0x33}},
        {"FComp", {0xC8}},
    };
    return cases;
}

const std::string& fixture_src() {
    static const std::string src = [] {
        std::ifstream in(std::string(SOURCE_DIR) + "/test/fixtures/zcow_reader.bbq");
        std::ostringstream oss;
        oss << in.rdbuf();
        return oss.str();
    }();
    return src;
}

// The shape a parse produced, independent of which bytes back it — so a document
// and its re-parse can be compared without caring where the values came from.
void describe(const bbq::zcow::node* n, const bbq::zcow::source& src,
              const std::string& at, std::ostringstream& out) {
    if (!n) return;
    out << at << " t=" << (int)n->type << " tag=" << n->variant_tag
        << " len=" << (n->end_offset - n->start_offset)
        << " kids=" << n->kids.size();
    if (n->type == CaptureType::Computed && n->computed_value)
        out << " cv=" << (int)n->computed_value->kind;
    else if (!n->kids.size() && n->type != CaptureType::Computed) {
        int64_t v = 0; bool sgn = false;
        if (bbq::zcow::decode_int(n, src.buf, &v, &sgn)) out << " v=" << v;
    }
    out << "\n";
    for (size_t i = 0; i < n->kids.size(); i++) {
        const char* nm = n->kids[i]->name;
        describe(n->kids[i].get(), src,
                 (nm && *nm) ? at + "." + nm : at + "[" + std::to_string(i) + "]", out);
    }
}

std::string shape_of(const bbq::zcow::document& d) {
    std::ostringstream oss;
    describe(d.root(), d.src(), "$", oss);
    return oss.str();
}

}  // namespace

// Every rule: what the machine parsed, written back, is the bytes it consumed.
TEST(CEKMatrix, EveryRuleRoundTripsByteIdentical) {
    ASSERT_FALSE(fixture_src().empty()) << "fixture not found";
    for (const auto& c : matrix_cases()) {
        auto r = layer3_compile_and_run(fixture_src().c_str(), c.rule, c.bytes);
        EXPECT_TRUE(r.compiled) << c.rule << ": " << r.error;
        EXPECT_TRUE(r.success) << c.rule << ": " << r.error;
        if (!r.success) { delete r.grammar; continue; }

        // The document covers what was consumed, which may be a prefix.
        std::vector<uint8_t> consumed(c.bytes.begin(),
                                      c.bytes.begin() + r.meta.bytes_consumed);
        EXPECT_EQ(r.meta.doc.serialize(), consumed) << c.rule;
        EXPECT_TRUE(all_span_backed(r.meta.doc.root())) << c.rule;
        delete r.grammar;
    }
}

// ── The same laws, one layer up ─────────────────────────────────────────────
//
// ZCow's laws are proved over documents built by hand. Run them over documents the
// MACHINE produced, against every rule in the fixture, and whatever fails is
// something the parser is not recording.

namespace {

// A step down to a leaf. Array elements are anonymous, so the position has to be
// carried too — a name alone cannot tell two elements apart.
struct Step { const char* name; size_t index; };

void each_leaf(const bbq::zcow::node* n, std::vector<Step>& path,
               const std::function<void(const bbq::zcow::node*,
                                        const std::vector<Step>&)>& fn) {
    if (!n) return;
    if (n->kids.empty()) { fn(n, path); return; }
    for (size_t i = 0; i < n->kids.size(); i++) {
        path.push_back({n->kids[i]->name, i});
        each_leaf(n->kids[i].get(), path, fn);
        path.pop_back();
    }
}

// Walk the same route on the transient. own_path takes the name where there is one
// and the index otherwise, which is exactly what a Step holds.
// Addressed by POSITION at every step, never by name. A name is only unambiguous
// where names are unique among siblings, and they are not: an array of structs puts
// the elements' fields side by side, so `p` occurs once per element and a name-first
// lookup would find the first one every time.
bbq::zcow::node* reach(bbq::zcow::transient& t, const std::vector<Step>& path) {
    std::vector<const char*> names(path.size(), nullptr);
    std::vector<size_t> idx;
    for (const auto& s : path) idx.push_back(s.index);
    return t.own_path(names.data(), idx.data(), names.size());
}

}  // namespace

// GetPut over the whole grammar: read every leaf, write it straight back, and the
// document must be byte-identical. This is the law that caught Bool at the layer
// below; here it runs against every leaf of every rule.
TEST(CEKLaw, GetPutOverEveryRule) {
    for (const auto& c : matrix_cases()) {
        auto r = layer3_compile_and_run(fixture_src().c_str(), c.rule, c.bytes);
        EXPECT_TRUE(r.success) << c.rule << ": " << r.error;
        if (!r.success) { delete r.grammar; continue; }

        std::vector<uint8_t> before = r.meta.doc.serialize();
        bbq::zcow::transient t = r.meta.doc.begin_edit();
        const auto& src = r.meta.doc.src();

        std::vector<Step> names;
        each_leaf(r.meta.doc.root(), names,
                  [&](const bbq::zcow::node* leaf, const std::vector<Step>& path) {
            if (leaf->type == CaptureType::Computed || leaf->type == CaptureType::External) return;
            if (path.empty()) return;
            bbq::zcow::node* w = reach(t, path);
            if (!w) return;
            if (leaf->type == CaptureType::Bytes || leaf->type == CaptureType::String) {
                auto b = bbq::zcow::read_bytes(leaf, src);
                bbq::zcow::set_bytes(w, b.first, b.second);
            } else if (is_float_type(leaf->type)) {
                bbq::zcow::set_float(w, bbq::zcow::read_float(leaf, src));
            } else {
                bbq::zcow::set_int(w, bbq::zcow::read_int(leaf, src));
            }
        });

        EXPECT_EQ(std::move(t).commit().serialize(), before) << c.rule;
        delete r.grammar;
    }
}

// Persistence and Isolation (§1.2, §4.1) over machine-built documents.
TEST(CEKLaw, EditingNeverDisturbsTheDocumentItCameFrom) {
    for (const auto& c : matrix_cases()) {
        auto r = layer3_compile_and_run(fixture_src().c_str(), c.rule, c.bytes);
        EXPECT_TRUE(r.success) << c.rule << ": " << r.error;
        if (!r.success) { delete r.grammar; continue; }

        std::vector<uint8_t> before = r.meta.doc.serialize();
        {
            bbq::zcow::transient t = r.meta.doc.begin_edit();
            std::vector<Step> names;
            each_leaf(r.meta.doc.root(), names,
                      [&](const bbq::zcow::node* leaf, const std::vector<Step>& path) {
                if (path.empty() || leaf->type == CaptureType::Computed) return;
                if (leaf->type == CaptureType::Bytes || leaf->type == CaptureType::String ||
                    is_float_type(leaf->type)) return;
                if (bbq::zcow::node* w = reach(t, path))
                    bbq::zcow::set_int(w, 0x7F);
            });
            EXPECT_EQ(r.meta.doc.serialize(), before) << c.rule;   // before commit
            (void)std::move(t).commit();
        }
        EXPECT_EQ(r.meta.doc.serialize(), before) << c.rule;       // and after
        delete r.grammar;
    }
}

// Dependent fields (Nail) over machine-built documents: append to a counted array
// and the count must follow, without the caller touching it.
//
// This is the parser's half of the contract — ZCow recomputes what a node says it
// derives, but only the parser knows the linkage, at the moment it reads the count
// and uses it to bound the array.
TEST(CEKLaw, AppendingToACountedArrayUpdatesItsCount) {
    struct Counted { const char* rule; const char* array_field; const char* count_path; };
    const Counted counted[] = {
        {"Cnt",     "xs",    "n"},
        {"RArr",    "items", "n"},
        {"LebArr",  "xs",    "n"},
        {"NestArr", "gs",    "m"},
        {"Np",      "xs",    "h.n"},    // the count lives in a nested struct
    };

    for (const auto& k : counted) {
        std::vector<uint8_t> bytes;
        for (const auto& c : matrix_cases())
            if (std::strcmp(c.rule, k.rule) == 0) bytes = c.bytes;
        ASSERT_FALSE(bytes.empty()) << k.rule;

        auto r = layer3_compile_and_run(fixture_src().c_str(), k.rule, bytes);
        ASSERT_TRUE(r.success) << k.rule << ": " << r.error;

        const bbq::zcow::node* arr = bbq::zcow::child_named(r.meta.doc.root(), k.array_field);
        ASSERT_NE(arr, nullptr) << k.rule;
        const size_t n0 = bbq::zcow::size_of(arr);

        // Recording and recomputing are separate halves; say which one is missing.
        EXPECT_EQ(arr->derives, bbq::zcow::node::Derives::HasCount)
            << k.rule << ": the parser recorded no count linkage for this array";
        EXPECT_STREQ(arr->determines ? arr->determines : "<none>", k.count_path)
            << k.rule << ": the parser recorded the wrong field";

        bbq::zcow::transient t = r.meta.doc.begin_edit();
        const char* p[] = {k.array_field};
        bbq::zcow::node* xs = t.own_path(p, nullptr, 1);
        ASSERT_NE(xs, nullptr) << k.rule;
        bbq::zcow::set_int(t.append(xs, CaptureType::UInt8), 0x7F);
        bbq::zcow::document v = std::move(t).commit();

        // Resolve the count wherever the grammar put it.
        const bbq::zcow::node* cnt = v.root();
        for (const char* seg = k.count_path; cnt && seg && *seg; ) {
            const char* dot = std::strchr(seg, '.');
            std::string name(seg, dot ? (size_t)(dot - seg) : std::strlen(seg));
            cnt = bbq::zcow::child_named(cnt, name);
            seg = dot ? dot + 1 : nullptr;
        }
        ASSERT_NE(cnt, nullptr) << k.rule;
        EXPECT_EQ(bbq::zcow::read_int(cnt, v.src()), (int64_t)(n0 + 1))
            << k.rule << ": the parser did not record what this count derives from";

        delete r.grammar;
    }
}

// ── Bitfields ───────────────────────────────────────────────────────────────
//
// The parser records the run and its entries as it builds the document: the run
// owns the bytes and says which way they are laid out, each entry says where it
// sits. Nothing downstream re-derives that from the grammar.
//
// A top-level bitfield RULE is inlined flat, so the run is the rule's own root; an
// inline bitfield FIELD gets a wrapping scope. Both are a node whose span is
// exactly the run's bytes, which is all the write side needs.
TEST(CEKLaw, ATopLevelBitfieldRuleTagsItsRoot) {
    std::vector<uint8_t> bytes = {0xA5};   // Bf = bitfield<uint8> { hi: 4, lo: 4 }
    auto r = layer3_compile_and_run(fixture_src().c_str(), "Bf", bytes);
    ASSERT_TRUE(r.success) << r.error;

    const bbq::zcow::node* run = r.meta.doc.root();
    ASSERT_NE(run, nullptr);
    EXPECT_TRUE(run->bits_container) << "the parser did not record the run";

    // Declaration order is low bit first: hi is declared first, so hi is [0,4).
    EXPECT_EQ(bbq::zcow::read_bits(run, "hi", r.meta.doc.src()), 0x5);
    EXPECT_EQ(bbq::zcow::read_bits(run, "lo", r.meta.doc.src()), 0xA);

    bbq::zcow::transient t = r.meta.doc.begin_edit();
    bbq::zcow::write_bits(t.root_mut(), "hi", t.src(), 0x3);
    std::vector<uint8_t> out = std::move(t).commit().serialize();
    EXPECT_EQ(out, (std::vector<uint8_t>{0xA3}));

    auto again = layer3_compile_and_run(fixture_src().c_str(), "Bf", out);
    ASSERT_TRUE(again.success) << again.error;
    EXPECT_EQ(bbq::zcow::read_bits(again.meta.doc.root(), "hi", again.meta.doc.src()), 0x3);
    EXPECT_EQ(bbq::zcow::read_bits(again.meta.doc.root(), "lo", again.meta.doc.src()), 0xA);

    delete again.grammar;
    delete r.grammar;
}

TEST(CEKLaw, AnInlineBitfieldFieldTagsItsOwnScope) {
    // InlineBf = struct { lead: uint8, flags: bitfield<uint8>{hi:4, lo:4}, tail: uint8 }
    std::vector<uint8_t> bytes = {0x11, 0xA5, 0x22};
    auto r = layer3_compile_and_run(fixture_src().c_str(), "InlineBf", bytes);
    ASSERT_TRUE(r.success) << r.error;

    const bbq::zcow::node* run = bbq::zcow::child_named(r.meta.doc.root(), "flags");
    ASSERT_NE(run, nullptr);
    EXPECT_TRUE(run->bits_container) << "the parser did not record the run";
    EXPECT_EQ(bbq::zcow::read_bits(run, "hi", r.meta.doc.src()), 0x5);
    EXPECT_EQ(bbq::zcow::read_bits(run, "lo", r.meta.doc.src()), 0xA);

    bbq::zcow::transient t = r.meta.doc.begin_edit();
    const char* p[] = {"flags"};
    bbq::zcow::write_bits(t.own_path(p, nullptr, 1), "lo", t.src(), 0xC);
    std::vector<uint8_t> out = std::move(t).commit().serialize();
    // Only the run's byte moves; its neighbours are untouched.
    EXPECT_EQ(out, (std::vector<uint8_t>{0x11, 0xC5, 0x22}));

    auto again = layer3_compile_and_run(fixture_src().c_str(), "InlineBf", out);
    ASSERT_TRUE(again.success) << again.error;
    const bbq::zcow::node* run2 = bbq::zcow::child_named(again.meta.doc.root(), "flags");
    EXPECT_EQ(bbq::zcow::read_bits(run2, "lo", again.meta.doc.src()), 0xC);
    EXPECT_EQ(bbq::zcow::read_bits(run2, "hi", again.meta.doc.src()), 0x5);

    delete again.grammar;
    delete r.grammar;
}

// SBf = bitfield<uint8> { imm: signed 4, tag: unsigned 4 } — a signed entry reads
// back sign-extended from its own width, which is how instruction encodings carry
// immediates. Byte 0x2D: imm = 0xD = -3, tag = 0x2.
TEST(CEKLaw, ASignedBitfieldEntryIsSignExtended) {
    std::vector<uint8_t> bytes = {0x2D};
    auto r = layer3_compile_and_run(fixture_src().c_str(), "SBf", bytes);
    ASSERT_TRUE(r.success) << r.error;

    const bbq::zcow::node* run = r.meta.doc.root();
    EXPECT_TRUE(run->bits_container);
    EXPECT_EQ(bbq::zcow::read_bits(run, "imm", r.meta.doc.src()), -3);
    EXPECT_EQ(bbq::zcow::read_bits(run, "tag", r.meta.doc.src()), 2);

    // ...and the value the parse bound under the member name says the same thing.
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(run, "imm"), r.meta.doc.src()), -3);
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(run, "tag"), r.meta.doc.src()), 2);

    // A negative value written back survives the round trip.
    bbq::zcow::transient t = r.meta.doc.begin_edit();
    bbq::zcow::write_bits(t.root_mut(), "imm", t.src(), -6);
    std::vector<uint8_t> out = std::move(t).commit().serialize();
    EXPECT_EQ(out, (std::vector<uint8_t>{0x2A}));      // -6 = 0xA in 4 bits

    auto again = layer3_compile_and_run(fixture_src().c_str(), "SBf", out);
    ASSERT_TRUE(again.success) << again.error;
    EXPECT_EQ(bbq::zcow::read_bits(again.meta.doc.root(), "imm", again.meta.doc.src()), -6);
    EXPECT_EQ(bbq::zcow::read_bits(again.meta.doc.root(), "tag", again.meta.doc.src()), 2);

    delete again.grammar;
    delete r.grammar;
}

// What the parser reads out at parse time and what reading the run gives must be
// the same number — otherwise the two halves of the document disagree.
TEST(CEKLaw, ARecordedEntryValueAgreesWithReadingTheRun) {
    for (const char* rule : {"Bf", "InlineBf"}) {
        std::vector<uint8_t> bytes;
        for (const auto& c : matrix_cases())
            if (std::strcmp(c.rule, rule) == 0) bytes = c.bytes;
        ASSERT_FALSE(bytes.empty()) << rule;

        auto r = layer3_compile_and_run(fixture_src().c_str(), rule, bytes);
        ASSERT_TRUE(r.success) << rule << ": " << r.error;

        const bbq::zcow::node* run = std::strcmp(rule, "Bf") == 0
            ? r.meta.doc.root()
            : bbq::zcow::child_named(r.meta.doc.root(), "flags");
        ASSERT_NE(run, nullptr) << rule;

        for (const auto& e : run->kids) {
            if (!e->bits.is_entry) continue;
            EXPECT_EQ(bbq::zcow::read_int(e.get(), r.meta.doc.src()),
                      bbq::zcow::read_bits(run, e->name, r.meta.doc.src()))
                << rule << "." << e->name;
        }
        delete r.grammar;
    }
}

// The other dependent field: an @rest size holds the byte length of everything
// after it in its container, so resizing that content must move it.
TEST(CEKLaw, ResizingARestWindowUpdatesItsSize) {
    struct RestCase { const char* rule; const char* size_field; const char* grow; };
    const RestCase rest[] = {
        {"RestEof", "sz", "xs"},   // sz = bytes of the array that follows
    };

    for (const auto& k : rest) {
        std::vector<uint8_t> bytes;
        for (const auto& c : matrix_cases())
            if (std::strcmp(c.rule, k.rule) == 0) bytes = c.bytes;
        ASSERT_FALSE(bytes.empty()) << k.rule;

        auto r = layer3_compile_and_run(fixture_src().c_str(), k.rule, bytes);
        ASSERT_TRUE(r.success) << k.rule << ": " << r.error;

        const bbq::zcow::node* sz = bbq::zcow::child_named(r.meta.doc.root(), k.size_field);
        ASSERT_NE(sz, nullptr) << k.rule;
        const int64_t before = bbq::zcow::read_int(sz, r.meta.doc.src());

        bbq::zcow::transient t = r.meta.doc.begin_edit();
        const char* p[] = {k.grow};
        bbq::zcow::node* xs = t.own_path(p, nullptr, 1);
        ASSERT_NE(xs, nullptr) << k.rule;
        bbq::zcow::set_int(t.append(xs, CaptureType::UInt8), 0x7F);
        bbq::zcow::document v = std::move(t).commit();

        EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(v.root(), k.size_field), v.src()),
                  before + 1)
            << k.rule << ": the parser did not record which field measures this window";

        // ...and it has to come back OUT. A varint size field has no width of its own —
        // its width is a property of its value — so it is written back by re-encoding,
        // and a node that lost its encoding en route to the tree emits NOTHING while
        // still reading back correctly. One appended byte, one byte longer.
        EXPECT_EQ(v.serialize().size(), bytes.size() + 1)
            << k.rule << ": the size field did not survive being written back";
    }
}

// Rules whose TERMINATION reads how much input is left cannot be shape-stable
// across a round trip, and it is not a defect in the document: serializing yields
// the bytes the rule consumed, so `@remaining` is smaller on the way back in and
// the rule stops earlier. `Unt = array<uint8>(none, until(@remaining < 2))` over
// {1,2,3} takes two elements and consumes two bytes; re-parsing {1,2} takes one.
// The total length of the input is part of what such a parse means, and a
// consumed-prefix serialization does not carry it. Demonstrated below rather than
// quietly excluded.
TEST(CEKMatrix, TerminationOnRemainingIsNotRoundTripStable) {
    std::vector<uint8_t> bytes = {1, 2, 3};
    auto first = layer3_compile_and_run(fixture_src().c_str(), "Unt", bytes);
    ASSERT_TRUE(first.success) << first.error;
    EXPECT_EQ(bbq::zcow::size_of(bbq::zcow::child_named(first.meta.doc.root(), "xs")), 2u);

    std::vector<uint8_t> out = first.meta.doc.serialize();
    EXPECT_EQ(out, (std::vector<uint8_t>{1, 2}));      // byte-identical to what it consumed

    auto second = layer3_compile_and_run(fixture_src().c_str(), "Unt", out);
    ASSERT_TRUE(second.success) << second.error;
    // Fewer elements — the rule saw a shorter input, which is what it asked about.
    EXPECT_EQ(bbq::zcow::size_of(bbq::zcow::child_named(second.meta.doc.root(), "xs")), 1u);

    delete second.grammar;
    delete first.grammar;
}

// ...and feeding those bytes back gives the same document, for every rule whose
// meaning does not depend on how much input followed it.
TEST(CEKMatrix, EveryRuleReParsesToTheSameShape) {
    for (const auto& c : matrix_cases()) {
        if (std::strcmp(c.rule, "Unt") == 0) continue;   // see the test above
        auto first = layer3_compile_and_run(fixture_src().c_str(), c.rule, c.bytes);
        EXPECT_TRUE(first.success) << c.rule << ": " << first.error;
        if (!first.success) { delete first.grammar; continue; }
        std::vector<uint8_t> out = first.meta.doc.serialize();
        std::string shape1 = shape_of(first.meta.doc);

        auto second = layer3_compile_and_run(fixture_src().c_str(), c.rule, out);
        EXPECT_TRUE(second.success) << c.rule << ": " << second.error;
        if (!second.success) { delete second.grammar; delete first.grammar; continue; }
        EXPECT_EQ(shape_of(second.meta.doc), shape1) << c.rule;
        EXPECT_EQ(second.meta.doc.serialize(), out) << c.rule;

        delete second.grammar;
        delete first.grammar;
    }
}

// Taking a transient and committing without writing anything must not disturb the
// bytes: nothing was touched, so nothing stops being span-backed.
TEST(CEKMatrix, AnEmptyEditIsAlwaysIdentity) {
    for (const auto& c : matrix_cases()) {
        auto r = layer3_compile_and_run(fixture_src().c_str(), c.rule, c.bytes);
        EXPECT_TRUE(r.success) << c.rule << ": " << r.error;
        if (!r.success) { delete r.grammar; continue; }
        std::vector<uint8_t> before = r.meta.doc.serialize();

        bbq::zcow::document after = r.meta.doc.begin_edit().commit();
        EXPECT_EQ(after.serialize(), before) << c.rule;
        EXPECT_TRUE(all_span_backed(after.root())) << c.rule;
        delete r.grammar;
    }
}

// The other direction: build a document with no parsed bytes behind it, serialize
// it, and hand the result to the machine. Nothing here was ever read from a file,
// so every leaf owns its value.
TEST(CEKRoundTrip, ConstructedDocumentSerializesAndReParses) {
    bbq::zcow::builder b;
    b.add_field("a", 0, 0, CaptureType::UInt8);
    b.add_field("b", 0, 0, CaptureType::UInt16LE);
    b.add_field("c", 0, 0, CaptureType::UInt32BE);
    bbq::zcow::document blank = b.finish(true, 0, nullptr, 0).doc;

    bbq::zcow::transient t = blank.begin_edit();
    const char* pa[] = {"a"};
    const char* pb[] = {"b"};
    const char* pc[] = {"c"};
    bbq::zcow::set_int(t.own_path(pa, nullptr, 1), 0x07);
    bbq::zcow::set_int(t.own_path(pb, nullptr, 1), 0x0142);
    bbq::zcow::set_int(t.own_path(pc, nullptr, 1), 0x11223344);
    std::vector<uint8_t> out = std::move(t).commit().serialize();

    // Widths come from each leaf's declared type, since none of them has a span.
    EXPECT_EQ(out, (std::vector<uint8_t>{0x07, 0x42, 0x01, 0x11, 0x22, 0x33, 0x44}));

    auto r = layer3_compile_and_run(
        "Hdr = struct { a: uint8, b: uint16le, c: uint32be }", "Hdr", out);
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    const auto& src = r.meta.doc.src();
    const bbq::zcow::node* root = r.meta.doc.root();
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(root, "a"), src), 0x07);
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(root, "b"), src), 0x0142);
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(root, "c"), src), 0x11223344);
    EXPECT_EQ(r.meta.doc.serialize(), out);      // and it round-trips from there
    delete r.grammar;
}

// Constructing a counted array: the count is a field like any other, so building a
// valid document means setting it to match. Nothing recomputes it yet.
TEST(CEKRoundTrip, ConstructedArrayReParses) {
    bbq::zcow::builder b;
    b.add_field("n", 0, 0, CaptureType::UInt8);
    b.begin_array("xs", 0);
    b.end_array(0);
    bbq::zcow::document blank = b.finish(true, 0, nullptr, 0).doc;

    bbq::zcow::transient t = blank.begin_edit();
    const char* pxs[] = {"xs"};
    bbq::zcow::node* xs = t.own_path(pxs, nullptr, 1);
    ASSERT_NE(xs, nullptr);
    for (int v : {0x0A, 0x14, 0x1E})
        bbq::zcow::set_int(t.append(xs, CaptureType::UInt8), v);

    const char* pn[] = {"n"};
    bbq::zcow::set_int(t.own_path(pn, nullptr, 1), (int64_t)bbq::zcow::size_of(xs));
    std::vector<uint8_t> out = std::move(t).commit().serialize();

    EXPECT_EQ(out, (std::vector<uint8_t>{0x03, 0x0A, 0x14, 0x1E}));

    auto r = layer3_compile_and_run(
        "Doc = struct { n: uint8, xs: array<uint8>[n] }", "Doc", out);
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(bbq::zcow::size_of(bbq::zcow::child_named(r.meta.doc.root(), "xs")), 3u);
    EXPECT_EQ(r.meta.doc.serialize(), out);
    delete r.grammar;
}

// read → write → read, with a structural edit rather than a scalar one: the count
// must be corrected by hand for the result to re-parse, which is the writer layer's
// job and is not done anywhere yet.
TEST(CEKRoundTrip, AppendingToAnArrayNeedsItsCountCorrected) {
    std::vector<uint8_t> bytes = {0x02, 0x0A, 0x14};
    const char* src = "Doc = struct { n: uint8, xs: array<uint8>[n] }";

    auto r = layer3_compile_and_run(src, "Doc", bytes);
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;

    bbq::zcow::transient t = r.meta.doc.begin_edit();
    const char* pxs[] = {"xs"};
    bbq::zcow::node* xs = t.own_path(pxs, nullptr, 1);
    bbq::zcow::set_int(t.append(xs, CaptureType::UInt8), 0x1E);
    const char* pn[] = {"n"};
    bbq::zcow::set_int(t.own_path(pn, nullptr, 1), (int64_t)bbq::zcow::size_of(xs));
    std::vector<uint8_t> out = std::move(t).commit().serialize();

    EXPECT_EQ(out, (std::vector<uint8_t>{0x03, 0x0A, 0x14, 0x1E}));

    auto again = layer3_compile_and_run(src, "Doc", out);
    ASSERT_TRUE(again.success) << again.error;
    EXPECT_EQ(bbq::zcow::size_of(bbq::zcow::child_named(again.meta.doc.root(), "xs")), 3u);
    delete again.grammar;
    delete r.grammar;
}

TEST(CEKRoundTrip, ReadsThroughTheDocumentMatchTheBytes) {
    std::vector<uint8_t> bytes = {0x03, 0x0A, 0x14, 0x1E, 0x99};
    auto r = layer3_compile_and_run(
        "Doc = struct { n: uint8, xs: array<uint8>[n], tail: uint8 }", "Doc", bytes);
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;

    const bbq::zcow::node* root = r.meta.doc.root();
    const auto& src = r.meta.doc.src();
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(root, "n"), src), 3);
    const bbq::zcow::node* xs = bbq::zcow::child_named(root, "xs");
    ASSERT_EQ(bbq::zcow::size_of(xs), 3u);
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_at(xs, 0), src), 0x0A);
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_at(xs, 1), src), 0x14);
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_at(xs, 2), src), 0x1E);
    EXPECT_EQ(bbq::zcow::read_int(bbq::zcow::child_named(root, "tail"), src), 0x99);
    delete r.grammar;
}


