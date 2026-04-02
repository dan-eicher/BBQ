#include "CTypeEmitter.h"
#include "FrameProcessor.h"
#include <algorithm>
#include <cctype>

using namespace BBQ;

using bbqgen::FrameProcessor;

namespace bbqgen_c {

// ── Name conversion ────────────────────────────────────────

std::string to_snake_case(const std::string& name) {
    std::string result;
    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        if (std::isupper(c)) {
            // Insert _ before uppercase if:
            // - preceded by lowercase: "fooBar" → "foo_bar"
            // - preceded by uppercase followed by lowercase: "ABCDef" → "abc_def"
            if (i > 0) {
                char prev = name[i-1];
                if (std::islower(prev) || std::isdigit(prev))
                    result += '_';
                else if (std::isupper(prev) && i + 1 < name.size() && std::islower(name[i+1]))
                    result += '_';
            }
            result += (char)std::tolower(c);
        } else {
            result += c;
        }
    }
    return result;
}

std::string CTypeEmitter::prefixed_name(const std::string& name) {
    std::string snake = to_snake_case(name);
    if (prefix_.empty()) return snake;
    return prefix_ + "_" + snake;
}

std::string CTypeEmitter::type_name(const std::string& rule_name) {
    return prefixed_name(rule_name) + "_t";
}

std::string CTypeEmitter::read_func(const std::string& rule_name) {
    return prefixed_name(rule_name) + "_read";
}

std::string CTypeEmitter::write_func(const std::string& rule_name) {
    return prefixed_name(rule_name) + "_write";
}

// ── Public ─────────────────────────────────────────────────

std::string CTypeEmitter::emit(Grammar* grammar) {
    std::ostringstream out;
    out << "#ifndef BBQ_GENERATED_TYPES_H\n";
    out << "#define BBQ_GENERATED_TYPES_H\n\n";
    out << "#include <stdint.h>\n";
    out << "#include <stdbool.h>\n";
    out << "#include <stddef.h>\n";
    out << "#include \"bbq_runtime.h\"\n\n";
    emit_forward_decls(out);
    out << "\n";
    emit_type_defs(out);
    out << "\n";
    emit_free_funcs(out);
    out << "#endif /* BBQ_GENERATED_TYPES_H */\n";
    return out.str();
}

bool CTypeEmitter::emit_to_frame(Grammar* grammar,
                                  const std::string& frame_path,
                                  std::ostream& out) {
    FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    fp.CopyFramePart("forward_declarations");
    {
        std::ostringstream fwd;
        emit_forward_decls(fwd);
        fp.Emit(fwd.str());
    }

    fp.CopyFramePart("type_definitions");
    {
        std::ostringstream defs;
        emit_type_defs(defs);
        fp.Emit(defs.str());
    }

    fp.CopyFramePart("free_functions");
    {
        std::ostringstream frees;
        emit_free_funcs(frees);
        fp.Emit(frees.str());
    }

    // Copy remainder of frame
    fp.CopyFramePart("");
    return true;
}

// ── Forward declarations ───────────────────────────────────

void CTypeEmitter::emit_forward_decls(std::ostream& out) {
    // Register inline structs
    for (auto* rule : sema_.sorted_rules()) {
        if (auto* st = dynamic_cast<Struct*>(rule->body))
            register_inline_structs(rule->name, st);
    }

    // Forward-declare inline structs
    for (auto& [name, st] : inline_struct_order_)
        out << "typedef struct " << prefixed_name(name) << " " << prefixed_name(name) << "_t;\n";

    // Forward-declare rule types
    for (auto* rule : sema_.sorted_rules()) {
        auto* body = rule->body;
        if (dynamic_cast<Struct*>(body) ||
            dynamic_cast<Union*>(body) ||
            dynamic_cast<Switch*>(body) ||
            dynamic_cast<Alternatives*>(body) ||
            dynamic_cast<Bitfield*>(body)) {
            out << "typedef struct " << prefixed_name(rule->name)
                << " " << type_name(rule->name) << ";\n";
        }
    }
}

// ── Type definitions ───────────────────────────────────────

void CTypeEmitter::emit_type_defs(std::ostream& out) {
    for (auto* rule : sema_.sorted_rules()) {
        emit_rule_type(rule, out);
        out << "\n";
    }
}

void CTypeEmitter::emit_rule_type(Rule* rule, std::ostream& out) {
    auto* body = rule->body;
    std::string tname = type_name(rule->name);

    if (auto* st = dynamic_cast<Struct*>(body)) {
        emit_inline_structs(rule->name, out);
        emit_struct_type(rule->name, st, out);

    } else if (auto* u = dynamic_cast<Union*>(body)) {
        emit_union_type(rule->name, u, out);

    } else if (auto* sw = dynamic_cast<Switch*>(body)) {
        emit_switch_type(rule->name, sw, out);

    } else if (auto* alts = dynamic_cast<Alternatives*>(body)) {
        emit_alternatives_type(rule->name, alts, out);

    } else if (auto* arr = dynamic_cast<Array*>(body)) {
        // Array rule → struct with pointer + count
        std::string elem_t = c_type(arr->element);
        out << "typedef struct {\n";
        out << "    " << elem_t << "* items;\n";
        out << "    size_t count;\n";
        out << "} " << tname << ";\n";

    } else if (auto* opt = dynamic_cast<Optional*>(body)) {
        std::string elem_t = c_type(opt->element);
        out << "typedef struct {\n";
        out << "    bool has_value;\n";
        out << "    " << elem_t << " value;\n";
        out << "} " << tname << ";\n";

    } else if (auto* prim = dynamic_cast<Primitive*>(body)) {
        out << "typedef " << primitive_c_type(prim->kind) << " " << tname << ";\n";

    } else if (auto* rr = dynamic_cast<RuleRef*>(body)) {
        out << "typedef " << type_name(rr->name) << " " << tname << ";\n";

    } else if (auto* ext = dynamic_cast<Extern*>(body)) {
        out << "typedef " << ext->cpp_type << " " << tname << ";\n";

    } else if (auto* bf = dynamic_cast<Bitfield*>(body)) {
        out << "struct " << prefixed_name(rule->name) << " {\n";
        for (auto* entry : bf->entries)
            out << "    " << bitfield_entry_type(entry->width) << " " << entry->name << ";\n";
        out << "};\n";
    }
}

// ── Struct ─────────────────────────────────────────────────

void CTypeEmitter::emit_struct_type(const std::string& name, Struct* st, std::ostream& out) {
    out << "struct " << prefixed_name(name) << " {\n";
    for (auto* field : st->fields) {
        if (dynamic_cast<EndianSwitch*>(field->body)) continue;

        // In C, compute fields are included as regular fields
        // (the reader populates them, the writer can read them)
        out << "    " << c_type(field->body) << " " << field->name << ";\n";
    }
    out << "};\n";
}

// ── Union / Alternatives / Switch → tagged union ───────────

void CTypeEmitter::emit_tagged_union(const std::string& name,
                                      const std::vector<std::string>& variant_types,
                                      const std::vector<std::string>& variant_names,
                                      std::ostream& out,
                                      const std::vector<std::string>& enum_values) {
    std::string sname = prefixed_name(name);

    // Tag enum
    out << "enum " << sname << "_tag {\n";
    for (size_t i = 0; i < variant_names.size(); i++) {
        out << "    " << sname << "_" << variant_names[i];
        if (!enum_values.empty())
            out << " = " << enum_values[i];
        else if (i == 0)
            out << " = 0";
        out << ",\n";
    }
    out << "};\n\n";

    // Struct with tag + union
    out << "struct " << sname << " {\n";
    out << "    enum " << sname << "_tag tag;\n";
    out << "    union {\n";
    for (size_t i = 0; i < variant_types.size(); i++)
        out << "        " << variant_types[i] << " " << variant_names[i] << ";\n";
    out << "    } u;\n";
    out << "};\n";
}

void CTypeEmitter::emit_union_type(const std::string& name, Union* u, std::ostream& out) {
    std::vector<std::string> types, names;
    for (size_t i = 0; i < u->variants.size(); i++) {
        types.push_back(c_type(u->variants[i]->body));
        names.push_back(u->variants[i]->name.empty()
            ? "alt_" + std::to_string(i) : u->variants[i]->name);
    }
    emit_tagged_union(name, types, names, out);
}

void CTypeEmitter::emit_switch_type(const std::string& name, Switch* sw, std::ostream& out) {
    std::vector<std::string> types, names, values;
    bool has_int_cases = false;
    for (size_t i = 0; i < sw->cases.size(); i++) {
        auto* c = sw->cases[i];
        types.push_back(c_type(c->target));
        names.push_back("case_" + std::to_string(i));
        if (auto* iv = dynamic_cast<IntValue*>(c->value)) {
            values.push_back(std::to_string(iv->value));
            has_int_cases = true;
        } else if (auto* idv = dynamic_cast<IdentValue*>(c->value)) {
            values.push_back(idv->name);
            has_int_cases = true;
        } else if (auto* rv = dynamic_cast<RefValue*>(c->value)) {
            std::string path;
            for (size_t j = 0; j < rv->path.size(); j++) {
                if (j > 0) path += "_";
                path += rv->path[j];
            }
            values.push_back(path);
            has_int_cases = true;
        } else {
            values.push_back(std::to_string(i));
        }
    }
    if (sw->default_) {
        types.push_back(c_type((*sw->default_)->target));
        names.push_back("default_val");
        values.push_back("-1");
    }
    emit_tagged_union(name, types, names, out, has_int_cases ? values : std::vector<std::string>{});
}

void CTypeEmitter::emit_alternatives_type(const std::string& name, Alternatives* alts, std::ostream& out) {
    std::vector<std::string> types, names;
    for (size_t i = 0; i < alts->alts.size(); i++) {
        types.push_back(c_type(alts->alts[i]));
        names.push_back("alt_" + std::to_string(i));
    }
    emit_tagged_union(name, types, names, out);
}

// ── Inline struct support ──────────────────────────────────

void CTypeEmitter::register_inline_structs(const std::string& prefix, Struct* st) {
    for (auto* field : st->fields) {
        if (dynamic_cast<EndianSwitch*>(field->body)) continue;
        if (auto* nested = dynamic_cast<Struct*>(field->body)) {
            std::string name = prefix + "_" + field->name;
            if (inline_struct_names_.find(nested) == inline_struct_names_.end()) {
                register_inline_structs(name, nested);
                inline_struct_names_[nested] = name;
                inline_struct_order_.push_back({name, nested});
            }
        }
    }
}

void CTypeEmitter::emit_inline_structs(const std::string& rule_name, std::ostream& out) {
    std::string prefix = rule_name + "_";
    for (auto& [name, st] : inline_struct_order_) {
        if (name.substr(0, prefix.size()) == prefix) {
            emit_struct_type(name, st, out);
            out << "\n";
        }
    }
}

// ── Type mapping ───────────────────────────────────────────

std::string CTypeEmitter::c_type(TypeExpr* type) {
    if (auto* prim = dynamic_cast<Primitive*>(type)) {
        return primitive_c_type(prim->kind);
    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        return type_name(rr->name);
    } else if (auto* arr = dynamic_cast<Array*>(type)) {
        // Inline array field: pointer + count in a wrapper struct
        // For named rules this is handled by emit_rule_type
        std::string elem_t = c_type(arr->element);
        return "struct { " + elem_t + "* items; size_t count; }";
    } else if (auto* opt = dynamic_cast<Optional*>(type)) {
        std::string elem_t = c_type(opt->element);
        return "struct { bool has_value; " + elem_t + " value; }";
    } else if (auto* comp = dynamic_cast<Compute*>(type)) {
        return primitive_c_type(comp->result_type);
    } else if (auto* st = dynamic_cast<Struct*>(type)) {
        auto it = inline_struct_names_.find(st);
        if (it != inline_struct_names_.end())
            return prefixed_name(it->second) + "_t";
        return "/* anonymous struct */";
    } else if (auto* sw = dynamic_cast<Switch*>(type)) {
        // Inline switch → inline tagged union
        std::string result = "struct { int tag; union { ";
        for (size_t i = 0; i < sw->cases.size(); i++)
            result += c_type(sw->cases[i]->target) + " case_" + std::to_string(i) + "; ";
        if (sw->default_)
            result += c_type((*sw->default_)->target) + " default_val; ";
        result += "} u; }";
        return result;
    } else if (auto* ext = dynamic_cast<Extern*>(type)) {
        return ext->cpp_type;
    } else if (auto* bf = dynamic_cast<Bitfield*>(type)) {
        std::string result = "struct { ";
        for (auto* entry : bf->entries)
            result += bitfield_entry_type(entry->width) + " " + entry->name + "; ";
        result += "}";
        return result;
    }
    return "/* unknown type */";
}

std::string CTypeEmitter::primitive_c_type(PrimitiveKind* kind) {
    if (auto* ik = dynamic_cast<IntegerKind*>(kind)) {
        std::string base = (ik->sign == Signedness::Unsigned) ? "uint" : "int";
        switch (ik->width) {
            case Width::W8:  return base + "8_t";
            case Width::W16: return base + "16_t";
            case Width::W32: return base + "32_t";
            case Width::W64: return base + "64_t";
        }
    } else if (auto* fk = dynamic_cast<FloatKind*>(kind)) {
        return (fk->width == Width::W64) ? "double" : "float";
    } else if (dynamic_cast<BytesKind*>(kind)) {
        return "bbq_bytes_t";
    } else if (dynamic_cast<StringKind*>(kind)) {
        return "bbq_string_t";
    } else if (dynamic_cast<BoolKind*>(kind)) {
        return "bool";
    }
    return "/* unknown primitive */";
}

std::string CTypeEmitter::bitfield_entry_type(int64_t width) {
    if (width <= 8)  return "uint8_t";
    if (width <= 16) return "uint16_t";
    if (width <= 32) return "uint32_t";
    return "uint64_t";
}

// ── Free functions ─────────────────────────────────────────

// Does this type contain any heap-allocated data (arrays)?
bool CTypeEmitter::type_needs_free(TypeExpr* type) {
    if (dynamic_cast<Array*>(type)) return true;
    if (auto* st = dynamic_cast<Struct*>(type)) {
        for (auto* f : st->fields) {
            if (dynamic_cast<EndianSwitch*>(f->body)) continue;
            if (type_needs_free(f->body)) return true;
        }
    }
    if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        auto* rule = sema_.lookup_rule(rr->name);
        if (rule) return type_needs_free(rule->body);
    }
    if (dynamic_cast<Union*>(type)) return true;
    if (dynamic_cast<Switch*>(type)) return true;
    if (dynamic_cast<Alternatives*>(type)) return true;
    if (auto* opt = dynamic_cast<Optional*>(type))
        return type_needs_free(opt->element);
    return false;
}

void CTypeEmitter::emit_field_free(TypeExpr* type, const std::string& target,
                                    std::ostream& out, int indent) {
    std::string ind(indent * 4, ' ');

    if (auto* arr = dynamic_cast<Array*>(type)) {
        if (type_needs_free(arr->element)) {
            out << ind << "for (size_t i = 0; i < " << target << ".count; i++)\n";
            emit_field_free(arr->element, target + ".items[i]", out, indent + 1);
        }
        out << ind << "free(" << target << ".items);\n";
    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        auto* rule = sema_.lookup_rule(rr->name);
        if (rule && type_needs_free(rule->body)) {
            out << ind << prefixed_name(rr->name) << "_free(&" << target << ");\n";
        }
    } else if (auto* st = dynamic_cast<Struct*>(type)) {
        emit_struct_free_body(st, target + ".", out, indent);
    } else if (auto* opt = dynamic_cast<Optional*>(type)) {
        if (type_needs_free(opt->element)) {
            out << ind << "if (" << target << ".has_value)\n";
            emit_field_free(opt->element, target + ".value", out, indent + 1);
        }
    }
    // Union/Switch/Alternatives: would need tag-based dispatch to free the right variant.
    // For now, only the active variant's memory is leaked — acceptable for simple cases.
    // TODO: generate tag-dispatched free for unions.
}

void CTypeEmitter::emit_struct_free_body(Struct* st, const std::string& prefix,
                                          std::ostream& out, int indent) {
    for (auto* f : st->fields) {
        if (dynamic_cast<EndianSwitch*>(f->body)) continue;
        if (dynamic_cast<Compute*>(f->body)) continue;
        if (type_needs_free(f->body))
            emit_field_free(f->body, prefix + f->name, out, indent);
    }
}

void CTypeEmitter::emit_free_func(Rule* rule, std::ostream& out) {
    if (!type_needs_free(rule->body)) return;

    std::string tname = type_name(rule->name);
    std::string fname = prefixed_name(rule->name) + "_free";

    out << "static inline void " << fname << "(" << tname << "* p) {\n";
    out << "    (void)p;\n";

    if (auto* st = dynamic_cast<Struct*>(rule->body)) {
        emit_struct_free_body(st, "p->", out, 1);
    } else if (auto* arr = dynamic_cast<Array*>(rule->body)) {
        if (type_needs_free(arr->element)) {
            out << "    for (size_t i = 0; i < p->count; i++)\n";
            emit_field_free(arr->element, "p->items[i]", out, 2);
        }
        out << "    free(p->items);\n";
    } else if (auto* opt = dynamic_cast<Optional*>(rule->body)) {
        if (type_needs_free(opt->element)) {
            out << "    if (p->has_value)\n";
            emit_field_free(opt->element, "p->value", out, 2);
        }
    }

    out << "}\n\n";
}

void CTypeEmitter::emit_free_funcs(std::ostream& out) {
    bool any = false;
    for (auto* rule : sema_.sorted_rules()) {
        if (type_needs_free(rule->body)) { any = true; break; }
    }
    if (!any) return;

    out << "// ── Free functions ──\n\n";
    out << "#include <stdlib.h>\n\n";

    for (auto* rule : sema_.sorted_rules())
        emit_free_func(rule, out);
}

} // namespace bbqgen_c
