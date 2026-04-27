#include "ParserEmitter.h"
#include "FrameProcessor.h"
#include <cinttypes>
#include <climits>

using namespace BBQ;

namespace bbqgen {

// --- Helpers ---

std::string ParserEmitter::ind(int n) {
    return std::string(n * 4, ' ');
}

// --- Public: declarations ---

std::string ParserEmitter::emit_decls(Grammar* grammar) {
    std::ostringstream out;
    if (!namespace_.empty())
        out << "namespace " << namespace_ << " {\n\n";
    for (auto* rule : sema_.sorted_rules()) {
        emit_decl(rule, out);
    }
    if (!namespace_.empty())
        out << "\n} // namespace " << namespace_ << "\n";
    return out.str();
}

// --- Public: implementations ---

std::string ParserEmitter::emit_impls(Grammar* grammar) {
    std::ostringstream out;
    if (!namespace_.empty())
        out << "namespace " << namespace_ << " {\n\n";
    for (auto* rule : sema_.sorted_rules()) {
        emit_impl(rule, out);
        out << "\n";
    }
    if (!namespace_.empty())
        out << "} // namespace " << namespace_ << "\n";
    return out.str();
}

// --- Frame-based emission ---

bool ParserEmitter::emit_header_to_frame(Grammar* grammar,
                                          const std::string& frame_path,
                                          const std::string& types_include,
                                          std::ostream& out) {
    FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    fp.CopyFramePart("types_include");
    fp.EmitLine("#include \"" + types_include + "\"");

    fp.CopyFramePart("namespace_open");
    if (!namespace_.empty())
        fp.EmitLine("namespace " + namespace_ + " {");
    fp.EmitLine();

    fp.CopyFramePart("parse_declarations");
    {
        std::ostringstream decls;
        for (auto* rule : sema_.sorted_rules())
            emit_decl(rule, decls);
        fp.Emit(decls.str());
    }

    fp.CopyFramePart("namespace_close");
    if (!namespace_.empty())
        fp.EmitLine("} // namespace " + namespace_);
    fp.EmitLine();
    return true;
}

bool ParserEmitter::emit_impl_to_frame(Grammar* grammar,
                                         const std::string& frame_path,
                                         const std::string& parser_include,
                                         std::ostream& out) {
    FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    fp.CopyFramePart("parser_include");
    fp.EmitLine("#include \"" + parser_include + "\"");

    fp.CopyFramePart("namespace_open");
    if (!namespace_.empty())
        fp.EmitLine("namespace " + namespace_ + " {");
    fp.EmitLine();

    fp.CopyFramePart("parse_implementations");
    {
        std::ostringstream impls;
        for (auto* rule : sema_.sorted_rules()) {
            emit_impl(rule, impls);
            impls << "\n";
        }
        fp.Emit(impls.str());
    }

    fp.CopyFramePart("namespace_close");
    if (!namespace_.empty())
        fp.EmitLine("} // namespace " + namespace_);
    fp.EmitLine();
    return true;
}

// --- Declaration ---

void ParserEmitter::emit_decl(Rule* rule, std::ostream& out) {
    std::string type_name = cpp_type(rule->body);

    if (dynamic_cast<Struct*>(rule->body) ||
        dynamic_cast<Union*>(rule->body) ||
        dynamic_cast<Alternatives*>(rule->body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out);\n";
    } else if (auto* sw = dynamic_cast<Switch*>(rule->body)) {
        std::string disc_type = has_string_cases(sw) ? "const std::string&" : "int64_t";
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out, " << disc_type << " discriminator);\n";
    } else if (dynamic_cast<Array*>(rule->body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out);\n";
    } else if (dynamic_cast<Optional*>(rule->body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out);\n";
    } else if (dynamic_cast<Primitive*>(rule->body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << type_name << "& out);\n";
    } else if (auto* rr = dynamic_cast<RuleRef*>(rule->body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rr->name << "& out);\n";
    } else if (auto* ext = dynamic_cast<Extern*>(rule->body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << ext->cpp_type << "& out);\n";
    } else if (dynamic_cast<Bitfield*>(rule->body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out);\n";
    }
}

// --- Implementation ---

void ParserEmitter::emit_impl(Rule* rule, std::ostream& out) {
    auto* body = rule->body;
    current_rule_name_ = rule->name;

    if (auto* st = dynamic_cast<Struct*>(body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out) {\n";
        // Initialize scope chain with top-level struct fields
        scopes_.clear();
        EmitScope top_scope;
        top_scope.prefix = "out.";
        for (auto* f : st->fields) {
            if (!dynamic_cast<EndianSwitch*>(f->body))
                top_scope.fields.insert(f->name);
        }
        scopes_.push_back(std::move(top_scope));
        emit_struct_body(rule->name, st, out, 1);
        scopes_.clear();
        out << ind(1) << "return true;\n";
        out << "}\n";
    } else if (auto* u = dynamic_cast<Union*>(body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out) {\n";
        emit_union_body(rule->name, u, "out.data", out, 1);
        out << "}\n";
    } else if (auto* sw = dynamic_cast<Switch*>(body)) {
        std::string disc_type = has_string_cases(sw) ? "const std::string&" : "int64_t";
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out, " << disc_type << " discriminator) {\n";
        emit_switch_body(rule->name, sw, "discriminator", "out.data", out, 1);
        out << "}\n";
    } else if (auto* alts = dynamic_cast<Alternatives*>(body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out) {\n";
        emit_alternatives_body(rule->name, alts, "out.data", out, 1);
        out << "}\n";
    } else if (auto* arr = dynamic_cast<Array*>(body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out) {\n";
        emit_array_body(rule->name, arr, "out", out, 1);
        out << "}\n";
    } else if (auto* opt = dynamic_cast<Optional*>(body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out) {\n";
        emit_optional_body(rule->name, opt, "out", out, 1);
        out << "}\n";
    } else if (auto* prim = dynamic_cast<Primitive*>(body)) {
        std::string type_name = cpp_type(body);
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << type_name << "& out) {\n";
        out << ind(1) << "if (!ctx." << read_method(prim->kind) << "(out))\n";
        out << ind(2) << "return ctx.fail(\"" << rule->name << ": read failed\");\n";
        if (prim->constraint) {
            out << ind(1) << "if (!(" << compile_expr(*prim->constraint) << "))\n";
            out << ind(2) << "return ctx.fail(\"" << rule->name << ": constraint failed\");\n";
        }
        out << ind(1) << "return true;\n";
        out << "}\n";
    } else if (auto* rr = dynamic_cast<RuleRef*>(body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rr->name << "& out) {\n";
        out << ind(1) << "return parse_" << rr->name << "(ctx, out);\n";
        out << "}\n";
    } else if (auto* ext = dynamic_cast<Extern*>(body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << ext->cpp_type << "& out) {\n";
        out << ind(1) << "if (!" << ext->func_name << "(ctx, out))\n";
        out << ind(2) << "return ctx.fail(\"" << rule->name << ": read failed\");\n";
        if (ext->constraint) {
            out << ind(1) << "if (!(" << compile_expr(*ext->constraint) << "))\n";
            out << ind(2) << "return ctx.fail(\"" << rule->name << ": constraint failed\");\n";
        }
        out << ind(1) << "return true;\n";
        out << "}\n";
    } else if (auto* bf = dynamic_cast<Bitfield*>(body)) {
        out << "bool parse_" << rule->name << "(bbq::BBQContext& ctx, "
            << rule->name << "& out) {\n";
        emit_bitfield_read(rule->name, bf, "out", out, 1);
        out << ind(1) << "return true;\n";
        out << "}\n";
    }
}

// --- Struct body ---

void ParserEmitter::emit_struct_body(const std::string& rule_name, Struct* st, std::ostream& out, int indent) {
    for (auto* field : st->fields) {
        emit_field_read(rule_name, field, out, indent);
    }
}

void ParserEmitter::emit_field_read(const std::string& rule_name, Field* field, std::ostream& out, int indent) {
    // EndianSwitch pseudo-field: emit ctx.set_endian() call
    if (auto* es = dynamic_cast<EndianSwitch*>(field->body)) {
        out << ind(indent) << "ctx.set_endian(" << compile_expr(es->endian_expr) << ");\n";
        return;
    }

    // Use the innermost scope's prefix for the target
    std::string prefix = scopes_.empty() ? "out." : scopes_.back().prefix;
    std::string target = prefix + field->name;
    std::string ctx_name = rule_name + "." + field->name;

    // Find interval — may be on the Field, or on the type body (RuleRef, Primitive, Extern)
    Interval* interval_node = nullptr;
    if (field->interval.has_value()) {
        interval_node = *field->interval;
    } else if (auto* rr = dynamic_cast<RuleRef*>(field->body)) {
        if (rr->interval.has_value()) interval_node = *rr->interval;
    } else if (auto* ext = dynamic_cast<Extern*>(field->body)) {
        if (ext->interval.has_value()) interval_node = *ext->interval;
    }
    // Note: Primitive intervals are length-style (bytes[N]) and handled in emit_read_call

    bool has_interval = false;
    if (interval_node) {
        if (auto* se = dynamic_cast<StartEnd*>(interval_node)) {
            has_interval = true;
            out << ind(indent) << "{\n";
            out << ind(indent + 1) << "auto scope = ctx.push_interval("
                << compile_expr(se->start) << ", " << compile_expr(se->end) << ");\n";
            indent++;
        }
        // Length intervals are handled inside emit_read_call for bytes/string
    }

    // Compute fields: inline expression assignment
    if (auto* comp = dynamic_cast<Compute*>(field->body)) {
        out << ind(indent) << target << " = " << compile_expr(comp->expression) << ";\n";
    } else {
        emit_read_call(target, field->body, ctx_name, out, indent);
    }

    // Constraint check
    if (field->constraint) {
        out << ind(indent) << "if (!(" << compile_expr(*field->constraint) << "))\n";
        out << ind(indent + 1) << "return ctx.fail(\"" << ctx_name << ": constraint failed\");\n";
    }

    // Close interval scope
    if (has_interval) {
        indent--;
        out << ind(indent) << "}\n";
    }
}

void ParserEmitter::emit_read_call(const std::string& target, TypeExpr* type, const std::string& ctx, std::ostream& out, int indent, const std::string& on_fail) {
    // Helper: emit on_fail action + return ctx.fail(...)
    auto emit_fail = [&](const std::string& msg) {
        if (!on_fail.empty()) {
            out << ind(indent) << "{\n";
            out << ind(indent + 1) << on_fail << ";\n";
            out << ind(indent + 1) << "return ctx.fail(\"" << msg << "\");\n";
            out << ind(indent) << "}\n";
        } else {
            out << ind(indent + 1) << "return ctx.fail(\"" << msg << "\");\n";
        }
    };

    if (auto* prim = dynamic_cast<Primitive*>(type)) {
        // Check for bytes/string with interval (length)
        if (auto* bk = dynamic_cast<BytesKind*>(prim->kind)) {
            if (prim->interval) {
                if (auto* len = dynamic_cast<Length*>(*prim->interval)) {
                    out << ind(indent) << "if (!ctx.read_bytes(" << target << ", "
                        << compile_expr(len->length) << "))\n";
                    emit_fail(ctx + ": read bytes failed");
                    return;
                }
            }
            // bytes without length — read remaining
            out << ind(indent) << "if (!ctx.read_bytes(" << target << ", ctx.remaining()))\n";
            emit_fail(ctx + ": read bytes failed");
            return;
        }
        if (auto* sk = dynamic_cast<StringKind*>(prim->kind)) {
            if (prim->interval) {
                if (auto* len = dynamic_cast<Length*>(*prim->interval)) {
                    out << ind(indent) << "if (!ctx.read_string(" << target << ", "
                        << compile_expr(len->length) << "))\n";
                    emit_fail(ctx + ": read string failed");
                    return;
                }
            }
            out << ind(indent) << "if (!ctx.read_string(" << target << ", ctx.remaining()))\n";
            emit_fail(ctx + ": read string failed");
            return;
        }
        out << ind(indent) << "if (!ctx." << read_method(prim->kind) << "(" << target << "))\n";
        emit_fail(ctx + ": read failed");
        // Primitive-level constraint
        if (prim->constraint) {
            out << ind(indent) << "if (!(" << compile_expr(*prim->constraint) << "))\n";
            emit_fail(ctx + ": constraint failed");
        }
    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        // Wrap with ScopeGuard so child rule can access parent fields at runtime
        bool has_parent = !scopes_.empty();
        if (has_parent) {
            parent_contexts_.push_back({current_rule_name_, scopes_.front().fields});
            out << ind(indent) << "{ bbq::ScopeGuard _scope(ctx, &out);\n";
            indent++;
        }
        // Check if the target rule is a Switch (needs discriminator argument)
        auto* target_rule = sema_.lookup_rule(rr->name);
        if (target_rule && dynamic_cast<Switch*>(target_rule->body)) {
            auto* sw = dynamic_cast<Switch*>(target_rule->body);
            std::string disc = compile_expr(sw->discriminator);
            out << ind(indent) << "if (!parse_" << rr->name << "(ctx, " << target << ", "
                << disc << "))\n";
        } else {
            out << ind(indent) << "if (!parse_" << rr->name << "(ctx, " << target << "))\n";
        }
        emit_fail(ctx);
        if (has_parent) {
            indent--;
            out << ind(indent) << "}\n";
            parent_contexts_.pop_back();
        }
    } else if (auto* arr = dynamic_cast<Array*>(type)) {
        emit_array_body(ctx, arr, target, out, indent);
    } else if (auto* opt = dynamic_cast<Optional*>(type)) {
        emit_optional_body(ctx, opt, target, out, indent);
    } else if (auto* ext = dynamic_cast<Extern*>(type)) {
        out << ind(indent) << "if (!" << ext->func_name << "(ctx, " << target << "))\n";
        emit_fail(ctx + ": read failed");
        if (ext->constraint) {
            out << ind(indent) << "if (!(" << compile_expr(*ext->constraint) << "))\n";
            emit_fail(ctx + ": constraint failed");
        }
    } else if (auto* sw = dynamic_cast<Switch*>(type)) {
        // Inline switch — discriminator from compiled expression, target is the field
        emit_switch_body(ctx, sw, compile_expr(sw->discriminator), target, out, indent, /*is_inline=*/true);
    } else if (auto* nested_st = dynamic_cast<Struct*>(type)) {
        // Inline nested struct: push a new scope and emit field reads
        EmitScope nested_scope;
        nested_scope.prefix = target + ".";
        for (auto* f : nested_st->fields) {
            if (!dynamic_cast<EndianSwitch*>(f->body))
                nested_scope.fields.insert(f->name);
        }
        scopes_.push_back(std::move(nested_scope));
        emit_struct_body(ctx, nested_st, out, indent);
        scopes_.pop_back();
    } else if (auto* bf = dynamic_cast<Bitfield*>(type)) {
        emit_bitfield_read(ctx, bf, target, out, indent);
    }
}

// --- Union body ---

void ParserEmitter::emit_union_body(const std::string& rule_name, Union* u, const std::string& target, std::ostream& out, int indent) {
    for (size_t i = 0; i < u->variants.size(); i++) {
        out << ind(indent) << "{\n";
        out << ind(indent + 1) << "auto saved = ctx.save();\n";
        out << ind(indent + 1) << "auto& val = " << target << ".emplace<" << i << ">();\n";
        emit_try_parse_block(u->variants[i]->body, "val", rule_name, out, indent + 1);
        out << ind(indent + 1) << "ctx.restore(saved);\n";
        out << ind(indent) << "}\n";
    }
    out << ind(indent) << "return ctx.fail(\"" << rule_name << ": no variant matched\");\n";
}

// --- Switch body ---

bool ParserEmitter::has_string_cases(Switch* sw) {
    for (auto* c : sw->cases) {
        if (dynamic_cast<StrValue*>(c->value)) return true;
    }
    return false;
}

void ParserEmitter::emit_switch_body(const std::string& rule_name, Switch* sw, const std::string& disc_expr, const std::string& target, std::ostream& out, int indent, bool is_inline) {
    // When inline (switch is a field inside a struct), use break instead of
    // return so that subsequent struct fields are parsed after the switch.
    const char* exit_stmt = is_inline ? "break" : "return true";
    if (has_string_cases(sw)) {
        // String switch — emit if-else chain
        bool first = true;
        size_t variant_idx = 0;
        for (auto* c : sw->cases) {
            auto* sv = dynamic_cast<StrValue*>(c->value);
            if (!sv) { variant_idx++; continue; } // skip non-string (shouldn't mix)
            out << ind(indent);
            if (!first) out << "} else ";
            out << "if (" << disc_expr << " == \"" << sv->value << "\") {\n";
            out << ind(indent + 1) << "auto& val = " << target << ".emplace<" << variant_idx << ">();\n";
            emit_read_call("val", c->target, rule_name, out, indent + 1);
            out << ind(indent + 1) << exit_stmt << ";\n";
            first = false;
            variant_idx++;
        }
        if (sw->default_) {
            out << ind(indent) << "} else {\n";
            out << ind(indent + 1) << "auto& val = " << target << ".emplace<" << variant_idx << ">();\n";
            emit_read_call("val", (*sw->default_)->target, rule_name, out, indent + 1);
            out << ind(indent + 1) << exit_stmt << ";\n";
            out << ind(indent) << "}\n";
        } else {
            out << ind(indent) << "}\n";
            out << ind(indent) << "return ctx.fail(\"" << rule_name << ": unhandled case\");\n";
        }
        return;
    }

    // Integer/enum switch — emit C++ switch statement
    out << ind(indent) << "switch (static_cast<int64_t>(" << disc_expr << ")) {\n";
    size_t variant_idx = 0;
    for (auto* c : sw->cases) {
        if (auto* iv = dynamic_cast<IntValue*>(c->value)) {
            out << ind(indent) << "case " << iv->value << ": {\n";
        } else if (auto* idv = dynamic_cast<IdentValue*>(c->value)) {
            out << ind(indent) << "case " << idv->name << ": {\n";
        } else if (auto* rv = dynamic_cast<RefValue*>(c->value)) {
            std::string path;
            for (size_t j = 0; j < rv->path.size(); j++) {
                if (j > 0) path += "::";
                path += rv->path[j];
            }
            out << ind(indent) << "case " << path << ": {\n";
        }
        out << ind(indent + 1) << "auto& val = " << target << ".emplace<" << variant_idx << ">();\n";
        emit_read_call("val", c->target, rule_name, out, indent + 1);
        out << ind(indent + 1) << exit_stmt << ";\n";
        out << ind(indent) << "}\n";
        variant_idx++;
    }
    if (sw->default_) {
        out << ind(indent) << "default: {\n";
        out << ind(indent + 1) << "auto& val = " << target << ".emplace<" << variant_idx << ">();\n";
        emit_read_call("val", (*sw->default_)->target, rule_name, out, indent + 1);
        out << ind(indent + 1) << exit_stmt << ";\n";
        out << ind(indent) << "}\n";
    }
    if (!sw->default_) {
        out << ind(indent) << "default:\n";
        out << ind(indent + 1) << "return ctx.fail(\"" << rule_name << ": unhandled case\");\n";
    }
    out << ind(indent) << "}\n";
}

// --- Alternatives body ---

void ParserEmitter::emit_alternatives_body(const std::string& rule_name, Alternatives* alts, const std::string& target, std::ostream& out, int indent) {
    for (size_t i = 0; i < alts->alts.size(); i++) {
        out << ind(indent) << "{\n";
        out << ind(indent + 1) << "auto saved = ctx.save();\n";
        out << ind(indent + 1) << "auto& val = " << target << ".emplace<" << i << ">();\n";
        emit_try_parse_block(alts->alts[i], "val", rule_name, out, indent + 1);
        out << ind(indent + 1) << "ctx.restore(saved);\n";
        out << ind(indent) << "}\n";
    }
    out << ind(indent) << "return ctx.fail(\"" << rule_name << ": no alternative matched\");\n";
}

// --- Array body ---

void ParserEmitter::emit_array_body(const std::string& rule_name, Array* arr, const std::string& target, std::ostream& out, int indent) {
    std::string elem_type = cpp_type(arr->element);

    if (auto* fc = dynamic_cast<FixedCount*>(arr->spec)) {
        // Fixed count array
        std::string count_expr = compile_expr(fc->count);
        out << ind(indent) << "{ bbq::LoopGuard _lg(ctx);\n";
        out << ind(indent + 1) << "auto count = static_cast<size_t>(" << count_expr << ");\n";
        out << ind(indent + 1) << target << ".resize(count);\n";
        out << ind(indent + 1) << "for (size_t i = 0; i < count; i++) {\n";
        out << ind(indent + 2) << "ctx.set_loop_index(static_cast<int64_t>(i));\n";
        int body_indent = indent + 2;
        if (arr->element_interval) {
            auto* se = dynamic_cast<StartEnd*>(*arr->element_interval);
            out << ind(body_indent) << "auto elem_scope = ctx.push_interval("
                << compile_expr(se->start) << ", " << compile_expr(se->end) << ");\n";
        }
        array_nesting_depth_++;
        emit_read_call(target + "[i]", arr->element, rule_name + "[]", out, body_indent);
        array_nesting_depth_--;
        out << ind(indent + 1) << "}\n";
        out << ind(indent) << "}\n";
    } else if (auto* st = dynamic_cast<SepTerm*>(arr->spec)) {
        out << ind(indent) << target << ".clear();\n";

        // Helper lambda to emit separator parse code
        auto emit_sep = [&](const std::string& guard, int base_indent) {
            if (auto* ts = dynamic_cast<TypeSep*>(st->sep)) {
                std::string sep_type = cpp_type(ts->sep_type);
                out << ind(base_indent) << "if (" << guard << ") {\n";
                out << ind(base_indent + 1) << sep_type << " sep_;\n";
                emit_read_call("sep_", ts->sep_type, rule_name + ".sep", out, base_indent + 1);
                out << ind(base_indent) << "}\n";
            }
        };

        std::string pop_back = target + ".pop_back()";

        if (dynamic_cast<EofTerm*>(st->term)) {
            // Read until end
            out << ind(indent) << "{ bbq::LoopGuard _lg(ctx);\n";
            out << ind(indent + 1) << "size_t i = 0;\n";
            out << ind(indent + 1) << "while (!ctx.at_end()) {\n";
            out << ind(indent + 2) << "ctx.set_loop_index(static_cast<int64_t>(i));\n";
            emit_sep("!" + target + ".empty()", indent + 2);
            out << ind(indent + 2) << target << ".emplace_back();\n";
            array_nesting_depth_++;
            emit_read_call(target + ".back()", arr->element, rule_name + "[]", out, indent + 2, pop_back);
            array_nesting_depth_--;
            out << ind(indent + 2) << "i++;\n";
            out << ind(indent + 1) << "}\n";
            out << ind(indent) << "}\n";
        } else if (auto* ct = dynamic_cast<CountTerm*>(st->term)) {
            std::string count_expr = compile_expr(ct->count);
            out << ind(indent) << "{ bbq::LoopGuard _lg(ctx);\n";
            out << ind(indent + 1) << "auto count = static_cast<size_t>(" << count_expr << ");\n";
            out << ind(indent + 1) << target << ".reserve(count);\n";
            out << ind(indent + 1) << "for (size_t i = 0; i < count; i++) {\n";
            out << ind(indent + 2) << "ctx.set_loop_index(static_cast<int64_t>(i));\n";
            emit_sep("i > 0", indent + 2);
            int body_indent = indent + 2;
            if (arr->element_interval) {
                auto* se = dynamic_cast<StartEnd*>(*arr->element_interval);
                out << ind(body_indent) << "auto elem_scope = ctx.push_interval("
                    << compile_expr(se->start) << ", " << compile_expr(se->end) << ");\n";
            }
            out << ind(body_indent) << target << ".emplace_back();\n";
            array_nesting_depth_++;
            emit_read_call(target + ".back()", arr->element, rule_name + "[]", out, body_indent, pop_back);
            array_nesting_depth_--;
            out << ind(indent + 1) << "}\n";
            out << ind(indent) << "}\n";
        } else if (auto* ut = dynamic_cast<UntilTerm*>(st->term)) {
            std::string cond = compile_expr(ut->condition);
            out << ind(indent) << "{ bbq::LoopGuard _lg(ctx);\n";
            out << ind(indent + 1) << "size_t i = 0;\n";
            out << ind(indent + 1) << "while (!(" << cond << ")) {\n";
            out << ind(indent + 2) << "ctx.set_loop_index(static_cast<int64_t>(i));\n";
            emit_sep("!" + target + ".empty()", indent + 2);
            out << ind(indent + 2) << target << ".emplace_back();\n";
            array_nesting_depth_++;
            emit_read_call(target + ".back()", arr->element, rule_name + "[]", out, indent + 2, pop_back);
            array_nesting_depth_--;
            out << ind(indent + 2) << "i++;\n";
            out << ind(indent + 1) << "}\n";
            out << ind(indent) << "}\n";
        } else if (auto* tt = dynamic_cast<BBQ::TypeTerm*>(st->term)) {
            // Type terminator — try to parse terminator, if success break
            std::string term_type = cpp_type(tt->term_type);
            out << ind(indent) << "{ bbq::LoopGuard _lg(ctx);\n";
            out << ind(indent + 1) << "size_t i = 0;\n";
            out << ind(indent + 1) << "while (true) {\n";
            out << ind(indent + 2) << "{\n";
            out << ind(indent + 3) << "auto saved = ctx.save();\n";
            out << ind(indent + 3) << term_type << " term;\n";
            out << ind(indent + 3) << "if (parse_" << term_type << "(ctx, term)) break;\n";
            out << ind(indent + 3) << "ctx.restore(saved);\n";
            out << ind(indent + 2) << "}\n";
            out << ind(indent + 2) << "ctx.set_loop_index(static_cast<int64_t>(i));\n";
            emit_sep("!" + target + ".empty()", indent + 2);
            out << ind(indent + 2) << target << ".emplace_back();\n";
            array_nesting_depth_++;
            emit_read_call(target + ".back()", arr->element, rule_name + "[]", out, indent + 2, pop_back);
            array_nesting_depth_--;
            out << ind(indent + 2) << "i++;\n";
            out << ind(indent + 1) << "}\n";
            out << ind(indent) << "}\n";
        }
    }
}

// --- Optional body ---

void ParserEmitter::emit_optional_body(const std::string& rule_name, Optional* opt, const std::string& target, std::ostream& out, int indent) {
    if (opt->constraint) {
        // Predicated optional
        out << ind(indent) << "if (" << compile_expr(*opt->constraint) << ") {\n";
        std::string elem_type = cpp_type(opt->element);
        out << ind(indent + 1) << elem_type << " tmp;\n";
        emit_read_call("tmp", opt->element, rule_name, out, indent + 1);
        out << ind(indent + 1) << target << " = std::move(tmp);\n";
        out << ind(indent) << "} else {\n";
        out << ind(indent + 1) << target << " = std::nullopt;\n";
        out << ind(indent) << "}\n";
    } else {
        // Try/restore optional
        out << ind(indent) << "{\n";
        out << ind(indent + 1) << "auto saved = ctx.save();\n";
        std::string elem_type = cpp_type(opt->element);
        out << ind(indent + 1) << elem_type << " tmp;\n";
        out << ind(indent + 1) << "if (";
        emit_try_parse(opt->element, "tmp", out);
        out << ") {\n";
        out << ind(indent + 2) << target << " = std::move(tmp);\n";
        out << ind(indent + 1) << "} else {\n";
        out << ind(indent + 2) << "ctx.restore(saved);\n";
        out << ind(indent + 2) << target << " = std::nullopt;\n";
        out << ind(indent + 1) << "}\n";
        out << ind(indent) << "}\n";
    }
}

// --- Bitfield read ---

static int width_to_bits(Width w) {
    switch (w) {
        case Width::W8:  return 8;
        case Width::W16: return 16;
        case Width::W32: return 32;
        case Width::W64: return 64;
    }
    return 0;
}

static std::string hex_mask(uint64_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIX64, v);
    return buf;
}

void ParserEmitter::emit_bitfield_read(const std::string& ctx_name, Bitfield* bf,
    const std::string& target, std::ostream& out, int indent) {
    auto* ik = dynamic_cast<IntegerKind*>(bf->container);
    std::string container_type = types_.primitive_cpp_type(bf->container);
    std::string method = read_method(bf->container);

    out << ind(indent) << "{\n";
    out << ind(indent+1) << container_type << " _bf;\n";
    out << ind(indent+1) << "if (!ctx." << method << "(_bf))\n";
    out << ind(indent+2) << "return ctx.fail(\"" << ctx_name << ": read failed\");\n";

    Endianness e = ik->endian;
    if (e == Endianness::Default) e = sema_.default_endian();
    bool msb_first = (e == Endianness::Big);

    int container_bits = width_to_bits(ik->width);

    if (msb_first) {
        int bit_offset = container_bits;
        for (auto* entry : bf->entries) {
            bit_offset -= entry->width;
            uint64_t mask = (1ULL << entry->width) - 1;
            std::string entry_type = types_.bitfield_entry_type(entry->width);
            out << ind(indent+1) << target << "." << entry->name
                << " = static_cast<" << entry_type << ">(";
            if (bit_offset > 0)
                out << "(_bf >> " << bit_offset << ")";
            else
                out << "_bf";
            out << " & 0x" << hex_mask(mask) << ");\n";
        }
    } else {
        int bit_offset = 0;
        for (auto* entry : bf->entries) {
            uint64_t mask = (1ULL << entry->width) - 1;
            std::string entry_type = types_.bitfield_entry_type(entry->width);
            out << ind(indent+1) << target << "." << entry->name
                << " = static_cast<" << entry_type << ">(";
            if (bit_offset > 0)
                out << "(_bf >> " << bit_offset << ")";
            else
                out << "_bf";
            out << " & 0x" << hex_mask(mask) << ");\n";
            bit_offset += entry->width;
        }
    }

    out << ind(indent) << "}\n";
}

// --- Expression compilation ---

std::string ParserEmitter::compile_expr(Expr* expr) {
    if (auto* bin = dynamic_cast<BinOp*>(expr)) {
        std::string op;
        switch (bin->op) {
            case Binop::Add: op = " + "; break;
            case Binop::Sub: op = " - "; break;
            case Binop::Mul: op = " * "; break;
            case Binop::Div: op = " / "; break;
            case Binop::Mod: op = " % "; break;
            case Binop::BitAnd: op = " & "; break;
            case Binop::BitOr: op = " | "; break;
            case Binop::BitXor: op = " ^ "; break;
            case Binop::Shl: op = " << "; break;
            case Binop::Shr: op = " >> "; break;
            case Binop::And: op = " && "; break;
            case Binop::Or: op = " || "; break;
            case Binop::Eq: op = " == "; break;
            case Binop::Ne: op = " != "; break;
            case Binop::Lt: op = " < "; break;
            case Binop::Le: op = " <= "; break;
            case Binop::Gt: op = " > "; break;
            case Binop::Ge: op = " >= "; break;
        }
        return "(" + compile_expr(bin->left) + op + compile_expr(bin->right) + ")";
    } else if (auto* un = dynamic_cast<UnaryOp*>(expr)) {
        switch (un->op) {
            case Unaryop::Neg: return "(-" + compile_expr(un->operand) + ")";
            case Unaryop::BitNot: return "(~" + compile_expr(un->operand) + ")";
            case Unaryop::Not: return "(!" + compile_expr(un->operand) + ")";
            case Unaryop::Abs: return "bbq::abs(" + compile_expr(un->operand) + ")";
        }
    } else if (auto* tern = dynamic_cast<Ternary*>(expr)) {
        return "(" + compile_expr(tern->cond) + " ? "
             + compile_expr(tern->then_branch) + " : "
             + compile_expr(tern->else_branch) + ")";
    } else if (auto* il = dynamic_cast<IntLit*>(expr)) {
        if (il->value > INT32_MAX) {
            // Emit as unsigned hex literal for large values
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%" PRIX64 "ULL", static_cast<uint64_t>(il->value));
            return buf;
        } else if (il->value < 0) {
            return std::to_string(il->value) + "LL";
        }
        return std::to_string(il->value);
    } else if (auto* fl = dynamic_cast<FloatLit*>(expr)) {
        return std::to_string(fl->value) + "f";
    } else if (auto* sl = dynamic_cast<StrLit*>(expr)) {
        std::string escaped;
        for (char c : sl->value) {
            switch (c) {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:   escaped += c; break;
            }
        }
        return "\"" + escaped + "\"";
    } else if (auto* bl = dynamic_cast<BoolLit*>(expr)) {
        return bl->value ? "true" : "false";
    } else if (auto* ref = dynamic_cast<Ref*>(expr)) {
        return compile_ref_path(ref->path);
    } else if (auto* call = dynamic_cast<Call*>(expr)) {
        if (call->func == "peek") return "ctx.peek()";
        std::string result = "bbq::" + call->func + "(";
        for (size_t i = 0; i < call->args.size(); i++) {
            if (i > 0) result += ", ";
            result += compile_expr(call->args[i]);
        }
        result += ")";
        return result;
    } else if (auto* el = dynamic_cast<EndianLit*>(expr)) {
        return (el->value == Endianness::Little) ? "true" : "false";
    } else if (dynamic_cast<EOI*>(expr)) {
        return "ctx.total_size()";
    }
    return "/* unknown expr */";
}

std::string ParserEmitter::compile_ref_path(RefPath* path) {
    if (auto* simple = dynamic_cast<Simple*>(path)) {
        // Built-in context properties
        if (simple->name == "remaining") return "ctx.remaining()";
        if (simple->name == "pos") return "ctx.pos()";
        if (simple->name == "at_end") return "ctx.at_end()";
        if (simple->name == "i") {
            return (array_nesting_depth_ > 0) ? "i" : "ctx.loop_index()";
        }
        // 1. Compile-time scope chain (existing)
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->fields.count(simple->name))
                return it->prefix + simple->name;
        }
        // 2. Sema-resolved cross-rule reference
        if (simple->resolved_path.has_value() && simple->resolved_parent.has_value()) {
            return "static_cast<" + *simple->resolved_parent
                 + "*>(ctx.scope_ptr(0))->" + *simple->resolved_path;
        }
        // 3. Parent context stack (runtime resolution via ctx scope pointers)
        for (int idx = (int)parent_contexts_.size() - 1; idx >= 0; idx--) {
            if (parent_contexts_[idx].fields.count(simple->name)) {
                int levels_up = (int)parent_contexts_.size() - 1 - idx;
                return "static_cast<" + parent_contexts_[idx].rule_name
                     + "*>(ctx.scope_ptr(" + std::to_string(levels_up)
                     + "))->" + simple->name;
            }
        }
        // Fallback: bare name (shouldn't happen if sema passed)
        return simple->name;
    } else if (auto* fa = dynamic_cast<FieldAcc*>(path)) {
        return compile_ref_path(fa->base) + "." + fa->field;
    } else if (auto* ia = dynamic_cast<IndexAcc*>(path)) {
        return compile_ref_path(ia->base) + "[" + compile_expr(ia->index) + "]";
    }
    return "/* unknown path */";
}

// --- Read method name ---

std::string ParserEmitter::read_method(PrimitiveKind* kind) {
    if (auto* ik = dynamic_cast<IntegerKind*>(kind)) {
        std::string base = (ik->sign == Signedness::Unsigned) ? "read_uint" : "read_int";
        std::string width;
        switch (ik->width) {
            case Width::W8:  width = "8"; break;
            case Width::W16: width = "16"; break;
            case Width::W32: width = "32"; break;
            case Width::W64: width = "64"; break;
        }
        if (ik->width == Width::W8) return base + width;
        Endianness e = ik->endian;
        if (e == Endianness::Default) {
            // Use dispatching method (runtime endianness)
            return base + width;
        }
        std::string suffix = (e == Endianness::Big) ? "be" : "le";
        return base + width + suffix;
    } else if (auto* fk = dynamic_cast<FloatKind*>(kind)) {
        std::string width = (fk->width == Width::W64) ? "64" : "32";
        Endianness e = fk->endian;
        if (e == Endianness::Default) {
            return "read_float" + width;
        }
        std::string suffix = (e == Endianness::Big) ? "be" : "le";
        return "read_float" + width + suffix;
    } else if (dynamic_cast<BoolKind*>(kind)) {
        return "read_bool";
    }
    return "read_uint8"; // fallback
}

// --- Helper for try-parse in backtracking contexts ---

void ParserEmitter::emit_try_parse(TypeExpr* type, const std::string& target, std::ostream& out) {
    if (auto* prim = dynamic_cast<Primitive*>(type)) {
        out << "ctx." << read_method(prim->kind) << "(" << target << ")";
    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        out << "parse_" << rr->name << "(ctx, " << target << ")";
    } else if (auto* ext = dynamic_cast<Extern*>(type)) {
        out << ext->func_name << "(ctx, " << target << ")";
    } else {
        // Fallback — complex inline types in union/alternatives
        out << "false /* complex inline type */";
    }
}

void ParserEmitter::emit_try_parse_block(TypeExpr* type, const std::string& target, const std::string& rule_name, std::ostream& out, int indent) {
    if (auto* st = dynamic_cast<Struct*>(type)) {
        // Inline struct: parse fields sequentially, return true if all succeed
        // We use a do { ... } while(false) block so we can break on failure
        out << ind(indent) << "do {\n";
        out << ind(indent + 1) << "bool ok_ = true;\n";
        for (auto* field : st->fields) {
            if (auto* comp = dynamic_cast<Compute*>(field->body)) {
                out << ind(indent + 1) << target << "." << field->name
                    << " = " << compile_expr(comp->expression) << ";\n";
            } else {
                // Emit a read into the field of the struct variant
                std::string ftarget = target + "." + field->name;
                // Use a conditional to check success
                out << ind(indent + 1) << "if (!(";
                emit_try_parse(field->body, ftarget, out);
                out << ")) { ok_ = false; break; }\n";
            }
            if (field->constraint) {
                out << ind(indent + 1) << "if (!(" << compile_expr(*field->constraint) << ")) { ok_ = false; break; }\n";
            }
        }
        out << ind(indent + 1) << "if (ok_) return true;\n";
        out << ind(indent) << "} while (false);\n";
    } else {
        // Simple types: use single-expression form
        out << ind(indent) << "if (";
        emit_try_parse(type, target, out);
        out << ")\n";
        out << ind(indent + 1) << "return true;\n";
    }
}

} // namespace bbqgen
