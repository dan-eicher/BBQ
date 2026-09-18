// ipg_law_test — conformance against "Interval Parsing Grammars for File Format
// Parsing" (Zhang, Morrisett, Tan; PLDI 2023).
//
// The suite is enumerated from the paper, not from BBQ: one test per rule in
// Figures 5/7/8, per check in §3.2, per full-language feature in §3.4, per case
// study in §4, and per clause of the termination argument in §5. Where BBQ
// deliberately parts company with the paper the test pins BBQ's rule and names
// the paper rule it replaces; docs/Laws.md carries the full matrix, for this
// source and for the others BBQ is built on.
//
// The CEK is the reference semantics, so a law is stated against it. A law that
// a code generator could silently drop is stated against the generated C reader
// as well — a backend agreeing with the CEK on shape is not evidence that it
// kept the check.
//
// Its own binary with a ctest TIMEOUT: the §5 laws are about nontermination, and
// a broken one presents as a hang, not as a failed assertion.

#include <gtest/gtest.h>

#include "ipg_harness.h"

using ipg::c_run;
using ipg::diagnose;
using ipg::kid;
using ipg::run;

namespace {

// The arm a choice-valued field recorded: the wrapper under it carrying the
// variant tag. Null when the field is not a choice, or is not there.
const bbq::zcow::node* arm_of(const bbq::zcow::node* parent, const char* field) {
    const bbq::zcow::node* f = kid(parent, field);
    if (!f) return nullptr;
    for (auto& k : f->kids)
        if (k->variant_tag >= 0) return k.get();
    return nullptr;
}

// The integer a `compute` field recorded.
int64_t computed(const bbq::zcow::node* n) {
    EXPECT_NE(n, nullptr);
    EXPECT_NE(n->computed_value, nullptr);
    return n->computed_value->i;
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════
// §3.1  Core language syntax (Figure 5) — every term kind
// ══════════════════════════════════════════════════════════════════════

// Term `A[e_l, e_r]`: a nonterminal carries an interval that says which slice of
// the input its rule describes. The value is what says so — an offset alone would
// still be there if the rule had read the wrong bytes.
TEST(IpgTerm, NonterminalCarriesAnInterval) {
    auto r = run("Sub = struct { v: uint8 }\n"
                 "Top = struct { s: Sub[3, 4], k: compute(s.v : uint8) }",
                 "Top", {0x10, 0x11, 0x12, 0x99});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* s = kid(r.root(), "s");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->start_offset, 3u);   // it described byte 3, not byte 0
    EXPECT_EQ(s->end_offset, 4u);
    EXPECT_EQ(computed(kid(r.root(), "k")), 0x99) << "byte 3, not byte 0";
}

// Moving the interval moves what the rule reads, and nothing else about the spec
// changes. Without this, the test above passes on a parser that ignores intervals
// and happens to be pointed at the right byte.
TEST(IpgTerm, MovingTheIntervalMovesWhatIsRead) {
    for (auto at : {std::pair<const char*, int>{"[0, 1]", 0x10},
                    std::pair<const char*, int>{"[1, 2]", 0x11},
                    std::pair<const char*, int>{"[3, 4]", 0x99}}) {
        std::string g = std::string("Sub = struct { v: uint8 }\n"
                                    "Top = struct { s: Sub") + at.first +
                        ", k: compute(s.v : uint8) }";
        auto r = run(g, "Top", {0x10, 0x11, 0x12, 0x99});
        ASSERT_TRUE(r.compiled) << r.error;
        ASSERT_TRUE(r.success) << r.error;
        EXPECT_EQ(computed(kid(r.root(), "k")), at.second) << "at " << at.first;
    }
}

// Term `s[e_l, e_r]`: a terminal string over a slice. BBQ writes a terminal as a
// fixed-width read pinned by `where` (the magic-number idiom), or as `bytes`
// compared against a literal.
TEST(IpgTerm, TerminalMatchesOrFails) {
    const char* g = "Hdr = struct { magic: uint32be where magic == 0x89504E47 }";
    auto ok = run(g, "Hdr", {0x89, 0x50, 0x4E, 0x47});
    ASSERT_TRUE(ok.compiled) << ok.error;
    EXPECT_TRUE(ok.success) << ok.error;

    auto bad = run(g, "Hdr", {0x89, 0x50, 0x4E, 0x00});
    ASSERT_TRUE(bad.compiled) << bad.error;
    EXPECT_FALSE(bad.success);
}

// Term `{id=e}`: an attribute definition binds a name to a value. BBQ spells it
// `compute(e : T)`.
TEST(IpgTerm, AttributeDefinitionBindsAValue) {
    auto r = run("Hdr = struct { n: uint8, twice: compute(n * 2 : uint16) }",
                 "Hdr", {7});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(computed(kid(r.root(), "twice")), 14);
}

// Term `⟨e⟩`: a predicate. BBQ spells it `where e`.
TEST(IpgTerm, PredicateGatesTheParse) {
    const char* g = "Hdr = struct { n: uint8 where n > 0 && n < 10 }";
    EXPECT_TRUE(run(g, "Hdr", {5}).success);
    EXPECT_FALSE(run(g, "Hdr", {50}).success);
}

// Term `for id=e1 to e2 do A[e_l, e_r]`: an array of elements, each placed by an
// interval that may mention the loop variable. BBQ spells the loop variable
// `@index` — a bare identifier is always a field reference (Grammar §7.3).
TEST(IpgTerm, ArrayPlacesEachElementByItsOwnInterval) {
    // The bytes in between are 0x00, so reading sequentially instead of by the
    // stride would give 0xAA,0x00,0xBB rather than 0xAA,0xBB,0xCC.
    auto r = run("E   = struct { v: uint8, k: compute(v : uint8) }\n"
                 "Top = struct { base: uint8, n: uint8,\n"
                 "               xs: array<E>[n] @ [base + @index * 2, base + @index * 2 + 1] }",
                 "Top", {2, 3, 0xAA, 0x00, 0xBB, 0x00, 0xCC});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* xs = kid(r.root(), "xs");
    ASSERT_NE(xs, nullptr);
    ASSERT_EQ(xs->kids.size(), 3u);
    const int want[] = {0xAA, 0xBB, 0xCC};
    const size_t at[] = {2u, 4u, 6u};
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(xs->kids[i]->start_offset, at[i]) << "element " << i;
        EXPECT_EQ(computed(kid(xs->kids[i].get(), "k")), want[i]) << "element " << i;
    }
}

// The same confinement as any interval: an element placed outside the input is a
// parse failure, not a short array.
TEST(IpgTerm, AnElementIntervalThatEscapesTheInputFails) {
    auto r = run("E   = struct { v: uint8 }\n"
                 "Top = struct { base: uint8,\n"
                 "               xs: array<E>[3] @ [base + @index, base + @index + 1] }",
                 "Top", {2, 0xAA, 0xBB});   // element 2 wants [4,5) of a 3-byte input
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
}

// Reference `A(e).id` (Figure 5): the attribute of an array element. This is how
// the paper's ELF rule reaches a section's offset — `SH(i).ofs` — so it has to
// work in an interval, not only in a predicate.
TEST(IpgTerm, ArrayElementAttributeIsReachable) {
    auto r = run("Top = struct { xs: array<uint8>[3],\n"
                 "               a: compute(xs[0] : uint8),\n"
                 "               b: compute(xs[1] : uint8),\n"
                 "               c: compute(xs[2] : uint8) }",
                 "Top", {0x41, 0x42, 0x43});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    // Three indices, three values: a subscript that always read element 0 — or
    // always the last — passes a single-index test.
    EXPECT_EQ(computed(kid(r.root(), "a")), 0x41);
    EXPECT_EQ(computed(kid(r.root(), "b")), 0x42);
    EXPECT_EQ(computed(kid(r.root(), "c")), 0x43);
}

TEST(IpgTerm, ArrayElementAttributeOfAStructElement) {
    auto r = run("SH  = struct { ofs: uint8, sz: uint8 }\n"
                 "Top = struct { shs: array<SH>[2],\n"
                 "               a: compute(shs[0].ofs : uint8),\n"
                 "               b: compute(shs[0].sz : uint8),\n"
                 "               c: compute(shs[1].ofs : uint8),\n"
                 "               d: compute(shs[1].sz : uint8) }",
                 "Top", {1, 2, 3, 4});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    // Each index and each member reaches a different byte: an implementation that
    // collapsed the subscript, or the member, would collide here.
    EXPECT_EQ(computed(kid(r.root(), "a")), 1);
    EXPECT_EQ(computed(kid(r.root(), "b")), 2);
    EXPECT_EQ(computed(kid(r.root(), "c")), 3);
    EXPECT_EQ(computed(kid(r.root(), "d")), 4);
}

TEST(IpgTerm, AnArrayIndexPastTheEndFails) {
    auto r = run("Top = struct { xs: array<uint8>[2], pick: compute(xs[5] : uint8) }",
                 "Top", {0x41, 0x42});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
}

// ══════════════════════════════════════════════════════════════════════
// §3.2  Attribute checking
// ══════════════════════════════════════════════════════════════════════

// Property (1): "every attribute reference refers to a properly defined
// attribute". A name defined by no rule anywhere in the grammar is a static
// error, not a parse that fails on some inputs and not others.
TEST(IpgAttrCheck, AReferenceToANameNoRuleDefinesIsRejected) {
    auto d = diagnose("Top = struct { v: uint8 where nosuchfield > 0 }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
    EXPECT_TRUE(d.says("nosuchfield")) << d.text;
}

TEST(IpgAttrCheck, AnUndefinedNameInAnIntervalIsRejected) {
    auto d = diagnose("Sub = struct { v: uint8 }\n"
                      "Top = struct { s: Sub[nosuchfield, nosuchfield + 1] }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
    EXPECT_TRUE(d.says("nosuchfield")) << d.text;
}

TEST(IpgAttrCheck, AnUndefinedNameInAComputeIsRejected) {
    auto d = diagnose("Top = struct { v: compute(nosuchfield : uint8) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
    EXPECT_TRUE(d.says("nosuchfield")) << d.text;
}

// Property 1 holds at every depth, not just the first dot. A path resolved one
// step and then abandoned is how `a.nosuch` gets caught while `a.b.nosuch` sails
// through to a parse-time failure.
TEST(IpgAttrCheck, ADeepPathIsCheckedAtEveryStep) {
    const char* rules = "C = struct { v: uint8 }\n"
                        "B = struct { c: C }\n"
                        "A = struct { b: B }\n";
    auto good = diagnose(std::string(rules) +
                         "Top = struct { a: A, k: compute(a.b.c.v : uint8) }");
    EXPECT_TRUE(good.ok) << good.text;

    for (const char* bad : {"a.nosuch", "a.b.nosuch", "a.b.c.nosuch"}) {
        auto d = diagnose(std::string(rules) + "Top = struct { a: A, k: compute(" +
                          bad + " : uint8) }");
        ASSERT_TRUE(d.parsed);
        EXPECT_FALSE(d.ok) << bad << ": " << d.text;
        EXPECT_TRUE(d.says("nosuch")) << bad << ": " << d.text;
    }
}

// …and through a subscript, which is the paper's `A(e).id` reaching into a
// struct element.
TEST(IpgAttrCheck, APathThroughASubscriptIsChecked) {
    auto good = diagnose("C = struct { v: uint8 }\n"
                         "Top = struct { xs: array<C>[2], k: compute(xs[0].v : uint8) }");
    EXPECT_TRUE(good.ok) << good.text;

    auto bad = diagnose("C = struct { v: uint8 }\n"
                        "Top = struct { xs: array<C>[2], k: compute(xs[0].nosuch : uint8) }");
    ASSERT_TRUE(bad.parsed);
    EXPECT_FALSE(bad.ok) << bad.text;
}

// The permissive half has to stay permissive: a name this rule does not bind may
// come from the scope of a rule that calls it, and refusing that would make a
// helper rule unusable.
TEST(IpgAttrCheck, ACrossRuleNameIsStillAccepted) {
    auto d = diagnose("Inner = struct { x: uint8, k: compute(outer_field : uint8) }\n"
                      "Outer = struct { outer_field: uint8, i: Inner }");
    EXPECT_TRUE(d.ok) << d.text;
}

// `def(A)` is "the set of attributes that are defined in all alternatives". A name
// one arm binds and another does not is not an attribute of the rule, and reaching
// for it is a static error — not a parse that succeeds or fails depending on which
// arm the input took.
TEST(IpgAttrCheck, DefIsTheIntersectionOverAlternatives) {
    auto d = diagnose("Inner = struct { p: uint8 } | struct { q: uint8 }\n"
                      "Top   = struct { i: Inner, k: compute(i.p : uint8) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
    EXPECT_TRUE(d.says("alternative")) << d.text;
}

TEST(IpgAttrCheck, ANameNoAlternativeBindsIsRejected) {
    auto d = diagnose("Inner = struct { p: uint8 } | struct { q: uint8 }\n"
                      "Top   = struct { i: Inner, k: compute(i.zzz : uint8) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// The intersection itself — a name EVERY arm binds — is a legal reference in the
// paper and BBQ does not take it either: a choice is a tagged union in the
// generated types, so a field "of the choice" would need a tag dispatch at every
// use. A name common to every arm is a field the format has in common, and BBQ
// says to put it where it belongs, in front of the choice.
TEST(IpgAttrCheck, ANameEveryAlternativeBindsIsRefusedWithTheLift) {
    auto d = diagnose("Inner = struct { tag: uint8 where tag == 1, p: uint8 }\n"
                      "      | struct { tag: uint8, p: uint8 }\n"
                      "Top   = struct { i: Inner, k: compute(i.p : uint8) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
    EXPECT_TRUE(d.says("lift it out")) << d.text;
}

// Lifted, it reads the same value whichever arm matched — which is the shape the
// diagnostic asks for, so the suite has to show it works.
TEST(IpgAttrCheck, TheLiftedFieldReadsWhicheverArmMatched) {
    const char* g = "Body = struct { a: uint8 where a == 1 } | struct { b: uint8 }\n"
                    "Top  = struct { p: uint8, body: Body, k: compute(p : uint8) }";
    for (auto bytes : {std::vector<uint8_t>{0x42, 1}, std::vector<uint8_t>{0x37, 9}}) {
        auto r = run(g, "Top", bytes);
        ASSERT_TRUE(r.compiled) << r.error;
        ASSERT_TRUE(r.success) << r.error;
        EXPECT_EQ(computed(kid(r.root(), "k")), bytes[0]);
    }
}

// The loop variable is `@index`. A bare `i` is a field reference (Grammar §7.3)
// and, naming no field, is caught by the same check.
TEST(IpgAttrCheck, BareIInAnElementIntervalIsNotTheLoopVariable) {
    auto d = diagnose("E = struct { v: uint8 }\n"
                      "Top = struct { xs: array<E>[2] @ [i, i + 1] }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// Property (2): no circular definitions. The paper rejects an alternative whose
// dependency graph is not a DAG; BBQ rejects a rule cycle that no array or
// optional guards, which is the same cycle seen at rule granularity.
TEST(IpgAttrCheck, ACycleAmongRulesIsRejected) {
    auto d = diagnose("A = struct { b: B }\n"
                      "B = struct { a: A }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
    EXPECT_TRUE(d.says("circular")) << d.text;
}

// §3.2 also reorders terms into the dependency graph's topological order, so
// `B1[0, B2.a] B2[a1, EOI] {a1=2}` parses B2 first. BBQ does NOT reorder: in a
// binary format the term order IS the byte order, so a reference to a later
// field is an error rather than a rewrite.
TEST(IpgAttrCheck, TermsAreNotReorderedAForwardReferenceIsAnError) {
    auto d = diagnose("B2  = struct { a: uint8 }\n"
                      "B1  = struct { v: uint8 }\n"
                      "Top = struct { b1: B1[0, b2.a], b2: B2[1, 2] }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
    EXPECT_TRUE(d.says("forward reference")) << d.text;
}

// ══════════════════════════════════════════════════════════════════════
// §3.3  Parsing semantics (Figure 8)
// ══════════════════════════════════════════════════════════════════════

// R-AltSucc: the first alternative that succeeds is the result; later ones are
// never tried. Order is therefore observable — swapping the arms changes the
// parse of the same bytes.
TEST(IpgAlt, TheFirstSuccessWinsAndOrderIsObservable) {
    auto wide = run("N = uint32le | uint8", "N", {1, 2, 3, 4});
    ASSERT_TRUE(wide.compiled) << wide.error;
    ASSERT_TRUE(wide.success) << wide.error;
    EXPECT_EQ(wide.meta.bytes_consumed, 4u);

    auto narrow = run("N = uint8 | uint32le", "N", {1, 2, 3, 4});
    ASSERT_TRUE(narrow.compiled) << narrow.error;
    ASSERT_TRUE(narrow.success) << narrow.error;
    EXPECT_EQ(narrow.meta.bytes_consumed, 1u);
}

// R-AltFail: a failing alternative falls through to the next one.
TEST(IpgAlt, AFailingAlternativeFallsThroughToTheNext) {
    auto r = run("Top = struct { a: uint8 where a == 9 }\n"
                 "    | struct { b: uint8 where b == 1 }",
                 "Top", {1});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_NE(kid(r.root(), "alt_1"), nullptr);   // the second arm produced the tree
}

// R-AltFail again, with the choice nested rather than at the top: the enclosing
// parse carries on afterwards. A-Seq1 threads the terms after it whichever
// alternative won, so the last arm winning is not a special case.
//
// The arms read different numbers of bytes on purpose. A choice whose arms leave
// the same tree cannot witness WHICH arm ran, and which arm ran is the whole
// content of biased choice — so each case checks the variant the parse recorded
// and where the field after the choice landed.
TEST(IpgAlt, TheEnclosingParseContinuesAfterALaterArmWins) {
    const char* g = "Inner = struct { t: uint8 where t == 1, extra: uint8 }\n"
                    "      | struct { t: uint8 }\n"
                    "Top   = struct { i: Inner, z: uint8 }";

    // t == 1: the first arm takes two bytes, so `z` is the third.
    auto first = run(g, "Top", {1, 0xEE, 0x55});
    ASSERT_TRUE(first.compiled) << first.error;
    ASSERT_TRUE(first.success) << first.error;
    EXPECT_EQ(first.meta.bytes_consumed, 3u);
    ASSERT_NE(arm_of(first.root(), "i"), nullptr);
    EXPECT_STREQ(arm_of(first.root(), "i")->name, "alt_0");
    EXPECT_EQ(arm_of(first.root(), "i")->variant_tag, 0);
    ASSERT_NE(kid(first.root(), "z"), nullptr);
    EXPECT_EQ(kid(first.root(), "z")->start_offset, 2u);

    // t != 1: the first arm's `where` fails, the second takes one byte, `z` is
    // the second — and the first arm's `extra` is nowhere in the tree.
    auto second = run(g, "Top", {9, 0x55});
    ASSERT_TRUE(second.compiled) << second.error;
    ASSERT_TRUE(second.success) << second.error;
    EXPECT_EQ(second.meta.bytes_consumed, 2u);
    const bbq::zcow::node* arm = arm_of(second.root(), "i");
    ASSERT_NE(arm, nullptr);
    EXPECT_STREQ(arm->name, "alt_1");
    EXPECT_EQ(arm->variant_tag, 1);
    ASSERT_EQ(arm->kids.size(), 1u);
    EXPECT_EQ(kid(arm->kids[0].get(), "extra"), nullptr);
    ASSERT_NE(kid(second.root(), "z"), nullptr);
    EXPECT_EQ(kid(second.root(), "z")->start_offset, 1u);
}

TEST(IpgAlt, TheEnclosingParseContinuesAfterALaterUnionVariantWins) {
    const char* g = "A   = struct { t: uint8 where t == 1, extra: uint8 }\n"
                    "B   = struct { t: uint8 }\n"
                    "U   = union { asA: A, asB: B }\n"
                    "Top = struct { u: U, z: uint8 }";

    auto first = run(g, "Top", {1, 0xEE, 0x55});
    ASSERT_TRUE(first.compiled) << first.error;
    ASSERT_TRUE(first.success) << first.error;
    EXPECT_EQ(first.meta.bytes_consumed, 3u);
    ASSERT_NE(arm_of(first.root(), "u"), nullptr);
    EXPECT_STREQ(arm_of(first.root(), "u")->name, "asA");
    EXPECT_EQ(arm_of(first.root(), "u")->variant_tag, 0);

    auto second = run(g, "Top", {9, 0x55});
    ASSERT_TRUE(second.compiled) << second.error;
    ASSERT_TRUE(second.success) << second.error;
    EXPECT_EQ(second.meta.bytes_consumed, 2u);
    ASSERT_NE(arm_of(second.root(), "u"), nullptr);
    EXPECT_STREQ(arm_of(second.root(), "u")->name, "asB");
    EXPECT_EQ(arm_of(second.root(), "u")->variant_tag, 1);
    ASSERT_NE(kid(second.root(), "z"), nullptr);
    EXPECT_EQ(kid(second.root(), "z")->start_offset, 1u);
}

// R-Emp: when the alternatives run out, the result is Fail.
TEST(IpgAlt, WhenEveryAlternativeFailsTheRuleFails) {
    auto r = run("Top = struct { a: uint8 where a == 7 }\n"
                 "    | struct { b: uint8 where b == 8 }\n"
                 "    | struct { c: uint8 where c == 9 }",
                 "Top", {1});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
}

// R-AltSucc/R-AltFail start each alternative from a fresh environment and an
// empty tree list: nothing a failed arm bound or built survives into the next.
TEST(IpgAlt, AFailedArmLeavesNoBindingsBehind) {
    auto r = run("Top = struct { a: uint8, ghost: compute(a * 100 : uint16),\n"
                 "               z: uint8 where z == 9 }\n"
                 "    | struct { b: uint8, c: uint8 }",
                 "Top", {1, 2});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* arm = kid(r.root(), "alt_1");
    ASSERT_NE(arm, nullptr);
    ASSERT_EQ(arm->kids.size(), 1u);
    const bbq::zcow::node* body = arm->kids[0].get();
    EXPECT_EQ(kid(body, "ghost"), nullptr);   // the first arm's compute is gone
    EXPECT_NE(kid(body, "b"), nullptr);
    EXPECT_NE(kid(body, "c"), nullptr);
}

// A-Seq1/A-Seq2: terms are threaded left to right, and a later term sees what
// earlier terms bound. Varying the binding has to move the terms after it —
// a fixed-width read would satisfy any single case.
TEST(IpgSeq, LaterTermsSeeEarlierBindings) {
    const char* g = "Top = struct { n: uint8, data: bytes[n], tail: uint8,\n"
                    "               k: compute(tail : uint8) }";
    struct Case { std::vector<uint8_t> bytes; size_t consumed; size_t tail_at; };
    for (auto& c : {Case{{0, 0x7F}, 2u, 1u},
                    Case{{1, 0xAA, 0x7F}, 3u, 2u},
                    Case{{3, 0xAA, 0xBB, 0xCC, 0x7F}, 5u, 4u}}) {
        auto r = run(g, "Top", c.bytes);
        ASSERT_TRUE(r.compiled) << r.error;
        ASSERT_TRUE(r.success) << r.error;
        EXPECT_EQ(r.meta.bytes_consumed, c.consumed) << "n = " << (int)c.bytes[0];
        EXPECT_EQ(kid(r.root(), "tail")->start_offset, c.tail_at);
        EXPECT_EQ(computed(kid(r.root(), "k")), 0x7F);
    }

    // A binding that overruns the input fails; it is not clamped to what is left.
    auto over = run(g, "Top", {9, 0xAA, 0xBB});
    ASSERT_TRUE(over.compiled) << over.error;
    EXPECT_FALSE(over.success);
}

// A-Fail: one term failing fails the whole alternative — the terms before it do
// not stand on their own.
TEST(IpgSeq, OneFailingTermFailsTheWholeAlternative) {
    auto r = run("Top = struct { a: uint8, b: uint8 where b == 9, c: uint8 }",
                 "Top", {1, 2, 3});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
}

// T-Ter/T-NTSucc side condition `0 ≤ l ≤ r ≤ |s|`, plus footnote 1: `[n, n]` is
// a valid (empty) interval — "we need it to represent empty intervals for the
// empty string". A zero-length section in a table of sections is the file-format
// shape that depends on it.
TEST(IpgInterval, AnEmptyIntervalIsValid) {
    auto r = run("Sec = struct { body: bytes[@remaining] }\n"
                 "Top = struct { a: uint8, s: Sec[2, 2], z: uint8, k: compute(z : uint8) }",
                 "Top", {1, 2, 3, 4});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* s = kid(r.root(), "s");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->start_offset, s->end_offset);
    // Empty means empty: it read nothing, and the field after it is where the
    // window left the cursor rather than a byte further on.
    auto* body = kid(s, "body");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->end_offset, body->start_offset);
    EXPECT_EQ(kid(r.root(), "z")->start_offset, 2u);
    EXPECT_EQ(computed(kid(r.root(), "k")), 3);
}

// The same window, with the length coming from the input: zero reads nothing and
// carries on, non-zero reads exactly that much. One case alone cannot tell an
// empty window from a window that was ignored.
TEST(IpgInterval, AnEmptyIntervalComputedFromDataIsValid) {
    const char* g = "Sec = struct { body: bytes[@remaining] }\n"
                    "Top = struct { off: uint8, len: uint8, s: Sec[off, off + len] }";

    auto empty = run(g, "Top", {2, 0, 9, 9});
    ASSERT_TRUE(empty.compiled) << empty.error;
    ASSERT_TRUE(empty.success) << empty.error;
    auto* eb = kid(kid(empty.root(), "s"), "body");
    ASSERT_NE(eb, nullptr);
    EXPECT_EQ(eb->end_offset - eb->start_offset, 0u)
        << "an empty window yields an empty read, not a failure";

    auto two = run(g, "Top", {2, 2, 9, 9});
    ASSERT_TRUE(two.success) << two.error;
    auto* tb = kid(kid(two.root(), "s"), "body");
    ASSERT_NE(tb, nullptr);
    EXPECT_EQ(tb->start_offset, 2u);
    EXPECT_EQ(tb->end_offset - tb->start_offset, 2u);
}

// The same side condition, violated: start after end.
TEST(IpgInterval, StartAfterEndFails) {
    auto r = run("Sub = struct { v: uint8 }\n"
                 "Top = struct { lo: uint8, hi: uint8, s: Sub[lo, hi] }",
                 "Top", {6, 2, 0, 0, 0, 0, 9});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
}

TEST(IpgInterval, StartAfterEndIsRejectedStaticallyWhenItIsLiteral) {
    auto d = diagnose("Sub = struct { v: uint8 }\n"
                      "Top = struct { s: Sub[4, 2] }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// …and an end past the input.
TEST(IpgInterval, AnEndPastTheInputFails) {
    auto r = run("Sub = struct { v: uint8 }\n"
                 "Top = struct { n: uint8, s: Sub[0, n] }",
                 "Top", {99, 1, 2});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
}

// T-NTSucc: the subparser is handed `s[l, r]` and nothing else. A nested window
// may only narrow its parent's.
TEST(IpgInterval, ANestedWindowMustLieInsideItsParent) {
    auto r = run("Inner = struct { v: uint8 }\n"
                 "Mid   = struct { i: Inner[6, 7] }\n"
                 "Top   = struct { m: Mid[0, 4], tail: bytes[4] }",
                 "Top", {0, 1, 2, 3, 4, 5, 6, 7});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success) << "the inner window escapes the outer one";
}

TEST(IpgInterval, ASubparserCannotReadPastItsWindow) {
    auto r = run("Sub = struct { a: uint8, b: uint8, c: uint8 }\n"
                 "Top = struct { s: Sub[0, 2] }",
                 "Top", {1, 2, 3, 4});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success) << "Sub wants 3 bytes; its window holds 2";
}

// Divergence from T-NTSucc. The paper re-bases a subparse: inside `B[l, r]` the
// input IS `s[l, r]`, so `EOI` there is `r - l` and offsets come back rebased by
// `+l`. BBQ keeps one absolute coordinate system — `EOI` is the whole input —
// and names the window with `@end`/`@start`/`@remaining`. Same confinement, one
// set of offsets.
TEST(IpgInterval, EoiIsTheWholeInputAndTheWindowIsNamedByAtEnd) {
    auto r = run("Sub = struct { e: compute(EOI : uint32),\n"
                 "               s: compute(@start : uint32),\n"
                 "               p: compute(@pos : uint32),\n"
                 "               n: compute(@end : uint32),\n"
                 "               r: compute(@remaining : uint32) }\n"
                 "Top = struct { w: Sub[2, 6] }",
                 "Top", {0, 1, 2, 3, 4, 5, 6, 7});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* w = kid(r.root(), "w");
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(computed(kid(w, "e")), 8) << "EOI is the input, not the window";
    EXPECT_EQ(computed(kid(w, "s")), 2);
    EXPECT_EQ(computed(kid(w, "p")), 2);
    EXPECT_EQ(computed(kid(w, "n")), 6) << "@end is the window's right edge";
    EXPECT_EQ(computed(kid(w, "r")), 4);
}

// T-Attr: an attribute definition extends the environment and consumes nothing.
TEST(IpgAttr, AComputeConsumesNoInput) {
    auto r = run("Top = struct { a: uint8, c: compute(a + 1 : uint8), b: uint8 }",
                 "Top", {1, 2});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 2u);
    EXPECT_EQ(computed(kid(r.root(), "c")), 2);
}

// T-Pred/T-PredFail: a predicate consumes nothing and fails the alternative when
// it does not hold — wherever it is attached. A predicate the code generator
// drops is a check the format does not get.
TEST(IpgPred, APredicateConsumesNoInput) {
    auto r = run("Top = struct { a: uint8 where a == 1, b: uint8 }", "Top", {1, 2});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 2u);
}

TEST(IpgPred, AStructLevelPredicateIsEnforced) {
    const char* g = "Top = struct { a: uint8, b: uint8 } where a + b == 3";
    EXPECT_TRUE(run(g, "Top", {1, 2}).success);
    EXPECT_FALSE(run(g, "Top", {1, 9}).success);
}

TEST(IpgPred, AnArrayLevelPredicateIsEnforced) {
    const char* g = "Top = struct { xs: array<uint8>[2] where xs[0] == 4, z: uint8 }";
    EXPECT_TRUE(run(g, "Top", {4, 5, 6}).success);
    EXPECT_FALSE(run(g, "Top", {9, 5, 6}).success);
}

TEST(IpgPred, AnArrayLevelPredicateIsEnforcedByTheCReaderToo) {
    const char* g = "Top = struct { xs: array<uint8>[2] where xs[0] == 4, z: uint8 }";
    std::string err;
    EXPECT_TRUE(c_run(g, "top_t", "top_read", {4, 5, 6}, "out.z == 6", true, &err)) << err;
    EXPECT_TRUE(c_run(g, "top_t", "top_read", {9, 5, 6}, "", false, &err)) << err;
}

TEST(IpgPred, AnElementPredicateIsEnforced) {
    const char* g = "E = struct { v: uint8 where v < 10 }\n"
                    "Top = struct { xs: array<E>[2] }";
    EXPECT_TRUE(run(g, "Top", {1, 2}).success);
    EXPECT_FALSE(run(g, "Top", {1, 99}).success);
}

// T-Array-Empty: "when e2's value is less than or equal to e1's value, the
// for-loop for an array term does not run; it imposes no constraints and accepts
// any string."
TEST(IpgArray, ZeroTripsImposeNoConstraint) {
    auto r = run("E   = struct { v: uint8 where v == 200 }\n"
                 "Top = struct { n: uint8, xs: array<E>[n], z: uint8 }",
                 "Top", {0, 9});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* xs = kid(r.root(), "xs");
    ASSERT_NE(xs, nullptr);
    EXPECT_EQ(xs->kids.size(), 0u);
    EXPECT_EQ(r.meta.bytes_consumed, 2u);
}

// The loop variable ranges over `e1 .. e2 - 1` — the count, exclusive.
TEST(IpgArray, TheLoopVariableRangesOverTheCountExclusive) {
    auto r = run("E   = struct { seen: compute(@index : uint8), v: uint8 }\n"
                 "Top = struct { xs: array<E>[3] }",
                 "Top", {10, 11, 12});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* xs = kid(r.root(), "xs");
    ASSERT_EQ(xs->kids.size(), 3u);
    EXPECT_EQ(computed(kid(xs->kids[0].get(), "seen")), 0);
    EXPECT_EQ(computed(kid(xs->kids[1].get(), "seen")), 1);
    EXPECT_EQ(computed(kid(xs->kids[2].get(), "seen")), 2);
}

// ══════════════════════════════════════════════════════════════════════
// §3.4  The full language
// ══════════════════════════════════════════════════════════════════════

// Implicit intervals: the left-most term starts at 0 and each later term starts
// where the previous one ended, so a plain sequence needs no intervals at all.
TEST(IpgImplicit, TermsFollowOneAnotherWithoutWrittenIntervals) {
    auto r = run("Top = struct { a: uint8, b: uint16le, c: uint8 }",
                 "Top", {1, 2, 0, 3});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(kid(r.root(), "a")->start_offset, 0u);
    EXPECT_EQ(kid(r.root(), "b")->start_offset, 1u);
    EXPECT_EQ(kid(r.root(), "c")->start_offset, 3u);
}

// "If there is only one expression between the parentheses, it is viewed as the
// length of the interval" — the right endpoint is the left plus that length.
// It is a window, so what sits inside it cannot read past it.
TEST(IpgImplicit, ALoneExpressionIsALength) {
    auto r = run("Sub = struct { v: uint8, w: uint8 }\n"
                 "Top = struct { s: Sub[2], z: uint8 }",
                 "Top", {1, 2, 3, 4});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(kid(r.root(), "s")->start_offset, 0u);
    EXPECT_EQ(kid(r.root(), "s")->end_offset, 2u);
}

TEST(IpgImplicit, ALengthWindowConfinesWhatIsInsideIt) {
    auto r = run("Sub = struct { a: uint8, b: uint8, c: uint8 }\n"
                 "Top = struct { s: Sub[2] }",
                 "Top", {1, 2, 3, 4});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success) << "Sub wants 3 bytes; the length window holds 2";
}

// Local rules (`where D → …`): a nested rule may still see the enclosing scope,
// which is what makes a helper rule usable when it depends on an outer field.
// BBQ resolves an unqualified name outward through the enclosing scopes.
TEST(IpgLocal, ANestedRuleSeesTheEnclosingScope) {
    auto r = run("Inner = struct { x: uint8 where x == v }\n"
                 "Mid   = struct { i: Inner }\n"
                 "Top   = struct { v: uint8, m: Mid }",
                 "Top", {5, 5});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_TRUE(r.success) << r.error;

    auto bad = run("Inner = struct { x: uint8 where x == v }\n"
                   "Mid   = struct { i: Inner }\n"
                   "Top   = struct { v: uint8, m: Mid }",
                   "Top", {5, 6});
    EXPECT_FALSE(bad.success);
}

// Switch terms: "the choices are executed from left to right; if one of the
// conditions succeeds, then the corresponding nonterminal is used… and the
// remaining choices are skipped."
TEST(IpgSwitch, TheFirstMatchingCaseWinsAndLaterOnesAreSkipped) {
    // 4 is in both ranges. The arms are a byte and two bytes wide, and the parse
    // records which one it took, so the winner is witnessed twice over.
    auto r = run("X = struct { v: uint8 }\n"
                 "Y = struct { v: uint16le }\n"
                 "Top = struct { t: uint8, body: switch(t) { 0 .. 5: X; 3 .. 9: Y; default: reject; } }",
                 "Top", {4, 7, 8});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 2u) << "X (one byte) won, not Y (two)";
    ASSERT_NE(kid(r.root(), "body"), nullptr);
    EXPECT_EQ(kid(r.root(), "body")->variant_tag, 0) << "the first case, not the second";
}

// "If all conditions fail, the default choice is used."
TEST(IpgSwitch, TheDefaultIsTakenWhenNoCaseMatches) {
    auto r = run("X = struct { v: uint8 }\n"
                 "D = struct { v: uint16le }\n"
                 "Top = struct { t: uint8, body: switch(t) { 1: X; default: D; } }",
                 "Top", {9, 1, 2});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.meta.bytes_consumed, 3u);
    ASSERT_NE(kid(r.root(), "body"), nullptr);
    EXPECT_NE(kid(r.root(), "body")->variant_tag, 0) << "the default, not case 1";

    // The listed case, for the same reason the arms are different widths: a test
    // that only ever sees the default cannot tell dispatch from a constant.
    auto listed = run("X = struct { v: uint8 }\n"
                      "D = struct { v: uint16le }\n"
                      "Top = struct { t: uint8, body: switch(t) { 1: X; default: D; } }",
                      "Top", {1, 7});
    ASSERT_TRUE(listed.success) << listed.error;
    EXPECT_EQ(listed.meta.bytes_consumed, 2u);
    EXPECT_EQ(kid(listed.root(), "body")->variant_tag, 0);
}

// "The default branch must fail because of its always-invalid interval" — the
// fail-closed idiom. BBQ spells that `default: reject`.
TEST(IpgSwitch, ARejectDefaultFailsClosed) {
    auto r = run("X = struct { v: uint8 }\n"
                 "Top = struct { t: uint8, body: switch(t) { 1: X; default: reject; } }",
                 "Top", {9, 1});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_FALSE(r.success);
}

// Blackbox parsers: "by using an interval, the parser can control what can be
// seen by an external parser." The extern is handed its window and nothing else.
TEST(IpgBlackbox, AnExternSeesOnlyItsInterval) {
    const char* g = "Top = struct { a: uint8, e: extern(\"readit\", \"int\")[1, 3], z: uint8 }";

    ipg::ExternWitness::consume = 2;
    auto r = run(g, "Top", {0x10, 0x11, 0x12, 0x13});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    ASSERT_EQ(ipg::ExternWitness::seen.size(), 2u) << "the window, not the rest of the input";
    EXPECT_EQ(ipg::ExternWitness::seen[0], 0x11);
    EXPECT_EQ(ipg::ExternWitness::seen[1], 0x12);

    // Move the window and the blackbox is handed different bytes — which is how a
    // GIF parser hands a decompressor exactly the code area and nothing else.
    auto moved = run("Top = struct { a: uint8, e: extern(\"readit\", \"int\")[2, 4], z: uint8 }",
                     "Top", {0x10, 0x11, 0x12, 0x13, 0x14});
    ASSERT_TRUE(moved.success) << moved.error;
    ASSERT_EQ(ipg::ExternWitness::seen.size(), 2u);
    EXPECT_EQ(ipg::ExternWitness::seen[0], 0x12);
    EXPECT_EQ(ipg::ExternWitness::seen[1], 0x13);

    // A blackbox that cannot satisfy itself from what the window holds fails the
    // parse — it is a term like any other (T-NTFail).
    ipg::ExternWitness::consume = 4;
    auto starved = run(g, "Top", {0x10, 0x11, 0x12, 0x13});
    ASSERT_TRUE(starved.compiled) << starved.error;
    EXPECT_FALSE(starved.success) << "asked for 4 bytes inside a 2-byte window";
}

// ══════════════════════════════════════════════════════════════════════
// §3.5  Expressiveness — IPGs ⊄ CFGs
// ══════════════════════════════════════════════════════════════════════

// The paper's witness: {aⁿbⁿcⁿ | n > 0} is an IPG.
//   S → ⟨EOI mod 3 = 0⟩ {n=EOI/3} A[0,n] B[n,2n] C[2n,3n]
TEST(IpgExpressive, AnBnCnIsExpressible) {
    const char* g =
        "Top = struct {\n"
        "  n: compute(EOI / 3 : uint32),\n"
        "  a: bytes[n], b: bytes[n], c: bytes[n]\n"
        "} where EOI % 3 == 0";
    auto ok = run(g, "Top", {'a', 'a', 'b', 'b', 'c', 'c'});
    ASSERT_TRUE(ok.compiled) << ok.error;
    ASSERT_TRUE(ok.success) << ok.error;
    EXPECT_EQ(ok.meta.bytes_consumed, 6u);

    EXPECT_FALSE(run(g, "Top", {'a', 'a', 'b', 'b', 'c'}).success);
}

// Divergence. The paper admits left recursion when the interval strictly shrinks
// — `Int → Int[0, EOI-1] Digit[EOI-1, EOI]`. BBQ has no interval-shrinking
// termination argument, so it rejects any recursion an array or optional does
// not guard, rather than accepting some and looping on the rest.
TEST(IpgExpressive, UnguardedRecursionIsRejected) {
    auto d = diagnose("Int = struct { head: Int[0, EOI - 1], last: uint8 }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

TEST(IpgExpressive, RecursionGuardedByAnArrayIsAccepted) {
    auto r = run("Node = struct { n: uint8, kids: array<Node>[n] }",
                 "Node", {2, 0, 1, 0});
    ASSERT_TRUE(r.compiled) << r.error;
    EXPECT_TRUE(r.success) << r.error;
}

// ══════════════════════════════════════════════════════════════════════
// §4  Case studies
// ══════════════════════════════════════════════════════════════════════

// §4.1 ELF, and Figure 2: a header holds an offset and a length, and the data
// lives wherever the header says — including behind the cursor.
TEST(IpgCaseStudy, RandomAccessFromAParsedOffset) {
    // The header is read at [0,2) after the cursor has already passed it, and the
    // data at an offset the header gave — behind the cursor. Reading the values
    // back is what says the parse went there, rather than staying where it was.
    const char* g = "Data = struct { a: uint8, b: uint8 }\n"
                    "Hdr  = struct { off: uint8, len: uint8 }\n"
                    "Top  = struct { pad: bytes[4], h: Hdr[0, 2],\n"
                    "                d: Data[h.off, h.off + h.len],\n"
                    "                ka: compute(d.a : uint8), kb: compute(d.b : uint8) }";
    auto r = run(g, "Top", {1, 2, 0xAA, 0xBB, 0xCC, 0xDD});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* d = kid(r.root(), "d");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->start_offset, 1u) << "the section starts behind the cursor";
    EXPECT_EQ(computed(kid(r.root(), "ka")), 2);      // input[1]
    EXPECT_EQ(computed(kid(r.root(), "kb")), 0xAA);   // input[2]

    // Move the offset the header carries, and the data moves with it.
    auto moved = run(g, "Top", {3, 2, 0xAA, 0xBB, 0xCC, 0xDD});
    ASSERT_TRUE(moved.success) << moved.error;
    EXPECT_EQ(computed(kid(moved.root(), "ka")), 0xBB);   // input[3]
    EXPECT_EQ(computed(kid(moved.root(), "kb")), 0xCC);   // input[4]

    // An offset past the input is a parse failure, not a clamp.
    auto over = run(g, "Top", {9, 2, 0xAA, 0xBB, 0xCC, 0xDD});
    ASSERT_TRUE(over.compiled) << over.error;
    EXPECT_FALSE(over.success);
}

// §4.1, Figure 9b line 3: sections are placed by the offsets and sizes held in
// a previously parsed table of section headers —
//   for i=0 to H.num do Sec[SH(i).ofs, SH(i).ofs + SH(i).sz]
TEST(IpgCaseStudy, TheSectionTablePlacesSectionsByItsOwnEntries) {
    // Sections of DIFFERENT sizes, listed out of order, so neither a fixed stride
    // nor a sequential walk produces this result. Entry 0 says [6,8), entry 1 says
    // [5,6): the first section is two bytes and comes after the second.
    const char* g = "SH  = struct { ofs: uint8, sz: uint8 }\n"
                    "Sec = struct { body: bytes[@remaining] }\n"
                    "Top = struct {\n"
                    "  n:    uint8,\n"
                    "  shs:  array<SH>[n],\n"
                    "  secs: array<Sec>[n] @ [shs[@index].ofs, shs[@index].ofs + shs[@index].sz]\n"
                    "}";
    auto r = run(g, "Top", {2, 6, 2, 5, 1, 0xAA, 0xBB, 0xCC});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* secs = kid(r.root(), "secs");
    ASSERT_NE(secs, nullptr);
    ASSERT_EQ(secs->kids.size(), 2u);

    auto* b0 = kid(secs->kids[0].get(), "body");
    auto* b1 = kid(secs->kids[1].get(), "body");
    ASSERT_NE(b0, nullptr);
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(b0->start_offset, 6u);
    EXPECT_EQ(b0->end_offset - b0->start_offset, 2u);   // 0xBB 0xCC
    EXPECT_EQ(b1->start_offset, 5u);
    EXPECT_EQ(b1->end_offset - b1->start_offset, 1u);   // 0xAA

    // A table entry pointing outside the file fails the parse — a section header
    // is a claim about the file, and an unmet one is malformed input.
    auto bad = run(g, "Top", {2, 6, 2, 99, 1, 0xAA, 0xBB, 0xCC});
    ASSERT_TRUE(bad.compiled) << bad.error;
    EXPECT_FALSE(bad.success);
}

// The same random access through the generated C reader. The CEK resolves a path
// against the capture; the C reader resolves it against a struct it is building,
// which is a different mechanism and could agree on shape while disagreeing on
// which bytes it went to.
TEST(IpgCaseStudy, TheCReaderTakesTheSameRandomAccess) {
    const char* g = "Data = struct { a: uint8, b: uint8 }\n"
                    "Hdr  = struct { off: uint8, len: uint8 }\n"
                    "Top  = struct { pad: bytes[4], h: Hdr[0, 2],\n"
                    "                d: Data[h.off, h.off + h.len] }";
    std::string err;
    EXPECT_TRUE(c_run(g, "top_t", "top_read", {1, 2, 0xAA, 0xBB, 0xCC, 0xDD},
                      "out.h.off == 1 && out.d.a == 2 && out.d.b == 0xAA",
                      true, &err)) << err;
    EXPECT_TRUE(c_run(g, "top_t", "top_read", {3, 2, 0xAA, 0xBB, 0xCC, 0xDD},
                      "out.d.a == 0xBB && out.d.b == 0xCC", true, &err)) << err;
    EXPECT_TRUE(c_run(g, "top_t", "top_read", {9, 2, 0xAA, 0xBB, 0xCC, 0xDD},
                      "", false, &err)) << err;
}

// §4.2 GIF, and §2: the type-length-value pattern — a type picks the subparser,
// a length bounds it.
TEST(IpgCaseStudy, TypeLengthValue) {
    const char* g =
        "Text = struct { s: bytes[@remaining] }\n"
        "Num  = struct { v: uint16le }\n"
        "TLV  = struct { t: uint8, len: uint8 @rest,\n"
        "                v: switch(t) { 1: Text; 2: Num; default: reject; } }\n"
        "Top  = struct { items: array<TLV>(none, eof) }";
    auto r = run(g, "Top", {1, 3, 'a', 'b', 'c', 2, 2, 0x34, 0x12});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    auto* items = kid(r.root(), "items");
    ASSERT_NE(items, nullptr);
    EXPECT_EQ(items->kids.size(), 2u);

    // An unlisted type is malformed input, not data to skip.
    EXPECT_FALSE(run(g, "Top", {7, 1, 0x00}).success);
}

// §4.3 PDF: backward parsing. The offset table is found by reading from the end
// of the file, which an interval anchored at EOI expresses directly.
TEST(IpgCaseStudy, BackwardParsingFromTheEndOfInput) {
    // The trailer is read first, at the very end, and points back into the file.
    // Both the trailer's value and where it sends the parse are checked, and the
    // offset is varied so a parser that ignored it could not pass both cases.
    const char* g = "Tail = struct { off: uint8 }\n"
                    "Body = struct { b: bytes[@remaining] }\n"
                    "Top  = struct { t: Tail[EOI - 1, EOI], d: Body[t.off, EOI - 1],\n"
                    "                k: compute(t.off : uint8) }";
    struct Case { uint8_t off; size_t at, len; };
    for (auto& c : {Case{1, 1u, 2u}, Case{0, 0u, 3u}, Case{3, 3u, 0u}}) {
        auto r = run(g, "Top", {0xAA, 0xBB, 0xCC, c.off});
        ASSERT_TRUE(r.compiled) << r.error;
        ASSERT_TRUE(r.success) << r.error;
        EXPECT_EQ(computed(kid(r.root(), "k")), c.off);
        auto* b = kid(kid(r.root(), "d"), "b");
        ASSERT_NE(b, nullptr) << "off " << (int)c.off;
        EXPECT_EQ(b->start_offset, c.at) << "off " << (int)c.off;
        EXPECT_EQ(b->end_offset - b->start_offset, c.len) << "off " << (int)c.off;
    }

    // A trailer pointing past where the body must end is malformed.
    auto bad = run(g, "Top", {0xAA, 0xBB, 0xCC, 9});
    ASSERT_TRUE(bad.compiled) << bad.error;
    EXPECT_FALSE(bad.success);
}

// §4.3 PDF: two-pass parsing. "Intervals can overlap with each other to let the
// parser parse the same area more than once."
TEST(IpgCaseStudy, OverlappingIntervalsParseTheSameBytesTwice) {
    // Both windows are [0,2). The point is not that they overlap but that the two
    // passes read the same bytes DIFFERENTLY — one 16-bit value, two 8-bit ones —
    // so the values are what the test is about.
    auto r = run("Wide   = struct { x: uint16le }\n"
                 "Narrow = struct { hi: uint8, lo: uint8 }\n"
                 "Top    = struct { w: Wide[0, 2], n: Narrow[0, 2],\n"
                 "                  kx: compute(w.x : uint16),\n"
                 "                  khi: compute(n.hi : uint8),\n"
                 "                  klo: compute(n.lo : uint8) }",
                 "Top", {0x34, 0x12});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(kid(r.root(), "w")->start_offset, 0u);
    EXPECT_EQ(kid(r.root(), "n")->start_offset, 0u);
    EXPECT_EQ(computed(kid(r.root(), "kx")), 0x1234);
    EXPECT_EQ(computed(kid(r.root(), "khi")), 0x34);
    EXPECT_EQ(computed(kid(r.root(), "klo")), 0x12);
}

// ══════════════════════════════════════════════════════════════════════
// §5  Termination
// ══════════════════════════════════════════════════════════════════════
//
// BBQ does not run the paper's cycle enumeration and SMT check. It enforces the
// stronger structural rule instead: recursion must pass through a construct that
// can stop (an array's count or terminator, an optional's absence), and an
// unbounded array's element must make progress. Every nontermination witness the
// paper gives is rejected under it — statically where the shape is decidable,
// at the first non-advancing iteration where it is not.

// Figure 11b: `S → num[0,1] S[num.val, EOI]` — a seek back to a data-controlled
// offset, which the paper shows never terminates when num.val is 0.
TEST(IpgTermination, ASeekingSelfReferenceIsRejected) {
    auto d = diagnose("S = struct { num: uint8, rest: S[num, EOI] }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// Figure 11d: `S → ""[0,0] S[0, EOI]` — recursion on the same interval.
TEST(IpgTermination, RecursionOnTheSameIntervalIsRejected) {
    auto d = diagnose("S = struct { e: bytes[0], rest: S[0, EOI] }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// The paper's example of an obviously non-terminating grammar:
//   A → B[0, EOI] / s[0,1];  B → A[0, EOI] / s[0,1]
TEST(IpgTermination, AMutualCycleOnTheSameIntervalIsRejected) {
    auto d = diagnose("A = struct { b: B[0, EOI] }\n"
                      "B = struct { a: A[0, EOI] }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// Figure 11c: repeating a subparser that consumes nothing. The paper's extension
// adds `A.end > 0` for a rule that consumes at least one terminal; the shape
// that fails it is an unbounded repeat of a zero-width element, and it must be
// caught rather than run.
TEST(IpgTermination, AnEofArrayOfAZeroWidthElementIsRejected) {
    auto d = diagnose("E   = struct { c: compute(1 : uint8) }\n"
                      "Top = struct { xs: array<E>(none, eof) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

TEST(IpgTermination, AnEofArrayOfAnEmptyByteRunIsRejected) {
    auto d = diagnose("E   = struct { b: bytes[0] }\n"
                      "Top = struct { xs: array<E>(none, eof) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

TEST(IpgTermination, AnUntilArrayOfAZeroWidthElementIsRejected) {
    auto d = diagnose("E   = struct { c: compute(1 : uint8) }\n"
                      "Top = struct { xs: array<E>(none, until(@remaining == 0)) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// A counted array of a zero-width element terminates by its count, so it is
// allowed — the count is the decreasing measure.
TEST(IpgTermination, ACountedArrayOfAZeroWidthElementIsFine) {
    auto r = run("E   = struct { c: compute(1 : uint8) }\n"
                 "Top = struct { xs: array<E>[3] }",
                 "Top", {});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(kid(r.root(), "xs")->kids.size(), 3u);
}

// An element whose whole width the data decides establishes nothing: the input
// that makes it read zero is the input it never terminates on. Theorem 5.1 admits
// the grammars whose termination is settled up front, so this one is refused.
TEST(IpgTermination, AnElementWhoseWidthOnlyTheDataDecidesIsRejected) {
    auto d = diagnose("Top = struct { n: uint8, xs: array<bytes[n]>(none, eof) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// One arm of a biased choice that reads nothing is enough — the choice can take it.
TEST(IpgTermination, AnElementWithOneZeroWidthArmIsRejected) {
    auto d = diagnose("E   = struct { v: uint8 } | struct { c: compute(1 : uint8) }\n"
                      "Top = struct { xs: array<E>(none, eof) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// A fixed-width prefix settles it: every iteration reads that prefix whatever the
// rest does, so the array walks to the end of the input.
TEST(IpgTermination, AnElementWithAFixedWidthPrefixIsAccepted) {
    auto r = run("E   = struct { n: uint8, b: bytes[n] }\n"
                 "Top = struct { xs: array<E>(none, eof) }",
                 "Top", {2, 0xAA, 0xBB, 0, 1, 0xCC});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(kid(r.root(), "xs")->kids.size(), 3u);
}

TEST(IpgTermination, TheCReaderWalksTheSameEofArrayToTheEnd) {
    std::string err;
    EXPECT_TRUE(c_run("E   = struct { n: uint8, b: bytes[n] }\n"
                      "Top = struct { xs: array<E>(none, eof) }",
                      "top_t", "top_read", {2, 0xAA, 0xBB, 0, 1, 0xCC},
                      "out.xs.count == 3", true, &err)) << err;
}

// The GIF block list — `Blocks → Block[0, EOI] Blocks[Block.end, EOI]` — is the
// case the paper's extension exists to admit. BBQ writes it as an eof array, and
// it must keep working.
TEST(IpgTermination, AnEofArrayOfAProductiveElementTerminates) {
    auto r = run("Block = struct { len: uint8, body: bytes[len] }\n"
                 "Top   = struct { blocks: array<Block>(none, eof) }",
                 "Top", {2, 0xAA, 0xBB, 1, 0xCC});
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(kid(r.root(), "blocks")->kids.size(), 2u);
}

// §5: "if the input IPG contains blackbox parsers, we assume that those blackbox
// parsers always terminate; their termination checking is delegated to
// programmers." Terminating is not the same as consuming — a blackbox decides its
// own width, so an unbounded repeat of one is a loop whose progress the grammar
// cannot establish, and it is refused with the rest.
TEST(IpgTermination, AnUnboundedArrayOfABlackboxIsRejected) {
    auto d = diagnose("E   = struct { e: extern(\"readit\", \"int\") }\n"
                      "Top = struct { xs: array<E>(none, eof) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_FALSE(d.ok) << d.text;
}

// Giving that same element a width of its own settles the loop: the blackbox may
// still read whatever it likes, but the iteration is productive without it.
TEST(IpgTermination, ABlackboxBehindAFixedWidthFieldIsFine) {
    auto d = diagnose("E   = struct { tag: uint8, e: extern(\"readit\", \"int\") }\n"
                      "Top = struct { xs: array<E>(none, eof) }");
    ASSERT_TRUE(d.parsed);
    EXPECT_TRUE(d.ok) << d.text;
}
