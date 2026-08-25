// render_types_cpp_parity_test — CEK ↔ C++ ZCow parity (step 4.1).
//
// The bar for the C++ types is NOT "the header compiles" (that is the old
// compile-only render_types_cpp_test). It is: for the same grammar + bytes, the
// generated C++ handle decodes the IDENTICAL value the CEK index holds. The CEK
// is the reference; the C++ handle classes are the typed face over the SAME
// document. This is the prototype-in-Python → compile-to-C++ "zero
// drama" guarantee, mechanized.
//
// Mechanism: compile the fixture grammar, run the CEK on test bytes to build the
// index (meta.root), then read each field BOTH ways — through the generated
// handle and through the shared decoder over the index — and assert they match.
// The handle and the Python binding share backends/cpp/runtime/CaptureDecode.h,
// so handle==index-decode == what Python would return.
//
// The grammar is test/fixtures/zcow_parity.bbq, generated to ZcowParityTypes.h by
// cmake (bbqc -backend cpp). It is matrix-driven and grows as each gate (step 4.2)
// opens — never cut to elf64's subset.
#include <gtest/gtest.h>
#include "Parser.h"
#include "Sema.h"
#include "bbq_compile.h"     // bbq::Compiler / CompiledGrammar
#include "Machine.h"         // bbq::cek::CEKMachine
#include "ParseArena.h"      // bbq::ParseArena
#include "bbq_node.h"        // bbq::node_child / decode_int / bytes_view / seq + Capture
#include "ZcowParityTypes.h" // generated handle classes (namespace zp)

#include <cstdint>
#include <string_view>
#include <vector>

using namespace BBQ;
using namespace bbqgen;

namespace {

// Compile the fixture grammar, run the CEK over `bytes`, return the parse.
// The arena/grammar are leaked deliberately — the test process is short-lived and
// meta.root points into the arena, which must outlive every assertion.
struct Parsed {
    bbq::cek::CompiledGrammar* grammar;
    bbq::zcow::parse_result meta;
    const uint8_t* buf;

    // A handle holds where it is, so the document has to outlive it. Read-only: no
    // transient, so a setter on a handle built from these raises.
    const bbq::zcow::document& doc() const { return meta.doc; }
    bbq::zcow::node* node() const { return const_cast<bbq::zcow::node*>(meta.doc.root()); }
    const bbq::zcow::source* src() const { return &meta.doc.src(); }
};

// A test extern parser: consumes a fixed 4 bytes. Registered as "readit" so the
// extern fixture parses (the index records the consumed span; the C++ handle reads it).
bool test_extern_read4(const uint8_t*, size_t len, size_t* consumed, bbq::ParseArena*, void*) {
    if (len < 4) return false;
    *consumed = 4;
    return true;
}

Parsed cek_parse(const char* spec, const char* rule, const std::vector<uint8_t>& bytes) {
    auto* parser = new Parser();
    parser->init(spec, (int)std::char_traits<char>::length(spec));
    EXPECT_TRUE(parser->parse()) << "fixture parse failed";

    static bbqgen::ErrorReporter rep;
    auto* sema = new Sema(rep);
    EXPECT_TRUE(sema->analyze(parser->ast)) << "fixture sema failed";

    auto* compiler = new bbq::Compiler();
    auto* g = compiler->compile_grammar(parser->ast);
    EXPECT_NE(g, nullptr) << "compile_grammar null";

    auto* entry = g->lookup(std::string(rule));
    EXPECT_NE(entry, nullptr) << "no rule " << rule;

    auto* buf = new uint8_t[bytes.size()];
    std::copy(bytes.begin(), bytes.end(), buf);

    // Register the "readit" extern (interned in this grammar's pool for ptr-equality).
    auto* ext = new bbq::cek::ExternalParserTable::Entry[1];
    ext[0].name = g->strings.intern(std::string("readit"));
    ext[0].fn = &test_extern_read4;
    ext[0].user_data = nullptr;
    auto* ext_table = new bbq::cek::ExternalParserTable();
    ext_table->entries = ext;
    ext_table->count = 1;

    auto* arena = new bbq::ParseArena();
    auto* machine = new bbq::cek::CEKMachine();
    machine->arena = arena;
    machine->builtins = &g->builtins;
    machine->ext_parsers = ext_table;
    bbq::zcow::parse_result meta =
        machine->execute_from(entry, buf, bytes.size(), g->default_little_endian);
    return {g, std::move(meta), buf};
}

// The index-side decode (the CEK reference): pull a scalar child by name and
// decode it through the SHARED decoder the handle also uses.
int64_t index_int(const bbq::zcow::node* parent, const char* name, const uint8_t* buf) {
    const bbq::zcow::node* c = bbq::node_child(parent, name);
    EXPECT_NE(c, nullptr) << "no child " << name;
    return bbq::node_int(c, buf);
}

double index_float(const bbq::zcow::node* parent, const char* name, const uint8_t* buf) {
    const bbq::zcow::node* c = bbq::node_child(parent, name);
    EXPECT_NE(c, nullptr) << "no child " << name;
    return bbq::node_float(c, buf);
}

}  // namespace

// Spec text must match test/fixtures/zcow_parity.bbq (cmake generates the header
// from that file; this string drives the CEK side). Kept inline so the parse and
// the codegen come from the same source of truth at review time.
static const char* kFixture =
    "@endian little\n"
    "Inner = struct { a: uint8, b: uint16le }\n"
    "Flat = struct {\n"
    "  u8: uint8, u16: uint16le, u32: uint32le, be16: uint16be,\n"
    "  i8: int8, i16: int16le, i32: int32le, flag: bool,\n"
    "  f32: float32le, f64: float64le,\n"
    "  raw: bytes[2], s: string[3], hi: compute((u8 >> 4) & 0x0F : uint8),\n"
    "  inner: Inner, n: uint8, items: array<Inner>[n],\n"
    "  m: uint8, nums: array<uint16le>[m]\n"
    "}\n"
    "Opt = struct {\n"
    "  pflag: uint8, pmaybe: optional<uint16le> where pflag == 1,\n"
    "  rflag: uint8, rmaybe: optional<Inner> where rflag == 1\n"
    "}\n"
    "Bits = bitfield<uint8> { hi: 4, lo: 4 }\n"
    "Nest = struct { hdr: struct { a: uint8, b: uint16le }, c: uint8 }\n"
    "InlineBits = struct { lead: uint8,\n"
    "  flags: bitfield<uint8> { hi: 4, lo: 4 }, tail: uint8 }\n"
    "Pair = struct { p: uint8, q: uint8 }\n"
    "U = union { asInner: Inner, asPair: Pair }\n"
    "Alts = Inner | Pair\n"
    "Sw = struct { tag: uint8,\n"
    "  body: switch(tag) { 1: uint16le; 2: Inner; default: uint8; } }\n"
    "TopSw = switch(peek()) { 1: Inner; default: uint8; }\n"
    "OptSpan = struct { n: uint8, ob: optional<bytes[2]> where n == 1,\n"
    "  os: optional<string[3]> where n == 1 }\n"
    "Ext = struct { tag: uint8, blob: extern(\"readit\", \"uint32_t\"), tail: uint8 }\n"
    "Matrix = struct { rows: uint8, cols: uint8, data: array<array<uint8>[cols]>[rows] }\n"
    "TyByte = uint8\n"
    "TyInner = Inner\n"
    "OptTop = optional<uint8>\n";

// Bytes laid out for the fields above (little-endian unless suffixed be):
//  u8=0x12  u16=0x3456  u32=256  be16=258  raw={0xAA,0xBB}
//  inner{a=7,b=9}  n=2  items[0]{a=33,b=1} items[1]{a=34,b=2}
static const std::vector<uint8_t> kBytes = {
    0x12,                   // u8
    0x56, 0x34,             // u16le = 0x3456
    0x00, 0x01, 0x00, 0x00, // u32le = 256
    0x01, 0x02,             // be16  = 258
    0xFE,                   // i8  = -2
    0xFD, 0xFF,             // i16 = -3
    0xFC, 0xFF, 0xFF, 0xFF, // i32 = -4
    0x01,                   // flag = true
    0x00, 0x00, 0xC0, 0x3F, // f32 = 1.5
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // f64 = 2.5
    0xAA, 0xBB,             // raw[2]
    0x61, 0x62, 0x63,       // s = "abc"
    0x07, 0x09, 0x00,       // inner: a=7, b=9
    0x02,                   // n=2
    0x21, 0x01, 0x00,       // items[0]: a=33, b=1
    0x22, 0x02, 0x00,       // items[1]: a=34, b=2
    0x03,                   // m=3
    0x0A, 0x00, 0x14, 0x00, 0x1E, 0x00, // nums = {10, 20, 30} (uint16le)
};

TEST(RenderTypesCppParity, FlatScalarsMatchCek) {
    Parsed p = cek_parse(kFixture, "Flat", kBytes);
    ASSERT_TRUE(p.meta.success) << "CEK parse failed";
    ASSERT_NE(p.meta.doc.root(), nullptr);
    // execute_from(rule_entry) returns the struct node itself as root; its
    // children are the fields (the Python binding reads meta.root the same way).
    const bbq::zcow::node* flat =p.meta.doc.root();

    zp::Flat h(p.node(), p.src());

    // Each scalar: handle == the value the CEK index decodes (parity), and the
    // value matches the known bytes (correctness).
    EXPECT_EQ(h.u8(),   index_int(flat, "u8",   p.buf)); EXPECT_EQ(h.u8(),   0x12);
    EXPECT_EQ(h.u16(),  index_int(flat, "u16",  p.buf)); EXPECT_EQ(h.u16(),  0x3456);
    EXPECT_EQ(h.u32(),  index_int(flat, "u32",  p.buf)); EXPECT_EQ(h.u32(),  256u);
    EXPECT_EQ(h.be16(), index_int(flat, "be16", p.buf)); EXPECT_EQ(h.be16(), 258);
    EXPECT_EQ(h.hi(),   1);  // compute((0x12>>4)&0x0F)
}

TEST(RenderTypesCppParity, CowMutateRoundTrips) {
    // The handle's setters write CoW overrides the getters then read back — int via
    // set_int/get_int, float via set_float/get_float (NOT truncating set_int).
    Parsed p = cek_parse(kFixture, "Flat", kBytes);
    ASSERT_TRUE(p.meta.success);
    // A handle over a transient writes; the same class over a document refuses (see
    // below). Mutability is a property of the document, not of a second handle type.
    bbq::zcow::transient t = p.meta.doc.begin_edit();
    zp::Flat h = zp::Flat_of(t);

    EXPECT_EQ(h.u16(), 0x3456);          // baseline
    h.set_u16(0x0042);
    EXPECT_EQ(h.u16(), 0x0042);          // read back through the same document

    EXPECT_FLOAT_EQ(h.f32(), 1.5f);      // baseline
    h.set_f32(3.25f);
    EXPECT_FLOAT_EQ(h.f32(), 3.25f);     // float override — precise, not truncated to int
}

TEST(RenderTypesCppParity, SignedBoolFloatMatchCek) {
    Parsed p = cek_parse(kFixture, "Flat", kBytes);
    ASSERT_TRUE(p.meta.success) << "CEK parse failed";
    const bbq::zcow::node* flat =p.meta.doc.root();
    zp::Flat h(p.node(), p.src());

    EXPECT_EQ(h.i8(),  index_int(flat, "i8",  p.buf)); EXPECT_EQ(h.i8(),  -2);
    EXPECT_EQ(h.i16(), index_int(flat, "i16", p.buf)); EXPECT_EQ(h.i16(), -3);
    EXPECT_EQ(h.i32(), index_int(flat, "i32", p.buf)); EXPECT_EQ(h.i32(), -4);
    EXPECT_EQ(h.flag(), (bool)index_int(flat, "flag", p.buf)); EXPECT_TRUE(h.flag());
    EXPECT_EQ(h.f32(), (float)index_float(flat, "f32", p.buf)); EXPECT_FLOAT_EQ(h.f32(), 1.5f);
    EXPECT_EQ(h.f64(), index_float(flat, "f64", p.buf)); EXPECT_DOUBLE_EQ(h.f64(), 2.5);
}

TEST(RenderTypesCppParity, BytesAndNestedMatchCek) {
    Parsed p = cek_parse(kFixture, "Flat", kBytes);
    ASSERT_TRUE(p.meta.success);
    const bbq::zcow::node* flat =p.meta.doc.root();
    zp::Flat h(p.node(), p.src());

    bbq::bytes_view raw = h.raw();
    ASSERT_EQ(raw.size, 2u);
    EXPECT_EQ(raw.data[0], 0xAA);
    EXPECT_EQ(raw.data[1], 0xBB);

    EXPECT_EQ(h.s(), std::string_view("abc"));

    zp::Inner inner = h.inner();
    EXPECT_EQ(inner.a(), 7);
    EXPECT_EQ(inner.b(), 9);
}

TEST(RenderTypesCppParity, ArrayOfRuleMatchCek) {
    Parsed p = cek_parse(kFixture, "Flat", kBytes);
    ASSERT_TRUE(p.meta.success);
    const bbq::zcow::node* flat =p.meta.doc.root();
    zp::Flat h(p.node(), p.src());

    EXPECT_EQ(h.n(), 2);
    zp::seq<zp::Inner> items = h.items();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].a(), 33); EXPECT_EQ(items[0].b(), 1);
    EXPECT_EQ(items[1].a(), 34); EXPECT_EQ(items[1].b(), 2);
}

TEST(RenderTypesCppParity, ArrayOfPrimMatchCek) {
    Parsed p = cek_parse(kFixture, "Flat", kBytes);
    ASSERT_TRUE(p.meta.success);
    const bbq::zcow::node* flat =p.meta.doc.root();
    zp::Flat h(p.node(), p.src());

    EXPECT_EQ(h.m(), 3);
    zp::prim_seq<uint16_t> nums = h.nums();
    ASSERT_EQ(nums.size(), 3u);
    EXPECT_EQ(nums[0], 10);
    EXPECT_EQ(nums[1], 20);
    EXPECT_EQ(nums[2], 30);

    // Parity against the index decode of each element.
    const bbq::zcow::node* arr = bbq::node_child(flat, "nums");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->kids.size(), 3u);
    for (size_t i = 0; i < arr->kids.size(); i++)
        EXPECT_EQ(nums[i], (uint16_t)bbq::node_int(arr->kids[i].get(), p.buf));
}

TEST(RenderTypesCppParity, OptionalPresent) {
    // pflag=1 → pmaybe present (0x0102=258); rflag=1 → rmaybe present (Inner{a=5,b=6}).
    Parsed p = cek_parse(kFixture, "Opt",
                         {0x01, 0x02, 0x01, 0x01, 0x05, 0x06, 0x00});
    ASSERT_TRUE(p.meta.success);
    zp::Opt h(p.node(), p.src());

    zp::prim_opt<uint16_t> pm = h.pmaybe();
    ASSERT_TRUE(pm.has_value());
    EXPECT_EQ(pm.value(), 0x0102);
    EXPECT_EQ(pm.value(), (uint16_t)index_int(p.meta.doc.root(), "pmaybe", p.buf));

    zp::opt<zp::Inner> rm = h.rmaybe();
    ASSERT_TRUE(rm.has_value());
    EXPECT_EQ(rm.value().a(), 5);
    EXPECT_EQ(rm.value().b(), 6);
}

TEST(RenderTypesCppParity, OptionalAbsent) {
    // pflag=0 → pmaybe absent; rflag=0 → rmaybe absent (no children for either).
    Parsed p = cek_parse(kFixture, "Opt", {0x00, 0x00});
    ASSERT_TRUE(p.meta.success);
    zp::Opt h(p.node(), p.src());

    EXPECT_FALSE(h.pmaybe().has_value());
    EXPECT_FALSE(h.rmaybe().has_value());
    // The index agrees: no child by either name.
    EXPECT_EQ(bbq::node_child(p.meta.doc.root(), "pmaybe"), nullptr);
    EXPECT_EQ(bbq::node_child(p.meta.doc.root(), "rmaybe"), nullptr);
}

TEST(RenderTypesCppParity, BitfieldMatchCek) {
    // byte 0xA5, little-endian (lsb-first): hi=[0,4)=0x5, lo=[4,8)=0xA.
    Parsed p = cek_parse(kFixture, "Bits", {0xA5});
    ASSERT_TRUE(p.meta.success);
    zp::Bits h(p.node(), p.src());

    EXPECT_EQ(h.hi(), 0x5);
    EXPECT_EQ(h.lo(), 0xA);
    // Parity: each entry is a Computed capture in the index. The handle reads through the
    // container rather than through that value — same answer, and it is what makes a write
    // visible, since the Computed value is what the parse decoded and does not move.
    EXPECT_EQ(h.hi(), (uint8_t)bbq::node_int(bbq::node_child(p.meta.doc.root(), "hi"), p.buf));
    EXPECT_EQ(h.lo(), (uint8_t)bbq::node_int(bbq::node_child(p.meta.doc.root(), "lo"), p.buf));
}

TEST(RenderTypesCppParity, BitfieldEntriesAreWritable) {
    // The caller names a field. No shift, no mask, no container width — the grammar knows
    // the layout, so the generated accessor carries it.
    Parsed p = cek_parse(kFixture, "Bits", {0xA5});
    ASSERT_TRUE(p.meta.success);
    bbq::zcow::transient t = p.meta.doc.begin_edit();
    zp::Bits h = zp::Bits_of(t);

    EXPECT_EQ(h.hi(), 0x5);
    h.set_hi(0x3);
    EXPECT_EQ(h.hi(), 0x3);          // read back through the same document
    EXPECT_EQ(h.lo(), 0xA);          // the neighbouring entry is untouched

    h.set_lo(0xC);
    EXPECT_EQ(h.hi(), 0x3);
    EXPECT_EQ(h.lo(), 0xC);
    EXPECT_EQ(t.serialize(), (std::vector<uint8_t>{0xC3}));

    // And the edited byte re-parses to the values that were set.
    Parsed p2 = cek_parse(kFixture, "Bits", t.serialize());
    ASSERT_TRUE(p2.meta.success);
    EXPECT_EQ(bbq::node_int(bbq::node_child(p2.meta.doc.root(), "hi"), p2.buf), 0x3);
    EXPECT_EQ(bbq::node_int(bbq::node_child(p2.meta.doc.root(), "lo"), p2.buf), 0xC);
}

TEST(RenderTypesCppParity, WritingThroughAReadOnlyDocumentIsRefused) {
    // One handle class either way. A handle built over a document has nothing to write
    // into, and says so rather than doing nothing — the check Python needs too, since
    // it has no const to carry the distinction.
    Parsed p = cek_parse(kFixture, "Bits", {0xA5});
    ASSERT_TRUE(p.meta.success);
    zp::Bits h = zp::Bits_of(p.meta.doc);
    EXPECT_EQ(h.hi(), 0x5);
    EXPECT_THROW(h.set_hi(0x3), bbq::zcow::type_error);
    EXPECT_EQ(h.hi(), 0x5);   // and nothing moved
}

TEST(RenderTypesCppParity, InlineStructMatchCek) {
    // hdr is an inline anonymous struct → its own handle class Nest_hdr.
    Parsed p = cek_parse(kFixture, "Nest", {0x07, 0x08, 0x00, 0x09});
    ASSERT_TRUE(p.meta.success);
    zp::Nest h(p.node(), p.src());

    zp::Nest_hdr hdr = h.hdr();
    EXPECT_EQ(hdr.a(), 7);
    EXPECT_EQ(hdr.b(), 8);
    EXPECT_EQ(h.c(), 9);

    // Parity: hdr is a sub-node; its fields decode from the index.
    const bbq::zcow::node*hn = bbq::node_child(p.meta.doc.root(), "hdr");
    ASSERT_NE(hn, nullptr);
    EXPECT_EQ(hdr.a(), (uint8_t)index_int(hn, "a", p.buf));
    EXPECT_EQ(hdr.b(), (uint16_t)index_int(hn, "b", p.buf));
}

TEST(RenderTypesCppParity, InlineBitfieldFieldMatchCek) {
    // flags is an inline bitfield → sub-node InlineBits_flags of computed entries.
    // byte 0xA5 little-endian: hi=0x5, lo=0xA.
    Parsed p = cek_parse(kFixture, "InlineBits", {0x11, 0xA5, 0x22});
    ASSERT_TRUE(p.meta.success);
    zp::InlineBits h(p.node(), p.src());

    EXPECT_EQ(h.lead(), 0x11);
    EXPECT_EQ(h.tail(), 0x22);
    zp::InlineBits_flags f = h.flags();
    EXPECT_EQ(f.hi(), 0x5);
    EXPECT_EQ(f.lo(), 0xA);

    // Parity: flags is a sub-node of computed entries.
    const bbq::zcow::node*fn = bbq::node_child(p.meta.doc.root(), "flags");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(f.hi(), (uint8_t)bbq::node_int(bbq::node_child(fn, "hi"), p.buf));
    EXPECT_EQ(f.lo(), (uint8_t)bbq::node_int(bbq::node_child(fn, "lo"), p.buf));
}

TEST(RenderTypesCppParity, UnionMatchedVariant) {
    // Inner is the first variant and always parses (no constraint), so it wins:
    // asInner present, recording variant_tag 0; asPair absent.
    Parsed p = cek_parse(kFixture, "U", {0x05, 0x06, 0x00});
    ASSERT_TRUE(p.meta.success);
    zp::U h(p.node(), p.src());

    EXPECT_EQ(h.which(), 0);
    EXPECT_TRUE(h.is_asInner());
    EXPECT_FALSE(h.is_asPair());
    zp::Inner inr = h.asInner();
    EXPECT_EQ(inr.a(), 5);
    EXPECT_EQ(inr.b(), 6);
}

TEST(RenderTypesCppParity, AlternativesMatchedVariant) {
    Parsed p = cek_parse(kFixture, "Alts", {0x05, 0x06, 0x00});
    ASSERT_TRUE(p.meta.success);
    zp::Alts h(p.node(), p.src());

    EXPECT_EQ(h.which(), 0);
    EXPECT_TRUE(h.is_alt_0());
    EXPECT_FALSE(h.is_alt_1());
    EXPECT_EQ(h.alt_0().a(), 5);
    EXPECT_EQ(h.alt_0().b(), 6);
}

TEST(RenderTypesCppParity, SwitchScalarArm) {
    // tag=1 → case 0 (uint16le). which() reads the recorded ordinal, not the disc.
    Parsed p = cek_parse(kFixture, "Sw", {0x01, 0x07, 0x00});
    ASSERT_TRUE(p.meta.success);
    zp::Sw h(p.node(), p.src());
    EXPECT_EQ(h.tag(), 1);
    EXPECT_EQ(h.body_which(), 0);
    EXPECT_EQ(h.body_case_0(), 7);
}

TEST(RenderTypesCppParity, SwitchRulerefArm) {
    // tag=2 → case 1 (Inner).
    Parsed p = cek_parse(kFixture, "Sw", {0x02, 0x05, 0x06, 0x00});
    ASSERT_TRUE(p.meta.success);
    zp::Sw h(p.node(), p.src());
    EXPECT_EQ(h.body_which(), 1);
    zp::Inner inr = h.body_case_1();
    EXPECT_EQ(inr.a(), 5);
    EXPECT_EQ(inr.b(), 6);
}

TEST(RenderTypesCppParity, SwitchDefaultArm) {
    // tag=9 → default (ordinal after the cases = 2), uint8.
    Parsed p = cek_parse(kFixture, "Sw", {0x09, 0x2A});
    ASSERT_TRUE(p.meta.success);
    zp::Sw h(p.node(), p.src());
    EXPECT_EQ(h.body_which(), 2);
    EXPECT_EQ(h.body_default_val(), 0x2A);
}

TEST(RenderTypesCppParity, TopLevelSwitchRuleRulerefArm) {
    // peek()==1 → case 0 (Inner); the arm is the root's anonymous first child.
    Parsed p = cek_parse(kFixture, "TopSw", {0x01, 0x55, 0x66});
    ASSERT_TRUE(p.meta.success);
    zp::TopSw h(p.node(), p.src());
    EXPECT_EQ(h.which(), 0);
    zp::Inner inr = h.case_0();
    EXPECT_EQ(inr.a(), 1);
    EXPECT_EQ(inr.b(), 0x6655);
}

TEST(RenderTypesCppParity, TopLevelSwitchRuleDefaultArm) {
    // peek()==9 → default (ordinal 1), uint8.
    Parsed p = cek_parse(kFixture, "TopSw", {0x09});
    ASSERT_TRUE(p.meta.success);
    zp::TopSw h(p.node(), p.src());
    EXPECT_EQ(h.which(), 1);
    EXPECT_EQ(h.default_val(), 9);
}

TEST(RenderTypesCppParity, OptionalSpan) {
    // n=1 → ob="\xCA\xFE" present, os="abc" present.
    Parsed p = cek_parse(kFixture, "OptSpan", {0x01, 0xCA, 0xFE, 0x61, 0x62, 0x63});
    ASSERT_TRUE(p.meta.success);
    zp::OptSpan h(p.node(), p.src());

    zp::span_opt<bbq::bytes_view, uint8_t> ob = h.ob();
    ASSERT_TRUE(ob.has_value());
    ASSERT_EQ(ob.value().size, 2u);
    EXPECT_EQ(ob.value().data[0], 0xCA);
    EXPECT_EQ(ob.value().data[1], 0xFE);

    zp::span_opt<std::string_view, char> os = h.os();
    ASSERT_TRUE(os.has_value());
    EXPECT_EQ(os.value(), std::string_view("abc"));
}

TEST(RenderTypesCppParity, ExternField) {
    // blob = the 4 bytes the extern consumed; ZCow reads the span as bytes_view.
    Parsed p = cek_parse(kFixture, "Ext", {0xAA, 0x01, 0x02, 0x03, 0x04, 0xBB});
    ASSERT_TRUE(p.meta.success);
    zp::Ext h(p.node(), p.src());

    EXPECT_EQ(h.tag(), 0xAA);
    bbq::bytes_view blob = h.blob();
    ASSERT_EQ(blob.size, 4u);
    EXPECT_EQ(blob.data[0], 0x01);
    EXPECT_EQ(blob.data[3], 0x04);
    EXPECT_EQ(h.tail(), 0xBB);
}

TEST(RenderTypesCppParity, NestedArray) {
    // 2x2 matrix [[1,2],[3,4]] — seq<prim_seq<uint8>> composes.
    Parsed p = cek_parse(kFixture, "Matrix", {2, 2, 1, 2, 3, 4});
    ASSERT_TRUE(p.meta.success);
    zp::Matrix h(p.node(), p.src());

    auto data = h.data();
    ASSERT_EQ(data.size(), 2u);
    ASSERT_EQ(data[0].size(), 2u);
    EXPECT_EQ(data[0][0], 1); EXPECT_EQ(data[0][1], 2);
    EXPECT_EQ(data[1][0], 3); EXPECT_EQ(data[1][1], 4);
}

TEST(RenderTypesCppParity, TopLevelTypedefRules) {
    {
        Parsed p = cek_parse(kFixture, "TyByte", {0x2A});
        ASSERT_TRUE(p.meta.success);
        zp::TyByte h(p.node(), p.src());
        EXPECT_EQ(h.value(), 0x2A);
    }
    {
        Parsed p = cek_parse(kFixture, "TyInner", {0x07, 0x09, 0x00});
        ASSERT_TRUE(p.meta.success);
        zp::TyInner h(p.node(), p.src());
        zp::Inner inr = h.value();
        EXPECT_EQ(inr.a(), 7);
        EXPECT_EQ(inr.b(), 9);
    }
}

TEST(RenderTypesCppParity, TopLevelOptionalRule) {
    Parsed p = cek_parse(kFixture, "OptTop", {0x2A});
    ASSERT_TRUE(p.meta.success);
    zp::OptTop h(p.node(), p.src());
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h.value(), 0x2A);
}

// ── Writer (ZCow emit) matrix ──────────────────────────────────────────────
// Serializing is exercised end-to-end through the Python binding; these lock the C++
// emit() decision logic and encoders directly, and what a structural edit leaves behind.

TEST(RenderTypesCppParity, EmitUnmutatedIsIdentity) {
    Parsed p = cek_parse(kFixture, "Flat", kBytes);
    ASSERT_TRUE(p.meta.success);
    EXPECT_EQ(p.meta.doc.serialize(), kBytes);   // nothing written == memcpy of the input
}

TEST(RenderTypesCppParity, EmitSameWidthPatchPreservesNeighbors) {
    Parsed p = cek_parse(kFixture, "Flat", kBytes);
    ASSERT_TRUE(p.meta.success);
    bbq::zcow::transient t = p.meta.doc.begin_edit();
    zp::Flat h = zp::Flat_of(t);
    h.set_u16(0x0042);                                       // fixed-width edit -> in-place patch
    std::vector<uint8_t> out = t.serialize();
    ASSERT_EQ(out.size(), kBytes.size());                   // patched in place, no growth
    for (size_t i = 0; i < out.size(); i++)                 // only the u16 bytes (offset 1..2) move
        if (i != 1 && i != 2) EXPECT_EQ(out[i], kBytes[i]) << i;
    Parsed p2 = cek_parse(kFixture, "Flat", out);
    ASSERT_TRUE(p2.meta.success);
    EXPECT_EQ(index_int(p2.meta.doc.root(), "u16", p2.buf), 0x0042);
    EXPECT_EQ(index_int(p2.meta.doc.root(), "u32", p2.buf), 256);  // neighbor intact
}

TEST(RenderTypesCppParity, EmitLebWidthChangeReserializes) {
    // A uleb128 leaf edited to a wider varint (5 -> 300: 1 -> 2 bytes) MUST
    // re-serialize, not in-place patch — patch_node is fixed-width and would write
    // over the 1-byte slot. The CEK parses LEB (the view reader's LEB lands later);
    // emit's patch-vs-reserialize decision is what's under test.
    const char* spec = "Leb = struct { v: uleb128, w: uint8 }";
    Parsed p = cek_parse(spec, "Leb", {0x05, 0xAA});
    ASSERT_TRUE(p.meta.success);
    bbq::zcow::transient t = p.meta.doc.begin_edit();
    const char* pv[] = {"v"};
    bbq::zcow::node* v = t.own_path(pv, nullptr, 1);
    ASSERT_NE(v, nullptr);
    bbq::zcow::set_int(v, 300, bbq::zcow::Enc::Uleb);
    std::vector<uint8_t> out = std::move(t).commit().serialize();
    EXPECT_EQ(out, (std::vector<uint8_t>{0xAC, 0x02, 0xAA}));  // 300 -> AC 02 ; w preserved
    Parsed p2 = cek_parse(spec, "Leb", out);
    ASSERT_TRUE(p2.meta.success);
    EXPECT_EQ(index_int(p2.meta.doc.root(), "v", p2.buf), 300);
    EXPECT_EQ(index_int(p2.meta.doc.root(), "w", p2.buf), 0xAA);
}

TEST(RenderTypesCppParity, WritingThenRemovingAnElementLeavesNothingBehind) {
    // Writing into an element and then dropping it leaves neither the value nor the
    // element: a removed subtree goes in one piece, and nothing it held can reappear.
    Parsed p = cek_parse(kFixture, "Flat", kBytes);
    ASSERT_TRUE(p.meta.success);
    const bbq::zcow::node* items = bbq::node_child(p.meta.doc.root(), "items");
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->kids.size(), 2u);

    bbq::zcow::transient t = p.meta.doc.begin_edit();
    const char* pi[] = {"items"};
    bbq::zcow::node* arr = t.own_path(pi, nullptr, 1);
    ASSERT_NE(arr, nullptr);
    bbq::zcow::set_int(t.own_child(t.own_child(arr, 0), 0), 99);
    ASSERT_TRUE(t.remove(arr, 0));

    EXPECT_EQ(bbq::zcow::size_of(bbq::zcow::child_named(t.root(), "items")), 1u);
    // ...and the document it came from is untouched, element and value alike.
    EXPECT_EQ(bbq::node_child(p.meta.doc.root(), "items")->kids.size(), 2u);
}
