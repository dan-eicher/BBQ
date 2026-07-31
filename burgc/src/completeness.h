// Unified semantic analysis for burg grammars. Implements three
// checks that share a tree-automaton view of the rule set:
//
//   1. Duplicate-rule detection (error). Rules with identical
//      (nonterm, pattern signature, guard text) are flagged.
//   2. Brandner-style completeness (warning, opt-in). Tree shapes
//      that no rule covers are reported as concrete witnesses.
//   3. Top-down liveness from START (warning). Rules whose LHS is
//      never demanded as a child of a START-rooted derivation are
//      flagged as dead.
//
// This is BURG's runtime DP evaluated symbolically at compile time,
// with cost erased. The runtime DP is generated C++ code, so the
// compile-time pass re-walks the rules with the same matching logic
// expressed directly in C++ instead of via code emission.

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct BurgAnalysis;

// Automaton-size accounting, populated when the tree automaton is built.
// `tuples_evaluated` is the number of (operator, child-tuple) transition
// computations performed — the quantity that made the naive enumeration
// infeasible on large grammars (|states|^arity per worklist round), and the
// number the representer-state algorithm (Proebsting, "BURS Automata
// Generation", TOPLAS 17(3) §3.3, after Chase) keeps small. Exposed so a test
// can pin the bound instead of trusting the comment.
struct AnalysisStats {
    std::size_t states = 0;
    std::size_t representers = 0;      // summed over (operator, dimension)
    std::size_t transitions = 0;
    std::size_t tuples_evaluated = 0;
};

struct AnalysisReport {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    AnalysisStats stats;
};

// Per-constructor schema info derived from an ASDL JSON sidecar.
// Each entry maps a constructor name (which BURG will see as a TERM
// at pattern roots) to the ordered list of its node-typed (i.e.
// structural) field types. For an AST-shaped IR this directly
// describes "the i-th child of this terminal must be a node of
// type T". For slot-based IRs where BURG arity != node-field count,
// the schema can't drive the demand filter and the analysis falls
// back to its heuristic.
struct AsdlSchema {
    // constructor name -> ordered node-typed field types
    std::map<std::string, std::vector<std::string>> constructor_node_fields;
    // node sum-type name -> set of its constructor names
    std::map<std::string, std::vector<std::string>> sum_constructors;
};

struct AnalysisConfig {
    // Enable shape-enumeration warnings (uncovered tree shapes).
    // Off by default: without a schema constraining what shapes the
    // consumer's IR can actually produce, enumeration emits false
    // positives on shapes that combinatorially exist but are never
    // constructed.
    bool emit_coverage_warnings = false;

    // Skip the tree-automaton build and the dead-rule analysis.
    // Duplicate-rule detection still runs (cheap, no automaton).
    // The automaton's worklist re-enumeration is `|states|^arity` per
    // round and dominates burgc runtime on large grammars, so routine
    // regenerations skip it. Opt back in with --coverage for the CI
    // gate.
    bool skip_dead_rule_analysis = true;

    // Optional ASDL schema. When present and a TERM's pattern arity
    // matches its constructor's node-field count, the position-demand
    // filter uses the constructor's node-typed field types; otherwise
    // it falls back to a heuristic derived from the rule set itself.
    const AsdlSchema* asdl = nullptr;
};

AnalysisReport run_completeness_analysis(const BurgAnalysis& a,
                                           const AnalysisConfig& cfg = {});

// Load an ASDL JSON sidecar (asdl --json output) into an AsdlSchema.
// Returns true on success; on failure populates `error` with a
// human-readable message.
bool load_asdl_schema(const std::string& path,
                       AsdlSchema& out,
                       std::string& error);
