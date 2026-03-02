#include <gtest/gtest.h>
#include <sstream>
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

struct PipelineResult {
    bool ok;
    std::string types;
    std::string decls;
    std::string impls;
    std::string errors;
};

static PipelineResult run_pipeline(const char* src, const std::string& ns = "") {
    PipelineResult r{};
    auto* g = parse(src);
    if (!g) { r.errors = "parse failed"; return r; }

    ErrorReporter errors;
    Sema sema(errors);
    if (!sema.analyze(g)) {
        std::ostringstream oss;
        errors.print_all(oss);
        r.errors = oss.str();
        return r;
    }

    TypeEmitter types(sema, ns);
    r.types = types.emit(g);

    ParserEmitter pemit(sema, types, ns);
    r.decls = pemit.emit_decls(g);
    r.impls = pemit.emit_impls(g);

    r.ok = true;
    return r;
}

// --- Integration: header.bbq ---

TEST(Integration, HeaderExample) {
    auto r = run_pipeline(
        "Header = struct {\n"
        "    magic:   uint32be where magic == 0x89504E47,\n"
        "    version: uint16le,\n"
        "    flags:   uint8,\n"
        "    length:  uint32le where length > 0\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;

    // Types
    EXPECT_NE(r.types.find("struct Header"), std::string::npos);
    EXPECT_NE(r.types.find("uint32_t magic"), std::string::npos);
    EXPECT_NE(r.types.find("uint16_t version"), std::string::npos);
    EXPECT_NE(r.types.find("uint8_t flags"), std::string::npos);
    EXPECT_NE(r.types.find("uint32_t length"), std::string::npos);

    // Parser
    EXPECT_NE(r.impls.find("parse_Header"), std::string::npos);
    EXPECT_NE(r.impls.find("read_uint32be(out.magic)"), std::string::npos);
    EXPECT_NE(r.impls.find("constraint failed"), std::string::npos);
}

// --- Integration: ipv4.bbq ---

TEST(Integration, IPv4Example) {
    auto r = run_pipeline(
        "@endian big\n"
        "IPv4Header = struct {\n"
        "    ver_ihl:        uint8,\n"
        "    version:        compute((ver_ihl >> 4) & 0x0F : uint8),\n"
        "    ihl:            compute(ver_ihl & 0x0F : uint8),\n"
        "    dscp_ecn:       uint8,\n"
        "    total_length:   uint16,\n"
        "    identification: uint16,\n"
        "    flags_offset:   uint16,\n"
        "    flags:          compute((flags_offset >> 13) & 0x07 : uint8),\n"
        "    frag_offset:    compute(flags_offset & 0x1FFF : uint16),\n"
        "    ttl:            uint8,\n"
        "    protocol:       uint8,\n"
        "    checksum:       uint16,\n"
        "    src_addr:       uint32,\n"
        "    dst_addr:       uint32\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;

    // Types
    EXPECT_NE(r.types.find("struct IPv4Header"), std::string::npos);
    EXPECT_NE(r.types.find("uint8_t ver_ihl"), std::string::npos);
    EXPECT_NE(r.types.find("uint8_t version"), std::string::npos);
    EXPECT_NE(r.types.find("uint8_t ihl"), std::string::npos);

    // Parser — compute fields are inline assignments
    EXPECT_NE(r.impls.find("out.version = "), std::string::npos);
    EXPECT_NE(r.impls.find("out.ihl = "), std::string::npos);
    // Default-endian fields use dispatching methods (runtime endianness)
    EXPECT_NE(r.impls.find("read_uint16("), std::string::npos);
    EXPECT_NE(r.impls.find("read_uint32("), std::string::npos);
}

// --- Integration: tlv.bbq ---

TEST(Integration, TLVExample) {
    auto r = run_pipeline(
        "@endian little\n"
        "TLVEntry = struct {\n"
        "    type:   uint8,\n"
        "    length: uint16,\n"
        "    value:  bytes[length]\n"
        "}\n"
        "TLVStream = struct {\n"
        "    entries: array<TLVEntry>(none, eof)\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;

    // Types
    EXPECT_NE(r.types.find("struct TLVEntry"), std::string::npos);
    EXPECT_NE(r.types.find("uint8_t type"), std::string::npos);
    EXPECT_NE(r.types.find("uint16_t length"), std::string::npos);
    EXPECT_NE(r.types.find("std::vector<uint8_t> value"), std::string::npos);
    EXPECT_NE(r.types.find("struct TLVStream"), std::string::npos);

    // Parser — array with eof terminator
    EXPECT_NE(r.impls.find("while (!ctx.at_end())"), std::string::npos);
    // Default-endian fields use dispatching methods (runtime endianness)
    EXPECT_NE(r.impls.find("read_uint16("), std::string::npos);
    // bytes[length] → read_bytes with length
    EXPECT_NE(r.impls.find("read_bytes"), std::string::npos);
}

// --- Integration: namespace ---

TEST(Integration, WithNamespace) {
    auto r = run_pipeline(
        "Foo = struct { x: uint8 }", "myformat");
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.types.find("namespace myformat"), std::string::npos);
    EXPECT_NE(r.decls.find("namespace myformat"), std::string::npos);
    EXPECT_NE(r.impls.find("namespace myformat"), std::string::npos);
}

// --- Integration: multiple interdependent rules ---

TEST(Integration, MultiRule) {
    auto r = run_pipeline(
        "Header = struct { magic: uint32be, body_len: uint32le }\n"
        "Body = struct { data: bytes[16] }\n"
        "File = struct { hdr: Header, body: Body }");
    ASSERT_TRUE(r.ok) << r.errors;

    // All three types defined
    EXPECT_NE(r.types.find("struct Header"), std::string::npos);
    EXPECT_NE(r.types.find("struct Body"), std::string::npos);
    EXPECT_NE(r.types.find("struct File"), std::string::npos);

    // parse_File calls parse_Header and parse_Body
    EXPECT_NE(r.impls.find("parse_Header(ctx, out.hdr)"), std::string::npos);
    EXPECT_NE(r.impls.find("parse_Body(ctx, out.body)"), std::string::npos);
}

// --- Integration: union with backtracking ---

TEST(Integration, UnionBacktracking) {
    auto r = run_pipeline(
        "Text = struct { marker: uint8 where marker == 1 }\n"
        "Bin = struct { marker: uint8 where marker == 2 }\n"
        "Msg = union {\n"
        "    text: Text,\n"
        "    binary: Bin\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    EXPECT_NE(r.types.find("struct Msg"), std::string::npos);
    EXPECT_NE(r.types.find("std::variant<Text, Bin> data"), std::string::npos);
    EXPECT_NE(r.impls.find("ctx.save()"), std::string::npos);
    EXPECT_NE(r.impls.find("ctx.restore(saved)"), std::string::npos);
    EXPECT_NE(r.impls.find("emplace<0>"), std::string::npos);
}

// --- Integration: error detection ---

TEST(Integration, ErrorUndefinedRule) {
    auto r = run_pipeline("F = struct { hdr: Nonexistent }");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.errors.find("undefined rule"), std::string::npos);
}

TEST(Integration, ErrorCircularDep) {
    auto r = run_pipeline(
        "A = struct { b: B }\n"
        "B = struct { a: A }");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.errors.find("circular"), std::string::npos);
}

TEST(Integration, MissingFrameFile) {
    auto* g = parse("H = struct { x: uint8 }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    ASSERT_TRUE(sema.analyze(g));

    // TypeEmitter with nonexistent frame path should return false
    TypeEmitter types(sema);
    std::ostringstream out;
    EXPECT_FALSE(types.emit_to_frame(g, "/nonexistent/path/Types.frame", out));

    // ParserEmitter with nonexistent frame path should return false
    ParserEmitter pemit(sema, types);
    EXPECT_FALSE(pemit.emit_header_to_frame(g, "/nonexistent/ParserHeader.frame", "Types.h", out));
    EXPECT_FALSE(pemit.emit_impl_to_frame(g, "/nonexistent/ParserImpl.frame", "Parser.h", out));
}

TEST(Integration, HexConstraint) {
    auto r = run_pipeline(
        "Header = struct {\n"
        "    magic: uint32be where magic == 0x89504E47\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Constraint should use unsigned hex, not negative decimal
    EXPECT_NE(r.impls.find("0x89504E47ULL"), std::string::npos)
        << "should emit hex for large uint32 constant";
    EXPECT_EQ(r.impls.find("-1991225785"), std::string::npos)
        << "should not contain negative decimal";
}

TEST(Integration, SwitchNamedRule) {
    auto r = run_pipeline(
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
    ASSERT_TRUE(r.ok) << r.errors;
    // Types: File struct + Payload variant type
    EXPECT_NE(r.types.find("struct File"), std::string::npos);
    EXPECT_NE(r.types.find("struct Payload"), std::string::npos);
    // Implementation: parse_File should call parse_Payload with 3 args
    EXPECT_NE(r.impls.find("parse_Payload(ctx, out.data, out.type)"), std::string::npos);
    // Declaration: parse_Payload should have discriminator param
    EXPECT_NE(r.decls.find("int64_t discriminator"), std::string::npos);
}

TEST(Integration, StringSwitch) {
    auto r = run_pipeline(
        "JsonBody = struct { data: uint8 }\n"
        "XmlBody = struct { data: uint16le }\n"
        "Format = switch(t) {\n"
        "    \"json\": JsonBody;\n"
        "    \"xml\": XmlBody;\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Types should exist
    EXPECT_NE(r.types.find("struct Format"), std::string::npos);
    // Implementation should use if-else, not switch
    EXPECT_NE(r.impls.find("== \"json\""), std::string::npos);
    EXPECT_NE(r.impls.find("== \"xml\""), std::string::npos);
    // Declaration should use const std::string&
    EXPECT_NE(r.decls.find("const std::string&"), std::string::npos);
}

TEST(Integration, RemainingInArray) {
    auto r = run_pipeline(
        "Chunk = struct { data: uint8 }\n"
        "Stream = struct {\n"
        "    chunks: array<Chunk>(none, until(remaining < 1))\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    EXPECT_NE(r.impls.find("ctx.remaining()"), std::string::npos);
    EXPECT_EQ(r.impls.find("out.remaining"), std::string::npos);
}

TEST(Integration, ExternRule) {
    auto r = run_pipeline(
        "Payload = extern(\"decompress_zlib\", \"std::vector<uint8_t>\")");
    ASSERT_TRUE(r.ok) << r.errors;
    // Type: using declaration
    EXPECT_NE(r.types.find("using Payload = std::vector<uint8_t>"), std::string::npos);
    // Declaration
    EXPECT_NE(r.decls.find("parse_Payload"), std::string::npos);
    EXPECT_NE(r.decls.find("std::vector<uint8_t>& out"), std::string::npos);
    // Implementation calls the extern function
    EXPECT_NE(r.impls.find("decompress_zlib(ctx, out)"), std::string::npos);
}

TEST(Integration, ExternInStruct) {
    auto r = run_pipeline(
        "Chunk = struct {\n"
        "    length: uint32le,\n"
        "    data: extern(\"decompress_zlib\", \"std::vector<uint8_t>\")\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Type: struct field with extern type
    EXPECT_NE(r.types.find("std::vector<uint8_t> data"), std::string::npos);
    // Implementation calls the extern function
    EXPECT_NE(r.impls.find("decompress_zlib(ctx, out.data)"), std::string::npos);
}

TEST(Integration, BitfieldInStruct) {
    auto r = run_pipeline(
        "@endian big\n"
        "Header = struct {\n"
        "    flags: bitfield<uint16be> {\n"
        "        version: 4,\n"
        "        ihl: 4,\n"
        "        dscp: 6,\n"
        "        ecn: 2\n"
        "    },\n"
        "    total_length: uint16be\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Types: struct with anonymous struct for bitfield
    EXPECT_NE(r.types.find("struct Header"), std::string::npos);
    EXPECT_NE(r.types.find("version"), std::string::npos);
    EXPECT_NE(r.types.find("uint16_t total_length"), std::string::npos);
    // Parser: bitfield read with shift-mask
    EXPECT_NE(r.impls.find("read_uint16be(_bf)"), std::string::npos);
    EXPECT_NE(r.impls.find("_bf >> 12"), std::string::npos);
}

TEST(Integration, BitfieldRule) {
    auto r = run_pipeline(
        "Flags = bitfield<uint8> {\n"
        "    reserved: 2,\n"
        "    urgent: 1,\n"
        "    ack: 1,\n"
        "    push: 1,\n"
        "    reset: 1,\n"
        "    syn: 1,\n"
        "    fin: 1\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Types: named struct
    EXPECT_NE(r.types.find("struct Flags"), std::string::npos);
    EXPECT_NE(r.types.find("uint8_t reserved"), std::string::npos);
    EXPECT_NE(r.types.find("uint8_t fin"), std::string::npos);
    // Parser function
    EXPECT_NE(r.impls.find("parse_Flags"), std::string::npos);
    EXPECT_NE(r.impls.find("read_uint8(_bf)"), std::string::npos);
}

TEST(Integration, ElementInterval) {
    auto r = run_pipeline(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    num: uint32le,\n"
        "    off: uint32le,\n"
        "    sz: uint32le,\n"
        "    entries: array<Entry>[num] @ [off + i * sz, off + (i + 1) * sz]\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Should have push_interval inside the for loop
    EXPECT_NE(r.impls.find("push_interval"), std::string::npos);
    EXPECT_NE(r.impls.find("elem_scope"), std::string::npos);
}

// --- Integration: nested struct with parent scope references ---

TEST(Integration, LengthPrefixedNestedStruct) {
    auto r = run_pipeline(
        "Packet = struct {\n"
        "    len: uint32le,\n"
        "    body: struct {\n"
        "        data: bytes[len]\n"
        "    }\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Types: named inline struct
    EXPECT_NE(r.types.find("struct Packet_body"), std::string::npos);
    EXPECT_NE(r.types.find("Packet_body body"), std::string::npos);
    // Parser: nested field uses out.body.data, parent ref uses out.len
    EXPECT_NE(r.impls.find("out.len"), std::string::npos);
    EXPECT_NE(r.impls.find("out.body.data"), std::string::npos);
}

TEST(Integration, VersionConditionalNestedStruct) {
    auto r = run_pipeline(
        "Msg = struct {\n"
        "    version: uint16le,\n"
        "    inner: struct {\n"
        "        flags: uint8,\n"
        "        data: uint8 where version >= 2\n"
        "    }\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Types
    EXPECT_NE(r.types.find("struct Msg_inner"), std::string::npos);
    EXPECT_NE(r.types.find("struct Msg"), std::string::npos);
    // Parser: constraint references parent field
    EXPECT_NE(r.impls.find("out.version"), std::string::npos);
    EXPECT_NE(r.impls.find("out.inner.data"), std::string::npos);
    EXPECT_NE(r.impls.find("constraint failed"), std::string::npos);
}

// --- Integration: TIFF-like runtime endian ---

TEST(Integration, TIFFLikeRuntimeEndian) {
    auto r = run_pipeline(
        "TIFF = struct {\n"
        "    byte_order: uint16le,\n"
        "    @endian: byte_order == 0x4949 ? little : big,\n"
        "    magic: uint16,\n"
        "    offset: uint32\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;

    // Types — no @endian field in struct
    EXPECT_NE(r.types.find("uint16_t byte_order"), std::string::npos);
    EXPECT_NE(r.types.find("uint16_t magic"), std::string::npos);
    EXPECT_NE(r.types.find("uint32_t offset"), std::string::npos);

    // Parser — set_endian call and dispatching reads
    EXPECT_NE(r.impls.find("ctx.set_endian("), std::string::npos);
    EXPECT_NE(r.impls.find("read_uint16("), std::string::npos);
    EXPECT_NE(r.impls.find("read_uint32("), std::string::npos);
}

// === Cross-rule scope resolution ===

TEST(Integration, CrossRuleFieldRef) {
    // Full pipeline: cross-rule field reference
    auto r = run_pipeline(
        "Inner = struct {\n"
        "    raw: uint8,\n"
        "    val: compute(raw != 0 ? raw : parent_field : uint8)\n"
        "}\n"
        "Outer = struct {\n"
        "    parent_field: uint8,\n"
        "    child: Inner\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Types should exist
    EXPECT_NE(r.types.find("struct Inner"), std::string::npos);
    EXPECT_NE(r.types.find("struct Outer"), std::string::npos);
    // ScopeGuard wraps the parse_Inner call
    EXPECT_NE(r.impls.find("ScopeGuard"), std::string::npos);
    // parse_Inner exists
    EXPECT_NE(r.impls.find("parse_Inner"), std::string::npos);
}

TEST(Integration, CrossRuleArrayIndex) {
    // Full pipeline: events[i-1].field pattern
    auto r = run_pipeline(
        "MidiEvent = struct {\n"
        "    raw_status: uint8,\n"
        "    effective: compute(i > 0 ? raw_status : 0 : uint8)\n"
        "}\n"
        "MidiTrack = struct {\n"
        "    events: array<MidiEvent>(none, eof)\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    // Types
    EXPECT_NE(r.types.find("struct MidiEvent"), std::string::npos);
    EXPECT_NE(r.types.find("struct MidiTrack"), std::string::npos);
    // parse_MidiEvent uses ctx.loop_index() for 'i'
    EXPECT_NE(r.impls.find("ctx.loop_index()"), std::string::npos);
    // LoopGuard in array loop
    EXPECT_NE(r.impls.find("LoopGuard"), std::string::npos);
    // ScopeGuard wraps parse_MidiEvent call
    EXPECT_NE(r.impls.find("ScopeGuard"), std::string::npos);
}

TEST(Integration, MixedExplicitAndRuntimeEndian) {
    auto r = run_pipeline(
        "H = struct {\n"
        "    marker: uint16le,\n"
        "    @endian: marker == 0xFF ? little : big,\n"
        "    a: uint32,\n"
        "    b: uint32be\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;

    // Explicit le field unchanged
    EXPECT_NE(r.impls.find("read_uint16le(out.marker)"), std::string::npos);
    // Default-endian uses dispatching
    EXPECT_NE(r.impls.find("read_uint32(out.a)"), std::string::npos);
    // Explicit be field unchanged
    EXPECT_NE(r.impls.find("read_uint32be(out.b)"), std::string::npos);
}

TEST(Integration, PeekInUntilArray) {
    auto r = run_pipeline(
        "Msg = struct {\n"
        "    data: array<uint8>(none, until(peek() == 0)),\n"
        "    null: uint8\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    EXPECT_NE(r.impls.find("ctx.peek()"), std::string::npos);
}

TEST(Integration, PeekInWhereConstraint) {
    auto r = run_pipeline(
        "Msg = struct {\n"
        "    tag: uint8 where peek() == 0xFF\n"
        "}");
    ASSERT_TRUE(r.ok) << r.errors;
    EXPECT_NE(r.impls.find("ctx.peek()"), std::string::npos);
}
