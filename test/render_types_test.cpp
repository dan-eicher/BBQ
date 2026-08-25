// render_types_test — structural pins for the destination-driven C type-header
// generator (backends/render/RenderTypesC). render_types_c was validated
// byte-for-byte against the (now retired) CTypeEmitter during cutover; this
// guards the per-construct shape going forward (c_backend_e2e covers it
// behaviorally by compiling + round-tripping the generated types).
#include <gtest/gtest.h>
#include "Parser.h"
#include "Sema.h"
#include "CompilerCtx.h"
#include "RenderTypes.h"
#include <string>

using namespace BBQ;

namespace {

std::string render(const std::string& spec, const std::string& prefix = "") {
    Parser parser;
    parser.init(spec.c_str(), (int)spec.size());
    EXPECT_TRUE(parser.parse()) << "parse failed";
    bbqgen::ErrorReporter rep;
    bbqgen::Sema sema(rep);
    EXPECT_TRUE(sema.analyze(parser.ast)) << "sema failed";
    std::string tmpl = std::string(SOURCE_DIR) + "/backends/render/templates/types_c.inja";
    bbq::render::CompilerCtx ctx{parser.ast, &sema, nullptr, prefix};
    return bbq::render::render_types_c(ctx, tmpl);
}

bool has(const std::string& h, const std::string& needle) {
    return h.find(needle) != std::string::npos;
}

}  // namespace

TEST(RenderTypes, FlatStruct) {
    auto h = render("Sample = struct { x: uint8, y: uint16le }");
    EXPECT_TRUE(has(h, "struct sample {"));
    EXPECT_TRUE(has(h, "uint8_t x;"));
    EXPECT_TRUE(has(h, "uint16_t y;"));
    EXPECT_TRUE(has(h, "typedef struct sample sample_t;"));
}

TEST(RenderTypes, NestedStruct) {
    auto h = render("Nested = struct { hdr: struct { a: uint8 }, b: uint8 }");
    EXPECT_TRUE(has(h, "struct nested_hdr {"));        // inline struct hoisted
    EXPECT_TRUE(has(h, "nested_hdr_t hdr;"));
}

TEST(RenderTypes, PrimitiveRule) {
    EXPECT_TRUE(has(render("Byte = uint8"), "typedef uint8_t byte_t;"));
}

TEST(RenderTypes, Union) {
    auto h = render("A = struct { a: uint8 }\nB = struct { b: uint16le }\n"
                    "Msg = union { small: A, large: B }");
    EXPECT_TRUE(has(h, "enum msg_tag {"));
    EXPECT_TRUE(has(h, "msg_small = 0,"));
    EXPECT_TRUE(has(h, "msg_large,"));
    EXPECT_TRUE(has(h, "enum msg_tag tag;"));
    EXPECT_TRUE(has(h, "a_t small;"));
    EXPECT_TRUE(has(h, "b_t large;"));
}

TEST(RenderTypes, Switch) {
    auto h = render("TypeA = struct { a: uint8 }\nTypeB = struct { b: uint16le }\n"
                    "Msg = struct { tag: uint8, body: switch(tag) { 1: TypeA; 2: TypeB; } }");
    EXPECT_TRUE(has(h, "type_a_t case_0;"));
    EXPECT_TRUE(has(h, "type_b_t case_1;"));
}

TEST(RenderTypes, ArrayRule) {
    auto h = render("Item = struct { val: uint8 }\nList = struct { count: uint8, items: array<Item>[count] }");
    EXPECT_TRUE(has(h, "item_t* items;"));
    EXPECT_TRUE(has(h, "size_t count;"));
    EXPECT_TRUE(has(h, "list_free"));                  // owns the items array
}

TEST(RenderTypes, OptionalRule) {
    auto h = render("Foo = struct { present: uint8, val: optional<uint16le> where present == 1 }");
    EXPECT_TRUE(has(h, "bool has_value;"));
    EXPECT_TRUE(has(h, "uint16_t value;"));
}

TEST(RenderTypes, Bitfield) {
    auto h = render("@endian big\nFlags = bitfield<uint8> { high: 4, low: 4 }");
    EXPECT_TRUE(has(h, "struct flags {"));
    EXPECT_TRUE(has(h, "uint8_t high;"));
    EXPECT_TRUE(has(h, "uint8_t low;"));
}

// A signed entry is read back sign-extended from its own width, so the field it lands
// in has to be signed; the entries beside it keep the unsigned default. The needles
// carry the leading space because "int16_t imm;" is a substring of "uint16_t imm;",
// and a pin that cannot go red is not a pin.
TEST(RenderTypes, SignedBitfieldEntry) {
    auto h = render("Imm = bitfield<uint32be> { op: 8, imm: signed 12, rest: unsigned 12 }");
    EXPECT_TRUE(has(h, "struct imm {"));
    EXPECT_TRUE(has(h, " uint8_t op;"));
    EXPECT_TRUE(has(h, " int16_t imm;"));
    EXPECT_TRUE(has(h, " uint16_t rest;"));
}

// The inline spelling is a second call site — a bitfield FIELD is spelled where it
// sits, not hoisted to a rule of its own.
TEST(RenderTypes, SignedBitfieldEntryInline) {
    auto h = render("Insn = struct { lead: uint8, bits: bitfield<uint32be> { imm: signed 12, rest: 20 } }");
    EXPECT_TRUE(has(h, "struct { int16_t imm; uint32_t rest; } bits;"));
}

TEST(RenderTypes, BytesFieldFreed) {
    auto h = render("Foo = struct { len: uint8, data: bytes[len] }");
    EXPECT_TRUE(has(h, "bbq_bytes_t data;"));
    EXPECT_TRUE(has(h, "foo_free"));                   // owns the bytes
    EXPECT_TRUE(has(h, "free((void*)"));
}

TEST(RenderTypes, UserHeader) {
    auto h = render("@header (. typedef int my_t; .)\nFoo = struct { x: uint8 }");
    EXPECT_TRUE(has(h, "typedef int my_t;"));          // verbatim user block
    EXPECT_TRUE(has(h, "#endif /* BBQ_GENERATED_TYPES_H */"));
}

// Two prefixed headers can be included by one translation unit; a guard shared
// between them would make the second include expand to nothing.
TEST(RenderTypes, IncludeGuardFollowsPrefix) {
    auto cap = render("Foo = struct { x: uint8 }", "Cap");
    EXPECT_TRUE(has(cap, "#ifndef CAP_TYPES_H"));
    EXPECT_TRUE(has(cap, "#define CAP_TYPES_H"));
    EXPECT_TRUE(has(cap, "#endif /* CAP_TYPES_H */"));

    auto exp = render("Bar = struct { y: uint8 }", "Exp");
    EXPECT_TRUE(has(exp, "#ifndef EXP_TYPES_H"));
    EXPECT_FALSE(has(exp, "CAP_TYPES_H"));
}
