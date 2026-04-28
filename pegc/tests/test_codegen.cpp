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

TEST(Codegen, BareIdentLiteralEmitsMatch) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"struct\" .\n"
        "END Test.\n"
    );
    EXPECT_NE(code.find("match(\"struct\")"), std::string::npos);
    EXPECT_EQ(code.find("keyword("), std::string::npos);
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

TEST(Codegen, CharsetDefsEmitted) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "CHARACTERS\n"
        "  letter = 'a'..'z' + 'A'..'Z' + '_' .\n"
        "  digit  = '0'..'9' .\n"
        "PRODUCTIONS\n"
        "Test = \"x\" .\n"
        "END Test.\n"
    );
    // Each CharsetDef emits a static predicate function
    EXPECT_NE(code.find("static bool is_letter(char c)"), std::string::npos);
    EXPECT_NE(code.find("static bool is_digit(char c)"), std::string::npos);
    // Body uses the charset predicate inlined
    EXPECT_NE(code.find("c >= 97 && c <= 122"), std::string::npos);  // 'a'..'z'
    EXPECT_NE(code.find("c >= 48 && c <= 57"), std::string::npos);   // '0'..'9'
}

TEST(Codegen, TokenWithCharRangeAndPlus) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "TOKENS\n"
        "  digits = '0'..'9' { '0'..'9' } .\n"
        "PRODUCTIONS\n"
        "Test = digits<x> .\n"
        "END Test.\n"
    );
    // Token body emits CharRange checks (peek() comparison)
    EXPECT_NE(code.find("peek() < 48 || peek() > 57"), std::string::npos);
    // Token body has no skip() — count
    auto token_pos = code.find("bool Parser::digits");
    ASSERT_NE(token_pos, std::string::npos);
    auto token_end = code.find("\n}\n", token_pos);
    ASSERT_NE(token_end, std::string::npos);
    auto token_body = code.substr(token_pos, token_end - token_pos);
    EXPECT_EQ(token_body.find("skip()"), std::string::npos);
}

TEST(Codegen, TokenEmittedAsLexMode) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "CHARACTERS\n"
        "  letter = 'a'..'z' + 'A'..'Z' + '_' .\n"
        "  digit  = '0'..'9' .\n"
        "TOKENS\n"
        "  ident = letter { letter | digit } .\n"
        "PRODUCTIONS\n"
        "Test = ident<x> .\n"
        "END Test.\n"
    );
    // Token method emitted with span signature
    EXPECT_NE(code.find("bool Parser::ident(peg::Span& out)"), std::string::npos);
    // Token captures from start position
    EXPECT_NE(code.find("const char* _start = pos();"), std::string::npos);
    EXPECT_NE(code.find("out.ptr = _start"), std::string::npos);
    // Production calling ident emits skip() before call (production-mode)
    auto prod_pos = code.find("bool Parser::parse_Test()");
    ASSERT_NE(prod_pos, std::string::npos);
    auto skip_in_prod = code.find("skip()", prod_pos);
    auto ident_call = code.find("ident(x)", prod_pos);
    EXPECT_LT(skip_in_prod, ident_call);
    // Token body itself does NOT emit skip() — count skip() before vs inside
    auto token_pos = code.find("bool Parser::ident(peg::Span& out)");
    ASSERT_NE(token_pos, std::string::npos);
    auto token_end = code.find("\n}\n", token_pos);
    ASSERT_NE(token_end, std::string::npos);
    auto token_body = code.substr(token_pos, token_end - token_pos);
    EXPECT_EQ(token_body.find("skip()"), std::string::npos)
        << "Token body should not contain skip() calls";
}

TEST(Codegen, IgnoreReferencesNamedCharset) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "CHARACTERS\n"
        "  ws = ' ' + '\\t' + '\\n' .\n"
        "IGNORE ws\n"
        "PRODUCTIONS\n"
        "Test = \"x\" .\n"
        "END Test.\n"
    );
    // Charset predicate is generated
    EXPECT_NE(code.find("static bool is_ws(char c)"), std::string::npos);
    // IGNORE clause's lambda body calls is_ws(c), not inline predicate
    EXPECT_NE(code.find("is_ws(c)"), std::string::npos);
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
    EXPECT_NE(code.find("for (;;)"), std::string::npos);
    EXPECT_NE(code.find("match(\"x\")"), std::string::npos);
}

TEST(Codegen, ChoiceKeywordUsesGeneralChoice) {
    auto code = generate_impl(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"struct\" | \"union\" | \"enum\" .\n"
        "END Test.\n"
    );
    EXPECT_EQ(code.find("peek_keyword"), std::string::npos);
    EXPECT_EQ(code.find("peek_at(\"struct\")"), std::string::npos);
    EXPECT_NE(code.find("save()"), std::string::npos);
}
