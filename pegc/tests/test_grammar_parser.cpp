#include <gtest/gtest.h>
#include <string>

#include "Parser.h"

using namespace PegGrammar;

// Helper: parse a .peg grammar string, return Grammar AST or nullptr on error
static Grammar* parse(const char* src) {
    Parser parser;
    parser.init(src, static_cast<int>(strlen(src)));
    if (!parser.parse()) return nullptr;
    return parser.ast;
}

// ═══════════════════════════════════════════════════════════════
// Minimal Grammar Tests
// ═══════════════════════════════════════════════════════════════

TEST(GrammarParse, MinimalGrammar) {
    auto* g = parse(
        "COMPILER Foo\n"
        "PRODUCTIONS\n"
        "Foo = \"hello\" .\n"
        "END Foo.\n"
    );
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->name, "Foo");
    EXPECT_EQ(g->productions.size(), 1u);
    EXPECT_EQ(g->productions[0]->name, "Foo");
}

TEST(GrammarParse, EmptyGrammarNoProductions) {
    auto* g = parse(
        "COMPILER Empty\n"
        "PRODUCTIONS\n"
        "END Empty.\n"
    );
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->name, "Empty");
    EXPECT_EQ(g->productions.size(), 0u);
}

// ═══════════════════════════════════════════════════════════════
// CHARACTERS Section
// ═══════════════════════════════════════════════════════════════

TEST(GrammarParse, CharacterSets) {
    auto* g = parse(
        "COMPILER Test\n"
        "CHARACTERS\n"
        "  letter = 'a'..'z' + 'A'..'Z' + '_' .\n"
        "  digit  = '0'..'9' .\n"
        "  strchar = ANY - '\"' - '\\r' - '\\n' .\n"
        "PRODUCTIONS\n"
        "Test = \"x\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->charsets.size(), 3u);
    EXPECT_EQ(g->charsets[0]->name, "letter");
    EXPECT_EQ(g->charsets[1]->name, "digit");
    EXPECT_EQ(g->charsets[2]->name, "strchar");

    // letter = union of ranges and single
    auto* body0 = g->charsets[0]->body;
    ASSERT_NE(body0, nullptr);
    auto* u = dynamic_cast<Union*>(body0);
    ASSERT_NE(u, nullptr);  // top level is union
}

// ═══════════════════════════════════════════════════════════════
// TOKENS Section
// ═══════════════════════════════════════════════════════════════

TEST(GrammarParse, TokenDefinitions) {
    auto* g = parse(
        "COMPILER Test\n"
        "CHARACTERS\n"
        "  letter = 'a'..'z' + 'A'..'Z' + '_' .\n"
        "  digit  = '0'..'9' .\n"
        "TOKENS\n"
        "  ident = letter { letter | digit } .\n"
        "  number = digit { digit } .\n"
        "PRODUCTIONS\n"
        "Test = \"x\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->tokens.size(), 2u);
    EXPECT_EQ(g->tokens[0]->name, "ident");
    EXPECT_EQ(g->tokens[1]->name, "number");
}

// ═══════════════════════════════════════════════════════════════
// COMMENTS Section
// ═══════════════════════════════════════════════════════════════

TEST(GrammarParse, CommentDefinitions) {
    auto* g = parse(
        "COMPILER Test\n"
        "COMMENTS FROM \"//\" TO \"\\n\"\n"
        "COMMENTS FROM \"/*\" TO \"*/\"\n"
        "PRODUCTIONS\n"
        "Test = \"x\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->comments.size(), 2u);
    EXPECT_EQ(g->comments[0]->open, "//");
    EXPECT_EQ(g->comments[0]->close, "\n");
    EXPECT_EQ(g->comments[1]->open, "/*");
    EXPECT_EQ(g->comments[1]->close, "*/");
    EXPECT_FALSE(g->comments[0]->nested);
}

// ═══════════════════════════════════════════════════════════════
// PEG Expressions
// ═══════════════════════════════════════════════════════════════

TEST(GrammarParse, LiteralExpression) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"hello\" \"world\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(g->productions.size(), 1u);
    auto* body = g->productions[0]->body;
    auto* seq = dynamic_cast<Sequence*>(body);
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(seq->elements.size(), 2u);
    auto* lit1 = dynamic_cast<Literal*>(seq->elements[0]);
    auto* lit2 = dynamic_cast<Literal*>(seq->elements[1]);
    ASSERT_NE(lit1, nullptr);
    ASSERT_NE(lit2, nullptr);
    EXPECT_EQ(lit1->value, "hello");
    EXPECT_EQ(lit2->value, "world");
}

TEST(GrammarParse, ChoiceExpression) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"a\" | \"b\" | \"c\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    auto* body = g->productions[0]->body;
    auto* choice = dynamic_cast<Choice*>(body);
    ASSERT_NE(choice, nullptr);
    EXPECT_EQ(choice->alternatives.size(), 3u);
}

TEST(GrammarParse, StarOptionalGrouping) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = { \"a\" } [ \"b\" ] ( \"c\" | \"d\" ) .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    auto* body = g->productions[0]->body;
    auto* seq = dynamic_cast<Sequence*>(body);
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(seq->elements.size(), 3u);

    auto* star = dynamic_cast<Star*>(seq->elements[0]);
    ASSERT_NE(star, nullptr);

    auto* opt = dynamic_cast<Optional*>(seq->elements[1]);
    ASSERT_NE(opt, nullptr);

    auto* group = dynamic_cast<Group*>(seq->elements[2]);
    ASSERT_NE(group, nullptr);
}

TEST(GrammarParse, RuleCallNoArgs) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = Foo Bar .\n"
        "Foo = \"x\" .\n"
        "Bar = \"y\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->productions.size(), 3u);
    auto* body = g->productions[0]->body;
    auto* seq = dynamic_cast<Sequence*>(body);
    ASSERT_NE(seq, nullptr);
    auto* call1 = dynamic_cast<RuleCall*>(seq->elements[0]);
    auto* call2 = dynamic_cast<RuleCall*>(seq->elements[1]);
    ASSERT_NE(call1, nullptr);
    ASSERT_NE(call2, nullptr);
    EXPECT_EQ(call1->name, "Foo");
    EXPECT_EQ(call2->name, "Bar");
    EXPECT_TRUE(call1->args.empty());
}

TEST(GrammarParse, RuleCallWithArgs) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = Foo<result> .\n"
        "Foo = \"x\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    auto* body = g->productions[0]->body;
    auto* call = dynamic_cast<RuleCall*>(body);
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->name, "Foo");
    ASSERT_EQ(call->args.size(), 1u);
    EXPECT_EQ(call->args[0]->expr, "result");
}

// ═══════════════════════════════════════════════════════════════
// Semantic Actions
// ═══════════════════════════════════════════════════════════════

TEST(GrammarParse, SemanticAction) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"hello\" (. code here .) .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    auto* body = g->productions[0]->body;
    auto* seq = dynamic_cast<Sequence*>(body);
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(seq->elements.size(), 2u);
    auto* act = dynamic_cast<Action*>(seq->elements[1]);
    ASSERT_NE(act, nullptr);
    EXPECT_EQ(act->code, "code here");
}

// ═══════════════════════════════════════════════════════════════
// Parameters
// ═══════════════════════════════════════════════════════════════

TEST(GrammarParse, FormalParams) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Foo <. int &result .> = \"x\" .\n"
        "Test = \"y\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(g->productions.size(), 2u);
    auto* foo = g->productions[0];
    EXPECT_EQ(foo->name, "Foo");
    ASSERT_EQ(foo->params.size(), 1u);
    EXPECT_EQ(foo->params[0]->type_code, "int");
    EXPECT_EQ(foo->params[0]->name, "result");
    EXPECT_TRUE(foo->params[0]->is_ref);
}

TEST(GrammarParse, LocalVars) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test (. int x = 0; .) = \"y\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    auto* p = g->productions[0];
    ASSERT_TRUE(p->locals_code.has_value());
    // The locals code should contain "int x = 0;"
    EXPECT_NE(p->locals_code->find("int"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// Header Code
// ═══════════════════════════════════════════════════════════════

TEST(GrammarParse, HeaderCode) {
    auto* g = parse(
        "COMPILER Test\n"
        "(. #include <vector> .)\n"
        "PRODUCTIONS\n"
        "Test = \"x\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(g->headers.size(), 1u);
    EXPECT_NE(g->headers[0]->code.find("include"), std::string::npos);
}
