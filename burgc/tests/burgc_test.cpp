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

static std::string frame_dir() {
    return std::string(SOURCE_DIR) + "/frames";
}

// Helper: parse + analyze + generate with a specific backend
// Returns header + impl concatenated (for searching both)
static std::string gen_code(const char* input, BurgBackend& backend) {
    Parser parser;
    parser.init(input, (int)strlen(input));
    if (!parser.parse() || !parser.ast) return "";
    BurgGenerator gen;
    if (!gen.analyze(parser.ast)) return "";
    if (backend.needs_impl_file()) {
        std::ostringstream hdr, impl;
        backend.generate(hdr, impl, gen.analysis(), "test.h", frame_dir());
        return hdr.str() + "\n" + impl.str();
    }
    std::ostringstream out;
    backend.generate(out, gen.analysis(), frame_dir());
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

TEST(BurgParser, ParsesSimpleCosts) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    for (auto* rule : spec->rules)
        EXPECT_EQ(rule->cost, 1);
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

TEST(BurgAnalysis, AutoAssignsRuleNumbers) {
    Parser parser;
    const char* input = "TERM X=1 TERM Y=2 RULES r: X = 1; s: Y = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());
    BurgGenerator gen;
    EXPECT_TRUE(gen.analyze(parser.ast));
    EXPECT_TRUE(gen.errors().empty());
    // Rules are renumbered sequentially starting at 1, regardless of source order.
    ASSERT_EQ(parser.ast->rules.size(), 2u);
    EXPECT_EQ(parser.ast->rules[0]->rule_number, 1);
    EXPECT_EQ(parser.ast->rules[1]->rule_number, 2);
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
    EXPECT_NE(out.str().find("closure_"), std::string::npos);
}

TEST(BurgCppBackend, EmitsRuleLookup) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
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
    const char* input = "TERM X=1 RULES r: X = 3 (. do_thing(); .);";
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
    EXPECT_EQ(out.str().find("burg_reduce"), std::string::npos);
}

TEST(BurgCppBackend, EmitsUserHeaders) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
    EXPECT_NE(out.str().find("#include \"emitter.h\""), std::string::npos);
}

TEST(BurgCppBackend, StartSymbolIsFirstNonterminal) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/chain.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
    EXPECT_NE(out.str().find("stmt_NT = 1"), std::string::npos);
}

// ── Graph-aware codegen tests ─────────────────────────────

TEST(BurgCppBackend, EmitsStateCache) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
    std::string code = out.str();
    EXPECT_NE(code.find("BurgState* burg_label(BURG_NODE_TYPE node)"), std::string::npos);
    EXPECT_NE(code.find("BurgState* burg_label_tree(BURG_NODE_TYPE node)"), std::string::npos);
    EXPECT_NE(code.find("void burg_dp(BurgState* p, BURG_NODE_TYPE node)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_ARITY(node)"), std::string::npos);
    EXPECT_NE(code.find("BURG_NODE_CHILD(node, i)"), std::string::npos);
}

TEST(BurgCppBackend, GraphLabelCachesBeforeDP) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
    std::string code = out.str();
    // In the graph label function, cache_store should appear before burg_dp call
    auto label_pos = code.find("BurgState* burg_label(BURG_NODE_TYPE node)");
    auto cache_pos = code.find("burg_cache_store(id, p)", label_pos);
    auto dp_pos = code.find("burg_dp(p, node)", cache_pos);
    ASSERT_NE(label_pos, std::string::npos);
    ASSERT_NE(cache_pos, std::string::npos);
    ASSERT_NE(dp_pos, std::string::npos);
    EXPECT_LT(cache_pos, dp_pos);
}

TEST(BurgCppBackend, IndexedChildrenInDP) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
    EXPECT_NE(out.str().find("void burg_reduce(BURG_NODE_TYPE node, BurgState* state, int goalnt)"),
              std::string::npos);
}

TEST(BurgCppBackend, EmitsArenaAllocator) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
    std::string code = out.str();
    EXPECT_NE(code.find("arena_alloc"), std::string::npos);
    EXPECT_NE(code.find("arena_reset"), std::string::npos);
    EXPECT_NE(code.find("arena_"), std::string::npos);
    // No malloc in generated code
    EXPECT_EQ(code.find("malloc("), std::string::npos);
}

TEST(BurgCppBackend, TreeFastPathInRewrite) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
    std::string code = out.str();
    // Fast path: check SUCC_COUNT, call burg_label_tree directly
    EXPECT_NE(code.find("BURG_NODE_SUCC_COUNT(root) == 0"), std::string::npos);
    EXPECT_NE(code.find("burg_label_tree(root)"), std::string::npos);
    // Slow path: RPO still present
    EXPECT_NE(code.find("BURG_NODE_SUCC("), std::string::npos);
    EXPECT_NE(code.find("burg_label(n)"), std::string::npos);
}

TEST(BurgCppBackend, TreeLabelHasNoCache) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
    std::string code = out.str();
    // Find burg_label_tree body — should NOT have cache calls
    auto tree_pos = code.find("BurgState* burg_label_tree(BURG_NODE_TYPE node)");
    auto tree_end = code.find("burg_label(BURG_NODE_TYPE node)", tree_pos + 10);
    ASSERT_NE(tree_pos, std::string::npos);
    ASSERT_NE(tree_end, std::string::npos);
    std::string tree_body = code.substr(tree_pos, tree_end - tree_pos);
    EXPECT_EQ(tree_body.find("burg_cache"), std::string::npos);
    // But should have arena_alloc and burg_dp
    EXPECT_NE(tree_body.find("arena_alloc"), std::string::npos);
    EXPECT_NE(tree_body.find("burg_dp"), std::string::npos);
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
    const char* input = "TERM X=1 RULES r: X where (. pred(n) .) = 2 (. action(); .);";
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
    gen.generate(out, frame_dir());
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
    gen.generate(out, frame_dir());
    EXPECT_NE(out.str().find("burg_reduce(n, s,"), std::string::npos);
}

// Error reporting regression tests.
//
// C++: burg_set_error() throws BurgError. Consumers handle errors by
// catching, so the codegen does NOT emit polling checks (they'd be
// dead code under the throw model).
//
// C: burg_set_error() stores in the context. Consumers call
// burg_has_error() after burg_rewrite() to detect failure. Codegen
// emits short-circuit polling checks at the top of burg_reduce and
// between phases of burg_rewrite.
//
// Without these, the consumer either sees a process abort (assert)
// or silent miscompilation when its IR carries an unknown op.
TEST(BurgCppBackend, ErrorReportingThrowsBurgError) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/actions.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
    std::string code = out.str();
    // BurgError class is defined and burg_set_error throws it
    EXPECT_NE(code.find("class BurgError"), std::string::npos);
    EXPECT_NE(code.find("throw BurgError"), std::string::npos);
    EXPECT_NE(code.find("burg_set_error("), std::string::npos);
    // No assert/abort calls in generated code
    EXPECT_EQ(code.find("assert(false"), std::string::npos);
    // No polling — exception model unwinds the stack instead
    EXPECT_EQ(code.find("burg_clear_error()"), std::string::npos);
    EXPECT_EQ(code.find("if (burg_has_error()"), std::string::npos);
}

TEST(BurgCBackend, ErrorReportingPollsContext) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1\n"
        "RULES r: X = 1 (. emit_x(node); .);", *backend);
    // Public error API on the context struct
    EXPECT_NE(code.find("burg_error_msg"), std::string::npos);
    EXPECT_NE(code.find("bool burg_has_error("), std::string::npos);
    EXPECT_NE(code.find("burg_set_error("), std::string::npos);
    // burg_rewrite clears errors first; reduce/rewrite poll between phases
    EXPECT_NE(code.find("burg_clear_error("), std::string::npos);
    EXPECT_NE(code.find("if (burg_has_error("), std::string::npos);
    // No assert/abort calls
    EXPECT_EQ(code.find("assert(false"), std::string::npos);
}

TEST(BurgCppBackend, RewriteResetsArenaFirst) {
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    std::ostringstream out;
    gen.generate(out, frame_dir());
    std::string code = out.str();
    // arena_reset should be the first thing in burg_rewrite
    auto rewrite_pos = code.find("void burg_rewrite(");
    auto arena_pos = code.find("arena_reset()", rewrite_pos);
    auto succ_pos = code.find("BURG_NODE_SUCC_COUNT", rewrite_pos);
    ASSERT_NE(arena_pos, std::string::npos);
    ASSERT_NE(succ_pos, std::string::npos);
    EXPECT_LT(arena_pos, succ_pos);
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

TEST(BurgParser, ParsesPrivateBlock) {
    Parser parser;
    const char* input =
        "TERM X=1\n"
        "PRIVATE (. static int helper() { return 42; } .)\n"
        "RULES r: X = 1;";
    parser.init(input, (int)strlen(input));
    EXPECT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    EXPECT_NE(parser.ast->private_helpers.find("helper"), std::string::npos);
    EXPECT_NE(parser.ast->private_helpers.find("return 42"), std::string::npos);
}

TEST(BurgCppBackend, EmitsPrivateBlockInPrivateSection) {
    std::string code = gen_cpp(
        "TERM X=1\n"
        "PRIVATE (. int magic_helper() { return 7; } .)\n"
        "RULES r: X = 1;");
    // The PRIVATE block lands inside the class
    EXPECT_NE(code.find("magic_helper"), std::string::npos);
    // …after the first `private:` keyword (i.e. in the private section)
    auto private_pos = code.find("private:");
    auto helper_pos = code.find("magic_helper");
    ASSERT_NE(private_pos, std::string::npos);
    ASSERT_NE(helper_pos, std::string::npos);
    EXPECT_LT(private_pos, helper_pos);
}

TEST(BurgCBackend, EmitsPrivateBlockInImpl) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1\n"
        "PRIVATE (. static int magic_helper(void) { return 7; } .)\n"
        "RULES r: X = 1 (. (void)node; .);", *backend);
    // The PRIVATE block lands at file scope in the .c
    EXPECT_NE(code.find("magic_helper"), std::string::npos);
    // …and is NOT exposed in the public context struct
    auto ctx_pos = code.find("typedef struct burg_ctx_t");
    auto ctx_end = code.find("} burg_ctx_t");
    ASSERT_NE(ctx_pos, std::string::npos);
    ASSERT_NE(ctx_end, std::string::npos);
    auto helper_pos_in_struct = code.find("magic_helper", ctx_pos);
    EXPECT_TRUE(helper_pos_in_struct == std::string::npos
             || helper_pos_in_struct > ctx_end);
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
    gen.generate(out, frame_dir());
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
    // Uses snake_case typedef for C
    EXPECT_NE(code.find("typedef struct burg_ctx_t"), std::string::npos);
    EXPECT_NE(code.find("int count_ = 0;"), std::string::npos);
    // State struct is snake_case too
    EXPECT_NE(code.find("burg_state_t"), std::string::npos);
    // Uses CRT types instead of STL
    EXPECT_NE(code.find("bbq_arena arena"), std::string::npos);
    EXPECT_NE(code.find("bbq_htree* state_cache"), std::string::npos);
    // Has lifecycle functions
    EXPECT_NE(code.find("burg_ctx_init"), std::string::npos);
    EXPECT_NE(code.find("burg_ctx_free"), std::string::npos);
    // No class wrapper or C++ STL
    EXPECT_EQ(code.find("class BurgMatcher"), std::string::npos);
    EXPECT_EQ(code.find("std::vector"), std::string::npos);
}

TEST(BurgCBackend, StaticFunctions) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1 RULES r: X = 1 (. action(); .);", *backend);
    // Public functions in impl (not static)
    EXPECT_NE(code.find("void burg_rewrite(BURG_NODE_TYPE root, burg_ctx_t* ctx)"),
              std::string::npos);
    EXPECT_NE(code.find("void burg_reduce(BURG_NODE_TYPE node, burg_state_t* state, int goalnt, burg_ctx_t* ctx)"),
              std::string::npos);
    // Internal functions are static
    EXPECT_NE(code.find("static burg_state_t* burg_label(BURG_NODE_TYPE node, burg_ctx_t* ctx)"),
              std::string::npos);
}

TEST(BurgCBackend, CtxThreadedInCalls) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1 TERM Y=2\n"
        "RULES\n"
        "r: X = 1 (. action(); .);\n"
        "r: Y(r) = 2 (. action2(); .);", *backend);
    // Graph label recursive call should pass ctx
    EXPECT_NE(code.find("burg_label(BURG_NODE_CHILD(node, i), ctx)"), std::string::npos);
    // Tree label recursive call should pass ctx
    EXPECT_NE(code.find("burg_label_tree(BURG_NODE_CHILD(node, i), ctx)"), std::string::npos);
    // Cache calls should pass ctx
    EXPECT_NE(code.find("burg_cache_lookup(id, ctx)"), std::string::npos);
    EXPECT_NE(code.find("burg_cache_store(id, p, ctx)"), std::string::npos);
    EXPECT_NE(code.find("burg_cache_clear(ctx)"), std::string::npos);
    // Arena calls should pass ctx
    EXPECT_NE(code.find("arena_alloc(sizeof(burg_state_t), ctx)"), std::string::npos);
    EXPECT_NE(code.find("arena_reset(ctx)"), std::string::npos);
}

TEST(BurgCBackend, NoClassWrapper) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1 RULES r: X = 1;", *backend);
    EXPECT_EQ(code.find("class "), std::string::npos);
    EXPECT_NE(code.find("burg_state_t"), std::string::npos);
    EXPECT_NE(code.find("burg_ctx_t"), std::string::npos);
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

TEST(BurgCBackend, NamespaceBecomesPrefixInC) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1 NAMESPACE myns RULES r: X = 1;", *backend);
    // C has no namespaces — NAMESPACE becomes a symbol prefix
    EXPECT_EQ(code.find("namespace"), std::string::npos);
    // User-facing names are prefixed (snake_case)
    EXPECT_NE(code.find("myns_burg_ctx_t"), std::string::npos);
    EXPECT_NE(code.find("myns_burg_ctx_init"), std::string::npos);
    EXPECT_NE(code.find("myns_burg_rewrite"), std::string::npos);
    // Internal statics are not prefixed
    EXPECT_NE(code.find("static void burg_dp"), std::string::npos);
}
