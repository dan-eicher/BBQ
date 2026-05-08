// Regression tests for the worklist-based PEG analysis suite:
// FIRST/FOLLOW/nullable Kildall, ordered-choice masking, Star/suffix
// conflicts, always-failing productions, useless predicates, unused
// terminals.

#include <gtest/gtest.h>
#include <cstring>
#include <string>

#include "Parser.h"
#include "PegAnalysis.h"

using namespace PegGrammar;
using namespace pegc;

static Grammar* parse(const char* src) {
    Parser parser;
    parser.init(src, static_cast<int>(strlen(src)));
    if (!parser.parse()) return nullptr;
    return parser.ast;
}

static AnalysisResult analyze_default(Grammar* g) {
    PegAnalysis a;
    return a.analyze(g);
}

static AnalysisResult analyze_with_coverage(Grammar* g) {
    PegAnalysis a;
    PegAnalysisConfig cfg;
    cfg.emit_coverage_warnings = true;
    return a.analyze(g, cfg);
}

static bool first_contains_literal(const std::vector<FirstElement>& s,
                                     const std::string& v) {
    for (auto& e : s)
        if (e.kind == FirstElement::Literal && e.value == v) return true;
    return false;
}

static bool warnings_contain(const AnalysisResult& r, const std::string& sub) {
    for (auto& w : r.warnings)
        if (w.find(sub) != std::string::npos) return true;
    return false;
}

// ═══════════════════════════════════════════════════════════════
// FIRST: Kildall convergence + nullable-prefix bug fix
// ═══════════════════════════════════════════════════════════════

TEST(PegKildall, MutualRecursionFirstSet) {
    // A = B "x" / "a"     (FIRST(A) ⊇ FIRST(B) ∪ {"a"})
    // B = A? "b"           (FIRST(B) ⊇ FIRST(A) ∪ {"b"})
    //
    // Old single-pass would cut off at A → B → A and miss elements.
    // Kildall converges: FIRST(A) ⊇ {"a", "b"}, FIRST(B) ⊇ {"a", "b"}.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "A = B \"x\" | \"a\" .\n"
        "B = [A] \"b\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    ASSERT_TRUE(r.ok());
    auto& a_first = r.productions["A"].first_set;
    auto& b_first = r.productions["B"].first_set;
    EXPECT_TRUE(first_contains_literal(a_first, "a"));
    EXPECT_TRUE(first_contains_literal(a_first, "b"));
    EXPECT_TRUE(first_contains_literal(b_first, "a"));
    EXPECT_TRUE(first_contains_literal(b_first, "b"));
}

TEST(PegKildall, SequenceWithNullablePrefix) {
    // A = "a"? "b"   →  FIRST(A) = {"a", "b"}
    // The old first_of(Sequence) only returned first_of(elements[0]),
    // missing "b" when "a" is nullable. Regression for that bug.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "A = [\"a\"] \"b\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    ASSERT_TRUE(r.ok());
    auto& af = r.productions["A"].first_set;
    EXPECT_TRUE(first_contains_literal(af, "a"));
    EXPECT_TRUE(first_contains_literal(af, "b"));
}

TEST(PegKildall, NullablePropagatesThroughCall) {
    // A is nullable iff its body is. B calls A?, so B's first element
    // is nullable through the call.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "A = [\"a\"] .\n"
        "B = A \"b\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.productions["A"].nullable);
    EXPECT_FALSE(r.productions["B"].nullable);
    auto& bf = r.productions["B"].first_set;
    EXPECT_TRUE(first_contains_literal(bf, "a"));
    EXPECT_TRUE(first_contains_literal(bf, "b"));
}

// ═══════════════════════════════════════════════════════════════
// FOLLOW: nullable tail + Star/Plus self-tail
// ═══════════════════════════════════════════════════════════════

TEST(PegKildall, FollowThroughNullableTail) {
    // S = A B
    // A = "a"
    // B = ["b"]
    // FOLLOW(A) ⊇ FIRST(B) = {"b"}, plus FOLLOW(S) since B is nullable.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "S = A B .\n"
        "A = \"a\" .\n"
        "B = [\"b\"] .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    ASSERT_TRUE(r.ok());
    auto& a_follow = r.productions["A"].follow_set;
    EXPECT_TRUE(first_contains_literal(a_follow, "b"));
    EXPECT_TRUE(first_contains_literal(a_follow, "$"));  // EOF via nullable B
}

TEST(PegKildall, FollowThroughStarSelfTail) {
    // R = (A B)*
    // FOLLOW(B) inside the body must include FIRST(A) — the body
    // can repeat, so B's tail in one iteration starts with FIRST(A).
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "R = { A B } .\n"
        "A = \"a\" .\n"
        "B = \"b\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    ASSERT_TRUE(r.ok());
    auto& b_follow = r.productions["B"].follow_set;
    EXPECT_TRUE(first_contains_literal(b_follow, "a"));
}

// ═══════════════════════════════════════════════════════════════
// Always-failing production
// ═══════════════════════════════════════════════════════════════

TEST(PegKildall, AlwaysFailingProduction) {
    // A is left-recursive — its FIRST stays empty after the worklist
    // skips it. B inherits that emptiness via its first call to A,
    // and since A is not nullable, B's FIRST is also empty —
    // triggering the always-failing warning on B.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "R = B .\n"
        "A = A \"x\" .\n"
        "B = A \"y\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    // A errors out left-recursive; the analysis still runs subsequent
    // passes and detects B as always-failing.
    EXPECT_TRUE(warnings_contain(r, "can never match"));
}

// ═══════════════════════════════════════════════════════════════
// Useless lookahead predicates
// ═══════════════════════════════════════════════════════════════

TEST(PegKildall, UselessAndPredicateAlwaysSucceeds) {
    // &("a"?) — body is nullable so the predicate trivially succeeds.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "R = &([\"a\"]) \"b\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    EXPECT_TRUE(warnings_contain(r, "&(...)"));
    EXPECT_TRUE(warnings_contain(r, "always succeeds"));
}

TEST(PegKildall, UselessNotPredicateAlwaysFails) {
    // !("a"?) — body is nullable so !body always fails.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "R = !([\"a\"]) \"b\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    EXPECT_TRUE(warnings_contain(r, "!(...)"));
    EXPECT_TRUE(warnings_contain(r, "always fails"));
}

// ═══════════════════════════════════════════════════════════════
// Unused tokens / charsets
// ═══════════════════════════════════════════════════════════════

TEST(PegKildall, UnusedTokenWarns) {
    auto* g = parse(
        "COMPILER Test\n"
        "TOKENS\n"
        "  used = 'a' .\n"
        "  dead = 'b' .\n"
        "PRODUCTIONS\n"
        "R = used .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    EXPECT_TRUE(warnings_contain(r, "token dead"));
    EXPECT_FALSE(warnings_contain(r, "token used"));
}

TEST(PegKildall, UnusedCharsetWarns) {
    auto* g = parse(
        "COMPILER Test\n"
        "CHARACTERS\n"
        "  used = 'a' .\n"
        "  dead = 'b' .\n"
        "TOKENS\n"
        "  tok = used .\n"
        "PRODUCTIONS\n"
        "R = tok .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    EXPECT_TRUE(warnings_contain(r, "charset dead"));
    EXPECT_FALSE(warnings_contain(r, "charset used"));
}

// ═══════════════════════════════════════════════════════════════
// Undeclared identifier (typo detection)
// ═══════════════════════════════════════════════════════════════

TEST(PegKildall, UndeclaredIdentifierIsError) {
    // misspelled_helper isn't a charset/token/production/built-in.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "R = misspelled_helper \"x\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    EXPECT_FALSE(r.ok());
    bool found = false;
    for (auto& e : r.errors)
        if (e.find("undeclared identifier") != std::string::npos &&
            e.find("misspelled_helper") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(PegKildall, BuiltinNamesGetNoSpecialTreatment) {
    // The is_builtin allowlist (ident, integer, keyword, ...) was
    // dropped: those names must now be declared as TOKENS (or whatever
    // kind) in the grammar like any other identifier. This test is
    // the regression guard against re-introducing the allowlist —
    // calling `ident<s>` without a grammar declaration is the same
    // undeclared-identifier error any other typo would produce.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "R (. std::string s; int64_t n; .)\n"
        "  = ident<s> integer<n> .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    EXPECT_FALSE(r.ok());
    bool found_ident = false;
    for (auto& e : r.errors)
        if (e.find("undeclared identifier") != std::string::npos &&
            e.find("ident") != std::string::npos) found_ident = true;
    EXPECT_TRUE(found_ident);
}

// ═══════════════════════════════════════════════════════════════
// --coverage opt-in checks (masking, Star/suffix conflict)
// ═══════════════════════════════════════════════════════════════

TEST(PegKildall, MaskingNotEmittedByDefault) {
    // Both alternatives start with the same literal "foo" — alt 1
    // is masked by alt 0 on FIRST-set grounds. Default analysis
    // doesn't surface it.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "R = \"foo\" \"a\" | \"foo\" \"b\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_default(g);
    EXPECT_FALSE(warnings_contain(r, "masked"));
}

TEST(PegKildall, MaskingEmittedUnderCoverage) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "R = \"foo\" \"a\" | \"foo\" \"b\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_with_coverage(g);
    EXPECT_TRUE(warnings_contain(r, "masked"));
}

TEST(PegKildall, StarSuffixConflictUnderCoverage) {
    // "a"* "a" — Star greedily consumes; trailing "a" never matches.
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "R = { \"a\" } \"a\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_with_coverage(g);
    EXPECT_TRUE(warnings_contain(r, "greedily"));
}

TEST(PegKildall, StarFollowConflictUnderCoverage) {
    // The Star/follow conflict from Redziejowski 2009: name's body
    // ends in [a-z]*, and name is followed by name in term — so
    // [a-z]* greedily eats letters that the next name() needs.
    // Detected via FOLLOW(name) ⊇ FIRST([a-z]*-body).
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "term = name name .\n"
        "name = letter { letter } .\n"
        "letter = \"a\" | \"b\" .\n"
        "END Test.\n");
    ASSERT_NE(g, nullptr);
    auto r = analyze_with_coverage(g);
    EXPECT_TRUE(warnings_contain(r, "FOLLOW("));
}
