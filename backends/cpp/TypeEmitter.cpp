#include "TypeEmitter.h"
#include "FrameProcessor.h"
#include <algorithm>
#include <cctype>

using namespace BBQ;

namespace bbqgen {

// --- Public ---

std::string TypeEmitter::emit(Grammar* grammar) {
    std::ostringstream out;

    if (!namespace_.empty())
        out << "namespace " << namespace_ << " {\n\n";

    emit_forward_decls(out);
    out << "\n";
    emit_type_defs(out);

    if (!namespace_.empty())
        out << "} // namespace " << namespace_ << "\n";

    return out.str();
}

bool TypeEmitter::emit_to_frame(Grammar* grammar,
                                const std::string& frame_path,
                                std::ostream& out) {
    FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    // Copy up to -->namespace_open
    fp.CopyFramePart("namespace_open");
    if (!namespace_.empty())
        fp.EmitLine("namespace " + namespace_ + " {");
    fp.EmitLine();

    // Copy up to -->forward_declarations
    fp.CopyFramePart("forward_declarations");
    {
        std::ostringstream fwd;
        emit_forward_decls(fwd);
        fp.Emit(fwd.str());
    }

    // Copy up to -->type_definitions
    fp.CopyFramePart("type_definitions");
    {
        std::ostringstream defs;
        emit_type_defs(defs);
        fp.Emit(defs.str());
    }

    // Copy up to -->namespace_close
    fp.CopyFramePart("namespace_close");
    if (!namespace_.empty())
        fp.EmitLine("} // namespace " + namespace_);
    fp.EmitLine();
    return true;
}

// --- Forward declarations ---

void TypeEmitter::emit_forward_decls(std::ostream& out) {
    // Pre-scan all rules to register inline structs and collect their names
    for (auto* rule : sema_.sorted_rules()) {
        if (auto* st = dynamic_cast<Struct*>(rule->body)) {
            register_inline_structs(rule->name, st);
        }
    }
    // Emit forward decls for inline structs
    for (auto& [name, st] : inline_struct_order_) {
        out << "struct " << name << ";\n";
    }

    for (auto* rule : sema_.sorted_rules()) {
        auto* body = rule->body;
        if (dynamic_cast<Struct*>(body) ||
            dynamic_cast<Union*>(body) ||
            dynamic_cast<Switch*>(body) ||
            dynamic_cast<Alternatives*>(body) ||
            dynamic_cast<Bitfield*>(body)) {
            out << "struct " << rule->name << ";\n";
        }
        // Primitive/RuleRef/Array/Optional rules get using declarations, no fwd decl needed
    }
}

// --- Type definitions ---

void TypeEmitter::emit_type_defs(std::ostream& out) {
    for (auto* rule : sema_.sorted_rules()) {
        emit_rule_type(rule, out);
        out << "\n";
    }
}

void TypeEmitter::emit_rule_type(Rule* rule, std::ostream& out) {
    auto* body = rule->body;

    if (auto* st = dynamic_cast<Struct*>(body)) {
        // Emit any inline structs that belong to this rule (already registered)
        emit_inline_structs(rule->name, out);
        emit_struct_type(rule->name, st, out);
    } else if (auto* u = dynamic_cast<Union*>(body)) {
        emit_union_type(rule->name, u, out);
    } else if (auto* sw = dynamic_cast<Switch*>(body)) {
        emit_switch_type(rule->name, sw, out);
    } else if (auto* alts = dynamic_cast<Alternatives*>(body)) {
        emit_alternatives_type(rule->name, alts, out);
    } else if (auto* arr = dynamic_cast<Array*>(body)) {
        out << "using " << rule->name << " = std::vector<"
            << cpp_type(arr->element) << ">;\n";
    } else if (auto* opt = dynamic_cast<Optional*>(body)) {
        out << "using " << rule->name << " = std::optional<"
            << cpp_type(opt->element) << ">;\n";
    } else if (auto* prim = dynamic_cast<Primitive*>(body)) {
        out << "using " << rule->name << " = "
            << primitive_cpp_type(prim->kind) << ";\n";
    } else if (auto* rr = dynamic_cast<RuleRef*>(body)) {
        out << "using " << rule->name << " = " << rr->name << ";\n";
    } else if (auto* ext = dynamic_cast<Extern*>(body)) {
        out << "using " << rule->name << " = " << ext->cpp_type << ";\n";
    } else if (auto* bf = dynamic_cast<Bitfield*>(body)) {
        out << "struct " << rule->name << " {\n";
        for (auto* entry : bf->entries) {
            out << "    " << bitfield_entry_type(entry->width) << " " << entry->name << ";\n";
        }
        out << "};\n";
    }
}

// --- Struct ---

void TypeEmitter::emit_struct_type(const std::string& name, Struct* st, std::ostream& out) {
    out << "struct " << name << " {\n";
    for (auto* field : st->fields) {
        if (dynamic_cast<EndianSwitch*>(field->body)) continue;
        out << "    " << cpp_type(field->body) << " " << field->name << ";\n";
    }
    out << "};\n";
}

// --- Union ---

void TypeEmitter::emit_union_type(const std::string& name, Union* u, std::ostream& out) {
    out << "struct " << name << " {\n";
    // Collect variant types
    std::vector<std::string> types;
    for (auto* v : u->variants) {
        types.push_back(cpp_type(v->body));
    }
    out << "    std::variant<" << variant_type_list(types) << "> data;\n";
    out << "};\n";
}

// --- Switch ---

void TypeEmitter::emit_switch_type(const std::string& name, Switch* sw, std::ostream& out) {
    out << "struct " << name << " {\n";
    std::vector<std::string> types;
    for (auto* c : sw->cases) {
        types.push_back(cpp_type(c->target));
    }
    if (sw->default_ && !(*sw->default_)->is_reject) {
        // `default: reject` is unreachable — no payload type, no
        // variant slot needed. The parser emits a fail at this
        // position; the variant simply doesn't grow.
        types.push_back(cpp_type(*(*sw->default_)->target));
    }
    out << "    std::variant<" << variant_type_list(types) << "> data;\n";
    out << "};\n";
}

// --- Alternatives ---

void TypeEmitter::emit_alternatives_type(const std::string& name, Alternatives* alts, std::ostream& out) {
    out << "struct " << name << " {\n";
    std::vector<std::string> types;
    for (auto* alt : alts->alts) {
        types.push_back(cpp_type(alt));
    }
    out << "    std::variant<" << variant_type_list(types) << "> data;\n";
    out << "};\n";
}

// --- Helper ---

std::string TypeEmitter::variant_type_list(const std::vector<std::string>& types) {
    std::string result;
    for (size_t i = 0; i < types.size(); i++) {
        if (i > 0) result += ", ";
        result += types[i];
    }
    return result;
}

// --- Inline struct support ---

void TypeEmitter::register_inline_structs(const std::string& prefix, Struct* st) {
    for (auto* field : st->fields) {
        if (dynamic_cast<EndianSwitch*>(field->body)) continue;
        if (auto* nested = dynamic_cast<Struct*>(field->body)) {
            std::string name = prefix + "_" + field->name;
            if (inline_struct_names_.find(nested) == inline_struct_names_.end()) {
                // Recurse first so inner structs are emitted before outer
                register_inline_structs(name, nested);
                inline_struct_names_[nested] = name;
                inline_struct_order_.push_back({name, nested});
            }
        }
    }
}

void TypeEmitter::emit_inline_structs(const std::string& rule_name, std::ostream& out) {
    // Emit inline structs whose name starts with "RuleName_"
    std::string prefix = rule_name + "_";
    for (auto& [name, st] : inline_struct_order_) {
        if (name.substr(0, prefix.size()) == prefix) {
            emit_struct_type(name, st, out);
            out << "\n";
        }
    }
}

// --- Type mapping ---

std::string TypeEmitter::cpp_type(TypeExpr* type) {
    if (auto* prim = dynamic_cast<Primitive*>(type)) {
        return primitive_cpp_type(prim->kind);
    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        return rr->name;
    } else if (auto* arr = dynamic_cast<Array*>(type)) {
        return "std::vector<" + cpp_type(arr->element) + ">";
    } else if (auto* opt = dynamic_cast<Optional*>(type)) {
        return "std::optional<" + cpp_type(opt->element) + ">";
    } else if (auto* comp = dynamic_cast<Compute*>(type)) {
        return primitive_cpp_type(comp->result_type);
    } else if (auto* st = dynamic_cast<Struct*>(type)) {
        // Look up registered inline struct name
        auto it = inline_struct_names_.find(st);
        if (it != inline_struct_names_.end())
            return it->second;
        return "/* anonymous struct */";
    } else if (auto* sw = dynamic_cast<Switch*>(type)) {
        // Inline switch — return variant of case target types
        std::string result = "std::variant<";
        bool first = true;
        for (auto* c : sw->cases) {
            if (!first) result += ", ";
            first = false;
            result += cpp_type(c->target);
        }
        if (sw->default_ && !(*sw->default_)->is_reject) {
            if (!first) result += ", ";
            result += cpp_type(*(*sw->default_)->target);
        }
        result += ">";
        return result;
    } else if (auto* ext = dynamic_cast<Extern*>(type)) {
        return ext->cpp_type;
    } else if (auto* bf = dynamic_cast<Bitfield*>(type)) {
        std::string result = "struct { ";
        for (auto* entry : bf->entries) {
            result += bitfield_entry_type(entry->width) + " " + entry->name + "; ";
        }
        result += "}";
        return result;
    }
    return "/* unknown type */";
}

std::string TypeEmitter::primitive_cpp_type(PrimitiveKind* kind) {
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
        return "std::vector<uint8_t>";
    } else if (dynamic_cast<StringKind*>(kind)) {
        return "std::string";
    } else if (dynamic_cast<BoolKind*>(kind)) {
        return "bool";
    }
    return "/* unknown primitive */";
}

std::string TypeEmitter::bitfield_entry_type(int64_t width) {
    if (width <= 8) return "uint8_t";
    if (width <= 16) return "uint16_t";
    if (width <= 32) return "uint32_t";
    return "uint64_t";
}

} // namespace bbqgen
