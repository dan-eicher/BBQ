#include <gtest/gtest.h>
#include "Parser.h"
#include "BBQ_AST.h"
#include "Sema.h"
#include "CTypeEmitter.h"
#include "CReaderEmitter.h"
#include "CWriterEmitter.h"

using namespace BBQ;
using namespace bbqgen;
using namespace bbqgen_c;

static Grammar* parse(const char* src) {
    auto* parser = new Parser();
    parser->init(src, static_cast<int>(strlen(src)));
    if (!parser->parse()) return nullptr;
    return parser->ast;
}

static Sema* make_sema(const char* src, Grammar** g_out) {
    auto* g = parse(src);
    if (!g) return nullptr;
    auto* errors = new ErrorReporter();
    auto* sema = new Sema(*errors);
    if (!sema->analyze(g)) { delete sema; delete errors; return nullptr; }
    if (g_out) *g_out = g;
    return sema;
}

static std::string emit_c_reader(const char* src) {
    Grammar* g;
    auto* sema = make_sema(src, &g);
    if (!sema) return "";
    CTypeEmitter types(*sema);
    types.emit(g);
    CReaderEmitter reader(*sema, types);
    return reader.emit_impls(g);
}

static std::string emit_c_writer(const char* src) {
    Grammar* g;
    auto* sema = make_sema(src, &g);
    if (!sema) return "";
    CTypeEmitter types(*sema);
    types.emit(g);
    CWriterEmitter writer(*sema, types);
    return writer.emit_impls(g);
}

static std::string emit_c_types(const char* src) {
    auto* g = parse(src);
    if (!g) return "";
    ErrorReporter errors;
    Sema sema(errors);
    if (!sema.analyze(g)) return "";
    CTypeEmitter emitter(sema);
    return emitter.emit(g);
}

// ── Snake case conversion ──

TEST(CSnakeCase, Simple) {
    EXPECT_EQ(to_snake_case("FooBar"), "foo_bar");
    EXPECT_EQ(to_snake_case("simple"), "simple");
    EXPECT_EQ(to_snake_case("ABCDef"), "abc_def");
    EXPECT_EQ(to_snake_case("CapFile"), "cap_file");
    EXPECT_EQ(to_snake_case("MethodInfo"), "method_info");
    EXPECT_EQ(to_snake_case("U16Value"), "u16_value");
}

// ── Struct ──

TEST(CTypeEmitter, SimpleStruct) {
    auto out = emit_c_types("Foo = struct { x: uint8, y: uint32be }");
    EXPECT_NE(out.find("struct foo"), std::string::npos);
    EXPECT_NE(out.find("uint8_t x"), std::string::npos);
    EXPECT_NE(out.find("uint32_t y"), std::string::npos);
    // Should NOT have std::variant or std::vector
    EXPECT_EQ(out.find("std::"), std::string::npos);
}

// ── Union → tagged union ──

TEST(CTypeEmitter, UnionType) {
    auto out = emit_c_types(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "Msg = union {\n"
        "    text: Text,\n"
        "    binary: Bin\n"
        "}");
    EXPECT_NE(out.find("struct msg"), std::string::npos);
    EXPECT_NE(out.find("enum msg_tag"), std::string::npos);
    EXPECT_NE(out.find("union"), std::string::npos);
    // No std::variant
    EXPECT_EQ(out.find("std::variant"), std::string::npos);
}

// ── Array → pointer + count ──

TEST(CTypeEmitter, ArrayType) {
    auto out = emit_c_types("Colors = array<uint8>[3]");
    EXPECT_NE(out.find("uint8_t* items"), std::string::npos);
    EXPECT_NE(out.find("size_t count"), std::string::npos);
    // No std::vector
    EXPECT_EQ(out.find("std::vector"), std::string::npos);
}

// ── Optional → has_value + value ──

TEST(CTypeEmitter, OptionalType) {
    auto out = emit_c_types("MaybeU8 = optional<uint8>");
    EXPECT_NE(out.find("bool has_value"), std::string::npos);
    EXPECT_NE(out.find("uint8_t value"), std::string::npos);
    EXPECT_EQ(out.find("std::optional"), std::string::npos);
}

// ── Primitive typedef ──

TEST(CTypeEmitter, PrimitiveTypedef) {
    auto out = emit_c_types("MyByte = uint8");
    EXPECT_NE(out.find("typedef uint8_t my_byte_t"), std::string::npos);
}

// ── Bytes and string ──

TEST(CTypeEmitter, BytesType) {
    auto out = emit_c_types("Payload = struct { data: bytes[10] }");
    EXPECT_NE(out.find("bbq_bytes_t data"), std::string::npos);
}

// ── Bitfield ──

TEST(CTypeEmitter, BitfieldType) {
    auto out = emit_c_types("Flags = bitfield<uint8> { a: 4, b: 4 }");
    EXPECT_NE(out.find("uint8_t a"), std::string::npos);
    EXPECT_NE(out.find("uint8_t b"), std::string::npos);
}

// ── Compute fields included ──

TEST(CTypeEmitter, ComputeFieldIncluded) {
    auto out = emit_c_types(
        "Hdr = struct {\n"
        "    raw: uint8,\n"
        "    version: compute((raw >> 4) & 0x0F : uint8)\n"
        "}");
    EXPECT_NE(out.find("uint8_t raw"), std::string::npos);
    EXPECT_NE(out.find("uint8_t version"), std::string::npos);
}

// ── Naming conventions ──

TEST(CTypeEmitter, NamingConventions) {
    auto out = emit_c_types("FooBar = struct { x: uint8 }");
    // Type name is snake_case_t
    EXPECT_NE(out.find("foo_bar_t"), std::string::npos);
}

// ── Prefix support ──

static std::string emit_c_types_prefixed(const char* src, const std::string& prefix) {
    auto* g = parse(src);
    if (!g) return "";
    ErrorReporter errors;
    Sema sema(errors);
    if (!sema.analyze(g)) return "";
    CTypeEmitter emitter(sema, prefix);
    return emitter.emit(g);
}

static std::string emit_c_reader_prefixed(const char* src, const std::string& prefix) {
    Grammar* g;
    auto* sema = make_sema(src, &g);
    if (!sema) return "";
    CTypeEmitter types(*sema, prefix);
    types.emit(g);
    CReaderEmitter reader(*sema, types);
    return reader.emit_impls(g);
}

static std::string emit_c_writer_prefixed(const char* src, const std::string& prefix) {
    Grammar* g;
    auto* sema = make_sema(src, &g);
    if (!sema) return "";
    CTypeEmitter types(*sema, prefix);
    types.emit(g);
    CWriterEmitter writer(*sema, types);
    return writer.emit_impls(g);
}

TEST(CTypeEmitter, PrefixAppliedToTypeNames) {
    auto out = emit_c_types_prefixed("Foo = struct { x: uint8 }", "DNS");
    EXPECT_NE(out.find("dns_foo_t"), std::string::npos);
    EXPECT_NE(out.find("struct dns_foo"), std::string::npos);
    // No unprefixed type
    EXPECT_EQ(out.find("typedef struct foo "), std::string::npos);
}

TEST(CTypeEmitter, PrefixAppliedToUnionTags) {
    auto out = emit_c_types_prefixed(
        "A = struct { x: uint8 }\n"
        "B = struct { y: uint16le }\n"
        "Msg = union { a: A, b: B }", "Net");
    EXPECT_NE(out.find("enum net_msg_tag"), std::string::npos);
    EXPECT_NE(out.find("struct net_msg"), std::string::npos);
}

TEST(CTypeEmitter, PrefixAppliedToFreeFunc) {
    auto out = emit_c_types_prefixed(
        "Item = struct { val: uint8 }\n"
        "List = struct { items: array<Item>[3] }", "Pkt");
    EXPECT_NE(out.find("pkt_list_free"), std::string::npos);
    EXPECT_EQ(out.find("\nstatic inline void list_free"), std::string::npos);
}

TEST(CReaderEmitter, PrefixAppliedToReadFunc) {
    auto out = emit_c_reader_prefixed("Foo = struct { x: uint8 }", "DNS");
    EXPECT_NE(out.find("dns_foo_read"), std::string::npos);
    EXPECT_EQ(out.find("\nbool foo_read"), std::string::npos);
}

TEST(CWriterEmitter, PrefixAppliedToWriteFunc) {
    auto out = emit_c_writer_prefixed("Foo = struct { x: uint8 }", "DNS");
    EXPECT_NE(out.find("dns_foo_write"), std::string::npos);
    EXPECT_EQ(out.find("\nbool foo_write"), std::string::npos);
}

TEST(CTypeEmitter, EmptyPrefixUnchanged) {
    auto out = emit_c_types_prefixed("Foo = struct { x: uint8 }", "");
    EXPECT_NE(out.find("foo_t"), std::string::npos);
    EXPECT_EQ(out.find("_foo_t"), std::string::npos);
}

// ── Reader tests ──────────────────────────────────────────

TEST(CReaderEmitter, StructReader) {
    auto out = emit_c_reader("Foo = struct { x: uint8, y: uint16be }");
    EXPECT_NE(out.find("foo_read"), std::string::npos);
    EXPECT_NE(out.find("bbq_read_u8"), std::string::npos);
    EXPECT_NE(out.find("bbq_read_u16be"), std::string::npos);
    // C-style pointers, not C++ references
    EXPECT_NE(out.find("bbq_ctx_t* ctx"), std::string::npos);
    EXPECT_EQ(out.find("BBQContext"), std::string::npos);
}

TEST(CReaderEmitter, ArrayReader) {
    auto out = emit_c_reader(
        "Item = struct { val: uint8 }\n"
        "List = struct { count: uint8, items: array<Item>[count] }");
    EXPECT_NE(out.find("list_read"), std::string::npos);
    EXPECT_NE(out.find("calloc"), std::string::npos);
    EXPECT_NE(out.find("bbq_push_loop"), std::string::npos);
}

TEST(CReaderEmitter, BitfieldReader) {
    auto out = emit_c_reader("@endian big\nFlags = bitfield<uint8> { a: 4, b: 4 }");
    EXPECT_NE(out.find("_bf >> "), std::string::npos);
    EXPECT_NE(out.find("& 0xF"), std::string::npos);
}

TEST(CReaderEmitter, ComputeField) {
    auto out = emit_c_reader(
        "Hdr = struct {\n"
        "    raw: uint8,\n"
        "    version: compute((raw >> 4) & 0x0F : uint8)\n"
        "}");
    EXPECT_NE(out.find("out->version ="), std::string::npos);
    EXPECT_NE(out.find("out->raw >> 4"), std::string::npos);
}

// ── Writer tests ──────────────────────────────────────────

TEST(CWriterEmitter, StructWriter) {
    auto out = emit_c_writer("Foo = struct { x: uint8, y: uint16be }");
    EXPECT_NE(out.find("foo_write"), std::string::npos);
    EXPECT_NE(out.find("bbq_write_u8"), std::string::npos);
    EXPECT_NE(out.find("bbq_write_u16be"), std::string::npos);
    EXPECT_NE(out.find("bbq_write_ctx_t* ctx"), std::string::npos);
}

TEST(CWriterEmitter, ComputeFieldSkipped) {
    auto out = emit_c_writer(
        "Hdr = struct {\n"
        "    raw: uint8,\n"
        "    version: compute((raw >> 4) & 0x0F : uint8)\n"
        "}");
    // Writer should write raw but NOT version (it's computed)
    EXPECT_NE(out.find("in->raw"), std::string::npos);
    EXPECT_EQ(out.find("in->version"), std::string::npos);
}

TEST(CWriterEmitter, BitfieldWriter) {
    auto out = emit_c_writer("@endian big\nFlags = bitfield<uint8> { a: 4, b: 4 }");
    EXPECT_NE(out.find("_bf |="), std::string::npos);
    EXPECT_NE(out.find("bbq_write_u8"), std::string::npos);
}

TEST(CWriterEmitter, ArrayWriter) {
    auto out = emit_c_writer("Items = array<uint8>[3]");
    EXPECT_NE(out.find("in->count"), std::string::npos);
    EXPECT_NE(out.find("for (size_t i"), std::string::npos);
}
