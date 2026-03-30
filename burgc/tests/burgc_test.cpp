#include <gtest/gtest.h>
#include <sstream>
#include "generator.h"
#include "Parser.h"

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

static std::string source_path(const char* relative) {
    return std::string(SOURCE_DIR) + "/" + relative;
}

// ── Parser tests ──────────────────────────────────────────

TEST(BurgParser, ParsesSimpleSpec) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->terminals.size(), 4u);
    EXPECT_EQ(spec->start, "reg");
    EXPECT_EQ(spec->rules.size(), 4u);
}

TEST(BurgParser, ParsesChainRules) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/chain.burg"));
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->terminals.size(), 3u);
    EXPECT_EQ(spec->start, "stmt");
    EXPECT_EQ(spec->rules.size(), 5u);
}

TEST(BurgParser, ParsesExplicitCosts) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/costs.burg"));
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->rules.size(), 3u);
    // First rule: Const with cost 1
    EXPECT_EQ(spec->rules[0]->cost, 1);
    // Second rule: Add with cost 2
    EXPECT_EQ(spec->rules[1]->cost, 2);
}

TEST(BurgParser, TerminalDeclarations) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->terminals[0]->name, "Const");
    EXPECT_EQ(spec->terminals[0]->number, 1);
    EXPECT_EQ(spec->terminals[1]->name, "Add");
    EXPECT_EQ(spec->terminals[1]->number, 2);
}

TEST(BurgParser, TreePatternStructure) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    // Rule 0: reg : Const = 1  (leaf pattern)
    EXPECT_TRUE(spec->rules[0]->pattern->is_leaf());
    EXPECT_EQ(spec->rules[0]->pattern->name, "Const");
    // Rule 1: reg : Add(reg, reg) = 2  (binary pattern)
    EXPECT_FALSE(spec->rules[1]->pattern->is_leaf());
    EXPECT_EQ(spec->rules[1]->pattern->name, "Add");
    EXPECT_EQ(spec->rules[1]->pattern->children.size(), 2u);
    EXPECT_EQ(spec->rules[1]->pattern->children[0]->name, "reg");
    EXPECT_EQ(spec->rules[1]->pattern->children[1]->name, "reg");
}

TEST(BurgParser, DefaultCostIsZero) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    for (auto* rule : spec->rules) {
        EXPECT_EQ(rule->cost, 0);
    }
}

TEST(BurgParser, RejectsInvalidInput) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/nonexistent.burg"));
    EXPECT_EQ(spec, nullptr);
    EXPECT_FALSE(gen.errors().empty());
}

TEST(BurgParser, InlineParseSimple) {
    Parser parser;
    const char* input = "TERM X=1 RULES r: X = 1;";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    EXPECT_EQ(parser.ast->terminals.size(), 1u);
    EXPECT_EQ(parser.ast->rules.size(), 1u);
}

TEST(BurgParser, InlineParseRejectsIncomplete) {
    Parser parser;
    const char* input = "TERM X=1";  // missing RULES section
    parser.init(input, (int)strlen(input));
    EXPECT_FALSE(parser.parse());
}

// ── Analysis tests ────────────────────────────────────────

TEST(BurgAnalysis, CollectsTerminals) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    EXPECT_TRUE(gen.analyze(spec));
    EXPECT_TRUE(gen.errors().empty());
}

TEST(BurgAnalysis, DetectsChainRules) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/chain.burg"));
    ASSERT_NE(spec, nullptr);
    EXPECT_TRUE(gen.analyze(spec));
    EXPECT_TRUE(gen.errors().empty());
}

TEST(BurgAnalysis, RejectsDuplicateTerminals) {
    Parser parser;
    const char* input = "TERM X=1 X=2 RULES r: X = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());

    BurgGenerator gen;
    // Parse through inline AST
    EXPECT_FALSE(gen.analyze(parser.ast));
    EXPECT_FALSE(gen.errors().empty());
}

TEST(BurgAnalysis, RejectsUndeclaredTerminalInPattern) {
    Parser parser;
    const char* input = "TERM X=1 RULES r: Y(r) = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());

    BurgGenerator gen;
    EXPECT_FALSE(gen.analyze(parser.ast));
}

// ── Code generation tests ─────────────────────────────────

TEST(BurgCodegen, GeneratesOutput) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_FALSE(code.empty());
    // Header guard
    EXPECT_NE(code.find("#pragma once"), std::string::npos);
}

TEST(BurgCodegen, EmitsTerminalConstants) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("BURG_Const"), std::string::npos);
    EXPECT_NE(code.find("BURG_Add"), std::string::npos);
    EXPECT_NE(code.find("BURG_Mul"), std::string::npos);
    EXPECT_NE(code.find("BURG_Load"), std::string::npos);
}

TEST(BurgCodegen, EmitsNonterminalConstants) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("reg_NT"), std::string::npos);
    EXPECT_NE(code.find("BURG_MAX_NT"), std::string::npos);
}

TEST(BurgCodegen, EmitsStateStruct) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("struct BurgState"), std::string::npos);
    EXPECT_NE(code.find("int op;"), std::string::npos);
    EXPECT_NE(code.find("BurgState** children;"), std::string::npos);
    EXPECT_NE(code.find("int child_count;"), std::string::npos);
}

TEST(BurgCodegen, EmitsStateFunction) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("burg_state"), std::string::npos);
    EXPECT_NE(code.find("switch (op)"), std::string::npos);
    EXPECT_NE(code.find("case BURG_Const:"), std::string::npos);
    EXPECT_NE(code.find("case BURG_Add:"), std::string::npos);
}

TEST(BurgCodegen, EmitsClosuresForChainRules) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/chain.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // Chain rules: reg→con and stmt→reg should produce closures
    EXPECT_NE(code.find("closure_"), std::string::npos);
}

TEST(BurgCodegen, EmitsRuleLookup) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("burg_rule"), std::string::npos);
    EXPECT_NE(code.find("burg_label"), std::string::npos);
    EXPECT_NE(code.find("burg_nt_name"), std::string::npos);
}

TEST(BurgCodegen, NontermNameTable) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/chain.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("\"stmt\""), std::string::npos);
    EXPECT_NE(code.find("\"reg\""), std::string::npos);
    EXPECT_NE(code.find("\"con\""), std::string::npos);
}

// ── Integration: generated code compiles ──────────────────

TEST(BurgCodegen, GeneratedCodeHasCorrectStructure) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/costs.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // Costs should appear in the generated code
    EXPECT_NE(code.find("+ 1"), std::string::npos);  // cost 1
    EXPECT_NE(code.find("+ 2"), std::string::npos);  // cost 2

    // Unary operator: Mov has one child
    EXPECT_NE(code.find("case BURG_Mov:"), std::string::npos);
}

// ── Semantic action tests ─────────────────────────────────

TEST(BurgParser, ParsesActions) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->rules.size(), 5u);
    // Rule 0 has action
    EXPECT_FALSE(spec->rules[0]->action.empty());
    EXPECT_NE(spec->rules[0]->action.find("emit_const"), std::string::npos);
    // Rule 4 (stmt : reg) has no action
    EXPECT_TRUE(spec->rules[4]->action.empty());
}

TEST(BurgParser, ParsesHeaderBlocks) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->headers.size(), 1u);
    EXPECT_NE(spec->headers[0].find("emitter.h"), std::string::npos);
}

TEST(BurgParser, ActionsOptional) {
    // Existing specs without actions should still parse fine
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    for (auto* rule : spec->rules) {
        EXPECT_TRUE(rule->action.empty());
    }
    EXPECT_TRUE(spec->headers.empty());
}

TEST(BurgParser, InlineParseAction) {
    Parser parser;
    const char* input = "TERM X=1 RULES r: X = 1 (. do_thing(); .);";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    ASSERT_EQ(parser.ast->rules.size(), 1u);
    EXPECT_NE(parser.ast->rules[0]->action.find("do_thing"), std::string::npos);
}

TEST(BurgParser, InlineParseActionWithCost) {
    Parser parser;
    const char* input = "TERM X=1 RULES r: X = 1 (3) (. do_thing(); .);";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    ASSERT_EQ(parser.ast->rules.size(), 1u);
    EXPECT_EQ(parser.ast->rules[0]->cost, 3);
    EXPECT_NE(parser.ast->rules[0]->action.find("do_thing"), std::string::npos);
}

TEST(BurgCodegen, EmitsReducerForActions) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("burg_reduce"), std::string::npos);
    EXPECT_NE(code.find("emit_const(node)"), std::string::npos);
    EXPECT_NE(code.find("emit_add(node)"), std::string::npos);
    EXPECT_NE(code.find("emit_load(node)"), std::string::npos);
}

TEST(BurgCodegen, ReducerHasChildReductions) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // Add(reg, reg) should reduce indexed children
    EXPECT_NE(code.find("BURG_NODE_CHILD(node, 0)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_CHILD(node, 1)"), std::string::npos);
    EXPECT_NE(code.find("state->children[0]"), std::string::npos);
    EXPECT_NE(code.find("state->children[1]"), std::string::npos);
}

TEST(BurgCodegen, NoReducerWithoutActions) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // No actions → no reducer generated
    EXPECT_EQ(code.find("burg_reduce"), std::string::npos);
}

TEST(BurgCodegen, EmitsUserHeaders) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("#include \"emitter.h\""), std::string::npos);
}

TEST(BurgCodegen, StartSymbolIsFirstNonterminal) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/chain.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // START stmt — should be nonterminal index 1
    EXPECT_NE(code.find("stmt_NT = 1"), std::string::npos);
}

// ── Graph-aware codegen tests ─────────────────────────────

TEST(BurgCodegen, EmitsStateCache) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("burg_state_cache"), std::string::npos);
    EXPECT_NE(code.find("burg_cache_lookup"), std::string::npos);
    EXPECT_NE(code.find("burg_cache_store"), std::string::npos);
}

TEST(BurgCodegen, EmitsMacroChecks) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("BURG_NODE_TYPE"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_OP"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_CHILD"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_ID"), std::string::npos);
}

TEST(BurgCodegen, LabelFuncUsesMacros) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("burg_label(BURG_NODE_TYPE node)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_OP(node)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_ARITY(node)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_CHILD(node, i)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_ID(node)"), std::string::npos);
}

TEST(BurgCodegen, LabelCachesBeforeDP) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // Cache store must appear BEFORE the switch (DP)
    auto cache_pos = code.find("burg_cache_store(id, p)");
    auto switch_pos = code.find("switch (op)");
    ASSERT_NE(cache_pos, std::string::npos);
    ASSERT_NE(switch_pos, std::string::npos);
    EXPECT_LT(cache_pos, switch_pos);
}

TEST(BurgCodegen, IndexedChildrenInDP) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // Add(reg, reg) should use indexed children in DP
    EXPECT_NE(code.find("p->children[0]->rule["), std::string::npos);
    EXPECT_NE(code.find("p->children[1]->rule["), std::string::npos);
    EXPECT_NE(code.find("p->child_count >= 2"), std::string::npos);
}

TEST(BurgCodegen, ReducerTakesNodeAndState) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("burg_reduce(BURG_NODE_TYPE node, BurgState* state, int goalnt)"),
              std::string::npos);
}

// ── Where guard tests ─────────────────────────────────────

TEST(BurgParser, ParsesWhereGuard) {
    Parser parser;
    const char* input = "TERM X=1 RULES r: X where (. is_pure(node) .) = 1;";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    ASSERT_EQ(parser.ast->rules.size(), 1u);
    EXPECT_NE(parser.ast->rules[0]->guard.find("is_pure"), std::string::npos);
}

TEST(BurgParser, ParsesWhereGuardWithCostAndAction) {
    Parser parser;
    const char* input = "TERM X=1 RULES r: X where (. pred(n) .) = 1 (2) (. action(); .);";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    ASSERT_EQ(parser.ast->rules.size(), 1u);
    EXPECT_NE(parser.ast->rules[0]->guard.find("pred"), std::string::npos);
    EXPECT_EQ(parser.ast->rules[0]->cost, 2);
    EXPECT_NE(parser.ast->rules[0]->action.find("action"), std::string::npos);
}

TEST(BurgParser, WhereGuardOptional) {
    Parser parser;
    const char* input = "TERM X=1 RULES r: X = 1;";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    ASSERT_EQ(parser.ast->rules.size(), 1u);
    EXPECT_TRUE(parser.ast->rules[0]->guard.empty());
}

TEST(BurgCodegen, EmitsGuardPredicate) {
    Parser parser;
    const char* input =
        "TERM X=1 TERM Y=2 START r RULES\n"
        "r: X where (. is_pure(node) .) = 1 (. action(); .);";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());

    BurgGenerator gen;
    ASSERT_TRUE(gen.analyze(parser.ast));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // Guard should appear in the label function
    EXPECT_NE(code.find("is_pure(node)"), std::string::npos);
}

// ── Graph rewriter tests ──────────────────────────────────

TEST(BurgCodegen, EmitsRewriteFunc) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    EXPECT_NE(code.find("burg_rewrite(BURG_NODE_TYPE root)"), std::string::npos);
    // RPO walk uses successor macros
    EXPECT_NE(code.find("BURG_NODE_SUCC_COUNT"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_SUCC("), std::string::npos);
    // Labels all nodes
    EXPECT_NE(code.find("burg_label(n)"), std::string::npos);
}

TEST(BurgCodegen, RewriteFuncReducesWhenActions) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // When actions exist, rewrite calls burg_reduce
    EXPECT_NE(code.find("burg_reduce(n, s,"), std::string::npos);
}

TEST(BurgCodegen, RewriteFuncClearsCache) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));

    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();

    // Should clear cache at start
    auto clear_pos = code.find("burg_cache_clear()");
    auto rpo_pos = code.find("rpo");
    ASSERT_NE(clear_pos, std::string::npos);
    ASSERT_NE(rpo_pos, std::string::npos);
    EXPECT_LT(clear_pos, rpo_pos);
}
