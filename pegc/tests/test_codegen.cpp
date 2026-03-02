#include <gtest/gtest.h>
#include <string>
#include <cstring>

#include "Parser.h"
#include "PegAnalysis.h"
#include "ParserEmitter.h"

using namespace PegGrammar;
using namespace pegc;

static Grammar* parse(const char* src) {
    Parser parser;
    parser.init(src, static_cast<int>(strlen(src)));
    if (!parser.parse()) return nullptr;
    return parser.ast;
}

static std::string generate_impl(const char* src) {
    auto* g = parse(src);
    if (!g) return "";

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    if (!result.ok()) return "";

    ParserEmitter emitter(result);
    return emitter.emit_impl(g);
}

static int count_occurrences(const std::string& s, const std::string& sub) {
    int n = 0;
    size_t pos = 0;
    while ((pos = s.find(sub, pos)) != std::string::npos) {
        n++;
        pos += sub.size();
    }
    return n;
}

// ═══════════════════════════════════════════════════════════════
// Basic Code Generation
// ═══════════════════════════════════════════════════════════════

TEST(Codegen, LiteralEmission) {
    // Non-identifier literal → match()
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"++\" .\n"
        "END Test.\n"
    );
    EXPECT_NE(code.find("match(\"++\")"), std::string::npos);
    EXPECT_NE(code.find("parse_Test"), std::string::npos);
}

TEST(Codegen, KeywordEmission) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"struct\" .\n"
        "END Test.\n"
    );
    // "struct" looks like an identifier → should use keyword()
    EXPECT_NE(code.find("keyword(\"struct\")"), std::string::npos);
}

TEST(Codegen, SequenceEmission) {
    // Non-identifier literals → match()
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"(\" \"+\" \")\" .\n"
        "END Test.\n"
    );
    // Should have three match calls
    EXPECT_EQ(count_occurrences(code, "match(\""), 3);
}

TEST(Codegen, ChoiceEmission) {
    // Non-identifier literals → peek_at for head-fail optimization
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"(\" | \"+\" | \"-\" .\n"
        "END Test.\n"
    );
    // Should have peek_at for head-fail optimization (if/else chain)
    EXPECT_NE(code.find("peek_at"), std::string::npos);
    // Head-fail uses if/else, not lambdas
    EXPECT_EQ(code.find("[&]()"), std::string::npos);
}

TEST(Codegen, ChoiceGeneralEmission) {
    // Non-literal starts → general choice with lambdas
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = Foo | Bar .\n"
        "Foo = \"x\" .\n"
        "Bar = \"y\" .\n"
        "END Test.\n"
    );
    // General choice uses save/restore + lambda
    EXPECT_NE(code.find("save()"), std::string::npos);
    EXPECT_NE(code.find("[&]() -> bool"), std::string::npos);
    EXPECT_NE(code.find("restore("), std::string::npos);
}

TEST(Codegen, StarEmission) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = { \"x\" } .\n"
        "END Test.\n"
    );
    EXPECT_NE(code.find("for (;;)"), std::string::npos);
    EXPECT_NE(code.find("save()"), std::string::npos);
    EXPECT_NE(code.find("restore("), std::string::npos);
    // Body wrapped in lambda
    EXPECT_NE(code.find("[&]() -> bool"), std::string::npos);
}

TEST(Codegen, OptionalEmission) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = [ \"x\" ] .\n"
        "END Test.\n"
    );
    EXPECT_NE(code.find("save()"), std::string::npos);
    EXPECT_NE(code.find("restore("), std::string::npos);
    // Body wrapped in lambda
    EXPECT_NE(code.find("[&]() -> bool"), std::string::npos);
}

TEST(Codegen, SemanticAction) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"x\" (. result = 42; .) .\n"
        "END Test.\n"
    );
    EXPECT_NE(code.find("result = 42;"), std::string::npos);
}

TEST(Codegen, ParameterizedProduction) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Foo <. int &result .> = \"x\" .\n"
        "Test = Foo<result> .\n"
        "END Test.\n"
    );
    EXPECT_NE(code.find("parse_Foo(int& result)"), std::string::npos);
    EXPECT_NE(code.find("parse_Foo(result)"), std::string::npos);
}

TEST(Codegen, SkipSetup) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "COMMENTS FROM \"//\" TO \"\\n\"\n"
        "PRODUCTIONS\n"
        "Test = \"x\" .\n"
        "END Test.\n"
    );
    EXPECT_NE(code.find("set_comments"), std::string::npos);
    EXPECT_NE(code.find("//"), std::string::npos);
}

TEST(Codegen, RuleCallEmission) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = Foo .\n"
        "Foo = \"x\" .\n"
        "END Test.\n"
    );
    EXPECT_NE(code.find("parse_Foo()"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// DDCG composition: choice inside star
// ═══════════════════════════════════════════════════════════════

TEST(Codegen, ChoiceInsideStar) {
    // This is the pattern that was broken before: star body is a choice.
    // The star lambda wraps the choice, so return true/false in choice
    // returns from the lambda, not the production.
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = { \"+\" | \"-\" } .\n"
        "END Test.\n"
    );
    // Star wraps body in lambda
    EXPECT_NE(code.find("for (;;)"), std::string::npos);
    EXPECT_NE(code.find("[&]() -> bool"), std::string::npos);
    // Choice inside star uses head-fail (peek_at)
    EXPECT_NE(code.find("peek_at"), std::string::npos);
    // No bare "return true" at production level from inside choice
    // (return true/false are inside lambdas)
}

TEST(Codegen, AndPredicate) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = &\"x\" \"x\" .\n"
        "END Test.\n"
    );
    // And uses lambda + restore
    EXPECT_NE(code.find("[&]() -> bool"), std::string::npos);
    EXPECT_NE(code.find("restore("), std::string::npos);
    // And checks: if body failed, &e fails
    EXPECT_NE(code.find("if (!_ok"), std::string::npos);
}

TEST(Codegen, NotPredicate) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = !\"y\" \"x\" .\n"
        "END Test.\n"
    );
    // Not uses lambda + restore
    EXPECT_NE(code.find("[&]() -> bool"), std::string::npos);
    EXPECT_NE(code.find("restore("), std::string::npos);
    // Not checks: if body succeeded, !e fails
    EXPECT_NE(code.find("if (_ok"), std::string::npos);
}

TEST(Codegen, PlusEmission) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = { \"x\" }+ .\n"
        "END Test.\n"
    );
    // Plus = first occurrence (no lambda) + star loop (with lambda)
    EXPECT_NE(code.find("for (;;)"), std::string::npos);
    // First occurrence uses keyword directly
    EXPECT_NE(code.find("keyword(\"x\")"), std::string::npos);
}

TEST(Codegen, ChoiceKeywordHeadFail) {
    // Keyword-like literals use peek_keyword for head-fail
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"struct\" | \"union\" | \"enum\" .\n"
        "END Test.\n"
    );
    EXPECT_NE(code.find("peek_keyword(\"struct\")"), std::string::npos);
    EXPECT_NE(code.find("peek_keyword(\"union\")"), std::string::npos);
    // Last alternative has no peek guard (else branch)
    EXPECT_EQ(count_occurrences(code, "peek_keyword"), 2);
}
