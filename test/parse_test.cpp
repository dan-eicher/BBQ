#include <gtest/gtest.h>
#include "Parser.h"
#include "BBQ_AST.h"

using namespace BBQ;

// Helper: parse a BBQ source string, return Grammar* (nullptr on error)
static Grammar* parse(const char* src) {
    auto* parser = new Parser();
    parser->init(src, static_cast<int>(strlen(src)));
    if (!parser->parse()) return nullptr;
    return parser->ast;
}

// --- Minimal struct ---

TEST(Parse, MinimalStruct) {
    auto* g = parse("Foo = struct { x: uint8 }");
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(g->rules.size(), 1u);
    EXPECT_EQ(g->rules[0]->name, "Foo");

    auto* st = dynamic_cast<Struct*>(g->rules[0]->body);
    ASSERT_NE(st, nullptr);
    ASSERT_EQ(st->fields.size(), 1u);
    EXPECT_EQ(st->fields[0]->name, "x");

    auto* prim = dynamic_cast<Primitive*>(st->fields[0]->body);
    ASSERT_NE(prim, nullptr);
    auto* ik = dynamic_cast<IntegerKind*>(prim->kind);
    ASSERT_NE(ik, nullptr);
    EXPECT_EQ(ik->width, Width::W8);
    EXPECT_EQ(ik->sign, Signedness::Unsigned);
}

// --- Struct with constraint ---

TEST(Parse, StructWithConstraint) {
    auto* g = parse("Hdr = struct { magic: uint32be where magic == 0x89504E47 }");
    ASSERT_NE(g, nullptr);
    auto* st = dynamic_cast<Struct*>(g->rules[0]->body);
    ASSERT_NE(st, nullptr);
    // Constraint lives on the Primitive node (PrimitiveType consumes "where"),
    // not on the Field itself.
    auto* prim = dynamic_cast<Primitive*>(st->fields[0]->body);
    ASSERT_NE(prim, nullptr);
    EXPECT_TRUE(prim->constraint.has_value());
}

// --- Union ---

TEST(Parse, UnionDecl) {
    auto* g = parse(
        "Msg = union {\n"
        "    text: TextPayload,\n"
        "    binary: BinPayload\n"
        "}");
    ASSERT_NE(g, nullptr);
    auto* u = dynamic_cast<Union*>(g->rules[0]->body);
    ASSERT_NE(u, nullptr);
    ASSERT_EQ(u->variants.size(), 2u);
    EXPECT_EQ(u->variants[0]->name, "text");
    EXPECT_EQ(u->variants[1]->name, "binary");
}

// --- Array (fixed count) ---

TEST(Parse, ArrayFixedCount) {
    auto* g = parse("Colors = array<uint8>[3]");
    ASSERT_NE(g, nullptr);
    auto* arr = dynamic_cast<Array*>(g->rules[0]->body);
    ASSERT_NE(arr, nullptr);
    auto* fc = dynamic_cast<FixedCount*>(arr->spec);
    ASSERT_NE(fc, nullptr);
}

// --- Array (sep/term) ---

TEST(Parse, ArraySepTerm) {
    auto* g = parse("Items = array<Entry>(none, eof)");
    ASSERT_NE(g, nullptr);
    auto* arr = dynamic_cast<Array*>(g->rules[0]->body);
    ASSERT_NE(arr, nullptr);
    auto* st = dynamic_cast<SepTerm*>(arr->spec);
    ASSERT_NE(st, nullptr);
    EXPECT_NE(dynamic_cast<NoneSep*>(st->sep), nullptr);
    EXPECT_NE(dynamic_cast<EofTerm*>(st->term), nullptr);
}

// --- Optional ---

TEST(Parse, OptionalDecl) {
    auto* g = parse("MaybeExt = optional<ExtHeader>");
    ASSERT_NE(g, nullptr);
    auto* opt = dynamic_cast<Optional*>(g->rules[0]->body);
    ASSERT_NE(opt, nullptr);
}

// --- Switch ---

TEST(Parse, SwitchDecl) {
    auto* g = parse(
        "Payload = switch(hdr_type) {\n"
        "    1: TextPayload;\n"
        "    2: BinPayload;\n"
        "    default: RawPayload;\n"
        "}");
    ASSERT_NE(g, nullptr);
    auto* sw = dynamic_cast<Switch*>(g->rules[0]->body);
    ASSERT_NE(sw, nullptr);
    ASSERT_EQ(sw->cases.size(), 2u);
    EXPECT_TRUE(sw->default_.has_value());
}

// --- Compute ---

TEST(Parse, ComputeDecl) {
    auto* g = parse(
        "H = struct {\n"
        "    ver_ihl: uint8,\n"
        "    ihl: compute(ver_ihl & 0x0F : uint8)\n"
        "}");
    ASSERT_NE(g, nullptr);
    auto* st = dynamic_cast<Struct*>(g->rules[0]->body);
    ASSERT_NE(st, nullptr);
    ASSERT_EQ(st->fields.size(), 2u);
    auto* comp = dynamic_cast<Compute*>(st->fields[1]->body);
    ASSERT_NE(comp, nullptr);
}

// --- Endian directive ---

TEST(Parse, EndianDirective) {
    auto* g = parse("@endian big\nFoo = struct { x: uint32 }");
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(g->directives.size(), 1u);
    EXPECT_EQ(g->directives[0]->endian, Endianness::Big);
}

// --- Rule-level alternatives ---

TEST(Parse, Alternatives) {
    auto* g = parse("Num = uint8 | uint16le | uint32le");
    ASSERT_NE(g, nullptr);
    auto* alts = dynamic_cast<Alternatives*>(g->rules[0]->body);
    ASSERT_NE(alts, nullptr);
    EXPECT_EQ(alts->alts.size(), 3u);
}

// --- Expressions ---

TEST(Parse, ArithmeticExpr) {
    auto* g = parse("H = struct { a: uint8, b: uint8 where b > a + 1 }");
    ASSERT_NE(g, nullptr);
    auto* st = dynamic_cast<Struct*>(g->rules[0]->body);
    ASSERT_NE(st, nullptr);
    // Constraint lives on the Primitive, not the Field
    auto* prim = dynamic_cast<Primitive*>(st->fields[1]->body);
    ASSERT_NE(prim, nullptr);
    EXPECT_TRUE(prim->constraint.has_value());
    auto* binop = dynamic_cast<BinOp*>(*prim->constraint);
    ASSERT_NE(binop, nullptr);
    EXPECT_EQ(binop->op, Binop::Gt);
}

// --- Interval [start, end] ---

TEST(Parse, IntervalStartEnd) {
    auto* g = parse("F = struct { hdr: Header[0, 64] }");
    ASSERT_NE(g, nullptr);
    auto* st = dynamic_cast<Struct*>(g->rules[0]->body);
    ASSERT_NE(st, nullptr);
    // Interval lives on the RuleRef, not the Field
    auto* rr = dynamic_cast<RuleRef*>(st->fields[0]->body);
    ASSERT_NE(rr, nullptr);
    EXPECT_TRUE(rr->interval.has_value());
    auto* se = dynamic_cast<StartEnd*>(*rr->interval);
    ASSERT_NE(se, nullptr);
}

// --- Interval [length] ---

TEST(Parse, IntervalLength) {
    auto* g = parse("F = struct { data: bytes[16] }");
    ASSERT_NE(g, nullptr);
    auto* st = dynamic_cast<Struct*>(g->rules[0]->body);
    ASSERT_NE(st, nullptr);
    // Interval lives on the Primitive node
    auto* prim = dynamic_cast<Primitive*>(st->fields[0]->body);
    ASSERT_NE(prim, nullptr);
    EXPECT_TRUE(prim->interval.has_value());
    auto* len = dynamic_cast<Length*>(*prim->interval);
    ASSERT_NE(len, nullptr);
}

// --- Function call ---

TEST(Parse, FunctionCall) {
    auto* g = parse(
        "H = struct {\n"
        "    data: bytes[16],\n"
        "    crc: uint32 where crc == crc32(data, 16)\n"
        "}");
    ASSERT_NE(g, nullptr);
    auto* st = dynamic_cast<Struct*>(g->rules[0]->body);
    ASSERT_NE(st, nullptr);
    // Constraint on the Primitive, not the Field
    auto* prim = dynamic_cast<Primitive*>(st->fields[1]->body);
    ASSERT_NE(prim, nullptr);
    EXPECT_TRUE(prim->constraint.has_value());
}

// --- Multiple rules ---

TEST(Parse, MultipleRules) {
    auto* g = parse(
        "Header = struct { magic: uint32be }\n"
        "Body = struct { data: bytes[16] }\n"
        "File = struct { hdr: Header, body: Body }");
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(g->rules.size(), 3u);
    EXPECT_EQ(g->rules[0]->name, "Header");
    EXPECT_EQ(g->rules[1]->name, "Body");
    EXPECT_EQ(g->rules[2]->name, "File");
}

// --- Dotted reference ---

TEST(Parse, DottedRef) {
    auto* g = parse(
        "F = struct {\n"
        "    hdr: Header,\n"
        "    data: bytes[hdr.length]\n"
        "}");
    ASSERT_NE(g, nullptr);
    auto* st = dynamic_cast<Struct*>(g->rules[0]->body);
    ASSERT_NE(st, nullptr);
    // Interval is on the Primitive (bytes)
    auto* prim = dynamic_cast<Primitive*>(st->fields[1]->body);
    ASSERT_NE(prim, nullptr);
    EXPECT_TRUE(prim->interval.has_value());
}

// --- EOI expression ---

TEST(Parse, EOIExpr) {
    auto* g = parse("F = struct { footer: Footer[EOI - 16, EOI] }");
    ASSERT_NE(g, nullptr);
}

// --- Error cases ---

TEST(Parse, ErrorMissingEquals) {
    auto* g = parse("Foo struct { x: uint8 }");
    EXPECT_EQ(g, nullptr);
}

TEST(Parse, ErrorUnclosedStruct) {
    auto* g = parse("Foo = struct { x: uint8");
    EXPECT_EQ(g, nullptr);
}

// --- Ternary expression ---

TEST(Parse, TernaryExpr) {
    auto* g = parse(
        "H = struct {\n"
        "    a: uint8,\n"
        "    b: uint8 where (a > 0 ? true : false)\n"
        "}");
    ASSERT_NE(g, nullptr);
}

// --- Array with count terminator ---

TEST(Parse, ArrayCountTerm) {
    auto* g = parse("Items = array<Entry>(none, count(10))");
    ASSERT_NE(g, nullptr);
    auto* arr = dynamic_cast<Array*>(g->rules[0]->body);
    ASSERT_NE(arr, nullptr);
    auto* st = dynamic_cast<SepTerm*>(arr->spec);
    ASSERT_NE(st, nullptr);
    EXPECT_NE(dynamic_cast<CountTerm*>(st->term), nullptr);
}

// --- Array with until terminator ---

TEST(Parse, ArrayUntilTerm) {
    auto* g = parse("Pkts = array<Packet>(none, until(remaining < 4))");
    ASSERT_NE(g, nullptr);
    auto* arr = dynamic_cast<Array*>(g->rules[0]->body);
    ASSERT_NE(arr, nullptr);
    auto* st = dynamic_cast<SepTerm*>(arr->spec);
    ASSERT_NE(st, nullptr);
    EXPECT_NE(dynamic_cast<UntilTerm*>(st->term), nullptr);
}
