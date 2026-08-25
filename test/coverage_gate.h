#pragma once
//
// Coverage gates over a fixture: every rule, every variant ARM, both sides of every
// optional.
//
// A table that lists every RULE looks complete and is not: a rule with three switch
// arms and one input leaves two thirds of it unparsed, and which arm was taken is what
// decides how the bytes after it are read. So the denominator comes from the grammar
// and the numerator from what the parser recorded.
//
// The two variant shapes differ in where the tag lands, and the generated which() shows
// it: a SWITCH tags one field with the ordinal it took, so its arms are (field, 0..N-1);
// a UNION or ALTERNATIVES tags the variant NODE that is present, so its arms are
// (variant name, its ordinal) and only one exists per parse. An OPTIONAL carries no tag
// at all — the arm IS whether the node is there, and the absent half is the one that
// moves everything after it.
//
#include "BBQ_AST.h"
#include "CaptureCow.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace coverage {

// Every (node name, arm ordinal) a rule declares. A ruleref is not descended into: the
// rule it names carries its own arms.
inline void declared_arms(BBQ::TypeExpr* t, const std::string& field,
                          std::vector<std::pair<std::string, int>>& out) {
    if (!t) return;
    switch (t->node_kind()) {
    case BBQ::NodeKind::Switch: {
        auto* s = static_cast<BBQ::Switch*>(t);
        int arms = (int)s->cases.size() + (s->default_ ? 1 : 0);
        for (int a = 0; a < arms; a++) out.emplace_back(field, a);
        for (auto* c : s->cases) declared_arms(c->target, field, out);
        if (s->default_ && (*s->default_)->target)
            declared_arms(*(*s->default_)->target, field, out);
        return;
    }
    case BBQ::NodeKind::Union: {
        auto* u = static_cast<BBQ::Union*>(t);
        for (size_t i = 0; i < u->variants.size(); i++) {
            out.emplace_back(u->variants[i]->name, (int)i);
            declared_arms(u->variants[i]->body, u->variants[i]->name, out);
        }
        return;
    }
    case BBQ::NodeKind::Alternatives: {
        auto* a = static_cast<BBQ::Alternatives*>(t);
        for (size_t i = 0; i < a->alts.size(); i++)
            out.emplace_back("alt_" + std::to_string(i), (int)i);
        return;
    }
    case BBQ::NodeKind::Struct:
        for (auto* f : static_cast<BBQ::Struct*>(t)->fields)
            declared_arms(f->body, f->name, out);
        return;
    case BBQ::NodeKind::Array:
        declared_arms(static_cast<BBQ::Array*>(t)->element, field, out);
        return;
    case BBQ::NodeKind::Optional:
        declared_arms(static_cast<BBQ::Optional*>(t)->element, field, out);
        return;
    default:
        return;
    }
}

// Which arms a parse actually took: field name -> the tags seen at it. An unnamed
// tagged node is the rule's own top level, filed under "".
inline void taken_arms(const bbq::zcow::node* n, const char* name,
                       std::map<std::string, std::set<int>>& out) {
    if (!n) return;
    if (n->variant_tag >= 0) out[name ? name : ""].insert(n->variant_tag);
    for (const auto& k : n->kids) taken_arms(k.get(), k->name ? k->name : name, out);
}

inline void declared_optionals(BBQ::TypeExpr* t, const std::string& field,
                               std::vector<std::string>& out) {
    if (!t) return;
    switch (t->node_kind()) {
    case BBQ::NodeKind::Optional:
        out.push_back(field);
        declared_optionals(static_cast<BBQ::Optional*>(t)->element, field, out);
        return;
    case BBQ::NodeKind::Struct:
        for (auto* f : static_cast<BBQ::Struct*>(t)->fields)
            declared_optionals(f->body, f->name, out);
        return;
    case BBQ::NodeKind::Array:
        declared_optionals(static_cast<BBQ::Array*>(t)->element, field, out);
        return;
    case BBQ::NodeKind::Switch: {
        auto* s = static_cast<BBQ::Switch*>(t);
        for (auto* c : s->cases) declared_optionals(c->target, field, out);
        if (s->default_ && (*s->default_)->target)
            declared_optionals(*(*s->default_)->target, field, out);
        return;
    }
    case BBQ::NodeKind::Union:
        for (auto* v : static_cast<BBQ::Union*>(t)->variants)
            declared_optionals(v->body, v->name, out);
        return;
    default:
        return;
    }
}

inline void present_names(const bbq::zcow::node* n, std::set<std::string>& out) {
    if (!n) return;
    if (n->name) out.insert(n->name);
    for (const auto& k : n->kids) present_names(k.get(), out);
}

// What a fixture's inputs managed to reach, per rule: the arms taken, and the field
// names each successful parse produced.
struct Reached {
    std::map<std::string, std::set<int>> arms;
    std::vector<std::set<std::string>> present;   // one entry per successful parse
};

inline void observe(const bbq::zcow::node* root, Reached& r) {
    taken_arms(root, nullptr, r.arms);
    std::set<std::string> names;
    present_names(root, names);
    r.present.push_back(std::move(names));
}

// The three verdicts. Each returns "" when the fixture is covered, or a comma-separated
// list of what nothing exercises.
inline std::string missing_rules(const std::vector<BBQ::Rule*>& rules,
                                 const std::set<std::string>& has_case) {
    std::string out;
    for (auto* r : rules)
        if (!has_case.count(r->name)) out += (out.empty() ? "" : ", ") + r->name;
    return out;
}

inline std::string missing_arms(const std::vector<BBQ::Rule*>& rules,
                                std::map<std::string, Reached>& reached) {
    std::string out;
    for (auto* r : rules) {
        std::vector<std::pair<std::string, int>> want;
        declared_arms(r->body, std::string(), want);
        for (const auto& [key, arm] : want)
            if (!reached[r->name].arms[key].count(arm))
                out += (out.empty() ? "" : ", ") + r->name +
                       (key.empty() ? "" : "." + key) + " arm " + std::to_string(arm);
    }
    return out;
}

inline std::string missing_optional_sides(const std::vector<BBQ::Rule*>& rules,
                                          std::map<std::string, Reached>& reached) {
    std::string out;
    for (auto* r : rules) {
        std::vector<std::string> opts;
        declared_optionals(r->body, std::string(), opts);
        for (const auto& f : opts) {
            if (f.empty()) continue;          // a rule that IS an optional
            bool present = false, absent = false;
            for (const auto& names : reached[r->name].present)
                (names.count(f) ? present : absent) = true;
            if (!present || !absent)
                out += (out.empty() ? "" : ", ") + r->name + "." + f +
                       (present ? " never absent" : " never present");
        }
    }
    return out;
}

}  // namespace coverage
