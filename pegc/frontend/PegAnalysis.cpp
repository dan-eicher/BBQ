#include "PegAnalysis.h"

#include <functional>
#include <sstream>

using namespace PegGrammar;

namespace pegc {

// ═══════════════════════════════════════════════════════════════
// Built-in token methods on the pegc runtime cursor.
// RuleCalls to any of these are legal even without a declaration
// in the grammar — they map to PegRuntime.h methods. Anything else
// is a typo.
// ═══════════════════════════════════════════════════════════════

bool PegAnalysis::is_builtin(const std::string& name) {
    static const std::set<std::string> builtins = {
        "at_end", "match", "match_charset",
        "peek_at", "peek_charset",
        "keyword", "peek_keyword",
        "ident", "qualified_ident",
        "integer", "floating",
        "string_lit", "char_lit",
        "scan_to",
    };
    return builtins.count(name) > 0;
}

// ═══════════════════════════════════════════════════════════════
// Main entry point
// ═══════════════════════════════════════════════════════════════

AnalysisResult PegAnalysis::analyze(Grammar* grammar,
                                      const PegAnalysisConfig& cfg) {
    result_ = AnalysisResult{};

    // Collect charset/token names for classification
    for (auto* cs : grammar->charsets)
        charset_names_.insert(cs->name);
    for (auto* tk : grammar->tokens)
        token_names_.insert(tk->name);

    collect_productions(grammar);
    // Validate that every RuleCall references a declared production,
    // a charset, a token, or a known pegc-runtime built-in. Reject
    // typos before the analyses that consume the call graph.
    for (auto& entry : result_.productions) {
        const std::string& prod_name = entry.first;
        ProductionInfo& info = entry.second;
        std::function<void(PegGrammar::PegExpr*)> check =
            [&](PegGrammar::PegExpr* expr) {
            if (!expr) return;
            if (auto* rc = dynamic_cast<PegGrammar::RuleCall*>(expr)) {
                if (!charset_names_.count(rc->name) &&
                    !token_names_.count(rc->name) &&
                    !result_.productions.count(rc->name) &&
                    !is_builtin(rc->name)) {
                    result_.errors.push_back(
                        "undeclared identifier '" + rc->name + "' in production " + prod_name);
                }
            } else if (auto* seq = dynamic_cast<PegGrammar::Sequence*>(expr)) {
                for (auto* e : seq->elements) check(e);
            } else if (auto* ch = dynamic_cast<PegGrammar::Choice*>(expr)) {
                for (auto* a : ch->alternatives) check(a);
            } else if (auto* s = dynamic_cast<PegGrammar::Star*>(expr)) check(s->body);
            else if (auto* p = dynamic_cast<PegGrammar::Plus*>(expr)) check(p->body);
            else if (auto* o = dynamic_cast<PegGrammar::Optional*>(expr)) check(o->body);
            else if (auto* a = dynamic_cast<PegGrammar::And*>(expr)) check(a->body);
            else if (auto* n = dynamic_cast<PegGrammar::Not*>(expr)) check(n->body);
            else if (auto* g = dynamic_cast<PegGrammar::Group*>(expr)) check(g->body);
            else if (auto* r = dynamic_cast<PegGrammar::Resolver*>(expr)) check(r->body);
        };
        check(info.node->body);
    }

    detect_left_recursion();
    build_callers_graph();
    compute_first_sets();
    compute_follow_sets(grammar);
    classify_alternatives(grammar);
    if (cfg.emit_coverage_warnings) {
        // High-FP-rate checks: real grammars use shared-prefix
        // alternatives intentionally. Opt-in via --coverage.
        detect_masked_alternatives();
        detect_star_suffix_conflict(grammar);
        detect_star_follow_conflict(grammar);
    }
    detect_always_failing();
    detect_useless_predicates(grammar);
    detect_unused_terminals(grammar);
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

void PegAnalysis::build_callers_graph() {
    for (auto& [name, info] : result_.productions) {
        for (auto& callee : info.calls)
            callers_[callee].insert(name);
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
            info.left_recursive = true;
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
// FIRST set helpers
// ═══════════════════════════════════════════════════════════════

bool PegAnalysis::first_element_eq(const FirstElement& a, const FirstElement& b) {
    return a.kind == b.kind && a.value == b.value;
}

bool PegAnalysis::first_set_eq(const std::vector<FirstElement>& a,
                                 const std::vector<FirstElement>& b) {
    if (a.size() != b.size()) return false;
    // Order-insensitive compare: every element of a is in b.
    for (auto& x : a) {
        bool found = false;
        for (auto& y : b)
            if (first_element_eq(x, y)) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

void PegAnalysis::first_set_union(std::vector<FirstElement>& dest,
                                    const std::vector<FirstElement>& src,
                                    bool include_epsilon) {
    for (auto& e : src) {
        if (!include_epsilon && e.kind == FirstElement::Epsilon) continue;
        bool dup = false;
        for (auto& d : dest)
            if (first_element_eq(d, e)) { dup = true; break; }
        if (!dup) dest.push_back(e);
    }
}

bool PegAnalysis::first_set_contains_epsilon(const std::vector<FirstElement>& s) {
    for (auto& e : s)
        if (e.kind == FirstElement::Epsilon) return true;
    return false;
}

std::vector<FirstElement>
PegAnalysis::first_set_without_epsilon(const std::vector<FirstElement>& s) {
    std::vector<FirstElement> out;
    out.reserve(s.size());
    for (auto& e : s)
        if (e.kind != FirstElement::Epsilon) out.push_back(e);
    return out;
}

// True iff every element of `b` (excluding epsilon) is also in `a`.
// Used by masking detection: A masks B iff FIRST(A) ⊇ FIRST(B).
bool PegAnalysis::first_set_subsumes(const std::vector<FirstElement>& a,
                                       const std::vector<FirstElement>& b) {
    for (auto& y : b) {
        if (y.kind == FirstElement::Epsilon) continue;
        bool found = false;
        for (auto& x : a)
            if (first_element_eq(x, y)) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Phase 3: FIRST + nullable as a Kildall worklist
// ═══════════════════════════════════════════════════════════════

bool PegAnalysis::nullable_of(PegExpr* expr) {
    if (!expr) return true;
    if (dynamic_cast<Literal*>(expr) ||
        dynamic_cast<CharsetRef*>(expr) ||
        dynamic_cast<TokenRef*>(expr) ||
        dynamic_cast<AnyExpr*>(expr)) {
        return false;
    }
    if (auto* rc = dynamic_cast<RuleCall*>(expr)) {
        if (charset_names_.count(rc->name) || token_names_.count(rc->name))
            return false;
        auto it = result_.productions.find(rc->name);
        if (it != result_.productions.end()) return it->second.nullable;
        if (is_builtin(rc->name)) return false;  // built-in tokens consume input
        // Otherwise unknown — collect_calls should have errored.
        // Conservative for nullable: assume not nullable.
        return false;
    }
    if (auto* seq = dynamic_cast<Sequence*>(expr)) {
        for (auto* e : seq->elements)
            if (!nullable_of(e)) return false;
        return true;
    }
    if (auto* ch = dynamic_cast<Choice*>(expr)) {
        for (auto* a : ch->alternatives)
            if (nullable_of(a)) return true;
        return false;
    }
    if (dynamic_cast<Star*>(expr) || dynamic_cast<Optional*>(expr)) return true;
    if (auto* p = dynamic_cast<Plus*>(expr)) return nullable_of(p->body);
    if (auto* g = dynamic_cast<Group*>(expr)) return nullable_of(g->body);
    if (auto* r = dynamic_cast<Resolver*>(expr)) return nullable_of(r->body);
    // Action / And / Not consume nothing — treat as nullable.
    return true;
}

std::vector<FirstElement> PegAnalysis::first_of(PegExpr* expr) {
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
            auto it = result_.productions.find(rc->name);
            if (it != result_.productions.end()) {
                // Read from the table — no recursion across productions.
                result = it->second.first_set;
            } else if (is_builtin(rc->name)) {
                // Built-in pegc-runtime token method (ident, integer,
                // qualified_ident, scan_to, etc.). Opaque to FIRST
                // analysis; treat as a Token-kind element keyed on
                // the method name so callers don't show empty FIRST.
                result.push_back({FirstElement::Token, rc->name});
            }
            // else: undeclared identifier — collect_calls reports the
            // error; FIRST stays empty here, which is correct
            // (production can't match anything via this call).
        }
    } else if (auto* seq = dynamic_cast<Sequence*>(expr)) {
        // Walk the sequence consuming nullable prefix.
        bool all_nullable = true;
        for (size_t i = 0; i < seq->elements.size(); i++) {
            auto f = first_of(seq->elements[i]);
            first_set_union(result, f, /*include_epsilon=*/false);
            if (!nullable_of(seq->elements[i])) {
                all_nullable = false;
                break;
            }
        }
        if (all_nullable) result.push_back({FirstElement::Epsilon, ""});
    } else if (auto* ch = dynamic_cast<Choice*>(expr)) {
        for (auto* alt : ch->alternatives) {
            auto f = first_of(alt);
            first_set_union(result, f);
        }
    } else if (auto* star = dynamic_cast<Star*>(expr)) {
        result = first_of(star->body);
        if (!first_set_contains_epsilon(result))
            result.push_back({FirstElement::Epsilon, ""});
    } else if (auto* plus = dynamic_cast<Plus*>(expr)) {
        return first_of(plus->body);
    } else if (auto* opt = dynamic_cast<Optional*>(expr)) {
        result = first_of(opt->body);
        if (!first_set_contains_epsilon(result))
            result.push_back({FirstElement::Epsilon, ""});
    } else if (auto* grp = dynamic_cast<Group*>(expr)) {
        return first_of(grp->body);
    } else if (dynamic_cast<AnyExpr*>(expr)) {
        result.push_back({FirstElement::Any, ""});
    } else if (auto* csr = dynamic_cast<CharsetRef*>(expr)) {
        result.push_back({FirstElement::Charset, csr->name});
    } else if (auto* tkr = dynamic_cast<TokenRef*>(expr)) {
        result.push_back({FirstElement::Token, tkr->name});
    } else if (auto* res = dynamic_cast<Resolver*>(expr)) {
        return first_of(res->body);
    } else {
        // Action / And / Not — match without consuming, so contribute ε.
        result.push_back({FirstElement::Epsilon, ""});
    }

    return result;
}

void PegAnalysis::compute_first_sets() {
    bbq::Worklist<std::string> wl;
    for (auto& [name, _] : result_.productions) wl.push(name);

    while (!wl.empty()) {
        std::string name = wl.pop();
        auto& info = result_.productions[name];
        if (info.left_recursive) continue;  // FIRST is undefined; skip

        auto new_first = first_of(info.node->body);
        if (!first_set_eq(new_first, info.first_set)) {
            info.first_set = std::move(new_first);
            info.nullable = first_set_contains_epsilon(info.first_set);
            // Re-process every production that calls this one — its
            // FIRST may grow.
            auto cit = callers_.find(name);
            if (cit != callers_.end())
                for (auto& caller : cit->second) wl.push(caller);
        }
    }

    // first_testable: usable for head-fail optimization (literal or charset
    // start, not ε).
    for (auto& [name, info] : result_.productions) {
        info.first_testable = !info.first_set.empty() &&
            info.first_set[0].kind != FirstElement::Epsilon;
    }
}

// ═══════════════════════════════════════════════════════════════
// Phase 4: FOLLOW as a Kildall worklist
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::walk_for_follow(PegExpr* expr,
                                    ProductionInfo& owner,
                                    const std::vector<FirstElement>& tail_first,
                                    bbq::Worklist<std::string>& wl) {
    if (!expr) return;

    if (auto* rc = dynamic_cast<RuleCall*>(expr)) {
        if (charset_names_.count(rc->name) || token_names_.count(rc->name))
            return;
        auto it = result_.productions.find(rc->name);
        if (it == result_.productions.end()) return;
        auto& callee = it->second;
        std::vector<FirstElement> grew = callee.follow_set;
        first_set_union(grew, tail_first, /*include_epsilon=*/false);
        if (!first_set_eq(grew, callee.follow_set)) {
            callee.follow_set = std::move(grew);
            // Re-walk every production that calls this one — sites of
            // calls to other productions inside that owner may now see
            // a larger tail through nullable paths.
            wl.push(callee.name);
        }
        return;
    }

    if (auto* seq = dynamic_cast<Sequence*>(expr)) {
        // For each element i in the sequence, the "tail after i" is
        // FIRST(elements[i+1..]) plus tail_first if that suffix is
        // nullable.
        size_t n = seq->elements.size();
        for (size_t i = 0; i < n; i++) {
            std::vector<FirstElement> sf;
            bool suffix_nullable = true;
            for (size_t j = i + 1; j < n; j++) {
                auto fj = first_of(seq->elements[j]);
                first_set_union(sf, fj, /*include_epsilon=*/false);
                if (!nullable_of(seq->elements[j])) {
                    suffix_nullable = false;
                    break;
                }
            }
            if (suffix_nullable)
                first_set_union(sf, tail_first, /*include_epsilon=*/false);
            // For RuleCall children, also propagate owner's follow when
            // the suffix is nullable — that's how FOLLOW chains across
            // productions.
            if (suffix_nullable) {
                first_set_union(sf, owner.follow_set,
                                /*include_epsilon=*/false);
            }
            walk_for_follow(seq->elements[i], owner, sf, wl);
        }
        return;
    }

    if (auto* ch = dynamic_cast<Choice*>(expr)) {
        for (auto* alt : ch->alternatives)
            walk_for_follow(alt, owner, tail_first, wl);
        return;
    }
    if (auto* g = dynamic_cast<Group*>(expr)) {
        walk_for_follow(g->body, owner, tail_first, wl);
        return;
    }
    if (auto* r = dynamic_cast<Resolver*>(expr)) {
        walk_for_follow(r->body, owner, tail_first, wl);
        return;
    }
    if (auto* opt = dynamic_cast<Optional*>(expr)) {
        walk_for_follow(opt->body, owner, tail_first, wl);
        return;
    }
    if (auto* star = dynamic_cast<Star*>(expr)) {
        // Body is followed by itself when repeating, then by the outer tail.
        std::vector<FirstElement> body_tail;
        first_set_union(body_tail, first_of(star->body),
                        /*include_epsilon=*/false);
        first_set_union(body_tail, tail_first, /*include_epsilon=*/false);
        walk_for_follow(star->body, owner, body_tail, wl);
        return;
    }
    if (auto* plus = dynamic_cast<Plus*>(expr)) {
        std::vector<FirstElement> body_tail;
        first_set_union(body_tail, first_of(plus->body),
                        /*include_epsilon=*/false);
        first_set_union(body_tail, tail_first, /*include_epsilon=*/false);
        walk_for_follow(plus->body, owner, body_tail, wl);
        return;
    }
    if (auto* a = dynamic_cast<And*>(expr)) {
        // Lookahead consumes nothing — its body's "tail" is the same
        // input position as the predicate; we pass empty tail to keep
        // it conservative.
        walk_for_follow(a->body, owner, {}, wl);
        return;
    }
    if (auto* n = dynamic_cast<Not*>(expr)) {
        walk_for_follow(n->body, owner, {}, wl);
        return;
    }
    // Literal / RuleCall (already handled) / charset / Action: nothing
    // to recurse into.
}

void PegAnalysis::compute_follow_sets(Grammar* grammar) {
    if (grammar->productions.empty()) return;

    // Seed: start production has EOF in its FOLLOW.
    const std::string& start_name = grammar->productions[0]->name;
    auto sit = result_.productions.find(start_name);
    if (sit != result_.productions.end()) {
        sit->second.follow_set.push_back({FirstElement::Literal, "$"});
    }

    bbq::Worklist<std::string> wl;
    for (auto& [name, _] : result_.productions) wl.push(name);

    while (!wl.empty()) {
        std::string name = wl.pop();
        auto it = result_.productions.find(name);
        if (it == result_.productions.end()) continue;
        if (it->second.left_recursive) continue;
        walk_for_follow(it->second.node->body, it->second, /*tail=*/{}, wl);
    }
}

// ═══════════════════════════════════════════════════════════════
// Phase 5: Testable classification + persist per-alternative FIRST
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::classify_alternatives(Grammar* grammar) {
    // Walk all productions, find Choice nodes, classify alternatives,
    // and persist per-alternative FIRST in the side table.
    std::function<void(PegExpr*)> walk = [&](PegExpr* expr) {
        if (!expr) return;
        if (auto* ch = dynamic_cast<Choice*>(expr)) {
            std::vector<std::vector<FirstElement>> alts_first;
            alts_first.reserve(ch->alternatives.size());
            for (auto* alt : ch->alternatives) {
                analyze_alternative(alt);
                alts_first.push_back(first_of(alt));
            }
            result_.alternative_firsts[ch] = std::move(alts_first);
            for (auto* alt : ch->alternatives) walk(alt);
        } else if (auto* seq = dynamic_cast<Sequence*>(expr)) {
            for (auto* e : seq->elements) walk(e);
        } else if (auto* s = dynamic_cast<Star*>(expr)) {
            walk(s->body);
        } else if (auto* p = dynamic_cast<Plus*>(expr)) {
            walk(p->body);
        } else if (auto* o = dynamic_cast<Optional*>(expr)) {
            walk(o->body);
        } else if (auto* a = dynamic_cast<And*>(expr)) {
            walk(a->body);
        } else if (auto* n = dynamic_cast<Not*>(expr)) {
            walk(n->body);
        } else if (auto* g = dynamic_cast<Group*>(expr)) {
            walk(g->body);
        } else if (auto* r = dynamic_cast<Resolver*>(expr)) {
            walk(r->body);
        }
    };
    for (auto* p : grammar->productions) walk(p->body);
}

AlternativeInfo PegAnalysis::analyze_alternative(PegExpr* expr) {
    AlternativeInfo info;

    auto first = first_of(expr);
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
// Phase 6: Ordered-choice masking detection
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::detect_masked_alternatives() {
    // Find each choice's containing production for the warning text.
    // We don't track Choice → owner_production directly; recover by
    // iterating productions and walking their bodies.
    std::map<Choice*, std::string> choice_owner;
    std::function<void(PegExpr*, const std::string&)> walk =
        [&](PegExpr* expr, const std::string& owner) {
        if (!expr) return;
        if (auto* ch = dynamic_cast<Choice*>(expr)) {
            choice_owner[ch] = owner;
            for (auto* alt : ch->alternatives) walk(alt, owner);
        } else if (auto* seq = dynamic_cast<Sequence*>(expr)) {
            for (auto* e : seq->elements) walk(e, owner);
        } else if (auto* s = dynamic_cast<Star*>(expr)) walk(s->body, owner);
        else if (auto* p = dynamic_cast<Plus*>(expr)) walk(p->body, owner);
        else if (auto* o = dynamic_cast<Optional*>(expr)) walk(o->body, owner);
        else if (auto* a = dynamic_cast<And*>(expr)) walk(a->body, owner);
        else if (auto* n = dynamic_cast<Not*>(expr)) walk(n->body, owner);
        else if (auto* g = dynamic_cast<Group*>(expr)) walk(g->body, owner);
        else if (auto* r = dynamic_cast<Resolver*>(expr)) walk(r->body, owner);
    };
    for (auto& [name, info] : result_.productions)
        walk(info.node->body, name);

    for (auto& [ch, alts_first] : result_.alternative_firsts) {
        const std::string& owner = choice_owner[ch];
        for (size_t j = 1; j < alts_first.size(); j++) {
            for (size_t i = 0; i < j; i++) {
                if (first_set_subsumes(alts_first[i], alts_first[j])) {
                    result_.warnings.push_back(
                        "alternative " + std::to_string(j) +
                        " of choice in " + owner +
                        " may be masked by alternative " + std::to_string(i) +
                        " (FIRST set subsumed)");
                    break;  // one warning per masked alternative
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Phase 7: Greedy Star/Plus + suffix conflict
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::detect_star_suffix_conflict(Grammar* grammar) {
    std::function<void(PegExpr*, const std::string&)> walk =
        [&](PegExpr* expr, const std::string& owner) {
        if (!expr) return;
        if (auto* seq = dynamic_cast<Sequence*>(expr)) {
            const auto& es = seq->elements;
            for (size_t i = 0; i + 1 < es.size(); i++) {
                PegExpr* body = nullptr;
                const char* op = nullptr;
                if (auto* s = dynamic_cast<Star*>(es[i])) { body = s->body; op = "Star"; }
                else if (auto* p = dynamic_cast<Plus*>(es[i])) { body = p->body; op = "Plus"; }
                if (!body) continue;

                // Build FIRST of the suffix.
                std::vector<FirstElement> sf;
                bool nullable_suffix = true;
                for (size_t j = i + 1; j < es.size(); j++) {
                    first_set_union(sf, first_of(es[j]),
                                    /*include_epsilon=*/false);
                    if (!nullable_of(es[j])) { nullable_suffix = false; break; }
                }
                // A nullable suffix wouldn't get blocked by a greedy
                // repetition in the same way — skip those.
                if (sf.empty() || nullable_suffix) continue;

                auto body_first = first_set_without_epsilon(first_of(body));
                if (first_set_subsumes(body_first, sf)) {
                    result_.warnings.push_back(
                        std::string(op) + "'s body FIRST in " + owner +
                        " subsumes the following suffix's FIRST; "
                        "the repetition will greedily consume what "
                        "the suffix needs to match");
                }
            }
            for (auto* e : es) walk(e, owner);
        } else if (auto* ch = dynamic_cast<Choice*>(expr)) {
            for (auto* alt : ch->alternatives) walk(alt, owner);
        } else if (auto* s = dynamic_cast<Star*>(expr)) walk(s->body, owner);
        else if (auto* p = dynamic_cast<Plus*>(expr)) walk(p->body, owner);
        else if (auto* o = dynamic_cast<Optional*>(expr)) walk(o->body, owner);
        else if (auto* a = dynamic_cast<And*>(expr)) walk(a->body, owner);
        else if (auto* n = dynamic_cast<Not*>(expr)) walk(n->body, owner);
        else if (auto* g = dynamic_cast<Group*>(expr)) walk(g->body, owner);
        else if (auto* r = dynamic_cast<Resolver*>(expr)) walk(r->body, owner);
    };
    for (auto* p : grammar->productions) walk(p->body, p->name);
}

// ═══════════════════════════════════════════════════════════════
// Star/Plus + FOLLOW conflict (Redziejowski 2009 "Conflict with
// follow"). For a Star/Plus at the trailing position of a
// production's body, the FIRST of the body must not overlap
// FOLLOW(production) — otherwise the iteration greedily consumes
// what callers expect to come *after* the production.
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::detect_star_follow_conflict(Grammar* grammar) {
    // Walk each production body looking for trailing Star/Plus
    // expressions: the Star/Plus is "trailing" if every successor
    // in its enclosing scope is nullable, so the iteration's tail
    // is FOLLOW of the containing production.
    std::function<void(PegExpr*, ProductionInfo&, bool)> walk =
        [&](PegExpr* expr, ProductionInfo& owner, bool tail_is_follow) {
        if (!expr) return;
        if (auto* seq = dynamic_cast<Sequence*>(expr)) {
            // Only the LAST element inherits the parent's tail.
            // Earlier elements are followed by their successors;
            // those are the in-sequence cases handled by
            // detect_star_suffix_conflict.
            for (size_t i = 0; i + 1 < seq->elements.size(); i++)
                walk(seq->elements[i], owner, false);
            if (!seq->elements.empty())
                walk(seq->elements.back(), owner, tail_is_follow);
            return;
        }
        if (auto* ch = dynamic_cast<Choice*>(expr)) {
            for (auto* alt : ch->alternatives)
                walk(alt, owner, tail_is_follow);
            return;
        }
        if (auto* g = dynamic_cast<Group*>(expr)) {
            walk(g->body, owner, tail_is_follow);
            return;
        }
        if (auto* r = dynamic_cast<Resolver*>(expr)) {
            walk(r->body, owner, tail_is_follow);
            return;
        }
        // Trailing Star/Plus: compare body's FIRST to FOLLOW(owner).
        PegExpr* body = nullptr;
        const char* op = nullptr;
        if (auto* s = dynamic_cast<Star*>(expr)) { body = s->body; op = "Star"; }
        else if (auto* p = dynamic_cast<Plus*>(expr)) { body = p->body; op = "Plus"; }
        if (!body) return;
        if (!tail_is_follow) {
            // Inner Star/Plus — the suffix-within-Sequence check
            // (detect_star_suffix_conflict) handles it.
            walk(body, owner, false);
            return;
        }
        auto body_first = first_set_without_epsilon(first_of(body));
        if (body_first.empty()) {
            walk(body, owner, false);
            return;
        }
        // Redziejowski's `e ≍ FOLLOW(E)` — body FIRST and FOLLOW
        // are not disjoint. Even partial overlap means the
        // iteration consumes some terminal that the caller expected
        // to come *after* the production.
        std::vector<FirstElement> overlap;
        for (auto& f : owner.follow_set) {
            if (f.kind == FirstElement::Epsilon) continue;
            for (auto& b : body_first)
                if (first_element_eq(f, b)) { overlap.push_back(f); break; }
        }
        if (!overlap.empty()) {
            std::ostringstream os;
            os << "trailing " << op << " in " << owner.name
               << ": body FIRST overlaps FOLLOW(" << owner.name
               << ") — iteration will greedily consume what callers "
               << "expect to follow";
            result_.warnings.push_back(os.str());
        }
        walk(body, owner, false);
    };

    for (auto* p : grammar->productions) {
        auto it = result_.productions.find(p->name);
        if (it == result_.productions.end()) continue;
        if (it->second.left_recursive) continue;
        walk(p->body, it->second, /*tail_is_follow=*/true);
    }
}

// ═══════════════════════════════════════════════════════════════
// Phase 8: Always-failing productions
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::detect_always_failing() {
    for (auto& [name, info] : result_.productions) {
        if (info.left_recursive) continue;
        // Empty FIRST + not nullable means there's no input the
        // production can ever match.
        if (info.first_set.empty() && !info.nullable) {
            result_.warnings.push_back(
                "production " + name + " has empty FIRST and is not "
                "nullable; it can never match");
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Phase 9: Useless lookahead predicates
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::detect_useless_predicates(Grammar* grammar) {
    std::function<void(PegExpr*, const std::string&)> walk =
        [&](PegExpr* expr, const std::string& owner) {
        if (!expr) return;
        if (auto* a = dynamic_cast<And*>(expr)) {
            if (nullable_of(a->body)) {
                result_.warnings.push_back(
                    "predicate &(...) in " + owner +
                    " always succeeds (body matches empty)");
            }
            walk(a->body, owner);
            return;
        }
        if (auto* n = dynamic_cast<Not*>(expr)) {
            if (nullable_of(n->body)) {
                result_.warnings.push_back(
                    "predicate !(...) in " + owner +
                    " always fails (body matches empty)");
            } else {
                auto bf = first_set_without_epsilon(first_of(n->body));
                if (bf.empty()) {
                    result_.warnings.push_back(
                        "predicate !(...) in " + owner +
                        " always succeeds (body never matches)");
                }
            }
            walk(n->body, owner);
            return;
        }
        if (auto* seq = dynamic_cast<Sequence*>(expr)) {
            for (auto* e : seq->elements) walk(e, owner);
        } else if (auto* ch = dynamic_cast<Choice*>(expr)) {
            for (auto* alt : ch->alternatives) walk(alt, owner);
        } else if (auto* s = dynamic_cast<Star*>(expr)) walk(s->body, owner);
        else if (auto* p = dynamic_cast<Plus*>(expr)) walk(p->body, owner);
        else if (auto* o = dynamic_cast<Optional*>(expr)) walk(o->body, owner);
        else if (auto* g = dynamic_cast<Group*>(expr)) walk(g->body, owner);
        else if (auto* r = dynamic_cast<Resolver*>(expr)) walk(r->body, owner);
    };
    for (auto* p : grammar->productions) walk(p->body, p->name);
}

// ═══════════════════════════════════════════════════════════════
// Phase 10: Unused tokens and charsets
// ═══════════════════════════════════════════════════════════════

void PegAnalysis::collect_all_refs(PegExpr* expr,
                                     std::set<std::string>& tokens,
                                     std::set<std::string>& charsets,
                                     std::set<std::string>& rules) {
    if (!expr) return;
    if (auto* rc = dynamic_cast<RuleCall*>(expr)) {
        if (charset_names_.count(rc->name)) charsets.insert(rc->name);
        else if (token_names_.count(rc->name)) tokens.insert(rc->name);
        else rules.insert(rc->name);
    } else if (auto* csr = dynamic_cast<CharsetRef*>(expr)) {
        charsets.insert(csr->name);
    } else if (auto* tkr = dynamic_cast<TokenRef*>(expr)) {
        tokens.insert(tkr->name);
    } else if (auto* seq = dynamic_cast<Sequence*>(expr)) {
        for (auto* e : seq->elements) collect_all_refs(e, tokens, charsets, rules);
    } else if (auto* ch = dynamic_cast<Choice*>(expr)) {
        for (auto* a : ch->alternatives) collect_all_refs(a, tokens, charsets, rules);
    } else if (auto* s = dynamic_cast<Star*>(expr)) collect_all_refs(s->body, tokens, charsets, rules);
    else if (auto* p = dynamic_cast<Plus*>(expr)) collect_all_refs(p->body, tokens, charsets, rules);
    else if (auto* o = dynamic_cast<Optional*>(expr)) collect_all_refs(o->body, tokens, charsets, rules);
    else if (auto* a = dynamic_cast<And*>(expr)) collect_all_refs(a->body, tokens, charsets, rules);
    else if (auto* n = dynamic_cast<Not*>(expr)) collect_all_refs(n->body, tokens, charsets, rules);
    else if (auto* g = dynamic_cast<Group*>(expr)) collect_all_refs(g->body, tokens, charsets, rules);
    else if (auto* r = dynamic_cast<Resolver*>(expr)) collect_all_refs(r->body, tokens, charsets, rules);
}

void PegAnalysis::detect_unused_terminals(Grammar* grammar) {
    std::set<std::string> used_tokens, used_charsets, used_rules;
    for (auto* p : grammar->productions)
        collect_all_refs(p->body, used_tokens, used_charsets, used_rules);

    // Tokens reference charsets internally — collect those too.
    for (auto* tk : grammar->tokens)
        // Token bodies use TokenExpr, not PegExpr; charset names appear
        // in TCharset nodes. We do a lightweight traversal here.
        (void)tk;  // skipped: token-internal charset references aren't
                    // tracked yet; this avoids false positives on
                    // charsets used only by tokens. (See below.)

    // Walk token bodies to find charset references.
    std::function<void(TokenExpr*)> walk_tk = [&](TokenExpr* te) {
        if (!te) return;
        if (auto* tc = dynamic_cast<TCharset*>(te)) {
            used_charsets.insert(tc->name);
        } else if (auto* ts = dynamic_cast<TSequence*>(te)) {
            for (auto* e : ts->elements) walk_tk(e);
        } else if (auto* ta = dynamic_cast<TAlternation*>(te)) {
            for (auto* e : ta->alternatives) walk_tk(e);
        } else if (auto* st = dynamic_cast<TStar*>(te)) walk_tk(st->body);
        else if (auto* pl = dynamic_cast<TPlus*>(te)) walk_tk(pl->body);
        else if (auto* op = dynamic_cast<TOptional*>(te)) walk_tk(op->body);
    };
    for (auto* tk : grammar->tokens) walk_tk(tk->pattern);

    for (auto* tk : grammar->tokens)
        if (!used_tokens.count(tk->name))
            result_.warnings.push_back(
                "token " + tk->name + " is declared but never used");
    for (auto* cs : grammar->charsets)
        if (!used_charsets.count(cs->name))
            result_.warnings.push_back(
                "charset " + cs->name + " is declared but never used");
}

// ═══════════════════════════════════════════════════════════════
// Phase 11: Topological sort
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
// Phase 12: Unreachable production detection
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
