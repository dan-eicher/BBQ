#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include "Parser.h"
#include "BBQ_AST.h"
#include "Sema.h"
#include "TypeEmitter.h"
#include "ParserEmitter.h"

using namespace BBQ;
using namespace bbqgen;

// Preamble that the generated code needs to compile
static const char* PREAMBLE =
    "#include <cstdint>\n"
    "#include <variant>\n"
    "#include <vector>\n"
    "#include <optional>\n"
    "#include <string>\n"
    "#include <cstring>\n"
    "#include \"BBQContext.h\"\n"
    "\n";

static Grammar* do_parse(const char* src) {
    auto* parser = new Parser();
    parser->init(src, static_cast<int>(strlen(src)));
    if (!parser->parse()) return nullptr;
    return parser->ast;
}

// Run the full pipeline and return combined C++ source that should compile
static bool compiles(const char* src, const char* extra_preamble = nullptr) {
    auto* g = do_parse(src);
    if (!g) return false;

    ErrorReporter errors;
    Sema sema(errors);
    if (!sema.analyze(g)) return false;

    TypeEmitter types(sema);
    std::string type_code = types.emit(g);

    ParserEmitter pemit(sema, types);
    std::string decl_code = pemit.emit_decls(g);
    std::string impl_code = pemit.emit_impls(g);

    // Combine into a single translation unit
    std::string combined;
    combined += PREAMBLE;
    if (extra_preamble) combined += extra_preamble;
    combined += type_code;
    combined += "\n";
    combined += decl_code;
    combined += "\n";
    combined += impl_code;

    // Write to temp file
    char tmppath[] = "/tmp/bbq_compile_XXXXXX.cpp";
    int fd = mkstemps(tmppath, 4);
    if (fd < 0) return false;

    FILE* fp = fdopen(fd, "w");
    if (!fp) { close(fd); return false; }
    fputs(combined.c_str(), fp);
    fclose(fp);

    // Run g++ syntax check
    std::string cmd = "g++ -std=c++17 -fsyntax-only -I ";
    cmd += SOURCE_DIR;
    cmd += "/backends/cpp/runtime -I ";
    cmd += SOURCE_DIR;
    cmd += "/frontend ";
    cmd += tmppath;
    cmd += " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) { unlink(tmppath); return false; }

    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        output += buf;

    int status = pclose(pipe);
    unlink(tmppath);

    if (status != 0) {
        // Print diagnostics for debugging
        fprintf(stderr, "--- Compilation failed ---\n%s\n--- Source ---\n%s\n",
                output.c_str(), combined.c_str());
    }

    return status == 0;
}

TEST(CodegenCompile, SimpleStruct) {
    EXPECT_TRUE(compiles(
        "Header = struct {\n"
        "    magic: uint32be,\n"
        "    version: uint16le,\n"
        "    flags: uint8\n"
        "}"));
}

TEST(CodegenCompile, UnionBacktracking) {
    EXPECT_TRUE(compiles(
        "Text = struct { marker: uint8 }\n"
        "Bin = struct { marker: uint8, len: uint16le }\n"
        "Msg = union {\n"
        "    text: Text,\n"
        "    binary: Bin\n"
        "}"));
}

TEST(CodegenCompile, SwitchIntCases) {
    EXPECT_TRUE(compiles(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "P = switch(t) {\n"
        "    1: Text;\n"
        "    2: Bin;\n"
        "    default: Text;\n"
        "}"));
}

TEST(CodegenCompile, SwitchStringCases) {
    EXPECT_TRUE(compiles(
        "JsonBody = struct { data: uint8 }\n"
        "XmlBody = struct { data: uint16le }\n"
        "Format = switch(t) {\n"
        "    \"json\": JsonBody;\n"
        "    \"xml\": XmlBody;\n"
        "    default: JsonBody;\n"
        "}"));
}

TEST(CodegenCompile, SwitchIdentCases) {
    EXPECT_TRUE(compiles(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "P = switch(t) {\n"
        "    FOO: Text;\n"
        "    BAR: Bin;\n"
        "}",
        "constexpr int64_t FOO = 1;\nconstexpr int64_t BAR = 2;\n"));
}

TEST(CodegenCompile, SwitchNamedFromStruct) {
    EXPECT_TRUE(compiles(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "Payload = switch(type) {\n"
        "    1: Text;\n"
        "    2: Bin;\n"
        "}\n"
        "File = struct {\n"
        "    type: uint8,\n"
        "    data: Payload\n"
        "}"));
}

TEST(CodegenCompile, InlineSwitchInStruct) {
    EXPECT_TRUE(compiles(
        "Text = struct { data: uint8 }\n"
        "Bin = struct { data: uint16le }\n"
        "Msg = struct {\n"
        "    type: uint8,\n"
        "    payload: switch(type) { 1: Text; 2: Bin; }\n"
        "}"));
}

// Regression: an inline switch must use break (not return) so that
// fields declared after the switch are still parsed.
TEST(CodegenCompile, InlineSwitchWithTrailingFields) {
    EXPECT_TRUE(compiles(
        "HeaderA = struct { a: uint8 }\n"
        "HeaderB = struct { b: uint16be }\n"
        "Packet = struct {\n"
        "    tag: uint8,\n"
        "    header: switch(tag) { 1: HeaderA; 2: HeaderB; },\n"
        "    payload: uint32be\n"
        "}"));
}

TEST(CodegenCompile, ArrayEof) {
    EXPECT_TRUE(compiles(
        "Entry = struct { x: uint8 }\n"
        "Items = array<Entry>(none, eof)"));
}

TEST(CodegenCompile, ArrayCountWithSep) {
    EXPECT_TRUE(compiles(
        "Comma = struct { sep: uint8 }\n"
        "Item = struct { val: uint8 }\n"
        "Items = array<Item>(Comma, count(5))"));
}

TEST(CodegenCompile, OptionalTryRestore) {
    EXPECT_TRUE(compiles(
        "Ext = struct { x: uint8 }\n"
        "MaybeExt = optional<Ext>"));
}

TEST(CodegenCompile, ComputeFields) {
    EXPECT_TRUE(compiles(
        "H = struct {\n"
        "    ver_ihl: uint8,\n"
        "    version: compute((ver_ihl >> 4) & 0x0F : uint8),\n"
        "    ihl: compute(ver_ihl & 0x0F : uint8)\n"
        "}"));
}

TEST(CodegenCompile, BytesAndString) {
    EXPECT_TRUE(compiles(
        "Record = struct {\n"
        "    len: uint16le,\n"
        "    data: bytes[len],\n"
        "    tag: string[4]\n"
        "}"));
}

TEST(CodegenCompile, IntervalStartEnd) {
    EXPECT_TRUE(compiles(
        "Header = struct { x: uint32be }\n"
        "F = struct { hdr: Header[0, 64] }"));
}

TEST(CodegenCompile, RemainingInUntil) {
    EXPECT_TRUE(compiles(
        "Pkt = struct { x: uint8 }\n"
        "Pkts = array<Pkt>(none, until(remaining < 4))"));
}

TEST(CodegenCompile, PNGLikeFormat) {
    EXPECT_TRUE(compiles(
        "@endian big\n"
        "Header = struct {\n"
        "    magic: uint32 where magic == 0x89504E47,\n"
        "    version: uint32\n"
        "}\n"
        "Chunk = struct {\n"
        "    length: uint32,\n"
        "    ctype: uint32,\n"
        "    data: bytes[length],\n"
        "    crc: uint32\n"
        "}\n"
        "File = struct {\n"
        "    header: Header,\n"
        "    chunks: array<Chunk>(none, eof)\n"
        "}"));
}

TEST(CodegenCompile, TLVWithDispatch) {
    EXPECT_TRUE(compiles(
        "@endian little\n"
        "TextData = struct { data: uint8 }\n"
        "BinData = struct { data: uint16 }\n"
        "Entry = struct {\n"
        "    type: uint8,\n"
        "    length: uint16,\n"
        "    value: switch(type) { 1: TextData; 2: BinData; default: TextData; }\n"
        "}\n"
        "Stream = struct {\n"
        "    entries: array<Entry>(none, eof)\n"
        "}"));
}

TEST(CodegenCompile, NestedStructs) {
    EXPECT_TRUE(compiles(
        "Inner = struct { x: uint8 }\n"
        "Middle = struct { inner: Inner, y: uint16le }\n"
        "Outer = struct { mid: Middle, z: uint32be }"));
}

TEST(CodegenCompile, ExternType) {
    EXPECT_TRUE(compiles(
        "Payload = extern(\"decompress_zlib\", \"std::vector<uint8_t>\")",
        "bool decompress_zlib(bbq::BBQContext& ctx, std::vector<uint8_t>& out) { return true; }\n"));
}

TEST(CodegenCompile, BitfieldBE) {
    EXPECT_TRUE(compiles(
        "@endian big\n"
        "Header = struct {\n"
        "    flags: bitfield<uint16be> {\n"
        "        version: 4,\n"
        "        ihl: 4,\n"
        "        dscp: 6,\n"
        "        ecn: 2\n"
        "    },\n"
        "    total_length: uint16be\n"
        "}"));
}

TEST(CodegenCompile, BitfieldLE) {
    EXPECT_TRUE(compiles(
        "Flags = bitfield<uint16le> {\n"
        "    a: 5,\n"
        "    b: 6,\n"
        "    c: 5\n"
        "}"));
}

TEST(CodegenCompile, BitfieldStandalone) {
    EXPECT_TRUE(compiles(
        "TCPFlags = bitfield<uint8> {\n"
        "    reserved: 2,\n"
        "    urgent: 1,\n"
        "    ack: 1,\n"
        "    push: 1,\n"
        "    reset: 1,\n"
        "    syn: 1,\n"
        "    fin: 1\n"
        "}"));
}

TEST(CodegenCompile, ElementInterval) {
    EXPECT_TRUE(compiles(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    num: uint32le,\n"
        "    off: uint32le,\n"
        "    sz: uint32le,\n"
        "    entries: array<Entry>[num] @ [off + i * sz, off + (i + 1) * sz]\n"
        "}"));
}

TEST(CodegenCompile, RuntimeEndianSwitch) {
    EXPECT_TRUE(compiles(
        "TIFF = struct {\n"
        "    byte_order: uint16le,\n"
        "    @endian: byte_order == 0x4949 ? little : big,\n"
        "    magic: uint16,\n"
        "    offset: uint32\n"
        "}"));
}
