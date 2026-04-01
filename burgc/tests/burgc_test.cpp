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

// Helper: parse + analyze + generate with a specific backend
static std::string gen_code(const char* input, BurgBackend& backend) {
    Parser parser;
    parser.init(input, (int)strlen(input));
    if (!parser.parse() || !parser.ast) return "";
    BurgGenerator gen;
    if (!gen.analyze(parser.ast)) return "";
    std::ostringstream out;
    backend.generate(out, gen.analysis());
    return out.str();
}

// Helper: parse + analyze + generate with default (C++) backend
static std::string gen_cpp(const char* input) {
    auto backend = create_cpp_backend();
    return gen_code(input, *backend);
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
    EXPECT_EQ(spec->rules[0]->cost, 1);
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
    EXPECT_TRUE(spec->rules[0]->pattern->is_leaf());
    EXPECT_EQ(spec->rules[0]->pattern->name, "Const");
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
    for (auto* rule : spec->rules)
        EXPECT_EQ(rule->cost, 0);
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
    const char* input = "TERM X=1";
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

// ── C++ backend codegen tests ─────────────────────────────

TEST(BurgCppBackend, GeneratesOutput) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    EXPECT_FALSE(code.empty());
    EXPECT_NE(code.find("#pragma once"), std::string::npos);
}

TEST(BurgCppBackend, EmitsTerminalConstants) {
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

TEST(BurgCppBackend, EmitsNonterminalConstants) {
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

TEST(BurgCppBackend, EmitsClassWrapper) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    EXPECT_NE(code.find("class BurgMatcher {"), std::string::npos);
    EXPECT_NE(code.find("}; // class BurgMatcher"), std::string::npos);
}

TEST(BurgCppBackend, EmitsStateStruct) {
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
}

TEST(BurgCppBackend, EmitsClosuresForChainRules) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/chain.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    EXPECT_NE(out.str().find("closure_"), std::string::npos);
}

TEST(BurgCppBackend, EmitsRuleLookup) {
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

TEST(BurgCppBackend, NontermNameTable) {
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

TEST(BurgCppBackend, CostsInGeneratedCode) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/costs.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    EXPECT_NE(code.find("+ 1"), std::string::npos);
    EXPECT_NE(code.find("+ 2"), std::string::npos);
    EXPECT_NE(code.find("case BURG_Mov:"), std::string::npos);
}

// ── Semantic action tests ─────────────────────────────────

TEST(BurgParser, ParsesActions) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->rules.size(), 5u);
    EXPECT_FALSE(spec->rules[0]->action.empty());
    EXPECT_NE(spec->rules[0]->action.find("emit_const"), std::string::npos);
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
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    for (auto* rule : spec->rules)
        EXPECT_TRUE(rule->action.empty());
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

TEST(BurgCppBackend, EmitsReducerForActions) {
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

TEST(BurgCppBackend, ReducerHasChildReductions) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    EXPECT_NE(code.find("BURG_NODE_CHILD(node, 0)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_CHILD(node, 1)"), std::string::npos);
    EXPECT_NE(code.find("state->children[0]"), std::string::npos);
    EXPECT_NE(code.find("state->children[1]"), std::string::npos);
}

TEST(BurgCppBackend, NoReducerWithoutActions) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    EXPECT_EQ(out.str().find("burg_reduce"), std::string::npos);
}

TEST(BurgCppBackend, EmitsUserHeaders) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    EXPECT_NE(out.str().find("#include \"emitter.h\""), std::string::npos);
}

TEST(BurgCppBackend, StartSymbolIsFirstNonterminal) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/chain.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    EXPECT_NE(out.str().find("stmt_NT = 1"), std::string::npos);
}

// ── Graph-aware codegen tests ─────────────────────────────

TEST(BurgCppBackend, EmitsStateCache) {
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

TEST(BurgCppBackend, EmitsMacroChecks) {
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

TEST(BurgCppBackend, LabelFuncIsMethod) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    EXPECT_NE(code.find("BurgState* burg_label(BURG_NODE_TYPE node)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_OP(node)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_ARITY(node)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_CHILD(node, i)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_ID(node)"), std::string::npos);
}

TEST(BurgCppBackend, LabelCachesBeforeDP) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    auto cache_pos = code.find("burg_cache_store(id, p)");
    auto switch_pos = code.find("switch (op)");
    ASSERT_NE(cache_pos, std::string::npos);
    ASSERT_NE(switch_pos, std::string::npos);
    EXPECT_LT(cache_pos, switch_pos);
}

TEST(BurgCppBackend, IndexedChildrenInDP) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    EXPECT_NE(code.find("p->children[0]->rule["), std::string::npos);
    EXPECT_NE(code.find("p->children[1]->rule["), std::string::npos);
    EXPECT_NE(code.find("p->child_count >= 2"), std::string::npos);
}

TEST(BurgCppBackend, ReducerIsMethod) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    EXPECT_NE(out.str().find("void burg_reduce(BURG_NODE_TYPE node, BurgState* state, int goalnt)"),
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

TEST(BurgCppBackend, EmitsGuardPredicate) {
    std::string code = gen_cpp(
        "TERM X=1 TERM Y=2 START r RULES\n"
        "r: X where (. is_pure(node) .) = 1 (. action(); .);");
    EXPECT_NE(code.find("is_pure(node)"), std::string::npos);
}

// ── Graph rewriter tests ──────────────────────────────────

TEST(BurgCppBackend, EmitsRewriteFunc) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    EXPECT_NE(code.find("void burg_rewrite(BURG_NODE_TYPE root)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_SUCC_COUNT"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_SUCC("), std::string::npos);
    EXPECT_NE(code.find("burg_label(n)"), std::string::npos);
}

TEST(BurgCppBackend, RewriteFuncReducesWhenActions) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    EXPECT_NE(out.str().find("burg_reduce(n, s,"), std::string::npos);
}

TEST(BurgCppBackend, RewriteFuncClearsCache) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    auto clear_pos = code.find("burg_cache_clear()");
    auto rpo_pos = code.find("rpo");
    ASSERT_NE(clear_pos, std::string::npos);
    ASSERT_NE(rpo_pos, std::string::npos);
    EXPECT_LT(clear_pos, rpo_pos);
}

// ── MEMBERS tests ─────────────────────────────────────────

TEST(BurgParser, ParsesMembersBlock) {
    Parser parser;
    const char* input =
        "TERM X=1\n"
        "MEMBERS (. int count_ = 0;\n"
        "    Heap* heap_ = nullptr; .)\n"
        "RULES r: X = 1;";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    EXPECT_NE(parser.ast->members.find("count_"), std::string::npos);
    EXPECT_NE(parser.ast->members.find("heap_"), std::string::npos);
}

TEST(BurgCppBackend, EmitsMembersInClass) {
    std::string code = gen_cpp(
        "TERM X=1\n"
        "MEMBERS (. int count_ = 0; .)\n"
        "RULES r: X = 1;");
    EXPECT_NE(code.find("class BurgMatcher {"), std::string::npos);
    EXPECT_NE(code.find("int count_ = 0;"), std::string::npos);
    auto class_pos = code.find("class BurgMatcher {");
    auto member_pos = code.find("int count_ = 0;");
    auto close_pos = code.find("}; // class BurgMatcher");
    EXPECT_LT(class_pos, member_pos);
    EXPECT_LT(member_pos, close_pos);
}

TEST(BurgCppBackend, NoMembersBlockStillWorks) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out);
    std::string code = out.str();
    EXPECT_NE(code.find("class BurgMatcher {"), std::string::npos);
    EXPECT_NE(code.find("}; // class BurgMatcher"), std::string::npos);
}

// ── C backend tests ───────────────────────────────────────

TEST(BurgCBackend, EmitsContextStruct) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1\n"
        "MEMBERS (. int count_ = 0; .)\n"
        "RULES r: X = 1;", *backend);
    EXPECT_NE(code.find("struct BurgContext"), std::string::npos);
    EXPECT_NE(code.find("int count_ = 0;"), std::string::npos);
    EXPECT_NE(code.find("burg_state_cache"), std::string::npos);
    // No class wrapper
    EXPECT_EQ(code.find("class BurgMatcher"), std::string::npos);
}

TEST(BurgCBackend, StaticFunctions) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1 RULES r: X = 1 (. action(); .);", *backend);
    EXPECT_NE(code.find("static BurgState* burg_label(BURG_NODE_TYPE node, BurgContext* ctx)"),
              std::string::npos);
    EXPECT_NE(code.find("static void burg_reduce(BURG_NODE_TYPE node, BurgState* state, int goalnt, BurgContext* ctx)"),
              std::string::npos);
    EXPECT_NE(code.find("static void burg_rewrite(BURG_NODE_TYPE root, BurgContext* ctx)"),
              std::string::npos);
}

TEST(BurgCBackend, CtxThreadedInCalls) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1 TERM Y=2\n"
        "RULES\n"
        "r: X = 1 (. action(); .);\n"
        "r: Y(r) = 2 (. action2(); .);", *backend);
    // Label recursive call should pass ctx
    EXPECT_NE(code.find("burg_label(BURG_NODE_CHILD(node, i), ctx)"), std::string::npos);
    // Cache calls should pass ctx
    EXPECT_NE(code.find("burg_cache_lookup(id, ctx)"), std::string::npos);
    EXPECT_NE(code.find("burg_cache_store(id, p, ctx)"), std::string::npos);
    EXPECT_NE(code.find("burg_cache_clear(ctx)"), std::string::npos);
}

TEST(BurgCBackend, NoClassWrapper) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1 RULES r: X = 1;", *backend);
    EXPECT_EQ(code.find("class "), std::string::npos);
    EXPECT_NE(code.find("struct BurgState"), std::string::npos);
    EXPECT_NE(code.find("struct BurgContext"), std::string::npos);
}

// ── NAMESPACE tests ───────────────────────────────────────

TEST(BurgParser, ParsesNamespace) {
    Parser parser;
    const char* input = "TERM X=1 NAMESPACE foo RULES r: X = 1;";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    EXPECT_EQ(parser.ast->ns, "foo");
}

TEST(BurgParser, NamespaceOptional) {
    Parser parser;
    const char* input = "TERM X=1 RULES r: X = 1;";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    EXPECT_TRUE(parser.ast->ns.empty());
}

TEST(BurgCppBackend, EmitsNamespace) {
    std::string code = gen_cpp(
        "TERM X=1 NAMESPACE myns RULES r: X = 1;");
    EXPECT_NE(code.find("namespace myns {"), std::string::npos);
    EXPECT_NE(code.find("} // namespace myns"), std::string::npos);
    // Class should be inside the namespace
    auto ns_pos = code.find("namespace myns {");
    auto class_pos = code.find("class BurgMatcher {");
    auto ns_close = code.find("} // namespace myns");
    EXPECT_LT(ns_pos, class_pos);
    EXPECT_LT(class_pos, ns_close);
}

TEST(BurgCppBackend, NoNamespaceWithout) {
    std::string code = gen_cpp("TERM X=1 RULES r: X = 1;");
    EXPECT_EQ(code.find("namespace"), std::string::npos);
}

TEST(BurgCBackend, EmitsNamespace) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1 NAMESPACE myns RULES r: X = 1;", *backend);
    EXPECT_NE(code.find("namespace myns {"), std::string::npos);
    EXPECT_NE(code.find("} // namespace myns"), std::string::npos);
    // Structs should be inside the namespace
    auto ns_pos = code.find("namespace myns {");
    auto struct_pos = code.find("struct BurgState");
    auto ns_close = code.find("} // namespace myns");
    EXPECT_LT(ns_pos, struct_pos);
    EXPECT_LT(struct_pos, ns_close);
}
