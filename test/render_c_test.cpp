// render_c_test — end-to-end for the destination-driven C reader generator
// (backends/render). For each spec: compile the kont graph, render the reader
// via render_reader_c, generate the struct types via render_types_c,
// compile the result -Werror, run it on test bytes, and check the parse.
//
// This is the growing acceptance suite for the new reader: each construct the
// lowering learns moves a case from "unsupported" to "tested" here, and the
// gate is behavioral (compile -Werror + parse), matching c_backend_e2e.
#include <gtest/gtest.h>
#include "Parser.h"
#include "Sema.h"
#include "CompilerCtx.h"
#include "RenderTypes.h"
#include "bbq_compile.h"
#include "RenderEmit.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

using namespace BBQ;
using namespace bbqgen;

namespace {

// Compile a spec to a reader, build a main that parses `bytes` into `type_name`
// via `fn_name`, compile -Werror, run. Returns true iff the binary exits 0.
// `check` is a C boolean expression over `out` (only evaluated when the parse
// is expected to succeed). `want_parse_ok` lets a `where` rule assert rejection.
bool run(const std::string& spec, const std::string& type_name, const std::string& fn_name,
         const std::vector<uint8_t>& bytes, const std::string& check, bool want_parse_ok,
         std::string* err, const std::string& prelude = "") {
    auto* parser = new Parser();
    parser->init(spec.c_str(), (int)spec.size());
    if (!parser->parse()) { if (err) *err = "parse failed"; return false; }
    ErrorReporter rep;
    Sema sema(rep);
    if (!sema.analyze(parser->ast)) { if (err) *err = "sema failed"; return false; }

    ::bbq::Compiler compiler;
    auto* g = compiler.compile_grammar(parser->ast);
    if (!g) { if (err) *err = "compile_grammar null"; return false; }

    bbq::render::CompilerCtx ctx{parser->ast, &sema, g, ""};
    std::string tmpl = std::string(SOURCE_DIR) + "/backends/render/templates";
    std::string reader = bbq::render::render_reader_c(ctx, tmpl);
    std::string decls = bbq::render::render_reader_decls(ctx);

    std::string types_tmpl = std::string(SOURCE_DIR) + "/backends/render/templates/types_c.inja";
    std::string types_h = bbq::render::render_types_c(ctx, types_tmpl);

    std::string dir = "/tmp/bbq_render_test_" + std::to_string(getpid());
    system(("mkdir -p " + dir).c_str());
    { std::ofstream f(dir + "/testTypes.h"); f << types_h; }
    {
        std::ofstream f(dir + "/test.c");
        f << "#include \"testTypes.h\"\n#include \"bbq_runtime.h\"\n#include <stdlib.h>\n#include <string.h>\n";
        if (!prelude.empty()) f << prelude << "\n";
        f << decls << reader;
        f << "int main(void){\n  static const uint8_t buf[] = {";
        for (size_t i = 0; i < bytes.size(); i++) f << (int)bytes[i] << (i + 1 < bytes.size() ? "," : "");
        f << "};\n  bbq_ctx_t ctx; bbq_ctx_init(&ctx, buf, sizeof buf);\n";
        f << "  " << type_name << " out; memset(&out, 0, sizeof out);\n";
        f << "  bool ok = " << fn_name << "(&ctx, &out);\n";
        f << "  if (ok != " << (want_parse_ok ? "true" : "false") << ") return 1;\n";
        if (want_parse_ok) f << "  if (!(" << check << ")) return 2;\n";
        f << "  return 0;\n}\n";
    }
    std::string rt = std::string(SOURCE_DIR) + "/backends/c/runtime";
    std::string cc = "cc -Wall -Wextra -Werror -std=c11 -I" + dir + " -I" + rt + " " +
                     dir + "/test.c -o " + dir + "/bin 2>&1";
    FILE* p = popen(cc.c_str(), "r");
    char buf[8192] = {}; if (p) fread(buf, 1, sizeof buf - 1, p);
    int rc = p ? pclose(p) : -1;
    if (rc != 0) { if (err) *err = std::string("compile failed:\n") + buf; return false; }

    int run_rc = system((dir + "/bin").c_str());
    system(("rm -rf " + dir).c_str());
    if (run_rc != 0) { if (err) *err = "run failed rc=" + std::to_string(run_rc); return false; }
    return true;
}

}  // namespace

TEST(RenderC, FlatStruct) {
    std::string err;
    EXPECT_TRUE(run("Sample = struct { x: uint8, y: uint16le }", "sample_t", "sample_read",
                    {5, 2, 0}, "out.x == 5 && out.y == 2", true, &err)) << err;
}

TEST(RenderC, WhereAccepts) {
    std::string err;
    EXPECT_TRUE(run("Magic = struct { ver: uint8 where ver == 1 }", "magic_t", "magic_read",
                    {1}, "out.ver == 1", true, &err)) << err;
}

TEST(RenderC, WhereRejects) {
    std::string err;
    EXPECT_TRUE(run("Magic = struct { ver: uint8 where ver == 1 }", "magic_t", "magic_read",
                    {2}, "", false, &err)) << err;
}

TEST(RenderC, NestedStruct) {
    std::string err;
    EXPECT_TRUE(run("Nested = struct { hdr: struct { a: uint8 }, b: uint8 }", "nested_t", "nested_read",
                    {7, 9}, "out.hdr.a == 7 && out.b == 9", true, &err)) << err;
}

TEST(RenderC, RuleRef) {
    std::string err;
    EXPECT_TRUE(run("Inner = struct { a: uint8 }\nOuter = struct { tag: uint8, inner: Inner }",
                    "outer_t", "outer_read", {3, 7}, "out.tag == 3 && out.inner.a == 7", true, &err)) << err;
}

TEST(RenderC, ArrayOfRule) {
    std::string err;
    EXPECT_TRUE(run("Item = struct { val: uint8 }\nList = struct { count: uint8, items: array<Item>[count] }",
                    "list_t", "list_read", {2, 10, 20},
                    "out.count == 2 && out.items.count == 2 && "
                    "out.items.items[0].val == 10 && out.items.items[1].val == 20", true, &err)) << err;
}

TEST(RenderC, Switch) {
    std::string err;
    EXPECT_TRUE(run("TypeA = struct { a: uint8 }\nTypeB = struct { b: uint16le }\n"
                    "Msg = struct { tag: uint8, body: switch(tag) { 1: TypeA; 2: TypeB; } }",
                    "msg_t", "msg_read", {1, 42},
                    "out.tag == 1 && out.body.tag == 1 && out.body.u.case_0.a == 42", true, &err)) << err;
}

TEST(RenderC, Bitfield) {
    std::string err;
    EXPECT_TRUE(run("@endian big\nFlags = bitfield<uint8> { high: 4, low: 4 }", "flags_t", "flags_read",
                    {0x12}, "out.high == 1 && out.low == 2", true, &err)) << err;
}

TEST(RenderC, OptionalPresent) {
    std::string err;
    EXPECT_TRUE(run("Opt = struct { flag: uint8, extra: optional<uint16le> where flag != 0 }",
                    "opt_t", "opt_read", {1, 0x34, 0x12},
                    "out.flag == 1 && out.extra.has_value && out.extra.value == 0x1234", true, &err)) << err;
}

TEST(RenderC, OptionalAbsent) {
    std::string err;
    EXPECT_TRUE(run("Opt = struct { flag: uint8, extra: optional<uint16le> where flag != 0 }",
                    "opt_t", "opt_read", {0},
                    "out.flag == 0 && !out.extra.has_value", true, &err)) << err;
}

TEST(RenderC, ArrayOfPrim) {
    std::string err;
    EXPECT_TRUE(run("P = struct { n: uint8, xs: array<uint8>[n] }", "p_t", "p_read",
                    {3, 11, 22, 33},
                    "out.n == 3 && out.xs.count == 3 && out.xs.items[0] == 11 && "
                    "out.xs.items[1] == 22 && out.xs.items[2] == 33", true, &err)) << err;
}

TEST(RenderC, PosBuiltin) {
    std::string err;
    EXPECT_TRUE(run("Q = struct { a: uint8, b: uint8 where @pos == 2 }", "q_t", "q_read",
                    {5, 6}, "out.a == 5 && out.b == 6", true, &err)) << err;
}

TEST(RenderC, BytesField) {
    std::string err;
    EXPECT_TRUE(run("B = struct { n: uint8, data: bytes[n] }", "b_t", "b_read",
                    {3, 'a', 'b', 'c'},
                    "out.n == 3 && out.data.length == 3 && out.data.data[0] == 'a' && out.data.data[2] == 'c'",
                    true, &err)) << err;
}

TEST(RenderC, Compute) {
    std::string err;
    EXPECT_TRUE(run("C = struct { raw: uint8, version: compute((raw >> 4) & 0x0F : uint8), ihl: compute(raw & 0x0F : uint8) }",
                    "c_t", "c_read", {0x12},
                    "out.raw == 0x12 && out.version == 1 && out.ihl == 2", true, &err)) << err;
}

TEST(RenderC, EndianBig) {
    std::string err;
    EXPECT_TRUE(run("@endian big\nH = struct { v: uint16 }", "h_t", "h_read",
                    {0x12, 0x34}, "out.v == 0x1234", true, &err)) << err;
}

// Interval confinement — BBQ's core feature. `val` is read inside the window
// [off, off+1] (the reader seeks to off, then confines), so val == buf[off].
TEST(RenderC, FieldInterval) {
    std::string err;
    EXPECT_TRUE(run("S = struct { off: uint8, val: uint8 [off, off + 1] }", "s_t", "s_read",
                    {3, 0, 0, 42}, "out.off == 3 && out.val == 42", true, &err)) << err;
}

// A window that escapes the input must fail the parse (the IPG confinement check),
// not read out of bounds — mirrors cek_test's element-interval escape case.
TEST(RenderC, FieldIntervalOverrun) {
    std::string err;
    EXPECT_TRUE(run("S = struct { off: uint8, val: uint8 [off, off + 1] }", "s_t", "s_read",
                    {10, 0, 0, 0}, "", false, &err)) << err;
}

// Proves the window is POPPED, not just pushed: `extra` reads AFTER val's window.
// off=2 → val=buf[2] inside [2,3]; without the pop, effective_end stays 3 and
// extra's read at pos 3 would overrun. With the pop it reads buf[3]. (Catches the
// "did just enough to pass one test" failure mode.)
TEST(RenderC, FieldIntervalPops) {
    std::string err;
    EXPECT_TRUE(run("S = struct { off: uint8, val: uint8 [off, off + 1], extra: uint8 }",
                    "s_t", "s_read", {2, 0, 99, 42},
                    "out.off == 2 && out.val == 99 && out.extra == 42", true, &err)) << err;
}

// Array element-interval (random access): each element parsed at a computed offset
// using @index — the elf64 section-table shape. items[i] = buf[base + i].
TEST(RenderC, ArrayElementInterval) {
    std::string err;
    EXPECT_TRUE(run("S = struct { n: uint8, base: uint8, "
                    "items: array<uint8>[n] @ [base + @index, base + @index + 1] }",
                    "s_t", "s_read", {2, 4, 0, 0, 77, 88},
                    "out.n == 2 && out.items.count == 2 && "
                    "out.items.items[0] == 77 && out.items.items[1] == 88", true, &err)) << err;
}

// Array of bytes elements (formerly crashed the generator).
// Nested inline array<array<...>> — outer slot must use the OUTER array's own loop
// frame (base+level), not innermost. rows[i].items[j] = buf[2 + i*m + j].
TEST(RenderC, NestedArray) {
    std::string err;
    EXPECT_TRUE(run("S = struct { n: uint8, m: uint8, rows: array<array<uint8>[m]>[n] }",
                    "s_t", "s_read", {2, 2, 10, 11, 20, 21},
                    "out.rows.count == 2 && out.rows.items[0].count == 2 && "
                    "out.rows.items[0].items[0] == 10 && out.rows.items[0].items[1] == 11 && "
                    "out.rows.items[1].items[0] == 20 && out.rows.items[1].items[1] == 21", true, &err)) << err;
}

// SCOPING: a nested rule (I, two levels down) reads an ancestor field (R.v at the
// root) in its constraint. Tests the scope-ptr-up-the-stack path in the C reader.
TEST(RenderC, AncestorAccess) {
    std::string err;
    const char* g = "R = struct { v: uint8, m: M }\nM = struct { i: I }\n"
                    "I = struct { x: uint8 where x == v }";
    EXPECT_TRUE(run(g, "r_t", "r_read", {5, 5}, "out.v == 5 && out.m.i.x == 5", true, &err)) << err;
}
TEST(RenderC, AncestorAccessRejects) {
    std::string err;
    const char* g = "R = struct { v: uint8, m: M }\nM = struct { i: I }\n"
                    "I = struct { x: uint8 where x == v }";
    EXPECT_TRUE(run(g, "r_t", "r_read", {5, 6}, "", false, &err)) << err;
}

// SCOPING: a later sibling field reads an already-parsed array element (xs[0]) —
// "siblings see each other's stuff". Tests array indexing in a constraint expr.
TEST(RenderC, SiblingArrayAccess) {
    std::string err;
    const char* g = "S = struct { n: uint8, xs: array<uint8>[n], y: uint8 where y == xs[0] }";
    EXPECT_TRUE(run(g, "s_t", "s_read", {2, 7, 8, 7}, "out.y == 7", true, &err)) << err;
}
TEST(RenderC, SiblingArrayAccessRejects) {
    std::string err;
    const char* g = "S = struct { n: uint8, xs: array<uint8>[n], y: uint8 where y == xs[0] }";
    EXPECT_TRUE(run(g, "s_t", "s_read", {2, 7, 8, 9}, "", false, &err)) << err;
}

// ── SWEEP: one test per known/suspected gap (run to establish red/green baseline) ──
// EOF-mode array: read elements until end of input.
TEST(RenderC, SweepEofArray) {
    std::string err;
    const char* g = "Item = struct { v: uint8 }\nDoc = struct { items: array<Item>(none, eof) }";
    EXPECT_TRUE(run(g, "doc_t", "doc_read", {1, 2, 3},
                    "out.items.count == 3 && out.items.items[0].v == 1 && out.items.items[2].v == 3",
                    true, &err)) << err;
}
// Until-mode array: read elements until a condition holds.
TEST(RenderC, SweepUntilArray) {
    std::string err;
    const char* g = "Item = struct { v: uint8 }\nDoc = struct { n: uint8, items: array<Item>(none, until(@index >= n)) }";
    EXPECT_TRUE(run(g, "doc_t", "doc_read", {2, 10, 20, 30},
                    "out.items.count == 2 && out.items.items[0].v == 10", true, &err)) << err;
}
// resync mode: on per-element parse failure, restore, advance one byte, retry the
// element; eof terminates. Only 0xAA parses as Item (where v==0xAA); other bytes are
// skipped. Matches the CEK oracle: {0xAA,0x01,0xAA} -> 2 items, the 0x01 skipped.
TEST(RenderC, SweepResyncArray) {
    std::string err;
    const char* g = "Item = struct { v: uint8 where v == 0xAA }\n"
                    "Doc = struct { items: array<Item>(none, eof) resync }";
    EXPECT_TRUE(run(g, "doc_t", "doc_read", {0xAA, 0x01, 0xAA},
                    "out.items.count == 2 && out.items.items[0].v == 0xAA && "
                    "out.items.items[1].v == 0xAA", true, &err)) << err;
}

// counted + resync: collect `n` elements, skipping bad bytes between them (n good
// elements available). Well-defined and matches the CEK: n=2 over {0xAA,0x01,0xAA}
// collects the two 0xAA, skipping the 0x01. (The eof-BEFORE-count corner is left
// untested on purpose — its semantics are undecided; see note in conversation.)
TEST(RenderC, SweepCountedResyncArray) {
    std::string err;
    const char* g = "Item = struct { v: uint8 where v == 0xAA }\n"
                    "Doc = struct { n: uint8, items: array<Item>[n] resync }";
    EXPECT_TRUE(run(g, "doc_t", "doc_read", {2, 0xAA, 0x01, 0xAA},
                    "out.items.count == 2 && out.items.items[0].v == 0xAA && "
                    "out.items.items[1].v == 0xAA", true, &err)) << err;
    // eof before the declared count is met: a malformed structure — must FAIL, never
    // silently return fewer than n (IPG correct-by-construction).
    EXPECT_TRUE(run(g, "doc_t", "doc_read", {3, 0xAA, 0x01, 0xAA}, "", false, &err)) << err;
}

// Bare optional (no `where`).
TEST(RenderC, SweepBareOptional) {
    std::string err;
    const char* g = "S = struct { f: uint8, x: optional<uint8> }";
    EXPECT_TRUE(run(g, "s_t", "s_read", {5, 7}, "out.f == 5", true, &err)) << err;
}
// @rest: the size field scopes the remainder of the struct to exactly `size` bytes.
TEST(RenderC, SweepRestField) {
    std::string err;
    const char* g = "S = struct { size: uint8 @rest, a: uint8, b: uint8 }";
    EXPECT_TRUE(run(g, "s_t", "s_read", {2, 10, 20},
                    "out.size == 2 && out.a == 10 && out.b == 20", true, &err)) << err;
}
// extern field: the user supplies `bool decode(bbq_ctx_t*, uint8_t* out)` reading
// from the cursor (the legacy C convention). The `[n]` confines it to n bytes.
TEST(RenderC, SweepExtern) {
    std::string err;
    const char* g = "S = struct { n: uint8, p: extern(\"decode\", \"uint8_t\")[n] }";
    const char* prelude =
        "static bool decode(bbq_ctx_t* ctx, uint8_t* out) { return bbq_read_u8(ctx, out); }";
    EXPECT_TRUE(run(g, "s_t", "s_read", {1, 9}, "out.n == 1 && out.p == 9", true, &err, prelude)) << err;
}
// mid-struct @endian switch (EndianSwitch / set_endian op): `@endian: <expr>`.
// a reads big-endian (directive), then switch to little, b reads little-endian.
TEST(RenderC, SweepEndianSwitch) {
    std::string err;
    const char* g = "@endian big\nS = struct { a: uint16, @endian: little, b: uint16 }";
    EXPECT_TRUE(run(g, "s_t", "s_read", {0, 1, 2, 0}, "out.a == 1 && out.b == 2", true, &err)) << err;
}

// optional<Rule> — the present element must land in `.value` (was mis-rendered as a
// hard constraint that failed when absent).
TEST(RenderC, OptionalRulePresent) {
    std::string err;
    EXPECT_TRUE(run("I = struct { a: uint8 }\nO = struct { f: uint8, x: optional<I> where f != 0 }",
                    "o_t", "o_read", {1, 42},
                    "out.f == 1 && out.x.has_value && out.x.value.a == 42", true, &err)) << err;
}
TEST(RenderC, OptionalRuleAbsent) {
    std::string err;
    EXPECT_TRUE(run("I = struct { a: uint8 }\nO = struct { f: uint8, x: optional<I> where f != 0 }",
                    "o_t", "o_read", {0},
                    "out.f == 0 && !out.x.has_value", true, &err)) << err;
}

TEST(RenderC, OptionalBytes) {
    std::string err;
    EXPECT_TRUE(run("O = struct { f: uint8, x: optional<bytes[2]> where f != 0 }",
                    "o_t", "o_read", {1, 'a', 'b'},
                    "out.x.has_value && out.x.value.data[0] == 'a' && out.x.value.data[1] == 'b'",
                    true, &err)) << err;
}

TEST(RenderC, ArrayBytesElem) {
    std::string err;
    EXPECT_TRUE(run("S = struct { n: uint8, items: array<bytes[2]>[n] }",
                    "s_t", "s_read", {2, 'a', 'b', 'c', 'd'},
                    "out.items.count == 2 && out.items.items[0].data[0] == 'a' && "
                    "out.items.items[1].data[1] == 'd'", true, &err)) << err;
}

// IPG per-read confinement: a uint16 (2 bytes) confined to a 1-byte window must
// fail — the read can't overrun the interval's effective_end even with input left.
TEST(RenderC, IntervalConfineReadOverrun) {
    std::string err;
    EXPECT_TRUE(run("S = struct { x: uint16 [0, 1] }", "s_t", "s_read",
                    {0xAA, 0xBB, 0xCC}, "", false, &err)) << err;
}

// IPG lower-bound confinement: a nested interval whose start escapes BELOW the
// parent window must fail (the new bidirectional check / effective_start). The
// inner @[0,1] seeks to 0, before the outer [1,3) window's start → reject.
TEST(RenderC, IntervalStartEscapesParent) {
    std::string err;
    EXPECT_TRUE(run("Inner = struct { x: uint8 [0, 1] }\n"
                    "S = struct { hdr: uint8, body: Inner [1, 3] }",
                    "s_t", "s_read", {9, 10, 11, 12}, "", false, &err)) << err;
}

// Endian register persists across a rule call: a `@endian: little` switch in the
// caller is inherited by an invoked sub-rule's unsuffixed read (CEK parity — the
// register is seeded once at the outermost entry, not reset per rule).
TEST(RenderC, EndianCrossRule) {
    std::string err;
    EXPECT_TRUE(run("@endian big\nInner = struct { v: uint16 }\n"
                    "S = struct { @endian: little, w: Inner }",
                    "s_t", "s_read", {2, 0}, "out.w.v == 2", true, &err)) << err;
}

// Backtrack rolls back the partial write: the bare optional's inner writes
// p.value.x=10, then its `where y==99` fails and the branch backtracks. The CEK
// truncates the capture (p unbound); the c-owning reader zeroes .value, so an
// absent optional is cleanly absent (has_value==false, value==0), never a ghost of
// the failed attempt.
TEST(RenderC, BacktrackRollsBackOptional) {
    std::string err;
    EXPECT_TRUE(run("Pair = struct { x: uint8, y: uint8 where y == 99 }\n"
                    "S = struct { p: optional<Pair>, z: uint8 }",
                    "s_t", "s_read", {10, 50},
                    "out.p.has_value == false && out.p.value.x == 0 && out.z == 10", true, &err)) << err;
}
