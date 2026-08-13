// render_view_parser_test — the cpp-zcow (view) parser builds the SAME index the
// CEK builds. The generated view reader (ZcowReaderReader.h, the burg view profile
// over the kont IR) parses bytes into a FieldCapture index; this asserts that index
// decodes identically to the CEK's for the same bytes, and reads correctly through
// the generated handle classes. Grows with the fixture as the view profile does.
#include <gtest/gtest.h>
#include "Parser.h"
#include "Sema.h"
#include "bbq_compile.h"      // bbq::Compiler / CompiledGrammar
#include "Machine.h"          // bbq::cek::CEKMachine
#include "bbq_reader.h"       // the view parse context
#include "bbq_node.h"         // decode_int over the index
#include "ZcowReaderTypes.h"  // generated handle classes (namespace zr)
#include "ZcowReaderReader.h" // generated view parser (namespace zr)

#include <cstdint>
#include <string>
#include <vector>

using namespace BBQ;
using namespace bbqgen;

// The view-side extern (global, matching the generated parser's forward decl):
// consumes a fixed 4 bytes, like the CEK reference.
bool readit(const uint8_t*, size_t len, size_t* consumed) {
    if (len < 4) return false; *consumed = 4; return true;
}

namespace {

// CEK-side extern (the reference): consumes 4 bytes, like the view's `readit`.
static bool cek_readit(const uint8_t*, size_t len, size_t* consumed, bbq::ParseArena*, void*) {
    if (len < 4) return false; *consumed = 4; return true;
}

// CEK reference. Compile the fixture grammar ONCE (cached — the truncation sweep
// re-parses many prefixes), then run a fresh machine per parse. `cek_meta` returns
// the metadata WITHOUT asserting success (the negative axis must observe the CEK
// rejecting); `cek_root` asserts success and returns the root (positive tests).
static bbq::cek::CompiledGrammar* cek_grammar(const char* spec) {
    static const char* cached_spec = nullptr;
    static bbq::cek::CompiledGrammar* cached = nullptr;
    if (cached && cached_spec == spec) return cached;
    auto* parser = new Parser();
    parser->init(spec, (int)std::char_traits<char>::length(spec));
    EXPECT_TRUE(parser->parse());
    static ErrorReporter rep;
    auto* sema = new Sema(rep);
    EXPECT_TRUE(sema->analyze(parser->ast));
    auto* g = new bbq::Compiler();
    cached = g->compile_grammar(parser->ast);
    cached_spec = spec;
    return cached;
}

bbq::CaptureMetadata cek_meta(const char* spec, const char* rule,
                              const uint8_t* buf, size_t len) {
    auto* cg = cek_grammar(spec);
    auto* arena = new bbq::ParseArena();
    auto* m = new bbq::cek::CEKMachine();
    m->arena = arena;
    m->builtins = &cg->builtins;
    auto* ext = new bbq::cek::ExternalParserTable::Entry[1];
    ext[0].name = cg->strings.intern(std::string("readit"));
    ext[0].fn = &cek_readit; ext[0].user_data = nullptr;
    auto* ext_table = new bbq::cek::ExternalParserTable();
    ext_table->entries = ext; ext_table->count = 1;
    m->ext_parsers = ext_table;
    return m->execute_from(cg->lookup(std::string(rule)), buf, len, cg->default_little_endian);
}

const bbq::FieldCapture* cek_root(const char* spec, const char* rule,
                                  const std::vector<uint8_t>& bytes, const uint8_t* buf) {
    bbq::CaptureMetadata meta = cek_meta(spec, rule, buf, bytes.size());
    EXPECT_TRUE(meta.success);
    return meta.root;
}

int64_t dec(const bbq::FieldCapture* parent, const char* name, const uint8_t* buf) {
    int64_t bits = 0; bool sgn = false;
    EXPECT_TRUE(bbq::decode_int(bbq::node_child(parent, name), buf, &bits, &sgn));
    return bits;
}

// Must match test/fixtures/zcow_reader.bbq (cmake generates the view reader from it).
const char* kSpec =
    "@endian little\n"
    "Flat = struct { u8: uint8, u16: uint16le, u32: uint32le, be16: uint16be, i32: int32le }\n"
    "Nest = struct { hdr: struct { a: uint8, b: uint16le }, c: uint8 }\n"
    "Arr = struct { n: uint8, xs: array<uint16le>[n] }\n"
    "Pair = struct { p: uint8, q: uint8 }\n"
    "Outer = struct { x: uint8, inner: Pair }\n"
    "RArr = struct { n: uint8, items: array<Pair>[n] }\n"
    "Bytes = struct { n: uint8, data: bytes[n] }\n"
    "Str = struct { s: string[3] }\n"
    "Wh = struct { ver: uint8 where ver == 1, x: uint8 }\n"
    "WhTwice = struct { lo: uint8, v: uint8 where v >= lo && v <= lo + 8, hi: uint8 }\n"
    "Comp = struct { raw: uint8, hi: compute((raw >> 4) & 0x0F : uint8), lo: compute(raw & 0x0F : uint8) }\n"
    "Iv = struct { off: uint8, val: uint8 [off, off + 1] }\n"
    "Pos = struct { a: uint8, b: uint8 where @pos == 2 }\n"
    "Op = struct { f: uint8, v: optional<uint16le> where f == 1, r: optional<Pair> where f == 1 }\n"
    "Bare = struct { x: uint8, v: optional<uint8> }\n"
    "Sw = struct { tag: uint8, body: switch(tag) { 1: uint16le; 2: Pair; default: uint8; } }\n"
    "VA = struct { t: uint8 where t == 1, a: uint8 }\n"
    "VB = struct { t: uint8 where t == 2, b: uint8 }\n"
    "U = union { asA: VA, asB: VB }\n"
    "Alts = VA | VB\n"
    "Bf = bitfield<uint8> { hi: 4, lo: 4 }\n"
    "InlineBf = struct { lead: uint8, flags: bitfield<uint8> { hi: 4, lo: 4 }, tail: uint8 }\n"
    "Eof = struct { items: array<Pair>(none, eof) }\n"
    "Unt = struct { xs: array<uint8>(none, until(@remaining < 2)) }\n"
    "Es = struct { a: uint16, @endian: big, b: uint16 }\n"
    "RsItem = struct { v: uint8 where v != 0 }\n"
    "Resync = struct { n: uint8, items: array<RsItem>[n] resync }\n"
    "Ext = struct { tag: uint8, blob: extern(\"readit\", \"uint32_t\"), tail: uint8 }\n"
    "SwRange = struct { tag: uint8, body: switch(tag) { 1..3: uint16le; default: uint8; } }\n"
    "Tern = struct { f: uint8, g: compute((f > 0 ? f * 2 : 99) : uint8) }\n"
    "Bstart = struct { pad: uint8, val: uint8 where @start == 0 }\n"
    "Rest = struct { size: uint8 @rest, a: uint16le }\n"
    "RestEof = struct { sz: uleb128 @rest, xs: array<uint8>(none, eof) }\n"
    "Cnt = struct { n: uint8, xs: array<uint8>(none, count(n)) }\n"
    "TopSw = switch(peek()) { 1: Pair; default: uint8; }\n"
    "Np = struct { h: struct { n: uint8 }, xs: array<uint8>[h.n] }\n"
    "OScope = struct { base: uint8, cnt: uint8, items: array<uint8>[cnt]"
    " @ [base + @index, base + @index + 1] }\n"
    "AllPrim = struct { u8: uint8, i8: int8, u16: uint16le, u16b: uint16be,"
    " i16: int16le, u32: uint32le, u32b: uint32be, i32: int32le, u64: uint64le,"
    " i64: int64le, f32: float32le, f64: float64le, fl: bool }\n"
    "Leb = struct { a: uleb128, b: sleb64, c: uint8 }\n"
    "LebArr = struct { n: uint8, xs: array<uleb128>[n] }\n"
    "LebOpt = struct { f: uint8, v: optional<uleb128> where f == 1, tail: uint8 }\n"
    "Mat = struct { rows: uint8, cols: uint8, data: array<array<uint8>[cols]>[rows] }\n"
    "OptSpan = struct { n: uint8, ob: optional<bytes[2]> where n == 1,"
    " os: optional<string[3]> where n == 1 }\n"
    "TyByte = uint8\n"
    "TyRule = Pair\n"
    "OptTop = optional<uint8>\n"
    "NestGrp = struct { n: uint8, xs: array<uint8>[n] }\n"
    "NestArr = struct { m: uint8, gs: array<NestGrp>[m] }\n";

}  // namespace

TEST(RenderViewParser, FlatStructMatchesCek) {
    // u8=0x12 u16=0x3456 u32=0x100 be16=0x0102 i32=-4
    std::vector<uint8_t> bytes = {
        0x12, 0x56, 0x34, 0x00, 0x01, 0x00, 0x00, 0x01, 0x02, 0xFC, 0xFF, 0xFF, 0xFF };
    auto* buf = new uint8_t[bytes.size()];
    std::copy(bytes.begin(), bytes.end(), buf);

    // The generated view parser builds the index.
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Flat_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    ASSERT_NE(vm.root, nullptr);

    // The CEK builds its index from the same bytes.
    const bbq::FieldCapture* ck = cek_root(kSpec, "Flat", bytes, buf);
    ASSERT_NE(ck, nullptr);

    // Parity: every field decodes identically from both indexes.
    for (const char* f : {"u8", "u16", "u32", "be16", "i32"})
        EXPECT_EQ(dec(vm.root, f, buf), dec(ck, f, buf)) << f;

    // And the known values, read through the generated handle over the view index.
    zr::Flat h(vm.root, buf, nullptr);
    EXPECT_EQ(h.u8(),   0x12);
    EXPECT_EQ(h.u16(),  0x3456);
    EXPECT_EQ(h.u32(),  0x100u);
    EXPECT_EQ(h.be16(), 0x0102);
    EXPECT_EQ(h.i32(),  -4);
}

TEST(RenderViewParser, FloatBoolComputeHandleMatchesCek) {
    // The handle accessor reads a computed field at its REAL kind: a float compute as
    // double (not flattened through the int path → 2 instead of 2.5), a bool as bool.
    std::vector<uint8_t> bytes = {0x05};   // x = 5  → half = 5/2.0 = 2.5, big = (5 > 100) = false
    auto* buf = new uint8_t[bytes.size()];
    std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::FComp_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    ASSERT_NE(vm.root, nullptr);
    zr::FComp h(vm.root, buf, nullptr);
    EXPECT_DOUBLE_EQ(h.half(), 5 / 2.0);   // 2.5 — not truncated to 2.0
    EXPECT_EQ(h.big(), 5 > 100);           // false — a real bool, not int
}

TEST(RenderViewParser, NestedStructMatchesCek) {
    // hdr{a=7,b=9}, c=0x0B — the view nests via begin_struct/end_struct stencils.
    std::vector<uint8_t> bytes = { 0x07, 0x09, 0x00, 0x0B };
    auto* buf = new uint8_t[bytes.size()];
    std::copy(bytes.begin(), bytes.end(), buf);

    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Nest_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);

    const bbq::FieldCapture* ck = cek_root(kSpec, "Nest", bytes, buf);
    ASSERT_NE(ck, nullptr);

    // The "hdr" sub-node nests identically in both indexes.
    const bbq::FieldCapture* vh = bbq::node_child(vm.root, "hdr");
    const bbq::FieldCapture* ch = bbq::node_child(ck, "hdr");
    ASSERT_NE(vh, nullptr); ASSERT_NE(ch, nullptr);
    EXPECT_EQ(dec(vh, "a", buf), dec(ch, "a", buf));
    EXPECT_EQ(dec(vh, "b", buf), dec(ch, "b", buf));
    EXPECT_EQ(dec(vm.root, "c", buf), dec(ck, "c", buf));

    // And through the generated handles (inline struct → zr::Nest_hdr).
    zr::Nest h(vm.root, buf, nullptr);
    zr::Nest_hdr hdr = h.hdr();
    EXPECT_EQ(hdr.a(), 7);
    EXPECT_EQ(hdr.b(), 9);
    EXPECT_EQ(h.c(), 0x0B);
}

TEST(RenderViewParser, CountedArrayMatchesCek) {
    // n=3, xs = {0x0010, 0x0020, 0x0030} (uint16le)
    std::vector<uint8_t> bytes = { 0x03, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00 };
    auto* buf = new uint8_t[bytes.size()];
    std::copy(bytes.begin(), bytes.end(), buf);

    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Arr_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);

    const bbq::FieldCapture* ck = cek_root(kSpec, "Arr", bytes, buf);
    ASSERT_NE(ck, nullptr);

    // The array node nests identically (same child count + element decode).
    const bbq::FieldCapture* va = bbq::node_child(vm.root, "xs");
    const bbq::FieldCapture* ca = bbq::node_child(ck, "xs");
    ASSERT_NE(va, nullptr); ASSERT_NE(ca, nullptr);
    ASSERT_EQ(va->child_count, ca->child_count);
    ASSERT_EQ(va->child_count, 3);

    // Through the generated handle: prim_seq<uint16_t>.
    zr::Arr h(vm.root, buf, nullptr);
    EXPECT_EQ(h.n(), 3);
    bbq::prim_seq<uint16_t> xs = h.xs();
    ASSERT_EQ(xs.size(), 3u);
    EXPECT_EQ(xs[0], 0x10);
    EXPECT_EQ(xs[1], 0x20);
    EXPECT_EQ(xs[2], 0x30);
}

TEST(RenderViewParser, RulerefFieldMatchesCek) {
    // x=0x55, inner: Pair{p=7, q=8} — cross-rule invoke into the shared reader.
    std::vector<uint8_t> bytes = { 0x55, 0x07, 0x08 };
    auto* buf = new uint8_t[bytes.size()];
    std::copy(bytes.begin(), bytes.end(), buf);

    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Outer_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Outer", bytes, buf);

    const bbq::FieldCapture* vi = bbq::node_child(vm.root, "inner");
    const bbq::FieldCapture* ci = bbq::node_child(ck, "inner");
    ASSERT_NE(vi, nullptr); ASSERT_NE(ci, nullptr);
    EXPECT_EQ(dec(vi, "p", buf), dec(ci, "p", buf));
    EXPECT_EQ(dec(vi, "q", buf), dec(ci, "q", buf));

    zr::Outer h(vm.root, buf, nullptr);
    EXPECT_EQ(h.x(), 0x55);
    zr::Pair inner = h.inner();
    EXPECT_EQ(inner.p(), 7);
    EXPECT_EQ(inner.q(), 8);
}

TEST(RenderViewParser, ArrayOfRuleMatchesCek) {
    // n=2, items: [Pair{1,2}, Pair{3,4}] — invoke per element into the array scope.
    std::vector<uint8_t> bytes = { 0x02, 1, 2, 3, 4 };
    auto* buf = new uint8_t[bytes.size()];
    std::copy(bytes.begin(), bytes.end(), buf);

    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::RArr_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "RArr", bytes, buf);

    const bbq::FieldCapture* va = bbq::node_child(vm.root, "items");
    const bbq::FieldCapture* ca = bbq::node_child(ck, "items");
    ASSERT_EQ(va->child_count, ca->child_count);

    zr::RArr h(vm.root, buf, nullptr);
    bbq::seq<zr::Pair> items = h.items();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].p(), 1); EXPECT_EQ(items[0].q(), 2);
    EXPECT_EQ(items[1].p(), 3); EXPECT_EQ(items[1].q(), 4);
}

TEST(RenderViewParser, BytesAndStringMatchCek) {
    {   // bytes[n]: n=3, data = {0xAA,0xBB,0xCC}
        std::vector<uint8_t> bytes = { 0x03, 0xAA, 0xBB, 0xCC };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Bytes_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::Bytes h(vm.root, buf, nullptr);
        bbq::bytes_view d = h.data();
        ASSERT_EQ(d.size, 3u);
        EXPECT_EQ(d.data[0], 0xAA); EXPECT_EQ(d.data[2], 0xCC);
    }
    {   // string[3] = "abc"
        std::vector<uint8_t> bytes = { 0x61, 0x62, 0x63 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Str_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::Str h(vm.root, buf, nullptr);
        EXPECT_EQ(h.s(), std::string_view("abc"));
    }
}

TEST(RenderViewParser, WhereConstraint) {
    {   // ver=1 passes
        std::vector<uint8_t> bytes = { 0x01, 0x2A };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Wh_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::Wh h(vm.root, buf, nullptr);
        EXPECT_EQ(h.ver(), 1); EXPECT_EQ(h.x(), 0x2A);
    }
    {   // ver=2 fails the where (decode-on-access constraint)
        std::vector<uint8_t> bytes = { 0x02, 0x2A };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Wh_read(buf, bytes.size(), arena);
        EXPECT_FALSE(vm.success);
    }
}

TEST(RenderViewParser, ComputeMatchesCek) {
    // raw=0xA5 → hi=0xA, lo=0x5 (computed, stored as Computed captures).
    std::vector<uint8_t> bytes = { 0xA5 };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Comp_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);

    const bbq::FieldCapture* ck = cek_root(kSpec, "Comp", bytes, buf);
    EXPECT_EQ(bbq::node_computed_int(bbq::node_child(vm.root, "hi")),
              bbq::node_computed_int(bbq::node_child(ck, "hi")));

    zr::Comp h(vm.root, buf, nullptr);
    EXPECT_EQ(h.hi(), 0xA);
    EXPECT_EQ(h.lo(), 0x5);
}

TEST(RenderViewParser, FieldIntervalMatchesCek) {
    // off=2 → val read from byte 2 (window [2,3)); byte 1 skipped.
    std::vector<uint8_t> bytes = { 0x02, 0xFF, 0x2A };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Iv_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Iv", bytes, buf);
    EXPECT_EQ(dec(vm.root, "val", buf), dec(ck, "val", buf));
    zr::Iv h(vm.root, buf, nullptr);
    EXPECT_EQ(h.val(), 0x2A);
}

TEST(RenderViewParser, PosBuiltin) {
    // @pos in the where spells r.pos in the view (not ctx). After a,b, pos==2 → ok.
    std::vector<uint8_t> ok = { 0x0A, 0x0B };
    auto* buf = new uint8_t[ok.size()]; std::copy(ok.begin(), ok.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Pos_read(buf, ok.size(), arena);
    ASSERT_TRUE(vm.success);
    zr::Pos h(vm.root, buf, nullptr);
    EXPECT_EQ(h.a(), 0x0A);
    EXPECT_EQ(h.b(), 0x0B);
}

TEST(RenderViewParser, OptionalPredicated) {
    {   // f=1 → v + r present
        std::vector<uint8_t> bytes = { 0x01, 0x34, 0x12, 0x05, 0x06 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Op_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::Op h(vm.root, buf, nullptr);
        bbq::prim_opt<uint16_t> v = h.v();
        ASSERT_TRUE(v.has_value()); EXPECT_EQ(v.value(), 0x1234);
        bbq::opt<zr::Pair> r = h.r();
        ASSERT_TRUE(r.has_value()); EXPECT_EQ(r.value().p(), 5); EXPECT_EQ(r.value().q(), 6);
    }
    {   // f=0 → both absent
        std::vector<uint8_t> bytes = { 0x00 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Op_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::Op h(vm.root, buf, nullptr);
        EXPECT_FALSE(h.v().has_value());
        EXPECT_FALSE(h.r().has_value());
    }
}

TEST(RenderViewParser, OptionalBareTryRestore) {
    {   // 2 bytes → v present
        std::vector<uint8_t> bytes = { 0x0A, 0x2A };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Bare_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::Bare h(vm.root, buf, nullptr);
        ASSERT_TRUE(h.v().has_value()); EXPECT_EQ(h.v().value(), 0x2A);
    }
    {   // 1 byte → inner read fails, restore → v absent (no ghost capture)
        std::vector<uint8_t> bytes = { 0x0A };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Bare_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::Bare h(vm.root, buf, nullptr);
        EXPECT_FALSE(h.v().has_value());
    }
}

TEST(RenderViewParser, SwitchMatchesCek) {
    auto run = [](std::vector<uint8_t> bytes) {
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        static bbq::ParseArena arena;  // leak: index outlives the call
        return std::make_pair(zr::Sw_read(buf, bytes.size(), arena), buf);
    };
    {   // tag=1 → case 0 (uint16le)
        auto [vm, buf] = run({ 0x01, 0x34, 0x12 });
        ASSERT_TRUE(vm.success);
        zr::Sw h(vm.root, buf, nullptr);
        EXPECT_EQ(h.body_which(), 0);
        EXPECT_EQ(h.body_case_0(), 0x1234);
    }
    {   // tag=2 → case 1 (Pair)
        auto [vm, buf] = run({ 0x02, 0x05, 0x06 });
        ASSERT_TRUE(vm.success);
        zr::Sw h(vm.root, buf, nullptr);
        EXPECT_EQ(h.body_which(), 1);
        EXPECT_EQ(h.body_case_1().p(), 5);
        EXPECT_EQ(h.body_case_1().q(), 6);
    }
    {   // tag=9 → default (ordinal 2, uint8)
        auto [vm, buf] = run({ 0x09, 0x2A });
        ASSERT_TRUE(vm.success);
        zr::Sw h(vm.root, buf, nullptr);
        EXPECT_EQ(h.body_which(), 2);
        EXPECT_EQ(h.body_default_val(), 0x2A);
    }
}

TEST(RenderViewParser, UnionMatchesCek) {
    {   // t=1 → asA (first variant)
        std::vector<uint8_t> bytes = { 0x01, 0x0A };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::U_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::U h(vm.root, buf, nullptr);
        EXPECT_EQ(h.which(), 0);
        ASSERT_TRUE(h.is_asA());
        EXPECT_EQ(h.asA().a(), 0x0A);
    }
    {   // t=2 → asA fails its where, backtrack to asB
        std::vector<uint8_t> bytes = { 0x02, 0x0B };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::U_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::U h(vm.root, buf, nullptr);
        EXPECT_EQ(h.which(), 1);
        ASSERT_TRUE(h.is_asB());
        EXPECT_EQ(h.asB().b(), 0x0B);
    }
}

TEST(RenderViewParser, AlternativesMatchesCek) {
    std::vector<uint8_t> bytes = { 0x02, 0x0B };   // alt_0 (VA) fails → alt_1 (VB)
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Alts_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    zr::Alts h(vm.root, buf, nullptr);
    EXPECT_EQ(h.which(), 1);
    ASSERT_TRUE(h.is_alt_1());
    EXPECT_EQ(h.alt_1().b(), 0x0B);
}

TEST(RenderViewParser, BitfieldMatchesCek) {
    {   // top-level bitfield rule: byte 0xA5 (le/lsb-first) → hi=5, lo=0xA
        std::vector<uint8_t> bytes = { 0xA5 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Bf_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        const bbq::FieldCapture* ck = cek_root(kSpec, "Bf", bytes, buf);
        EXPECT_EQ(bbq::node_computed_int(bbq::node_child(vm.root, "hi")),
                  bbq::node_computed_int(bbq::node_child(ck, "hi")));
        zr::Bf h(vm.root, buf, nullptr);
        EXPECT_EQ(h.hi(), 0x5); EXPECT_EQ(h.lo(), 0xA);
    }
    {   // inline bitfield field nests under flags
        std::vector<uint8_t> bytes = { 0x11, 0xA5, 0x22 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::InlineBf_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::InlineBf h(vm.root, buf, nullptr);
        EXPECT_EQ(h.lead(), 0x11); EXPECT_EQ(h.tail(), 0x22);
        zr::InlineBf_flags f = h.flags();
        EXPECT_EQ(f.hi(), 0x5); EXPECT_EQ(f.lo(), 0xA);
    }
}

TEST(RenderViewParser, UncountedArraysMatchCek) {
    {   // eof: items run to the window end — 2 Pairs
        std::vector<uint8_t> bytes = { 1, 2, 3, 4 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Eof_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        const bbq::FieldCapture* ck = cek_root(kSpec, "Eof", bytes, buf);
        const bbq::FieldCapture* va = bbq::node_child(vm.root, "items");
        const bbq::FieldCapture* ca = bbq::node_child(ck, "items");
        ASSERT_EQ(va->child_count, ca->child_count);
        zr::Eof h(vm.root, buf, nullptr);
        ASSERT_EQ(h.items().size(), 2u);
        EXPECT_EQ(h.items()[1].p(), 3);
    }
    {   // until(@remaining < 2): loops while >=2 bytes remain; count matches the CEK
        std::vector<uint8_t> bytes = { 1, 2, 3 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Unt_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        const bbq::FieldCapture* ck = cek_root(kSpec, "Unt", bytes, buf);
        const bbq::FieldCapture* va = bbq::node_child(vm.root, "xs");
        const bbq::FieldCapture* ca = bbq::node_child(ck, "xs");
        ASSERT_EQ(va->child_count, ca->child_count);
        for (int i = 0; i < va->child_count; i++) {
            int64_t a=0,b=0; bool s=false;
            bbq::decode_int(&va->children[i], buf, &a, &s);
            bbq::decode_int(&ca->children[i], buf, &b, &s);
            EXPECT_EQ(a, b);
        }
    }
}

TEST(RenderViewParser, EndianSwitchMatchesCek) {
    // a: little (default), then @endian: big → b: big. a=0x1234, b=0x1234.
    std::vector<uint8_t> bytes = { 0x34, 0x12, 0x12, 0x34 };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Es_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Es", bytes, buf);
    // The index records the runtime endian (a=UInt16LE, b=UInt16BE after the switch),
    // identical to the CEK, AND the handle reads it correctly (scalar accessors now
    // decode via the recorded capture type, so they follow a mid-struct @endian switch).
    EXPECT_EQ(dec(vm.root, "a", buf), dec(ck, "a", buf));
    EXPECT_EQ(dec(vm.root, "b", buf), dec(ck, "b", buf));
    zr::Es h(vm.root, buf, nullptr);
    EXPECT_EQ(h.a(), 0x1234);
    EXPECT_EQ(h.b(), 0x1234);
}

TEST(RenderViewParser, ResyncArrayMatchesCek) {
    // n=2; the 0 bytes fail RsItem's where → skipped; collect {5, 7}.
    std::vector<uint8_t> bytes = { 0x02, 0x00, 0x05, 0x00, 0x07 };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Resync_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Resync", bytes, buf);
    const bbq::FieldCapture* va = bbq::node_child(vm.root, "items");
    const bbq::FieldCapture* ca = bbq::node_child(ck, "items");
    ASSERT_EQ(va->child_count, ca->child_count);
    zr::Resync h(vm.root, buf, nullptr);
    bbq::seq<zr::RsItem> items = h.items();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].v(), 5);
    EXPECT_EQ(items[1].v(), 7);
}

TEST(RenderViewParser, ExternMatchesCek) {
    // blob = the 4 bytes the extern consumed; the index records the span, read as bytes.
    std::vector<uint8_t> bytes = { 0xAA, 0x01, 0x02, 0x03, 0x04, 0xBB };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Ext_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Ext", bytes, buf);
    // The blob span is identical (start/end) in both indexes.
    const bbq::FieldCapture* vb = bbq::node_child(vm.root, "blob");
    const bbq::FieldCapture* cb = bbq::node_child(ck, "blob");
    ASSERT_NE(vb, nullptr); ASSERT_NE(cb, nullptr);
    EXPECT_EQ(vb->start_offset, cb->start_offset);
    EXPECT_EQ(vb->end_offset, cb->end_offset);
    EXPECT_EQ(vb->end_offset - vb->start_offset, 4u);
    zr::Ext h(vm.root, buf, nullptr);
    EXPECT_EQ(h.tag(), 0xAA);
    EXPECT_EQ(h.tail(), 0xBB);
    bbq::bytes_view blob = h.blob();
    ASSERT_EQ(blob.size, 4u);
    EXPECT_EQ(blob.data[0], 0x01); EXPECT_EQ(blob.data[3], 0x04);
}

TEST(RenderViewParser, TopLevelSwitchRuleMatchesCek) {
    // top-level switch RULE: discriminant is peek() (the unconsumed next byte). The
    // arm is the root's anonymous children[0]; variant_tag records which arm matched.
    {   // peek=1 → case 0 (Pair); peek doesn't consume, so Pair reads from byte 0.
        std::vector<uint8_t> bytes = { 0x01, 0x22 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::TopSw_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        const bbq::FieldCapture* ck = cek_root(kSpec, "TopSw", bytes, buf);
        EXPECT_EQ(vm.root->children[0].variant_tag, ck->children[0].variant_tag);
        zr::TopSw h(vm.root, buf, nullptr);
        EXPECT_EQ(h.which(), 0);
        EXPECT_EQ(h.case_0().p(), 0x01);
        EXPECT_EQ(h.case_0().q(), 0x22);
    }
    {   // peek=9 → default (uint8 = byte 0).
        std::vector<uint8_t> bytes = { 0x09 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::TopSw_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        const bbq::FieldCapture* ck = cek_root(kSpec, "TopSw", bytes, buf);
        EXPECT_EQ(vm.root->children[0].variant_tag, ck->children[0].variant_tag);
        zr::TopSw h(vm.root, buf, nullptr);
        EXPECT_EQ(h.which(), 1);
        EXPECT_EQ(h.default_val(), 0x09);
    }
}

TEST(RenderViewParser, NestedPathCountMatchesCek) {
    // The array count `h.n` is a NESTED field-path reference: navigate into the
    // completed `h` sub-struct and decode its `n` leaf (PathStart/FieldAccess/
    // CaptureValue), not a c-owning `out->h.n` struct member. h.n=3 → xs has 3.
    std::vector<uint8_t> bytes = { 0x03, 0xAA, 0xBB, 0xCC };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Np_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Np", bytes, buf);
    const bbq::FieldCapture* va = bbq::node_child(vm.root, "xs");
    const bbq::FieldCapture* ca = bbq::node_child(ck, "xs");
    ASSERT_NE(va, nullptr); ASSERT_NE(ca, nullptr);
    ASSERT_EQ(va->child_count, ca->child_count);
    ASSERT_EQ(va->child_count, 3);
    zr::Np h(vm.root, buf, nullptr);
    EXPECT_EQ(h.h().n(), 3);
    bbq::prim_seq<uint8_t> xs = h.xs();
    ASSERT_EQ(xs.size(), 3u);
    EXPECT_EQ(xs[0], 0xAA); EXPECT_EQ(xs[2], 0xCC);
}

TEST(RenderViewParser, OuterScopeRefFromArrayMatchesCek) {
    // The per-element interval `@[base + @index, ...]` references `base`, a field in
    // the ENCLOSING scope (parsed before the array opened) — the elf64 shdrs shape.
    // The lookup must walk outward from the array scope. base=3, cnt=3 → items read
    // from offsets 3,4,5 (random-access via the interval), skipping byte 2.
    std::vector<uint8_t> bytes = { 0x03, 0x03, 0xFF, 0x10, 0x20, 0x30 };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::OScope_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "OScope", bytes, buf);
    const bbq::FieldCapture* va = bbq::node_child(vm.root, "items");
    const bbq::FieldCapture* ca = bbq::node_child(ck, "items");
    ASSERT_NE(va, nullptr); ASSERT_NE(ca, nullptr);
    ASSERT_EQ(va->child_count, ca->child_count);
    for (int i = 0; i < va->child_count; i++) {
        int64_t a = 0, b = 0; bool s = false;
        bbq::decode_int(&va->children[i], buf, &a, &s);
        bbq::decode_int(&ca->children[i], buf, &b, &s);
        EXPECT_EQ(a, b) << i;
    }
    zr::OScope h(vm.root, buf, nullptr);
    bbq::prim_seq<uint8_t> items = h.items();
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0], 0x10); EXPECT_EQ(items[1], 0x20); EXPECT_EQ(items[2], 0x30);
}

TEST(RenderViewParser, AllPrimitivesMatchCek) {
    // Every primitive width/sign/endian/kind, view reader vs CEK, anchored to known bytes.
    std::vector<uint8_t> bytes = {
        0x12,                                           // u8  = 0x12
        0xFE,                                           // i8  = -2
        0x56, 0x34,                                     // u16le = 0x3456
        0x01, 0x02,                                     // u16be = 0x0102
        0xFD, 0xFF,                                     // i16le = -3
        0x00, 0x01, 0x00, 0x00,                         // u32le = 256
        0x01, 0x02, 0x03, 0x04,                         // u32be = 0x01020304
        0xFC, 0xFF, 0xFF, 0xFF,                         // i32le = -4
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // u64le = 256
        0xFB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // i64le = -5
        0x00, 0x00, 0xC0, 0x3F,                         // f32le = 1.5
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // f64le = 2.5
        0x01 };                                         // bool  = true
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::AllPrim_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "AllPrim", bytes, buf);
    for (const char* f : {"u8","i8","u16","u16b","i16","u32","u32b","i32","u64","i64","fl"})
        EXPECT_EQ(dec(vm.root, f, buf), dec(ck, f, buf)) << f;       // int/bool: view == CEK
    for (const char* f : {"f32","f64"}) {                            // float: view == CEK
        double a = 0, b = 0;
        bbq::decode_float(bbq::node_child(vm.root, f), buf, &a);
        bbq::decode_float(bbq::node_child(ck, f), buf, &b);
        EXPECT_EQ(a, b) << f;
    }
    zr::AllPrim h(vm.root, buf, nullptr);                            // known values via the handle
    EXPECT_EQ(h.u8(), 0x12);   EXPECT_EQ(h.i8(), -2);
    EXPECT_EQ(h.u16(), 0x3456); EXPECT_EQ(h.u16b(), 0x0102); EXPECT_EQ(h.i16(), -3);
    EXPECT_EQ(h.u32(), 256u);  EXPECT_EQ(h.u32b(), 0x01020304u); EXPECT_EQ(h.i32(), -4);
    EXPECT_EQ(h.u64(), 256u);  EXPECT_EQ(h.i64(), -5);
    EXPECT_FLOAT_EQ(h.f32(), 1.5f); EXPECT_DOUBLE_EQ(h.f64(), 2.5);
    EXPECT_TRUE(h.fl());
}

TEST(RenderViewParser, SwitchRangeCaseMatchesCek) {
    {   // tag=2 falls in the range 1..3 → uint16le arm
        std::vector<uint8_t> bytes = { 0x02, 0x34, 0x12 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::SwRange_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        const bbq::FieldCapture* ck = cek_root(kSpec, "SwRange", bytes, buf);
        const bbq::FieldCapture* vb = bbq::node_child(vm.root, "body");
        const bbq::FieldCapture* cb = bbq::node_child(ck, "body");
        ASSERT_NE(vb, nullptr); ASSERT_NE(cb, nullptr);
        EXPECT_EQ(vb->variant_tag, cb->variant_tag);   // same arm ordinal as the CEK
        zr::SwRange h(vm.root, buf, nullptr);
        EXPECT_EQ(h.body_which(), 0);
        EXPECT_EQ(h.body_case_0(), 0x1234);
    }
    {   // tag=9 → default arm (uint8)
        std::vector<uint8_t> bytes = { 0x09, 0x2A };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::SwRange_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::SwRange h(vm.root, buf, nullptr);
        EXPECT_EQ(h.body_which(), 1);
        EXPECT_EQ(h.body_default_val(), 0x2A);
    }
}

TEST(RenderViewParser, TernaryComputeMatchesCek) {
    {   // f=5 > 0 → g = f*2 = 10
        std::vector<uint8_t> bytes = { 0x05 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Tern_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        const bbq::FieldCapture* ck = cek_root(kSpec, "Tern", bytes, buf);
        EXPECT_EQ(bbq::node_computed_int(bbq::node_child(vm.root, "g")),
                  bbq::node_computed_int(bbq::node_child(ck, "g")));
        zr::Tern h(vm.root, buf, nullptr);
        EXPECT_EQ(h.g(), 10);
    }
    {   // f=0 → g = 99 (else branch)
        std::vector<uint8_t> bytes = { 0x00 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::Tern_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::Tern h(vm.root, buf, nullptr);
        EXPECT_EQ(h.g(), 99);
    }
}

TEST(RenderViewParser, StartBuiltinMatchesCek) {
    // @start (the current scope's start offset) in a where — 0 at the root scope.
    std::vector<uint8_t> bytes = { 0x0A, 0x0B };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Bstart_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Bstart", bytes, buf);
    ASSERT_NE(ck, nullptr);
    zr::Bstart h(vm.root, buf, nullptr);
    EXPECT_EQ(h.pad(), 0x0A); EXPECT_EQ(h.val(), 0x0B);
}

TEST(RenderViewParser, RestWindowMatchesCek) {
    // size=2 bounds the rest to 2 bytes → a (uint16le) fills it exactly.
    std::vector<uint8_t> bytes = { 0x02, 0x34, 0x12 };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Rest_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Rest", bytes, buf);
    ASSERT_NE(ck, nullptr);
    EXPECT_EQ(dec(vm.root, "size", buf), dec(ck, "size", buf));
    EXPECT_EQ(dec(vm.root, "a", buf), dec(ck, "a", buf));
    zr::Rest h(vm.root, buf, nullptr);
    EXPECT_EQ(h.size(), 2); EXPECT_EQ(h.a(), 0x1234);
}

TEST(RenderViewParser, CountTermArrayMatchesCek) {
    // count(N) array (re-evaluated bound): n=3 → xs = {10,20,30}. Was #error-gated on
    // every compiled backend; the view now implements it CEK-faithfully.
    std::vector<uint8_t> bytes = { 0x03, 10, 20, 30 };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Cnt_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Cnt", bytes, buf);
    const bbq::FieldCapture* va = bbq::node_child(vm.root, "xs");
    const bbq::FieldCapture* ca = bbq::node_child(ck, "xs");
    ASSERT_NE(va, nullptr); ASSERT_NE(ca, nullptr);
    ASSERT_EQ(va->child_count, ca->child_count);
    ASSERT_EQ(va->child_count, 3);
    zr::Cnt h(vm.root, buf, nullptr);
    EXPECT_EQ(h.n(), 3);
    bbq::prim_seq<uint8_t> xs = h.xs();
    ASSERT_EQ(xs.size(), 3u);
    EXPECT_EQ(xs[0], 10); EXPECT_EQ(xs[1], 20); EXPECT_EQ(xs[2], 30);
}

TEST(RenderViewParser, LebScalarsMatchCek) {
    // a=300 (uleb128 AC 02), b=-5 (sleb64 7B), c=0x42 — two varints then a fixed byte.
    // The view must decode + advance by the consumed bytes (not a fixed width) and
    // record Computed captures, identical to the CEK.
    std::vector<uint8_t> bytes = { 0xAC, 0x02, 0x7B, 0x42 };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Leb_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Leb", bytes, buf);
    EXPECT_EQ(bbq::node_computed_int(bbq::node_child(vm.root, "a")),
              bbq::node_computed_int(bbq::node_child(ck, "a")));
    EXPECT_EQ(bbq::node_computed_int(bbq::node_child(vm.root, "b")),
              bbq::node_computed_int(bbq::node_child(ck, "b")));
    EXPECT_EQ(dec(vm.root, "c", buf), dec(ck, "c", buf));
    // The c capture's offset proves the varints advanced the cursor correctly.
    EXPECT_EQ(bbq::node_child(vm.root, "c")->start_offset, 3u);
    zr::Leb h(vm.root, buf, nullptr);
    EXPECT_EQ(h.a(), 300u);
    EXPECT_EQ(h.b(), -5);
    EXPECT_EQ(h.c(), 0x42);
}

TEST(RenderViewParser, LebArrayMatchesCek) {
    // n=3, xs = uleb128 {1, 300, 5} = 01, AC 02, 05 — variable-width elements.
    std::vector<uint8_t> bytes = { 0x03, 0x01, 0xAC, 0x02, 0x05 };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::LebArr_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "LebArr", bytes, buf);
    const bbq::FieldCapture* va = bbq::node_child(vm.root, "xs");
    const bbq::FieldCapture* ca = bbq::node_child(ck, "xs");
    ASSERT_NE(va, nullptr); ASSERT_NE(ca, nullptr);
    ASSERT_EQ(va->child_count, ca->child_count);
    ASSERT_EQ(va->child_count, 3);
    for (int i = 0; i < 3; i++)
        EXPECT_EQ(bbq::node_computed_int(&va->children[i]),
                  bbq::node_computed_int(&ca->children[i])) << i;
}

TEST(RenderViewParser, LebOptionalMatchesCek) {
    {   // f=1 → v present (uleb128 300), tail=0x42
        std::vector<uint8_t> bytes = { 0x01, 0xAC, 0x02, 0x42 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::LebOpt_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        const bbq::FieldCapture* ck = cek_root(kSpec, "LebOpt", bytes, buf);
        EXPECT_EQ(bbq::node_computed_int(bbq::node_child(vm.root, "v")),
                  bbq::node_computed_int(bbq::node_child(ck, "v")));
        EXPECT_EQ(dec(vm.root, "tail", buf), 0x42);
    }
    {   // f=0 → v absent, tail right after f
        std::vector<uint8_t> bytes = { 0x00, 0x42 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::LebOpt_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        EXPECT_EQ(bbq::node_child(vm.root, "v"), nullptr);
        EXPECT_EQ(dec(vm.root, "tail", buf), 0x42);
    }
}

TEST(RenderViewParser, NestedArrayMatchesCek) {
    // rows=2, cols=3 → data = [[1,2,3],[4,5,6]]; the view nests begin_array per row.
    std::vector<uint8_t> bytes = { 0x02, 0x03, 1, 2, 3, 4, 5, 6 };
    auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
    bbq::ParseArena arena;
    bbq::CaptureMetadata vm = zr::Mat_read(buf, bytes.size(), arena);
    ASSERT_TRUE(vm.success);
    const bbq::FieldCapture* ck = cek_root(kSpec, "Mat", bytes, buf);
    const bbq::FieldCapture* vd = bbq::node_child(vm.root, "data");
    const bbq::FieldCapture* cd = bbq::node_child(ck, "data");
    ASSERT_NE(vd, nullptr); ASSERT_NE(cd, nullptr);
    ASSERT_EQ(vd->child_count, cd->child_count);
    ASSERT_EQ(vd->child_count, 2);
    for (int r = 0; r < 2; r++) {
        ASSERT_EQ(vd->children[r].child_count, cd->children[r].child_count);
        ASSERT_EQ(vd->children[r].child_count, 3);
        for (int c = 0; c < 3; c++) {
            int64_t a = 0, b = 0; bool s = false;
            bbq::decode_int(&vd->children[r].children[c], buf, &a, &s);
            bbq::decode_int(&cd->children[r].children[c], buf, &b, &s);
            EXPECT_EQ(a, b) << r << "," << c;
        }
    }
    zr::Mat h(vm.root, buf, nullptr);
    bbq::seq<bbq::prim_seq<uint8_t>> data = h.data();
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0][0], 1); EXPECT_EQ(data[0][2], 3);
    EXPECT_EQ(data[1][0], 4); EXPECT_EQ(data[1][2], 6);
}

TEST(RenderViewParser, OptionalSpanMatchesCek) {
    {   // n=1 → ob (bytes[2]) + os (string[3]) present
        std::vector<uint8_t> bytes = { 0x01, 0xAA, 0xBB, 0x61, 0x62, 0x63 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::OptSpan_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::OptSpan h(vm.root, buf, nullptr);
        auto ob = h.ob();                       // span_opt<bytes_view, uint8_t>
        ASSERT_TRUE(ob.has_value());
        EXPECT_EQ(ob.value().size, 2u);
        EXPECT_EQ(ob.value().data[0], 0xAA); EXPECT_EQ(ob.value().data[1], 0xBB);
        auto os = h.os();                       // span_opt<string_view, char>
        ASSERT_TRUE(os.has_value());
        EXPECT_EQ(os.value(), std::string_view("abc"));
    }
    {   // n=0 → both absent
        std::vector<uint8_t> bytes = { 0x00 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::OptSpan_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::OptSpan h(vm.root, buf, nullptr);
        EXPECT_FALSE(h.ob().has_value());
        EXPECT_FALSE(h.os().has_value());
    }
}

TEST(RenderViewParser, TopLevelNonStructRules) {
    {   // top-level primitive typedef: the root is a single uint8
        std::vector<uint8_t> bytes = { 0x2A };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::TyByte_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::TyByte h(vm.root, buf, nullptr);
        EXPECT_EQ(h.value(), 0x2A);
    }
    {   // top-level rule alias: the root is a Pair
        std::vector<uint8_t> bytes = { 0x07, 0x08 };
        auto* buf = new uint8_t[bytes.size()]; std::copy(bytes.begin(), bytes.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::TyRule_read(buf, bytes.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::TyRule h(vm.root, buf, nullptr);
        zr::Pair inr = h.value();
        EXPECT_EQ(inr.p(), 7); EXPECT_EQ(inr.q(), 8);
    }
    {   // top-level bare optional present + absent
        std::vector<uint8_t> present = { 0x2A };
        auto* buf = new uint8_t[present.size()]; std::copy(present.begin(), present.end(), buf);
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = zr::OptTop_read(buf, present.size(), arena);
        ASSERT_TRUE(vm.success);
        zr::OptTop h(vm.root, buf, nullptr);
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ(h.value(), 0x2A);
    }
}

// ── NEGATIVE axis: truncation must reject exactly like the CEK ───────────────
// Denominator = the CEK feature set (every fixture rule). For each rule, feed every
// prefix of its valid bytes (len 0..N) to the view reader and the CEK over the SAME
// length bound: the reader must accept iff the CEK accepts — and never overrun or
// crash. This is the failure surface the positive tests above cannot see.

using ViewReadFn = bbq::CaptureMetadata (*)(const uint8_t*, size_t, bbq::ParseArena&);

namespace {
struct RuleCase { const char* rule; ViewReadFn read; std::vector<uint8_t> valid; };

std::vector<RuleCase> all_rule_cases() {
    return {
        {"Flat", zr::Flat_read, {0x12,0x56,0x34,0x00,0x01,0x00,0x00,0x01,0x02,0xFC,0xFF,0xFF,0xFF}},
        {"Nest", zr::Nest_read, {0x07,0x09,0x00,0x0B}},
        {"Arr", zr::Arr_read, {0x03,0x10,0x00,0x20,0x00,0x30,0x00}},
        {"Pair", zr::Pair_read, {0x07,0x08}},
        {"Outer", zr::Outer_read, {0x55,0x07,0x08}},
        {"RArr", zr::RArr_read, {0x02,1,2,3,4}},
        {"Bytes", zr::Bytes_read, {0x03,0xAA,0xBB,0xCC}},
        {"Str", zr::Str_read, {0x61,0x62,0x63}},
        {"WhTwice", zr::WhTwice_read, {0x05,0x0A,0x2A}},
        {"Wh", zr::Wh_read, {0x01,0x2A}},
        {"Comp", zr::Comp_read, {0xA5}},
        {"Iv", zr::Iv_read, {0x02,0xFF,0x2A}},
        {"Pos", zr::Pos_read, {0x0A,0x0B}},
        {"Op", zr::Op_read, {0x01,0x34,0x12,0x05,0x06}},
        {"Bare", zr::Bare_read, {0x0A,0x2A}},
        {"Sw", zr::Sw_read, {0x01,0x34,0x12}},
        {"VA", zr::VA_read, {0x01,0x0A}},
        {"VB", zr::VB_read, {0x02,0x0B}},
        {"U", zr::U_read, {0x01,0x0A}},
        {"Alts", zr::Alts_read, {0x02,0x0B}},
        {"Bf", zr::Bf_read, {0xA5}},
        {"InlineBf", zr::InlineBf_read, {0x11,0xA5,0x22}},
        {"Eof", zr::Eof_read, {1,2,3,4}},
        {"Unt", zr::Unt_read, {1,2,3}},
        {"Es", zr::Es_read, {0x34,0x12,0x12,0x34}},
        {"RsItem", zr::RsItem_read, {0x05}},
        {"Resync", zr::Resync_read, {0x02,0x00,0x05,0x00,0x07}},
        {"Ext", zr::Ext_read, {0xAA,0x01,0x02,0x03,0x04,0xBB}},
        {"SwRange", zr::SwRange_read, {0x02,0x34,0x12}},
        {"Tern", zr::Tern_read, {0x05}},
        {"Bstart", zr::Bstart_read, {0x0A,0x0B}},
        {"Rest", zr::Rest_read, {0x02,0x34,0x12}},
        {"RestEof", zr::RestEof_read, {0x03,10,20,30}},
        {"Cnt", zr::Cnt_read, {0x03,10,20,30}},
        {"TopSw", zr::TopSw_read, {0x01,0x22}},
        {"Np", zr::Np_read, {0x03,0xAA,0xBB,0xCC}},
        {"OScope", zr::OScope_read, {0x03,0x03,0xFF,0x10,0x20,0x30}},
        {"AllPrim", zr::AllPrim_read, {0x12,0xFE,0x56,0x34,0x01,0x02,0xFD,0xFF,
            0x00,0x01,0x00,0x00,0x01,0x02,0x03,0x04,0xFC,0xFF,0xFF,0xFF,
            0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xFB,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0x00,0x00,0xC0,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x40,0x01}},
        {"Leb", zr::Leb_read, {0xAC,0x02,0x7B,0x42}},
        {"LebArr", zr::LebArr_read, {0x03,0x01,0xAC,0x02,0x05}},
        {"LebOpt", zr::LebOpt_read, {0x01,0xAC,0x02,0x42}},
        {"Mat", zr::Mat_read, {0x02,0x03,1,2,3,4,5,6}},
        {"OptSpan", zr::OptSpan_read, {0x01,0xAA,0xBB,0x61,0x62,0x63}},
        {"TyByte", zr::TyByte_read, {0x2A}},
        {"TyRule", zr::TyRule_read, {0x07,0x08}},
        {"OptTop", zr::OptTop_read, {0x2A}},
        {"NestGrp", zr::NestGrp_read, {0x02,0xAA,0xBB}},
        {"NestArr", zr::NestArr_read, {0x02, 0x01,0x11, 0x02,0x22,0x33}},
    };
}
}  // namespace

TEST(RenderViewParser, TruncationRejectsLikeCek) {
    for (auto& c : all_rule_cases()) {
        for (size_t L = 0; L <= c.valid.size(); L++) {
            bbq::ParseArena arena;
            bbq::CaptureMetadata vm = c.read(c.valid.data(), L, arena);
            bbq::CaptureMetadata ck = cek_meta(kSpec, c.rule, c.valid.data(), L);
            EXPECT_EQ(vm.success, ck.success)
                << c.rule << " @ prefix " << L << "/" << c.valid.size()
                << "  (view=" << vm.success << " cek=" << ck.success << ")";
        }
    }
}

// ── NEGATIVE axis: single-byte corruption must reject exactly like the CEK ───
// Flip each byte to 0x00 / 0xFF / +1 and assert the view reader accepts iff the CEK
// accepts. Catches over-large counts/lengths, bad switch tags, broken discriminants,
// and constraint violations — the reader must AGREE with the CEK on every malformed
// input (and never overrun/crash), which the positive tests cannot exercise.
TEST(RenderViewParser, ByteCorruptionRejectsLikeCek) {
    for (auto& c : all_rule_cases()) {
        for (size_t pos = 0; pos < c.valid.size(); pos++) {
            for (uint8_t nv : {uint8_t(0x00), uint8_t(0xFF), uint8_t(c.valid[pos] + 1)}) {
                if (nv == c.valid[pos]) continue;
                std::vector<uint8_t> b = c.valid;
                b[pos] = nv;
                bbq::ParseArena arena;
                bbq::CaptureMetadata vm = c.read(b.data(), b.size(), arena);
                bbq::CaptureMetadata ck = cek_meta(kSpec, c.rule, b.data(), b.size());
                EXPECT_EQ(vm.success, ck.success)
                    << c.rule << " byte[" << pos << "]=0x" << std::hex << (int)nv
                    << "  (view=" << vm.success << " cek=" << ck.success << ")";
            }
        }
    }
}
