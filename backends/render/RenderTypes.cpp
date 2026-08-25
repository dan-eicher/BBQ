#include "RenderTypes.h"

#include "CompilerCtx.h"
#include "BBQ_AST.h"
#include "Sema.h"
#include "CTypeMapper.h"

#include <nlohmann/json.hpp>
#include <inja/inja.hpp>

#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <stdexcept>

using namespace BBQ;
using bbqgen::Sema;
using bbqgen::SwitchCaseLabel;
using bbqgen_c::CTypeMapper;
using json = nlohmann::json;

namespace bbq::render {

namespace {

// The type model lowering. Type-name mapping is delegated to the canonical
// CTypeMapper (shared with the writer); this builds the JSON model the template
// renders and pre-renders the arbitrarily-deep _free bodies to strings (the same
// split the reader uses for expression sub-trees).
struct TypeLowerer {
    Sema& sema;
    CTypeMapper tm;

    TypeLowerer(Sema& s, const std::string& prefix, bool default_le)
        : sema(s), tm(s, prefix), default_le_(default_le) {}
    bool default_le_ = true;

    // A bitfield entry's bit position in the container. Entries are declared MSB-first for
    // a big-endian container and LSB-first for a little-endian one, so the shift is either
    // measured down from the top or accumulated from the bottom. Carrying it in the model
    // is what lets the generated accessor mask for the caller: the grammar knows the
    // layout, so nobody using the handle has to.
    json bitfield_entries(Bitfield* bf) {
        int cbits = 8;
        bool be = false;
        if (auto* ik = dynamic_cast<IntegerKind*>(bf->container)) {
            cbits = width_bits(ik->width);
            be = ik->endian == Endianness::Big ||
                 (ik->endian == Endianness::Default && !default_le_);
        }
        json entries = json::array();
        int offset_before = 0;
        for (auto* e : bf->entries) {
            const int w = (int)e->width;
            const int shift = be ? (cbits - offset_before - w) : offset_before;
            entries.push_back({{"name", e->name}, {"width", e->width},
                               {"shift", shift},
                               {"mask", (uint64_t)((1ull << w) - 1)},
                               {"signed", e->sign == Signedness::Signed},
                               {"be", be},
                               {"container_bits", cbits}});
            offset_before += w;
        }
        return entries;
    }

    // ── Neutral type tree (the shared contract; the c_type/cpp_type mapper
    // callbacks walk it). Carries STRUCTURE + raw names + neutral prim descriptors
    // — no language type spellings. Mirrors the recursion of CTypeMapper::c_type.
    static int width_bits(Width w) {
        switch (w) { case Width::W8: return 8; case Width::W16: return 16;
                     case Width::W32: return 32; case Width::W64: return 64; } return 0;
    }
    // Resolve a field's endianness against the grammar default (for the C++/view
    // decode atom; the C owning type ignores `be`).
    bool is_be(Endianness e) {
        if (e == Endianness::Default) return sema.default_endian() == Endianness::Big;
        return e == Endianness::Big;
    }
    json prim_node(PrimitiveKind* k) {
        if (auto* ik = dynamic_cast<IntegerKind*>(k))
            return {{"kind", "int"}, {"w", width_bits(ik->width)},
                    {"signed", ik->sign == Signedness::Signed}, {"be", is_be(ik->endian)}};
        if (auto* fk = dynamic_cast<FloatKind*>(k))
            return {{"kind", "float"}, {"w", fk->width == Width::W64 ? 64 : 32}, {"be", is_be(fk->endian)}};
        if (dynamic_cast<BytesKind*>(k))  return {{"kind", "bytes"}};
        if (dynamic_cast<StringKind*>(k)) return {{"kind", "string"}};
        if (dynamic_cast<BoolKind*>(k))   return {{"kind", "bool"}};
        return {{"kind", "unknown"}};
    }
    std::string inline_struct_name(Struct* st) {
        for (auto& [nm, ist] : tm.inline_structs()) if (ist == st) return nm;
        return "";
    }
    json type_node(TypeExpr* t) {
        switch (t->node_kind()) {
            case NodeKind::Primitive: return {{"t", "scalar"}, {"prim", prim_node(static_cast<Primitive*>(t)->kind)}};
            // Computed fields share the scalar's C spelling but are accessed
            // differently (no bytes read) — kept distinct so the C++/view backends
            // can tell them apart; the C owning struct spells both the same.
            case NodeKind::Compute:   return {{"t", "computed"}, {"prim", prim_node(static_cast<Compute*>(t)->result_type)}};
            case NodeKind::RuleRef:   return {{"t", "ruleref"}, {"rule", static_cast<RuleRef*>(t)->name}};
            case NodeKind::Array:     return {{"t", "array"}, {"elem", type_node(static_cast<Array*>(t)->element)}};
            case NodeKind::Optional:  return {{"t", "optional"}, {"elem", type_node(static_cast<Optional*>(t)->element)}};
            case NodeKind::Extern:    return {{"t", "verbatim"}, {"spelling", static_cast<Extern*>(t)->cpp_type}};
            case NodeKind::Struct:    return {{"t", "named"}, {"name", inline_struct_name(static_cast<Struct*>(t))}};
            case NodeKind::Switch: {
                auto* sw = static_cast<Switch*>(t);
                json members = json::array();
                for (size_t i = 0; i < sw->cases.size(); i++)
                    members.push_back({{"type", type_node(sw->cases[i]->target)}, {"name", "case_" + std::to_string(i)}});
                if (sw->default_ && !(*sw->default_)->is_reject)
                    members.push_back({{"type", type_node(*(*sw->default_)->target)}, {"name", "default_val"}});
                return {{"t", "tagged_inline"}, {"members", members}};
            }
            case NodeKind::Bitfield: {
                auto* bf = static_cast<Bitfield*>(t);
                return {{"t", "bitfield_inline"}, {"entries", bitfield_entries(bf)}};
            }
            default: return {{"t", "unknown"}};
        }
    }

    json struct_fields(Struct* st) {
        json fields = json::array();
        for (auto* field : st->fields) {
            if (dynamic_cast<EndianSwitch*>(field->body)) continue;
            fields.push_back({{"type", type_node(field->body)}, {"name", field->name}});
        }
        return fields;
    }

    // ── Tagged-union model (enum + union). Canonical `name` + raw member names;
    // the backend spells the enum constants / tag from them. ──
    json tagged_model(const std::string& name,
                      const std::vector<json>& types,
                      const std::vector<std::string>& names,
                      const std::vector<std::string>& enum_values,
                      bool variant_named) {
        json enums = json::array();
        for (size_t i = 0; i < names.size(); i++) {
            json e = {{"member", names[i]}, {"has_value", false}, {"value", ""}};
            if (!enum_values.empty())      { e["value"] = enum_values[i]; e["has_value"] = true; }
            else if (i == 0)               { e["value"] = "0";            e["has_value"] = true; }
            enums.push_back(e);
        }
        json variants = json::array();
        for (size_t i = 0; i < types.size(); i++)
            variants.push_back({{"type", types[i]}, {"member", names[i]}, {"tag", (int)i}});
        // variant_named: union/alternatives capture each arm under its variant name
        // (read by name + the recorded variant_tag); switch arms capture under the
        // field name + the recorded case ordinal — a different read shape.
        return {{"kind", "tagged"}, {"name", name}, {"variant_named", variant_named},
                {"enum", enums}, {"variants", variants}};
    }

    json union_model(const std::string& name, Union* u) {
        std::vector<json> types; std::vector<std::string> names;
        for (size_t i = 0; i < u->variants.size(); i++) {
            types.push_back(type_node(u->variants[i]->body));
            names.push_back(u->variants[i]->name.empty()
                ? "alt_" + std::to_string(i) : u->variants[i]->name);
        }
        return tagged_model(name, types, names, {}, /*variant_named=*/true);
    }

    json switch_model(const std::string& name, Switch* sw) {
        std::vector<json> types; std::vector<std::string> names, values;
        bool has_int_cases = false;
        const auto& labels = sema.switch_case_labels(sw);  // resolved fact, not re-classified here
        for (size_t i = 0; i < sw->cases.size(); i++) {
            types.push_back(type_node(sw->cases[i]->target));
            names.push_back("case_" + std::to_string(i));
            const SwitchCaseLabel& l = labels[i];
            switch (l.kind) {
                case SwitchCaseLabel::Kind::Int:
                case SwitchCaseLabel::Kind::Range: values.push_back(std::to_string(l.lo)); has_int_cases = true; break;
                case SwitchCaseLabel::Kind::Ident:
                case SwitchCaseLabel::Kind::Ref:   values.push_back(l.name);                has_int_cases = true; break;
                case SwitchCaseLabel::Kind::Positional: values.push_back(std::to_string(i)); break;
            }
        }
        if (sw->default_ && !(*sw->default_)->is_reject) {
            types.push_back(type_node(*(*sw->default_)->target));
            names.push_back("default_val");
            values.push_back("-1");
        }
        return tagged_model(name, types, names,
                            has_int_cases ? values : std::vector<std::string>{},
                            /*variant_named=*/false);
    }

    json alternatives_model(const std::string& name, Alternatives* alts) {
        std::vector<json> types; std::vector<std::string> names;
        for (size_t i = 0; i < alts->alts.size(); i++) {
            types.push_back(type_node(alts->alts[i]));
            names.push_back("alt_" + std::to_string(i));
        }
        return tagged_model(name, types, names, {}, /*variant_named=*/true);
    }

    // ── _free generation (the recursive owned-memory walk, rendered to strings) ──
    // Ownership is a resolved Sema fact (sema.type_needs_free) — read it, never
    // re-derive it here. This emitter only spells the C free code for owners.

    // Emit the C that frees `target` of this type — switch on the node tag,
    // recursing for nested types; tagged types delegate to their typed emitter.
    void emit_field_free(TypeExpr* type, const std::string& target,
                         std::ostream& out, int indent) {
        std::string ind(indent * 4, ' ');
        switch (type->node_kind()) {
            case NodeKind::Primitive: {
                auto* p = static_cast<Primitive*>(type);
                if (dynamic_cast<BytesKind*>(p->kind) || dynamic_cast<StringKind*>(p->kind))
                    out << ind << "free((void*)" << target << ".data);\n";
                break;
            }
            case NodeKind::Array: {
                auto* arr = static_cast<Array*>(type);
                if (sema.type_needs_free(arr->element)) {
                    out << ind << "for (size_t i = 0; i < " << target << ".count; i++)\n";
                    emit_field_free(arr->element, target + ".items[i]", out, indent + 1);
                }
                out << ind << "free(" << target << ".items);\n";
                break;
            }
            case NodeKind::RuleRef: {
                auto* rr = static_cast<RuleRef*>(type);
                auto* rule = sema.lookup_rule(rr->name);
                if (rule && sema.type_needs_free(rule->body))
                    out << ind << tm.prefixed_name(rr->name) << "_free(&" << target << ");\n";
                break;
            }
            case NodeKind::Struct:
                emit_struct_free_body(static_cast<Struct*>(type), target + ".", out, indent);
                break;
            case NodeKind::Optional: {
                auto* opt = static_cast<Optional*>(type);
                if (sema.type_needs_free(opt->element)) {
                    out << ind << "if (" << target << ".has_value)\n";
                    emit_field_free(opt->element, target + ".value", out, indent + 1);
                }
                break;
            }
            case NodeKind::Union: emit_union_free(static_cast<Union*>(type), target, out, indent); break;
            case NodeKind::Switch: emit_switch_free(static_cast<Switch*>(type), target, out, indent); break;
            case NodeKind::Alternatives: emit_alt_free(static_cast<Alternatives*>(type), target, out, indent); break;
            default: break;
        }
    }

    // Tagged-type free emitters. Split by type (the visitor / free_body call the
    // matching one) so there is no dynamic_cast dispatch — only the switch-case
    // *value* still classifies CaseValue (a separate, non-TypeExpr hierarchy).
    void emit_union_free(Union* u, const std::string& target, std::ostream& out, int indent) {
        std::string ind(indent * 4, ' '), ind2((indent + 1) * 4, ' ');
        out << ind << "switch (" << target << ".tag) {\n";
        for (size_t i = 0; i < u->variants.size(); i++) {
            if (!sema.type_needs_free(u->variants[i]->body)) continue;
            std::string vn = u->variants[i]->name.empty()
                ? "alt_" + std::to_string(i) : u->variants[i]->name;
            out << ind << "case " << i << ":\n";
            emit_field_free(u->variants[i]->body, target + ".u." + vn, out, indent + 1);
            out << ind2 << "break;\n";
        }
        out << ind << "default: break;\n" << ind << "}\n";
    }
    void emit_alt_free(Alternatives* alts, const std::string& target, std::ostream& out, int indent) {
        std::string ind(indent * 4, ' '), ind2((indent + 1) * 4, ' ');
        out << ind << "switch (" << target << ".tag) {\n";
        for (size_t i = 0; i < alts->alts.size(); i++) {
            if (!sema.type_needs_free(alts->alts[i])) continue;
            out << ind << "case " << i << ":\n";
            emit_field_free(alts->alts[i], target + ".u.alt_" + std::to_string(i), out, indent + 1);
            out << ind2 << "break;\n";
        }
        out << ind << "default: break;\n" << ind << "}\n";
    }
    void emit_switch_free(Switch* sw, const std::string& target, std::ostream& out, int indent) {
        std::string ind(indent * 4, ' '), ind2((indent + 1) * 4, ' ');
        out << ind << "switch (" << target << ".tag) {\n";
        const auto& labels = sema.switch_case_labels(sw);  // resolved fact, not re-classified here
        for (size_t i = 0; i < sw->cases.size(); i++) {
            if (!sema.type_needs_free(sw->cases[i]->target)) continue;
            const SwitchCaseLabel& l = labels[i];
            switch (l.kind) {
                case SwitchCaseLabel::Kind::Int:   out << ind << "case " << l.lo << ":\n"; break;
                case SwitchCaseLabel::Kind::Range:
                    for (int64_t v = l.lo; v <= l.hi; v++) out << ind << "case " << v << ":\n";
                    break;
                case SwitchCaseLabel::Kind::Ident:
                case SwitchCaseLabel::Kind::Ref:   out << ind << "case " << l.name << ":\n"; break;
                case SwitchCaseLabel::Kind::Positional: out << ind << "case " << i << ":\n"; break;
            }
            emit_field_free(sw->cases[i]->target, target + ".u.case_" + std::to_string(i), out, indent + 1);
            out << ind2 << "break;\n";
        }
        if (sw->default_ && !(*sw->default_)->is_reject
            && sema.type_needs_free(*(*sw->default_)->target)) {
            out << ind << "default:\n";
            emit_field_free(*(*sw->default_)->target, target + ".u.default_val", out, indent + 1);
            out << ind2 << "break;\n";
        } else {
            out << ind << "default: break;\n";
        }
        out << ind << "}\n";
    }

    void emit_struct_free_body(Struct* st, const std::string& pfx,
                               std::ostream& out, int indent) {
        for (auto* f : st->fields) {
            if (dynamic_cast<EndianSwitch*>(f->body)) continue;
            if (dynamic_cast<Compute*>(f->body)) continue;
            if (sema.type_needs_free(f->body))
                emit_field_free(f->body, pfx + f->name, out, indent);
        }
    }

    // Free body for a rule (the `p->`-rooted top level) — the all-Owned
    // counterpart shape of emit_field_free, switched on the rule body's tag.
    std::string free_body(Rule* rule) {
        std::ostringstream out;
        out << "    (void)p;\n";
        TypeExpr* body = rule->body;
        switch (body->node_kind()) {
            case NodeKind::Struct:
                emit_struct_free_body(static_cast<Struct*>(body), "p->", out, 1);
                break;
            case NodeKind::Array: {
                auto* arr = static_cast<Array*>(body);
                if (sema.type_needs_free(arr->element)) {
                    out << "    for (size_t i = 0; i < p->count; i++)\n";
                    emit_field_free(arr->element, "p->items[i]", out, 2);
                }
                out << "    free(p->items);\n";
                break;
            }
            case NodeKind::Optional: {
                auto* opt = static_cast<Optional*>(body);
                if (sema.type_needs_free(opt->element)) {
                    out << "    if (p->has_value)\n";
                    emit_field_free(opt->element, "p->value", out, 2);
                }
                break;
            }
            case NodeKind::Union: emit_union_free(static_cast<Union*>(body), "(*p)", out, 1); break;
            case NodeKind::Switch: emit_switch_free(static_cast<Switch*>(body), "(*p)", out, 1); break;
            case NodeKind::Alternatives: emit_alt_free(static_cast<Alternatives*>(body), "(*p)", out, 1); break;
            default: break;
        }
        return out.str();
    }

    // Aggregate (struct-like) rule bodies get a forward declaration; scalar
    // typedefs/arrays/optionals do not.
    bool is_struct_like(TypeExpr* t) {
        switch (t->node_kind()) {
            case NodeKind::Struct: case NodeKind::Union: case NodeKind::Switch:
            case NodeKind::Alternatives: case NodeKind::Bitfield:
                return true;
            default: return false;
        }
    }

    // ── Top-level model build ──
    json build(Grammar* grammar) {
        json model;

        // All name lists carry the canonical CamelCase name; the template spells.
        json fwd_inline = json::array();
        for (auto& [name, st] : tm.inline_structs()) {
            (void)st;
            fwd_inline.push_back(name);
        }
        model["forward_inline"] = fwd_inline;

        json fwd_rules = json::array();
        for (auto* rule : sema.sorted_rules())
            if (is_struct_like(rule->body))
                fwd_rules.push_back({{"name", rule->name}});
        model["forward_rules"] = fwd_rules;

        json defs = json::array();
        for (auto* rule : sema.sorted_rules())
            defs.push_back(rule_def(rule));
        model["defs"] = defs;

        json frees = json::array();
        for (auto* rule : sema.sorted_rules()) {
            if (!sema.type_needs_free(rule->body)) continue;
            frees.push_back({{"name", rule->name}, {"body", free_body(rule)}});
        }
        model["frees"] = frees;
        model["has_frees"] = !frees.empty();

        std::string uh;
        for (auto* cb : grammar->codes)
            if (cb->section == CodeSection::HeaderBlock)
                uh += cb->code + "\n";
        model["user_header"] = uh;

        return model;
    }

    // The struct case of rule_def, factored out so the visitor can call it.
    // Every def carries only the canonical CamelCase `name` (rule / inline struct);
    // each backend spells it (C: c_sname/c_tname via the mapper; C++: as-is).
    json struct_def(Struct* st, const std::string& rule_name) {
        json inlines = json::array();
        std::string pfx = rule_name + "_";
        for (auto& [name, ist] : tm.inline_structs())
            if (name.substr(0, pfx.size()) == pfx)
                inlines.push_back({{"name", name}, {"fields", struct_fields(ist)}});
        return {{"kind", "struct"}, {"name", rule_name},
                {"inlines", inlines}, {"fields", struct_fields(st)}};
    }

    // Per-rule type model, switched on the rule body's node tag. Each TypeExpr
    // variant builds its JSON record (static_cast is safe — the tag fixes the type).
    json rule_def(Rule* rule) {
        TypeExpr* body = rule->body;
        const std::string& rn = rule->name;   // canonical CamelCase; backends spell it
        switch (body->node_kind()) {
            case NodeKind::Struct:       return struct_def(static_cast<Struct*>(body), rn);
            case NodeKind::Union:        return union_model(rn, static_cast<Union*>(body));
            case NodeKind::Switch:       return switch_model(rn, static_cast<Switch*>(body));
            case NodeKind::Alternatives: return alternatives_model(rn, static_cast<Alternatives*>(body));
            case NodeKind::Array:
                return {{"kind", "array"}, {"name", rn},
                        {"elem_type", type_node(static_cast<Array*>(body)->element)}};
            case NodeKind::Optional:
                return {{"kind", "optional"}, {"name", rn},
                        {"elem_type", type_node(static_cast<Optional*>(body)->element)}};
            // typedef bodies (primitive / ruleref / extern) — the underlying is a type use.
            case NodeKind::Primitive:
            case NodeKind::RuleRef:
            case NodeKind::Extern:
                return {{"kind", "typedef"}, {"name", rn}, {"underlying", type_node(body)}};
            case NodeKind::Bitfield:
                return {{"kind", "bitfield"}, {"name", rn},
                        {"entries", bitfield_entries(static_cast<Bitfield*>(body))}};
            default: return {{"kind", "unknown"}};
        }
    }
};

}  // namespace

// ── The C name mapper: neutral type_node → C type spelling. Registered as inja
// callbacks so the template carries no C-specific type logic (the same split as
// the reader's read_fn/ctype). A C++ backend supplies its own callbacks over the
// SAME neutral nodes.
static std::string ctype_prim(const json& p) {
    std::string k = p["kind"];
    if (k == "int")    { return (p["signed"].get<bool>() ? "int" : "uint") + std::to_string(p["w"].get<int>()) + "_t"; }
    if (k == "float")  return p["w"].get<int>() == 64 ? "double" : "float";
    if (k == "bool")   return "bool";
    if (k == "bytes")  return "bbq_bytes_t";
    if (k == "string") return "bbq_string_t";
    return "/*?prim*/";
}
// A signed entry is read back sign-extended from its own width, so the field it lands
// in has to be signed — an unsigned spelling would report a 4-bit 0xD as 13 while the
// grammar declared -3.
static std::string bitfield_ctype(int64_t w, bool is_signed = false) {
    if (w <= 8)  return is_signed ? "int8_t"  : "uint8_t";
    if (w <= 16) return is_signed ? "int16_t" : "uint16_t";
    if (w <= 32) return is_signed ? "int32_t" : "uint32_t";
    return is_signed ? "int64_t" : "uint64_t";
}
// The one shared lowering — both render_types_c and render_types_cpp call it.
json lower_type_model(const CompilerCtx& ctx) {
    TypeLowerer lower(*ctx.sema, ctx.prefix, ctx.default_le());
    return lower.build(ctx.ast);
}

std::string render_types_c(const CompilerCtx& ctx, const std::string& template_path) {
    json model = lower_type_model(ctx);

    // Two grammars' headers can meet in one translation unit. A fixed guard makes
    // the second one vanish, so derive it from the prefix that already namespaces
    // every type in the file; unprefixed output keeps the generic guard.
    std::string tp = ctx.type_prefix();
    std::string guard = "BBQ_GENERATED_TYPES_H";
    if (!tp.empty()) {
        guard = tp + "_TYPES_H";
        for (char& c : guard) c = (char)std::toupper((unsigned char)c);
    }
    model["guard"] = guard;

    std::ifstream tf(template_path);
    std::stringstream ss;
    ss << tf.rdbuf();
    std::string tmpl = ss.str();

    inja::Environment env;
    env.set_trim_blocks(true);
    env.set_lstrip_blocks(true);

    // Recursive C spelling of a neutral type-use node. Rule/struct names go
    // through the ONE canonical C namer (CTypeMapper) — never a reimplementation,
    // so the names can't drift from the model's defs.
    CTypeMapper namer(*ctx.sema, ctx.prefix);
    std::function<std::string(const json&)> c_type = [&](const json& n) -> std::string {
        std::string t = n["t"];
        if (t == "scalar" || t == "computed") return ctype_prim(n["prim"]);
        if (t == "ruleref")  return namer.type_name(n["rule"].get<std::string>());
        if (t == "named")    return namer.type_name(n["name"].get<std::string>());
        if (t == "verbatim") return n["spelling"].get<std::string>();
        if (t == "array")    return "struct { " + c_type(n["elem"]) + "* items; size_t count; }";
        if (t == "optional") return "struct { bool has_value; " + c_type(n["elem"]) + " value; }";
        if (t == "tagged_inline") {
            std::string s = "struct { int tag; union { ";
            for (auto& m : n["members"]) s += c_type(m["type"]) + " " + m["name"].get<std::string>() + "; ";
            return s + "} u; }";
        }
        if (t == "bitfield_inline") {
            std::string s = "struct { ";
            for (auto& e : n["entries"])
                s += bitfield_ctype(e["width"].get<int64_t>(), e.value("signed", false))
                   + " " + e["name"].get<std::string>() + "; ";
            return s + "}";
        }
        return "/*?type*/";
    };
    env.add_callback("c_type", 1, [&](inja::Arguments& a) { return c_type(*a[0]); });
    env.add_callback("bitfield_type", 2, [&](inja::Arguments& a) {
        return bitfield_ctype(a[0]->get<int64_t>(), a[1]->get<bool>());
    });
    // Canonical CamelCase name → C struct tag (c_sname) / typedef name (c_tname),
    // through the one C namer. The template never spells names itself.
    env.add_callback("c_sname", 1, [&](inja::Arguments& a) { return namer.prefixed_name(a[0]->get<std::string>()); });
    env.add_callback("c_tname", 1, [&](inja::Arguments& a) { return namer.type_name(a[0]->get<std::string>()); });

    return env.render(tmpl, model);
}

// ── The C++ name/type mapper: neutral type_node → C++ spelling. C++ uses the
// canonical CamelCase rule name AS-IS (converting is C's job); scalars share C's
// spellings, bytes/string become views. Pure (no mapper state). ──
namespace {

std::string cpp_atom(const json& p) {
    std::string k = p["kind"];
    int w = p.value("w", 0);
    bool be = p.value("be", false);
    if (k == "int")   return w == 8 ? "bbq_rd_u8" : "bbq_rd_u" + std::to_string(w) + (be ? "be" : "le");
    if (k == "float") return std::string("bbq_rd_f") + std::to_string(w) + (be ? "be" : "le");
    return "bbq_rd_u8";  // bool
}
std::function<std::string(const json&)> make_cpp_type() {
    auto self = std::make_shared<std::function<std::string(const json&)>>();
    *self = [self](const json& n) -> std::string {
        std::string t = n["t"];
        if (t == "scalar" || t == "computed" || t == "bits") {
            const json& p = n["prim"];
            std::string k = p["kind"];
            if (k == "bytes")  return "bbq::bytes_view";
            if (k == "string") return "std::string_view";
            if (k == "bool")   return "bool";
            if (k == "float")  return p["w"].get<int>() == 64 ? "double" : "float";
            return (p["signed"].get<bool>() ? "int" : "uint") + std::to_string(p["w"].get<int>()) + "_t";
        }
        // A handle is one class, not a pair parameterised on a cursor type: whether it
        // can be written is a property of the document it came from, which the cursor
        // it holds already carries.
        if (t == "ruleref")  return n["rule"].get<std::string>();
        if (t == "named")    return n["name"].get<std::string>();
        if (t == "verbatim") return "bbq::bytes_view";   // extern = the recorded span
        if (t == "array") {
            // Numbers decode per element; bytes/string borrow a span; anything with
            // structure gets a handle per element.
            const json& e = n["elem"];
            std::string et = e["t"];
            if (et == "scalar" || et == "computed") {
                std::string k = e["prim"]["kind"];
                if (k == "bytes")  return "span_seq<bbq::bytes_view, uint8_t>";
                if (k == "string") return "span_seq<std::string_view, char>";
                return "prim_seq<" + (*self)(e) + ">";
            }
            return "seq<" + (*self)(e) + ">";
        }
        if (t == "optional") {
            const json& e = n["elem"];
            std::string et = e["t"];
            if (et == "scalar" || et == "computed") {
                std::string k = e["prim"]["kind"];
                if (k == "bytes")  return "span_opt<bbq::bytes_view, uint8_t>";
                if (k == "string") return "span_opt<std::string_view, char>";
                return "prim_opt<" + (*self)(e) + ">";
            }
            return "opt<" + (*self)(e) + ">";
        }
        return "/*?cpptype*/";
    };
    return *self;
}

// Decode the node reached by `c` (a `const zcow::node*` C++ expression) as `n` —
// the read expression for a tagged arm (switch case / union variant), where the
// accessor name and the index child differ. Union variants capture under their
// variant name; switch arms under the field name; a top-level switch rule's arm is
// the root's anonymous children[0]. Mirrors the per-type field accessors.
// Read the node expression `c` denotes, as type `n` — one bbq::zcow call per case.
// The buffer, the offsets and the copy-on-write are the document's; nothing here
// reaches past a call into it.
std::string cpp_decode_ptr(const json& n, const std::string& c,
                           const std::function<std::string(const json&)>& cpp_type) {
    std::string t = n["t"];
    if (t == "scalar" || t == "computed" || t == "verbatim") {
        std::string k = (t == "verbatim") ? "bytes" : n["prim"]["kind"].get<std::string>();
        if (k == "bytes")
            return "[&]{ auto _b = bbq::zcow::read_bytes(" + c + ", *s_);"
                   " return bbq::bytes_view{_b.first, _b.second}; }()";
        if (k == "string") return "bbq::zcow::read_str(" + c + ", *s_)";
        if (k == "float")
            return "(" + cpp_type(n) + ")bbq::zcow::read_float(" + c + ", *s_)";
        return "(" + cpp_type(n) + ")bbq::zcow::read_int(" + c + ", *s_)";
    }
    // A handle or a container: hand it where it is. Braces because a container is an
    // aggregate of (node, source, transient) and a handle's ctor takes the same three.
    return cpp_type(n) + "{ " + c + ", s_, t_ }";
}
// A bitfield entry as a class field. It carries its position in the container, so the
// generated accessor masks on the caller's behalf — a bitfield entry has no storage of
// its own, and making the consumer supply a shift would be handing them the one detail a
// grammar compiler exists to remove.
json bits_field(const json& e) {
    const int w = e["width"].get<int>();
    const int sw = w <= 8 ? 8 : (w <= 16 ? 16 : (w <= 32 ? 32 : 64));
    // A signed entry is read back sign-extended from its own width, so the field it
    // is presented as has to be signed too — a 4-bit `0xD` is -3, and an unsigned
    // spelling would report 13 while the document says otherwise.
    const bool sgn = e.value("signed", false);
    return {{"name", e["name"]},
            {"type", {{"t", "bits"},
                      {"prim", {{"kind", "int"}, {"w", sw}, {"signed", sgn}}},
                      {"shift", e["shift"]}, {"mask", e["mask"]},
                      {"signed", sgn}, {"width", w}, {"be", e["be"]}}}};
}

// Decode a named child (union variant / switch field arm). A handle or container
// needs the writable node, since it may be written through; a value only needs to
// be read.
std::string cpp_decode_child(const json& n, const std::string& child,
                             const std::function<std::string(const json&)>& cpp_type) {
    const std::string t = n["t"];
    const bool by_value = (t == "scalar" || t == "computed" || t == "verbatim");
    return cpp_decode_ptr(n, (by_value ? "r_(\"" : "sub_(\"") + child + "\")", cpp_type);
}
// Decode the first (anonymous) child — a top-level switch rule's arm.
std::string cpp_decode_first(const json& n,
                             const std::function<std::string(const json&)>& cpp_type) {
    const std::string t = n["t"];
    const bool by_value = (t == "scalar" || t == "computed" || t == "verbatim");
    return cpp_decode_ptr(n, by_value
        ? "bbq::zcow::child_at(n_, 0)"
        : "(t_ ? t_->own_child(n_, (size_t)0) "
          ": const_cast<bbq::zcow::node*>(bbq::zcow::child_at(n_, 0)))", cpp_type);
}

}  // namespace

std::string render_types_cpp(const CompilerCtx& ctx,
                             const std::string& template_path,
                             const std::string& ns) {
    json neutral = lower_type_model(ctx);

    // The C++ ZCow type model is fully general over the neutral type nodes: every
    // field type maps to a decode-on-access accessor via cpp_type/cpp_decode_ptr
    // (scalar/computed/bytes/string/ruleref/named/array/optional/tagged_inline/
    // bitfield_inline/verbatim-extern, recursively). No feature gate — the read
    // shape composes; the only hard error is an unrecognized node ("unknown"),
    // which is an internal invariant violation, not a dropped feature.
    auto cpp_type = make_cpp_type();
    json classes = json::array(), forward = json::array(), tagged_classes = json::array();
    for (auto& d : neutral["defs"]) {
        std::string kind = d.value("kind", std::string());
        // A bitfield rule is structurally a struct of computed entries: each entry
        // is a Computed node in the document, so it renders as a node_computed_int
        // accessor — identical to the "computed" field branch. Synthesize one
        // computed field per entry, storage width rounded up to a C++ integer.
        if (kind == "bitfield") {
            json fields = json::array();
            for (auto& e : d["entries"]) fields.push_back(bits_field(e));
            forward.push_back(d["name"]);
            classes.push_back({{"name", d["name"]}, {"fields", fields}});
            continue;
        }
        // tagged rules (kind "tagged"): all read the recorded variant_tag, never
        // re-derive a discriminant. Two index shapes:
        //  - variant_named (union/alternatives): the matched arm is a NAMED child
        //    ("asInner"/"alt_0"); which()/is_<v>()/<v>() key off the name.
        //  - switch RULE (e.g. `Table = switch(peek()){...}`, wasm.bbq): the arm is
        //    the root's anonymous children[0]; which() reads its variant_tag and each
        //    case decodes that first child.
        if (kind == "tagged") {
            forward.push_back(d["name"]);
            tagged_classes.push_back({{"name", d["name"]},
                                      {"variant_named", d.value("variant_named", false)},
                                      {"variants", d["variants"]}});
            continue;
        }
        // Top-level non-struct rules wrap the root in a value() accessor (precomputed
        // over the shared decode machinery), so there is no gate. A top-level typedef
        // (Foo = uint8 / Inner / extern) and a top-level optional put the value in the
        // root's anonymous children[0]; a top-level array IS the root array node.
        if (kind == "typedef" || kind == "optional" || kind == "array") {
            std::string vctype, vexpr; bool is_optional = (kind == "optional");
            if (kind == "typedef") {
                vctype = cpp_type(d["underlying"]);
                vexpr  = cpp_decode_first(d["underlying"], cpp_type);
            } else if (kind == "optional") {
                vctype = cpp_type(d["elem_type"]);
                vexpr  = cpp_decode_first(d["elem_type"], cpp_type);   // children[0] when present
            } else {  // array — the root node is the sequence
                json arr = {{"t", "array"}, {"elem", d["elem_type"]}};
                vctype = cpp_type(arr);
                vexpr  = vctype + "{ n_, buf_, zc_ }";
            }
            forward.push_back(d["name"]);
            classes.push_back({{"name", d["name"]}, {"value_class", true},
                               {"is_optional", is_optional},
                               {"value_ctype", vctype}, {"value_expr", vexpr}});
            continue;
        }
        if (kind != "struct")
            throw std::runtime_error("C++ backend: rule '" + d.value("name", "?") +
                                     "' (unrecognized kind '" + kind + "')");
        // Inline anonymous structs become their own handle classes (declared first
        // via `forward`); the outer field of type `named` returns the inline handle.
        for (auto& inl : d.value("inlines", json::array())) {
            forward.push_back(inl["name"]);
            classes.push_back({{"name", inl["name"]}, {"fields", inl["fields"]}});
        }
        // Inline bitfield fields nest their computed entries under a sub-node named
        // for the field (CEK: result.flags.high). Synthesize a handle class of
        // computed entries and rewrite the field to a `named` ref to it.
        json fields = d["fields"];   // mutable copy
        for (auto& f : fields) {
            if (f["type"]["t"] != "bitfield_inline") continue;
            std::string cn = d["name"].get<std::string>() + "_" + f["name"].get<std::string>();
            json bf_fields = json::array();
            for (auto& e : f["type"]["entries"]) bf_fields.push_back(bits_field(e));
            forward.push_back(cn);
            classes.push_back({{"name", cn}, {"fields", bf_fields}});
            f["type"] = {{"t", "named"}, {"name", cn}};
        }
        forward.push_back(d["name"]);
        classes.push_back({{"name", d["name"]}, {"fields", fields}});
    }
    json model = {{"namespace", ns}, {"has_ns", !ns.empty()},
                  {"forward", forward}, {"classes", classes},
                  {"tagged_classes", tagged_classes}};

    std::ifstream tf(template_path);
    std::stringstream ss;
    ss << tf.rdbuf();

    inja::Environment env;
    env.set_trim_blocks(true);
    env.set_lstrip_blocks(true);
    env.add_callback("cpp_type", 1, [cpp_type](inja::Arguments& a) { return cpp_type(*a[0]); });
    env.add_callback("cpp_atom", 1, [](inja::Arguments& a) { return cpp_atom(*a[0]); });
    env.add_callback("cpp_decode_child", 2, [cpp_type](inja::Arguments& a) {
        return cpp_decode_child(*a[0], a[1]->get<std::string>(), cpp_type);
    });
    env.add_callback("cpp_decode_first", 1, [cpp_type](inja::Arguments& a) {
        return cpp_decode_first(*a[0], cpp_type);
    });

    return env.render(ss.str(), model);
}

}  // namespace bbq::render
