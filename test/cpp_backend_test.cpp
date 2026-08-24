// The generated C++ backend, tested on its own.
//
// The reader is a lowering of the same IR the CEK evaluates, so it has to produce the
// same document — and a document it produced has to behave the same way. Checking only
// that the two indexes match would leave the C++ side with no evidence of its own that
// what it built writes back correctly; that is what leaning on another layer as the
// canary looks like, and it is why this suite exists separately from
// `render_view_parser_test` (index parity) and `cross_backend_test` (the producers
// agree).
//
// Same laws and the same 49-rule matrix as `cek_tests`, over documents built by
// `zr::<Rule>_read`.

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "CaptureCow.h"
#include "bbq_node.h"
#include "ZcowReaderTypes.h"    // generated handle classes (namespace zr)
#include "ZcowReaderReader.h"   // generated view parser (namespace zr)

using namespace bbq;

// The view-side extern the fixture's `Ext` rule calls: consumes a fixed 4 bytes.
bool readit(const uint8_t*, size_t len, size_t* consumed) {
    if (len < 4) return false;
    *consumed = 4;
    return true;
}

namespace {

using ReadFn = zcow::parse_result (*)(const uint8_t*, size_t, ParseArena&);

struct Case { const char* rule; ReadFn read; std::vector<uint8_t> bytes; };

// Every rule in the fixture, which is every `type_expr` constructor the grammar has.
// A rule added to the fixture and not listed here is a hole, so this table is the
// coverage claim.
const std::vector<Case>& cases() {
    static const std::vector<Case> c = {
        {"Flat", zr::Flat_read, {0x12,0x56,0x34,0x00,0x01,0x00,0x00,0x01,0x02,0xFC,0xFF,0xFF,0xFF}},
        {"Nest", zr::Nest_read, {0x07,0x09,0x00,0x0B}},
        {"Arr", zr::Arr_read, {0x03,0x10,0x00,0x20,0x00,0x30,0x00}},
        {"Pair", zr::Pair_read, {0x07,0x08}},
        {"Outer", zr::Outer_read, {0x55,0x07,0x08}},
        {"RArr", zr::RArr_read, {0x02,1,2,3,4}},
        {"Bytes", zr::Bytes_read, {0x03,0xAA,0xBB,0xCC}},
        {"Str", zr::Str_read, {0x61,0x62,0x63}},
        {"Wh", zr::Wh_read, {0x01,0x2A}},
        {"WhTwice", zr::WhTwice_read, {0x05,0x0A,0x2A}},
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
        {"SBf", zr::SBf_read, {0x2D}},
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
        {"FComp", zr::FComp_read, {0xC8}},
    };
    return c;
}

// A parse and the arena its nodes live in, kept together so a document outlives the
// handles into it.
struct Parsed {
    ParseArena arena;
    zcow::parse_result r;
};
std::unique_ptr<Parsed> parse(const Case& c, const std::vector<uint8_t>& bytes) {
    auto p = std::make_unique<Parsed>();
    p->r = c.read(bytes.data(), bytes.size(), p->arena);
    return p;
}

bool all_span_backed(const zcow::node* n) {
    if (!n) return true;
    if (!n->parsed) return false;
    for (const auto& k : n->kids) if (!all_span_backed(k.get())) return false;
    return true;
}

// A step down to a leaf. Array elements are anonymous, so the position is carried
// too — a name alone cannot tell two elements apart, and name lookup is ambiguous
// anywhere an array of structs puts `p`,`q`,`p`,`q` side by side.
struct Step { const char* name; size_t index; };

void each_leaf(const zcow::node* n, std::vector<Step>& path,
               const std::function<void(const zcow::node*, const std::vector<Step>&)>& fn) {
    if (!n) return;
    if (n->kids.empty()) { fn(n, path); return; }
    for (size_t i = 0; i < n->kids.size(); i++) {
        path.push_back({n->kids[i]->name, i});
        each_leaf(n->kids[i].get(), path, fn);
        path.pop_back();
    }
}

zcow::node* reach(zcow::transient& t, const std::vector<Step>& path) {
    std::vector<const char*> names(path.size(), nullptr);   // positional throughout
    std::vector<size_t> idx;
    for (const auto& s : path) idx.push_back(s.index);
    return t.own_path(names.data(), idx.data(), names.size());
}

// The shape a parse produced, independent of which bytes back it.
void describe(const zcow::node* n, const zcow::source& src, const std::string& at,
              std::ostringstream& out) {
    if (!n) return;
    out << at << " t=" << (int)n->type << " tag=" << n->variant_tag
        << " len=" << (n->end_offset - n->start_offset)
        << " kids=" << n->kids.size();
    if (n->type == CaptureType::Computed && n->computed_value)
        out << " cv=" << (int)n->computed_value->kind;
    else if (n->kids.empty() && n->type != CaptureType::Computed) {
        int64_t v = 0; bool sgn = false;
        if (zcow::decode_int(n, src.buf, &v, &sgn)) out << " v=" << v;
    }
    out << "\n";
    for (size_t i = 0; i < n->kids.size(); i++) {
        const char* nm = n->kids[i]->name;
        describe(n->kids[i].get(), src,
                 (nm && *nm) ? at + "." + nm : at + "[" + std::to_string(i) + "]", out);
    }
}
std::string shape_of(const zcow::document& d) {
    std::ostringstream oss;
    describe(d.root(), d.src(), "$", oss);
    return oss.str();
}

}  // namespace

// ── The matrix ──────────────────────────────────────────────────────────────

TEST(CppMatrix, EveryRuleRoundTripsByteIdentical) {
    for (const auto& c : cases()) {
        auto p = parse(c, c.bytes);
        EXPECT_TRUE(p->r.success) << c.rule << ": reader rejected authored bytes";
        if (!p->r.success) continue;

        std::vector<uint8_t> consumed(c.bytes.begin(), c.bytes.begin() + p->r.bytes_consumed);
        EXPECT_EQ(p->r.doc.serialize(), consumed) << c.rule;
        EXPECT_TRUE(all_span_backed(p->r.doc.root())) << c.rule;
    }
}

TEST(CppMatrix, EveryRuleReParsesToTheSameShape) {
    for (const auto& c : cases()) {
        // `Unt` terminates on how much input is left, so serializing the consumed
        // prefix legitimately changes what it reads. Demonstrated in cek_tests.
        if (std::strcmp(c.rule, "Unt") == 0) continue;

        auto first = parse(c, c.bytes);
        EXPECT_TRUE(first->r.success) << c.rule;
        if (!first->r.success) continue;
        std::vector<uint8_t> out = first->r.doc.serialize();
        std::string shape1 = shape_of(first->r.doc);

        auto second = parse(c, out);
        EXPECT_TRUE(second->r.success) << c.rule << ": re-parse rejected its own output";
        if (!second->r.success) continue;
        EXPECT_EQ(shape_of(second->r.doc), shape1) << c.rule;
        EXPECT_EQ(second->r.doc.serialize(), out) << c.rule;
    }
}

TEST(CppMatrix, AnEmptyEditIsAlwaysIdentity) {
    for (const auto& c : cases()) {
        auto p = parse(c, c.bytes);
        EXPECT_TRUE(p->r.success) << c.rule;
        if (!p->r.success) continue;
        std::vector<uint8_t> before = p->r.doc.serialize();
        zcow::document after = p->r.doc.begin_edit().commit();
        EXPECT_EQ(after.serialize(), before) << c.rule;
        EXPECT_TRUE(all_span_backed(after.root())) << c.rule;
    }
}

// ── The laws, over documents this backend built ─────────────────────────────

// GetPut: read every leaf, write it straight back, and the bytes do not move.
TEST(CppLaw, GetPutOverEveryRule) {
    for (const auto& c : cases()) {
        auto p = parse(c, c.bytes);
        EXPECT_TRUE(p->r.success) << c.rule;
        if (!p->r.success) continue;

        std::vector<uint8_t> before = p->r.doc.serialize();
        zcow::transient t = p->r.doc.begin_edit();
        const auto& src = p->r.doc.src();

        std::vector<Step> path;
        each_leaf(p->r.doc.root(), path,
                  [&](const zcow::node* leaf, const std::vector<Step>& at) {
            if (at.empty()) return;
            if (leaf->type == CaptureType::Computed || leaf->type == CaptureType::External) return;
            zcow::node* w = reach(t, at);
            if (!w) return;
            if (leaf->type == CaptureType::Bytes || leaf->type == CaptureType::String) {
                auto b = zcow::read_bytes(leaf, src);
                zcow::set_bytes(w, b.first, b.second);
            } else if (is_float_type(leaf->type)) {
                zcow::set_float(w, zcow::read_float(leaf, src));
            } else {
                zcow::set_int(w, zcow::read_int(leaf, src));
            }
        });

        EXPECT_EQ(std::move(t).commit().serialize(), before) << c.rule;
    }
}

// Persistence and isolation (§1.2, §4.1): the version an edit came from is preserved,
// and the transient's mutations are not visible in it — before or after commit.
TEST(CppLaw, EditingNeverDisturbsTheDocumentItCameFrom) {
    for (const auto& c : cases()) {
        auto p = parse(c, c.bytes);
        EXPECT_TRUE(p->r.success) << c.rule;
        if (!p->r.success) continue;

        std::vector<uint8_t> before = p->r.doc.serialize();
        {
            zcow::transient t = p->r.doc.begin_edit();
            std::vector<Step> path;
            each_leaf(p->r.doc.root(), path,
                      [&](const zcow::node* leaf, const std::vector<Step>& at) {
                if (at.empty() || leaf->type == CaptureType::Computed) return;
                if (leaf->type == CaptureType::Bytes || leaf->type == CaptureType::String ||
                    is_float_type(leaf->type)) return;
                if (zcow::node* w = reach(t, at)) zcow::set_int(w, 0x7F);
            });
            EXPECT_EQ(p->r.doc.serialize(), before) << c.rule << " (before commit)";
            (void)std::move(t).commit();
        }
        EXPECT_EQ(p->r.doc.serialize(), before) << c.rule << " (after commit)";
    }
}

// Figure 2.2a: an update copies the path and shares everything else — pointer
// identity, since value equality would pass for a deep copy too.
TEST(CppLaw, NothingOffTheEditedPathIsCopied) {
    auto p = parse(cases()[0], cases()[0].bytes);   // Flat
    ASSERT_TRUE(p->r.success);
    const zcow::node* before_u16 = zcow::child_named(p->r.doc.root(), "u16");
    ASSERT_NE(before_u16, nullptr);

    zcow::transient t = p->r.doc.begin_edit();
    zcow::set_int(t.own_child(t.root_mut(), "u8"), 0x99);
    zcow::document v = std::move(t).commit();

    EXPECT_NE(v.root(), p->r.doc.root());                                   // on the path
    EXPECT_NE(zcow::child_named(v.root(), "u8"),
              zcow::child_named(p->r.doc.root(), "u8"));                    // on the path
    EXPECT_EQ(zcow::child_named(v.root(), "u16"), before_u16);              // shared
}

// Def 4.1 / §7.2: an invalidated transient is not a legal argument.
TEST(CppLaw, EveryOperationRefusesAnInvalidatedTransient) {
    auto p = parse(cases()[0], cases()[0].bytes);
    ASSERT_TRUE(p->r.success);
    zcow::transient t = p->r.doc.begin_edit();
    (void)std::move(t).commit();
    EXPECT_THROW((void)t.root(), zcow::invalidated);
    EXPECT_THROW((void)t.serialize(), zcow::invalidated);
    EXPECT_THROW((void)t.own_child(nullptr, (size_t)0), zcow::invalidated);
}

// ── Dependent fields, through this backend's documents ──────────────────────

TEST(CppLaw, AppendingToACountedArrayUpdatesItsCount) {
    struct K { const char* rule; ReadFn read; const char* arr; const char* count; };
    const K counted[] = {
        {"Cnt",     zr::Cnt_read,     "xs",    "n"},
        {"RArr",    zr::RArr_read,    "items", "n"},
        {"LebArr",  zr::LebArr_read,  "xs",    "n"},
        {"NestArr", zr::NestArr_read, "gs",    "m"},
        {"Np",      zr::Np_read,      "xs",    "h.n"},   // the count is a struct deeper
    };

    for (const auto& k : counted) {
        std::vector<uint8_t> bytes;
        for (const auto& c : cases()) if (std::strcmp(c.rule, k.rule) == 0) bytes = c.bytes;
        ASSERT_FALSE(bytes.empty()) << k.rule;

        Case c{k.rule, k.read, bytes};
        auto p = parse(c, bytes);
        ASSERT_TRUE(p->r.success) << k.rule;

        const zcow::node* arr = zcow::child_named(p->r.doc.root(), k.arr);
        ASSERT_NE(arr, nullptr) << k.rule;
        // The linkage is recorded by the PARSER, so this backend's documents must carry
        // it exactly as the CEK's do.
        EXPECT_EQ(arr->derives, zcow::node::Derives::HasCount)
            << k.rule << ": the generated reader recorded no count linkage";
        EXPECT_STREQ(arr->determines ? arr->determines : "<none>", k.count) << k.rule;

        const size_t n0 = zcow::size_of(arr);
        zcow::transient t = p->r.doc.begin_edit();
        zcow::node* xs = t.own_child(t.root_mut(), k.arr);
        ASSERT_NE(xs, nullptr) << k.rule;
        zcow::set_int(t.append(xs, CaptureType::UInt8), 0x7F);
        zcow::document v = std::move(t).commit();

        const zcow::node* cnt = v.root();
        for (const char* seg = k.count; cnt && seg && *seg; ) {
            const char* dot = std::strchr(seg, '.');
            cnt = zcow::child_named(cnt, std::string(seg, dot ? (size_t)(dot - seg)
                                                              : std::strlen(seg)));
            seg = dot ? dot + 1 : nullptr;
        }
        ASSERT_NE(cnt, nullptr) << k.rule;
        EXPECT_EQ(zcow::read_int(cnt, v.src()), (int64_t)(n0 + 1)) << k.rule;
    }
}

TEST(CppLaw, ResizingARestWindowUpdatesItsSize) {
    Case c{"RestEof", zr::RestEof_read, {0x03,10,20,30}};
    auto p = parse(c, c.bytes);
    ASSERT_TRUE(p->r.success);
    const int64_t before = zcow::read_int(zcow::child_named(p->r.doc.root(), "sz"),
                                          p->r.doc.src());

    zcow::transient t = p->r.doc.begin_edit();
    zcow::node* xs = t.own_child(t.root_mut(), "xs");
    ASSERT_NE(xs, nullptr);
    zcow::set_int(t.append(xs, CaptureType::UInt8), 0x7F);
    zcow::document v = std::move(t).commit();

    EXPECT_EQ(zcow::read_int(zcow::child_named(v.root(), "sz"), v.src()), before + 1);
}

// ── Bitfields, through this backend's documents and handles ─────────────────

TEST(CppBits, ATopLevelRunIsTaggedAndRoundTrips) {
    Case c{"Bf", zr::Bf_read, {0xA5}};
    auto p = parse(c, c.bytes);
    ASSERT_TRUE(p->r.success);
    const zcow::node* run = p->r.doc.root();
    EXPECT_TRUE(run->bits_container) << "the generated reader recorded no bitfield run";
    EXPECT_EQ(zcow::read_bits(run, "hi", p->r.doc.src()), 0x5);
    EXPECT_EQ(zcow::read_bits(run, "lo", p->r.doc.src()), 0xA);

    zcow::transient t = p->r.doc.begin_edit();
    zcow::write_bits(t.root_mut(), "hi", t.src(), 0x3);
    std::vector<uint8_t> out = std::move(t).commit().serialize();
    EXPECT_EQ(out, (std::vector<uint8_t>{0xA3}));

    auto again = parse(c, out);
    ASSERT_TRUE(again->r.success);
    EXPECT_EQ(zcow::read_bits(again->r.doc.root(), "hi", again->r.doc.src()), 0x3);
    EXPECT_EQ(zcow::read_bits(again->r.doc.root(), "lo", again->r.doc.src()), 0xA);
}

TEST(CppBits, ASignedEntryIsSignExtended) {
    Case c{"SBf", zr::SBf_read, {0x2D}};      // imm = 0xD = -3, tag = 2
    auto p = parse(c, c.bytes);
    ASSERT_TRUE(p->r.success);
    const zcow::node* run = p->r.doc.root();
    EXPECT_EQ(zcow::read_bits(run, "imm", p->r.doc.src()), -3);
    EXPECT_EQ(zcow::read_bits(run, "tag", p->r.doc.src()), 2);
    // ...and what the parse bound under the member name says the same thing.
    EXPECT_EQ(zcow::read_int(zcow::child_named(run, "imm"), p->r.doc.src()), -3);

    zcow::transient t = p->r.doc.begin_edit();
    zcow::write_bits(t.root_mut(), "imm", t.src(), -6);
    EXPECT_EQ(std::move(t).commit().serialize(), (std::vector<uint8_t>{0x2A}));
}

TEST(CppBits, AnInlineRunLeavesItsNeighboursAlone) {
    Case c{"InlineBf", zr::InlineBf_read, {0x11,0xA5,0x22}};
    auto p = parse(c, c.bytes);
    ASSERT_TRUE(p->r.success);
    const zcow::node* run = zcow::child_named(p->r.doc.root(), "flags");
    ASSERT_NE(run, nullptr);
    EXPECT_TRUE(run->bits_container);

    zcow::transient t = p->r.doc.begin_edit();
    zcow::write_bits(t.own_child(t.root_mut(), "flags"), "lo", t.src(), 0xC);
    EXPECT_EQ(std::move(t).commit().serialize(),
              (std::vector<uint8_t>{0x11, 0xC5, 0x22}));
}

// ── The handles: the grammar's names and types over the same document ───────

TEST(CppHandles, ReadingThroughAHandleAgreesWithReadingTheDocument) {
    Case c{"Flat", zr::Flat_read, cases()[0].bytes};
    auto p = parse(c, c.bytes);
    ASSERT_TRUE(p->r.success);

    zr::Flat h = zr::Flat_of(p->r.doc);
    const auto& src = p->r.doc.src();
    const zcow::node* root = p->r.doc.root();
    EXPECT_EQ(h.u8(),   (uint8_t)zcow::read_int(zcow::child_named(root, "u8"), src));
    EXPECT_EQ(h.u16(),  (uint16_t)zcow::read_int(zcow::child_named(root, "u16"), src));
    EXPECT_EQ(h.u32(),  (uint32_t)zcow::read_int(zcow::child_named(root, "u32"), src));
    EXPECT_EQ(h.be16(), (uint16_t)zcow::read_int(zcow::child_named(root, "be16"), src));
    EXPECT_EQ(h.i32(),  (int32_t)zcow::read_int(zcow::child_named(root, "i32"), src));
}

TEST(CppHandles, WritingThroughAHandleReadsBackAndSerializes) {
    Case c{"Flat", zr::Flat_read, cases()[0].bytes};
    auto p = parse(c, c.bytes);
    ASSERT_TRUE(p->r.success);

    zcow::transient t = p->r.doc.begin_edit();
    zr::Flat h = zr::Flat_of(t);
    h.set_u16(0x0042);
    EXPECT_EQ(h.u16(), 0x0042);          // read-your-writes through the same handle
    EXPECT_EQ(h.u8(), 0x12);             // neighbour untouched

    std::vector<uint8_t> out = std::move(t).commit().serialize();
    ASSERT_EQ(out.size(), c.bytes.size());          // fixed width: patched in place
    for (size_t i = 0; i < out.size(); i++)
        if (i != 1 && i != 2) EXPECT_EQ(out[i], c.bytes[i]) << i;

    auto again = parse(c, out);
    ASSERT_TRUE(again->r.success);
    EXPECT_EQ(zr::Flat_of(again->r.doc).u16(), 0x0042);
}

TEST(CppHandles, AHandleOverADocumentRefusesAWrite) {
    // One handle class: mutability is a property of the document it came from, so a
    // setter on a read-only one raises rather than silently doing nothing. Python
    // wraps the same object model and has no const to carry the distinction.
    Case c{"Flat", zr::Flat_read, cases()[0].bytes};
    auto p = parse(c, c.bytes);
    ASSERT_TRUE(p->r.success);
    zr::Flat h = zr::Flat_of(p->r.doc);
    EXPECT_EQ(h.u16(), 0x3456);
    EXPECT_THROW(h.set_u16(0x0042), zcow::type_error);
    EXPECT_EQ(h.u16(), 0x3456);          // and nothing moved
}

TEST(CppHandles, ABitfieldEntryIsNamedNotShifted) {
    Case c{"Bf", zr::Bf_read, {0xA5}};
    auto p = parse(c, c.bytes);
    ASSERT_TRUE(p->r.success);

    zcow::transient t = p->r.doc.begin_edit();
    zr::Bf h = zr::Bf_of(t);
    EXPECT_EQ(h.hi(), 0x5);
    EXPECT_EQ(h.lo(), 0xA);
    h.set_hi(0x3);
    EXPECT_EQ(h.hi(), 0x3);
    EXPECT_EQ(h.lo(), 0xA);              // the neighbouring entry is untouched
    EXPECT_EQ(std::move(t).commit().serialize(), (std::vector<uint8_t>{0xA3}));
}

TEST(CppHandles, AnArrayHandleIteratesAndIndexes) {
    Case c{"Arr", zr::Arr_read, {0x03,0x10,0x00,0x20,0x00,0x30,0x00}};
    auto p = parse(c, c.bytes);
    ASSERT_TRUE(p->r.success);

    zr::Arr h = zr::Arr_of(p->r.doc);
    auto xs = h.xs();
    ASSERT_EQ(xs.size(), 3u);
    EXPECT_EQ(xs[0], 0x0010);
    EXPECT_EQ(xs[1], 0x0020);
    EXPECT_EQ(xs[2], 0x0030);

    std::vector<uint16_t> seen;
    for (uint16_t v : xs) seen.push_back(v);
    EXPECT_EQ(seen, (std::vector<uint16_t>{0x0010, 0x0020, 0x0030}));
}
