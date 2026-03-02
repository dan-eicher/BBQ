#include <gtest/gtest.h>
#include "Parser.h"
#include "BBQ_AST.h"
#include "Sema.h"
#include "TypeEmitter.h"
#include "ParserEmitter.h"

using namespace BBQ;
using namespace bbqgen;

static Grammar* parse(const char* src) {
    auto* parser = new Parser();
    parser->init(src, static_cast<int>(strlen(src)));
    if (!parser->parse()) return nullptr;
    return parser->ast;
}

static std::string emit_parser(const char* src) {
    auto* g = parse(src);
    if (!g) return "";
    ErrorReporter errors;
    Sema sema(errors);
    if (!sema.analyze(g)) return "";
    TypeEmitter types(sema);
    ParserEmitter emitter(sema, types);
    return emitter.emit_impls(g);
}

static std::string emit_parser_decls(const char* src) {
    auto* g = parse(src);
    if (!g) return "";
    ErrorReporter errors;
    Sema sema(errors);
    if (!sema.analyze(g)) return "";
    TypeEmitter types(sema);
    ParserEmitter emitter(sema, types);
    return emitter.emit_decls(g);
}

TEST(ParserEmitter, StructReads) {
    auto out = emit_parser(
        "Header = struct {\n"
        "    magic: uint32be,\n"
        "    version: uint16le\n"
        "}");
    EXPECT_NE(out.find("parse_Header"), std::string::npos);
    EXPECT_NE(out.find("read_uint32be(out.magic)"), std::string::npos);
    EXPECT_NE(out.find("read_uint16le(out.version)"), std::string::npos);
}

TEST(ParserEmitter, Constraint) {
    auto out = emit_parser(
        "Header = struct {\n"
        "    magic: uint32be where magic == 0x89504E47\n"
        "}");
    EXPECT_NE(out.find("read_uint32be(out.magic)"), std::string::npos);
    EXPECT_NE(out.find("constraint failed"), std::string::npos);
}

TEST(ParserEmitter, ComputeField) {
    auto out = emit_parser(
        "H = struct {\n"
        "    ver_ihl: uint8,\n"
        "    ihl: compute(ver_ihl & 0x0F : uint8)\n"
        "}");
    EXPECT_NE(out.find("out.ihl = (out.ver_ihl & 15)"), std::string::npos);
}

TEST(ParserEmitter, UnionBacktracking) {
    auto out = emit_parser(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "Msg = union {\n"
        "    text: Text,\n"
        "    binary: Bin\n"
        "}");
    EXPECT_NE(out.find("ctx.save()"), std::string::npos);
    EXPECT_NE(out.find("ctx.restore(saved)"), std::string::npos);
    EXPECT_NE(out.find("no variant matched"), std::string::npos);
}

TEST(ParserEmitter, SwitchDispatch) {
    auto out = emit_parser(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "P = switch(t) {\n"
        "    1: Text;\n"
        "    2: Bin;\n"
        "    default: Text;\n"
        "}");
    EXPECT_NE(out.find("switch"), std::string::npos);
    EXPECT_NE(out.find("case 1:"), std::string::npos);
    EXPECT_NE(out.find("case 2:"), std::string::npos);
    EXPECT_NE(out.find("default:"), std::string::npos);
}

TEST(ParserEmitter, ArrayFixed) {
    auto out = emit_parser("Colors = array<uint8>[3]");
    EXPECT_NE(out.find("out.resize(count)"), std::string::npos);
    EXPECT_NE(out.find("for (size_t i = 0; i < count; i++)"), std::string::npos);
}

TEST(ParserEmitter, ArrayEof) {
    auto out = emit_parser(
        "Entry = struct { x: uint8 }\n"
        "Items = array<Entry>(none, eof)");
    EXPECT_NE(out.find("while (!ctx.at_end())"), std::string::npos);
    EXPECT_NE(out.find("out.emplace_back()"), std::string::npos);
}

TEST(ParserEmitter, ArrayUntil) {
    auto out = emit_parser(
        "Pkt = struct { x: uint8 }\n"
        "Pkts = array<Pkt>(none, until(remaining < 4))");
    EXPECT_NE(out.find("while (!("), std::string::npos);
}

TEST(ParserEmitter, Interval) {
    auto out = emit_parser(
        "Header = struct { x: uint32be }\n"
        "F = struct { hdr: Header[0, 64] }");
    EXPECT_NE(out.find("push_interval(0, 64)"), std::string::npos);
}

TEST(ParserEmitter, EOIExpr) {
    auto out = emit_parser(
        "Footer = struct { x: uint32be }\n"
        "F = struct { footer: Footer[EOI - 16, EOI] }");
    EXPECT_NE(out.find("ctx.total_size()"), std::string::npos);
}

TEST(ParserEmitter, ExprCompilation) {
    auto out = emit_parser(
        "H = struct {\n"
        "    a: uint8,\n"
        "    b: uint8 where a + 1 > 0\n"
        "}");
    EXPECT_NE(out.find("out.a"), std::string::npos);
}

TEST(ParserEmitter, DefaultEndian) {
    auto out = emit_parser(
        "@endian big\n"
        "H = struct { x: uint32 }");
    // Default-endian fields use dispatching method (runtime endianness)
    EXPECT_NE(out.find("read_uint32("), std::string::npos);
    // Should NOT hardcode be/le suffix for Default-endian fields
    EXPECT_EQ(out.find("read_uint32be"), std::string::npos);
    EXPECT_EQ(out.find("read_uint32le"), std::string::npos);
}

TEST(ParserEmitter, Declarations) {
    auto* g = parse("H = struct { x: uint8 }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    ASSERT_TRUE(sema.analyze(g));
    TypeEmitter types(sema);
    ParserEmitter emitter(sema, types);
    auto decls = emitter.emit_decls(g);
    EXPECT_NE(decls.find("bool parse_H(bbq::BBQContext& ctx, H& out)"), std::string::npos);
}

TEST(ParserEmitter, BytesWithLength) {
    auto out = emit_parser(
        "H = struct { data: bytes[16] }");
    EXPECT_NE(out.find("read_bytes"), std::string::npos);
}

TEST(ParserEmitter, OptionalTryRestore) {
    auto out = emit_parser(
        "Ext = struct { x: uint8 }\n"
        "MaybeExt = optional<Ext>");
    EXPECT_NE(out.find("ctx.save()"), std::string::npos);
    EXPECT_NE(out.find("std::nullopt"), std::string::npos);
}

TEST(ParserEmitter, RuleRefDelegation) {
    auto out = emit_parser(
        "Header = struct { x: uint8 }\n"
        "Hdr = Header");
    EXPECT_NE(out.find("parse_Header(ctx, out)"), std::string::npos);
}

TEST(ParserEmitter, UnionWithInlineStruct) {
    auto out = emit_parser(
        "Msg = union {\n"
        "    text: struct { marker: uint8, data: uint8 },\n"
        "    binary: struct { marker: uint8, len: uint16le }\n"
        "}");
    // Should have real parse code, not false /* complex inline type */
    EXPECT_EQ(out.find("complex inline type"), std::string::npos);
    EXPECT_NE(out.find("read_uint8(val.marker)"), std::string::npos);
}

TEST(ParserEmitter, SwitchTopLevelUsesParam) {
    auto out = emit_parser(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "P = switch(t) {\n"
        "    1: Text;\n"
        "    2: Bin;\n"
        "}");
    // Top-level switch should use the discriminator parameter, not out.something
    EXPECT_NE(out.find("switch (static_cast<int64_t>(discriminator))"), std::string::npos);
}

TEST(ParserEmitter, SwitchInlineUsesExpr) {
    auto out = emit_parser(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "Msg = struct {\n"
        "    type: uint8,\n"
        "    payload: switch(type) { 1: Text; 2: Bin; }\n"
        "}");
    // Inline switch should use compiled expression (out.type)
    EXPECT_NE(out.find("switch (static_cast<int64_t>(out.type))"), std::string::npos);
}

TEST(ParserEmitter, ArrayWithTypeSeparator) {
    auto out = emit_parser(
        "Comma = struct { sep: uint8 }\n"
        "Record = struct { val: uint16le }\n"
        "Records = array<Record>(Comma, eof)");
    // Should have real separator parse code, not just a comment
    EXPECT_NE(out.find("Comma sep_"), std::string::npos);
    EXPECT_NE(out.find("parse_Comma"), std::string::npos);
    EXPECT_EQ(out.find("skip separator"), std::string::npos) << "stubs should be gone";
}

TEST(ParserEmitter, ArrayWithTypeSeparatorCount) {
    auto out = emit_parser(
        "Comma = struct { sep: uint8 }\n"
        "Item = struct { val: uint8 }\n"
        "Items = array<Item>(Comma, count(5))");
    EXPECT_NE(out.find("Comma sep_"), std::string::npos);
    EXPECT_NE(out.find("i > 0"), std::string::npos);
}

TEST(ParserEmitter, ArrayPopBackOnFailure) {
    auto out = emit_parser(
        "Entry = struct { x: uint8 }\n"
        "Items = array<Entry>(none, eof)");
    // After emplace_back(), a failed read should pop_back() before failing
    EXPECT_NE(out.find("pop_back()"), std::string::npos);
}

TEST(ParserEmitter, StringEscaping) {
    // StrLit with special chars — we can't easily parse a BBQ string with
    // embedded quotes through the scanner, but we can test the compile_expr
    // path indirectly. At minimum, verify normal strings pass through clean.
    auto out = emit_parser(
        "Tag = struct { name: string[4] }");
    // Just verifying the emitter doesn't crash on string types
    EXPECT_NE(out.find("read_string"), std::string::npos);
}

TEST(ParserEmitter, HexLiteralLargeValue) {
    auto out = emit_parser(
        "Header = struct {\n"
        "    magic: uint32be where magic == 0x89504E47\n"
        "}");
    // Must emit as unsigned hex, not negative decimal
    EXPECT_EQ(out.find("-1991225785"), std::string::npos) << "should not emit negative decimal";
    EXPECT_NE(out.find("0x89504E47ULL"), std::string::npos) << "should emit unsigned hex literal";
}

TEST(ParserEmitter, SwitchIdentCase) {
    auto out = emit_parser(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "P = switch(t) {\n"
        "    FOO: Text;\n"
        "    BAR: Bin;\n"
        "}");
    EXPECT_NE(out.find("case FOO:"), std::string::npos);
    EXPECT_NE(out.find("case BAR:"), std::string::npos);
}

TEST(ParserEmitter, SwitchStringIfElse) {
    auto out = emit_parser(
        "JsonBody = struct { data: uint8 }\n"
        "XmlBody = struct { data: uint16le }\n"
        "P = switch(t) {\n"
        "    \"json\": JsonBody;\n"
        "    \"xml\": XmlBody;\n"
        "}");
    // Should use if-else chain, not C++ switch
    EXPECT_EQ(out.find("switch ("), std::string::npos) << "string switch should not emit C++ switch";
    EXPECT_NE(out.find("== \"json\""), std::string::npos);
    EXPECT_NE(out.find("== \"xml\""), std::string::npos);
    EXPECT_NE(out.find("else if"), std::string::npos);
}

TEST(ParserEmitter, SwitchNamedFromStruct) {
    auto out = emit_parser(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "Payload = switch(type) {\n"
        "    1: Text;\n"
        "    2: Bin;\n"
        "}\n"
        "File = struct {\n"
        "    type: uint8,\n"
        "    data: Payload\n"
        "}");
    // Should pass discriminator as 3rd arg
    EXPECT_NE(out.find("parse_Payload(ctx, out.data, out.type)"), std::string::npos)
        << "named switch call should include discriminator";
}

TEST(ParserEmitter, RemainingExpr) {
    auto out = emit_parser(
        "Pkt = struct { x: uint8 }\n"
        "Pkts = array<Pkt>(none, until(remaining < 4))");
    EXPECT_NE(out.find("ctx.remaining()"), std::string::npos);
    EXPECT_EQ(out.find("out.remaining"), std::string::npos)
        << "remaining should map to ctx.remaining(), not out.remaining";
}

TEST(ParserEmitter, PosExpr) {
    auto out = emit_parser(
        "H = struct {\n"
        "    data: uint8,\n"
        "    offset: compute(pos : uint32)\n"
        "}");
    EXPECT_NE(out.find("ctx.pos()"), std::string::npos);
    EXPECT_EQ(out.find("out.pos"), std::string::npos)
        << "pos should map to ctx.pos(), not out.pos";
}

TEST(ParserEmitter, StringSwitchParamType) {
    auto decls = emit_parser_decls(
        "JsonBody = struct { data: uint8 }\n"
        "XmlBody = struct { data: uint16le }\n"
        "P = switch(t) {\n"
        "    \"json\": JsonBody;\n"
        "    \"xml\": XmlBody;\n"
        "}");
    EXPECT_NE(decls.find("const std::string&"), std::string::npos)
        << "string switch should use const std::string& parameter";
    EXPECT_EQ(decls.find("int64_t discriminator"), std::string::npos)
        << "string switch should not use int64_t parameter";
}

TEST(ParserEmitter, ExternCall) {
    auto out = emit_parser(
        "Payload = extern(\"decompress_zlib\", \"std::vector<uint8_t>\")");
    EXPECT_NE(out.find("decompress_zlib(ctx, out)"), std::string::npos);
    EXPECT_NE(out.find("parse_Payload"), std::string::npos);
}

TEST(ParserEmitter, ElementIntervalLoop) {
    auto out = emit_parser(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    num: uint32le,\n"
        "    off: uint32le,\n"
        "    sz: uint32le,\n"
        "    entries: array<Entry>[num] @ [off + i * sz, off + (i + 1) * sz]\n"
        "}");
    EXPECT_NE(out.find("push_interval"), std::string::npos);
    EXPECT_NE(out.find("elem_scope"), std::string::npos);
    // Verify i appears as bare i, not out.i
    EXPECT_EQ(out.find("out.i"), std::string::npos)
        << "loop variable i should compile to bare i, not out.i";
}

TEST(ParserEmitter, BitfieldBigEndian) {
    auto out = emit_parser(
        "Flags = bitfield<uint16be> {\n"
        "    version: 4,\n"
        "    ihl: 4,\n"
        "    dscp: 6,\n"
        "    ecn: 2\n"
        "}");
    EXPECT_NE(out.find("parse_Flags"), std::string::npos);
    EXPECT_NE(out.find("read_uint16be(_bf)"), std::string::npos);
    // BE = MSB-first: version at bits 15-12 (shift 12)
    EXPECT_NE(out.find("_bf >> 12"), std::string::npos);
    // ecn at bits 1-0 (no shift, just mask)
    EXPECT_NE(out.find("_bf & 0x3"), std::string::npos);
}

TEST(ParserEmitter, BitfieldLittleEndian) {
    auto out = emit_parser(
        "Flags = bitfield<uint16le> {\n"
        "    a: 5,\n"
        "    b: 6,\n"
        "    c: 5\n"
        "}");
    EXPECT_NE(out.find("read_uint16le(_bf)"), std::string::npos);
    // LE = LSB-first: a at bit 0 (no shift), b at bit 5, c at bit 11
    EXPECT_NE(out.find("_bf & 0x1F"), std::string::npos);
    EXPECT_NE(out.find("_bf >> 5"), std::string::npos);
    EXPECT_NE(out.find("_bf >> 11"), std::string::npos);
}

TEST(ParserEmitter, BitfieldDeclaration) {
    auto decls = emit_parser_decls(
        "Flags = bitfield<uint8> { a: 4, b: 4 }");
    EXPECT_NE(decls.find("bool parse_Flags(bbq::BBQContext& ctx, Flags& out)"), std::string::npos);
}

TEST(ParserEmitter, LoopVarI) {
    auto out = emit_parser(
        "Entry = struct { x: uint8 }\n"
        "Items = struct {\n"
        "    n: uint8,\n"
        "    off: uint32le,\n"
        "    sz: uint32le,\n"
        "    data: array<Entry>[n] @ [off + i * sz, off + (i + 1) * sz]\n"
        "}");
    // i should not be prefixed with out.
    EXPECT_EQ(out.find("out.i"), std::string::npos);
    // But out.off and out.sz should be present
    EXPECT_NE(out.find("out.off"), std::string::npos);
    EXPECT_NE(out.find("out.sz"), std::string::npos);
}

// === Nested struct codegen ===

TEST(ParserEmitter, NestedStructFieldReads) {
    auto out = emit_parser(
        "Msg = struct {\n"
        "    header: struct { magic: uint32be, version: uint16le },\n"
        "    payload: uint8\n"
        "}");
    // Nested struct fields should use out.header. prefix
    EXPECT_NE(out.find("read_uint32be(out.header.magic)"), std::string::npos);
    EXPECT_NE(out.find("read_uint16le(out.header.version)"), std::string::npos);
    // Top-level field
    EXPECT_NE(out.find("read_uint8(out.payload)"), std::string::npos);
}

TEST(ParserEmitter, NestedStructConstraintRefsParent) {
    auto out = emit_parser(
        "Msg = struct {\n"
        "    version: uint16le,\n"
        "    inner: struct {\n"
        "        data: uint8 where version > 0\n"
        "    }\n"
        "}");
    // Constraint in nested struct should reference out.version (parent field)
    EXPECT_NE(out.find("out.version"), std::string::npos);
    EXPECT_NE(out.find("read_uint8(out.inner.data)"), std::string::npos);
}

TEST(ParserEmitter, NestedStructIntervalRefsParent) {
    auto out = emit_parser(
        "Msg = struct {\n"
        "    len: uint32le,\n"
        "    inner: struct {\n"
        "        data: bytes[len]\n"
        "    }\n"
        "}");
    // Interval in nested struct should reference out.len (parent field)
    EXPECT_NE(out.find("out.len"), std::string::npos);
    EXPECT_NE(out.find("read_bytes(out.inner.data"), std::string::npos);
}

TEST(ParserEmitter, DeeplyNestedStructPrefix) {
    auto out = emit_parser(
        "Msg = struct {\n"
        "    mid: struct {\n"
        "        inner: struct { x: uint8 }\n"
        "    }\n"
        "}");
    // Should use out.mid.inner.x prefix
    EXPECT_NE(out.find("out.mid.inner.x"), std::string::npos);
}

// === Runtime endian switching ===

TEST(ParserEmitter, EndianSwitchEmitsSetEndian) {
    auto out = emit_parser(
        "TIFF = struct {\n"
        "    byte_order: uint16le,\n"
        "    @endian: byte_order == 0x4949 ? little : big,\n"
        "    magic: uint16,\n"
        "    offset: uint32\n"
        "}");
    // Should emit ctx.set_endian() call
    EXPECT_NE(out.find("ctx.set_endian("), std::string::npos);
    // true = little, false = big
    EXPECT_NE(out.find("true"), std::string::npos);
    EXPECT_NE(out.find("false"), std::string::npos);
}

TEST(ParserEmitter, DefaultEndianUsesDispatching) {
    auto out = emit_parser(
        "H = struct { x: uint16 }");
    // Default-endian uint16 should use dispatching method
    EXPECT_NE(out.find("read_uint16("), std::string::npos);
    // Should not have le/be suffix
    EXPECT_EQ(out.find("read_uint16le"), std::string::npos);
    EXPECT_EQ(out.find("read_uint16be"), std::string::npos);
}

TEST(ParserEmitter, ExplicitEndianUnaffected) {
    auto out = emit_parser(
        "H = struct {\n"
        "    @endian: little,\n"
        "    x: uint16le,\n"
        "    y: uint32be\n"
        "}");
    // Explicit le/be should still use hardcoded methods
    EXPECT_NE(out.find("read_uint16le("), std::string::npos);
    EXPECT_NE(out.find("read_uint32be("), std::string::npos);
}

TEST(ParserEmitter, EndianLitCompilation) {
    auto out = emit_parser(
        "H = struct {\n"
        "    @endian: big,\n"
        "    val: uint32\n"
        "}");
    // EndianLit(Big) should compile to false
    EXPECT_NE(out.find("ctx.set_endian(false)"), std::string::npos);
}

// === Cross-rule scope resolution ===

TEST(ParserEmitter, CrossRuleScope) {
    // ScopeGuard should be emitted around parse_X calls
    auto out = emit_parser(
        "Inner = struct { x: uint8 }\n"
        "Outer = struct {\n"
        "    y: uint16le,\n"
        "    child: Inner\n"
        "}");
    EXPECT_NE(out.find("ScopeGuard"), std::string::npos);
    EXPECT_NE(out.find("_scope(ctx, &out)"), std::string::npos) << out;
}

TEST(ParserEmitter, CrossRefExpr) {
    // Cross-rule reference should generate static_cast through ctx.scope_ptr
    auto out = emit_parser(
        "Inner = struct {\n"
        "    raw: uint8,\n"
        "    val: compute(raw != 0 ? raw : parent_field : uint8)\n"
        "}\n"
        "Outer = struct {\n"
        "    parent_field: uint8,\n"
        "    child: Inner\n"
        "}");
    // In parse_Inner, parent_field should resolve via scope_ptr
    // But since Inner is compiled without parent context (it's a standalone rule),
    // the runtime fallback uses scope_ptr
    EXPECT_NE(out.find("parse_Inner"), std::string::npos);
    // In parse_Outer, calling parse_Inner should push scope
    EXPECT_NE(out.find("ScopeGuard"), std::string::npos);
}

TEST(ParserEmitter, CrossRefLoopIndex) {
    // 'i' outside array loop should emit ctx.loop_index()
    auto out = emit_parser(
        "Inner = struct {\n"
        "    raw: uint8,\n"
        "    val: compute(i > 0 ? raw : 0 : uint8)\n"
        "}");
    EXPECT_NE(out.find("ctx.loop_index()"), std::string::npos);
}

TEST(ParserEmitter, LoopGuardEmitted) {
    // Array loops should emit LoopGuard
    auto out = emit_parser("Items = array<uint8>[3]");
    EXPECT_NE(out.find("LoopGuard"), std::string::npos);
    EXPECT_NE(out.find("set_loop_index"), std::string::npos);
}

TEST(ParserEmitter, LoopGuardEofArray) {
    // EOF-terminated array should also emit LoopGuard
    auto out = emit_parser(
        "Entry = struct { x: uint8 }\n"
        "Items = array<Entry>(none, eof)");
    EXPECT_NE(out.find("LoopGuard"), std::string::npos);
    EXPECT_NE(out.find("set_loop_index"), std::string::npos);
}

TEST(ParserEmitter, PeekExpr) {
    auto out = emit_parser(
        "Msg = struct {\n"
        "    data: array<uint8>(none, until(peek() == 0)),\n"
        "    null: uint8\n"
        "}");
    EXPECT_NE(out.find("ctx.peek()"), std::string::npos);
}
