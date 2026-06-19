// c_writer_emitter_test — fast unit pins for the C owning WRITER (the burg-driven
// OwningCWriter via render_writer_c) + the canonical snake_case name mapping. These are
// quick generator-output checks; the comprehensive behavioral net (every rule round-trips
// == CEK, value-checked) lives in cross_backend_test.
#include <gtest/gtest.h>
#include "Parser.h"
#include "Sema.h"
#include "Names.h"
#include "CompilerCtx.h"
#include "RenderEmit.h"
#include "bbq_compile.h"
#include <cstring>
#include <string>

using namespace BBQ;
using namespace bbqgen;

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

static std::string emit_c_writer(const char* src, const std::string& prefix = "") {
    Grammar* g;
    auto* sema = make_sema(src, &g);
    if (!sema) return "";
    bbq::render::CompilerCtx ctx{g, sema, nullptr, prefix};
    auto* cc = new bbq::Compiler();
    ctx.ir = cc->compile_grammar(g);
    return bbq::render::render_writer_c(
        ctx, std::string(SOURCE_DIR) + "/backends/render/templates");
}

// ── Snake case conversion (the canonical name mapping burg/templates rely on) ──

TEST(CSnakeCase, Simple) {
    EXPECT_EQ(to_snake_case("FooBar"), "foo_bar");
    EXPECT_EQ(to_snake_case("simple"), "simple");
    EXPECT_EQ(to_snake_case("ABCDef"), "abc_def");
    EXPECT_EQ(to_snake_case("CapFile"), "cap_file");
    EXPECT_EQ(to_snake_case("MethodInfo"), "method_info");
    EXPECT_EQ(to_snake_case("U16Value"), "u16_value");
}

// ── Writer (render_writer_c) ──

TEST(CWriter, PrefixAppliedToWriteFunc) {
    auto out = emit_c_writer("Foo = struct { x: uint8 }", "DNS");
    EXPECT_NE(out.find("dns_foo_write"), std::string::npos);
    EXPECT_EQ(out.find("bool foo_write("), std::string::npos);  // unprefixed must not appear
}

TEST(CWriter, StructWriter) {
    auto out = emit_c_writer("Foo = struct { x: uint8, y: uint16be }");
    EXPECT_NE(out.find("foo_write"), std::string::npos);
    EXPECT_NE(out.find("bbq_write_u8"), std::string::npos);
    EXPECT_NE(out.find("bbq_write_u16be"), std::string::npos);
    EXPECT_NE(out.find("bbq_write_ctx_t* ctx"), std::string::npos);
    EXPECT_NE(out.find("in->x"), std::string::npos);
    EXPECT_NE(out.find("in->y"), std::string::npos);
}

TEST(CWriter, ComputeFieldSkipped) {
    auto out = emit_c_writer(
        "Hdr = struct {\n"
        "    raw: uint8,\n"
        "    version: compute((raw >> 4) & 0x0F : uint8)\n"
        "}");
    // The stored field is written; the computed field is derived on read, never written.
    EXPECT_NE(out.find("in->raw"), std::string::npos);
    EXPECT_EQ(out.find("in->version"), std::string::npos);
}

TEST(CWriter, BitfieldWriter) {
    auto out = emit_c_writer("@endian big\nFlags = bitfield<uint8> { a: 4, b: 4 }");
    EXPECT_NE(out.find("_bf |="), std::string::npos);     // container assembled by shift/mask
    EXPECT_NE(out.find("bbq_write_u8"), std::string::npos);
}

TEST(CWriter, ArrayWriter) {
    auto out = emit_c_writer("Items = array<uint8>[3]");
    // Structure-driven loop over the stored count (no checkpoint/grow), element write.
    EXPECT_NE(out.find("bbq_w_loop_next"), std::string::npos);
    EXPECT_NE(out.find("bbq_write_u8"), std::string::npos);
}

TEST(CWriter, SwitchScalarArmTargetsUnionMember) {
    // The scalar arm (case 1) must write the canonical u.case_0 union member — the burg
    // naming fix; a bare `in->body` (the old defect) would not compile against the type.
    auto out = emit_c_writer(
        "S = struct { tag: uint8, body: switch(tag) { 1: uint16le; default: uint8; } }");
    EXPECT_NE(out.find("in->body.u.case_0"), std::string::npos);
    EXPECT_NE(out.find("in->body.u.default_val"), std::string::npos);
    EXPECT_NE(out.find("switch ((int64_t)(in->body.tag))"), std::string::npos);
}
