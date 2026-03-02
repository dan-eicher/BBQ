#include <gtest/gtest.h>

#include "ParseArena.h"
#include "StringPool.h"
#include "Capture.h"
#include "KontNode.h"
#include "Environment.h"
#include "Frame.h"
#include "Machine.h"
#include "Expr.h"

using namespace bbq::cek;

// ── Helpers ───────────────────────────────────────────────

// Build expression nodes in arena
static CompiledExpr* make_int_lit(ParseArena& arena, int64_t val) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = ExprOp::IntLit;
    e->int_val = val;
    return e;
}

static CompiledExpr* make_bool_lit(ParseArena& arena, bool val) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = ExprOp::BoolLit;
    e->bool_val = val;
    return e;
}

static CompiledExpr* make_ref(ParseArena& arena, const char* name) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = ExprOp::Ref;
    e->ref_name = name;
    return e;
}

static CompiledExpr* make_binary(ParseArena& arena, ExprOp op,
                                  CompiledExpr* left, CompiledExpr* right) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = op;
    e->binary.left = left;
    e->binary.right = right;
    return e;
}

static CompiledExpr* make_unary(ParseArena& arena, ExprOp op,
                                 CompiledExpr* operand) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = op;
    e->unary.operand = operand;
    return e;
}

static CompiledExpr* make_ternary(ParseArena& arena, CompiledExpr* cond,
                                   CompiledExpr* then_, CompiledExpr* else_) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = ExprOp::Ternary;
    e->ternary.cond = cond;
    e->ternary.then_ = then_;
    e->ternary.else_ = else_;
    return e;
}

static CompiledExpr* make_special(ParseArena& arena, ExprOp op) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = op;
    return e;
}

static CompiledExpr* make_remaining(ParseArena& arena) {
    return make_special(arena, ExprOp::Remaining);
}

static CompiledExpr* make_field_access(ParseArena& arena, CompiledExpr* base,
                                        const char* field) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = ExprOp::FieldAccess;
    e->field_access.base = base;
    e->field_access.field = field;
    return e;
}

static CompiledExpr* make_index_access(ParseArena& arena, CompiledExpr* base,
                                        CompiledExpr* index) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = ExprOp::IndexAccess;
    e->index_access.base = base;
    e->index_access.index = index;
    return e;
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
    e->binding = {name, 42, 0, 1, CaptureType::UInt8};
    e->parent = nullptr;

    EXPECT_EQ(e->lookup(other), nullptr);
}

TEST(CEKEnvironment, SingleBinding) {
    ParseArena arena;
    StringPool pool;
    auto* name = pool.intern("value");

    auto* e = arena.alloc<Environment>();
    e->binding = {name, 100, 0, 4, CaptureType::UInt32LE};
    e->parent = nullptr;

    auto* found = e->lookup(name);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->value, 100);
    EXPECT_EQ(found->start_offset, 0u);
    EXPECT_EQ(found->end_offset, 4u);
}

TEST(CEKEnvironment, ChainLookup) {
    ParseArena arena;
    StringPool pool;
    auto* x = pool.intern("x");
    auto* y = pool.intern("y");

    auto* e1 = arena.alloc<Environment>();
    e1->binding = {x, 10, 0, 1, CaptureType::UInt8};
    e1->parent = nullptr;

    auto* e2 = arena.alloc<Environment>();
    e2->binding = {y, 20, 1, 3, CaptureType::UInt16LE};
    e2->parent = e1;

    // Find y in current
    auto* fy = e2->lookup(y);
    ASSERT_NE(fy, nullptr);
    EXPECT_EQ(fy->value, 20);

    // Find x in parent
    auto* fx = e2->lookup(x);
    ASSERT_NE(fx, nullptr);
    EXPECT_EQ(fx->value, 10);
}

TEST(CEKEnvironment, Shadowing) {
    ParseArena arena;
    StringPool pool;
    auto* name = pool.intern("x");

    auto* e1 = arena.alloc<Environment>();
    e1->binding = {name, 1, 0, 1, CaptureType::UInt8};
    e1->parent = nullptr;

    auto* e2 = arena.alloc<Environment>();
    e2->binding = {name, 2, 1, 2, CaptureType::UInt8};
    e2->parent = e1;

    // Should find the most recent binding
    auto* found = e2->lookup(name);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->value, 2);
}

// ── CaptureBuilder ─────────────────────────────────────────

TEST(CEKCaptureBuilder, AddField) {
    CaptureBuilder builder;
    builder.add_field(nullptr, 0, 1, CaptureType::UInt8, 42);
    builder.add_field(nullptr, 1, 3, CaptureType::UInt16LE, 0x0201);
    EXPECT_EQ(builder.fields().size(), 2u);
    EXPECT_EQ(builder.fields()[0].computed_value, 42);
    EXPECT_EQ(builder.fields()[1].computed_value, 0x0201);
}

TEST(CEKCaptureBuilder, MarkRestore) {
    CaptureBuilder builder;
    builder.add_field(nullptr, 0, 1, CaptureType::UInt8, 1);
    auto m = builder.mark();
    builder.add_field(nullptr, 1, 2, CaptureType::UInt8, 2);
    builder.add_field(nullptr, 2, 3, CaptureType::UInt8, 3);
    EXPECT_EQ(builder.fields().size(), 3u);

    builder.restore(m);
    EXPECT_EQ(builder.fields().size(), 1u);
    EXPECT_EQ(builder.fields()[0].computed_value, 1);
}

// ── CEKMachine ─────────────────────────────────────────────

// Helper: build a simple chain: MatchPrimitive → Halt
static MatchPrimitiveNode* make_prim_node(ParseArena& arena, StringPool& pool,
                                           const char* name, PrimitiveInfo prim,
                                           KontNode* next) {
    auto* node = arena.alloc<MatchPrimitiveNode>();
    node->field_name = name ? pool.intern(name) : nullptr;
    node->prim = prim;
    node->next = next;
    return node;
}

TEST(CEKMachine, ReadUint8) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0x42);
}

TEST(CEKMachine, ReadUint16LE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Little},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};
    auto result = m.execute_from(entry, data, sizeof(data), true);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, 0x0201);
    EXPECT_EQ(result.root->children[0].type, CaptureType::UInt16LE);
}

TEST(CEKMachine, ReadUint16BE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Big},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, 0x0102);
    EXPECT_EQ(result.root->children[0].type, CaptureType::UInt16BE);
}

TEST(CEKMachine, ReadUint32LE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Little},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x04, 0x03, 0x02, 0x01};
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, 0x01020304);
    EXPECT_EQ(result.root->children[0].type, CaptureType::UInt32LE);
}

TEST(CEKMachine, ReadUint32BE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Big},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, 0x01020304);
    EXPECT_EQ(result.root->children[0].type, CaptureType::UInt32BE);
}

TEST(CEKMachine, ReadInt8Signed) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Signed}, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF};
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, -1);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Int8);
}

TEST(CEKMachine, ReadBool) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "flag",
        {PrimKind::Bool}, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01};
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, 1);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Bool);
}

TEST(CEKMachine, DefaultEndianLE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    // Native endian — should use machine's default (LE)
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Native},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};
    auto result = m.execute_from(entry, data, sizeof(data), /*little_endian=*/true);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, 0x0201);
    EXPECT_EQ(result.root->children[0].type, CaptureType::UInt16LE);
}

TEST(CEKMachine, DefaultEndianBE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    // Native endian — should use machine's default (BE)
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Native},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};
    auto result = m.execute_from(entry, data, sizeof(data), /*little_endian=*/false);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, 0x0102);
    EXPECT_EQ(result.root->children[0].type, CaptureType::UInt16BE);
}

TEST(CEKMachine, ChainedReads) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* second = make_prim_node(arena, pool, "b",
        {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Little},
        halt);
    auto* first = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned},
        second);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF, 0x01, 0x02};
    auto result = m.execute_from(first, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 2);
    EXPECT_EQ(result.root->children[0].computed_value, 0xFF);
    EXPECT_EQ(result.root->children[1].computed_value, 0x0201);
}

TEST(CEKMachine, EnvBinding) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "x",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Big},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x00, 0x00, 0x01, 0x00};
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    // After execution, env should have the binding
    ASSERT_NE(m.env, nullptr);
    auto* name = pool.intern("x");
    auto* binding = m.env->lookup(name);
    ASSERT_NE(binding, nullptr);
    EXPECT_EQ(binding->value, 0x00000100);
    EXPECT_EQ(binding->start_offset, 0u);
    EXPECT_EQ(binding->end_offset, 4u);
}

TEST(CEKMachine, BoundsCheckFail) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned}, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};  // Only 2 bytes, need 4
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message, nullptr);
    EXPECT_EQ(result.error_offset, 0u);
}

TEST(CEKMachine, EmptyInput) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, halt);

    CEKMachine m;
    m.arena = &arena;
    auto result = m.execute_from(entry, nullptr, 0);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message, nullptr);
    EXPECT_EQ(result.error_offset, 0u);
}

TEST(CEKMachine, HaltOnly) {
    ParseArena arena;
    auto* halt = arena.alloc<HaltNode>();

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01};
    auto result = m.execute_from(halt, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 0u);
    // No captures
    EXPECT_EQ(result.root, nullptr);
}

TEST(CEKMachine, MultipleCapturesInResult) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* third = make_prim_node(arena, pool, "c",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, halt);
    auto* second = make_prim_node(arena, pool, "b",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, third);
    auto* first = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, second);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0A, 0x0B, 0x0C};
    auto result = m.execute_from(first, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 3);

    // Verify each capture has correct offsets and values
    auto* a_name = pool.intern("a");
    auto* b_name = pool.intern("b");
    auto* c_name = pool.intern("c");

    auto* fa = result.root->child(a_name);
    ASSERT_NE(fa, nullptr);
    EXPECT_EQ(fa->computed_value, 0x0A);
    EXPECT_EQ(fa->start_offset, 0u);
    EXPECT_EQ(fa->end_offset, 1u);

    auto* fb = result.root->child(b_name);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->computed_value, 0x0B);
    EXPECT_EQ(fb->start_offset, 1u);
    EXPECT_EQ(fb->end_offset, 2u);

    auto* fc = result.root->child(c_name);
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc->computed_value, 0x0C);
    EXPECT_EQ(fc->start_offset, 2u);
    EXPECT_EQ(fc->end_offset, 3u);
}

// ── Phase 3: Struct + Nested Captures ─────────────────────

TEST(CEKCaptureBuilder, BeginEndStructCreatesNestedCapture) {
    ParseArena arena;
    StringPool pool;
    CaptureBuilder builder;
    auto* name = pool.intern("header");

    builder.begin_struct(name, 0);
    builder.add_field(pool.intern("a"), 0, 1, CaptureType::UInt8, 0x0A);
    builder.add_field(pool.intern("b"), 1, 2, CaptureType::UInt8, 0x0B);
    builder.end_struct(2);

    auto meta = builder.finish(arena, true, 2);
    ASSERT_NE(meta.root, nullptr);
    // Root should have 1 child: the struct
    EXPECT_EQ(meta.root->child_count, 1);
    auto* hdr = meta.root->child(name);
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->type, CaptureType::Struct);
    EXPECT_EQ(hdr->start_offset, 0u);
    EXPECT_EQ(hdr->end_offset, 2u);
    EXPECT_EQ(hdr->child_count, 2);
    EXPECT_EQ(hdr->children[0].computed_value, 0x0A);
    EXPECT_EQ(hdr->children[1].computed_value, 0x0B);
}

TEST(CEKCaptureBuilder, NestedStructInsideStruct) {
    ParseArena arena;
    StringPool pool;
    CaptureBuilder builder;
    auto* outer = pool.intern("outer");
    auto* inner = pool.intern("inner");
    auto* x = pool.intern("x");

    builder.begin_struct(outer, 0);
    builder.add_field(pool.intern("a"), 0, 1, CaptureType::UInt8, 1);
    builder.begin_struct(inner, 1);
    builder.add_field(x, 1, 2, CaptureType::UInt8, 2);
    builder.end_struct(2);
    builder.end_struct(2);

    auto meta = builder.finish(arena, true, 2);
    ASSERT_NE(meta.root, nullptr);
    EXPECT_EQ(meta.root->child_count, 1);
    auto* out = meta.root->child(outer);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->child_count, 2); // 'a' and inner struct
    auto* inn = out->child(inner);
    ASSERT_NE(inn, nullptr);
    EXPECT_EQ(inn->type, CaptureType::Struct);
    EXPECT_EQ(inn->child_count, 1);
    EXPECT_EQ(inn->children[0].computed_value, 2);
}

TEST(CEKCaptureBuilder, MarkRestoreWithStructScoping) {
    CaptureBuilder builder;
    StringPool pool;

    builder.add_field(pool.intern("a"), 0, 1, CaptureType::UInt8, 1);
    auto m = builder.mark();

    builder.begin_struct(pool.intern("s"), 1);
    builder.add_field(pool.intern("b"), 1, 2, CaptureType::UInt8, 2);
    // Don't end_struct — simulate backtrack
    builder.restore(m);

    EXPECT_EQ(builder.fields().size(), 1u);
    EXPECT_EQ(builder.fields()[0].computed_value, 1);
}

TEST(CEKMachine, SingleStructWithTwoFields) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;
    auto* field_b = make_prim_node(arena, pool, "b",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, end_s);
    auto* field_a = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, field_b);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("header");
    begin_s->next = field_a;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0A, 0x0B};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 1);
    auto* hdr = result.root->child(pool.intern("header"));
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->type, CaptureType::Struct);
    EXPECT_EQ(hdr->child_count, 2);
    EXPECT_EQ(hdr->children[0].computed_value, 0x0A);
    EXPECT_EQ(hdr->children[1].computed_value, 0x0B);
}

TEST(CEKMachine, NestedStructs) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    // outer { inner { x:u8 } }
    auto* end_outer = arena.alloc<EndStructNode>();
    end_outer->next = halt;
    auto* end_inner = arena.alloc<EndStructNode>();
    end_inner->next = end_outer;
    auto* field_x = make_prim_node(arena, pool, "x",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, end_inner);
    auto* begin_inner = arena.alloc<BeginStructNode>();
    begin_inner->struct_name = pool.intern("inner");
    begin_inner->next = field_x;
    auto* begin_outer = arena.alloc<BeginStructNode>();
    begin_outer->struct_name = pool.intern("outer");
    begin_outer->next = begin_inner;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(begin_outer, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    auto* out = result.root->child(pool.intern("outer"));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->type, CaptureType::Struct);
    EXPECT_EQ(out->child_count, 1);
    auto* inn = out->child(pool.intern("inner"));
    ASSERT_NE(inn, nullptr);
    EXPECT_EQ(inn->type, CaptureType::Struct);
    EXPECT_EQ(inn->child_count, 1);
    EXPECT_EQ(inn->children[0].computed_value, 0x42);
}

TEST(CEKMachine, StructCapturesHaveCorrectOffsets) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;
    auto* field_b = make_prim_node(arena, pool, "b",
        {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Little},
        end_s);
    auto* field_a = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, field_b);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("pkt");
    begin_s->next = field_a;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF, 0x01, 0x02};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* pkt = result.root->child(pool.intern("pkt"));
    ASSERT_NE(pkt, nullptr);
    EXPECT_EQ(pkt->start_offset, 0u);
    EXPECT_EQ(pkt->end_offset, 3u);
    EXPECT_EQ(pkt->children[0].start_offset, 0u);
    EXPECT_EQ(pkt->children[0].end_offset, 1u);
    EXPECT_EQ(pkt->children[1].start_offset, 1u);
    EXPECT_EQ(pkt->children[1].end_offset, 3u);
}

TEST(CEKMachine, StructChildLookupByInternedName) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;
    auto* field = make_prim_node(arena, pool, "magic",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, end_s);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("s");
    begin_s->next = field;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xAB};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* s = result.root->child(pool.intern("s"));
    ASSERT_NE(s, nullptr);
    // Lookup by interned name
    auto* magic = s->child(pool.intern("magic"));
    ASSERT_NE(magic, nullptr);
    EXPECT_EQ(magic->computed_value, 0xAB);
    // Lookup with different pointer to same string should fail
    // (this tests that pointer equality is used)
    const char local[] = "magic";
    EXPECT_EQ(s->child(local), nullptr);
}

TEST(CEKMachine, EmptyStruct) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("empty");
    begin_s->next = end_s;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 0u);
    ASSERT_NE(result.root, nullptr);
    auto* empty = result.root->child(pool.intern("empty"));
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->type, CaptureType::Struct);
    EXPECT_EQ(empty->child_count, 0);
    EXPECT_EQ(empty->start_offset, 0u);
    EXPECT_EQ(empty->end_offset, 0u);
}

// ── Phase 4: Expressions + Constraints + Compute ──────────

TEST(CEKExpr, IntLit) {
    ParseArena arena;
    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0};
    m.input = data;
    m.input_length = 1;
    m.pos = 0;
    m.env = nullptr;
    m.failed = false;

    auto* expr = make_int_lit(arena, 42);
    EXPECT_EQ(eval(expr, &m), 42);
    EXPECT_FALSE(m.failed);
}

TEST(CEKExpr, BoolLit) {
    ParseArena arena;
    CEKMachine m;
    m.arena = &arena;
    m.failed = false;

    EXPECT_EQ(eval(make_bool_lit(arena, true), &m), 1);
    EXPECT_EQ(eval(make_bool_lit(arena, false), &m), 0);
}

TEST(CEKExpr, RefLookup) {
    ParseArena arena;
    StringPool pool;
    CEKMachine m;
    m.arena = &arena;
    m.failed = false;

    auto* name = pool.intern("x");
    auto* env = arena.alloc<Environment>();
    env->binding = {name, 99, 0, 1, CaptureType::UInt8};
    env->parent = nullptr;
    m.env = env;

    auto* expr = make_ref(arena, name);
    EXPECT_EQ(eval(expr, &m), 99);
}

TEST(CEKExpr, ArithmeticOps) {
    ParseArena arena;
    CEKMachine m;
    m.arena = &arena;
    m.failed = false;

    EXPECT_EQ(eval(make_binary(arena, ExprOp::Add,
        make_int_lit(arena, 10), make_int_lit(arena, 20)), &m), 30);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Sub,
        make_int_lit(arena, 50), make_int_lit(arena, 8)), &m), 42);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Mul,
        make_int_lit(arena, 6), make_int_lit(arena, 7)), &m), 42);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Div,
        make_int_lit(arena, 84), make_int_lit(arena, 2)), &m), 42);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Mod,
        make_int_lit(arena, 10), make_int_lit(arena, 3)), &m), 1);
}

TEST(CEKExpr, ComparisonOps) {
    ParseArena arena;
    CEKMachine m;
    m.arena = &arena;
    m.failed = false;

    EXPECT_EQ(eval(make_binary(arena, ExprOp::Eq,
        make_int_lit(arena, 5), make_int_lit(arena, 5)), &m), 1);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Eq,
        make_int_lit(arena, 5), make_int_lit(arena, 6)), &m), 0);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Ne,
        make_int_lit(arena, 5), make_int_lit(arena, 6)), &m), 1);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Lt,
        make_int_lit(arena, 3), make_int_lit(arena, 5)), &m), 1);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Lt,
        make_int_lit(arena, 5), make_int_lit(arena, 3)), &m), 0);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Le,
        make_int_lit(arena, 5), make_int_lit(arena, 5)), &m), 1);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Gt,
        make_int_lit(arena, 5), make_int_lit(arena, 3)), &m), 1);
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Ge,
        make_int_lit(arena, 5), make_int_lit(arena, 5)), &m), 1);
}

TEST(CEKExpr, TernaryExpr) {
    ParseArena arena;
    CEKMachine m;
    m.arena = &arena;
    m.failed = false;

    // true ? 10 : 20
    auto* t = make_ternary(arena,
        make_bool_lit(arena, true), make_int_lit(arena, 10), make_int_lit(arena, 20));
    EXPECT_EQ(eval(t, &m), 10);

    // false ? 10 : 20
    auto* f = make_ternary(arena,
        make_bool_lit(arena, false), make_int_lit(arena, 10), make_int_lit(arena, 20));
    EXPECT_EQ(eval(f, &m), 20);
}

TEST(CEKExpr, PosRemainingEOI) {
    ParseArena arena;
    CEKMachine m;
    m.arena = &arena;
    m.failed = false;
    uint8_t data[] = {1, 2, 3, 4, 5};
    m.input = data;
    m.input_length = 5;
    m.pos = 2;

    EXPECT_EQ(eval(make_special(arena, ExprOp::Pos), &m), 2);
    EXPECT_EQ(eval(make_special(arena, ExprOp::Remaining), &m), 3);
    EXPECT_EQ(eval(make_special(arena, ExprOp::EOI), &m), 5);
    EXPECT_EQ(eval(make_special(arena, ExprOp::AtEnd), &m), 0);

    m.pos = 5;
    EXPECT_EQ(eval(make_special(arena, ExprOp::AtEnd), &m), 1);
    EXPECT_EQ(eval(make_special(arena, ExprOp::Remaining), &m), 0);
}

TEST(CEKExpr, UndefinedRefFails) {
    ParseArena arena;
    StringPool pool;
    CEKMachine m;
    m.arena = &arena;
    m.env = nullptr;
    m.failed = false;
    m.best_error_pos = 0;
    m.best_error_msg = nullptr;
    m.pos = 0;

    auto* expr = make_ref(arena, pool.intern("nonexistent"));
    eval(expr, &m);
    EXPECT_TRUE(m.failed);
}

TEST(CEKMachine, EvalConstraintPassing) {
    ParseArena arena;
    StringPool pool;
    // Read u8, then constrain val == 0x42
    auto* halt = arena.alloc<HaltNode>();
    auto* constraint_node = arena.alloc<EvalConstraintNode>();
    constraint_node->next = halt;
    // Build: val == 0x42
    constraint_node->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x42));
    auto* read = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, constraint_node);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(read, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0x42);
}

TEST(CEKMachine, EvalConstraintFailing) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* constraint_node = arena.alloc<EvalConstraintNode>();
    constraint_node->next = halt;
    constraint_node->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x42));
    auto* read = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, constraint_node);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF};  // val=0xFF, not 0x42
    auto result = m.execute_from(read, data, sizeof(data));

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message, nullptr);
    EXPECT_EQ(result.error_offset, 1u);  // Failed after reading 1 byte
}

TEST(CEKMachine, BindComputeBindsValueAndCaptures) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* compute = arena.alloc<BindComputeNode>();
    compute->field_name = pool.intern("total");
    compute->expr = make_int_lit(arena, 99);
    compute->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01};
    auto result = m.execute_from(compute, data, sizeof(data));

    EXPECT_TRUE(result.success);
    // Check env binding
    auto* binding = m.env->lookup(pool.intern("total"));
    ASSERT_NE(binding, nullptr);
    EXPECT_EQ(binding->value, 99);
    EXPECT_EQ(binding->type, CaptureType::Computed);
    // Check capture
    ASSERT_NE(result.root, nullptr);
    auto* total = result.root->child(pool.intern("total"));
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->computed_value, 99);
    EXPECT_EQ(total->type, CaptureType::Computed);
}

TEST(CEKMachine, StructWithFieldAndConstraint) {
    ParseArena arena;
    StringPool pool;
    // struct { magic:u8 where magic == 0xAB, len:u8 }
    auto* halt = arena.alloc<HaltNode>();
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;
    auto* field_len = make_prim_node(arena, pool, "len",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, end_s);
    auto* constraint = arena.alloc<EvalConstraintNode>();
    constraint->next = field_len;
    constraint->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("magic")),
        make_int_lit(arena, 0xAB));
    auto* field_magic = make_prim_node(arena, pool, "magic",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, constraint);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("pkt");
    begin_s->next = field_magic;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xAB, 0x05};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    auto* pkt = result.root->child(pool.intern("pkt"));
    ASSERT_NE(pkt, nullptr);
    EXPECT_EQ(pkt->child_count, 2);
    auto* magic = pkt->child(pool.intern("magic"));
    ASSERT_NE(magic, nullptr);
    EXPECT_EQ(magic->computed_value, 0xAB);
    auto* len = pkt->child(pool.intern("len"));
    ASSERT_NE(len, nullptr);
    EXPECT_EQ(len->computed_value, 0x05);
}

TEST(CEKMachine, StructWithComputeUsingPriorField) {
    ParseArena arena;
    StringPool pool;
    // struct { a:u8, total = a + 10 }
    auto* halt = arena.alloc<HaltNode>();
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;
    auto* compute = arena.alloc<BindComputeNode>();
    compute->field_name = pool.intern("total");
    compute->expr = make_binary(arena, ExprOp::Add,
        make_ref(arena, pool.intern("a")),
        make_int_lit(arena, 10));
    compute->next = end_s;
    auto* field_a = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, compute);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("s");
    begin_s->next = field_a;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x05};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* s = result.root->child(pool.intern("s"));
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->child_count, 2); // a + total
    auto* total = s->child(pool.intern("total"));
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->computed_value, 15); // 5 + 10
}

// ── Phase 5: Rule Invocation ──────────────────────────────

// Helper: build a simple rule body (reads fields, returns)
// Rule body: field_a:u8 → field_b:u8 → ReturnRule
static KontNode* make_simple_rule(ParseArena& arena, StringPool& pool,
                                   const char* fa, const char* fb) {
    auto* ret = arena.alloc<ReturnRuleNode>();
    ret->next = nullptr;
    auto* b = make_prim_node(arena, pool, fb,
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, ret);
    auto* a = make_prim_node(arena, pool, fa,
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, b);
    return a;
}

TEST(CEKMachine, InvokeRuleBasic) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Rule body: x:u8, y:u8, ReturnRule
    auto* rule_body = make_simple_rule(arena, pool, "x", "y");

    // Caller: InvokeRule("inner", rule_body) → Halt
    auto* invoke = arena.alloc<InvokeRuleNode>();
    invoke->rule_name = pool.intern("inner");
    invoke->entry = rule_body;
    invoke->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0A, 0x0B};
    auto result = m.execute_from(invoke, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    // Root should contain a struct named "inner"
    auto* inner = result.root->child(pool.intern("inner"));
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->type, CaptureType::Struct);
    EXPECT_EQ(inner->child_count, 2);
    EXPECT_EQ(inner->children[0].computed_value, 0x0A);
    EXPECT_EQ(inner->children[1].computed_value, 0x0B);
}

TEST(CEKMachine, CallerEnvRestoredAfterRuleReturn) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Rule body: x:u8, ReturnRule
    auto* ret = arena.alloc<ReturnRuleNode>();
    auto* rule_field = make_prim_node(arena, pool, "x",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, ret);

    // Caller: a:u8 → InvokeRule("inner") → Halt
    auto* invoke = arena.alloc<InvokeRuleNode>();
    invoke->rule_name = pool.intern("inner");
    invoke->entry = rule_field;
    invoke->next = halt;
    auto* field_a = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, invoke);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};
    auto result = m.execute_from(field_a, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    // After rule return, "x" (rule-internal) should NOT be in env
    // The env should have: inner (struct binding) → a
    auto* x_binding = m.env->lookup(pool.intern("x"));
    EXPECT_EQ(x_binding, nullptr);
    // But "a" should still be accessible
    auto* a_binding = m.env->lookup(pool.intern("a"));
    ASSERT_NE(a_binding, nullptr);
    EXPECT_EQ(a_binding->value, 0x01);
}

TEST(CEKMachine, RuleBindingAccessibleInCallerEnv) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Rule body: val:u8, ReturnRule
    auto* ret = arena.alloc<ReturnRuleNode>();
    auto* rule_field = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, ret);

    // Caller: InvokeRule("inner") → Halt
    auto* invoke = arena.alloc<InvokeRuleNode>();
    invoke->rule_name = pool.intern("inner");
    invoke->entry = rule_field;
    invoke->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(invoke, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    // "inner" should be bound as a Struct in the caller's env
    auto* inner_binding = m.env->lookup(pool.intern("inner"));
    ASSERT_NE(inner_binding, nullptr);
    EXPECT_EQ(inner_binding->type, CaptureType::Struct);
    EXPECT_EQ(inner_binding->start_offset, 0u);
    EXPECT_EQ(inner_binding->end_offset, 1u);
}

TEST(CEKMachine, NestedRuleCalls) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Rule C body: z:u8, ReturnRule
    auto* ret_c = arena.alloc<ReturnRuleNode>();
    auto* c_field = make_prim_node(arena, pool, "z",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, ret_c);

    // Rule B body: InvokeRule("c", c_field) → ReturnRule
    auto* ret_b = arena.alloc<ReturnRuleNode>();
    auto* invoke_c = arena.alloc<InvokeRuleNode>();
    invoke_c->rule_name = pool.intern("c");
    invoke_c->entry = c_field;
    invoke_c->next = ret_b;

    // Caller A: InvokeRule("b") → Halt
    auto* invoke_b = arena.alloc<InvokeRuleNode>();
    invoke_b->rule_name = pool.intern("b");
    invoke_b->entry = invoke_c;
    invoke_b->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF};
    auto result = m.execute_from(invoke_b, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    // Root → b (struct) → c (struct) → z (field)
    auto* b = result.root->child(pool.intern("b"));
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->type, CaptureType::Struct);
    auto* c = b->child(pool.intern("c"));
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->type, CaptureType::Struct);
    EXPECT_EQ(c->child_count, 1);
    EXPECT_EQ(c->children[0].computed_value, 0xFF);
}

TEST(CEKMachine, RuleWithConstraint) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Rule body: magic:u8 where magic == 0xAB → ReturnRule
    auto* ret = arena.alloc<ReturnRuleNode>();
    auto* constraint = arena.alloc<EvalConstraintNode>();
    constraint->next = ret;
    constraint->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("magic")),
        make_int_lit(arena, 0xAB));
    auto* field = make_prim_node(arena, pool, "magic",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, constraint);

    auto* invoke = arena.alloc<InvokeRuleNode>();
    invoke->rule_name = pool.intern("header");
    invoke->entry = field;
    invoke->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xAB};
    auto result = m.execute_from(invoke, data, sizeof(data));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    auto* hdr = result.root->child(pool.intern("header"));
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->type, CaptureType::Struct);
    auto* magic = hdr->child(pool.intern("magic"));
    ASSERT_NE(magic, nullptr);
    EXPECT_EQ(magic->computed_value, 0xAB);

    // Failing case
    uint8_t bad[] = {0xFF};
    auto result2 = m.execute_from(invoke, bad, sizeof(bad));
    EXPECT_FALSE(result2.success);
    EXPECT_EQ(result2.error_offset, 1u);
}

TEST(CEKMachine, RuleCallFollowedByMoreFields) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Rule body: x:u8, ReturnRule
    auto* ret = arena.alloc<ReturnRuleNode>();
    auto* rule_field = make_prim_node(arena, pool, "x",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, ret);

    // Caller: InvokeRule("inner") → b:u8 → Halt
    auto* field_b = make_prim_node(arena, pool, "b",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, halt);
    auto* invoke = arena.alloc<InvokeRuleNode>();
    invoke->rule_name = pool.intern("inner");
    invoke->entry = rule_field;
    invoke->next = field_b;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0A, 0x0B};
    auto result = m.execute_from(invoke, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 2); // inner struct + b field
    auto* inner = result.root->child(pool.intern("inner"));
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->type, CaptureType::Struct);
    EXPECT_EQ(inner->child_count, 1);
    EXPECT_EQ(inner->children[0].computed_value, 0x0A);
    auto* b = result.root->child(pool.intern("b"));
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->computed_value, 0x0B);
}

// ── Phase 6: Backtracking ─────────────────────────────────

// Choice codegen pattern (a la Peggy's OP_CHOICE/OP_COMMIT):
//   BeginChoice [alt2, alt3] → alt1_body → Commit → join
//                               alt2_body → Commit → join
//                               alt3_body → join   (last alt: no commit needed)

TEST(CEKMachine, ChoiceFirstSucceeds) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // join point = halt
    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = halt;

    // Alt 1: val:u8 where val==0x0A → commit → halt
    auto* c1 = arena.alloc<EvalConstraintNode>();
    c1->next = commit;
    c1->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x0A));
    auto* alt1 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c1);

    // Alt 2 (last): val:u8 where val==0x0B → halt (no commit)
    auto* c2 = arena.alloc<EvalConstraintNode>();
    c2->next = halt;
    c2->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x0B));
    auto* alt2 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c2);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = alt1;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0A};
    auto result = m.execute_from(choice, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0x0A);
}

TEST(CEKMachine, ChoiceFirstFailsSecondSucceeds) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = halt;

    // Alt 1: val:u8 where val==0x0A → commit → halt
    auto* c1 = arena.alloc<EvalConstraintNode>();
    c1->next = commit;
    c1->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x0A));
    auto* alt1 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c1);

    // Alt 2 (last): val:u8 where val==0x0B → halt
    auto* c2 = arena.alloc<EvalConstraintNode>();
    c2->next = halt;
    c2->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x0B));
    auto* alt2 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c2);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = alt1;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0B};
    auto result = m.execute_from(choice, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, 0x0B);
}

TEST(CEKMachine, ChoiceThreeAltsThirdMatches) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = halt;

    // Alt 1 and 2 get commit, alt 3 (last) goes straight to halt
    auto make_constrained_alt = [&](int64_t expected, KontNode* join) -> KontNode* {
        auto* c = arena.alloc<EvalConstraintNode>();
        c->next = join;
        c->constraint = make_binary(arena, ExprOp::Eq,
            make_ref(arena, pool.intern("val")),
            make_int_lit(arena, expected));
        return make_prim_node(arena, pool, "val",
            {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c);
    };

    auto* alt1 = make_constrained_alt(0x01, commit);
    auto* alt2 = make_constrained_alt(0x02, commit);
    auto* alt3 = make_constrained_alt(0x03, halt);    // Last: no commit

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = alt1;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(2 * sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2;
    remaining[1] = alt3;
    choice->alternatives = remaining;
    choice->alt_count = 2;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x03};
    auto result = m.execute_from(choice, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0x03);
}

TEST(CEKMachine, ChoiceAllAlternativesFail) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = halt;

    auto make_constrained_alt = [&](int64_t expected, KontNode* join) -> KontNode* {
        auto* c = arena.alloc<EvalConstraintNode>();
        c->next = join;
        c->constraint = make_binary(arena, ExprOp::Eq,
            make_ref(arena, pool.intern("val")),
            make_int_lit(arena, expected));
        return make_prim_node(arena, pool, "val",
            {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c);
    };

    auto* alt1 = make_constrained_alt(0x01, commit);
    auto* alt2 = make_constrained_alt(0x02, halt);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = alt1;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF};
    auto result = m.execute_from(choice, data, sizeof(data));

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message, nullptr);
    EXPECT_EQ(result.error_offset, 1u);  // Failed after reading the byte
}

TEST(CEKMachine, ChoiceRestoresPosEnvOnBacktrack) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = halt;

    // Alt 1: reads u16 (2 bytes) then fails constraint
    auto* c1 = arena.alloc<EvalConstraintNode>();
    c1->next = commit;
    c1->constraint = make_bool_lit(arena, false);  // Always fails
    auto* alt1 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Little}, c1);

    // Alt 2 (last): reads u8 (1 byte) → halt
    auto* alt2 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, halt);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = alt1;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xAA, 0xBB};
    auto result = m.execute_from(choice, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->children[0].computed_value, 0xAA);
}

TEST(CEKMachine, ChoiceRestoresCapturesOnBacktrack) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = halt;

    // First: read a:u8, then choice
    // Alt 1: reads b:u8 then fails
    auto* c1 = arena.alloc<EvalConstraintNode>();
    c1->next = commit;
    c1->constraint = make_bool_lit(arena, false);
    auto* alt1_b = make_prim_node(arena, pool, "b",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c1);

    // Alt 2 (last): reads c:u8 → halt
    auto* alt2_c = make_prim_node(arena, pool, "c",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, halt);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = alt1_b;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2_c;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    auto* field_a = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, choice);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};
    auto result = m.execute_from(field_a, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 2);
    auto* a = result.root->child(pool.intern("a"));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->computed_value, 0x01);
    auto* c = result.root->child(pool.intern("c"));
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->computed_value, 0x02);
}

TEST(CEKMachine, CommitPreventsBacktrackIntoChoice) {
    // The key test: after a choice commits, a LATER failure must NOT
    // backtrack into the already-committed choice.
    //
    // Graph: BeginChoice [alt2] → alt1(val:u8, commit) → bad:u32(fails) → halt
    //                              alt2(val:u8)                          → halt
    //
    // Input: [0x0A] — alt1 succeeds and commits, then bad:u32 fails.
    // Without commit, fail() would find the AlternativeFrame and try alt2.
    // With commit, the frame is gone — parse fails outright.
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // After the committed choice, try to read u32 (will fail — only 1 byte input)
    auto* bad_read = make_prim_node(arena, pool, "bad",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned}, halt);

    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = bad_read;

    // Alt 1: val:u8 → commit → bad_read (which will fail)
    auto* alt1 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, commit);

    // Alt 2 (last): val:u8 → halt
    auto* alt2 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, halt);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = alt1;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0A};  // 1 byte: alt1 reads it, commits, then bad_read fails
    auto result = m.execute_from(choice, data, sizeof(data));

    // Must FAIL — bad_read can't read u32 from 0 remaining bytes.
    // Without CommitChoice, it would backtrack to alt2 and succeed.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_offset, 1u);  // Failed at pos 1 trying to read u32
}

TEST(CEKMachine, OptionalPresent) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Optional body: u8 → EndOptional → Halt
    auto* end_opt = arena.alloc<EndOptionalNode>();
    end_opt->next = halt;
    auto* opt_body = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, end_opt);

    auto* begin_opt = arena.alloc<BeginOptionalNode>();
    begin_opt->inner = opt_body;
    begin_opt->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(begin_opt, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0x42);
}

TEST(CEKMachine, OptionalAbsentSkips) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* end_opt = arena.alloc<EndOptionalNode>();
    end_opt->next = halt;
    auto* opt_body = make_prim_node(arena, pool, "big",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned}, end_opt);

    auto* begin_opt = arena.alloc<BeginOptionalNode>();
    begin_opt->inner = opt_body;
    begin_opt->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01};
    auto result = m.execute_from(begin_opt, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 0u);
    // No captures should have been produced (optional was absent)
    EXPECT_EQ(result.root, nullptr);
}

TEST(CEKMachine, OptionalAbsentProducesNoCapture) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* end_opt = arena.alloc<EndOptionalNode>();
    end_opt->next = halt;
    auto* opt_body = make_prim_node(arena, pool, "b",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned}, end_opt);
    auto* begin_opt = arena.alloc<BeginOptionalNode>();
    begin_opt->inner = opt_body;
    begin_opt->next = halt;
    auto* field_a = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, begin_opt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF};
    auto result = m.execute_from(field_a, data, sizeof(data));

    EXPECT_TRUE(result.success);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0xFF);
}

TEST(CEKMachine, NestedOptionalInsideChoice) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = halt;

    // Alt 1: optional(u32) → u8 where val==0x0A → commit → halt
    auto* end_opt = arena.alloc<EndOptionalNode>();
    auto* c1 = arena.alloc<EvalConstraintNode>();
    c1->next = commit;
    c1->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x0A));
    auto* read_val1 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c1);
    end_opt->next = read_val1;
    auto* opt_body = make_prim_node(arena, pool, "extra",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned}, end_opt);
    auto* begin_opt = arena.alloc<BeginOptionalNode>();
    begin_opt->inner = opt_body;
    begin_opt->next = read_val1;

    // Alt 2 (last): u8 where val==0x0B → halt
    auto* c2 = arena.alloc<EvalConstraintNode>();
    c2->next = halt;
    c2->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x0B));
    auto* alt2 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c2);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = begin_opt;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0A};
    auto result = m.execute_from(choice, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    // Should have 1 capture: val=0x0A (optional was absent, so no "extra")
    EXPECT_EQ(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0x0A);
}

TEST(CEKMachine, ChoiceInsideStruct) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // struct pkt { magic:u8, choice(val where==0x01 | val where==0x02) }
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;

    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = end_s;

    // Alt1: val:u8 constraint==0x01 → commit → end_struct
    auto* c1 = arena.alloc<EvalConstraintNode>();
    c1->next = commit;
    c1->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x01));
    auto* alt1 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c1);

    // Alt2 (last): val:u8 constraint==0x02 → end_struct
    auto* c2 = arena.alloc<EvalConstraintNode>();
    c2->next = end_s;
    c2->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("val")),
        make_int_lit(arena, 0x02));
    auto* alt2 = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c2);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = alt1;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    auto* field_magic = make_prim_node(arena, pool, "magic",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, choice);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("pkt");
    begin_s->next = field_magic;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xAB, 0x02};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    auto* pkt = result.root->child(pool.intern("pkt"));
    ASSERT_NE(pkt, nullptr);
    EXPECT_EQ(pkt->child_count, 2);
    auto* magic_f = pkt->child(pool.intern("magic"));
    ASSERT_NE(magic_f, nullptr);
    EXPECT_EQ(magic_f->computed_value, 0xAB);
    auto* val = pkt->child(pool.intern("val"));
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->computed_value, 0x02);
}

// ── Array Tests ───────────────────────────────────────────

// Helper: wire up a basic array of primitives
// Returns the BeginArrayNode entry; end wires to `after`.
// body: MatchPrimitive → ArrayNext (loop)
static BeginArrayNode* make_prim_array(
    ParseArena& arena, StringPool& pool,
    const char* array_name, const char* elem_name,
    PrimitiveInfo prim, ArrayMode mode,
    CompiledExpr* count_expr, CompiledExpr* until_expr,
    KontNode* after)
{
    auto* end_arr = arena.alloc<EndArrayNode>();
    end_arr->next = after;

    auto* arr_next = arena.alloc<ArrayNextNode>();
    arr_next->end_node = end_arr;
    arr_next->mode = mode;
    arr_next->count_expr = count_expr;
    arr_next->until_expr = until_expr;

    auto* elem = make_prim_node(arena, pool, elem_name, prim, arr_next);

    arr_next->next = elem;  // loop back (no separator)

    auto* begin_arr = arena.alloc<BeginArrayNode>();
    begin_arr->array_name = pool.intern(array_name);
    begin_arr->mode = mode;
    begin_arr->count_expr = count_expr;
    begin_arr->end_node = end_arr;
    begin_arr->next = elem;

    return begin_arr;
}

// --- CaptureBuilder array tests ---

TEST(CaptureBuilder, BeginEndArrayCapture) {
    CaptureBuilder builder;
    ParseArena arena;

    builder.begin_array("items", 0);
    builder.add_field("e0", 0, 1, CaptureType::UInt8, 0x10);
    builder.add_field("e1", 1, 2, CaptureType::UInt8, 0x20);
    builder.end_array(2);

    auto meta = builder.finish(arena, true, 2);
    ASSERT_NE(meta.root, nullptr);
    EXPECT_EQ(meta.root->child_count, 1);
    auto* arr = meta.root->child("items");
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->type, CaptureType::Array);
    EXPECT_EQ(arr->child_count, 2);
    EXPECT_EQ(arr->children[0].computed_value, 0x10);
    EXPECT_EQ(arr->children[1].computed_value, 0x20);
}

TEST(CaptureBuilder, ArrayInsideStructCapture) {
    CaptureBuilder builder;
    ParseArena arena;

    builder.begin_struct("outer", 0);
    builder.add_field("hdr", 0, 1, CaptureType::UInt8, 0xFF);
    builder.begin_array("data", 1);
    builder.add_field("e0", 1, 2, CaptureType::UInt8, 0x01);
    builder.add_field("e1", 2, 3, CaptureType::UInt8, 0x02);
    builder.end_array(3);
    builder.end_struct(3);

    auto meta = builder.finish(arena, true, 3);
    ASSERT_NE(meta.root, nullptr);
    auto* outer = meta.root->child("outer");
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->type, CaptureType::Struct);
    EXPECT_EQ(outer->child_count, 2);  // hdr + data array
    auto* data = outer->child("data");
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->type, CaptureType::Array);
    EXPECT_EQ(data->child_count, 2);
    EXPECT_EQ(data->children[0].computed_value, 0x01);
    EXPECT_EQ(data->children[1].computed_value, 0x02);
}

// --- Fixed-count array tests ---

TEST(CEKMachine, FixedCountBasic) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    auto* begin = make_prim_array(arena, pool, "items", "e", u8,
        ArrayMode::FixedCount, make_int_lit(arena, 3), nullptr, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x10, 0x20, 0x30};
    auto result = m.execute_from(begin, data, sizeof(data));

    EXPECT_TRUE(result.success);
    ASSERT_NE(result.root, nullptr);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->type, CaptureType::Array);
    EXPECT_EQ(arr->child_count, 3);
    EXPECT_EQ(arr->children[0].computed_value, 0x10);
    EXPECT_EQ(arr->children[1].computed_value, 0x20);
    EXPECT_EQ(arr->children[2].computed_value, 0x30);
    EXPECT_EQ(result.bytes_consumed, 3u);
}

TEST(CEKMachine, FixedCountEmpty) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    auto* begin = make_prim_array(arena, pool, "items", "e", u8,
        ArrayMode::FixedCount, make_int_lit(arena, 0), nullptr, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF};
    auto result = m.execute_from(begin, data, sizeof(data));

    EXPECT_TRUE(result.success);
    ASSERT_NE(result.root, nullptr);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->type, CaptureType::Array);
    EXPECT_EQ(arr->child_count, 0);
    EXPECT_EQ(result.bytes_consumed, 0u);
}

TEST(CEKMachine, FixedCountDynamic) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    // array<uint8>[count] where count is a previously-read field
    auto* count_ref = make_ref(arena, pool.intern("count"));
    auto* begin_arr = make_prim_array(arena, pool, "items", "e", u8,
        ArrayMode::FixedCount, count_ref, nullptr, halt);

    // count:u8 → begin_arr
    auto* read_count = make_prim_node(arena, pool, "count", u8, begin_arr);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x02, 0xAA, 0xBB};
    auto result = m.execute_from(read_count, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->child_count, 2);
    EXPECT_EQ(arr->children[0].computed_value, 0xAA);
    EXPECT_EQ(arr->children[1].computed_value, 0xBB);
    EXPECT_EQ(result.bytes_consumed, 3u);
}

TEST(CEKMachine, FixedCountBoundsFail) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    auto* begin = make_prim_array(arena, pool, "items", "e", u8,
        ArrayMode::FixedCount, make_int_lit(arena, 3), nullptr, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x10, 0x20};  // Only 2 bytes for 3-element array
    auto result = m.execute_from(begin, data, sizeof(data));

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_offset, 2u);  // Failed trying to read 3rd element
}

TEST(CEKMachine, FixedCountStructElements) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // array<struct{x:u8, y:u8}>[2]
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* end_arr = arena.alloc<EndArrayNode>();
    end_arr->next = halt;

    auto* arr_next = arena.alloc<ArrayNextNode>();
    arr_next->end_node = end_arr;
    arr_next->mode = ArrayMode::FixedCount;
    arr_next->count_expr = make_int_lit(arena, 2);

    // Element body: BeginStruct → x:u8 → y:u8 → EndStruct → ArrayNext
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = arr_next;
    auto* read_y = make_prim_node(arena, pool, "y", u8, end_s);
    auto* read_x = make_prim_node(arena, pool, "x", u8, read_y);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = nullptr;  // anonymous struct element
    begin_s->next = read_x;

    arr_next->next = begin_s;  // loop back to element body

    auto* begin_arr = arena.alloc<BeginArrayNode>();
    begin_arr->array_name = pool.intern("points");
    begin_arr->mode = ArrayMode::FixedCount;
    begin_arr->count_expr = make_int_lit(arena, 2);
    begin_arr->end_node = end_arr;
    begin_arr->next = begin_s;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    auto result = m.execute_from(begin_arr, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* arr = result.root->child(pool.intern("points"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->type, CaptureType::Array);
    EXPECT_EQ(arr->child_count, 2);
    // Each child is a Struct
    EXPECT_EQ(arr->children[0].type, CaptureType::Struct);
    EXPECT_EQ(arr->children[0].child_count, 2);
    auto* x0 = arr->children[0].child(pool.intern("x"));
    ASSERT_NE(x0, nullptr);
    EXPECT_EQ(x0->computed_value, 0x01);
    auto* y0 = arr->children[0].child(pool.intern("y"));
    ASSERT_NE(y0, nullptr);
    EXPECT_EQ(y0->computed_value, 0x02);
    EXPECT_EQ(arr->children[1].type, CaptureType::Struct);
    EXPECT_EQ(arr->children[1].child_count, 2);
    auto* x1 = arr->children[1].child(pool.intern("x"));
    ASSERT_NE(x1, nullptr);
    EXPECT_EQ(x1->computed_value, 0x03);
    auto* y1 = arr->children[1].child(pool.intern("y"));
    ASSERT_NE(y1, nullptr);
    EXPECT_EQ(y1->computed_value, 0x04);
    EXPECT_EQ(result.bytes_consumed, 4u);
}

// --- EOF/Count/Until array tests ---

TEST(CEKMachine, EofArrayBasic) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    auto* begin = make_prim_array(arena, pool, "items", "e", u8,
        ArrayMode::EOF_, nullptr, nullptr, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0A, 0x0B, 0x0C, 0x0D};
    auto result = m.execute_from(begin, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->child_count, 4);
    EXPECT_EQ(arr->children[0].computed_value, 0x0A);
    EXPECT_EQ(arr->children[1].computed_value, 0x0B);
    EXPECT_EQ(arr->children[2].computed_value, 0x0C);
    EXPECT_EQ(arr->children[3].computed_value, 0x0D);
    EXPECT_EQ(result.bytes_consumed, 4u);
}

TEST(CEKMachine, EofArrayEmpty) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    auto* begin = make_prim_array(arena, pool, "items", "e", u8,
        ArrayMode::EOF_, nullptr, nullptr, halt);

    CEKMachine m;
    m.arena = &arena;
    auto result = m.execute_from(begin, nullptr, 0);

    EXPECT_TRUE(result.success);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->child_count, 0);
}

TEST(CEKMachine, CountArray) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    // Count mode: re-evaluate count_expr each iteration
    auto* count_ref = make_ref(arena, pool.intern("n"));
    auto* begin = make_prim_array(arena, pool, "items", "e", u8,
        ArrayMode::Count, count_ref, nullptr, halt);

    // n:u8 → array
    auto* read_n = make_prim_node(arena, pool, "n", u8, begin);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x03, 0x11, 0x22, 0x33};
    auto result = m.execute_from(read_n, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->child_count, 3);
    EXPECT_EQ(arr->children[0].computed_value, 0x11);
    EXPECT_EQ(arr->children[1].computed_value, 0x22);
    EXPECT_EQ(arr->children[2].computed_value, 0x33);
    EXPECT_EQ(result.bytes_consumed, 4u);  // 1 (n) + 3 (elements)
}

TEST(CEKMachine, UntilArray) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    // Until mode: stop when remaining < 1
    auto* until_cond = make_binary(arena, ExprOp::Lt,
        make_remaining(arena), make_int_lit(arena, 1));
    auto* begin = make_prim_array(arena, pool, "items", "e", u8,
        ArrayMode::Until, nullptr, until_cond, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xAA, 0xBB, 0xCC};
    auto result = m.execute_from(begin, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->child_count, 3);
    EXPECT_EQ(arr->children[0].computed_value, 0xAA);
    EXPECT_EQ(arr->children[1].computed_value, 0xBB);
    EXPECT_EQ(arr->children[2].computed_value, 0xCC);
    EXPECT_EQ(result.bytes_consumed, 3u);
}

// --- Separator ---

TEST(CEKMachine, ArrayWithSeparator) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* end_arr = arena.alloc<EndArrayNode>();
    end_arr->next = halt;

    auto* arr_next = arena.alloc<ArrayNextNode>();
    arr_next->end_node = end_arr;
    arr_next->mode = ArrayMode::FixedCount;
    arr_next->count_expr = make_int_lit(arena, 3);

    // Element body: elem:u8 → ArrayNext
    auto* elem = make_prim_node(arena, pool, "e", u8, arr_next);

    // Separator: sep:u8 (constraint: sep == 0xFF) → elem_body
    auto* sep_constraint = arena.alloc<EvalConstraintNode>();
    sep_constraint->next = elem;
    sep_constraint->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("sep")),
        make_int_lit(arena, 0xFF));
    auto* sep = make_prim_node(arena, pool, "sep", u8, sep_constraint);

    arr_next->next = sep;  // After first element, go through separator

    auto* begin_arr = arena.alloc<BeginArrayNode>();
    begin_arr->array_name = pool.intern("items");
    begin_arr->mode = ArrayMode::FixedCount;
    begin_arr->count_expr = make_int_lit(arena, 3);
    begin_arr->end_node = end_arr;
    begin_arr->next = elem;  // First element has no preceding separator

    CEKMachine m;
    m.arena = &arena;
    // 0x01, 0xFF, 0x02, 0xFF, 0x03
    uint8_t data[] = {0x01, 0xFF, 0x02, 0xFF, 0x03};
    auto result = m.execute_from(begin_arr, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 5u);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    // Array captures: e(0x01), sep(0xFF), e(0x02), sep(0xFF), e(0x03)
    EXPECT_EQ(arr->child_count, 5);
    EXPECT_EQ(arr->children[0].computed_value, 0x01);  // e[0]
    EXPECT_EQ(arr->children[1].computed_value, 0xFF);  // sep
    EXPECT_EQ(arr->children[2].computed_value, 0x02);  // e[1]
    EXPECT_EQ(arr->children[3].computed_value, 0xFF);  // sep
    EXPECT_EQ(arr->children[4].computed_value, 0x03);  // e[2]
}

// --- LoopVar ---

TEST(CEKMachine, LoopVarEval) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // array<u8>[3] with a compute node that captures loop var i
    auto* end_arr = arena.alloc<EndArrayNode>();
    end_arr->next = halt;

    auto* arr_next = arena.alloc<ArrayNextNode>();
    arr_next->end_node = end_arr;
    arr_next->mode = ArrayMode::FixedCount;
    arr_next->count_expr = make_int_lit(arena, 3);

    // Element body: bind_compute("idx", LoopVar) → elem:u8 → ArrayNext
    auto* loop_var_expr = arena.alloc<CompiledExpr>();
    loop_var_expr->op = ExprOp::LoopVar;

    auto* bind_idx = arena.alloc<BindComputeNode>();
    bind_idx->field_name = pool.intern("idx");
    bind_idx->expr = loop_var_expr;

    auto* elem = make_prim_node(arena, pool, "e", u8, arr_next);
    bind_idx->next = elem;

    arr_next->next = bind_idx;  // loop back

    auto* begin_arr = arena.alloc<BeginArrayNode>();
    begin_arr->array_name = pool.intern("items");
    begin_arr->mode = ArrayMode::FixedCount;
    begin_arr->count_expr = make_int_lit(arena, 3);
    begin_arr->end_node = end_arr;
    begin_arr->next = bind_idx;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xAA, 0xBB, 0xCC};
    auto result = m.execute_from(begin_arr, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    // Each element produces 2 captures: idx (computed), e (uint8)
    // idx values should be 0, 1, 2
    EXPECT_EQ(arr->child_count, 6);  // 3 * (idx + e)
    // Verify names to ensure correct capture ordering
    EXPECT_EQ(arr->children[0].name, pool.intern("idx"));
    EXPECT_EQ(arr->children[1].name, pool.intern("e"));
    // idx values: 0, 1, 2
    EXPECT_EQ(arr->children[0].computed_value, 0);   // idx=0
    EXPECT_EQ(arr->children[2].computed_value, 1);   // idx=1
    EXPECT_EQ(arr->children[4].computed_value, 2);   // idx=2
    // data values: 0xAA, 0xBB, 0xCC
    EXPECT_EQ(arr->children[1].computed_value, 0xAA);
    EXPECT_EQ(arr->children[3].computed_value, 0xBB);
    EXPECT_EQ(arr->children[5].computed_value, 0xCC);
}

TEST(CEKMachine, LoopVarOutsideFails) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Just try to evaluate LoopVar with no LoopFrame
    auto* loop_var_expr = arena.alloc<CompiledExpr>();
    loop_var_expr->op = ExprOp::LoopVar;

    auto* bind = arena.alloc<BindComputeNode>();
    bind->field_name = pool.intern("idx");
    bind->expr = loop_var_expr;
    bind->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x00};
    auto result = m.execute_from(bind, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

// --- Integration ---

TEST(CEKMachine, ArrayInsideStruct) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // struct pkt { count:u8, items:array<u8>[count] }
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;

    auto* count_ref = make_ref(arena, pool.intern("count"));
    auto* arr = make_prim_array(arena, pool, "items", "e", u8,
        ArrayMode::FixedCount, count_ref, nullptr, end_s);

    auto* read_count = make_prim_node(arena, pool, "count", u8, arr);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("pkt");
    begin_s->next = read_count;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x02, 0xAA, 0xBB};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* pkt = result.root->child(pool.intern("pkt"));
    ASSERT_NE(pkt, nullptr);
    EXPECT_EQ(pkt->type, CaptureType::Struct);
    auto* count = pkt->child(pool.intern("count"));
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(count->computed_value, 2);
    auto* items = pkt->child(pool.intern("items"));
    ASSERT_NE(items, nullptr);
    EXPECT_EQ(items->type, CaptureType::Array);
    EXPECT_EQ(items->child_count, 2);
    EXPECT_EQ(items->children[0].computed_value, 0xAA);
    EXPECT_EQ(items->children[1].computed_value, 0xBB);
}

TEST(CEKMachine, ArrayOfStructs) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    PrimitiveInfo u16{PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned};

    // array<struct{id:u8, val:u16le}>[2]
    auto* end_arr = arena.alloc<EndArrayNode>();
    end_arr->next = halt;

    auto* arr_next = arena.alloc<ArrayNextNode>();
    arr_next->end_node = end_arr;
    arr_next->mode = ArrayMode::FixedCount;
    arr_next->count_expr = make_int_lit(arena, 2);

    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = arr_next;
    auto* read_val = make_prim_node(arena, pool, "val", u16, end_s);
    auto* read_id = make_prim_node(arena, pool, "id", u8, read_val);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = nullptr;
    begin_s->next = read_id;

    arr_next->next = begin_s;

    auto* begin_arr = arena.alloc<BeginArrayNode>();
    begin_arr->array_name = pool.intern("records");
    begin_arr->mode = ArrayMode::FixedCount;
    begin_arr->count_expr = make_int_lit(arena, 2);
    begin_arr->end_node = end_arr;
    begin_arr->next = begin_s;

    CEKMachine m;
    m.arena = &arena;
    // record 0: id=0x01, val=0x0302 (LE)
    // record 1: id=0x04, val=0x0605 (LE)
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    auto result = m.execute_from(begin_arr, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* arr2 = result.root->child(pool.intern("records"));
    ASSERT_NE(arr2, nullptr);
    EXPECT_EQ(arr2->type, CaptureType::Array);
    EXPECT_EQ(arr2->child_count, 2);
    EXPECT_EQ(arr2->children[0].type, CaptureType::Struct);
    auto* id0 = arr2->children[0].child(pool.intern("id"));
    ASSERT_NE(id0, nullptr);
    EXPECT_EQ(id0->computed_value, 0x01);
    auto* val0 = arr2->children[0].child(pool.intern("val"));
    ASSERT_NE(val0, nullptr);
    EXPECT_EQ(val0->computed_value, 0x0302);
    // Second struct
    EXPECT_EQ(arr2->children[1].type, CaptureType::Struct);
    auto* id1 = arr2->children[1].child(pool.intern("id"));
    ASSERT_NE(id1, nullptr);
    EXPECT_EQ(id1->computed_value, 0x04);
    auto* val1 = arr2->children[1].child(pool.intern("val"));
    ASSERT_NE(val1, nullptr);
    EXPECT_EQ(val1->computed_value, 0x0605);
    EXPECT_EQ(result.bytes_consumed, 6u);
}

TEST(CEKMachine, EofArrayPartialTrailingElement) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // EOF array of u16 on 5 bytes: should produce 2 elements, not fail
    PrimitiveInfo u16{PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned};
    auto* begin = make_prim_array(arena, pool, "items", "e", u16,
        ArrayMode::EOF_, nullptr, nullptr, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};  // 5 bytes, 2.5 u16s
    auto result = m.execute_from(begin, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->child_count, 2);
    EXPECT_EQ(arr->children[0].computed_value, 0x0201);
    EXPECT_EQ(arr->children[1].computed_value, 0x0403);
    EXPECT_EQ(result.bytes_consumed, 4u);  // 4 bytes consumed, trailing byte left
}

TEST(CEKMachine, UntilArrayElementFailEndsGracefully) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Until array of u32, but only 3 bytes of input.
    // The until condition (remaining < 1) is false at start (remaining=3),
    // so it tries to read a u32, which fails (need 4 bytes).
    // With soft-fail, this should produce 0 elements, not a hard failure.
    PrimitiveInfo u32{PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned};
    auto* until_cond = make_binary(arena, ExprOp::Lt,
        make_remaining(arena), make_int_lit(arena, 1));
    auto* begin = make_prim_array(arena, pool, "items", "e", u32,
        ArrayMode::Until, nullptr, until_cond, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02, 0x03};
    auto result = m.execute_from(begin, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* arr = result.root->child(pool.intern("items"));
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->child_count, 0);
    EXPECT_EQ(result.bytes_consumed, 0u);
}

TEST(CEKMachine, FixedCountArrayStillHardFails) {
    // FixedCount arrays should NOT soft-fail — if the count says 3
    // and the 3rd element can't be read, that's a real error.
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u16{PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned};
    auto* begin = make_prim_array(arena, pool, "items", "e", u16,
        ArrayMode::FixedCount, make_int_lit(arena, 3), nullptr, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};  // 5 bytes, need 6 for 3 u16s
    auto result = m.execute_from(begin, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

TEST(CEKMachine, ArrayEnvDoesNotLeakToParent) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // struct { x:u8, items:array<u8>[2], y:u8 where y != x }
    // If array env leaks, 'x' would resolve to the last element's 'x' binding
    // (which doesn't exist here, but 'e' would shadow if named 'x').
    // Use same field name 'v' inside array and after to detect leak.
    //
    // v:u8(=0x01) → array<v:u8>[2](=0xAA,0xBB) → constraint(v == 0x01)
    // If env leaks, v resolves to 0xBB (last element), constraint fails.
    // If env is restored, v resolves to 0x01 (parent), constraint passes.

    auto* check_v = arena.alloc<EvalConstraintNode>();
    check_v->next = halt;
    check_v->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("v")),
        make_int_lit(arena, 0x01));

    auto* arr = make_prim_array(arena, pool, "items", "v", u8,
        ArrayMode::FixedCount, make_int_lit(arena, 2), nullptr, check_v);

    auto* read_v = make_prim_node(arena, pool, "v", u8, arr);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0xAA, 0xBB};
    auto result = m.execute_from(read_v, data, sizeof(data));

    // If env leaked, v==0xBB and constraint fails.
    // With proper restore, v==0x01 and constraint passes.
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
}

TEST(CEKMachine, BacktrackBestErrorTracking) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Both alts fail, so no commits needed
    // Alt 1: u8 → u8 → fails (reads 2 bytes before failing)
    auto* c1 = arena.alloc<EvalConstraintNode>();
    c1->next = halt;
    c1->constraint = make_bool_lit(arena, false);
    auto* alt1_b = make_prim_node(arena, pool, "b",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c1);
    auto* alt1_a = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, alt1_b);

    // Alt 2 (last): u8 → fails (reads 1 byte before failing)
    auto* c2 = arena.alloc<EvalConstraintNode>();
    c2->next = halt;
    c2->constraint = make_bool_lit(arena, false);
    auto* alt2_a = make_prim_node(arena, pool, "a",
        {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned}, c2);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = alt1_a;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2_a;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02, 0x03};
    auto result = m.execute_from(choice, data, sizeof(data));

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_offset, 2u);
}

// ══════════════════════════════════════════════════════════════
// Phase 8 — Switch Dispatch + Optional with Constraint
// ══════════════════════════════════════════════════════════════

// --- Helper: build a SwitchDispatchNode ---
static SwitchDispatchNode* make_switch(ParseArena& arena,
                                        CompiledExpr* disc,
                                        SwitchDispatchNode::Case* cases,
                                        int case_count,
                                        KontNode* default_target) {
    auto* sw = arena.alloc<SwitchDispatchNode>();
    sw->discriminator = disc;
    sw->cases = cases;
    sw->case_count = case_count;
    sw->default_target = default_target;
    return sw;
}

// --- Switch — Integer dispatch ---

TEST(CEKMachine, SwitchIntFirstCase) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // switch(disc) { case 1: read a:u8; case 2: read b:u8; }
    // disc = 1 → reads a
    auto* read_a = make_prim_node(arena, pool, "a", u8, halt);
    auto* read_b = make_prim_node(arena, pool, "b", u8, halt);

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(2 * sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {1, nullptr, read_a};
    cases[1] = {2, nullptr, read_b};

    auto* sw = make_switch(arena, make_int_lit(arena, 1), cases, 2, nullptr);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(sw, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    auto* a = result.root->child(pool.intern("a"));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->computed_value, 0x42);
    // 'b' should not be captured
    EXPECT_EQ(result.root->child(pool.intern("b")), nullptr);
}

TEST(CEKMachine, SwitchIntSecondCase) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* read_a = make_prim_node(arena, pool, "a", u8, halt);
    auto* read_b = make_prim_node(arena, pool, "b", u8, halt);

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(2 * sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {1, nullptr, read_a};
    cases[1] = {2, nullptr, read_b};

    auto* sw = make_switch(arena, make_int_lit(arena, 2), cases, 2, nullptr);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF};
    auto result = m.execute_from(sw, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* b = result.root->child(pool.intern("b"));
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->computed_value, 0xFF);
    EXPECT_EQ(result.root->child(pool.intern("a")), nullptr);
}

TEST(CEKMachine, SwitchIntDefault) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* read_a = make_prim_node(arena, pool, "a", u8, halt);
    auto* read_default = make_prim_node(arena, pool, "d", u8, halt);

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {1, nullptr, read_a};

    auto* sw = make_switch(arena, make_int_lit(arena, 99), cases, 1, read_default);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x77};
    auto result = m.execute_from(sw, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* d = result.root->child(pool.intern("d"));
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->computed_value, 0x77);
    EXPECT_EQ(result.root->child(pool.intern("a")), nullptr);
}

TEST(CEKMachine, SwitchIntNoDefaultFails) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* read_a = make_prim_node(arena, pool, "a", u8, halt);

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {1, nullptr, read_a};

    // disc=99, no default → should fail
    auto* sw = make_switch(arena, make_int_lit(arena, 99), cases, 1, nullptr);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(sw, data, sizeof(data));

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message, nullptr);
}

// --- Switch — String/ident dispatch ---

TEST(CEKMachine, SwitchStringCase) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* read_a = make_prim_node(arena, pool, "a", u8, halt);
    auto* read_b = make_prim_node(arena, pool, "b", u8, halt);

    // Interned string case values: pointer → int64_t
    const char* str_foo = pool.intern("foo");
    const char* str_bar = pool.intern("bar");

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(2 * sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {reinterpret_cast<int64_t>(str_foo), str_foo, read_a};
    cases[1] = {reinterpret_cast<int64_t>(str_bar), str_bar, read_b};

    // Discriminator evaluates to the interned pointer for "bar"
    auto* disc = make_int_lit(arena, reinterpret_cast<int64_t>(str_bar));
    auto* sw = make_switch(arena, disc, cases, 2, nullptr);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xCC};
    auto result = m.execute_from(sw, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* b = result.root->child(pool.intern("b"));
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->computed_value, 0xCC);
    EXPECT_EQ(result.root->child(pool.intern("a")), nullptr);
}

TEST(CEKMachine, SwitchIdentCase) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* read_x = make_prim_node(arena, pool, "x", u8, halt);
    auto* read_y = make_prim_node(arena, pool, "y", u8, halt);

    // Ident cases: TypeA, TypeB (interned)
    const char* id_a = pool.intern("TypeA");
    const char* id_b = pool.intern("TypeB");

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(2 * sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {reinterpret_cast<int64_t>(id_a), id_a, read_x};
    cases[1] = {reinterpret_cast<int64_t>(id_b), id_b, read_y};

    // Disc = TypeA
    auto* disc = make_int_lit(arena, reinterpret_cast<int64_t>(id_a));
    auto* sw = make_switch(arena, disc, cases, 2, nullptr);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xDD};
    auto result = m.execute_from(sw, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* x = result.root->child(pool.intern("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->computed_value, 0xDD);
    EXPECT_EQ(result.root->child(pool.intern("y")), nullptr);
}

// --- Switch — Integration ---

TEST(CEKMachine, SwitchInsideStruct) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // struct pkt { tag:u8, switch(tag) { case 1: a:u8; case 2: b:u8; } }
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;

    auto* read_a = make_prim_node(arena, pool, "a", u8, end_s);
    auto* read_b = make_prim_node(arena, pool, "b", u8, end_s);

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(2 * sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {1, nullptr, read_a};
    cases[1] = {2, nullptr, read_b};

    auto* sw = make_switch(arena, make_ref(arena, pool.intern("tag")),
                           cases, 2, nullptr);

    auto* read_tag = make_prim_node(arena, pool, "tag", u8, sw);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("pkt");
    begin_s->next = read_tag;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x02, 0xBB};  // tag=2 → case 2 → read b
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    auto* pkt = result.root->child(pool.intern("pkt"));
    ASSERT_NE(pkt, nullptr);
    auto* tag = pkt->child(pool.intern("tag"));
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(tag->computed_value, 2);
    auto* b = pkt->child(pool.intern("b"));
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->computed_value, 0xBB);
    EXPECT_EQ(pkt->child(pool.intern("a")), nullptr);
}

TEST(CEKMachine, SwitchCaseWithStruct) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    PrimitiveInfo u16{PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned};

    // switch(disc) { case 1: struct { x:u8, y:u8 }; case 2: z:u16le; }
    auto* read_z = make_prim_node(arena, pool, "z", u16, halt);

    auto* end_inner = arena.alloc<EndStructNode>();
    end_inner->next = halt;
    auto* read_y = make_prim_node(arena, pool, "y", u8, end_inner);
    auto* read_x = make_prim_node(arena, pool, "x", u8, read_y);
    auto* begin_inner = arena.alloc<BeginStructNode>();
    begin_inner->struct_name = pool.intern("pair");
    begin_inner->next = read_x;

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(2 * sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {1, nullptr, begin_inner};
    cases[1] = {2, nullptr, read_z};

    auto* sw = make_switch(arena, make_int_lit(arena, 1), cases, 2, nullptr);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xAA, 0xBB};
    auto result = m.execute_from(sw, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    auto* pair = result.root->child(pool.intern("pair"));
    ASSERT_NE(pair, nullptr);
    auto* x = pair->child(pool.intern("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->computed_value, 0xAA);
    auto* y = pair->child(pool.intern("y"));
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(y->computed_value, 0xBB);
}

TEST(CEKMachine, SwitchDynamicDiscriminator) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    PrimitiveInfo u16{PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned};

    // Read type_byte:u8, then switch(type_byte) { case 1: val8:u8; case 2: val16:u16; }
    auto* read_val8 = make_prim_node(arena, pool, "val8", u8, halt);
    auto* read_val16 = make_prim_node(arena, pool, "val16", u16, halt);

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(2 * sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {1, nullptr, read_val8};
    cases[1] = {2, nullptr, read_val16};

    auto* sw = make_switch(arena, make_ref(arena, pool.intern("type_byte")),
                           cases, 2, nullptr);
    auto* read_type = make_prim_node(arena, pool, "type_byte", u8, sw);

    CEKMachine m;
    m.arena = &arena;
    // type_byte=2 → read u16le (0x0302 = 770 LE)
    uint8_t data[] = {0x02, 0x02, 0x03};
    auto result = m.execute_from(read_type, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    auto* val16 = result.root->child(pool.intern("val16"));
    ASSERT_NE(val16, nullptr);
    EXPECT_EQ(val16->computed_value, 0x0302);
    EXPECT_EQ(result.root->child(pool.intern("val8")), nullptr);
}

// --- Optional with Constraint ---

TEST(CEKMachine, OptionalConstraintPresent) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // Read flags:u8, then optional(flags & 1): extra:u8
    // flags=0x01 → constraint true → extra parsed
    auto* read_extra = make_prim_node(arena, pool, "extra", u8, halt);

    auto* cond = arena.alloc<EvalConstraintNode>();
    cond->constraint = make_binary(arena, ExprOp::BitAnd,
        make_ref(arena, pool.intern("flags")),
        make_int_lit(arena, 1));
    cond->next = read_extra;
    cond->on_false = halt;  // Skip element on false

    auto* read_flags = make_prim_node(arena, pool, "flags", u8, cond);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0xEE};
    auto result = m.execute_from(read_flags, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    auto* extra = result.root->child(pool.intern("extra"));
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->computed_value, 0xEE);
}

TEST(CEKMachine, OptionalConstraintAbsent) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // flags=0x00 → constraint false → skip extra
    auto* read_extra = make_prim_node(arena, pool, "extra", u8, halt);

    auto* cond = arena.alloc<EvalConstraintNode>();
    cond->constraint = make_binary(arena, ExprOp::BitAnd,
        make_ref(arena, pool.intern("flags")),
        make_int_lit(arena, 1));
    cond->next = read_extra;
    cond->on_false = halt;

    auto* read_flags = make_prim_node(arena, pool, "flags", u8, cond);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x00, 0xEE};
    auto result = m.execute_from(read_flags, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);  // Only flags consumed
    EXPECT_EQ(result.root->child(pool.intern("extra")), nullptr);
}

TEST(CEKMachine, OptionalConstraintWithCapture) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // struct pkt { flags:u8, optional(flags & 1): extra:u8, tail:u8 }
    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;
    auto* read_tail = make_prim_node(arena, pool, "tail", u8, end_s);

    auto* read_extra = make_prim_node(arena, pool, "extra", u8, read_tail);

    auto* cond = arena.alloc<EvalConstraintNode>();
    cond->constraint = make_binary(arena, ExprOp::BitAnd,
        make_ref(arena, pool.intern("flags")),
        make_int_lit(arena, 1));
    cond->next = read_extra;
    cond->on_false = read_tail;  // Skip extra, continue to tail

    auto* read_flags = make_prim_node(arena, pool, "flags", u8, cond);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("pkt");
    begin_s->next = read_flags;

    // Case 1: flags=0x01 → extra present
    {
        CEKMachine m;
        m.arena = &arena;
        uint8_t data[] = {0x01, 0xAA, 0xBB};
        auto result = m.execute_from(begin_s, data, sizeof(data));

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.bytes_consumed, 3u);
        auto* pkt = result.root->child(pool.intern("pkt"));
        ASSERT_NE(pkt, nullptr);
        EXPECT_EQ(pkt->child_count, 3);  // flags, extra, tail
        auto* extra = pkt->child(pool.intern("extra"));
        ASSERT_NE(extra, nullptr);
        EXPECT_EQ(extra->computed_value, 0xAA);
        auto* tail = pkt->child(pool.intern("tail"));
        ASSERT_NE(tail, nullptr);
        EXPECT_EQ(tail->computed_value, 0xBB);
    }

    // Case 2: flags=0x00 → extra absent
    {
        CEKMachine m;
        m.arena = &arena;
        uint8_t data[] = {0x00, 0xCC};
        auto result = m.execute_from(begin_s, data, sizeof(data));

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.bytes_consumed, 2u);
        auto* pkt = result.root->child(pool.intern("pkt"));
        ASSERT_NE(pkt, nullptr);
        EXPECT_EQ(pkt->child_count, 2);  // flags, tail (no extra)
        EXPECT_EQ(pkt->child(pool.intern("extra")), nullptr);
        auto* tail = pkt->child(pool.intern("tail"));
        ASSERT_NE(tail, nullptr);
        EXPECT_EQ(tail->computed_value, 0xCC);
    }
}

// --- EvalConstraint backward compat ---

TEST(CEKMachine, EvalConstraintOnFalseNull) {
    // Verify that on_false==nullptr (default) still fails normally
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* cond = arena.alloc<EvalConstraintNode>();
    cond->constraint = make_bool_lit(arena, false);
    cond->next = halt;
    // on_false is default nullptr — should fail, not branch

    auto* read = make_prim_node(arena, pool, "v", u8, cond);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(read, data, sizeof(data));

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message, nullptr);
}

// --- Edge cases: eval failure + backtracking ---

TEST(CEKMachine, SwitchDiscriminatorEvalFails) {
    // Discriminator references undefined variable → eval fails → early return
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* read_a = make_prim_node(arena, pool, "a", u8, halt);

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {1, nullptr, read_a};

    // Reference "nonexistent" — not in environment
    auto* sw = make_switch(arena, make_ref(arena, pool.intern("nonexistent")),
                           cases, 1, read_a);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(sw, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

TEST(CEKMachine, SwitchNoMatchInsideChoiceBacktracks) {
    // Switch no-match (no default) inside a choice alternative should
    // backtrack to the next alternative, not hard-fail the whole parse.
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // Alt 1: switch(val) { case 99: a:u8 }  — val=1, no match, no default → fail
    auto* read_a = make_prim_node(arena, pool, "a", u8, halt);
    auto* cases1 = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases1[0] = {99, nullptr, read_a};
    auto* commit1 = arena.alloc<CommitChoiceNode>();
    commit1->next = halt;
    auto* sw1 = make_switch(arena, make_ref(arena, pool.intern("val")),
                            cases1, 1, nullptr);
    // Wire: read val → switch → (commit on the case target side? no, cases go to halt)
    // Actually: alt1 = read_val → switch. If switch fails, backtrack.
    // But we need val in the env for the switch. So alt1 reads val, then switches on it.
    auto* read_val1 = make_prim_node(arena, pool, "val", u8, sw1);

    // Alt 2 (last): just read b:u8 — always succeeds
    auto* read_b = make_prim_node(arena, pool, "b", u8, halt);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = read_val1;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = read_b;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01};  // val=1, switch case 99 misses, backtracks to alt2: b=0x01
    auto result = m.execute_from(choice, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* b = result.root->child(pool.intern("b"));
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->computed_value, 0x01);
}

TEST(CEKMachine, OptionalConstraintEvalFailsPropagates) {
    // When on_false is set but the constraint expression itself fails
    // (e.g., undefined ref), the failure should propagate — NOT take the
    // on_false branch. Eval failure != constraint-false.
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* read_extra = make_prim_node(arena, pool, "extra", u8, halt);

    auto* cond = arena.alloc<EvalConstraintNode>();
    // Reference "nonexistent" — not in environment → eval fails
    cond->constraint = make_ref(arena, pool.intern("nonexistent"));
    cond->next = read_extra;
    cond->on_false = halt;  // Should NOT be taken on eval failure

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(cond, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

// --- Integration: Switch after Optional ---

TEST(CEKMachine, SwitchAfterOptional) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // struct pkt {
    //   flags:u8,
    //   optional(flags & 0x80): extra:u8,
    //   type:u8,
    //   switch(type) { case 1: a:u8; case 2: b:u8; }
    // }

    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;

    auto* read_a = make_prim_node(arena, pool, "a", u8, end_s);
    auto* read_b = make_prim_node(arena, pool, "b", u8, end_s);

    auto* cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(2 * sizeof(SwitchDispatchNode::Case),
                       alignof(SwitchDispatchNode::Case)));
    cases[0] = {1, nullptr, read_a};
    cases[1] = {2, nullptr, read_b};

    auto* sw = make_switch(arena, make_ref(arena, pool.intern("type")),
                           cases, 2, nullptr);

    auto* read_type = make_prim_node(arena, pool, "type", u8, sw);

    auto* read_extra = make_prim_node(arena, pool, "extra", u8, read_type);

    auto* cond = arena.alloc<EvalConstraintNode>();
    cond->constraint = make_binary(arena, ExprOp::BitAnd,
        make_ref(arena, pool.intern("flags")),
        make_int_lit(arena, 0x80));
    cond->next = read_extra;
    cond->on_false = read_type;

    auto* read_flags = make_prim_node(arena, pool, "flags", u8, cond);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("pkt");
    begin_s->next = read_flags;

    // Test: flags=0x80 (extra present), extra=0x11, type=2, b=0x22
    {
        CEKMachine m;
        m.arena = &arena;
        uint8_t data[] = {0x80, 0x11, 0x02, 0x22};
        auto result = m.execute_from(begin_s, data, sizeof(data));

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.bytes_consumed, 4u);
        auto* pkt = result.root->child(pool.intern("pkt"));
        ASSERT_NE(pkt, nullptr);
        EXPECT_EQ(pkt->child(pool.intern("flags"))->computed_value, 0x80);
        auto* extra = pkt->child(pool.intern("extra"));
        ASSERT_NE(extra, nullptr);
        EXPECT_EQ(extra->computed_value, 0x11);
        EXPECT_EQ(pkt->child(pool.intern("type"))->computed_value, 2);
        auto* b = pkt->child(pool.intern("b"));
        ASSERT_NE(b, nullptr);
        EXPECT_EQ(b->computed_value, 0x22);
        EXPECT_EQ(pkt->child(pool.intern("a")), nullptr);
    }

    // Test: flags=0x00 (extra absent), type=1, a=0x33
    {
        CEKMachine m;
        m.arena = &arena;
        uint8_t data[] = {0x00, 0x01, 0x33};
        auto result = m.execute_from(begin_s, data, sizeof(data));

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.bytes_consumed, 3u);
        auto* pkt = result.root->child(pool.intern("pkt"));
        ASSERT_NE(pkt, nullptr);
        EXPECT_EQ(pkt->child(pool.intern("extra")), nullptr);
        EXPECT_EQ(pkt->child(pool.intern("type"))->computed_value, 1);
        auto* a = pkt->child(pool.intern("a"));
        ASSERT_NE(a, nullptr);
        EXPECT_EQ(a->computed_value, 0x33);
    }
}

// ── BitfieldReadNode ─────────────────────────────────────────

// Helper: build a BitfieldReadNode (entries flatten into enclosing scope)
static BitfieldReadNode* make_bitfield_node(ParseArena& arena, StringPool& pool,
                                             PrimitiveInfo container,
                                             std::initializer_list<std::pair<const char*, int>> entries_init,
                                             KontNode* next_node) {
    auto* node = arena.alloc<BitfieldReadNode>();
    node->container = container;
    node->entry_count = static_cast<int>(entries_init.size());
    node->entries = static_cast<BitfieldReadNode::Entry*>(
        arena.allocate(sizeof(BitfieldReadNode::Entry) * entries_init.size(),
                       alignof(BitfieldReadNode::Entry)));
    int idx = 0;
    for (auto& [n, w] : entries_init) {
        node->entries[idx].name = n ? pool.intern(n) : nullptr;
        node->entries[idx].width_bits = w;
        idx++;
    }
    node->next = next_node;
    return node;
}

TEST(CEKBitfield, BE_U8) {
    // u8 container, MSB-first, 2 entries (4+4 bits)
    // Input: 0xAB → version=0xA (high nibble), ihl=0xB (low nibble)
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8 = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Big};
    auto* bf = make_bitfield_node(arena, pool, u8,
        {{"version", 4}, {"ihl", 4}}, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xAB};
    auto result = m.execute_from(bf, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    // Entries are flat at root level
    EXPECT_EQ(result.root->child_count, 2);
    EXPECT_EQ(result.root->child(pool.intern("version"))->computed_value, 0xA);
    EXPECT_EQ(result.root->child(pool.intern("ihl"))->computed_value, 0xB);
}

TEST(CEKBitfield, BE_U16) {
    // u16be container, MSB-first, 4 entries (4+4+6+2 = 16 bits)
    // IP header first 16 bits: version:4, ihl:4, dscp:6, ecn:2
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u16be = {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Big};
    auto* bf = make_bitfield_node(arena, pool, u16be,
        {{"version", 4}, {"ihl", 4}, {"dscp", 6}, {"ecn", 2}}, halt);

    CEKMachine m;
    m.arena = &arena;
    // 0x45C0 = 0100 0101 1100 0000
    // MSB-first: version(4)=0100=4, ihl(4)=0101=5, dscp(6)=110000=48, ecn(2)=00=0
    uint8_t data[] = {0x45, 0xC0};
    auto result = m.execute_from(bf, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    EXPECT_EQ(result.root->child_count, 4);
    EXPECT_EQ(result.root->child(pool.intern("version"))->computed_value, 4);
    EXPECT_EQ(result.root->child(pool.intern("ihl"))->computed_value, 5);
    EXPECT_EQ(result.root->child(pool.intern("dscp"))->computed_value, 48);
    EXPECT_EQ(result.root->child(pool.intern("ecn"))->computed_value, 0);
}

TEST(CEKBitfield, BE_U32) {
    // u32be container, MSB-first
    // Input: 0xFF001234
    // Entries: a:8, b:8, c:16
    // MSB-first: a=0xFF, b=0x00, c=0x1234
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u32be = {PrimKind::Integer, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Big};
    auto* bf = make_bitfield_node(arena, pool, u32be,
        {{"a", 8}, {"b", 8}, {"c", 16}}, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF, 0x00, 0x12, 0x34};
    auto result = m.execute_from(bf, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    EXPECT_EQ(result.root->child(pool.intern("a"))->computed_value, 0xFF);
    EXPECT_EQ(result.root->child(pool.intern("b"))->computed_value, 0x00);
    EXPECT_EQ(result.root->child(pool.intern("c"))->computed_value, 0x1234);
}

TEST(CEKBitfield, LE_U16) {
    // u16le container, LSB-first, 3 entries (5+6+5 = 16 bits)
    // Input: 0x1F 0xF8 → as u16le = 0xF81F
    // 0xF81F = 1111 1000 0001 1111
    // LSB-first: a(5) = bits[0:4] = 11111 = 31
    //            b(6) = bits[5:10] = 000000 = 0
    //            c(5) = bits[11:15] = 11111 = 31
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u16le = {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Little};
    auto* bf = make_bitfield_node(arena, pool, u16le,
        {{"a", 5}, {"b", 6}, {"c", 5}}, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x1F, 0xF8};
    auto result = m.execute_from(bf, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    EXPECT_EQ(result.root->child(pool.intern("a"))->computed_value, 31);
    EXPECT_EQ(result.root->child(pool.intern("b"))->computed_value, 0);
    EXPECT_EQ(result.root->child(pool.intern("c"))->computed_value, 31);
}

TEST(CEKBitfield, LE_U8) {
    // u8 with explicit Little endian, LSB-first
    // Input: 0xB5 = 10110101
    // LSB-first: lo(4) = 0101 = 5, hi(4) = 1011 = 11
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8le = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Little};
    auto* bf = make_bitfield_node(arena, pool, u8le,
        {{"lo", 4}, {"hi", 4}}, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xB5};
    auto result = m.execute_from(bf, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.root->child(pool.intern("lo"))->computed_value, 5);
    EXPECT_EQ(result.root->child(pool.intern("hi"))->computed_value, 11);
}

TEST(CEKBitfield, TruncatedInput) {
    // Input shorter than container → fail
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u16be = {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Big};
    auto* bf = make_bitfield_node(arena, pool, u16be,
        {{"a", 8}, {"b", 8}}, halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42};  // Only 1 byte for a u16 container
    auto result = m.execute_from(bf, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

TEST(CEKBitfield, EntryInConstraint) {
    // Extract a bitfield entry, then use it in a where-clause constraint
    // bitfield<u8 BE> { version:4, ihl:4 } → where(version == 4)
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* constraint = arena.alloc<EvalConstraintNode>();
    constraint->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("version")),
        make_int_lit(arena, 4));
    constraint->next = halt;

    PrimitiveInfo u8be = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Big};
    auto* bf = make_bitfield_node(arena, pool, u8be,
        {{"version", 4}, {"ihl", 4}}, constraint);

    CEKMachine m;
    m.arena = &arena;
    // 0x45 = 0100 0101 → MSB-first: version=4, ihl=5
    uint8_t data[] = {0x45};
    auto result = m.execute_from(bf, data, sizeof(data));
    EXPECT_TRUE(result.success);

    // Now with version != 4 → constraint fails
    uint8_t data2[] = {0x35};  // version=3, ihl=5
    auto result2 = m.execute_from(bf, data2, sizeof(data2));
    EXPECT_FALSE(result2.success);
}

TEST(CEKBitfield, CaptureStructure) {
    // Verify entries are flat Computed fields at root level
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8be = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Big};
    auto* bf = make_bitfield_node(arena, pool, u8be,
        {{"x", 3}, {"y", 5}}, halt);

    CEKMachine m;
    m.arena = &arena;
    // 0xE7 = 11100111 → MSB-first: x(3)=111=7, y(5)=00111=7
    uint8_t data[] = {0xE7};
    auto result = m.execute_from(bf, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.root->child_count, 2);

    auto* x = result.root->child(pool.intern("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, CaptureType::Computed);
    EXPECT_EQ(x->start_offset, 0u);
    EXPECT_EQ(x->end_offset, 1u);
    EXPECT_EQ(x->computed_value, 7);

    auto* y = result.root->child(pool.intern("y"));
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(y->type, CaptureType::Computed);
    EXPECT_EQ(y->computed_value, 7);
}

TEST(CEKBitfield, InsideStruct) {
    // Bitfield entries flatten into the enclosing struct
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;

    PrimitiveInfo u8be = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Big};
    auto* bf = make_bitfield_node(arena, pool, u8be,
        {{"a", 4}, {"b", 4}}, end_s);

    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("pkt");
    begin_s->next = bf;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x93};  // MSB-first: a=9, b=3
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* pkt = result.root->child(pool.intern("pkt"));
    ASSERT_NE(pkt, nullptr);
    EXPECT_EQ(pkt->type, CaptureType::Struct);
    // Entries are direct children of the struct
    EXPECT_EQ(pkt->child(pool.intern("a"))->computed_value, 9);
    EXPECT_EQ(pkt->child(pool.intern("b"))->computed_value, 3);
}

TEST(CEKBitfield, NativeEndianUsesRuntime) {
    // Container with PrimEndian::Native — derives MSB/LSB from machine state
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8native = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Native};
    auto* bf = make_bitfield_node(arena, pool, u8native,
        {{"hi", 4}, {"lo", 4}}, halt);

    // 0xAB = 10101011
    uint8_t data[] = {0xAB};

    // Machine in BE mode (little_endian=false) → MSB-first → hi=0xA, lo=0xB
    {
        CEKMachine m;
        m.arena = &arena;
        auto result = m.execute_from(bf, data, sizeof(data), false);
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.root->child(pool.intern("hi"))->computed_value, 0xA);
        EXPECT_EQ(result.root->child(pool.intern("lo"))->computed_value, 0xB);
    }

    // Machine in LE mode (little_endian=true) → LSB-first
    // LSB-first: hi(4)=bits[0:3]=1011=11, lo(4)=bits[4:7]=1010=10
    {
        CEKMachine m;
        m.arena = &arena;
        auto result = m.execute_from(bf, data, sizeof(data), true);
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.root->child(pool.intern("hi"))->computed_value, 11);
        EXPECT_EQ(result.root->child(pool.intern("lo"))->computed_value, 10);
    }
}

// ── SetEndianNode ─────────────────────────────────────────────

TEST(CEKSetEndian, ToLittle) {
    // Set LE, then read u16 native → LE byte order
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u16native = {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Native};
    auto* read = make_prim_node(arena, pool, "val", u16native, halt);

    auto* set_endian = arena.alloc<SetEndianNode>();
    set_endian->expr = make_int_lit(arena, 1);  // non-zero = little endian
    set_endian->next = read;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};
    // Start BE, switch to LE: 0x01,0x02 as LE u16 = 0x0201
    auto result = m.execute_from(set_endian, data, sizeof(data), false);

    EXPECT_TRUE(result.success);
    auto* val = result.root->child(pool.intern("val"));
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->computed_value, 0x0201);
}

TEST(CEKSetEndian, ToBig) {
    // Set BE, then read u16 native → BE byte order
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u16native = {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Native};
    auto* read = make_prim_node(arena, pool, "val", u16native, halt);

    auto* set_endian = arena.alloc<SetEndianNode>();
    set_endian->expr = make_int_lit(arena, 0);  // 0 = big endian
    set_endian->next = read;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};
    // Start LE, switch to BE: 0x01,0x02 as BE u16 = 0x0102
    auto result = m.execute_from(set_endian, data, sizeof(data), true);

    EXPECT_TRUE(result.success);
    auto* val = result.root->child(pool.intern("val"));
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->computed_value, 0x0102);
}

TEST(CEKSetEndian, EvalFails) {
    // Expression has undefined ref → parse fails
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* set_endian = arena.alloc<SetEndianNode>();
    set_endian->expr = make_ref(arena, pool.intern("nonexistent"));
    set_endian->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x00};
    auto result = m.execute_from(set_endian, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

TEST(CEKSetEndian, FromParsedField) {
    // Read byte_order:u8, set endian from its value, then read val:u16 native
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u16native = {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Native};
    auto* read_val = make_prim_node(arena, pool, "val", u16native, halt);

    auto* set_endian = arena.alloc<SetEndianNode>();
    set_endian->expr = make_ref(arena, pool.intern("byte_order"));
    set_endian->next = read_val;

    PrimitiveInfo u8 = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    auto* read_bo = make_prim_node(arena, pool, "byte_order", u8, set_endian);

    // byte_order=1 (LE), then u16: 0x01,0x02 → 0x0201
    {
        CEKMachine m;
        m.arena = &arena;
        uint8_t data[] = {0x01, 0x01, 0x02};
        auto result = m.execute_from(read_bo, data, sizeof(data), false);

        EXPECT_TRUE(result.success);
        auto* val = result.root->child(pool.intern("val"));
        ASSERT_NE(val, nullptr);
        EXPECT_EQ(val->computed_value, 0x0201);
    }

    // byte_order=0 (BE), then u16: 0x01,0x02 → 0x0102
    {
        CEKMachine m;
        m.arena = &arena;
        uint8_t data[] = {0x00, 0x01, 0x02};
        auto result = m.execute_from(read_bo, data, sizeof(data), false);

        EXPECT_TRUE(result.success);
        auto* val = result.root->child(pool.intern("val"));
        ASSERT_NE(val, nullptr);
        EXPECT_EQ(val->computed_value, 0x0102);
    }
}

TEST(CEKSetEndian, AffectsBitfield) {
    // Set endian, then bitfield with Native container → bit ordering follows runtime
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    PrimitiveInfo u8native = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Native};
    auto* bf = make_bitfield_node(arena, pool, u8native,
        {{"hi", 4}, {"lo", 4}}, halt);

    // 0xAB = 10101011
    uint8_t data[] = {0xAB};

    // Set to BE → MSB-first: hi=0xA, lo=0xB
    {
        auto* set_be = arena.alloc<SetEndianNode>();
        set_be->expr = make_int_lit(arena, 0);
        set_be->next = bf;

        CEKMachine m;
        m.arena = &arena;
        auto result = m.execute_from(set_be, data, sizeof(data), true);  // start LE

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.root->child(pool.intern("hi"))->computed_value, 0xA);
        EXPECT_EQ(result.root->child(pool.intern("lo"))->computed_value, 0xB);
    }

    // Set to LE → LSB-first: hi=11, lo=10
    {
        auto* set_le = arena.alloc<SetEndianNode>();
        set_le->expr = make_int_lit(arena, 1);
        set_le->next = bf;

        CEKMachine m;
        m.arena = &arena;
        auto result = m.execute_from(set_le, data, sizeof(data), false);  // start BE

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.root->child(pool.intern("hi"))->computed_value, 11);
        EXPECT_EQ(result.root->child(pool.intern("lo"))->computed_value, 10);
    }
}

// ── Integration: Bitfield + Switch ───────────────────────────

TEST(CEKBitfield, ThenSwitch) {
    // Extract a type field from a bitfield, then switch on it
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Switch cases: type==1 → read a:u8, type==2 → read b:u16be
    PrimitiveInfo u8 = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    auto* case1_read = make_prim_node(arena, pool, "a", u8, halt);

    PrimitiveInfo u16be = {PrimKind::Integer, PrimWidth::W16, PrimSign::Unsigned, PrimEndian::Big};
    auto* case2_read = make_prim_node(arena, pool, "b", u16be, halt);

    auto* sw = arena.alloc<SwitchDispatchNode>();
    sw->discriminator = make_ref(arena, pool.intern("type_field"));
    sw->case_count = 2;
    sw->cases = static_cast<SwitchDispatchNode::Case*>(
        arena.allocate(sizeof(SwitchDispatchNode::Case) * 2,
                       alignof(SwitchDispatchNode::Case)));
    sw->cases[0] = {1, nullptr, case1_read};
    sw->cases[1] = {2, nullptr, case2_read};
    sw->default_target = nullptr;

    // Bitfield: u8 BE, type_field:4, padding:4
    PrimitiveInfo u8be = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Big};
    auto* bf = make_bitfield_node(arena, pool, u8be,
        {{"type_field", 4}, {"padding", 4}}, sw);

    // type_field=1, padding=0, then a=0x42
    {
        CEKMachine m;
        m.arena = &arena;
        uint8_t data[] = {0x10, 0x42};  // 0x10 = type_field=1, padding=0
        auto result = m.execute_from(bf, data, sizeof(data));
        EXPECT_TRUE(result.success);
        auto* a = result.root->child(pool.intern("a"));
        ASSERT_NE(a, nullptr);
        EXPECT_EQ(a->computed_value, 0x42);
    }

    // type_field=2, padding=0, then b=0x1234
    {
        CEKMachine m;
        m.arena = &arena;
        uint8_t data[] = {0x20, 0x12, 0x34};  // 0x20 = type_field=2, padding=0
        auto result = m.execute_from(bf, data, sizeof(data));
        EXPECT_TRUE(result.success);
        auto* b = result.root->child(pool.intern("b"));
        ASSERT_NE(b, nullptr);
        EXPECT_EQ(b->computed_value, 0x1234);
    }
}

TEST(CEKBitfield, InsideArray) {
    // Array of bitfields — entries flatten into the array.
    // 3 elements × 2 entries each = 6 flat children.
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* end_arr = arena.alloc<EndArrayNode>();
    end_arr->next = halt;

    PrimitiveInfo u8be = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Big};
    auto* bf = make_bitfield_node(arena, pool, u8be,
        {{"hi", 4}, {"lo", 4}}, nullptr);  // next set to arr_next below

    auto* arr_next = arena.alloc<ArrayNextNode>();
    arr_next->mode = ArrayMode::FixedCount;
    arr_next->end_node = end_arr;
    arr_next->next = bf;  // loop back to body
    bf->next = arr_next;

    auto* begin_arr = arena.alloc<BeginArrayNode>();
    begin_arr->array_name = pool.intern("items");
    begin_arr->mode = ArrayMode::FixedCount;
    begin_arr->count_expr = make_int_lit(arena, 3);
    begin_arr->end_node = end_arr;
    begin_arr->next = bf;

    CEKMachine m;
    m.arena = &arena;
    // 3 bytes: 0xAB, 0xCD, 0xEF → each splits into hi/lo nibbles
    uint8_t data[] = {0xAB, 0xCD, 0xEF};
    auto result = m.execute_from(begin_arr, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    auto* items = result.root->child(pool.intern("items"));
    ASSERT_NE(items, nullptr);
    EXPECT_EQ(items->type, CaptureType::Array);
    // 3 elements × 2 entries = 6 flat children
    EXPECT_EQ(items->child_count, 6);

    // Check by index: hi, lo, hi, lo, hi, lo
    EXPECT_EQ(items->children[0].computed_value, 0xA);  // elem 0 hi
    EXPECT_EQ(items->children[1].computed_value, 0xB);  // elem 0 lo
    EXPECT_EQ(items->children[2].computed_value, 0xC);  // elem 1 hi
    EXPECT_EQ(items->children[3].computed_value, 0xD);  // elem 1 lo
    EXPECT_EQ(items->children[4].computed_value, 0xE);  // elem 2 hi
    EXPECT_EQ(items->children[5].computed_value, 0xF);  // elem 2 lo
}

// ── ExternalCallNode ─────────────────────────────────────────

// Test external parsers: consume N bytes and return success
static bool ext_consume_4(const uint8_t* /*data*/, size_t length,
                           size_t* consumed, ParseArena* /*arena*/,
                           void* /*user_data*/) {
    if (length < 4) return false;
    *consumed = 4;
    return true;
}

static bool ext_always_fail(const uint8_t* /*data*/, size_t /*length*/,
                             size_t* /*consumed*/, ParseArena* /*arena*/,
                             void* /*user_data*/) {
    return false;
}

static bool ext_consume_zero(const uint8_t* /*data*/, size_t /*length*/,
                              size_t* consumed, ParseArena* /*arena*/,
                              void* /*user_data*/) {
    *consumed = 0;
    return true;
}

static bool ext_with_user_data(const uint8_t* /*data*/, size_t length,
                                size_t* consumed, ParseArena* /*arena*/,
                                void* user_data) {
    // User data is a pointer to the expected consume count
    int n = *static_cast<int*>(user_data);
    if (length < static_cast<size_t>(n)) return false;
    *consumed = static_cast<size_t>(n);
    return true;
}

TEST(CEKExternalCall, Success) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* ext = arena.alloc<ExternalCallNode>();
    ext->func_name = pool.intern("decompress");
    ext->field_name = pool.intern("payload");
    ext->next = halt;

    ExternalParserTable::Entry entries[] = {
        {pool.intern("decompress"), ext_consume_4, nullptr}
    };
    ExternalParserTable table;
    table.entries = entries;
    table.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.ext_parsers = &table;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto result = m.execute_from(ext, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    auto* payload = result.root->child(pool.intern("payload"));
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(payload->type, CaptureType::External);
    EXPECT_EQ(payload->start_offset, 0u);
    EXPECT_EQ(payload->end_offset, 4u);
}

TEST(CEKExternalCall, NotRegistered) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* ext = arena.alloc<ExternalCallNode>();
    ext->func_name = pool.intern("unknown_parser");
    ext->field_name = pool.intern("data");
    ext->next = halt;

    ExternalParserTable table;
    table.entries = nullptr;
    table.count = 0;

    CEKMachine m;
    m.arena = &arena;
    m.ext_parsers = &table;
    uint8_t data[] = {0x00};
    auto result = m.execute_from(ext, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

TEST(CEKExternalCall, NoTable) {
    // ext_parsers is nullptr → fail
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* ext = arena.alloc<ExternalCallNode>();
    ext->func_name = pool.intern("decompress");
    ext->field_name = pool.intern("data");
    ext->next = halt;

    CEKMachine m;
    m.arena = &arena;
    // m.ext_parsers left as nullptr
    uint8_t data[] = {0x00};
    auto result = m.execute_from(ext, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

TEST(CEKExternalCall, ParserFails) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* ext = arena.alloc<ExternalCallNode>();
    ext->func_name = pool.intern("bad_parser");
    ext->field_name = pool.intern("data");
    ext->next = halt;

    ExternalParserTable::Entry entries[] = {
        {pool.intern("bad_parser"), ext_always_fail, nullptr}
    };
    ExternalParserTable table;
    table.entries = entries;
    table.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.ext_parsers = &table;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    auto result = m.execute_from(ext, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

TEST(CEKExternalCall, ZeroConsume) {
    // External parser succeeds but consumes 0 bytes (valid)
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* ext = arena.alloc<ExternalCallNode>();
    ext->func_name = pool.intern("noop");
    ext->field_name = pool.intern("marker");
    ext->next = halt;

    ExternalParserTable::Entry entries[] = {
        {pool.intern("noop"), ext_consume_zero, nullptr}
    };
    ExternalParserTable table;
    table.entries = entries;
    table.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.ext_parsers = &table;
    uint8_t data[] = {0x00};
    auto result = m.execute_from(ext, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 0u);
    auto* marker = result.root->child(pool.intern("marker"));
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->start_offset, 0u);
    EXPECT_EQ(marker->end_offset, 0u);
}

TEST(CEKExternalCall, InStruct) {
    // External call inside a struct — capture nests correctly
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = halt;

    auto* ext = arena.alloc<ExternalCallNode>();
    ext->func_name = pool.intern("decompress");
    ext->field_name = pool.intern("payload");
    ext->next = end_s;

    PrimitiveInfo u8 = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    auto* read_hdr = make_prim_node(arena, pool, "header", u8, ext);

    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("pkt");
    begin_s->next = read_hdr;

    ExternalParserTable::Entry entries[] = {
        {pool.intern("decompress"), ext_consume_4, nullptr}
    };
    ExternalParserTable table;
    table.entries = entries;
    table.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.ext_parsers = &table;
    uint8_t data[] = {0xFF, 0x01, 0x02, 0x03, 0x04};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 5u);
    auto* pkt = result.root->child(pool.intern("pkt"));
    ASSERT_NE(pkt, nullptr);
    EXPECT_EQ(pkt->child(pool.intern("header"))->computed_value, 0xFF);
    auto* payload = pkt->child(pool.intern("payload"));
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(payload->type, CaptureType::External);
    EXPECT_EQ(payload->start_offset, 1u);
    EXPECT_EQ(payload->end_offset, 5u);
}

TEST(CEKExternalCall, Backtrack) {
    // In a choice, external call fails → backtrack to alternative
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    // Alt 1: ext_always_fail → halt (will fail)
    auto* commit = arena.alloc<CommitChoiceNode>();
    commit->next = halt;

    auto* ext = arena.alloc<ExternalCallNode>();
    ext->func_name = pool.intern("bad_parser");
    ext->field_name = pool.intern("data");
    ext->next = commit;

    // Alt 2: read val:u8 → halt (will succeed)
    PrimitiveInfo u8 = {PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};
    auto* alt2 = make_prim_node(arena, pool, "val", u8, halt);

    auto* choice = arena.alloc<BeginChoiceNode>();
    choice->next = ext;
    auto** remaining = static_cast<KontNode**>(
        arena.allocate(sizeof(KontNode*), alignof(KontNode*)));
    remaining[0] = alt2;
    choice->alternatives = remaining;
    choice->alt_count = 1;

    ExternalParserTable::Entry entries[] = {
        {pool.intern("bad_parser"), ext_always_fail, nullptr}
    };
    ExternalParserTable table;
    table.entries = entries;
    table.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.ext_parsers = &table;
    uint8_t data[] = {0x42};
    auto result = m.execute_from(choice, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* val = result.root->child(pool.intern("val"));
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->computed_value, 0x42);
}

TEST(CEKExternalCall, WithConstraint) {
    // ExternalCall → EvalConstraint → Halt
    // The external call binds field_name in env; constraint references it.
    // (External binds value=0, so constraint checks == 0.)
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* constraint = arena.alloc<EvalConstraintNode>();
    constraint->constraint = make_binary(arena, ExprOp::Eq,
        make_ref(arena, pool.intern("payload")),
        make_int_lit(arena, 0));
    constraint->next = halt;

    auto* ext = arena.alloc<ExternalCallNode>();
    ext->func_name = pool.intern("decompress");
    ext->field_name = pool.intern("payload");
    ext->next = constraint;

    ExternalParserTable::Entry entries[] = {
        {pool.intern("decompress"), ext_consume_4, nullptr}
    };
    ExternalParserTable table;
    table.entries = entries;
    table.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.ext_parsers = &table;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    auto result = m.execute_from(ext, data, sizeof(data));

    EXPECT_TRUE(result.success);
}

TEST(CEKExternalCall, UserData) {
    // Verify user_data is passed through to the external parser
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* ext = arena.alloc<ExternalCallNode>();
    ext->func_name = pool.intern("dynamic");
    ext->field_name = pool.intern("chunk");
    ext->next = halt;

    int consume_count = 3;
    ExternalParserTable::Entry entries[] = {
        {pool.intern("dynamic"), ext_with_user_data, &consume_count}
    };
    ExternalParserTable table;
    table.entries = entries;
    table.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.ext_parsers = &table;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto result = m.execute_from(ext, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    auto* chunk = result.root->child(pool.intern("chunk"));
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->end_offset, 3u);
}

// ── FunctionTable ────────────────────────────────────────────

// Helper: build a Call expression
static CompiledExpr* make_call(ParseArena& arena, const char* func_name,
                                std::initializer_list<CompiledExpr*> args) {
    auto* e = arena.alloc<CompiledExpr>();
    e->op = ExprOp::Call;
    e->call.func_name = func_name;
    e->call.arg_count = static_cast<int>(args.size());
    e->call.args = static_cast<CompiledExpr**>(
        arena.allocate(sizeof(CompiledExpr*) * args.size(),
                       alignof(CompiledExpr*)));
    int i = 0;
    for (auto* a : args) e->call.args[i++] = a;
    return e;
}

static int64_t user_fn_double(const int64_t* args, int /*arg_count*/,
                               const CEKMachine* /*m*/) {
    return args[0] * 2;
}

static int64_t user_fn_sum3(const int64_t* args, int /*arg_count*/,
                              const CEKMachine* /*m*/) {
    return args[0] + args[1] + args[2];
}

TEST(CEKFunctionTable, UserFunctionInExpr) {
    // Register "double", use in a compute expression
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* compute = arena.alloc<BindComputeNode>();
    compute->field_name = pool.intern("result");
    compute->expr = make_call(arena, pool.intern("double"),
        {make_int_lit(arena, 21)});
    compute->next = halt;

    FunctionTable::Entry func_entries[] = {
        {pool.intern("double"), user_fn_double}
    };
    FunctionTable ftable;
    ftable.entries = func_entries;
    ftable.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.func_table = &ftable;
    uint8_t data[] = {0x00};
    auto result = m.execute_from(compute, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* res = result.root->child(pool.intern("result"));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->computed_value, 42);
}

TEST(CEKFunctionTable, MultipleArgs) {
    // Register "sum3" with 3 args
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* compute = arena.alloc<BindComputeNode>();
    compute->field_name = pool.intern("total");
    compute->expr = make_call(arena, pool.intern("sum3"),
        {make_int_lit(arena, 10), make_int_lit(arena, 20), make_int_lit(arena, 30)});
    compute->next = halt;

    FunctionTable::Entry func_entries[] = {
        {pool.intern("sum3"), user_fn_sum3}
    };
    FunctionTable ftable;
    ftable.entries = func_entries;
    ftable.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.func_table = &ftable;
    uint8_t data[] = {0x00};
    auto result = m.execute_from(compute, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* total = result.root->child(pool.intern("total"));
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->computed_value, 60);
}

TEST(CEKFunctionTable, BuiltinStillWorks) {
    // Built-in "abs" still works when func_table is set
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* compute = arena.alloc<BindComputeNode>();
    compute->field_name = pool.intern("val");
    compute->expr = make_call(arena, pool.intern("abs"),
        {make_int_lit(arena, -7)});
    compute->next = halt;

    FunctionTable::Entry func_entries[] = {
        {pool.intern("double"), user_fn_double}
    };
    FunctionTable ftable;
    ftable.entries = func_entries;
    ftable.count = 1;

    CEKMachine m;
    m.arena = &arena;
    m.func_table = &ftable;
    uint8_t data[] = {0x00};
    auto result = m.execute_from(compute, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* val = result.root->child(pool.intern("val"));
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->computed_value, 7);
}

TEST(CEKFunctionTable, UnknownFunctionFails) {
    // Unknown function with no table → still fails
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* compute = arena.alloc<BindComputeNode>();
    compute->field_name = pool.intern("val");
    compute->expr = make_call(arena, pool.intern("nonexistent"),
        {make_int_lit(arena, 1)});
    compute->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x00};
    auto result = m.execute_from(compute, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

// ── MatchBytesNode (hand-built) ─────────────────────────────

TEST(CEKMatchBytes, FixedLength) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* node = arena.alloc<MatchBytesNode>();
    node->field_name = pool.intern("data");
    node->length_expr = make_int_lit(arena, 4);
    node->is_string = false;
    node->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto result = m.execute_from(node, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    auto* d = result.root->child(pool.intern("data"));
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->type, CaptureType::Bytes);
    EXPECT_EQ(d->start_offset, 0u);
    EXPECT_EQ(d->end_offset, 4u);
}

TEST(CEKMatchBytes, DynamicLength) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    auto* bytes_node = arena.alloc<MatchBytesNode>();
    bytes_node->field_name = pool.intern("data");
    bytes_node->length_expr = make_ref(arena, pool.intern("n"));
    bytes_node->is_string = false;
    bytes_node->next = halt;

    auto* read_n = make_prim_node(arena, pool, "n", u8, bytes_node);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x03, 0xAA, 0xBB, 0xCC};
    auto result = m.execute_from(read_n, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    auto* d = result.root->child(pool.intern("data"));
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->start_offset, 1u);
    EXPECT_EQ(d->end_offset, 4u);
}

TEST(CEKMatchBytes, StringVariant) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* node = arena.alloc<MatchBytesNode>();
    node->field_name = pool.intern("text");
    node->length_expr = make_int_lit(arena, 3);
    node->is_string = true;
    node->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x41, 0x42, 0x43};  // "ABC"
    auto result = m.execute_from(node, data, sizeof(data));

    EXPECT_TRUE(result.success);
    auto* t = result.root->child(pool.intern("text"));
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->type, CaptureType::String);
}

TEST(CEKMatchBytes, BoundsCheckFail) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();

    auto* node = arena.alloc<MatchBytesNode>();
    node->field_name = pool.intern("data");
    node->length_expr = make_int_lit(arena, 10);
    node->is_string = false;
    node->next = halt;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x01, 0x02};
    auto result = m.execute_from(node, data, sizeof(data));

    EXPECT_FALSE(result.success);
}

// ── Float primitives ────────────────────────────────────────

TEST(CEKMachine, ReadFloat32LE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Float, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Little},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x00, 0x00, 0x80, 0x3F};  // 1.0f LE
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Float32LE);
    EXPECT_EQ(result.root->children[0].computed_value, 1);
}

TEST(CEKMachine, ReadFloat32BE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Float, PrimWidth::W32, PrimSign::Unsigned, PrimEndian::Big},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x3F, 0x80, 0x00, 0x00};  // 1.0f BE
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Float32BE);
    EXPECT_EQ(result.root->children[0].computed_value, 1);
}

TEST(CEKMachine, ReadFloat64LE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Float, PrimWidth::W64, PrimSign::Unsigned, PrimEndian::Little},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0x40};  // 42.0 LE
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 8u);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Float64LE);
    EXPECT_EQ(result.root->children[0].computed_value, 42);
}

TEST(CEKMachine, ReadFloat64BE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Float, PrimWidth::W64, PrimSign::Unsigned, PrimEndian::Big},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x40, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // 42.0 BE
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 8u);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Float64BE);
    EXPECT_EQ(result.root->children[0].computed_value, 42);
}

// ── Signed integer primitives ───────────────────────────────

TEST(CEKMachine, ReadInt16LE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W16, PrimSign::Signed, PrimEndian::Little},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFE, 0xFF};  // -2 LE
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Int16LE);
    EXPECT_EQ(result.root->children[0].computed_value, -2);
}

TEST(CEKMachine, ReadInt16BE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W16, PrimSign::Signed, PrimEndian::Big},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF, 0xFE};  // -2 BE
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Int16BE);
    EXPECT_EQ(result.root->children[0].computed_value, -2);
}

TEST(CEKMachine, ReadInt32LE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Signed, PrimEndian::Little},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFD, 0xFF, 0xFF, 0xFF};  // -3 LE
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Int32LE);
    EXPECT_EQ(result.root->children[0].computed_value, -3);
}

TEST(CEKMachine, ReadInt32BE) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    auto* entry = make_prim_node(arena, pool, "val",
        {PrimKind::Integer, PrimWidth::W32, PrimSign::Signed, PrimEndian::Big},
        halt);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0xFF, 0xFF, 0xFF, 0xFD};  // -3 BE
    auto result = m.execute_from(entry, data, sizeof(data));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Int32BE);
    EXPECT_EQ(result.root->children[0].computed_value, -3);
}

// ── Bitwise expression operators ────────────────────────────

TEST(CEKExpr, BitwiseOps) {
    ParseArena arena;
    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x00};
    m.input = data;
    m.input_length = 1;
    m.pos = 0;
    m.failed = false;

    // BitAnd: 0xFF & 0x0F = 0x0F
    EXPECT_EQ(eval(make_binary(arena, ExprOp::BitAnd,
        make_int_lit(arena, 0xFF), make_int_lit(arena, 0x0F)), &m), 0x0F);
    // BitOr: 0xF0 | 0x0F = 0xFF
    EXPECT_EQ(eval(make_binary(arena, ExprOp::BitOr,
        make_int_lit(arena, 0xF0), make_int_lit(arena, 0x0F)), &m), 0xFF);
    // BitXor: 0xFF ^ 0x0F = 0xF0
    EXPECT_EQ(eval(make_binary(arena, ExprOp::BitXor,
        make_int_lit(arena, 0xFF), make_int_lit(arena, 0x0F)), &m), 0xF0);
    // Shl: 1 << 4 = 16
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Shl,
        make_int_lit(arena, 1), make_int_lit(arena, 4)), &m), 16);
    // Shr: 16 >> 2 = 4
    EXPECT_EQ(eval(make_binary(arena, ExprOp::Shr,
        make_int_lit(arena, 16), make_int_lit(arena, 2)), &m), 4);
    // BitNot: ~0 = -1
    EXPECT_EQ(eval(make_unary(arena, ExprOp::BitNot,
        make_int_lit(arena, 0)), &m), -1);

    EXPECT_FALSE(m.failed);
}

// ── FieldAccess / IndexAccess (hand-built) ──────────────────

TEST(CEKExpr, FieldAccessEval) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // BeginStruct("hdr") → version:u8 → length:u8 → EndStruct
    //   → compute result = hdr.version → Halt
    auto* compute = arena.alloc<BindComputeNode>();
    compute->field_name = pool.intern("result");
    compute->expr = make_field_access(arena,
        make_ref(arena, pool.intern("hdr")),
        pool.intern("version"));
    compute->next = halt;

    auto* end_s = arena.alloc<EndStructNode>();
    end_s->next = compute;
    auto* read_len = make_prim_node(arena, pool, "length", u8, end_s);
    auto* read_ver = make_prim_node(arena, pool, "version", u8, read_len);
    auto* begin_s = arena.alloc<BeginStructNode>();
    begin_s->struct_name = pool.intern("hdr");
    begin_s->next = read_ver;

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x42, 0x10};
    auto result = m.execute_from(begin_s, data, sizeof(data));

    ASSERT_TRUE(result.success);
    auto* res = result.root->child(pool.intern("result"));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->computed_value, 0x42);
}

TEST(CEKExpr, IndexAccessEval) {
    ParseArena arena;
    StringPool pool;
    auto* halt = arena.alloc<HaltNode>();
    PrimitiveInfo u8{PrimKind::Integer, PrimWidth::W8, PrimSign::Unsigned};

    // array "items" of 3 u8, then compute second = items[1]
    auto* compute = arena.alloc<BindComputeNode>();
    compute->field_name = pool.intern("second");
    compute->expr = make_index_access(arena,
        make_ref(arena, pool.intern("items")),
        make_int_lit(arena, 1));
    compute->next = halt;

    auto* arr = make_prim_array(arena, pool, "items", nullptr, u8,
        ArrayMode::FixedCount, make_int_lit(arena, 3), nullptr, compute);

    CEKMachine m;
    m.arena = &arena;
    uint8_t data[] = {0x0A, 0x0B, 0x0C};
    auto result = m.execute_from(arr, data, sizeof(data));

    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    auto* second = result.root->child(pool.intern("second"));
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->computed_value, 0x0B);
}

// ═══════════════════════════════════════════════════════════════
// CEK Compiler end-to-end tests (Phase 11)
// ═══════════════════════════════════════════════════════════════

#include "Compiler.h"
#include "Parser.h"
#include "Sema.h"

static CaptureMetadata compile_and_run(const char* src, const std::string& rule_name,
                                        const uint8_t* data, size_t length) {
    // Parse
    auto* parser = new Parser();
    parser->init(src, static_cast<int>(strlen(src)));
    if (!parser->parse()) {
        CaptureMetadata meta{};
        meta.error_message = "parse failed";
        return meta;
    }

    // Sema
    bbqgen::ErrorReporter errors;
    bbqgen::Sema sema(errors);
    if (!sema.analyze(parser->ast)) {
        CaptureMetadata meta{};
        meta.error_message = "sema failed";
        return meta;
    }

    // Compile
    CEKCompiler compiler;
    auto* grammar = compiler.compile(parser->ast, sema);

    // Find rule entry
    KontNode* entry = grammar->lookup(rule_name);
    if (!entry) {
        CaptureMetadata meta{};
        meta.error_message = "rule not found";
        delete grammar;
        return meta;
    }

    // Static arenas keep captures alive after function returns.
    // Reset before each call to avoid unbounded growth.
    static ParseArena parse_arena;
    parse_arena.reset();

    CEKMachine m;
    m.arena = &parse_arena;
    return m.execute_from(entry, data, length, grammar->default_little_endian);
}

// Overload for convenience
static CaptureMetadata compile_and_run(const char* src, const std::string& rule_name,
                                        std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> data(bytes);
    return compile_and_run(src, rule_name, data.data(), data.size());
}

// Helper to set up external parser table
static CaptureMetadata compile_and_run_ext(const char* src, const std::string& rule_name,
                                            const uint8_t* data, size_t length,
                                            ExternalParserTable* ext) {
    auto* parser = new Parser();
    parser->init(src, static_cast<int>(strlen(src)));
    if (!parser->parse()) {
        CaptureMetadata meta{};
        meta.error_message = "parse failed";
        return meta;
    }

    bbqgen::ErrorReporter errors;
    bbqgen::Sema sema(errors);
    if (!sema.analyze(parser->ast)) {
        CaptureMetadata meta{};
        meta.error_message = "sema failed";
        return meta;
    }

    CEKCompiler compiler;
    auto* grammar = compiler.compile(parser->ast, sema);
    KontNode* entry = grammar->lookup(rule_name);
    if (!entry) {
        CaptureMetadata meta{};
        meta.error_message = "rule not found";
        delete grammar;
        return meta;
    }

    // Intern the function name in the grammar's pool so pointer equality works
    for (int i = 0; i < ext->count; i++) {
        ext->entries[i].name = grammar->strings.intern(ext->entries[i].name);
    }

    static ParseArena parse_arena;
    parse_arena.reset();

    CEKMachine m;
    m.arena = &parse_arena;
    m.ext_parsers = ext;
    return m.execute_from(entry, data, length, grammar->default_little_endian);
}

// --- Primitives ---

TEST(CEKCompiler, PrimitiveU8) {
    auto result = compile_and_run(
        "Pkt = struct { val: uint8 }",
        "Pkt", {0x42});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    ASSERT_GE(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0x42);
}

TEST(CEKCompiler, PrimitiveU16LE) {
    auto result = compile_and_run(
        "Pkt = struct { val: uint16le }",
        "Pkt", {0x34, 0x12});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_NE(result.root, nullptr);
    ASSERT_GE(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0x1234);
}

TEST(CEKCompiler, PrimitiveBool) {
    auto result = compile_and_run(
        "Pkt = struct { flag: bool }",
        "Pkt", {0x01});
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.root, nullptr);
    ASSERT_GE(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 1);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Bool);
}

// --- Struct ---

TEST(CEKCompiler, SimpleStruct) {
    auto result = compile_and_run(
        "Pkt = struct { a: uint8, b: uint8 }",
        "Pkt", {0x0A, 0x0B});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_GE(result.root->child_count, 2);
    EXPECT_EQ(result.root->children[0].computed_value, 0x0A);
    EXPECT_EQ(result.root->children[1].computed_value, 0x0B);
}

TEST(CEKCompiler, NestedStruct) {
    auto result = compile_and_run(
        "Inner = struct { x: uint8 }\n"
        "Outer = struct { hdr: Inner, tag: uint8 }",
        "Outer", {0xAA, 0xBB});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_GE(result.root->child_count, 2);
    // hdr is a struct (Inner invoked via InvokeRuleNode)
    EXPECT_EQ(result.root->children[0].type, CaptureType::Struct);
    ASSERT_GE(result.root->children[0].child_count, 1);
    EXPECT_EQ(result.root->children[0].children[0].computed_value, 0xAA);
    // tag
    EXPECT_EQ(result.root->children[1].computed_value, 0xBB);
}

// --- Compute + Constraint ---

TEST(CEKCompiler, Compute) {
    auto result = compile_and_run(
        "Pkt = struct { len: uint8, doubled: compute(len * 2 : uint8) }",
        "Pkt", {0x05});
    ASSERT_TRUE(result.success);
    ASSERT_GE(result.root->child_count, 2);
    EXPECT_EQ(result.root->children[0].computed_value, 5);
    EXPECT_EQ(result.root->children[1].computed_value, 10);
    EXPECT_EQ(result.root->children[1].type, CaptureType::Computed);
}

TEST(CEKCompiler, WherePass) {
    auto result = compile_and_run(
        "Pkt = struct { magic: uint8 where(magic == 0xAA) }",
        "Pkt", {0xAA});
    ASSERT_TRUE(result.success);
    ASSERT_GE(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0xAA);
}

TEST(CEKCompiler, WhereFail) {
    auto result = compile_and_run(
        "Pkt = struct { magic: uint8 where(magic == 0xAA) }",
        "Pkt", {0xBB});
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message, nullptr);
}

// --- Alternatives ---

TEST(CEKCompiler, AlternativesFirstMatch) {
    auto result = compile_and_run(
        "A = struct { x: uint8 } | struct { y: uint16le }",
        "A", {0x42});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    // First alt matched: inline struct with field x
    // Inline struct creates a Struct capture as a child of root
    auto& s = result.root->children[0];
    EXPECT_EQ(s.type, CaptureType::Struct);
    ASSERT_GE(s.child_count, 1);
    EXPECT_EQ(s.children[0].computed_value, 0x42);
}

TEST(CEKCompiler, AlternativesBacktrack) {
    // First alt fails (needs 4 bytes), second alt matches (needs 1 byte)
    auto result = compile_and_run(
        "A = struct { x: uint32le } | struct { y: uint8 }",
        "A", {0x42});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    // Second alt matched: inline struct with field y
    auto& s = result.root->children[0];
    EXPECT_EQ(s.type, CaptureType::Struct);
    ASSERT_GE(s.child_count, 1);
    EXPECT_EQ(s.children[0].computed_value, 0x42);
}

// --- Optional ---

TEST(CEKCompiler, OptionalPresent) {
    auto result = compile_and_run(
        "Pkt = struct { tag: uint8, opt: optional<uint8> }",
        "Pkt", {0x01, 0x42});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    // tag + optional value both captured
    ASSERT_GE(result.root->child_count, 2);
    EXPECT_EQ(result.root->children[0].computed_value, 0x01);
    EXPECT_EQ(result.root->children[1].computed_value, 0x42);
}

TEST(CEKCompiler, OptionalAbsent) {
    // optional fails gracefully on insufficient data
    auto result = compile_and_run(
        "Pkt = struct { tag: uint8, opt: optional<uint16le> }",
        "Pkt", {0x01});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    // Only tag captured; optional was absent
    EXPECT_EQ(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].computed_value, 0x01);
}

// --- Union ---

TEST(CEKCompiler, UnionFirstVariant) {
    auto result = compile_and_run(
        "Tiny = struct { val: uint8 }\n"
        "Wide = struct { val: uint32le }\n"
        "Msg = union {\n"
        "  tiny: Tiny,\n"
        "  wide: Wide\n"
        "}",
        "Msg", {0x42});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    // First variant matched (Small: struct with uint8)
    auto& s = result.root->children[0];
    EXPECT_EQ(s.type, CaptureType::Struct);
    ASSERT_GE(s.child_count, 1);
    EXPECT_EQ(s.children[0].computed_value, 0x42);
}

TEST(CEKCompiler, UnionBacktrack) {
    // First variant needs 4 bytes and fails; second needs 1 byte and succeeds
    auto result = compile_and_run(
        "Wide = struct { val: uint32le }\n"
        "Tiny = struct { val: uint8 }\n"
        "Msg = union {\n"
        "  wide: Wide,\n"
        "  tiny: Tiny\n"
        "}",
        "Msg", {0x42});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    ASSERT_NE(result.root, nullptr);
    auto& s = result.root->children[0];
    EXPECT_EQ(s.type, CaptureType::Struct);
    ASSERT_GE(s.child_count, 1);
    EXPECT_EQ(s.children[0].computed_value, 0x42);
}

// --- Switch ---

TEST(CEKCompiler, SwitchInt) {
    auto result = compile_and_run(
        "CaseA = struct { a: uint8 }\n"
        "CaseB = struct { b: uint16le }\n"
        "Pkt = struct {\n"
        "  type: uint8,\n"
        "  data: switch(type) {\n"
        "    1: CaseA;\n"
        "    2: CaseB;\n"
        "  }\n"
        "}",
        "Pkt", {0x01, 0xAA});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_GE(result.root->child_count, 2);
    EXPECT_EQ(result.root->children[0].computed_value, 0x01);  // type field
    // data field: switch dispatched to CaseA via InvokeRule
    auto& data = result.root->children[1];
    EXPECT_EQ(data.type, CaptureType::Struct);
    ASSERT_GE(data.child_count, 1);
    EXPECT_EQ(data.children[0].computed_value, 0xAA);
}

TEST(CEKCompiler, SwitchDefault) {
    auto result = compile_and_run(
        "CaseA = struct { a: uint8 }\n"
        "CaseB = struct { b: uint16le }\n"
        "Pkt = struct {\n"
        "  type: uint8,\n"
        "  data: switch(type) {\n"
        "    1: CaseA;\n"
        "    default: CaseB;\n"
        "  }\n"
        "}",
        "Pkt", {0xFF, 0x34, 0x12});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    ASSERT_GE(result.root->child_count, 2);
    EXPECT_EQ(result.root->children[0].computed_value, 0xFF);  // type field
    // data field: switch hit default → CaseB (struct with b: uint16le)
    EXPECT_EQ(result.root->children[1].type, CaptureType::Struct);
    ASSERT_GE(result.root->children[1].child_count, 1);
    EXPECT_EQ(result.root->children[1].children[0].computed_value, 0x1234);
}

// --- Array ---

TEST(CEKCompiler, ArrayFixed) {
    auto result = compile_and_run(
        "Pkt = struct { items: array<uint8>[3] }",
        "Pkt", {0x0A, 0x0B, 0x0C});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    // Array is a direct child of root (struct fields are flattened)
    ASSERT_GE(result.root->child_count, 1);
    auto& arr = result.root->children[0];
    EXPECT_EQ(arr.type, CaptureType::Array);
    EXPECT_EQ(arr.child_count, 3);
    EXPECT_EQ(arr.children[0].computed_value, 0x0A);
    EXPECT_EQ(arr.children[1].computed_value, 0x0B);
    EXPECT_EQ(arr.children[2].computed_value, 0x0C);
}

TEST(CEKCompiler, ArrayEOF) {
    auto result = compile_and_run(
        "Pkt = struct { items: array<uint8>(none, eof) }",
        "Pkt", {0x01, 0x02, 0x03, 0x04});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    ASSERT_GE(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Array);
    EXPECT_EQ(result.root->children[0].child_count, 4);
}

TEST(CEKCompiler, ArrayCount) {
    auto result = compile_and_run(
        "Pkt = struct { n: uint8, items: array<uint8>(none, count(n)) }",
        "Pkt", {0x02, 0xAA, 0xBB});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    ASSERT_GE(result.root->child_count, 2);
    EXPECT_EQ(result.root->children[0].computed_value, 0x02);  // n
    EXPECT_EQ(result.root->children[1].type, CaptureType::Array);
    EXPECT_EQ(result.root->children[1].child_count, 2);
}

TEST(CEKCompiler, ArrayUntil) {
    auto result = compile_and_run(
        "Pkt = struct { items: array<uint8>(none, until(i == 2)) }",
        "Pkt", {0x01, 0x02, 0x03, 0x04, 0x05});
    ASSERT_TRUE(result.success);
    // Until checks after each element; i starts at 0 and increments.
    // Element 0 parsed (i=0), ArrayNext bumps i to 1, until(1==2) false -> continue.
    // Element 1 parsed (i=1), ArrayNext bumps i to 2, until(2==2) true -> done.
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_GE(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Array);
    EXPECT_EQ(result.root->children[0].child_count, 2);
}

TEST(CEKCompiler, ArrayWithSep) {
    // array with uint8 separator, count of 3
    auto result = compile_and_run(
        "Sep = uint8\n"
        "Pkt = struct { items: array<uint8>(Sep, count(3)) }",
        "Pkt", {0x0A, 0xFF, 0x0B, 0xFF, 0x0C});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 5u);
    ASSERT_GE(result.root->child_count, 1);
    auto& arr = result.root->children[0];
    EXPECT_EQ(arr.type, CaptureType::Array);
    EXPECT_GE(arr.child_count, 3);
    // Element values at positions 0, 2, 4 (separators at 1, 3)
    EXPECT_EQ(arr.children[0].computed_value, 0x0A);
    EXPECT_EQ(arr.children[2].computed_value, 0x0B);
    EXPECT_EQ(arr.children[4].computed_value, 0x0C);
}

// --- Bitfield ---

TEST(CEKCompiler, Bitfield) {
    auto result = compile_and_run(
        "Pkt = struct { flags: bitfield<uint8> { a: 4, b: 4 } }",
        "Pkt", {0xAB});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 1u);
    // Bitfield entries flatten into the struct
    ASSERT_GE(result.root->child_count, 2);
    // uint8 = LE by default, so LSB-first: a = low 4 bits = 0xB, b = high 4 bits = 0xA
    EXPECT_EQ(result.root->children[0].computed_value, 0xB);
    EXPECT_EQ(result.root->children[1].computed_value, 0xA);
}

// --- Endian switch ---

TEST(CEKCompiler, EndianSwitch) {
    auto result = compile_and_run(
        "Pkt = struct { @endian: big, val: uint16 }",
        "Pkt", {0x12, 0x34});
    ASSERT_TRUE(result.success);
    // @endian big → uint16 (Native) reads as big-endian = 0x1234
    // Find the non-computed field (endian may produce a computed capture)
    for (int i = 0; i < result.root->child_count; i++) {
        if (result.root->children[i].type != CaptureType::Computed) {
            EXPECT_EQ(result.root->children[i].computed_value, 0x1234);
            break;
        }
    }
}

// --- Extern ---

TEST(CEKCompiler, Extern) {
    // Register an external parser that consumes 2 bytes
    auto ext_fn = [](const uint8_t*, size_t len, size_t* consumed,
                     ParseArena*, void*) -> bool {
        if (len < 2) return false;
        *consumed = 2;
        return true;
    };

    ExternalParserTable::Entry ext_entries[] = {
        {"my_parser", ext_fn, nullptr}
    };
    ExternalParserTable ext_table;
    ext_table.entries = ext_entries;
    ext_table.count = 1;

    uint8_t data[] = {0x01, 0x02, 0x03};
    auto result = compile_and_run_ext(
        "Pkt = struct { hdr: uint8, payload: extern(\"my_parser\", \"MyType\") }",
        "Pkt", data, sizeof(data), &ext_table);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 3u);
    ASSERT_GE(result.root->child_count, 2);
    EXPECT_EQ(result.root->children[0].computed_value, 0x01);  // hdr
    // External parser capture records [start, end) range
    EXPECT_EQ(result.root->children[1].type, CaptureType::External);
    EXPECT_EQ(result.root->children[1].start_offset, 1u);
    EXPECT_EQ(result.root->children[1].end_offset, 3u);
}

// --- Bytes/String ---

TEST(CEKCompiler, BytesLength) {
    auto result = compile_and_run(
        "Pkt = struct { data: bytes[4] }",
        "Pkt", {0x01, 0x02, 0x03, 0x04});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    ASSERT_GE(result.root->child_count, 1);
    EXPECT_EQ(result.root->children[0].type, CaptureType::Bytes);
    EXPECT_EQ(result.root->children[0].start_offset, 0u);
    EXPECT_EQ(result.root->children[0].end_offset, 4u);
}

TEST(CEKCompiler, StringLength) {
    auto result = compile_and_run(
        "Pkt = struct { n: uint8, name: string[n] }",
        "Pkt", {0x03, 0x41, 0x42, 0x43});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    ASSERT_GE(result.root->child_count, 2);
    EXPECT_EQ(result.root->children[0].computed_value, 0x03);  // n
    EXPECT_EQ(result.root->children[1].type, CaptureType::String);
    EXPECT_EQ(result.root->children[1].start_offset, 1u);
    EXPECT_EQ(result.root->children[1].end_offset, 4u);
}

// --- Complex expression ---

TEST(CEKCompiler, ComplexExpr) {
    // Test nested binary/ternary/call expression in compute
    auto result = compile_and_run(
        "Pkt = struct {\n"
        "  a: uint8,\n"
        "  b: uint8,\n"
        "  result: compute(a > b ? a - b : b - a : uint8)\n"
        "}",
        "Pkt", {0x0A, 0x03});
    ASSERT_TRUE(result.success);
    ASSERT_GE(result.root->child_count, 3);
    EXPECT_EQ(result.root->children[0].computed_value, 0x0A);
    EXPECT_EQ(result.root->children[1].computed_value, 0x03);
    EXPECT_EQ(result.root->children[2].computed_value, 7);  // 10 - 3
}

// --- Multi-rule ---

TEST(CEKCompiler, MultiRule) {
    auto result = compile_and_run(
        "Header = struct { magic: uint8 where(magic == 0xFF), len: uint8 }\n"
        "Body = struct { data: array<uint8>(none, eof) }\n"
        "Packet = struct { hdr: Header, body: Body }",
        "Packet", {0xFF, 0x02, 0xAA, 0xBB});
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_consumed, 4u);
    ASSERT_GE(result.root->child_count, 2);
    // hdr is a struct (Header rule invoked via InvokeRuleNode)
    auto& hdr = result.root->children[0];
    EXPECT_EQ(hdr.type, CaptureType::Struct);
    ASSERT_GE(hdr.child_count, 2);
    EXPECT_EQ(hdr.children[0].computed_value, 0xFF);  // magic
    EXPECT_EQ(hdr.children[1].computed_value, 0x02);  // len
    // body is a struct containing an array
    EXPECT_EQ(result.root->children[1].type, CaptureType::Struct);
}

// --- Dotted-ref FieldAccess/IndexAccess ---

TEST(CEKCompiler, DottedRefConstraint) {
    // hdr.version used in where constraint
    {
        auto result = compile_and_run(
            "Header = struct { version: uint8 }\n"
            "Pkt = struct { hdr: Header, tag: uint8 where(hdr.version == 1) }",
            "Pkt", {0x01, 0xAA});
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.bytes_consumed, 2u);
    }
    {
        auto result = compile_and_run(
            "Header = struct { version: uint8 }\n"
            "Pkt = struct { hdr: Header, tag: uint8 where(hdr.version == 1) }",
            "Pkt", {0x02, 0xAA});
        EXPECT_FALSE(result.success);
    }
}

TEST(CEKCompiler, DottedRefSwitch) {
    // hdr.type used as switch discriminator
    {
        auto result = compile_and_run(
            "Header = struct { type: uint8 }\n"
            "Pkt = struct { hdr: Header, data: switch(hdr.type) { 1: uint8; default: uint16le; } }",
            "Pkt", {0x01, 0x42});
        ASSERT_TRUE(result.success);
        EXPECT_EQ(result.bytes_consumed, 2u);
    }
    {
        auto result = compile_and_run(
            "Header = struct { type: uint8 }\n"
            "Pkt = struct { hdr: Header, data: switch(hdr.type) { 1: uint8; default: uint16le; } }",
            "Pkt", {0x02, 0xAA, 0xBB});
        ASSERT_TRUE(result.success);
        EXPECT_EQ(result.bytes_consumed, 3u);
    }
}

// --- Conditional optional ---

TEST(CEKCompiler, OptionalConditional) {
    // optional with where constraint
    {
        auto result = compile_and_run(
            "Pkt = struct { flags: uint8, extra: optional<uint8> where(flags == 1) }",
            "Pkt", {0x01, 0x42});
        ASSERT_TRUE(result.success);
        EXPECT_EQ(result.bytes_consumed, 2u);
        ASSERT_GE(result.root->child_count, 2);
        EXPECT_EQ(result.root->children[1].computed_value, 0x42);
    }
    {
        auto result = compile_and_run(
            "Pkt = struct { flags: uint8, extra: optional<uint8> where(flags == 1) }",
            "Pkt", {0x00});
        ASSERT_TRUE(result.success);
        EXPECT_EQ(result.bytes_consumed, 1u);
        EXPECT_EQ(result.root->child_count, 1);
    }
}

TEST(CEKCompiler, PeekBuiltin) {
    // peek() should return current byte value without consuming
    auto result = compile_and_run(
        "Msg = struct {\n"
        "    data: array<uint8>(none, until(peek() == 0)),\n"
        "    null: uint8\n"
        "}",
        "Msg", {0x41, 0x42, 0x43, 0x00});
    ASSERT_TRUE(result.success) << (result.error_message ? result.error_message : "");
    EXPECT_EQ(result.bytes_consumed, 4u);
    // data array should have 3 elements (0x41, 0x42, 0x43), then null = 0x00
    ASSERT_TRUE(result.root != nullptr);
    ASSERT_GE(result.root->child_count, 2);
    // First child is the data array
    auto& arr = result.root->children[0];
    EXPECT_EQ(arr.child_count, 3);
    // Last child is the null terminator
    auto& null_field = result.root->children[1];
    EXPECT_EQ(null_field.computed_value, 0);
}

TEST(CEKCompiler, PeekAtEnd) {
    // peek() at end-of-input should return 0
    auto result = compile_and_run(
        "Msg = struct {\n"
        "    data: array<uint8>(none, until(peek() == 0))\n"
        "}",
        "Msg", {0x41, 0x42});
    ASSERT_TRUE(result.success) << (result.error_message ? result.error_message : "");
    // All bytes consumed — peek() returned 0 at EOF
    EXPECT_EQ(result.bytes_consumed, 2u);
    ASSERT_TRUE(result.root != nullptr);
    ASSERT_GE(result.root->child_count, 1);
    auto& arr = result.root->children[0];
    EXPECT_EQ(arr.child_count, 2);
}
