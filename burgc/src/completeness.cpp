// burgc/src/completeness.cpp — see completeness.h
//
// The analysis is intentionally not a member of BurgGenerator: it
// operates on the immutable BurgAnalysis snapshot and produces a
// report. The caller (BurgGenerator::analyze) folds the report into
// its own errors_/warnings_.
//
// This is BURG's own runtime DP, evaluated symbolically at compile
// time. Each state is (root_terminal, bitset_of_producible_nonterms)
// — the runtime per-node state with cost erased. Transitions match
// rules against a (terminal, child-state-tuple); we re-walk the same
// rules the code emitter does, but compute abstract states instead of
// emitting C++.

#include "completeness.h"
#include "generator.h"
#include "Worklist.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

// Bit i (0-indexed) corresponds to nonterm at a_->nonterms[i] —
// 1-based nonterm_index value minus 1.
using NontermBits = uint64_t;
constexpr int MAX_NONTERMS = 64;  // matches the NontermBits = uint64_t width

inline NontermBits bit(int nt_index_1based) {
    return NontermBits{1} << (nt_index_1based - 1);
}

// ─────────────────────────────────────────────────────────────
// Pattern signature + duplicate detection
// ─────────────────────────────────────────────────────────────

std::string pattern_signature(burg_ast::TreePattern* pat) {
    std::string s = pat->name;
    if (!pat->children.empty()) {
        s += "(";
        for (size_t i = 0; i < pat->children.size(); i++) {
            if (i > 0) s += ", ";
            s += pattern_signature(pat->children[i]);
        }
        s += ")";
    }
    return s;
}

std::string trim_ws(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

void check_duplicate_rules(const BurgAnalysis& a,
                            std::vector<std::string>& errors) {
    using Key = std::tuple<std::string, std::string, std::string>;
    std::map<Key, burg_ast::Rule*> seen;
    for (auto* rule : a.spec->rules) {
        Key k{rule->nonterm,
              pattern_signature(rule->pattern),
              trim_ws(rule->guard)};
        auto it = seen.find(k);
        if (it != seen.end()) {
            std::ostringstream os;
            os << "duplicate rule " << rule->rule_number
               << " (same as rule " << it->second->rule_number << "): "
               << rule->nonterm << ": " << pattern_signature(rule->pattern);
            if (!std::get<2>(k).empty())
                os << " where (" << std::get<2>(k) << ")";
            errors.push_back(os.str());
        } else {
            seen[k] = rule;
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Tree automaton: build, then run completeness + dead-rule checks
// ─────────────────────────────────────────────────────────────

struct StateKey {
    std::string terminal;     // root terminal name
    NontermBits bits;
    bool operator==(const StateKey& o) const {
        return bits == o.bits && terminal == o.terminal;
    }
};
struct StateKeyHash {
    std::size_t operator()(const StateKey& k) const {
        std::size_t h = std::hash<std::string>{}(k.terminal);
        h ^= std::hash<NontermBits>{}(k.bits) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct State {
    StateKey key;
    // contributors[nt_1based] = set of rule_numbers contributing to bit(nt)
    std::map<int, std::set<int>> contributors;
};

// All transitions discovered during the build; each maps a
// (terminal, child-state-tuple) to the resulting state id.
struct TransitionRecord {
    std::string terminal;
    std::vector<int> child_states;
    int dest;
};

// True if a rule's pattern matches when its root terminal is `term`
// and child-position states are `tuple`. Conservative on nested
// grandchildren — see comment block in the function body.
bool rule_matches(const BurgAnalysis& a,
                   burg_ast::Rule* rule,
                   const std::string& term,
                   const std::vector<int>& tuple,
                   const std::vector<State>& states) {
    auto* pat = rule->pattern;
    if (pat->name != term) return false;
    if (pat->children.size() != tuple.size()) return false;

    for (size_t i = 0; i < pat->children.size(); i++) {
        auto* cp = pat->children[i];
        const State& cs = states[tuple[i]];
        if (cp->is_leaf() && a.term_names.count(cp->name)) {
            // Leaf terminal: child state's terminal must equal it.
            if (cs.key.terminal != cp->name) return false;
        } else if (cp->is_leaf()) {
            // Leaf nonterm: child state must have this nt bit set.
            auto it = a.nonterm_index.find(cp->name);
            if (it == a.nonterm_index.end()) return false;
            if (!(cs.key.bits & bit((int)it->second))) return false;
        } else {
            // Compound pattern at child position: child state's
            // terminal must equal the nested root. We don't
            // recursively check grandchildren — the abstract state
            // doesn't carry that info. This is conservative: rules
            // requiring deeply-nested shapes show up as "potentially
            // matches" in the analysis even when at runtime they
            // wouldn't.  For warnings (not errors) this is fine.
            if (cs.key.terminal != cp->name) return false;
        }
    }
    return true;
}

// Apply chain-rule closure to a (bits, contributors) pair until
// fixed point. Done inside the transition before interning so
// equivalent states merge.
//
// Chain rules are recorded as contributors whenever their from_nt
// is set at the state, regardless of whether to_nt was already set
// by an earlier chain. Speculative-guard automaton building merges
// states that would be distinct at runtime (e.g. a LookupK whose
// guards select only one element-typed nonterm), so a chain that
// looks redundant in the merged state is the runtime's only path
// to its to_nt in the narrower state. Crediting all such chains is
// strictly conservative for the dead-rule warning: it can only
// suppress false positives, never introduce false negatives.
void chain_close(const BurgAnalysis& a,
                  NontermBits& bits,
                  std::map<int, std::set<int>>& contributors) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& cr : a.chain_rules) {
            auto from_it = a.nonterm_index.find(cr.from_nt);
            auto to_it = a.nonterm_index.find(cr.to_nt);
            if (from_it == a.nonterm_index.end() ||
                to_it == a.nonterm_index.end()) continue;
            int from_idx = (int)from_it->second;
            int to_idx = (int)to_it->second;
            if (!(bits & bit(from_idx))) continue;
            contributors[to_idx].insert((int)cr.rule_number);
            if (!(bits & bit(to_idx))) {
                bits |= bit(to_idx);
                changed = true;
            }
        }
    }
}

// Compute the transition for (term, child_tuple). Walks all rules
// rooted at term, applies chain closure, returns the final
// (bits, contributors).
void compute_transition(const BurgAnalysis& a,
                         const std::string& term,
                         const std::vector<int>& tuple,
                         const std::vector<State>& states,
                         NontermBits& out_bits,
                         std::map<int, std::set<int>>& out_contribs) {
    out_bits = 0;
    out_contribs.clear();
    for (auto* rule : a.spec->rules) {
        if (rule->pattern->name != term) continue;
        if (!rule_matches(a, rule, term, tuple, states)) continue;
        auto it = a.nonterm_index.find(rule->nonterm);
        if (it == a.nonterm_index.end()) continue;
        int nt_idx = (int)it->second;
        out_bits |= bit(nt_idx);
        out_contribs[nt_idx].insert((int)rule->rule_number);
    }
    chain_close(a, out_bits, out_contribs);
}

// Group root-terminal patterns by (terminal, arity). A single TERM
// can appear with multiple arities across rules (rare but legal).
std::unordered_map<std::string, std::set<int>>
collect_terminal_arities(const BurgAnalysis& a) {
    std::unordered_map<std::string, std::set<int>> out;
    for (auto* rule : a.spec->rules) {
        if (a.term_names.count(rule->pattern->name)) {
            out[rule->pattern->name].insert((int)rule->pattern->children.size());
        }
    }
    return out;
}

// What can possibly appear at position i of a (terminal, arity)
// pattern across all rules? Used to prune the automaton enumeration:
// child states that satisfy no rule's position-i demand can never
// trigger a meaningful missing-rule warning (they'd just produce
// "any state at this position is uncovered" noise).
struct PositionDemand {
    NontermBits accepted_nts = 0;            // bit set iff some rule at pos i wants this nt
    std::set<std::string> accepted_terms;    // terminals allowed at root of child at pos i
};

std::map<std::tuple<std::string, int, int>, PositionDemand>
compute_position_demands(const BurgAnalysis& a, const AsdlSchema* schema) {
    std::map<std::tuple<std::string, int, int>, PositionDemand> out;

    // Heuristic demand: union of what each rule's position i wants.
    // This is what the analysis falls back to when no schema is
    // available, or for terminals where the schema's arity doesn't
    // match the rule pattern's arity (slot-based IRs).
    for (auto* rule : a.spec->rules) {
        if (!a.term_names.count(rule->pattern->name)) continue;
        int arity = (int)rule->pattern->children.size();
        for (size_t i = 0; i < rule->pattern->children.size(); i++) {
            auto* cp = rule->pattern->children[i];
            auto& d = out[{rule->pattern->name, arity, (int)i}];
            if (cp->is_leaf() && a.term_names.count(cp->name)) {
                d.accepted_terms.insert(cp->name);
            } else if (cp->is_leaf()) {
                auto it = a.nonterm_index.find(cp->name);
                if (it != a.nonterm_index.end())
                    d.accepted_nts |= bit((int)it->second);
            } else {
                d.accepted_terms.insert(cp->name);
            }
        }
    }

    // Schema overlay: when a TERM corresponds to an ASDL constructor
    // whose node-typed field count matches the BURG arity, replace
    // the heuristic demand at each position with the precise set of
    // constructors that produce the corresponding node-field type.
    if (!schema) return out;

    // Collect (term, arity) pairs we've seen.
    std::set<std::pair<std::string, int>> term_arities;
    for (auto& [k, _] : out)
        term_arities.insert({std::get<0>(k), std::get<1>(k)});

    for (auto& [term, arity] : term_arities) {
        auto cit = schema->constructor_node_fields.find(term);
        if (cit == schema->constructor_node_fields.end()) continue;
        const auto& node_fields = cit->second;
        if ((int)node_fields.size() != arity) continue;  // slot-based — keep heuristic

        for (int i = 0; i < arity; i++) {
            const std::string& field_type = node_fields[i];
            auto sit = schema->sum_constructors.find(field_type);
            if (sit == schema->sum_constructors.end()) continue;
            // Replace heuristic demand at this position with the
            // precise set: any constructor of `field_type` is OK.
            PositionDemand pd;
            for (auto& ctor : sit->second)
                pd.accepted_terms.insert(ctor);
            out[{term, arity, i}] = pd;
        }
    }

    return out;
}

bool state_satisfies(const State& s, const PositionDemand& d) {
    if (s.key.bits & d.accepted_nts) return true;
    if (d.accepted_terms.count(s.key.terminal)) return true;
    return false;
}

// Build the tree automaton. Returns false if the grammar is too
// large for the analysis (caller emits a diagnostic).
struct Automaton {
    std::vector<State> states;
    std::unordered_map<StateKey, int, StateKeyHash> intern_map;
    std::vector<TransitionRecord> transitions;
    // transitions_to[state_id] = indices into `transitions` whose dest is state_id
    std::vector<std::vector<int>> transitions_to;
    // missing_witnesses: (term, tuple) shapes that landed in the empty state
    std::vector<TransitionRecord> missing_witnesses;
};

bool build_automaton(const BurgAnalysis& a, const AsdlSchema* schema,
                       Automaton& aut) {
    if ((int)a.nonterms.size() > MAX_NONTERMS) return false;

    auto term_arities = collect_terminal_arities(a);
    auto pos_demands = compute_position_demands(a, schema);
    bbq::Worklist<int> wl;

    auto intern = [&](const StateKey& key,
                       std::map<int, std::set<int>> contribs) -> int {
        auto it = aut.intern_map.find(key);
        if (it != aut.intern_map.end()) {
            // Merge contributors into existing.
            State& s = aut.states[it->second];
            for (auto& [nt, rs] : contribs)
                s.contributors[nt].insert(rs.begin(), rs.end());
            return it->second;
        }
        int id = (int)aut.states.size();
        aut.states.push_back({key, std::move(contribs)});
        aut.transitions_to.emplace_back();
        aut.intern_map[key] = id;
        wl.push(id);
        return id;
    };

    auto record_transition = [&](const std::string& term,
                                   const std::vector<int>& tuple,
                                   int dest) {
        int trans_idx = (int)aut.transitions.size();
        aut.transitions.push_back({term, tuple, dest});
        aut.transitions_to[dest].push_back(trans_idx);
        if (aut.states[dest].key.bits == 0) {
            aut.missing_witnesses.push_back({term, tuple, dest});
        }
    };

    // Seed with arity-0 transitions for every terminal that appears
    // with arity 0 in some rule.
    for (auto& [term, arities] : term_arities) {
        if (arities.count(0)) {
            NontermBits bits;
            std::map<int, std::set<int>> contribs;
            compute_transition(a, term, {}, aut.states, bits, contribs);
            // Empty-tuple transition: state's terminal is `term`.
            int s = intern({term, bits}, contribs);
            record_transition(term, {}, s);
        }
    }

    // Worklist: each new state may unlock new (term, tuple)
    // transitions. We re-enumerate over discovered states each pop;
    // the transition cache (intern_map) ensures we only do real work
    // for newly-reachable tuples.
    auto enumerate_tuples = [&](const std::string& term, int arity,
                                  std::vector<int>& tuple,
                                  size_t pos,
                                  auto& self) -> void {
        if ((int)tuple.size() == arity) {
            NontermBits bits;
            std::map<int, std::set<int>> contribs;
            compute_transition(a, term, tuple, aut.states, bits, contribs);
            int dest = intern({term, bits}, contribs);
            record_transition(term, tuple, dest);
            return;
        }
        // Per-position demand filter: only enumerate child states
        // that some rule at this (term, arity, pos) could plausibly
        // accept. Without ASDL we use this as a heuristic for
        // "states the consumer's IR would meaningfully construct
        // at this position" — drops obvious false-positive shapes
        // like Add(<Halt>, ...) where Halt isn't expected at any
        // Add-rule's position 0 or 1.
        auto demand_it = pos_demands.find({term, arity, (int)pos});
        for (int s = 0; s < (int)aut.states.size(); s++) {
            if (demand_it != pos_demands.end() &&
                !state_satisfies(aut.states[s], demand_it->second))
                continue;
            tuple.push_back(s);
            self(term, arity, tuple, pos + 1, self);
            tuple.pop_back();
        }
    };

    while (!wl.empty()) {
        wl.pop();  // we just need progress — full re-enumerate below
        // For each (terminal, arity > 0), enumerate all tuples of
        // discovered states. New states get added inside intern();
        // the worklist drives further iteration until quiescent.
        size_t states_before = aut.states.size();
        for (auto& [term, arities] : term_arities) {
            for (int arity : arities) {
                if (arity == 0) continue;
                std::vector<int> tuple;
                enumerate_tuples(term, arity, tuple, 0, enumerate_tuples);
            }
        }
        if (aut.states.size() == states_before) break;  // quiescent
    }

    return true;
}

// Render a state for human-readable warnings: the root terminal plus
// the set of nonterms it can produce, e.g. "<reg-producing Const>".
std::string describe_state(const BurgAnalysis& a, const State& s) {
    std::ostringstream os;
    os << s.key.terminal;
    if (s.key.bits) {
        os << " producing {";
        bool first = true;
        for (size_t i = 0; i < a.nonterms.size(); i++) {
            if (s.key.bits & bit((int)(i + 1))) {
                if (!first) os << ",";
                os << a.nonterms[i];
                first = false;
            }
        }
        os << "}";
    } else {
        os << " producing nothing";
    }
    return os.str();
}

void report_missing_witnesses(const BurgAnalysis& a,
                                const Automaton& aut,
                                std::vector<std::string>& warnings) {
    if (aut.missing_witnesses.empty()) return;

    // Filter: skip witnesses where any child is itself an empty-bits
    // (uncovered) state. Those children are already reported as
    // missing on their own, so the parent shape is just derivative
    // noise — and without ASDL we can't tell whether the empty-bits
    // child shape is even constructible by the consumer's IR.
    std::map<std::string, std::set<std::string>> by_term;
    for (auto& w : aut.missing_witnesses) {
        bool has_empty_child = false;
        for (int cs : w.child_states) {
            if (aut.states[cs].key.bits == 0) { has_empty_child = true; break; }
        }
        if (has_empty_child) continue;

        std::ostringstream os;
        os << w.terminal << "(";
        for (size_t i = 0; i < w.child_states.size(); i++) {
            if (i > 0) os << ", ";
            os << "<" << describe_state(a, aut.states[w.child_states[i]]) << ">";
        }
        os << ")";
        by_term[w.terminal].insert(os.str());
    }

    for (auto& [term, shapes] : by_term) {
        std::ostringstream os;
        os << "terminal " << term << " has uncovered child shapes "
           << "(no rule matches; liveness is computed assuming guards may fire):";
        for (auto& shape : shapes) os << "\n    - " << shape;
        warnings.push_back(os.str());
    }
}

// Same-state demand closure: a demand for nt implies a demand for
// every chain-rule predecessor of nt at the same state. (When
// bit(nt) was produced via a chain rule X→nt at this state, the
// chain rule depends on bit(X) being present; demanding X here
// gives credit to whatever rule produced bit(X).)
void chain_demand_close(const BurgAnalysis& a, NontermBits& demanded) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& cr : a.chain_rules) {
            auto from_it = a.nonterm_index.find(cr.from_nt);
            auto to_it = a.nonterm_index.find(cr.to_nt);
            if (from_it == a.nonterm_index.end() ||
                to_it == a.nonterm_index.end()) continue;
            int from_idx = (int)from_it->second;
            int to_idx = (int)to_it->second;
            if ((demanded & bit(to_idx)) && !(demanded & bit(from_idx))) {
                demanded |= bit(from_idx);
                changed = true;
            }
        }
    }
}

// Compute per-(state, nt) demand. demanded[s] is the bitset of nts
// that are actually consumed at state s along some path from a
// START-rooted derivation. A rule is dead iff it never contributes
// to a demanded (state, nt) pair.
std::vector<NontermBits> compute_demand(const BurgAnalysis& a,
                                          const Automaton& aut) {
    std::vector<NontermBits> demanded(aut.states.size(), 0);
    auto start_it = a.nonterm_index.find(a.start_nonterm);
    if (start_it == a.nonterm_index.end()) return demanded;
    int start_idx = (int)start_it->second;

    // Seed: every state where bit(start_nt) is set is a candidate
    // root, demanding start_nt at that state.
    for (size_t i = 0; i < aut.states.size(); i++) {
        if (aut.states[i].key.bits & bit(start_idx)) {
            demanded[i] |= bit(start_idx);
            chain_demand_close(a, demanded[i]);
        }
    }

    // Index rules by number for O(1) lookup during propagation.
    std::unordered_map<int, burg_ast::Rule*> rule_by_num;
    for (auto* r : a.spec->rules) rule_by_num[(int)r->rule_number] = r;

    // Backward propagation through transitions to fixpoint.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& t : aut.transitions) {
            NontermBits dest_demand = demanded[t.dest];
            if (!dest_demand) continue;
            const State& dest = aut.states[t.dest];
            for (auto& [nt_idx, rule_set] : dest.contributors) {
                if (!(dest_demand & bit(nt_idx))) continue;
                for (int rnum : rule_set) {
                    auto rit = rule_by_num.find(rnum);
                    if (rit == rule_by_num.end()) continue;
                    auto* pat = rit->second->pattern;
                    // Chain rule (leaf non-terminal pattern) — its
                    // intra-state demand is handled by
                    // chain_demand_close at every state, so no
                    // propagation needed here.
                    if (pat->is_leaf() && !a.term_names.count(pat->name))
                        continue;
                    if (pat->children.size() != t.child_states.size()) continue;
                    for (size_t i = 0; i < pat->children.size(); i++) {
                        auto* cp = pat->children[i];
                        if (cp->is_leaf() && !a.term_names.count(cp->name)) {
                            auto cit = a.nonterm_index.find(cp->name);
                            if (cit == a.nonterm_index.end()) continue;
                            NontermBits new_demand = bit((int)cit->second);
                            int csid = t.child_states[i];
                            NontermBits before = demanded[csid];
                            NontermBits after = before | new_demand;
                            chain_demand_close(a, after);
                            if (after != before) {
                                demanded[csid] = after;
                                changed = true;
                            }
                        }
                    }
                }
            }
        }
    }
    return demanded;
}

void report_dead_rules(const BurgAnalysis& a,
                        const Automaton& aut,
                        std::vector<std::string>& warnings) {
    auto demanded = compute_demand(a, aut);

    // A rule is live iff some state's demanded set contains an nt
    // that the rule contributed to.
    std::set<int> live_rules;
    for (size_t s = 0; s < aut.states.size(); s++) {
        NontermBits d = demanded[s];
        if (!d) continue;
        for (auto& [nt_idx, rs] : aut.states[s].contributors) {
            if (d & bit(nt_idx)) {
                live_rules.insert(rs.begin(), rs.end());
            }
        }
    }

    for (auto* rule : a.spec->rules) {
        if (live_rules.count((int)rule->rule_number)) continue;
        std::ostringstream os;
        os << "rule " << rule->rule_number << " is dead (no demand for "
           << rule->nonterm << " from a "
           << a.start_nonterm << "-rooted derivation): "
           << rule->nonterm << ": " << pattern_signature(rule->pattern);
        if (!rule->guard.empty())
            os << " where (...)";
        warnings.push_back(os.str());
    }
}

}  // namespace

bool load_asdl_schema(const std::string& path,
                       AsdlSchema& out,
                       std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open ASDL JSON: " + path;
        return false;
    }
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        error = "ASDL JSON parse error: " + std::string(e.what());
        return false;
    }

    // Walk the asdl --json structure: definitions[].types[].fields[].
    // For each constructor (sum type's variant), record the field
    // types whose type names match a defined sum-type — those are
    // structural (node) children. Primitive-typed fields (int64,
    // string, etc.) are data, not children.
    std::set<std::string> sum_type_names;
    if (doc.contains("definitions")) {
        for (auto& def : doc["definitions"]) {
            if (def.value("type", "") == "sum")
                sum_type_names.insert(def.value("name", ""));
        }
    }

    if (doc.contains("definitions")) {
        for (auto& def : doc["definitions"]) {
            if (def.value("type", "") != "sum") continue;
            std::string sum_name = def.value("name", "");
            if (!def.contains("types")) continue;
            std::vector<std::string> ctors;
            for (auto& ctor : def["types"]) {
                std::string cname = ctor.value("name", "");
                ctors.push_back(cname);
                std::vector<std::string> node_fields;
                if (ctor.contains("fields")) {
                    for (auto& fld : ctor["fields"]) {
                        std::string ftype = fld.value("type", "");
                        if (sum_type_names.count(ftype))
                            node_fields.push_back(ftype);
                    }
                }
                out.constructor_node_fields[cname] = std::move(node_fields);
            }
            out.sum_constructors[sum_name] = std::move(ctors);
        }
    }
    return true;
}

AnalysisReport run_completeness_analysis(const BurgAnalysis& a,
                                           const AnalysisConfig& cfg) {
    AnalysisReport report;

    // 1. Duplicate rules — error. Cheap, no automaton needed.
    check_duplicate_rules(a, report.errors);

    // Bail before the expensive automaton build if the caller has
    // explicitly opted out. Routine codegen-only runs use this; the
    // final CI gate runs without it.
    if (cfg.skip_dead_rule_analysis)
        return report;

    // 2. Tree automaton — needed for both missing-rule and dead-rule
    // checks. Schema-aware filter when cfg.asdl is supplied;
    // heuristic otherwise.
    Automaton aut;
    if (!build_automaton(a, cfg.asdl, aut)) {
        report.warnings.push_back(
            "grammar exceeds NontermBits width (MAX_NONTERMS=" +
            std::to_string(MAX_NONTERMS) +
            ") — completeness analysis skipped");
        return report;
    }

    // Missing-rule (shape-enumeration) warnings are gated: without an
    // ASDL schema (zany-painting-spark.md), enumeration produces
    // false positives on shapes the consumer's IR could never
    // construct. Off by default; --coverage opts in.
    if (cfg.emit_coverage_warnings)
        report_missing_witnesses(a, aut, report.warnings);

    // Dead-rule warnings are precise (they only require the
    // contributors map, no shape-enumeration noise) and useful even
    // without ASDL. Always on (unless skip_dead_rule_analysis above).
    report_dead_rules(a, aut, report.warnings);

    return report;
}
