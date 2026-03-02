#include <gtest/gtest.h>
#include <string>
#include <cstring>

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

// ═══════════════════════════════════════════════════════════════
// Left Recursion Detection
// ═══════════════════════════════════════════════════════════════

TEST(PegAnalysis, DirectLeftRecursion) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Expr = Expr \"+\" Term | Term .\n"
        "Term = \"x\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors[0].find("left recursion"), std::string::npos);
    EXPECT_NE(result.errors[0].find("Expr"), std::string::npos);
}

TEST(PegAnalysis, IndirectLeftRecursion) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "A = B \"x\" .\n"
        "B = A \"y\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    EXPECT_FALSE(result.ok());
    // Should detect left recursion in at least one of A or B
    bool found = false;
    for (auto& e : result.errors) {
        if (e.find("left recursion") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(PegAnalysis, NoLeftRecursion) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Expr = Term { \"+\" Term } .\n"
        "Term = Factor { \"*\" Factor } .\n"
        "Factor = \"x\" | \"(\" Expr \")\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    EXPECT_TRUE(result.ok());
}

// ═══════════════════════════════════════════════════════════════
// FIRST Set Computation
// ═══════════════════════════════════════════════════════════════

TEST(PegAnalysis, FirstSetLiteral) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"hello\" \"world\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    EXPECT_TRUE(result.ok());

    auto& info = result.productions["Test"];
    ASSERT_FALSE(info.first_set.empty());
    EXPECT_EQ(info.first_set[0].kind, FirstElement::Literal);
    EXPECT_EQ(info.first_set[0].value, "hello");
    EXPECT_TRUE(info.first_testable);
}

TEST(PegAnalysis, FirstSetChoice) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"a\" | \"b\" | \"c\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    EXPECT_TRUE(result.ok());

    auto& info = result.productions["Test"];
    EXPECT_EQ(info.first_set.size(), 3u);
}

// ═══════════════════════════════════════════════════════════════
// Topological Sort
// ═══════════════════════════════════════════════════════════════

TEST(PegAnalysis, TopologicalOrder) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Expr = Term { \"+\" Term } .\n"
        "Term = Factor { \"*\" Factor } .\n"
        "Factor = \"x\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    EXPECT_TRUE(result.ok());

    // Factor should come before Term, Term before Expr
    auto pos = [&](const std::string& name) -> int {
        for (int i = 0; i < (int)result.topo_order.size(); i++)
            if (result.topo_order[i] == name) return i;
        return -1;
    };
    EXPECT_LT(pos("Factor"), pos("Term"));
    EXPECT_LT(pos("Term"), pos("Expr"));
}

// ═══════════════════════════════════════════════════════════════
// Unreachable Production Detection
// ═══════════════════════════════════════════════════════════════

TEST(PegAnalysis, UnreachableProduction) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = \"hello\" .\n"
        "Dead = \"unreachable\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    EXPECT_TRUE(result.ok());  // warnings don't count as errors
    EXPECT_EQ(result.warnings.size(), 1u);
    EXPECT_NE(result.warnings[0].find("Dead"), std::string::npos);
}

TEST(PegAnalysis, AllReachable) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Test = Foo .\n"
        "Foo = \"x\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.warnings.empty());
}

// ═══════════════════════════════════════════════════════════════
// Duplicate Production Detection
// ═══════════════════════════════════════════════════════════════

TEST(PegAnalysis, DuplicateProduction) {
    auto* g = parse(
        "COMPILER Test\n"
        "PRODUCTIONS\n"
        "Foo = \"x\" .\n"
        "Foo = \"y\" .\n"
        "END Test.\n"
    );
    ASSERT_NE(g, nullptr);

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.errors[0].find("duplicate"), std::string::npos);
}
