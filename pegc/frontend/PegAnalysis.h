#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <optional>

#include "GrammarAST.h"

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

    // Is the first element of this production "testable"?
    // (i.e., can we use head-fail optimization)
    bool first_testable = false;

    // Dependencies: which productions does this one call?
    std::set<std::string> calls;

    // Is this production reachable from the start production?
    bool reachable = true;
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

    bool ok() const { return errors.empty(); }
};

// ── PEG Analysis ────────────────────────────────────────────

class PegAnalysis {
public:
    AnalysisResult analyze(PegGrammar::Grammar* grammar);

private:
    // Phase 1: Collect productions, build call graph
    void collect_productions(PegGrammar::Grammar* grammar);
    void collect_calls(PegGrammar::PegExpr* expr, std::set<std::string>& calls);

    // Phase 2: Left recursion detection
    void detect_left_recursion();
    bool has_left_path(const std::string& from, const std::string& to,
                       std::set<std::string>& visited);
    PegGrammar::PegExpr* first_element(PegGrammar::PegExpr* expr);

    // Phase 3: FIRST set computation
    void compute_first_sets();
    std::vector<FirstElement> first_of(PegGrammar::PegExpr* expr,
                                       std::set<std::string>& expanding);

    // Phase 4: Testable classification + SPAN candidates
    void classify_alternatives(PegGrammar::Grammar* grammar);
    AlternativeInfo analyze_alternative(PegGrammar::PegExpr* expr);

    // Phase 5: Topological sort (for forward declarations)
    void topological_sort();

    // Phase 6: Unreachable production detection
    void detect_unreachable(PegGrammar::Grammar* grammar);
    void mark_reachable(const std::string& name, std::set<std::string>& visited);

    // Charset/token name sets for classification
    std::set<std::string> charset_names_;
    std::set<std::string> token_names_;

    AnalysisResult result_;
};

} // namespace pegc
