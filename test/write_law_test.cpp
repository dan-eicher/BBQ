// write_law_test — the laws BBQ's WRITE side is built on, stated from their
// sources the same way ipg_law_test states the read side's.
//
//   [LENS]      Foster, Greenwald, Moore, Pierce & Schmitt. Combinators for
//               Bidirectional Tree Transformations. TOPLAS 29(3), 2007.
//               get/put, GetPut / PutGet / PutPut, well behaved vs very well
//               behaved.
//   [EVERPARSE] Delignat-Lavaud, Fournet, Ramananandro et al. EverParse:
//               Verified Secure Zero-Copy Parsers. USENIX Security 2019.
//               correct / exact / non-malleable / complete.
//
// GetPut, PutGet and the transient/sharing laws are stated over the whole
// fixture matrix by `CEKLaw.*`, `CppLaw.*` and `ZCowLaw.*`. What is here is what
// those do not say: the laws BBQ deliberately does NOT hold. An unclaimed law
// with no test is indistinguishable from an untested one — someone reads
// "bidirectional" and assumes the rest.

#include <gtest/gtest.h>

#include "ipg_harness.h"

using ipg::kid;
using ipg::run;

namespace {

// Serialize the document as it stands.
std::vector<uint8_t> emit(const bbq::zcow::document& d) { return d.serialize(); }

}  // namespace

// ── [LENS] PutPut ────────────────────────────────────────────────────────────
//
// `put(a', put(a, c)) ⊑ put(a', c)` — "very well behaved" [LENS §3.2]. It says a
// write overwrites the one before it rather than composing with it. BBQ does not
// hold it, and Foster et al. report the same of their own map/flatten/merge and
// conditional combinators, "for reasons that seem pragmatically unavoidable".
//
// The test pins the failure rather than the law: this is what BBQ does, and a
// caller who expects the other thing should find out here.
TEST(LensLaw, PutPutIsNotClaimedForArrayEdits) {
    const char* g = "Top = struct { n: uint8, xs: array<uint8>[n] }";
    const std::vector<uint8_t> c = {2, 0xAA, 0xBB};

    auto append_to = [&](const std::vector<uint8_t>& bytes, uint8_t v) {
        auto r = run(g, "Top", bytes);
        EXPECT_TRUE(r.success) << r.error;
        auto t = r.meta.doc.begin_edit();
        bbq::zcow::node* xs = t.own_child(t.root_mut(), "xs");
        EXPECT_NE(xs, nullptr);
        bbq::zcow::node* added = t.append(xs, bbq::CaptureType::UInt8);
        bbq::zcow::set_int(added, v);
        return emit(std::move(t).commit());
    };

    // put(a', put(a, c)): append 0xCC, then append 0xDD to the result.
    std::vector<uint8_t> twice = append_to(append_to(c, 0xCC), 0xDD);

    // put(a', c): append 0xDD to the original.
    std::vector<uint8_t> single = append_to(c, 0xDD);

    EXPECT_NE(twice, single)
        << "PutPut is not claimed: the second write composes with the first "
           "rather than replacing it";
    // And the composition is the sane one — both elements survive, count settled.
    ASSERT_EQ(twice.size(), 5u);
    EXPECT_EQ(twice[0], 4);              // the count is a dependent field
    EXPECT_EQ(twice[3], 0xCC);
    EXPECT_EQ(twice[4], 0xDD);
}

// ── [EVERPARSE] where BBQ sits in the lattice ────────────────────────────────
//
// EverParse grades a parser/serializer pair: *correct* (serialize then parse is
// the identity), *exact* (parse then serialize is too), *non-malleable* (one
// abstract value has exactly one concrete representation) and *complete*. BBQ is
// correct and exact — that is GetPut and PutGet — and it is NOT non-malleable.
// Two things make it so, and both are deliberate.

// One: a parse that does not reach the end of the input leaves the rest
// untouched, so inputs differing past the parse are one document. `emit()` is the
// document, which is the consumed prefix — NOT the input byte for byte.
TEST(EverParseLattice, InputsDifferingPastTheParseAreOneDocument) {
    const char* g = "Top = struct { a: uint8, b: uint8 }";
    std::vector<std::vector<uint8_t>> out;
    for (const std::vector<uint8_t>& in : {std::vector<uint8_t>{1, 2},
                                           std::vector<uint8_t>{1, 2, 0xFF},
                                           std::vector<uint8_t>{1, 2, 0xFF, 0xEE}}) {
        auto r = run(g, "Top", in);
        ASSERT_TRUE(r.success) << r.error;
        EXPECT_EQ(r.meta.bytes_consumed, 2u) << "the grammar covers two bytes";
        out.push_back(emit(r.meta.doc));
    }
    // One abstract value, three concrete inputs: malleable by construction.
    EXPECT_EQ(out[0], out[1]);
    EXPECT_EQ(out[1], out[2]);
    // And what comes back is the parse, not the input — the trailing bytes are
    // not the document's to keep.
    EXPECT_EQ(out[2], (std::vector<uint8_t>{1, 2}));
}

// Two: a varint that is longer than it needs to be is still a varint. The decoder
// is strict about the WIDTH its carrier allows, not about minimality, so `81 00`
// is a two-byte encoding of 1 — and re-emitting it gives those two bytes back,
// because GetPut is byte equality and a re-canonicalised varint would break it.
TEST(EverParseLattice, ANonMinimalVarintIsAcceptedAndKept) {
    const char* g = "Top = struct { v: uleb128 }";

    auto minimal = run(g, "Top", {0x01});
    ASSERT_TRUE(minimal.success) << minimal.error;

    auto padded = run(g, "Top", {0x81, 0x00});
    ASSERT_TRUE(padded.success) << padded.error
        << "strictness is about the carrier's width, not minimality";

    // Same value from different bytes — malleable.
    ASSERT_NE(kid(minimal.root(), "v"), nullptr);
    ASSERT_NE(kid(padded.root(), "v"), nullptr);
    EXPECT_EQ(kid(minimal.root(), "v")->computed_value->i,
              kid(padded.root(), "v")->computed_value->i);

    // GetPut wins over canonicalisation: the padded form comes back as it went in.
    EXPECT_EQ(emit(padded.meta.doc), (std::vector<uint8_t>{0x81, 0x00}));
    EXPECT_EQ(emit(minimal.meta.doc), (std::vector<uint8_t>{0x01}));
}

// The strictness that IS claimed: an encoding wider than the carrier allows is a
// parse error, so malleability does not extend to unbounded padding.
TEST(EverParseLattice, AVarintWiderThanItsCarrierIsRejected) {
    auto r = run("Top = struct { v: uleb128 }",
                 "Top", {0x80, 0x80, 0x80, 0x80, 0x80, 0x00});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success) << "six continuation bytes exceed the 32-bit carrier";
}

// ── Depth is the input's to choose ───────────────────────────────────────────
//
// `Node = struct { n: uint8, kids: array<Node>[n] }` nests once per 0x01 byte, so
// a 30 KB file is a 30,000-deep document. Every walk over the tree — destroying
// it, measuring it, patching it, emitting it, settling its dependent fields — used
// to take one stack frame per level, which made a stack overflow something a file
// could ask for. They are all iterative now; these hold them that way.
//
// The depth is chosen to be well past where each of those used to die (~16k) and
// no further: the point is the shape, and the suite still has to run.
namespace {
constexpr int kDeep = 30000;

// depth-deep chain, then a terminator.
std::vector<uint8_t> deep_chain(int depth, uint8_t tag = 0x20) {
    std::vector<uint8_t> v;
    v.reserve(2 * (depth + 1));
    for (int i = 0; i < depth; i++) { v.push_back(1); v.push_back(tag); }
    v.push_back(0); v.push_back(tag);
    return v;
}
const char* kDeepGrammar = "Node = struct { n: uint8, tag: uint8, kids: array<Node>[n] }";

// The node `depth` levels down.
bbq::zcow::node* descend(bbq::zcow::transient& t, int depth) {
    bbq::zcow::node* n = t.root_mut();
    for (int i = 0; i < depth && n; i++) {
        bbq::zcow::node* kids = t.own_child(n, "kids");
        if (!kids || kids->kids.empty()) return nullptr;
        n = t.own_child(kids, size_t{0});
    }
    return n;
}
}  // namespace

TEST(DeepDocument, ParsingAndDestroyingADeepTreeDoesNotRecursePerLevel) {
    auto r = run(kDeepGrammar, "Node", deep_chain(kDeep));
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 2u * (kDeep + 1));
    // Destruction is the other half, and it happens when `r` goes out of scope.
}

TEST(DeepDocument, AnUneditedDeepTreeEmitsByteIdentical) {
    auto in = deep_chain(kDeep);
    auto r = run(kDeepGrammar, "Node", in);
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(emit(r.meta.doc), in);
}

// The walks that only a DIRTY path reaches: an unedited document emits by one
// memcpy, so the recursion hid behind the fast path until something was written
// at the bottom of the tree.
TEST(DeepDocument, AnEditAtTheBottomEmitsWithoutRecursingPerLevel) {
    auto in = deep_chain(kDeep);
    auto r = run(kDeepGrammar, "Node", in);
    ASSERT_TRUE(r.success) << r.error;

    auto t = r.meta.doc.begin_edit();
    bbq::zcow::node* deepest = descend(t, kDeep);
    ASSERT_NE(deepest, nullptr);
    bbq::zcow::node* tag = t.own_child(deepest, "tag");
    ASSERT_NE(tag, nullptr);
    bbq::zcow::set_int(tag, 0x7E);

    // Same width, so this is the patch path: changes_length + patch_node.
    std::vector<uint8_t> out = emit(std::move(t).commit());
    ASSERT_EQ(out.size(), in.size());
    EXPECT_EQ(out.back(), 0x7E);
    EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.end() - 1),
              std::vector<uint8_t>(in.begin(), in.end() - 1));
}

TEST(DeepDocument, AResizeAtTheBottomEmitsWithoutRecursingPerLevel) {
    auto in = deep_chain(kDeep);
    auto r = run(kDeepGrammar, "Node", in);
    ASSERT_TRUE(r.success) << r.error;

    auto t = r.meta.doc.begin_edit();
    bbq::zcow::node* deepest = descend(t, kDeep);
    ASSERT_NE(deepest, nullptr);
    bbq::zcow::node* kids = t.own_child(deepest, "kids");
    ASSERT_NE(kids, nullptr);
    // Appending makes the document longer, so emit rebuilds from the tree
    // (emit_node) instead of patching, and reconcile has to settle the count
    // 30,000 levels down.
    bbq::zcow::node* added = t.append(kids, bbq::CaptureType::Struct);
    bbq::zcow::set_int(t.append(added, bbq::CaptureType::UInt8), 0);
    bbq::zcow::set_int(t.append(added, bbq::CaptureType::UInt8), 0x55);

    std::vector<uint8_t> out = emit(std::move(t).commit());
    EXPECT_EQ(out.size(), in.size() + 2) << "the appended node's two bytes";
    EXPECT_EQ(out.back(), 0x55);
}

// ── GetPut, where it is easy to think it is weaker than it is ────────────────
//
// The reason `put` takes the original bytes [LENS Def 3.1] is that `get` throws
// information away. Bytes no field covers are that information: two interval-
// placed fields with a gap between them, and the gap has to survive.
TEST(LensLaw, GetPutKeepsBytesNoFieldCovers) {
    const std::vector<uint8_t> in = {0x11, 0x99, 0x98, 0x44};
    auto r = run("Sub = struct { x: uint8 }\n"
                 "Top = struct { s: Sub[0, 1], z: uint8[3, 4] }",
                 "Top", in);
    ASSERT_TRUE(r.success) << r.error;
    // Bytes 1 and 2 are named by nothing in the grammar.
    EXPECT_EQ(emit(r.meta.doc), in) << "the gap is part of what put puts back";
}
