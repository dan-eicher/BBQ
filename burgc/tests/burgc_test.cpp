#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include "generator.h"
#include "Parser.h"
#include "completeness.h"

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

// Helper: same, with --emit-rule-table turned on.
static std::string gen_code_with_table(const char* input, BurgBackend& backend) {
    backend.set_emit_rule_table(true);
    return gen_code(input, backend);
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

TEST(BurgAnalysis, RejectsDuplicateRules) {
    // Two rules with same (nonterm, pattern, guard) — must error.
    // Replaces the old "duplicate rule number" check that became
    // moot when rule numbers became auto-assigned.
    Parser parser;
    const char* input = "TERM X=1 RULES r: X = 1; r: X = 2;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());
    BurgGenerator gen;
    EXPECT_FALSE(gen.analyze(parser.ast));
    ASSERT_FALSE(gen.errors().empty());
    bool found = false;
    for (auto& e : gen.errors())
        if (e.find("duplicate rule") != std::string::npos) found = true;
    EXPECT_TRUE(found) << "expected 'duplicate rule' in errors";
}

TEST(BurgAnalysis, AcceptsRulesDifferingOnlyByGuard) {
    // Same (nonterm, pattern), different guards — NOT duplicates.
    Parser parser;
    const char* input =
        "TERM X=1 RULES "
        "r: X where (. cond_a(node) .) = 1; "
        "r: X where (. cond_b(node) .) = 2;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());
    BurgGenerator gen;
    EXPECT_TRUE(gen.analyze(parser.ast));
}

TEST(BurgAnalysis, ReportsUncoverableTreeShapeWithCoverage) {
    // Add has rules `reg: Add(reg, A)` and `reg: Add(A, B)` — multiple
    // terminals can appear at each position. Tuple (B-state, B-state)
    // is uncoverable: pos-1 demand of A doesn't match terminal=B,
    // and the (A, B) rule wants terminal=A at pos 0.
    Parser parser;
    const char* input =
        "TERM A=1 TERM B=2 TERM Add=3\n"
        "RULES "
        "reg: A = 1; "
        "reg: B = 1; "
        "reg: Add(reg, A) = 1; "
        "reg: Add(A, B) = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());

    // Default: shape warnings off
    {
        BurgGenerator gen;
        EXPECT_TRUE(gen.analyze(parser.ast));
        for (auto& w : gen.warnings())
            EXPECT_EQ(w.find("uncovered"), std::string::npos)
                << "default analyze should not emit shape warnings";
    }

    // --coverage on: shape warnings emitted
    {
        BurgGenerator gen;
        AnalysisConfig cfg;
        cfg.emit_coverage_warnings = true;
        cfg.skip_dead_rule_analysis = false;
        gen.set_completeness_config(cfg);
        EXPECT_TRUE(gen.analyze(parser.ast));
        bool found_add = false;
        for (auto& w : gen.warnings()) {
            if (w.find("Add") != std::string::npos &&
                w.find("uncovered") != std::string::npos) {
                found_add = true;
                break;
            }
        }
        EXPECT_TRUE(found_add) << "expected uncovered-Add warning under --coverage";
    }
}

TEST(BurgAnalysis, AsdlSchemaTightensDemandFilter) {
    // For an AST-shaped IR (where BURG arity matches ASDL node-field
    // count), the schema-aware filter enumerates child states based
    // on ASDL types instead of the heuristic. This surfaces uncovered
    // shapes that the heuristic misses — the heuristic only enumerates
    // states matching some rule's literal position demand, so a rule
    // like `expr: Add(expr, Const)` makes the heuristic enumerate
    // only Const-state at position 1; the schema demands "any expr
    // constructor", which includes Var, surfacing Add(_, Var) as
    // uncovered.
    AsdlSchema schema;
    schema.constructor_node_fields["Const"] = {};
    schema.constructor_node_fields["Var"] = {};
    schema.constructor_node_fields["Add"] = {"expr", "expr"};
    schema.sum_constructors["expr"] = {"Add", "Const", "Var"};

    Parser parser;
    const char* input =
        "TERM Const=1 TERM Var=2 TERM Add=3\n"
        "RULES "
        "expr: Const = 1; "
        "expr: Var = 1; "
        "expr: Add(expr, Const) = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());

    auto count_add_uncovered = [](const std::vector<std::string>& ws) {
        int n = 0;
        for (auto& w : ws)
            if (w.find("Add") != std::string::npos &&
                w.find("uncovered") != std::string::npos) n++;
        return n;
    };

    // Heuristic only — no warning surfaces (heuristic restricts pos 1
    // to terminal=Const).
    {
        BurgGenerator gen;
        AnalysisConfig cfg;
        cfg.emit_coverage_warnings = true;
        cfg.skip_dead_rule_analysis = false;
        gen.set_completeness_config(cfg);
        EXPECT_TRUE(gen.analyze(parser.ast));
        EXPECT_EQ(count_add_uncovered(gen.warnings()), 0);
    }

    // With schema — Add(*, Var) shape surfaces.
    {
        BurgGenerator gen;
        AnalysisConfig cfg;
        cfg.emit_coverage_warnings = true;
        cfg.skip_dead_rule_analysis = false;
        cfg.asdl = &schema;
        gen.set_completeness_config(cfg);
        EXPECT_TRUE(gen.analyze(parser.ast));
        EXPECT_GE(count_add_uncovered(gen.warnings()), 1);
    }
}

TEST(AsdlSchema, LoadsJsonSidecar) {
    // Write a tiny ASDL JSON sidecar (in the shape `asdl --json`
    // produces) and load it via load_asdl_schema().
    std::string tmp = "/tmp/burgc_test_asdl_schema.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "definitions": [
                {
                    "name": "expr",
                    "type": "sum",
                    "types": [
                        {
                            "name": "Add",
                            "fields": [
                                {"type": "expr", "name": "l", "flag": "none"},
                                {"type": "expr", "name": "r", "flag": "none"}
                            ]
                        },
                        {
                            "name": "Const",
                            "fields": [
                                {"type": "int64", "name": "v", "flag": "none"}
                            ]
                        }
                    ]
                }
            ]
        })";
    }

    AsdlSchema schema;
    std::string err;
    ASSERT_TRUE(load_asdl_schema(tmp, schema, err)) << err;

    ASSERT_EQ(schema.constructor_node_fields.count("Add"), 1u);
    ASSERT_EQ(schema.constructor_node_fields.at("Add").size(), 2u);
    EXPECT_EQ(schema.constructor_node_fields.at("Add")[0], "expr");
    EXPECT_EQ(schema.constructor_node_fields.at("Add")[1], "expr");

    ASSERT_EQ(schema.constructor_node_fields.count("Const"), 1u);
    EXPECT_TRUE(schema.constructor_node_fields.at("Const").empty());

    ASSERT_EQ(schema.sum_constructors.count("expr"), 1u);
    EXPECT_EQ(schema.sum_constructors.at("expr").size(), 2u);
}

TEST(BurgAnalysis, ReportsDeadRule) {
    // 'aux' is produced by a rule but never demanded as a child of a
    // reg-rooted derivation. Should be flagged dead.
    Parser parser;
    const char* input =
        "TERM X=1\n"
        "START reg\n"
        "RULES "
        "reg: X = 1; "
        "aux: X = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());
    BurgGenerator gen;
    AnalysisConfig cfg;
    cfg.skip_dead_rule_analysis = false;
    gen.set_completeness_config(cfg);
    EXPECT_TRUE(gen.analyze(parser.ast));
    bool found_dead = false;
    for (auto& w : gen.warnings()) {
        if (w.find("dead") != std::string::npos &&
            w.find("aux") != std::string::npos) {
            found_dead = true;
            break;
        }
    }
    EXPECT_TRUE(found_dead) << "expected dead-rule warning for aux";
}

// The automaton build must scale like Proebsting's algorithm ("BURS Automata
// Generation", TOPLAS 17(3) §3.3), not like naive tuple enumeration. There,
// transitions are computed over REPRESENTER states — per (operator, dimension)
// equivalence classes keeping only what can matter at that child position
// (Fig. 11 Project(); credited to Chase) — and only when a dimension gains a
// NEW representer (Fig. 12), so each (operator, representer-tuple) is evaluated
// once. The naive form enumerates raw-state tuples, |states|^arity per worklist
// round, re-recording transitions every round: on this fixture (12 leaf
// terminals feeding one arity-5 operator, all equivalent at every position)
// that is 12^5 tuples per round — and on the real codegen_wasm grammar it ran
// 25+ minutes into 6 GB of duplicate transition records without emitting one
// warning. Here all 12 leaf states project to ONE representer per dimension,
// so the whole automaton is a handful of evaluations.
TEST(BurgAnalysis, AutomatonScalesByRepresenterStatesNotRawTuples) {
    std::string input = "TERM Op5=100\n";
    for (int i = 1; i <= 12; i++)
        input += "TERM L" + std::to_string(i) + "=" + std::to_string(i) + "\n";
    input += "START stmt\nRULES\n";
    for (int i = 1; i <= 12; i++)
        input += "v: L" + std::to_string(i) + " = 1;\n";
    input += "stmt: Op5(v, v, v, v, v) = 1;\n";

    Parser parser;
    parser.init(input.c_str(), (int)input.size());
    ASSERT_TRUE(parser.parse());
    BurgGenerator gen;
    ASSERT_TRUE(gen.analyze(parser.ast));

    AnalysisConfig cfg;
    cfg.emit_coverage_warnings = true;
    cfg.skip_dead_rule_analysis = false;
    AnalysisReport report = run_completeness_analysis(gen.analysis(), cfg);

    EXPECT_LT(report.stats.tuples_evaluated, 1000u)
        << "transition computations must be bounded by representer-tuple "
           "combinations, not raw-state tuples (12^5 per round here)";
    EXPECT_LT(report.stats.transitions, 1000u)
        << "each (operator, tuple) transition must be recorded once, not once "
           "per worklist round";
    // And the answer itself: a complete grammar, so no uncovered shapes.
    for (auto& w : report.warnings)
        EXPECT_EQ(w.find("uncovered"), std::string::npos) << w;
}

// ── ENTRY: declaring the grammar's other entry points ─────────────────────────
//
// Goal-directed reduction is the formalism, not an extension: iburg exposes
// `rule(state, goalnt)` (iburg.pdf p.8) and Proebsting reduces via
// `RuleTable(state, goal)` (BURS Automata Generation p.464-465, Fig. 4). START is
// merely the ROOT's goal. A consumer whose control nodes cannot live in the
// grammar — javelina's structurer, because a `br` depth is a scope-stack property
// no action can reach — is a hand-written extension of Reduce() that demands
// other nonterminals at its pattern frontiers. The analysis has no way to see
// those demands, so it reports the whole family dead. ENTRY declares them.

TEST(BurgParser, ParsesEntryDecls) {
    Parser parser;
    const char* input =
        "TERM X=1\n"
        "START stmt\n"
        "ENTRY cond\n"
        "ENTRY ncond\n"
        "RULES stmt: X = 1; cond: X = 1; ncond: X = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    ASSERT_EQ(parser.ast->entries.size(), 2u);
    EXPECT_EQ(parser.ast->entries[0], "cond");
    EXPECT_EQ(parser.ast->entries[1], "ncond");
    EXPECT_EQ(parser.ast->start, "stmt");
}

TEST(BurgParser, EntryDeclOptional) {
    Parser parser;
    const char* input = "TERM X=1\nSTART reg\nRULES reg: X = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());
    ASSERT_NE(parser.ast, nullptr);
    EXPECT_TRUE(parser.ast->entries.empty());
}

// The point of the whole declaration: a family reachable ONLY by name from the
// consumer is live, and the analysis can finally say so.
TEST(BurgAnalysis, EntrySeedsDemandSoItsRulesAreNotDead) {
    const char* with_entry =
        "TERM X=1\nTERM Y=2\n"
        "START stmt\n"
        "ENTRY aux\n"
        "RULES stmt: X = 1; aux: Y = 1;";
    const char* without =
        "TERM X=1\nTERM Y=2\n"
        "START stmt\n"
        "RULES stmt: X = 1; aux: Y = 1;";

    auto aux_dead = [](const char* src) {
        Parser parser;
        parser.init(src, (int)strlen(src));
        EXPECT_TRUE(parser.parse());
        BurgGenerator gen;
        AnalysisConfig cfg;
        cfg.skip_dead_rule_analysis = false;
        gen.set_completeness_config(cfg);
        EXPECT_TRUE(gen.analyze(parser.ast));
        for (auto& w : gen.warnings())
            if (w.find("dead") != std::string::npos &&
                w.find("aux") != std::string::npos) return true;
        return false;
    };

    // Undeclared, the rule really is unreachable from START — the warning is
    // correct, and this half is what makes the other half mean something.
    EXPECT_TRUE(aux_dead(without)) << "without ENTRY, aux is genuinely unreachable";
    EXPECT_FALSE(aux_dead(with_entry)) << "ENTRY aux must seed demand for aux";
}

// A declaration that names nothing is a typo, and a typo that silently does
// nothing would hand back exactly the dead-rule warnings ENTRY exists to remove.
TEST(BurgAnalysis, UnknownEntryIsAnError) {
    Parser parser;
    const char* input =
        "TERM X=1\nSTART reg\nENTRY nosuch\nRULES reg: X = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());
    BurgGenerator gen;
    EXPECT_FALSE(gen.analyze(parser.ast));
    bool named = false;
    for (auto& e : gen.errors())
        if (e.find("nosuch") != std::string::npos) named = true;
    EXPECT_TRUE(named) << "the error must name the undeclared entry";
}

// Declaring the start nonterminal adds no demand it does not already have.
TEST(BurgAnalysis, EntryOnStartIsHarmless) {
    Parser parser;
    const char* input =
        "TERM X=1\nSTART reg\nENTRY reg\nRULES reg: X = 1;";
    parser.init(input, (int)strlen(input));
    ASSERT_TRUE(parser.parse());
    BurgGenerator gen;
    AnalysisConfig cfg;
    cfg.skip_dead_rule_analysis = false;
    gen.set_completeness_config(cfg);
    EXPECT_TRUE(gen.analyze(parser.ast));
    for (auto& w : gen.warnings())
        EXPECT_EQ(w.find("dead"), std::string::npos) << w;
}

// ENTRY is an ANALYSIS declaration: the goal-directed C API it describes
// (burg_label_root + burg_reduce) already exists, so the matcher must not change.
TEST(BurgCBackend, EntryDoesNotAlterGeneratedMatcher) {
    const char* base =
        "TERM X=1\nSTART stmt\n%%RULES stmt: X = 1 (. emit_x(node); .); cond: X = 1;";
    std::string with_entry(base), without(base);
    with_entry.replace(with_entry.find("%%"), 2, "ENTRY cond\n");
    without.replace(without.find("%%"), 2, "");

    auto backend_a = create_c_backend();
    auto backend_b = create_c_backend();
    EXPECT_EQ(gen_code(with_entry.c_str(), *backend_a),
              gen_code(without.c_str(), *backend_b));
}

TEST(BurgAnalysis, NoFalsePositivesOnSimpleGrammar) {
    // simple.burg is well-formed and complete; the analysis should
    // produce zero warnings on it.
    BurgGenerator gen;
    auto* spec = gen.parse(source_path("tests/data/simple.burg"));
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(gen.analyze(spec));
    EXPECT_TRUE(gen.warnings().empty())
        << "first warning was: "
        << (gen.warnings().empty() ? "" : gen.warnings()[0]);
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

// ── The goal-directed entry (burg_label_root + burg_cost) ──────────────────────
//
// burg_rewrite hard-codes the start nonterminal and keeps its labeled state to itself,
// so a consumer could neither read what the DP claims a cover costs nor tile a tree
// toward a nonterminal its CONTEXT names. These two exports close that. The behaviour
// is asserted end-to-end by burgc_coverage_{c,cpp}_e2e; these tests pin the emitted
// SHAPE — in particular that label_root reuses the same prologue as burg_rewrite,
// since a stale arena would hand back a state pointing at freed memory.

TEST(BurgCBackend, EmitsLabelRootAndCost) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1\n"
        "RULES r: X = 1 (. emit_x(node); .);", *backend);
    EXPECT_NE(code.find("int burg_cost(burg_state_t* state, int goalnt);"), std::string::npos)
        << "burg_cost must be declared in the public API";
    EXPECT_NE(code.find("burg_state_t* burg_label_root(BURG_NODE_TYPE root, burg_ctx_t* ctx);"),
              std::string::npos)
        << "burg_label_root must be declared in the public API";
    // An absent cover must read back as the state's own sentinel, not 0 ("free").
    EXPECT_NE(code.find("goalnt > BURG_MAX_NT) return BURG_MAX_COST;"), std::string::npos);
    EXPECT_NE(code.find("return state->cost[goalnt];"), std::string::npos);
    // Same prologue as burg_rewrite: poll the error sink, THEN reset the arena.
    auto lr = code.find("burg_state_t* burg_label_root(BURG_NODE_TYPE root, burg_ctx_t* ctx) {");
    ASSERT_NE(lr, std::string::npos);
    auto poll  = code.find("if (burg_has_error(ctx)) return NULL;", lr);
    auto reset = code.find("arena_reset(ctx);", lr);
    auto label = code.find("return burg_label_tree(root, ctx);", lr);
    ASSERT_NE(poll, std::string::npos) << "label_root must short-circuit on a pending error";
    ASSERT_NE(reset, std::string::npos);
    ASSERT_NE(label, std::string::npos);
    EXPECT_LT(poll, reset);
    EXPECT_LT(reset, label);
}

TEST(BurgCppBackend, EmitsLabelRootAndCost) {
    std::string code = gen_cpp(
        "TERM X=1\n"
        "RULES r: X = 1 (. emit_x(node); .);");
    EXPECT_NE(code.find("static int burg_cost(BurgState* state, int goalnt)"), std::string::npos);
    EXPECT_NE(code.find("BurgState* burg_label_root(BURG_NODE_TYPE root)"), std::string::npos);
    EXPECT_NE(code.find("goalnt > BURG_MAX_NT) return BURG_MAX_COST;"), std::string::npos);
    // C++ reports by throwing, so there is no error poll to emit — but the arena reset
    // is not optional: the returned state is allocated out of it.
    auto lr = code.find("BurgState* burg_label_root(BURG_NODE_TYPE root)");
    ASSERT_NE(lr, std::string::npos);
    EXPECT_EQ(code.find("burg_has_error", lr), std::string::npos)
        << "the C++ backend throws; a poll here would be dead code";
    auto reset = code.find("arena_reset();", lr);
    auto label = code.find("return burg_label_tree(root);", lr);
    ASSERT_NE(reset, std::string::npos);
    ASSERT_NE(label, std::string::npos);
    EXPECT_LT(reset, label);
}

// Backend parity: the C backend exports burg_rule/burg_cost/burg_nt_name/burg_reduce
// as public functions, so the C++ backend must expose the same names as public members.
// They were emitted into the class's PRIVATE section, which made the goal-directed
// entry usable from C and unusable from C++ — half an API is the bug this pins.
TEST(BurgCppBackend, ConsumerFacingMembersArePublic) {
    std::string code = gen_cpp(
        "TERM X=1\n"
        "RULES r: X = 1 (. emit_x(node); .);");
    // The consumer-facing section is opened by the LAST access specifier in the class;
    // everything the frame emits before it is private (arena, cache, DP, labelers).
    auto section = code.rfind("public:");
    auto last_private = code.rfind("private:");
    ASSERT_NE(section, std::string::npos);
    ASSERT_NE(last_private, std::string::npos);
    ASSERT_GT(section, last_private) << "the consumer section must be the last one opened";
    for (const char* sym : {"int burg_rule(", "int burg_cost(", "const char* burg_nt_name(",
                            "void burg_reduce(", "BurgState* burg_label_root(",
                            "void burg_rewrite("}) {
        auto pos = code.find(sym);
        ASSERT_NE(pos, std::string::npos) << sym << " must be emitted";
        EXPECT_GT(pos, section) << sym << " must sit in the consumer-facing section";
    }
    // …and the labelers must NOT: they are `static` in the C backend for the same reason.
    for (const char* sym : {"BurgState* burg_label_tree(", "BurgState* burg_label("}) {
        auto pos = code.find(sym);
        ASSERT_NE(pos, std::string::npos) << sym << " must be emitted";
        EXPECT_LT(pos, section) << sym << " must stay private";
    }
}

// ── --emit-rule-table ─────────────────────────────────────────────────────────
//
// Exporting the rules as data is what lets a consumer check the labeler's cover
// against its own search — the DP cannot audit itself. The e2e drivers run that
// search; these pin the emission: opt-in, both backends, and a pattern encoding
// that distinguishes a chain rule from a terminal one (get that wrong and the
// oracle agrees with the labeler about nothing).

static const char* RULE_TABLE_GRAMMAR =
    "TERM Const = 1\n"
    "TERM Add   = 2\n"
    "START stmt\n"
    "RULES\n"
    "reg : Const where (. is_small(node) .) = 1 (. emit_c(node); .);\n"
    "reg : Add(reg, reg) = 2 (. emit_a(node); .);\n"
    "stmt: reg = 0;\n";

TEST(BurgCBackend, RuleTableIsOptIn) {
    auto off = create_c_backend();
    EXPECT_EQ(gen_code(RULE_TABLE_GRAMMAR, *off).find("burg_rule_table"), std::string::npos)
        << "nothing inside the matcher reads the table; it must not be emitted by default";
    auto on = create_c_backend();
    std::string code = gen_code_with_table(RULE_TABLE_GRAMMAR, *on);
    EXPECT_NE(code.find("extern const burg_rule_row_t burg_rule_table[];"), std::string::npos);
    EXPECT_NE(code.find("extern const int burg_rule_table_len;"), std::string::npos);
    EXPECT_NE(code.find("int burg_rule_guard(int rule, BURG_NODE_TYPE node, burg_ctx_t* ctx);"),
              std::string::npos);
    EXPECT_NE(code.find("const int burg_rule_table_len = 3;"), std::string::npos);
}

TEST(BurgCppBackend, RuleTableIsOptIn) {
    EXPECT_EQ(gen_cpp(RULE_TABLE_GRAMMAR).find("burg_rule_table"), std::string::npos);
    auto on = create_cpp_backend();
    std::string code = gen_code_with_table(RULE_TABLE_GRAMMAR, *on);
    // Inside the class, so the arrays are inline members rather than externs.
    EXPECT_NE(code.find("inline static const struct burg_rule_row_t burg_rule_table[]"),
              std::string::npos);
    EXPECT_NE(code.find("inline static const int burg_rule_table_len = 3;"), std::string::npos);
    EXPECT_NE(code.find("static int burg_rule_guard(int rule, BURG_NODE_TYPE node)"),
              std::string::npos);
}

// The encoding has to distinguish the three shapes a pattern can take.
TEST(BurgCBackend, RuleTableEncodesPatternsAndGuards) {
    auto backend = create_c_backend();
    std::string code = gen_code_with_table(RULE_TABLE_GRAMMAR, *backend);
    // `reg: Const` — one terminal node, no children.
    EXPECT_NE(code.find("burg_pat_1[] = {{1, (int)BURG_Const, 0}}"), std::string::npos);
    // `reg: Add(reg, reg)` — a terminal with two NONTERMINAL leaves (is_term 0),
    // which is where a consumer's search recurses. (The start nonterminal is
    // rotated to index 1, so `reg` is 2 here.)
    EXPECT_NE(code.find("burg_pat_2[] = {{1, (int)BURG_Add, 2}, {0, (int)2, 0}, {0, (int)2, 0}}"),
              std::string::npos);
    // `stmt: reg` — a chain rule: a lone nonterminal leaf, no terminal at all.
    EXPECT_NE(code.find("burg_pat_3[] = {{0, (int)2, 0}}"), std::string::npos);
    // Rule 1 is guarded, rules 2 and 3 are not, and the guard is evaluable by number.
    EXPECT_NE(code.find(", burg_pat_1, 1},"), std::string::npos) << "rule 1 is flagged guarded";
    EXPECT_NE(code.find(", burg_pat_2, 0},"), std::string::npos) << "rule 2 is flagged unguarded";
    EXPECT_NE(code.find("case 1: return (is_small(node)) ? 1 : 0;"), std::string::npos);
    EXPECT_NE(code.find("default: return 1;"), std::string::npos)
        << "an unguarded rule must evaluate to 1 so callers need no special case";
}

// Variable-arity children must be COSTED, not just reduced. The reducer tiles
// children beyond the declared pattern with START; iburg (p.4, p.7's state())
// sums every child's cost through the nonterminal demanded of it, so the DP arm
// must carry the same loop on the cost side — otherwise the labeler minimizes
// over covers of a smaller tree than the reducer emits. Behaviour is pinned by
// the coverage e2e drivers (Call fixture); this pins the emitted shape and that
// the loop starts at the DECLARED child count, mirroring emit_child_reductions.
TEST(BurgCBackend, DPCostsVariableArityChildren) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM Const = 1\n"
        "TERM Add   = 2\n"
        "TERM Call  = 3\n"
        "START stmt\n"
        "RULES\n"
        "reg : Const = 1;\n"
        "reg : Add(reg, reg) = 1;\n"
        "reg : Call = 3;\n"
        "stmt: reg = 0;\n", *backend);
    // The leaf-terminal rule (declared 0 children): extras from 0, costed at START.
    EXPECT_NE(code.find("for (int _ci = 0; _ci < p->child_count; _ci++)"), std::string::npos);
    EXPECT_NE(code.find("c += p->children[_ci]->cost[1];"), std::string::npos);
    // The binary rule: extras start past its two declared children.
    EXPECT_NE(code.find("for (int _ci = 2; _ci < p->child_count; _ci++)"), std::string::npos);
}

// A leaf pattern (arity 0) and an operand pattern (arity N) for the SAME terminal
// must disambiguate by arity — matching trees by operator + arity is BURG's job.
// Regression: the leaf rule was emitted with no `child_count` guard, so it matched
// higher-arity nodes too and, being cheapest, won — e.g. `Node` would steal a
// `Node(val,val)` node. The leaf rule must guard `child_count == 0`.
TEST(BurgCppBackend, LeafRuleGuardsArity) {
    const char* input =
        "TERM Lit = 1\n"
        "TERM Node = 2\n"
        "START val\n"
        "RULES\n"
        "val : Lit            = 1;\n"
        "val : Node           = 1;\n"
        "val : Node(val, val) = 1;\n";
    std::string code = gen_cpp(input);
    ASSERT_FALSE(code.empty());
    EXPECT_NE(code.find("child_count == 0"), std::string::npos)
        << "leaf rule emitted with no arity guard — it would over-match arity-N nodes";
}

// The inverse of LeafRuleGuardsArity: a terminal with ONLY a leaf rule must NOT be
// arity-guarded. In slot-based IRs the operands are virtual and child_count is
// unrelated to the pattern, so a `child_count == 0` check would wrongly reject real
// nodes that carry children. The guard is emitted only to disambiguate a terminal
// that has BOTH a leaf and an operand rule. (Regression: a blanket leaf guard
// segfaulted slot-based consumers like the jitterator calc.)
TEST(BurgCppBackend, LeafOnlyTerminalUnguarded) {
    const char* input =
        "TERM Op = 1\n"
        "START val\n"
        "RULES\n"
        "val : Op = 1;\n";
    std::string code = gen_cpp(input);
    ASSERT_FALSE(code.empty());
    EXPECT_EQ(code.find("child_count == 0"), std::string::npos)
        << "leaf-only terminal wrongly arity-guarded — breaks slot-based IRs";
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
    // Clearing is the CALLER's tool for per-call isolation, so the API exposes it...
    EXPECT_NE(code.find("burg_clear_error("), std::string::npos);
    // ...but rewrite/reduce poll rather than clear: a pending error short-circuits them.
    EXPECT_NE(code.find("if (burg_has_error("), std::string::npos);
    // No assert/abort calls
    EXPECT_EQ(code.find("assert(false"), std::string::npos);
}

// A body is lowered as many burg_rewrite calls over one ctx with a single check at the
// end. burg_rewrite must therefore SHORT-CIRCUIT on a pending error, not clear it —
// clearing let a later statement wipe an earlier statement's no-cover, and the truncated
// body passed its check. Asserted on the text because it is an ordering property of the
// emitted prologue; burgc_coverage_c_e2e asserts the behaviour it produces.
TEST(BurgCBackend, RewriteShortCircuitsOnPendingError) {
    auto backend = create_c_backend();
    std::string code = gen_code(
        "TERM X=1\n"
        "RULES r: X = 1 (. emit_x(node); .);", *backend);
    // Anchor on the DEFINITION, not the header declaration — the ordering claim is about
    // burg_rewrite's own prologue, and anything emitted between the two would otherwise
    // supply the poll/reset pair this reads.
    auto rewrite_pos = code.find("void burg_rewrite(BURG_NODE_TYPE root, burg_ctx_t* ctx) {");
    ASSERT_NE(rewrite_pos, std::string::npos);
    auto poll_pos  = code.find("if (burg_has_error(ctx)) return;", rewrite_pos);
    auto arena_pos = code.find("arena_reset(ctx)", rewrite_pos);
    ASSERT_NE(poll_pos, std::string::npos) << "burg_rewrite must open by polling";
    ASSERT_NE(arena_pos, std::string::npos);
    EXPECT_LT(poll_pos, arena_pos) << "the poll must precede the arena reset";
    // And it must NOT clear on entry: the only burg_clear_error in the emitted matcher
    // is the public API definition, which is defined before burg_rewrite.
    EXPECT_EQ(code.find("burg_clear_error(ctx);", rewrite_pos), std::string::npos);
}

// Uncovered-root reporting is asserted end-to-end instead of by grepping the emitted
// text: burgc_coverage_c_e2e and burgc_coverage_cpp_e2e compile the generated matcher
// and run it. A string count cannot tell a matcher that reports the right thing from one
// that reports a stale message from the previous rewrite, which is what the C backend did.

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

// ── COMPILER (namespace) tests ────────────────────────────

TEST(BurgParser, ParsesNamespace) {
    Parser parser;
    const char* input = "TERM X=1 COMPILER foo RULES r: X = 1;";
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
        "TERM X=1 COMPILER myns RULES r: X = 1;");
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
        "TERM X=1 COMPILER myns RULES r: X = 1;", *backend);
    // C has no namespaces — COMPILER becomes a symbol prefix
    EXPECT_EQ(code.find("namespace"), std::string::npos);
    // User-facing names are prefixed (snake_case)
    EXPECT_NE(code.find("myns_burg_ctx_t"), std::string::npos);
    EXPECT_NE(code.find("myns_burg_ctx_init"), std::string::npos);
    EXPECT_NE(code.find("myns_burg_rewrite"), std::string::npos);
    // Internal statics are not prefixed
    EXPECT_NE(code.find("static void burg_dp"), std::string::npos);
}
