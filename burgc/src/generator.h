#pragma once

#include "BurgAST.h"
#include "completeness.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <ostream>
#include <memory>

// ── Analysis types ────────────────────────────────────────

struct ChainRule {
    int64_t rule_number;
    int64_t cost;
    std::string from_nt;  // RHS nonterminal
    std::string to_nt;    // LHS nonterminal
    std::string guard;    // where-clause predicate (empty if none)
};

struct BurgAnalysis {
    burg_ast::Spec* spec = nullptr;
    std::unordered_map<std::string, int64_t> term_map;
    std::unordered_set<std::string> term_names;
    std::vector<std::string> nonterms;
    std::unordered_map<std::string, int64_t> nonterm_index;
    std::string start_nonterm;
    // Declared ENTRY nonterminals (see burg_ast::Spec::entries): additional
    // goals a consumer reduces to by name. Validated in analyze(); consumed by
    // the liveness analysis, which seeds demand for each exactly as for START.
    std::vector<std::string> entries;
    int64_t max_rule = 0;
    bool has_actions = false;
    bool has_guards = false;
    std::vector<ChainRule> chain_rules;
    std::unordered_map<std::string, std::vector<burg_ast::Rule*>> rules_by_nt;
};

// ── Frontend: parse + analyze ─────────────────────────────

class BurgGenerator {
public:
    burg_ast::Spec* parse(const std::string& input_file);
    bool analyze(burg_ast::Spec* spec);

    const BurgAnalysis& analysis() const { return analysis_; }
    const std::vector<std::string>& errors() const { return errors_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

    // Tunables for the unified completeness analysis. Today: just
    // the opt-in for coverage warnings; ASDL-aware filtering will
    // populate this when zany-painting-spark.md lands.
    void set_completeness_config(AnalysisConfig cfg) {
        completeness_cfg_ = std::move(cfg);
    }

    // Convenience: generate with default (C++) backend
    void generate(std::ostream& out, const std::string& frame_dir = "");

private:
    BurgAnalysis analysis_;
    std::vector<std::string> errors_;
    std::vector<std::string> warnings_;
    AnalysisConfig completeness_cfg_;

    void collect_nonterms();
    void classify_rules();
    void check_coverage();
};

// ── Backend base class ────────────────────────────────────

class BurgBackend {
public:
    virtual ~BurgBackend() = default;
    virtual void generate(std::ostream& out, const BurgAnalysis& a,
                          const std::string& frame_dir = "") = 0;
    // Two-file generation (C backend overrides this)
    // hdr_name is the header filename for #include and guard derivation
    // frame_dir is the directory containing frame templates
    virtual void generate(std::ostream& hdr, std::ostream& impl,
                          const BurgAnalysis& a, const std::string& hdr_name,
                          const std::string& frame_dir) {
        generate(hdr, a);
        (void)impl; (void)hdr_name; (void)frame_dir;
    }
    virtual bool needs_impl_file() const { return false; }

    // Opt-in: also export the labeled rule set (pattern, cost, guard) so a
    // consumer can check the labeler's chosen cover against its own search over
    // the same rules. Off by default — it is dead weight for a normal consumer,
    // and it is the only thing in the output that no other code path reads.
    void set_emit_rule_table(bool on) { emit_rule_table_ = on; }

protected:
    const BurgAnalysis* a_ = nullptr;
    bool emit_rule_table_ = false;

    // Backend customization — override to thread context parameter
    virtual std::string ctx_arg() const { return ""; }    // appended: ", ctx"
    virtual std::string ctx_solo() const { return ""; }   // standalone: "ctx"
    // Prefix on user-facing symbol names (the C backend's COMPILER namespace).
    virtual std::string sym_prefix() const { return ""; }
    virtual std::string ctx_type_name() const { return "burg_ctx_t"; }
    virtual std::string state_type() const { return "BurgState"; }  // struct name
    // Whether the backend stores errors and the consumer polls
    // burg_has_error() to detect them. The C++ backend overrides to
    // false because burg_set_error() throws; the polling checks would
    // be dead code.
    virtual bool needs_error_polling() const { return true; }

    // Terminal/nonterminal constant definitions (constexpr for C++, #define for C)
    void emit_defines(std::ostream& out);
    void emit_user_headers(std::ostream& out);
    void emit_macros_check(std::ostream& out);
    void emit_state_struct(std::ostream& out, int indent);

    // Shared function bodies (emitted between backend-provided signatures)
    void emit_closures(std::ostream& out, int indent,
                       const std::string& prefix, bool fwd_decls);
    void emit_dp_body(std::ostream& out, int indent);
    void emit_label_alloc(std::ostream& out, int indent);
    void emit_label_tree_body(std::ostream& out, int indent);
    virtual void emit_label_body(std::ostream& out, int indent);
    void emit_rule_func_body(std::ostream& out, int indent);
    void emit_cost_func_body(std::ostream& out, int indent);
    void emit_label_root_body(std::ostream& out, int indent);
    void emit_nt_name_body(std::ostream& out, int indent);

    // --emit-rule-table. The C backend splits these across its .h/.c (types and
    // extern declarations in the header, data in the impl); the C++ backend emits
    // both inside the class, where `storage` makes the arrays inline members.
    void emit_rule_table_types(std::ostream& out, int indent);
    void emit_rule_table_decls(std::ostream& out);
    void emit_rule_table_data(std::ostream& out, int indent, const std::string& storage);
    void emit_rule_guard_body(std::ostream& out, int indent);
    void emit_reduce_body(std::ostream& out, int indent);
    virtual void emit_rewrite_body(std::ostream& out, int indent);

    // Sub-helpers
    void emit_label_dp_switch(std::ostream& out, int indent);
    virtual void emit_rpo_dfs(std::ostream& out, int indent);
    void emit_child_reductions(std::ostream& out,
                               burg_ast::TreePattern* pat, int indent);

    // Pattern analysis helpers
    void emit_pattern_guards(burg_ast::TreePattern* pat,
                             const std::string& state_var,
                             std::string& guard);
    std::string pattern_guard(burg_ast::TreePattern* pat);
    std::string pattern_cost_expr(burg_ast::TreePattern* pat);
    bool is_terminal(const std::string& name) const;

    // Utilities
    static std::string trim(const std::string& s);
    static void pad(std::ostream& out, int indent);
public:
    static std::string pattern_string(burg_ast::TreePattern* pat);
};

// ── Backend factories ─────────────────────────────────────

std::unique_ptr<BurgBackend> create_cpp_backend();
std::unique_ptr<BurgBackend> create_c_backend();
