#pragma once

#include "BurgAST.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <ostream>

class BurgGenerator {
public:
    // Parse a .burg file into an AST
    burg_ast::Spec* parse(const std::string& input_file);

    // Analyze the spec: collect terminals/nonterminals, validate, compute closures
    bool analyze(burg_ast::Spec* spec);

    // Generate graph-aware matcher code to output stream
    void generate(std::ostream& out);

    // Access errors
    const std::vector<std::string>& errors() const { return errors_; }

private:
    burg_ast::Spec* spec_ = nullptr;

    // Collected from analysis
    std::unordered_map<std::string, int64_t> term_map_;     // name → symbol number
    std::unordered_set<std::string> term_names_;
    std::vector<std::string> nonterms_;                      // ordered list
    std::unordered_map<std::string, int64_t> nonterm_index_; // name → NT index (1-based)
    std::string start_nonterm_;
    int64_t max_rule_ = 0;
    bool has_actions_ = false;
    bool has_guards_ = false;

    // Chain rules: rules where pattern is just a nonterminal
    struct ChainRule {
        int64_t rule_number;
        int64_t cost;
        std::string from_nt;  // RHS nonterminal
        std::string to_nt;    // LHS nonterminal
    };
    std::vector<ChainRule> chain_rules_;

    // Per-nonterminal: which rules define it, for decode tables
    std::unordered_map<std::string, std::vector<burg_ast::Rule*>> rules_by_nt_;

    std::vector<std::string> errors_;

    // Analysis helpers
    bool is_terminal(const std::string& name) const;
    void collect_nonterms();
    void classify_rules();

    // Code generation helpers
    void emit_header(std::ostream& out);
    void emit_user_headers(std::ostream& out);
    void emit_macros_check(std::ostream& out);
    void emit_state_struct(std::ostream& out);
    void emit_cache(std::ostream& out);
    void emit_closures(std::ostream& out);
    void emit_label_func(std::ostream& out);
    void emit_rule_func(std::ostream& out);
    void emit_nt_name(std::ostream& out);
    void emit_reduce_func(std::ostream& out);
    void emit_rewrite_func(std::ostream& out);

    // Pattern matching code generation (indexed children)
    std::string pattern_cost_expr(burg_ast::TreePattern* pat);
    std::string pattern_guard(burg_ast::TreePattern* pat);
    int pattern_min_arity(burg_ast::TreePattern* pat);

    // Reducer helpers
    void emit_child_reductions(std::ostream& out, burg_ast::TreePattern* pat,
                               int indent);
};
