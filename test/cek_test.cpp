#include <gtest/gtest.h>

#include "ParseArena.h"
#include "StringPool.h"
#include "Capture.h"
#include "KontNode.h"
#include "Environment.h"
#include "Frame.h"
#include "Machine.h"

using namespace bbq::cek;

// Test helper: wrap an integer in an IntValue allocated in `arena`.
// Used by Binding initializers and add_field/add_computed calls in
// the migrated tests, since the new IR types those slots as `Value*`
// instead of the old `int64_t`.
static inline bbq::cek::Value* iv(bbq::cek::ParseArena& arena, int64_t v) {
    auto* x = arena.alloc<bbq::cek::IntValue>();
    x->v = v;
    return x;
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
    builder.add_field(nullptr, 0, 1, CaptureType::UInt8, iv(arena, 42));
    builder.add_field(nullptr, 1, 3, CaptureType::UInt16LE, iv(arena, 0x0201));
    EXPECT_EQ(builder.fields().size(), 2u);
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(builder.fields()[0].computed_value)->v, 42);
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(builder.fields()[1].computed_value)->v, 0x0201);
}

TEST(CEKCaptureBuilder, MarkRestore) {
    ParseArena arena;
    CaptureBuilder builder;
    builder.add_field(nullptr, 0, 1, CaptureType::UInt8, iv(arena, 1));
    auto m = builder.mark();
    builder.add_field(nullptr, 1, 2, CaptureType::UInt8, iv(arena, 2));
    builder.add_field(nullptr, 2, 3, CaptureType::UInt8, iv(arena, 3));
    EXPECT_EQ(builder.fields().size(), 3u);

    builder.restore(m);
    EXPECT_EQ(builder.fields().size(), 1u);
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(builder.fields()[0].computed_value)->v, 1);
}

TEST(CEKCaptureBuilder, AddComputed) {
    ParseArena arena;
    StringPool pool;
    CaptureBuilder builder;
    const char* nm = pool.intern("derived");
    builder.add_computed(nm, iv(arena, 0xABCD), /*pos=*/8);
    ASSERT_EQ(builder.fields().size(), 1u);
    const auto& f = builder.fields()[0];
    EXPECT_EQ(f.name, nm);
    EXPECT_EQ(f.type, CaptureType::Computed);
    EXPECT_EQ(f.start_offset, 8u);
    EXPECT_EQ(f.end_offset, 8u);   // zero-width at pos
    ASSERT_NE(f.computed_value, nullptr);
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(f.computed_value)->v, 0xABCD);
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
    match->field_name = "x";
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

TEST(CEKLayer2Leaves, AtEndFalseAtStart) {
    ParseArena arena;
    auto* k = arena.alloc<AtEndKont>();
    uint8_t buf[4] = {};
    auto r = run_leaf(k, arena, buf, 4);
    ASSERT_NE(r.result, nullptr);
    ASSERT_EQ(r.result->tag, ValueTag::BoolValue);
    EXPECT_FALSE(static_cast<BoolValue*>(r.result)->v);
}

TEST(CEKLayer2Leaves, AtEndTrueOnEmpty) {
    ParseArena arena;
    auto* k = arena.alloc<AtEndKont>();
    auto r = run_leaf(k, arena, nullptr, 0);
    ASSERT_NE(r.result, nullptr);
    EXPECT_TRUE(static_cast<BoolValue*>(r.result)->v);
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
    ASSERT_NE(meta.root, nullptr);
    ASSERT_EQ(meta.root->child_count, 1);
    EXPECT_STREQ(meta.root->children[0].name, "S");
    EXPECT_EQ(meta.root->children[0].type, CaptureType::Struct);
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
    CaptureMetadata meta;
};

MatchResult run_match_prim(ParseArena& arena, const char* name,
                           PrimitiveInfo prim,
                           const uint8_t* data, size_t length,
                           bool little_endian = true) {
    auto* halt = arena.alloc<HaltNode>();
    auto* match = arena.alloc<MatchPrimitiveNode>();
    match->field_name = name;
    match->prim = prim;
    match->next = halt;

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
    ASSERT_NE(r.meta.root, nullptr);
    ASSERT_EQ(r.meta.root->child_count, 1);
    EXPECT_EQ(r.meta.root->children[0].start_offset, 0u);
    EXPECT_EQ(r.meta.root->children[0].end_offset, 1u);
    EXPECT_EQ(r.meta.root->children[0].type, CaptureType::UInt8);
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
    CaptureMetadata meta;
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

    CEKMachine m;
    m.arena = &grammar->arena;
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
    ASSERT_NE(r.meta.root, nullptr);
    // Top-level struct fields land directly under root (no Hdr wrap)
    // — matches the user-facing convention `result.a` / `result.b`.
    ASSERT_EQ(r.meta.root->child_count, 2);
    EXPECT_STREQ(r.meta.root->children[0].name, "a");
    EXPECT_STREQ(r.meta.root->children[1].name, "b");
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
    ASSERT_NE(r.meta.root, nullptr);
    ASSERT_EQ(r.meta.root->child_count, 2);
    EXPECT_STREQ(r.meta.root->children[1].name, "twice");
    EXPECT_EQ(r.meta.root->children[1].type, CaptureType::Computed);
    ASSERT_NE(r.meta.root->children[1].computed_value, nullptr);
    EXPECT_EQ(r.meta.root->children[1].computed_value->tag, ValueTag::IntValue);
    EXPECT_EQ(static_cast<bbq::cek::IntValue*>(r.meta.root->children[1].computed_value)->v, 14);
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
    ASSERT_EQ(r.meta.root->child_count, 2);
    EXPECT_EQ(r.meta.root->children[1].type, CaptureType::Array);
    EXPECT_EQ(r.meta.root->children[1].child_count, 3);
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
    auto& s = r.meta.root->children[0];
    EXPECT_EQ(s.type, CaptureType::Array);
    EXPECT_EQ(s.child_count, 5);
    delete r.grammar;
}

TEST(CEKLayer3, ArrayUntilCondition) {
    auto r = layer3_compile_and_run(
        "Stream = array<uint8>(none, until(remaining < 2))",
        "Stream",
        {0xAA, 0xBB, 0xCC, 0xDD});  // stops when remaining < 2 → after 3rd byte
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto& s = r.meta.root->children[0];
    EXPECT_EQ(s.type, CaptureType::Array);
    EXPECT_GE(s.child_count, 1);   // some elements consumed
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
    auto& arr = r.meta.root->children[0];
    EXPECT_EQ(arr.type, CaptureType::Array);
    EXPECT_EQ(arr.child_count, 3);
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
    auto& arr = r.meta.root->children[0];
    EXPECT_EQ(arr.type, CaptureType::Array);
    EXPECT_EQ(arr.child_count, 0);
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
    ASSERT_EQ(r.meta.root->child_count, 2);
    EXPECT_EQ(r.meta.root->children[1].type, CaptureType::Bytes);
    EXPECT_EQ(r.meta.root->children[1].end_offset - r.meta.root->children[1].start_offset, 3u);
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
    ASSERT_NE(r.meta.root, nullptr);
    ASSERT_EQ(r.meta.root->child_count, 3);
    EXPECT_STREQ(r.meta.root->children[0].name, "val");
    EXPECT_STREQ(r.meta.root->children[1].name, "n");
    EXPECT_STREQ(r.meta.root->children[2].name, "kids");

    auto& kids = r.meta.root->children[2];
    EXPECT_EQ(kids.type, CaptureType::Array);
    ASSERT_EQ(kids.child_count, 2);

    // First child: leaf
    auto& kid0 = kids.children[0];
    ASSERT_EQ(kid0.child_count, 3);
    auto& kid0_kids = kid0.children[2];
    EXPECT_EQ(kid0_kids.type, CaptureType::Array);
    EXPECT_EQ(kid0_kids.child_count, 0);

    // Second child: has one grandchild
    auto& kid1 = kids.children[1];
    ASSERT_EQ(kid1.child_count, 3);
    auto& kid1_kids = kid1.children[2];
    EXPECT_EQ(kid1_kids.type, CaptureType::Array);
    ASSERT_EQ(kid1_kids.child_count, 1);

    auto& leaf = kid1_kids.children[0];
    ASSERT_EQ(leaf.child_count, 3);
    auto& leaf_kids = leaf.children[2];
    EXPECT_EQ(leaf_kids.child_count, 0);

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
    auto* m_z = arena.alloc<MatchPrimitiveNode>();
    m_z->field_name = pool.intern("z");
    m_z->prim = p_bool;
    m_z->next = end_st;
    auto* m_y = arena.alloc<MatchPrimitiveNode>();
    m_y->field_name = pool.intern("y");
    m_y->prim = p_u16le;
    m_y->next = m_z;
    auto* m_x = arena.alloc<MatchPrimitiveNode>();
    m_x->field_name = pool.intern("x");
    m_x->prim = p_u8;
    m_x->next = m_y;
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
    ASSERT_NE(meta.root, nullptr);
    ASSERT_EQ(meta.root->child_count, 1);
    auto& s = meta.root->children[0];
    EXPECT_EQ(s.type, CaptureType::Struct);
    EXPECT_STREQ(s.name, "S");
    ASSERT_EQ(s.child_count, 3);
    EXPECT_STREQ(s.children[0].name, "x");
    EXPECT_STREQ(s.children[1].name, "y");
    EXPECT_STREQ(s.children[2].name, "z");

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


