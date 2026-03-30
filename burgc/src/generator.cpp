#include "generator.h"
#include "Parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cassert>

// ── Parsing ────────────────────────────────────────────────

burg_ast::Spec* BurgGenerator::parse(const std::string& input_file) {
    std::ifstream ifs(input_file);
    if (!ifs) {
        errors_.push_back("cannot open file: " + input_file);
        return nullptr;
    }
    std::string src((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());

    Parser parser;
    parser.init(src.data(), (int)src.size());
    if (!parser.parse() || !parser.ast) {
        auto f = parser.furthest();
        errors_.push_back("parse failed at line " + std::to_string(f.line) +
                          " col " + std::to_string(f.col));
        return nullptr;
    }
    return parser.ast;
}

// ── Analysis ───────────────────────────────────────────────

bool BurgGenerator::is_terminal(const std::string& name) const {
    return term_names_.count(name) > 0;
}

void BurgGenerator::collect_nonterms() {
    std::unordered_set<std::string> seen;
    for (auto* rule : spec_->rules) {
        if (!seen.count(rule->nonterm)) {
            seen.insert(rule->nonterm);
            nonterms_.push_back(rule->nonterm);
        }
    }
    // Assign 1-based indices (convention: start symbol is 1)
    if (!spec_->start.empty()) {
        auto it = std::find(nonterms_.begin(), nonterms_.end(), spec_->start);
        if (it != nonterms_.end() && it != nonterms_.begin()) {
            std::rotate(nonterms_.begin(), it, it + 1);
        }
        start_nonterm_ = spec_->start;
    } else if (!nonterms_.empty()) {
        start_nonterm_ = nonterms_[0];
    }
    for (size_t i = 0; i < nonterms_.size(); i++) {
        nonterm_index_[nonterms_[i]] = (int64_t)(i + 1);
    }
}

void BurgGenerator::classify_rules() {
    for (auto* rule : spec_->rules) {
        rules_by_nt_[rule->nonterm].push_back(rule);
        if (rule->rule_number > max_rule_)
            max_rule_ = rule->rule_number;

        if (rule->pattern->is_leaf() && !is_terminal(rule->pattern->name)) {
            chain_rules_.push_back({
                rule->rule_number, rule->cost,
                rule->pattern->name, rule->nonterm
            });
        }
    }
}

bool BurgGenerator::analyze(burg_ast::Spec* spec) {
    spec_ = spec;
    errors_.clear();

    for (auto* t : spec->terminals) {
        if (term_names_.count(t->name)) {
            errors_.push_back("duplicate terminal: " + t->name);
        }
        term_names_.insert(t->name);
        term_map_[t->name] = t->number;
    }

    collect_nonterms();
    classify_rules();

    has_actions_ = false;
    has_guards_ = false;
    for (auto* rule : spec->rules) {
        if (!rule->action.empty()) has_actions_ = true;
        if (!rule->guard.empty()) has_guards_ = true;
    }

    // Validate: pattern roots of non-chain rules must be terminals
    for (auto* rule : spec->rules) {
        if (!rule->pattern->is_leaf() && !is_terminal(rule->pattern->name)) {
            errors_.push_back("rule " + std::to_string(rule->rule_number) +
                              ": pattern root '" + rule->pattern->name +
                              "' is not a declared terminal");
        }
        for (auto* child : rule->pattern->children) {
            if (!child->is_leaf()) {
                if (!is_terminal(child->name)) {
                    errors_.push_back("rule " + std::to_string(rule->rule_number) +
                                      ": nested pattern operator '" + child->name +
                                      "' is not a terminal");
                }
            }
        }
    }

    if (nonterms_.empty()) {
        errors_.push_back("no rules defined");
    }

    return errors_.empty();
}

// ── Helpers ────────────────────────────────────────────────

static void pad(std::ostream& out, int indent) {
    for (int i = 0; i < indent; i++) out << "    ";
}

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string pattern_string(burg_ast::TreePattern* pat) {
    std::string s = pat->name;
    if (!pat->children.empty()) {
        s += "(";
        for (size_t i = 0; i < pat->children.size(); i++) {
            if (i > 0) s += ", ";
            s += pattern_string(pat->children[i]);
        }
        s += ")";
    }
    return s;
}

// ── Code Generation ────────────────────────────────────────

void BurgGenerator::generate(std::ostream& out) {
    emit_header(out);
    emit_user_headers(out);
    emit_macros_check(out);
    emit_state_struct(out);
    emit_cache(out);
    emit_closures(out);
    emit_label_func(out);
    emit_rule_func(out);
    emit_nt_name(out);
    if (has_actions_)
        emit_reduce_func(out);
    emit_rewrite_func(out);
}

void BurgGenerator::emit_header(std::ostream& out) {
    out << "// Generated by burgc — do not edit\n";
    out << "#pragma once\n\n";
    out << "#include <cstdint>\n";
    out << "#include <cstdlib>\n";
    out << "#include <climits>\n";
    out << "#include <unordered_map>\n";
    out << "#include <unordered_set>\n";
    out << "#include <vector>\n";
    out << "#include <algorithm>\n\n";

    out << "// ── Terminal symbols ──\n\n";
    for (auto* t : spec_->terminals) {
        out << "constexpr int BURG_" << t->name << " = " << t->number << ";\n";
    }

    out << "\n// ── Nonterminal indices ──\n\n";
    for (size_t i = 0; i < nonterms_.size(); i++) {
        out << "constexpr int " << nonterms_[i] << "_NT = " << (i + 1) << ";\n";
    }
    out << "constexpr int BURG_MAX_NT = " << nonterms_.size() << ";\n";
    out << "constexpr int BURG_MAX_COST = SHRT_MAX;\n\n";
}

void BurgGenerator::emit_user_headers(std::ostream& out) {
    if (!spec_->headers.empty()) {
        out << "// ── User headers ──\n\n";
        for (auto& h : spec_->headers) {
            std::string trimmed = trim(h);
            if (!trimmed.empty())
                out << trimmed << "\n";
        }
        out << "\n";
    }
}

void BurgGenerator::emit_macros_check(std::ostream& out) {
    out << "// ── Node access macros (must be defined by consumer) ──\n\n";
    const char* macros[] = {
        "BURG_NODE_TYPE",
        "BURG_NODE_OP",
        "BURG_NODE_ARITY",
        "BURG_NODE_CHILD",
        "BURG_NODE_ID",
        "BURG_NODE_SUCC_COUNT",
        "BURG_NODE_SUCC",
    };
    for (auto* m : macros) {
        out << "#ifndef " << m << "\n";
        out << "#error \"Define " << m << " before including this header\"\n";
        out << "#endif\n";
    }
    out << "\n";
}

void BurgGenerator::emit_state_struct(std::ostream& out) {
    out << "// ── State record ──\n\n";
    out << "struct BurgState {\n";
    out << "    int op;\n";
    out << "    BurgState** children;\n";
    out << "    int child_count;\n";
    out << "    short cost[" << (nonterms_.size() + 1) << "];\n";
    out << "    short rule[" << (nonterms_.size() + 1) << "];\n";
    out << "};\n\n";
}

void BurgGenerator::emit_cache(std::ostream& out) {
    out << "// ── State cache ──\n\n";
    out << "static std::unordered_map<void*, BurgState*> burg_state_cache;\n\n";
    out << "static BurgState* burg_cache_lookup(void* id) {\n";
    out << "    auto it = burg_state_cache.find(id);\n";
    out << "    return (it != burg_state_cache.end()) ? it->second : nullptr;\n";
    out << "}\n\n";
    out << "static void burg_cache_store(void* id, BurgState* state) {\n";
    out << "    burg_state_cache[id] = state;\n";
    out << "}\n\n";
    out << "static void burg_cache_clear() {\n";
    out << "    burg_state_cache.clear();\n";
    out << "}\n\n";
}

void BurgGenerator::emit_closures(std::ostream& out) {
    std::unordered_map<std::string, std::vector<ChainRule*>> chains_to;
    for (auto& cr : chain_rules_) {
        chains_to[cr.from_nt].push_back(&cr);
    }

    // Forward declarations
    for (auto& [nt, _] : chains_to) {
        out << "static void closure_" << nt << "(BurgState* p, int c);\n";
    }
    if (!chains_to.empty()) out << "\n";

    for (auto& [nt, chains] : chains_to) {
        out << "static void closure_" << nt << "(BurgState* p, int c) {\n";
        for (auto* cr : chains) {
            int64_t nt_idx = nonterm_index_[cr->to_nt];
            out << "    if (c + " << cr->cost << " < p->cost[" << nt_idx << "]) {\n";
            out << "        p->cost[" << nt_idx << "] = c + " << cr->cost << ";\n";
            out << "        p->rule[" << nt_idx << "] = " << cr->rule_number << ";\n";
            if (chains_to.count(cr->to_nt)) {
                out << "        closure_" << cr->to_nt << "(p, c + " << cr->cost << ");\n";
            }
            out << "    }\n";
        }
        out << "}\n\n";
    }
}

// ── Pattern matching helpers (indexed children) ────────────

// Minimum arity required for a pattern to match
int BurgGenerator::pattern_min_arity(burg_ast::TreePattern* pat) {
    return (int)pat->children.size();
}

// Generate the structural guard for a pattern match.
// Uses p->children[i] instead of p->left/p->right.
std::string BurgGenerator::pattern_guard(burg_ast::TreePattern* pat) {
    std::string guard;
    int arity = (int)pat->children.size();

    // Arity check
    if (arity > 0) {
        guard = "p->child_count >= " + std::to_string(arity);
    }

    for (int i = 0; i < arity; i++) {
        auto* child = pat->children[i];
        std::string child_var = "p->children[" + std::to_string(i) + "]";

        if (child->is_leaf() && !is_terminal(child->name)) {
            // Nonterminal leaf: check child matched this NT
            int64_t nt_idx = nonterm_index_[child->name];
            if (!guard.empty()) guard += " && ";
            guard += child_var + "->rule[" + std::to_string(nt_idx) + "]";
        } else if (child->is_leaf() && is_terminal(child->name)) {
            // Terminal leaf: check child's op
            if (!guard.empty()) guard += " && ";
            guard += child_var + "->op == BURG_" + child->name;
        } else if (!child->is_leaf()) {
            // Nested terminal pattern: check child's op + grandchildren
            if (!guard.empty()) guard += " && ";
            guard += child_var + "->op == BURG_" + child->name;
            for (size_t j = 0; j < child->children.size(); j++) {
                auto* grandchild = child->children[j];
                std::string gc_var = child_var + "->children[" + std::to_string(j) + "]";
                if (grandchild->is_leaf() && !is_terminal(grandchild->name)) {
                    int64_t nt_idx = nonterm_index_[grandchild->name];
                    guard += " && " + gc_var + "->rule[" + std::to_string(nt_idx) + "]";
                }
            }
        }
    }
    return guard;
}

// Generate the cost expression summing child nonterminal costs.
std::string BurgGenerator::pattern_cost_expr(burg_ast::TreePattern* pat) {
    std::string cost_parts;
    for (size_t i = 0; i < pat->children.size(); i++) {
        auto* child = pat->children[i];
        std::string child_var = "p->children[" + std::to_string(i) + "]";

        if (child->is_leaf() && !is_terminal(child->name)) {
            int64_t nt_idx = nonterm_index_[child->name];
            if (!cost_parts.empty()) cost_parts += " + ";
            cost_parts += child_var + "->cost[" + std::to_string(nt_idx) + "]";
        } else if (!child->is_leaf()) {
            for (size_t j = 0; j < child->children.size(); j++) {
                auto* grandchild = child->children[j];
                std::string gc_var = child_var + "->children[" + std::to_string(j) + "]";
                if (grandchild->is_leaf() && !is_terminal(grandchild->name)) {
                    int64_t nt_idx = nonterm_index_[grandchild->name];
                    if (!cost_parts.empty()) cost_parts += " + ";
                    cost_parts += gc_var + "->cost[" + std::to_string(nt_idx) + "]";
                }
            }
        }
    }
    if (cost_parts.empty()) cost_parts = "0";
    return cost_parts;
}

// ── Label function (graph-aware, macro-based, cached) ──────

void BurgGenerator::emit_label_func(std::ostream& out) {
    // Find chain rules targeting each nonterminal
    std::unordered_map<std::string, std::vector<ChainRule*>> chains_to;
    for (auto& cr : chain_rules_) {
        chains_to[cr.from_nt].push_back(&cr);
    }

    out << "// ── Label function (bottom-up, cached) ──\n\n";
    out << "static BurgState* burg_label(BURG_NODE_TYPE node) {\n";
    out << "    void* id = BURG_NODE_ID(node);\n";
    out << "    BurgState* cached = burg_cache_lookup(id);\n";
    out << "    if (cached) return cached;\n\n";

    out << "    int op = BURG_NODE_OP(node);\n";
    out << "    int arity = BURG_NODE_ARITY(node);\n\n";

    out << "    auto* p = (BurgState*)malloc(sizeof(BurgState));\n";
    out << "    p->op = op;\n";
    out << "    p->child_count = arity;\n";
    out << "    p->children = arity > 0\n";
    out << "        ? (BurgState**)malloc(arity * sizeof(BurgState*))\n";
    out << "        : nullptr;\n";
    out << "    for (int i = 0; i <= BURG_MAX_NT; i++) {\n";
    out << "        p->cost[i] = BURG_MAX_COST;\n";
    out << "        p->rule[i] = 0;\n";
    out << "    }\n\n";

    out << "    // Cache BEFORE DP (back-edge cut-point safety)\n";
    out << "    burg_cache_store(id, p);\n\n";

    out << "    // Label children recursively (cache handles cycles)\n";
    out << "    for (int i = 0; i < arity; i++)\n";
    out << "        p->children[i] = burg_label(BURG_NODE_CHILD(node, i));\n\n";

    out << "    // ── Cost DP ──\n";
    out << "    switch (op) {\n";

    // Group non-chain rules by terminal
    std::unordered_map<std::string, std::vector<burg_ast::Rule*>> rules_by_term;
    for (auto* rule : spec_->rules) {
        if (rule->pattern->is_leaf() && !is_terminal(rule->pattern->name))
            continue; // Skip chain rules
        rules_by_term[rule->pattern->name].push_back(rule);
    }

    for (auto& [term, rules] : rules_by_term) {
        out << "    case BURG_" << term << ":\n";

        for (auto* rule : rules) {
            int64_t nt_idx = nonterm_index_[rule->nonterm];
            std::string guard = pattern_guard(rule->pattern);
            std::string cost_expr = pattern_cost_expr(rule->pattern);
            std::string trimmed_guard = trim(rule->guard);

            if (guard.empty() && trimmed_guard.empty()) {
                // Leaf terminal, no where guard — always matches
                out << "        {\n";
                out << "            int c = " << cost_expr << " + " << rule->cost << ";\n";
                out << "            if (c < p->cost[" << nt_idx << "]) {\n";
                out << "                p->cost[" << nt_idx << "] = c;\n";
                out << "                p->rule[" << nt_idx << "] = " << rule->rule_number << ";\n";
                if (chains_to.count(rule->nonterm))
                    out << "                closure_" << rule->nonterm << "(p, c);\n";
                out << "            }\n";
                out << "        }\n";
            } else {
                // Build combined guard: structural + where predicate
                std::string full_guard = guard;
                if (!trimmed_guard.empty()) {
                    if (!full_guard.empty()) full_guard += " && ";
                    full_guard += "(" + trimmed_guard + ")";
                }
                out << "        if (" << full_guard << ") {\n";
                out << "            int c = " << cost_expr << " + " << rule->cost << ";\n";
                out << "            if (c < p->cost[" << nt_idx << "]) {\n";
                out << "                p->cost[" << nt_idx << "] = c;\n";
                out << "                p->rule[" << nt_idx << "] = " << rule->rule_number << ";\n";
                if (chains_to.count(rule->nonterm))
                    out << "                closure_" << rule->nonterm << "(p, c);\n";
                out << "            }\n";
                out << "        }\n";
            }
        }
        out << "        break;\n";
    }

    out << "    }\n";
    out << "    return p;\n";
    out << "}\n\n";
}

void BurgGenerator::emit_rule_func(std::ostream& out) {
    out << "// ── Rule lookup ──\n\n";
    out << "static int burg_rule(BurgState* state, int goalnt) {\n";
    out << "    if (!state || goalnt < 1 || goalnt > BURG_MAX_NT) return 0;\n";
    out << "    return state->rule[goalnt];\n";
    out << "}\n\n";
}

void BurgGenerator::emit_nt_name(std::ostream& out) {
    out << "// ── Nonterminal names (for debugging) ──\n\n";
    out << "static const char* burg_nt_name(int nt) {\n";
    out << "    static const char* names[] = {\n";
    out << "        \"<invalid>\",\n";
    for (auto& nt : nonterms_) {
        out << "        \"" << nt << "\",\n";
    }
    out << "    };\n";
    out << "    if (nt >= 1 && nt <= BURG_MAX_NT) return names[nt];\n";
    out << "    return names[0];\n";
    out << "}\n";
}

// ── Reducer (graph-aware: receives both node and state) ────

// Emit child reductions for a pattern using indexed children.
void BurgGenerator::emit_child_reductions(std::ostream& out,
                                           burg_ast::TreePattern* pat,
                                           int indent) {
    for (size_t i = 0; i < pat->children.size(); i++) {
        auto* child = pat->children[i];
        std::string idx = std::to_string(i);

        if (child->is_leaf() && !is_terminal(child->name)) {
            int64_t nt_idx = nonterm_index_[child->name];
            pad(out, indent);
            out << "burg_reduce(BURG_NODE_CHILD(node, " << idx << "), "
                << "state->children[" << idx << "], " << nt_idx << ");\n";
        } else if (!child->is_leaf()) {
            // Nested terminal — recurse into grandchildren
            for (size_t j = 0; j < child->children.size(); j++) {
                auto* grandchild = child->children[j];
                if (grandchild->is_leaf() && !is_terminal(grandchild->name)) {
                    int64_t nt_idx = nonterm_index_[grandchild->name];
                    pad(out, indent);
                    out << "burg_reduce(BURG_NODE_CHILD(BURG_NODE_CHILD(node, " << idx
                        << "), " << j << "), state->children[" << idx
                        << "]->children[" << j << "], " << nt_idx << ");\n";
                }
            }
        }
    }
}

void BurgGenerator::emit_reduce_func(std::ostream& out) {
    out << "\n// ── Reducer (top-down pass) ──\n\n";
    out << "static void burg_reduce(BURG_NODE_TYPE node, BurgState* state, int goalnt) {\n";
    out << "    int rule = burg_rule(state, goalnt);\n";
    out << "    switch (rule) {\n";

    for (auto* rule : spec_->rules) {
        out << "    case " << rule->rule_number << ": { // "
            << rule->nonterm << ": " << pattern_string(rule->pattern) << "\n";

        if (rule->pattern->is_leaf() && !is_terminal(rule->pattern->name)) {
            // Chain rule: reduce same node for the source nonterminal
            int64_t src_idx = nonterm_index_[rule->pattern->name];
            out << "        burg_reduce(node, state, " << src_idx << ");\n";
        } else {
            emit_child_reductions(out, rule->pattern, 2);
        }

        if (!rule->action.empty()) {
            std::string trimmed = trim(rule->action);
            if (!trimmed.empty())
                out << "        " << trimmed << "\n";
        }

        out << "        break;\n";
        out << "    }\n";
    }

    out << "    }\n";
    out << "}\n";
}

// ── Graph rewriter (walks successor edges, labels + reduces everything) ──

void BurgGenerator::emit_rewrite_func(std::ostream& out) {
    int64_t start_idx = nonterm_index_[start_nonterm_];

    out << "\n// ── Graph rewriter (RPO walk over successor edges) ──\n\n";

    out << "static void burg_rewrite(BURG_NODE_TYPE root) {\n";
    out << "    burg_cache_clear();\n\n";

    // Iterative DFS to compute reverse postorder
    out << "    // Compute reverse postorder via iterative DFS\n";
    out << "    std::vector<BURG_NODE_TYPE> rpo;\n";
    out << "    {\n";
    out << "        std::unordered_set<void*> visited;\n";
    out << "        struct Frame { BURG_NODE_TYPE node; int succ; };\n";
    out << "        std::vector<Frame> stack;\n";
    out << "        stack.push_back({root, 0});\n";
    out << "        visited.insert(BURG_NODE_ID(root));\n\n";

    out << "        while (!stack.empty()) {\n";
    out << "            auto& f = stack.back();\n";
    out << "            int sc = BURG_NODE_SUCC_COUNT(f.node);\n";
    out << "            if (f.succ < sc) {\n";
    out << "                BURG_NODE_TYPE s = BURG_NODE_SUCC(f.node, f.succ);\n";
    out << "                f.succ++;\n";
    out << "                if (s && visited.insert(BURG_NODE_ID(s)).second)\n";
    out << "                    stack.push_back({s, 0});\n";
    out << "            } else {\n";
    out << "                rpo.push_back(f.node);\n";
    out << "                stack.pop_back();\n";
    out << "            }\n";
    out << "        }\n";
    out << "        // Reverse: postorder → reverse postorder\n";
    out << "        std::reverse(rpo.begin(), rpo.end());\n";
    out << "    }\n\n";

    // Label all nodes (data-flow children resolved recursively via cache)
    out << "    // Label every node (data-flow children labeled recursively)\n";
    out << "    for (auto* n : rpo)\n";
    out << "        burg_label(n);\n\n";

    // Reduce all nodes
    if (has_actions_) {
        out << "    // Reduce every node with the start nonterminal\n";
        out << "    for (auto* n : rpo) {\n";
        out << "        BurgState* s = burg_cache_lookup(BURG_NODE_ID(n));\n";
        out << "        if (s && s->rule[" << start_idx << "])\n";
        out << "            burg_reduce(n, s, " << start_idx << ");\n";
        out << "    }\n";
    }

    out << "}\n";
}
