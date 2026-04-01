#include "generator.h"
#include "Parser.h"
#include <fstream>
#include <algorithm>
#include <cstring>

// ══════════════════════════════════════════════════════════
// BurgGenerator — frontend (parse + analyze)
// ══════════════════════════════════════════════════════════

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

void BurgGenerator::collect_nonterms() {
    auto& a = analysis_;
    std::unordered_set<std::string> seen;
    for (auto* rule : a.spec->rules) {
        // Collect LHS nonterminals
        if (!seen.count(rule->nonterm)) {
            seen.insert(rule->nonterm);
            a.nonterms.push_back(rule->nonterm);
        }
        // Collect chain rule source nonterminals (leaf patterns that aren't terminals)
        if (rule->pattern->is_leaf() && !a.term_names.count(rule->pattern->name)) {
            if (!seen.count(rule->pattern->name)) {
                seen.insert(rule->pattern->name);
                a.nonterms.push_back(rule->pattern->name);
            }
        }
    }
    if (!a.spec->start.empty()) {
        auto it = std::find(a.nonterms.begin(), a.nonterms.end(), a.spec->start);
        if (it != a.nonterms.end() && it != a.nonterms.begin())
            std::rotate(a.nonterms.begin(), it, it + 1);
        a.start_nonterm = a.spec->start;
    } else if (!a.nonterms.empty()) {
        a.start_nonterm = a.nonterms[0];
    }
    for (size_t i = 0; i < a.nonterms.size(); i++)
        a.nonterm_index[a.nonterms[i]] = (int64_t)(i + 1);
}

void BurgGenerator::classify_rules() {
    auto& a = analysis_;
    for (auto* rule : a.spec->rules) {
        a.rules_by_nt[rule->nonterm].push_back(rule);
        if (rule->rule_number > a.max_rule)
            a.max_rule = rule->rule_number;
        if (rule->pattern->is_leaf() && !a.term_names.count(rule->pattern->name)) {
            a.chain_rules.push_back({
                rule->rule_number, rule->cost,
                rule->pattern->name, rule->nonterm
            });
        }
    }
}

bool BurgGenerator::analyze(burg_ast::Spec* spec) {
    analysis_ = BurgAnalysis{};
    analysis_.spec = spec;
    errors_.clear();

    auto& a = analysis_;
    for (auto* t : spec->terminals) {
        if (a.term_names.count(t->name))
            errors_.push_back("duplicate terminal: " + t->name);
        a.term_names.insert(t->name);
        a.term_map[t->name] = t->number;
    }

    collect_nonterms();
    classify_rules();

    for (auto* rule : spec->rules) {
        if (!rule->action.empty()) a.has_actions = true;
        if (!rule->guard.empty()) a.has_guards = true;
    }

    for (auto* rule : spec->rules) {
        if (!rule->pattern->is_leaf() && !a.term_names.count(rule->pattern->name)) {
            errors_.push_back("rule " + std::to_string(rule->rule_number) +
                              ": pattern root '" + rule->pattern->name +
                              "' is not a declared terminal");
        }
        for (auto* child : rule->pattern->children) {
            if (!child->is_leaf() && !a.term_names.count(child->name)) {
                errors_.push_back("rule " + std::to_string(rule->rule_number) +
                                  ": nested pattern operator '" + child->name +
                                  "' is not a terminal");
            }
        }
    }

    if (a.nonterms.empty())
        errors_.push_back("no rules defined");

    return errors_.empty();
}

void BurgGenerator::generate(std::ostream& out) {
    auto backend = create_cpp_backend();
    backend->generate(out, analysis_);
}

// ══════════════════════════════════════════════════════════
// BurgBackend — shared helpers
// ══════════════════════════════════════════════════════════

void BurgBackend::pad(std::ostream& out, int indent) {
    for (int i = 0; i < indent; i++) out << "    ";
}

std::string BurgBackend::trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string BurgBackend::pattern_string(burg_ast::TreePattern* pat) {
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

bool BurgBackend::is_terminal(const std::string& name) const {
    return a_->term_names.count(name) > 0;
}

std::string BurgBackend::pattern_guard(burg_ast::TreePattern* pat) {
    std::string guard;
    int arity = (int)pat->children.size();
    if (arity > 0)
        guard = "p->child_count >= " + std::to_string(arity);

    for (int i = 0; i < arity; i++) {
        auto* child = pat->children[i];
        std::string cv = "p->children[" + std::to_string(i) + "]";
        if (child->is_leaf() && !is_terminal(child->name)) {
            int64_t idx = a_->nonterm_index.at(child->name);
            if (!guard.empty()) guard += " && ";
            guard += cv + "->rule[" + std::to_string(idx) + "]";
        } else if (child->is_leaf() && is_terminal(child->name)) {
            if (!guard.empty()) guard += " && ";
            guard += cv + "->op == BURG_" + child->name;
        } else if (!child->is_leaf()) {
            if (!guard.empty()) guard += " && ";
            guard += cv + "->op == BURG_" + child->name;
            for (size_t j = 0; j < child->children.size(); j++) {
                auto* gc = child->children[j];
                std::string gv = cv + "->children[" + std::to_string(j) + "]";
                if (gc->is_leaf() && !is_terminal(gc->name)) {
                    int64_t idx = a_->nonterm_index.at(gc->name);
                    guard += " && " + gv + "->rule[" + std::to_string(idx) + "]";
                }
            }
        }
    }
    return guard;
}

std::string BurgBackend::pattern_cost_expr(burg_ast::TreePattern* pat) {
    std::string parts;
    for (size_t i = 0; i < pat->children.size(); i++) {
        auto* child = pat->children[i];
        std::string cv = "p->children[" + std::to_string(i) + "]";
        if (child->is_leaf() && !is_terminal(child->name)) {
            int64_t idx = a_->nonterm_index.at(child->name);
            if (!parts.empty()) parts += " + ";
            parts += cv + "->cost[" + std::to_string(idx) + "]";
        } else if (!child->is_leaf()) {
            for (size_t j = 0; j < child->children.size(); j++) {
                auto* gc = child->children[j];
                std::string gv = cv + "->children[" + std::to_string(j) + "]";
                if (gc->is_leaf() && !is_terminal(gc->name)) {
                    int64_t idx = a_->nonterm_index.at(gc->name);
                    if (!parts.empty()) parts += " + ";
                    parts += gv + "->cost[" + std::to_string(idx) + "]";
                }
            }
        }
    }
    if (parts.empty()) parts = "0";
    return parts;
}

// ── Shared preamble emitters ──────────────────────────────

void BurgBackend::emit_constants(std::ostream& out) {
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
    for (auto* t : a_->spec->terminals)
        out << "constexpr int BURG_" << t->name << " = " << t->number << ";\n";

    out << "\n// ── Nonterminal indices ──\n\n";
    for (size_t i = 0; i < a_->nonterms.size(); i++)
        out << "constexpr int " << a_->nonterms[i] << "_NT = " << (i + 1) << ";\n";
    out << "constexpr int BURG_MAX_NT = " << a_->nonterms.size() << ";\n";
    out << "constexpr int BURG_MAX_COST = SHRT_MAX;\n\n";
}

void BurgBackend::emit_user_headers(std::ostream& out) {
    if (!a_->spec->headers.empty()) {
        out << "// ── User headers ──\n\n";
        for (auto& h : a_->spec->headers) {
            std::string trimmed = trim(h);
            if (!trimmed.empty()) out << trimmed << "\n";
        }
        out << "\n";
    }
}

void BurgBackend::emit_macros_check(std::ostream& out) {
    out << "// ── Node access macros (must be defined by consumer) ──\n\n";
    const char* macros[] = {
        "BURG_NODE_TYPE", "BURG_NODE_OP", "BURG_NODE_ARITY",
        "BURG_NODE_CHILD", "BURG_NODE_ID", "BURG_NODE_SUCC_COUNT",
        "BURG_NODE_SUCC",
    };
    for (auto* m : macros) {
        out << "#ifndef " << m << "\n";
        out << "#error \"Define " << m << " before including this header\"\n";
        out << "#endif\n";
    }
    out << "\n";
}

void BurgBackend::emit_state_struct(std::ostream& out, int indent) {
    pad(out, indent); out << "struct BurgState {\n";
    pad(out, indent); out << "    int op;\n";
    pad(out, indent); out << "    BurgState** children;\n";
    pad(out, indent); out << "    int child_count;\n";
    pad(out, indent); out << "    short cost[" << (a_->nonterms.size() + 1) << "];\n";
    pad(out, indent); out << "    short rule[" << (a_->nonterms.size() + 1) << "];\n";
    pad(out, indent); out << "};\n\n";
}

// ── Closures ──────────────────────────────────────────────

void BurgBackend::emit_closures(std::ostream& out, int indent,
                                 const std::string& prefix, bool fwd_decls) {
    std::unordered_map<std::string, std::vector<ChainRule*>> chains_to;
    for (auto& cr : const_cast<std::vector<ChainRule>&>(a_->chain_rules))
        chains_to[cr.from_nt].push_back(&cr);

    if (fwd_decls) {
        for (auto& [nt, _] : chains_to) {
            pad(out, indent);
            out << prefix << "void closure_" << nt << "(BurgState* p, int c);\n";
        }
        if (!chains_to.empty()) out << "\n";
    }

    for (auto& [nt, chains] : chains_to) {
        pad(out, indent);
        out << prefix << "void closure_" << nt << "(BurgState* p, int c) {\n";
        for (auto* cr : chains) {
            int64_t nt_idx = a_->nonterm_index.at(cr->to_nt);
            pad(out, indent + 1);
            out << "if (c + " << cr->cost << " < p->cost[" << nt_idx << "]) {\n";
            pad(out, indent + 2);
            out << "p->cost[" << nt_idx << "] = c + " << cr->cost << ";\n";
            pad(out, indent + 2);
            out << "p->rule[" << nt_idx << "] = " << cr->rule_number << ";\n";
            if (chains_to.count(cr->to_nt)) {
                pad(out, indent + 2);
                out << "closure_" << cr->to_nt << "(p, c + " << cr->cost << ");\n";
            }
            pad(out, indent + 1); out << "}\n";
        }
        pad(out, indent); out << "}\n\n";
    }
}

// ── Label body ────────────────────────────────────────────

void BurgBackend::emit_label_dp_switch(std::ostream& out, int indent) {
    std::unordered_map<std::string, std::vector<ChainRule*>> chains_to;
    for (auto& cr : const_cast<std::vector<ChainRule>&>(a_->chain_rules))
        chains_to[cr.from_nt].push_back(&cr);

    std::unordered_map<std::string, std::vector<burg_ast::Rule*>> rules_by_term;
    for (auto* rule : a_->spec->rules) {
        if (rule->pattern->is_leaf() && !is_terminal(rule->pattern->name))
            continue;
        rules_by_term[rule->pattern->name].push_back(rule);
    }

    pad(out, indent); out << "switch (op) {\n";
    for (auto& [term, rules] : rules_by_term) {
        pad(out, indent); out << "case BURG_" << term << ":\n";
        for (auto* rule : rules) {
            int64_t nt_idx = a_->nonterm_index.at(rule->nonterm);
            std::string guard = pattern_guard(rule->pattern);
            std::string cost_expr = pattern_cost_expr(rule->pattern);
            std::string trimmed_guard = trim(rule->guard);

            if (guard.empty() && trimmed_guard.empty()) {
                pad(out, indent + 1); out << "{\n";
                pad(out, indent + 2); out << "int c = " << cost_expr << " + " << rule->cost << ";\n";
                pad(out, indent + 2); out << "if (c < p->cost[" << nt_idx << "]) {\n";
                pad(out, indent + 3); out << "p->cost[" << nt_idx << "] = c;\n";
                pad(out, indent + 3); out << "p->rule[" << nt_idx << "] = " << rule->rule_number << ";\n";
                if (chains_to.count(rule->nonterm)) {
                    pad(out, indent + 3);
                    out << "closure_" << rule->nonterm << "(p, c);\n";
                }
                pad(out, indent + 2); out << "}\n";
                pad(out, indent + 1); out << "}\n";
            } else {
                std::string full_guard = guard;
                if (!trimmed_guard.empty()) {
                    if (!full_guard.empty()) full_guard += " && ";
                    full_guard += "(" + trimmed_guard + ")";
                }
                pad(out, indent + 1); out << "if (" << full_guard << ") {\n";
                pad(out, indent + 2); out << "int c = " << cost_expr << " + " << rule->cost << ";\n";
                pad(out, indent + 2); out << "if (c < p->cost[" << nt_idx << "]) {\n";
                pad(out, indent + 3); out << "p->cost[" << nt_idx << "] = c;\n";
                pad(out, indent + 3); out << "p->rule[" << nt_idx << "] = " << rule->rule_number << ";\n";
                if (chains_to.count(rule->nonterm)) {
                    pad(out, indent + 3);
                    out << "closure_" << rule->nonterm << "(p, c);\n";
                }
                pad(out, indent + 2); out << "}\n";
                pad(out, indent + 1); out << "}\n";
            }
        }
        pad(out, indent + 1); out << "break;\n";
    }
    pad(out, indent); out << "}\n";
}

// DP switch body — shared between tree and graph label functions
void BurgBackend::emit_dp_body(std::ostream& out, int indent) {
    pad(out, indent); out << "int op = p->op;\n";
    emit_label_dp_switch(out, indent);
}

// Arena alloc + BurgState init — shared between tree and graph label
void BurgBackend::emit_label_alloc(std::ostream& out, int indent) {
    pad(out, indent); out << "int arity = BURG_NODE_ARITY(node);\n";
    pad(out, indent); out << "auto* p = (BurgState*)arena_alloc(sizeof(BurgState)" << ctx_arg() << ");\n";
    pad(out, indent); out << "p->op = BURG_NODE_OP(node);\n";
    pad(out, indent); out << "p->child_count = arity;\n";
    pad(out, indent); out << "p->children = arity > 0\n";
    pad(out, indent); out << "    ? (BurgState**)arena_alloc(arity * sizeof(BurgState*)" << ctx_arg() << ")\n";
    pad(out, indent); out << "    : nullptr;\n";
    pad(out, indent); out << "for (int i = 0; i <= BURG_MAX_NT; i++) {\n";
    pad(out, indent + 1); out << "p->cost[i] = BURG_MAX_COST;\n";
    pad(out, indent + 1); out << "p->rule[i] = 0;\n";
    pad(out, indent); out << "}\n";
}

// Tree label body — no cache, arena alloc, calls burg_dp
void BurgBackend::emit_label_tree_body(std::ostream& out, int indent) {
    emit_label_alloc(out, indent);
    out << "\n";
    pad(out, indent); out << "for (int i = 0; i < arity; i++)\n";
    pad(out, indent + 1); out << "p->children[i] = burg_label_tree(BURG_NODE_CHILD(node, i)" << ctx_arg() << ");\n\n";
    pad(out, indent); out << "burg_dp(p, node" << ctx_arg() << ");\n";
    pad(out, indent); out << "return p;\n";
}

// Graph label body — cache + arena alloc, calls burg_dp
void BurgBackend::emit_label_body(std::ostream& out, int indent) {
    pad(out, indent); out << "void* id = BURG_NODE_ID(node);\n";
    pad(out, indent); out << "BurgState* cached = burg_cache_lookup(id" << ctx_arg() << ");\n";
    pad(out, indent); out << "if (cached) return cached;\n\n";

    emit_label_alloc(out, indent);
    out << "\n";

    pad(out, indent); out << "// Cache BEFORE DP (back-edge cut-point safety)\n";
    pad(out, indent); out << "burg_cache_store(id, p" << ctx_arg() << ");\n\n";

    pad(out, indent); out << "for (int i = 0; i < arity; i++)\n";
    pad(out, indent + 1); out << "p->children[i] = burg_label(BURG_NODE_CHILD(node, i)" << ctx_arg() << ");\n\n";

    pad(out, indent); out << "burg_dp(p, node" << ctx_arg() << ");\n";
    pad(out, indent); out << "return p;\n";
}

// ── Rule + NT name ────────────────────────────────────────

void BurgBackend::emit_rule_func_body(std::ostream& out, int indent) {
    pad(out, indent); out << "if (!state || goalnt < 1 || goalnt > BURG_MAX_NT) return 0;\n";
    pad(out, indent); out << "return state->rule[goalnt];\n";
}

void BurgBackend::emit_nt_name_body(std::ostream& out, int indent) {
    pad(out, indent); out << "static const char* names[] = {\n";
    pad(out, indent + 1); out << "\"<invalid>\",\n";
    for (auto& nt : a_->nonterms) {
        pad(out, indent + 1); out << "\"" << nt << "\",\n";
    }
    pad(out, indent); out << "};\n";
    pad(out, indent); out << "if (nt >= 1 && nt <= BURG_MAX_NT) return names[nt];\n";
    pad(out, indent); out << "return names[0];\n";
}

// ── Reducer ───────────────────────────────────────────────

void BurgBackend::emit_child_reductions(std::ostream& out,
                                         burg_ast::TreePattern* pat, int indent) {
    for (size_t i = 0; i < pat->children.size(); i++) {
        auto* child = pat->children[i];
        std::string idx = std::to_string(i);
        if (child->is_leaf() && !is_terminal(child->name)) {
            int64_t nt_idx = a_->nonterm_index.at(child->name);
            pad(out, indent);
            out << "burg_reduce(BURG_NODE_CHILD(node, " << idx << "), "
                << "state->children[" << idx << "], " << nt_idx << ctx_arg() << ");\n";
        } else if (!child->is_leaf()) {
            for (size_t j = 0; j < child->children.size(); j++) {
                auto* gc = child->children[j];
                if (gc->is_leaf() && !is_terminal(gc->name)) {
                    int64_t nt_idx = a_->nonterm_index.at(gc->name);
                    pad(out, indent);
                    out << "burg_reduce(BURG_NODE_CHILD(BURG_NODE_CHILD(node, " << idx
                        << "), " << j << "), state->children[" << idx
                        << "]->children[" << j << "], " << nt_idx << ctx_arg() << ");\n";
                }
            }
        }
    }
}

void BurgBackend::emit_reduce_body(std::ostream& out, int indent) {
    pad(out, indent); out << "int rule = burg_rule(state, goalnt);\n";
    pad(out, indent); out << "switch (rule) {\n";

    for (auto* rule : a_->spec->rules) {
        pad(out, indent); out << "case " << rule->rule_number << ": { // "
            << rule->nonterm << ": " << pattern_string(rule->pattern) << "\n";

        if (rule->pattern->is_leaf() && !is_terminal(rule->pattern->name)) {
            int64_t src_idx = a_->nonterm_index.at(rule->pattern->name);
            pad(out, indent + 1);
            out << "burg_reduce(node, state, " << src_idx << ctx_arg() << ");\n";
        } else {
            emit_child_reductions(out, rule->pattern, indent + 1);
        }

        if (!rule->action.empty()) {
            std::string trimmed = trim(rule->action);
            if (!trimmed.empty()) {
                pad(out, indent + 1); out << trimmed << "\n";
            }
        }

        pad(out, indent + 1); out << "break;\n";
        pad(out, indent); out << "}\n";
    }

    pad(out, indent); out << "}\n";
}

// ── Rewriter ──────────────────────────────────────────────

void BurgBackend::emit_rpo_dfs(std::ostream& out, int indent) {
    pad(out, indent); out << "std::vector<BURG_NODE_TYPE> rpo;\n";
    pad(out, indent); out << "{\n";
    pad(out, indent + 1); out << "std::unordered_set<void*> visited;\n";
    pad(out, indent + 1); out << "struct Frame { BURG_NODE_TYPE node; int succ; };\n";
    pad(out, indent + 1); out << "std::vector<Frame> stack;\n";
    pad(out, indent + 1); out << "stack.push_back({root, 0});\n";
    pad(out, indent + 1); out << "visited.insert(BURG_NODE_ID(root));\n\n";

    pad(out, indent + 1); out << "while (!stack.empty()) {\n";
    pad(out, indent + 2); out << "auto& f = stack.back();\n";
    pad(out, indent + 2); out << "int sc = BURG_NODE_SUCC_COUNT(f.node);\n";
    pad(out, indent + 2); out << "if (f.succ < sc) {\n";
    pad(out, indent + 3); out << "BURG_NODE_TYPE s = BURG_NODE_SUCC(f.node, f.succ);\n";
    pad(out, indent + 3); out << "f.succ++;\n";
    pad(out, indent + 3); out << "if (s && visited.insert(BURG_NODE_ID(s)).second)\n";
    pad(out, indent + 4); out << "stack.push_back({s, 0});\n";
    pad(out, indent + 2); out << "} else {\n";
    pad(out, indent + 3); out << "rpo.push_back(f.node);\n";
    pad(out, indent + 3); out << "stack.pop_back();\n";
    pad(out, indent + 2); out << "}\n";
    pad(out, indent + 1); out << "}\n";
    pad(out, indent + 1); out << "std::reverse(rpo.begin(), rpo.end());\n";
    pad(out, indent); out << "}\n\n";
}

void BurgBackend::emit_rewrite_body(std::ostream& out, int indent) {
    int64_t start_idx = a_->nonterm_index.at(a_->start_nonterm);

    pad(out, indent); out << "arena_reset(" << ctx_solo() << ");\n\n";

    // Fast path: tree (no successor edges → skip RPO + cache)
    pad(out, indent); out << "if (BURG_NODE_SUCC_COUNT(root) == 0) {\n";
    pad(out, indent + 1); out << "BurgState* state = burg_label_tree(root" << ctx_arg() << ");\n";
    if (a_->has_actions) {
        pad(out, indent + 1); out << "if (state->rule[" << start_idx << "])\n";
        pad(out, indent + 2); out << "burg_reduce(root, state, " << start_idx << ctx_arg() << ");\n";
    }
    pad(out, indent + 1); out << "return;\n";
    pad(out, indent); out << "}\n\n";

    // Slow path: graph (RPO + cache)
    pad(out, indent); out << "burg_cache_clear(" << ctx_solo() << ");\n\n";

    pad(out, indent); out << "// Compute reverse postorder via iterative DFS\n";
    emit_rpo_dfs(out, indent);

    pad(out, indent); out << "// Label every node (data-flow children labeled recursively)\n";
    pad(out, indent); out << "for (auto* n : rpo)\n";
    pad(out, indent + 1); out << "burg_label(n" << ctx_arg() << ");\n\n";

    if (a_->has_actions) {
        pad(out, indent); out << "// Reduce every node with the start nonterminal\n";
        pad(out, indent); out << "for (auto* n : rpo) {\n";
        pad(out, indent + 1); out << "BurgState* s = burg_cache_lookup(BURG_NODE_ID(n)" << ctx_arg() << ");\n";
        pad(out, indent + 1); out << "if (s && s->rule[" << start_idx << "])\n";
        pad(out, indent + 2); out << "burg_reduce(n, s, " << start_idx << ctx_arg() << ");\n";
        pad(out, indent); out << "}\n";
    }
}
