#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <optional>

#include "GrammarAST.h"
#include "Worklist.h"

namespace pegc {

// ── First-set element ───────────────────────────────────────
// What a PEG expression can start with.

struct FirstElement {
    enum Kind { Literal, Charset, Token, Any, Epsilon };
    Kind kind;
    std::string value;  // literal string, charset name, or token name
};

// ── Per-production analysis metadata ────────────────────────

struct ProductionInfo {
    std::string name;
    PegGrammar::Production* node = nullptr;

    // FIRST set: what this production can start with
    std::vector<FirstElement> first_set;

    // ε ∈ FIRST: production matches the empty string.
    bool nullable = false;

    // FOLLOW set: terminals that can follow this production in any
    // START-rooted derivation. EOF is encoded as a literal "$".
    std::vector<FirstElement> follow_set;

    // Is the first element of this production "testable"?
    // (i.e., can we use head-fail optimization)
    bool first_testable = false;

    // Dependencies: which productions does this one call?
    std::set<std::string> calls;

    // Is this production reachable from the start production?
    bool reachable = true;

    // Did left-recursion detection mark this production?
    // FIRST/FOLLOW Kildall passes skip these so undefined values
    // don't propagate to callers.
    bool left_recursive = false;
};

// ── Per-choice alternative analysis ─────────────────────────

struct AlternativeInfo {
    // First element of this alternative
    std::optional<FirstElement> first;

    // Can we apply head-fail optimization?
    bool testable = false;

    // Is this a SPAN candidate? (Star(charset))
    bool span_candidate = false;
    std::string span_charset;  // charset name if span_candidate
};

// ── Analysis result ─────────────────────────────────────────

struct AnalysisResult {
    std::map<std::string, ProductionInfo> productions;
    std::vector<std::string> topo_order;  // topological order for forward decls
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    // Per-Choice FIRST sets, indexed by alternative position in the
    // Choice's `alternatives` vector. Populated by classify_alternatives
    // and consumed by detect_masked_alternatives.
    std::map<PegGrammar::Choice*, std::vector<std::vector<FirstElement>>>
        alternative_firsts;

    bool ok() const { return errors.empty(); }
};

// ── Analysis configuration ──────────────────────────────────

struct PegAnalysisConfig {
    // Enable shape-comparison checks that produce informational
    // warnings about ordered-choice masking and Star/Plus + suffix
    // conflicts. Off by default — real PEG grammars often share
    // alternative prefixes by design (PEG backtracks if alt 0
    // partially fails) and the warnings are noisy without further
    // grammar annotation.
    bool emit_coverage_warnings = false;
};

// ── PEG Analysis ────────────────────────────────────────────

class PegAnalysis {
public:
    AnalysisResult analyze(PegGrammar::Grammar* grammar,
                            const PegAnalysisConfig& cfg = {});

private:
    // Phase 1: Collect productions, build call graph
    void collect_productions(PegGrammar::Grammar* grammar);
    void collect_calls(PegGrammar::PegExpr* expr, std::set<std::string>& calls);

    // Phase 2: Left recursion detection
    void detect_left_recursion();
    bool has_left_path(const std::string& from, const std::string& to,
                       std::set<std::string>& visited);
    PegGrammar::PegExpr* first_element(PegGrammar::PegExpr* expr);

    // Reverse of `calls`: who calls each production. Built once after
    // collect_productions and reused by both Kildall passes.
    void build_callers_graph();
    std::map<std::string, std::set<std::string>> callers_;

    // Phase 3: FIRST + nullable as a Kildall worklist fixed-point.
    // Reads/writes through info.first_set / info.nullable; no
    // cross-production recursion (table-based reads).
    void compute_first_sets();
    std::vector<FirstElement> first_of(PegGrammar::PegExpr* expr);
    bool nullable_of(PegGrammar::PegExpr* expr);

    // Phase 4: FOLLOW as a Kildall worklist fixed-point.
    void compute_follow_sets(PegGrammar::Grammar* grammar);
    void walk_for_follow(PegGrammar::PegExpr* expr,
                          ProductionInfo& owner,
                          const std::vector<FirstElement>& tail_first,
                          bbq::Worklist<std::string>& wl);

    // Phase 5: Testable classification + persist per-alternative FIRST.
    void classify_alternatives(PegGrammar::Grammar* grammar);
    AlternativeInfo analyze_alternative(PegGrammar::PegExpr* expr);

    // Phase 6+: PEG-specific structural checks that ride on FIRST/FOLLOW.
    void detect_masked_alternatives();
    void detect_star_suffix_conflict(PegGrammar::Grammar* grammar);
    void detect_always_failing();
    void detect_useless_predicates(PegGrammar::Grammar* grammar);
    void detect_unused_terminals(PegGrammar::Grammar* grammar);

    // Topological sort (for forward declarations)
    void topological_sort();

    // Unreachable production detection
    void detect_unreachable(PegGrammar::Grammar* grammar);
    void mark_reachable(const std::string& name, std::set<std::string>& visited);

    // Helpers shared across the new passes.
    static bool first_set_eq(const std::vector<FirstElement>& a,
                              const std::vector<FirstElement>& b);
    static void first_set_union(std::vector<FirstElement>& dest,
                                 const std::vector<FirstElement>& src,
                                 bool include_epsilon = true);
    static bool first_set_contains_epsilon(const std::vector<FirstElement>& s);
    static std::vector<FirstElement> first_set_without_epsilon(
        const std::vector<FirstElement>& s);
    static bool first_set_subsumes(const std::vector<FirstElement>& a,
                                    const std::vector<FirstElement>& b);
    static bool first_element_eq(const FirstElement& a, const FirstElement& b);

    // Walk an expression collecting all references it makes to
    // RuleCalls / TokenRefs / CharsetRefs (used for unused-terminal
    // detection).
    void collect_all_refs(PegGrammar::PegExpr* expr,
                           std::set<std::string>& tokens,
                           std::set<std::string>& charsets,
                           std::set<std::string>& rules);

    // Charset/token name sets for classification
    std::set<std::string> charset_names_;
    std::set<std::string> token_names_;

    // Built-in token-matching methods on the pegc runtime cursor
    // (PegRuntime.h). RuleCalls to these are legal even though they
    // aren't declared in the grammar; calls to anything else are
    // typos and produce an error.
    static bool is_builtin(const std::string& name);

    AnalysisResult result_;
};

} // namespace pegc
