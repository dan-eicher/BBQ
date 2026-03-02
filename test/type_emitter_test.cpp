#include <gtest/gtest.h>
#include "Parser.h"
#include "BBQ_AST.h"
#include "Sema.h"
#include "TypeEmitter.h"

using namespace BBQ;
using namespace bbqgen;

static Grammar* parse(const char* src) {
    auto* parser = new Parser();
    parser->init(src, static_cast<int>(strlen(src)));
    if (!parser->parse()) return nullptr;
    return parser->ast;
}

static std::string emit_types(const char* src, const std::string& ns = "") {
    auto* g = parse(src);
    if (!g) return "";
    ErrorReporter errors;
    Sema sema(errors);
    if (!sema.analyze(g)) return "";
    TypeEmitter emitter(sema, ns);
    return emitter.emit(g);
}

TEST(TypeEmitter, SimpleStruct) {
    auto out = emit_types("Foo = struct { x: uint8, y: uint32be }");
    EXPECT_NE(out.find("struct Foo"), std::string::npos);
    EXPECT_NE(out.find("uint8_t x"), std::string::npos);
    EXPECT_NE(out.find("uint32_t y"), std::string::npos);
}

TEST(TypeEmitter, UnionType) {
    auto out = emit_types(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "Msg = union {\n"
        "    text: Text,\n"
        "    binary: Bin\n"
        "}");
    EXPECT_NE(out.find("struct Msg"), std::string::npos);
    EXPECT_NE(out.find("std::variant<Text, Bin> data"), std::string::npos);
}

TEST(TypeEmitter, ArrayType) {
    auto out = emit_types("Colors = array<uint8>[3]");
    EXPECT_NE(out.find("using Colors = std::vector<uint8_t>"), std::string::npos);
}

TEST(TypeEmitter, OptionalType) {
    auto out = emit_types("MaybeExt = optional<uint64le>");
    EXPECT_NE(out.find("using MaybeExt = std::optional<uint64_t>"), std::string::npos);
}

TEST(TypeEmitter, ComputeField) {
    auto out = emit_types(
        "H = struct {\n"
        "    ver_ihl: uint8,\n"
        "    ihl: compute(ver_ihl & 0x0F : uint8)\n"
        "}");
    EXPECT_NE(out.find("struct H"), std::string::npos);
    EXPECT_NE(out.find("uint8_t ver_ihl"), std::string::npos);
    EXPECT_NE(out.find("uint8_t ihl"), std::string::npos);
}

TEST(TypeEmitter, TopologicalOrder) {
    auto out = emit_types(
        "File = struct { hdr: Header, body: Body }\n"
        "Header = struct { magic: uint32be }\n"
        "Body = struct { data: uint8 }");
    // Header and Body must appear before File
    auto header_pos = out.find("struct Header");
    auto body_pos = out.find("struct Body");
    auto file_pos = out.find("struct File");
    ASSERT_NE(header_pos, std::string::npos);
    ASSERT_NE(body_pos, std::string::npos);
    ASSERT_NE(file_pos, std::string::npos);
    EXPECT_LT(header_pos, file_pos);
    EXPECT_LT(body_pos, file_pos);
}

TEST(TypeEmitter, Namespace) {
    auto out = emit_types("Foo = struct { x: uint8 }", "myformat");
    EXPECT_NE(out.find("namespace myformat {"), std::string::npos);
    EXPECT_NE(out.find("} // namespace myformat"), std::string::npos);
}

TEST(TypeEmitter, PrimitiveAlias) {
    auto out = emit_types("Byte = uint8");
    EXPECT_NE(out.find("using Byte = uint8_t"), std::string::npos);
}

TEST(TypeEmitter, RuleRefAlias) {
    auto out = emit_types(
        "Header = struct { x: uint8 }\n"
        "Hdr = Header");
    EXPECT_NE(out.find("using Hdr = Header"), std::string::npos);
}

TEST(TypeEmitter, SwitchType) {
    auto out = emit_types(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "P = switch(t) {\n"
        "    1: Text;\n"
        "    2: Bin;\n"
        "    default: Text;\n"
        "}");
    EXPECT_NE(out.find("struct P"), std::string::npos);
    EXPECT_NE(out.find("std::variant<Text, Bin, Text> data"), std::string::npos);
}

TEST(TypeEmitter, InlineSwitchField) {
    auto out = emit_types(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "Msg = struct {\n"
        "    type: uint8,\n"
        "    payload: switch(type) { 1: Text; 2: Bin; }\n"
        "}");
    EXPECT_NE(out.find("struct Msg"), std::string::npos);
    EXPECT_NE(out.find("std::variant<Text, Bin> payload"), std::string::npos);
}

TEST(TypeEmitter, StringAndBytesFields) {
    auto out = emit_types(
        "Rec = struct {\n"
        "    name: string,\n"
        "    data: bytes,\n"
        "    flag: bool\n"
        "}");
    EXPECT_NE(out.find("std::string name"), std::string::npos);
    EXPECT_NE(out.find("std::vector<uint8_t> data"), std::string::npos);
    EXPECT_NE(out.find("bool flag"), std::string::npos);
}

// === Inline struct type generation ===

TEST(TypeEmitter, InlineStructProducesNamedType) {
    auto out = emit_types(
        "Outer = struct {\n"
        "    x: uint8,\n"
        "    inner: struct { y: uint16le }\n"
        "}");
    // Should produce a named type Outer_inner
    EXPECT_NE(out.find("struct Outer_inner"), std::string::npos);
    EXPECT_NE(out.find("uint16_t y"), std::string::npos);
    // Parent struct should reference the named type
    EXPECT_NE(out.find("Outer_inner inner"), std::string::npos);
}

TEST(TypeEmitter, NestedInlineStructProducesChainedName) {
    auto out = emit_types(
        "Outer = struct {\n"
        "    mid: struct {\n"
        "        inner: struct { z: uint32le }\n"
        "    }\n"
        "}");
    // Should produce Outer_mid and Outer_mid_inner
    EXPECT_NE(out.find("struct Outer_mid_inner"), std::string::npos);
    EXPECT_NE(out.find("struct Outer_mid"), std::string::npos);
    EXPECT_NE(out.find("uint32_t z"), std::string::npos);
}

TEST(TypeEmitter, InlineStructCppTypeReturnsName) {
    auto out = emit_types(
        "Msg = struct {\n"
        "    header: struct { magic: uint32be, version: uint16le },\n"
        "    payload: uint8\n"
        "}");
    EXPECT_NE(out.find("struct Msg_header"), std::string::npos);
    EXPECT_NE(out.find("uint32_t magic"), std::string::npos);
    EXPECT_NE(out.find("Msg_header header"), std::string::npos);
}

TEST(TypeEmitter, EndianSwitchFieldSkipped) {
    auto out = emit_types(
        "TIFF = struct {\n"
        "    byte_order: uint16le,\n"
        "    @endian: byte_order == 0x4949 ? little : big,\n"
        "    magic: uint16,\n"
        "    offset: uint32\n"
        "}");
    // Struct should have byte_order, magic, offset — but no @endian field
    EXPECT_NE(out.find("uint16_t byte_order"), std::string::npos);
    EXPECT_NE(out.find("uint16_t magic"), std::string::npos);
    EXPECT_NE(out.find("uint32_t offset"), std::string::npos);
    // No empty-named field or EndianSwitch artifact
    EXPECT_EQ(out.find("endian"), std::string::npos);
}
