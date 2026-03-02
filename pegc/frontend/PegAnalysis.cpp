#include "PegAnalysis.h"

#include <functional>

using namespace PegGrammar;

namespace pegc {

// ═══════════════════════════════════════════════════════════════
// Main entry point
// ═══════════════════════════════════════════════════════════════

AnalysisResult PegAnalysis::analyze(Grammar* grammar) {
    result_ = AnalysisResult{};

    // Collect charset/token names for classification
    for (auto* cs : grammar->charsets)
        charset_names_.insert(cs->name);
    for (auto* tk : grammar->tokens)
        token_names_.insert(tk->name);

    collect_productions(grammar);
    detect_left_recursion();
    compute_first_sets();
    classify_alternatives(grammar);
    topological_sort();
    detect_unreachable(grammar);

    return std::move(result_);
}

// ═══════════════════════════════════════════════════════════════
// Phase 1: Collect productions, build call graph
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::collect_productions(Grammar* grammar) {
    for (auto* p : grammar->productions) {
        if (result_.productions.count(p->name)) {
            result_.errors.push_back(
                "duplicate production: " + p->name);
            continue;
        }
        ProductionInfo info;
        info.name = p->name;
        info.node = p;
        collect_calls(p->body, info.calls);
        result_.productions[p->name] = std::move(info);
    }
}

void PegAnalysis::collect_calls(PegExpr* expr, std::set<std::string>& calls) {
    if (!expr) return;

    if (auto* rc = dynamic_cast<RuleCall*>(expr)) {
        // Only count as a production call if not a charset/token
        if (!charset_names_.count(rc->name) && !token_names_.count(rc->name)) {
            calls.insert(rc->name);
        }
    } else if (auto* seq = dynamic_cast<Sequence*>(expr)) {
        for (auto* e : seq->elements) collect_calls(e, calls);
    } else if (auto* ch = dynamic_cast<Choice*>(expr)) {
        for (auto* a : ch->alternatives) collect_calls(a, calls);
    } else if (auto* s = dynamic_cast<Star*>(expr)) {
        collect_calls(s->body, calls);
    } else if (auto* p = dynamic_cast<Plus*>(expr)) {
        collect_calls(p->body, calls);
    } else if (auto* o = dynamic_cast<Optional*>(expr)) {
        collect_calls(o->body, calls);
    } else if (auto* a = dynamic_cast<And*>(expr)) {
        collect_calls(a->body, calls);
    } else if (auto* n = dynamic_cast<Not*>(expr)) {
        collect_calls(n->body, calls);
    } else if (auto* g = dynamic_cast<Group*>(expr)) {
        collect_calls(g->body, calls);
    } else if (auto* r = dynamic_cast<Resolver*>(expr)) {
        collect_calls(r->body, calls);
    }
}

// ═══════════════════════════════════════════════════════════════
// Phase 2: Left recursion detection
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::detect_left_recursion() {
    for (auto& [name, info] : result_.productions) {
        std::set<std::string> visited;
        if (has_left_path(name, name, visited)) {
            result_.errors.push_back(
                "left recursion detected in production: " + name);
        }
    }
}

PegExpr* PegAnalysis::first_element(PegExpr* expr) {
    if (auto* seq = dynamic_cast<Sequence*>(expr)) {
        if (!seq->elements.empty()) return first_element(seq->elements[0]);
    }
    if (auto* ch = dynamic_cast<Choice*>(expr)) {
        // All alternatives could be the first
        return nullptr;
    }
    if (auto* g = dynamic_cast<Group*>(expr)) {
        return first_element(g->body);
    }
    return expr;
}

bool PegAnalysis::has_left_path(const std::string& from, const std::string& to,
                                 std::set<std::string>& visited) {
    if (!result_.productions.count(from)) return false;
    if (visited.count(from)) return false;
    visited.insert(from);

    auto* body = result_.productions[from].node->body;

    // Get all possible "leftmost" rule calls
    std::vector<PegExpr*> leftmost;

    // For Choice, each alternative could be leftmost
    if (auto* ch = dynamic_cast<Choice*>(body)) {
        for (auto* alt : ch->alternatives) {
            auto* f = first_element(alt);
            if (f) leftmost.push_back(f);
        }
    } else {
        auto* f = first_element(body);
        if (f) leftmost.push_back(f);
    }

    for (auto* f : leftmost) {
        if (auto* rc = dynamic_cast<RuleCall*>(f)) {
            if (rc->name == to) return true;
            if (has_left_path(rc->name, to, visited)) return true;
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════
// Phase 3: FIRST set computation
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::compute_first_sets() {
    for (auto& [name, info] : result_.productions) {
        std::set<std::string> expanding;
        info.first_set = first_of(info.node->body, expanding);
        info.first_testable = !info.first_set.empty() &&
            info.first_set[0].kind != FirstElement::Epsilon;
    }
}

std::vector<FirstElement> PegAnalysis::first_of(PegExpr* expr,
                                                 std::set<std::string>& expanding) {
    std::vector<FirstElement> result;
    if (!expr) return result;

    if (auto* lit = dynamic_cast<Literal*>(expr)) {
        result.push_back({FirstElement::Literal, lit->value});
    } else if (auto* rc = dynamic_cast<RuleCall*>(expr)) {
        if (charset_names_.count(rc->name)) {
            result.push_back({FirstElement::Charset, rc->name});
        } else if (token_names_.count(rc->name)) {
            result.push_back({FirstElement::Token, rc->name});
        } else {
            // Inline the first set of the called production (with cycle guard)
            if (result_.productions.count(rc->name) &&
                !expanding.count(rc->name)) {
                expanding.insert(rc->name);
                result = first_of(result_.productions[rc->name].node->body, expanding);
                expanding.erase(rc->name);
            }
        }
    } else if (auto* seq = dynamic_cast<Sequence*>(expr)) {
        if (!seq->elements.empty()) {
            return first_of(seq->elements[0], expanding);
        }
    } else if (auto* ch = dynamic_cast<Choice*>(expr)) {
        for (auto* alt : ch->alternatives) {
            auto f = first_of(alt, expanding);
            result.insert(result.end(), f.begin(), f.end());
        }
    } else if (auto* star = dynamic_cast<Star*>(expr)) {
        auto f = first_of(star->body, expanding);
        result.insert(result.end(), f.begin(), f.end());
        result.push_back({FirstElement::Epsilon, ""});
    } else if (auto* plus = dynamic_cast<Plus*>(expr)) {
        return first_of(plus->body, expanding);
    } else if (auto* opt = dynamic_cast<Optional*>(expr)) {
        auto f = first_of(opt->body, expanding);
        result.insert(result.end(), f.begin(), f.end());
        result.push_back({FirstElement::Epsilon, ""});
    } else if (auto* grp = dynamic_cast<Group*>(expr)) {
        return first_of(grp->body, expanding);
    } else if (dynamic_cast<AnyExpr*>(expr)) {
        result.push_back({FirstElement::Any, ""});
    } else if (auto* csr = dynamic_cast<CharsetRef*>(expr)) {
        result.push_back({FirstElement::Charset, csr->name});
    } else if (auto* tkr = dynamic_cast<TokenRef*>(expr)) {
        result.push_back({FirstElement::Token, tkr->name});
    } else if (auto* res = dynamic_cast<Resolver*>(expr)) {
        return first_of(res->body, expanding);
    }
    // Action, And, Not — don't contribute to FIRST set

    return result;
}

// ═══════════════════════════════════════════════════════════════
// Phase 4: Testable classification + SPAN candidates
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::classify_alternatives(Grammar* grammar) {
    // Walk all productions, find Choice nodes, classify alternatives
    for (auto* p : grammar->productions) {
        if (auto* ch = dynamic_cast<Choice*>(p->body)) {
            for (auto* alt : ch->alternatives) {
                analyze_alternative(alt);
            }
        }
    }
}

AlternativeInfo PegAnalysis::analyze_alternative(PegExpr* expr) {
    AlternativeInfo info;

    std::set<std::string> expanding;
    auto first = first_of(expr, expanding);
    if (!first.empty()) {
        info.first = first[0];
        info.testable = (first[0].kind == FirstElement::Literal ||
                         first[0].kind == FirstElement::Charset);
    }

    // SPAN candidate: Star(charset_ref) or Star(token_ref for charset)
    if (auto* star = dynamic_cast<Star*>(expr)) {
        if (auto* csr = dynamic_cast<CharsetRef*>(star->body)) {
            info.span_candidate = true;
            info.span_charset = csr->name;
        } else if (auto* rc = dynamic_cast<RuleCall*>(star->body)) {
            if (charset_names_.count(rc->name)) {
                info.span_candidate = true;
                info.span_charset = rc->name;
            }
        }
    }

    return info;
}

// ═══════════════════════════════════════════════════════════════
// Phase 5: Topological sort
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::topological_sort() {
    std::set<std::string> visited;
    std::set<std::string> in_stack;

    std::function<void(const std::string&)> visit =
        [&](const std::string& name) {
        if (visited.count(name)) return;
        if (in_stack.count(name)) return;  // cycle — already reported
        in_stack.insert(name);

        if (result_.productions.count(name)) {
            for (auto& dep : result_.productions[name].calls) {
                visit(dep);
            }
        }

        in_stack.erase(name);
        visited.insert(name);
        result_.topo_order.push_back(name);
    };

    for (auto& [name, _] : result_.productions) {
        visit(name);
    }
}

// ═══════════════════════════════════════════════════════════════
// Phase 6: Unreachable production detection
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::detect_unreachable(Grammar* grammar) {
    if (grammar->productions.empty()) return;

    std::set<std::string> reachable;
    mark_reachable(grammar->productions[0]->name, reachable);

    for (auto& [name, info] : result_.productions) {
        if (!reachable.count(name)) {
            info.reachable = false;
            result_.warnings.push_back(
                "unreachable production: " + name);
        }
    }
}

void PegAnalysis::mark_reachable(const std::string& name,
                                  std::set<std::string>& visited) {
    if (visited.count(name)) return;
    visited.insert(name);

    if (result_.productions.count(name)) {
        for (auto& dep : result_.productions[name].calls) {
            mark_reachable(dep, visited);
        }
    }
}

} // namespace pegc
