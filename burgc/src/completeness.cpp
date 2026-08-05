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

// ─────────────────────────────────────────────────────────────
// Representer states (Proebsting, "BURS Automata Generation",
// TOPLAS 17(3) 1995, §3.3 Fig. 11 Project(), after Chase 1987)
// ─────────────────────────────────────────────────────────────
//
// Rule matching at child position i of an operator consults exactly
// two things about the child state: whether it carries the nonterminal
// bits some rule's nonterminal-leaf demands there, and whether its
// terminal equals one some rule's terminal-leaf (or nested pattern
// root) demands there. Everything else — the terminal when nothing
// demands it, bits no rule wants at that position — cannot affect any
// transition. So states are projected per (operator, dimension) into
// equivalence classes keyed by only that information, and transitions
// are computed over CLASS tuples, not raw-state tuples. On this
// grammar's worst operator (arity 5, ~40 raw i32-producing states per
// position) that is the difference between 40^5 tuples and 1.
struct RepKey {
    NontermBits bits;        // full bits ∩ the position's nonterminal demand
    std::string terminal;    // the terminal, iff it is demanded at the position
    bool operator==(const RepKey& o) const {
        return bits == o.bits && terminal == o.terminal;
    }
};
struct RepKeyHash {
    std::size_t operator()(const RepKey& k) const {
        std::size_t h = std::hash<std::string>{}(k.terminal);
        h ^= std::hash<NontermBits>{}(k.bits) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct Rep {
    RepKey key;
    std::vector<int> members;        // state ids projecting to this class
    // Some member's FULL bit set is nonzero — i.e. the class is inhabited by a
    // state that is itself covered. Witnesses through all-uncovered classes are
    // derivative noise (the child is already reported) and are filtered.
    bool any_covered_member = false;
};

// One dimension (child position) of an operator. `match_*` is what the RULES
// can consult at this position — the projection key is built from it, so it
// must never be widened past the rules. `enum_*` is which STATES are worth
// enumerating here at all — the heuristic demand, or the ASDL overlay when the
// schema matches (a strictly wider net that surfaces shapes the heuristic
// can't; see AsdlSchemaTightensDemandFilter). Conflating the two would key
// classes on demands no rule reads and break matching.
struct OpDim {
    NontermBits match_nts = 0;
    std::set<std::string> match_terms;
    NontermBits enum_nts = 0;
    std::set<std::string> enum_terms;
    std::vector<Rep> reps;
    std::unordered_map<RepKey, int, RepKeyHash> intern;
};

struct OpInfo {
    std::string term;
    int arity;
    std::vector<OpDim> dims;
};

// A transition, recorded once per (operator, representer-tuple). `op` is -1
// for the arity-0 leaf seeds (empty tuple).
struct TransitionRecord {
    std::string terminal;
    int op;
    std::vector<int> child_reps;
    int dest;
};

// True if a rule's pattern matches when its root terminal is `term` and the
// child positions hold states of the given representer classes. The checks
// read exactly the information RepKey retains — which is the argument that the
// projection is sound. Conservative on nested grandchildren, as before: a
// compound child pattern checks only the nested root's terminal.
bool rule_matches(const BurgAnalysis& a,
                   burg_ast::Rule* rule,
                   const std::string& term,
                   const std::vector<RepKey>& tuple) {
    auto* pat = rule->pattern;
    if (pat->name != term) return false;
    if (pat->children.size() != tuple.size()) return false;

    for (size_t i = 0; i < pat->children.size(); i++) {
        auto* cp = pat->children[i];
        const RepKey& ck = tuple[i];
        if (cp->is_leaf() && a.term_names.count(cp->name)) {
            if (ck.terminal != cp->name) return false;
        } else if (cp->is_leaf()) {
            auto it = a.nonterm_index.find(cp->name);
            if (it == a.nonterm_index.end()) return false;
            if (!(ck.bits & bit((int)it->second))) return false;
        } else {
            if (ck.terminal != cp->name) return false;
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

// Compute the transition for (term, representer-tuple). Walks all rules
// rooted at term, applies chain closure, returns the final
// (bits, contributors).
void compute_transition(const BurgAnalysis& a,
                         const std::string& term,
                         const std::vector<RepKey>& tuple,
                         NontermBits& out_bits,
                         std::map<int, std::set<int>>& out_contribs) {
    out_bits = 0;
    out_contribs.clear();
    for (auto* rule : a.spec->rules) {
        if (rule->pattern->name != term) continue;
        if (!rule_matches(a, rule, term, tuple)) continue;
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

// Build the tree automaton. Returns false if the grammar is too
// large for the analysis (caller emits a diagnostic).
struct Automaton {
    std::vector<State> states;
    std::unordered_map<StateKey, int, StateKeyHash> intern_map;
    std::vector<OpInfo> ops;
    std::vector<TransitionRecord> transitions;
    // missing_witnesses: (term, rep-tuple) shapes that landed in the empty state
    std::vector<TransitionRecord> missing_witnesses;
    std::size_t tuples_evaluated = 0;
};

// The worklist algorithm of Proebsting Fig. 5 / Fig. 12, cost-erased. Pop a
// state; for each (operator, dimension) it is usable in, project it to its
// representer class. Only when a dimension gains a NEW class are transitions
// enumerated — combinations of that class with the other dimensions' existing
// classes — so each (operator, representer-tuple) is computed and recorded
// exactly once. A state joining an existing class does no enumeration at all:
// the class's transitions already speak for it. (The old form popped a state,
// re-enumerated every raw-state tuple of every operator, and re-recorded every
// transition each round — |states|^arity work and memory per round, which on
// codegen_wasm.burg meant gigabytes of duplicate records and no answer.)
bool build_automaton(const BurgAnalysis& a, const AsdlSchema* schema,
                       Automaton& aut) {
    if ((int)a.nonterms.size() > MAX_NONTERMS) return false;

    auto term_arities = collect_terminal_arities(a);
    auto pos_demands = compute_position_demands(a, schema);
    bbq::Worklist<int> wl;

    // The operator table, with per-dimension demands. `match_*` comes from the
    // rules alone (it keys the projection); `enum_*` is the possibly
    // schema-widened demand from compute_position_demands (it gates which
    // states are enumerable at the position).
    for (auto& [term, arities] : term_arities) {
        for (int arity : arities) {
            if (arity == 0) continue;
            OpInfo op;
            op.term = term;
            op.arity = arity;
            op.dims.resize(arity);
            for (auto* rule : a.spec->rules) {
                if (rule->pattern->name != term) continue;
                if ((int)rule->pattern->children.size() != arity) continue;
                for (int i = 0; i < arity; i++) {
                    auto* cp = rule->pattern->children[i];
                    OpDim& d = op.dims[i];
                    if (cp->is_leaf() && a.term_names.count(cp->name)) {
                        d.match_terms.insert(cp->name);
                    } else if (cp->is_leaf()) {
                        auto it = a.nonterm_index.find(cp->name);
                        if (it != a.nonterm_index.end())
                            d.match_nts |= bit((int)it->second);
                    } else {
                        d.match_terms.insert(cp->name);
                    }
                }
            }
            for (int i = 0; i < arity; i++) {
                auto dit = pos_demands.find({term, arity, i});
                if (dit != pos_demands.end()) {
                    op.dims[i].enum_nts = dit->second.accepted_nts;
                    op.dims[i].enum_terms = dit->second.accepted_terms;
                }
            }
            aut.ops.push_back(std::move(op));
        }
    }

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
        aut.intern_map[key] = id;
        wl.push(id);
        return id;
    };

    auto record_transition = [&](const std::string& term, int op_idx,
                                   const std::vector<int>& reps,
                                   int dest) {
        aut.transitions.push_back({term, op_idx, reps, dest});
        if (aut.states[dest].key.bits == 0) {
            aut.missing_witnesses.push_back({term, op_idx, reps, dest});
        }
    };

    // Seed with arity-0 transitions for every terminal that appears
    // with arity 0 in some rule (ComputeLeafStates, Fig. 9).
    for (auto& [term, arities] : term_arities) {
        if (arities.count(0)) {
            NontermBits bits;
            std::map<int, std::set<int>> contribs;
            compute_transition(a, term, {}, bits, contribs);
            int s = intern({term, bits}, contribs);
            record_transition(term, -1, {}, s);
        }
    }

    // Enumerate all rep-tuples of `op` with dimension `fixed_dim` pinned to
    // `fixed_rep` and every other dimension ranging over its existing classes.
    // Called exactly once per new class, so a tuple is evaluated exactly once —
    // at the creation of the last-created class among its components.
    auto enumerate_with = [&](int op_idx, int fixed_dim, int fixed_rep) {
        OpInfo& op = aut.ops[op_idx];
        std::vector<int> chosen(op.arity);
        std::vector<RepKey> keys(op.arity);
        auto rec = [&](int d, auto& self) -> void {
            if (d == op.arity) {
                aut.tuples_evaluated++;
                NontermBits bits;
                std::map<int, std::set<int>> contribs;
                compute_transition(a, op.term, keys, bits, contribs);
                int dest = intern({op.term, bits}, contribs);
                record_transition(op.term, op_idx, chosen, dest);
                return;
            }
            if (d == fixed_dim) {
                chosen[d] = fixed_rep;
                keys[d] = op.dims[d].reps[fixed_rep].key;
                self(d + 1, self);
                return;
            }
            for (int r = 0; r < (int)op.dims[d].reps.size(); r++) {
                chosen[d] = r;
                keys[d] = op.dims[d].reps[r].key;
                self(d + 1, self);
            }
        };
        rec(0, rec);
    };

    while (!wl.empty()) {
        int s = wl.pop();
        // Copy out of the states vector: intern() during enumeration may grow
        // it and invalidate references.
        const NontermBits s_bits = aut.states[s].key.bits;
        const std::string s_term = aut.states[s].key.terminal;

        for (int op_idx = 0; op_idx < (int)aut.ops.size(); op_idx++) {
            OpInfo& op = aut.ops[op_idx];
            for (int d = 0; d < op.arity; d++) {
                OpDim& dim = op.dims[d];
                // Enumerability: some rule (or the schema) plausibly accepts
                // this state at this position.
                if (!(s_bits & dim.enum_nts) && !dim.enum_terms.count(s_term))
                    continue;
                RepKey rk{s_bits & dim.match_nts,
                          dim.match_terms.count(s_term) ? s_term : std::string()};
                auto it = dim.intern.find(rk);
                if (it != dim.intern.end()) {
                    Rep& rep = dim.reps[it->second];
                    rep.members.push_back(s);
                    rep.any_covered_member |= (s_bits != 0);
                    continue;   // existing class: its transitions already stand
                }
                int rep_id = (int)dim.reps.size();
                dim.reps.push_back({rk, {s}, s_bits != 0});
                dim.intern[rk] = rep_id;
                enumerate_with(op_idx, d, rep_id);
            }
        }
    }

    return true;
}

// Render a representer class for human-readable warnings. A class stands for
// every state projecting to it at this position, so it reads as a description
// of what CAN sit there, with one member as a concrete example.
std::string describe_rep(const BurgAnalysis& a, const Automaton& aut,
                          const Rep& rep) {
    std::ostringstream os;
    if (!rep.key.terminal.empty()) os << rep.key.terminal;
    if (rep.key.bits) {
        if (!rep.key.terminal.empty()) os << " ";
        os << "producing {";
        bool first = true;
        for (size_t i = 0; i < a.nonterms.size(); i++) {
            if (rep.key.bits & bit((int)(i + 1))) {
                if (!first) os << ",";
                os << a.nonterms[i];
                first = false;
            }
        }
        os << "}";
    } else if (rep.key.terminal.empty()) {
        os << "producing nothing usable here";
    }
    if (!rep.members.empty() && rep.key.terminal.empty()) {
        os << " (e.g. " << aut.states[rep.members[0]].key.terminal;
        if (rep.members.size() > 1)
            os << ", " << (rep.members.size() - 1) << " more";
        os << ")";
    }
    return os.str();
}

void report_missing_witnesses(const BurgAnalysis& a,
                                const Automaton& aut,
                                std::vector<std::string>& warnings) {
    if (aut.missing_witnesses.empty()) return;

    // Filter: skip witnesses where some child class contains no covered state
    // at all. Those children are already reported as missing on their own, so
    // the parent shape is just derivative noise — and without ASDL we can't
    // tell whether such a child shape is even constructible by the consumer's
    // IR.
    std::map<std::string, std::set<std::string>> by_term;
    for (auto& w : aut.missing_witnesses) {
        bool has_uncovered_child = false;
        for (size_t i = 0; i < w.child_reps.size(); i++) {
            const Rep& rep = aut.ops[w.op].dims[i].reps[w.child_reps[i]];
            if (!rep.any_covered_member) { has_uncovered_child = true; break; }
        }
        if (has_uncovered_child) continue;

        std::ostringstream os;
        os << w.terminal << "(";
        for (size_t i = 0; i < w.child_reps.size(); i++) {
            if (i > 0) os << ", ";
            os << "<" << describe_rep(a, aut, aut.ops[w.op].dims[i].reps[w.child_reps[i]])
               << ">";
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

    // Seed: every state producing a GOAL is a candidate root for that goal.
    // START is the root's goal; each declared ENTRY is a goal the consumer
    // reduces to by name at a frontier its own code owns (iburg p.8's
    // `rule(state, goalnt)` — goal-directed reduction is the formalism, so a
    // hand-written extension of Reduce() demanding another nonterminal is
    // ordinary use, not a special case). Undeclared, such a family is
    // unreachable from START and correctly reported dead; declared, it is
    // seeded here and its rules are live.
    std::vector<int> goals{(int)start_it->second};
    for (auto& e : a.entries) {
        auto it = a.nonterm_index.find(e);
        if (it == a.nonterm_index.end()) continue;   // analyze() already errored
        int idx = (int)it->second;
        if (idx != goals[0]) goals.push_back(idx);
    }

    for (size_t i = 0; i < aut.states.size(); i++) {
        for (int g : goals) {
            if (aut.states[i].key.bits & bit(g)) demanded[i] |= bit(g);
        }
        if (demanded[i]) chain_demand_close(a, demanded[i]);
    }

    // Index rules by number for O(1) lookup during propagation.
    std::unordered_map<int, burg_ast::Rule*> rule_by_num;
    for (auto* r : a.spec->rules) rule_by_num[(int)r->rule_number] = r;

    // Backward propagation through transitions to fixpoint. A transition's
    // children are representer CLASSES; a demand on a child position is a
    // demand on every state in that class — two states in one class are
    // interchangeable for matching but carry different contributors (different
    // terminals), so crediting only a representative member would report its
    // classmates' rules as falsely dead.
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
                    if (pat->children.size() != t.child_reps.size()) continue;
                    for (size_t i = 0; i < pat->children.size(); i++) {
                        auto* cp = pat->children[i];
                        if (cp->is_leaf() && !a.term_names.count(cp->name)) {
                            auto cit = a.nonterm_index.find(cp->name);
                            if (cit == a.nonterm_index.end()) continue;
                            NontermBits new_demand = bit((int)cit->second);
                            const Rep& rep =
                                aut.ops[t.op].dims[i].reps[t.child_reps[i]];
                            for (int csid : rep.members) {
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
    //
    // An ENUM-LIKE sum — every constructor nullary — is data too, and the
    // `is_enum` flag on the definition is asdl's own answer, carried in
    // the sidecar for exactly this. Reading it keeps one definition of
    // enum-ness; re-deriving it here would be a second one to drift from.
    // It matters because such a field IS a field: `Add(datatype
    // data_type, node left, node right)` has two children and three
    // fields, and counting the discriminant as a child makes every arity
    // check off by one for the consumers whose IR carries types on its
    // nodes.
    std::set<std::string> sum_type_names;
    if (doc.contains("definitions")) {
        for (auto& def : doc["definitions"]) {
            if (def.value("type", "") != "sum") continue;
            if (def.value("is_enum", false)) continue;
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
    report.stats.states = aut.states.size();
    report.stats.transitions = aut.transitions.size();
    report.stats.tuples_evaluated = aut.tuples_evaluated;
    for (auto& op : aut.ops)
        for (auto& dim : op.dims)
            report.stats.representers += dim.reps.size();

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
