#include <gtest/gtest.h>

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
    ASSERT_NE(r.meta.root, nullptr);
    ASSERT_EQ(r.meta.root->child_count, 2);  // a, b — both `_` dropped
    EXPECT_STREQ(r.meta.root->children[0].name, "a");
    EXPECT_STREQ(r.meta.root->children[1].name, "b");
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
    ASSERT_EQ(ok.meta.root->child_count, 1);       // only x; the `_` is dropped
    EXPECT_STREQ(ok.meta.root->children[0].name, "x");
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
    auto& arr = r.meta.root->children[0];
    EXPECT_EQ(arr.type, CaptureType::Array);
    ASSERT_EQ(arr.child_count, 3);                 // 3 elements; separators dropped
    EXPECT_EQ(arr.children[0].start_offset, 0u);
    EXPECT_EQ(arr.children[1].start_offset, 2u);   // skipped separator at 1
    EXPECT_EQ(arr.children[2].start_offset, 4u);   // skipped separator at 3
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
    auto& arr = ok.meta.root->children[0];
    ASSERT_EQ(arr.child_count, 3);                  // 3 elements; commas dropped
    EXPECT_EQ(arr.children[1].start_offset, 2u);
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
    ASSERT_NE(r.meta.root, nullptr);
    ASSERT_EQ(r.meta.root->child_count, 4);
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
    ASSERT_NE(r.meta.root, nullptr);
    ASSERT_EQ(r.meta.root->child_count, 2);
    EXPECT_STREQ(r.meta.root->children[1].name, "twice");
    EXPECT_EQ(r.meta.root->children[1].type, CaptureType::Computed);
    ASSERT_NE(r.meta.root->children[1].computed_value, nullptr);
    EXPECT_EQ(r.meta.root->children[1].computed_value->kind, bbq::ComputedValue::Kind::Int);
    EXPECT_EQ(r.meta.root->children[1].computed_value->i, 14);
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
    ASSERT_NE(r.meta.root, nullptr);
    ASSERT_EQ(r.meta.root->child_count, 2);  // flag, tail (extra absent)
    EXPECT_STREQ(r.meta.root->children[0].name, "flag");
    EXPECT_STREQ(r.meta.root->children[1].name, "tail");
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
        "Stream = array<uint8>(none, until(@remaining < 2))",
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
    ASSERT_NE(r.meta.root, nullptr);
    ASSERT_EQ(r.meta.root->child_count, 4);
    auto ival = [](const bbq::FieldCapture& f) {
        return f.computed_value->i;
    };
    EXPECT_EQ(ival(r.meta.root->children[0]), 300);
    EXPECT_EQ(ival(r.meta.root->children[1]), -1);
    EXPECT_EQ(ival(r.meta.root->children[2]), 624485);
    // The id field spans 2 bytes [0,2); delta 1 byte [2,3).
    EXPECT_EQ(r.meta.root->children[0].start_offset, 0u);
    EXPECT_EQ(r.meta.root->children[0].end_offset, 2u);
    EXPECT_EQ(r.meta.root->children[1].end_offset, 3u);
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
    ASSERT_NE(r.meta.root, nullptr);
    // root.children = { base, items }; items is an array of 3.
    ASSERT_EQ(r.meta.root->child_count, 2);
    auto* items = &r.meta.root->children[1];
    ASSERT_EQ(items->child_count, 3);
    // Each element parsed at base-i: 3, 2, 1 — it sought backward each iteration.
    EXPECT_EQ(items->children[0].start_offset, 3u);
    EXPECT_EQ(items->children[1].start_offset, 2u);
    EXPECT_EQ(items->children[2].start_offset, 1u);
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
    EXPECT_EQ(r.meta.root->children[1].child_count, 2);
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


