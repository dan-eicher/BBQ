#include "CReaderEmitter.h"
#include "FrameProcessor.h"
#include <cinttypes>
#include <climits>

using namespace BBQ;
using bbqgen::FrameProcessor;

namespace bbqgen_c {

// ── Helpers ────────────────────────────────────────────────

std::string CReaderEmitter::ind(int n) {
    return std::string(n * 4, ' ');
}

// ── Public: declarations ───────────────────────────────────

std::string CReaderEmitter::emit_decls(Grammar* grammar) {
    std::ostringstream out;
    for (auto* rule : sema_.sorted_rules())
        emit_decl(rule, out);
    return out.str();
}

std::string CReaderEmitter::emit_impls(Grammar* grammar) {
    std::ostringstream out;
    for (auto* rule : sema_.sorted_rules()) {
        emit_impl(rule, out);
        out << "\n";
    }
    return out.str();
}

// ── Frame-based emission ───────────────────────────────────

bool CReaderEmitter::emit_header_to_frame(Grammar* grammar,
                                            const std::string& frame_path,
                                            const std::string& types_include,
                                            std::ostream& out) {
    FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    fp.CopyFramePart("types_include");
    fp.EmitLine("#include \"" + types_include + "\"");

    fp.CopyFramePart("read_declarations");
    {
        std::ostringstream decls;
        for (auto* rule : sema_.sorted_rules())
            emit_decl(rule, decls);
        fp.Emit(decls.str());
    }

    fp.CopyFramePart("");
    return true;
}

bool CReaderEmitter::emit_impl_to_frame(Grammar* grammar,
                                          const std::string& frame_path,
                                          const std::string& reader_include,
                                          std::ostream& out) {
    FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    fp.CopyFramePart("reader_include");
    fp.EmitLine("#include \"" + reader_include + "\"");

    fp.CopyFramePart("read_implementations");
    {
        std::ostringstream impls;
        for (auto* rule : sema_.sorted_rules()) {
            emit_impl(rule, impls);
            impls << "\n";
        }
        fp.Emit(impls.str());
    }

    fp.CopyFramePart("");
    return true;
}

// ── Declaration ────────────────────────────────────────────

void CReaderEmitter::emit_decl(Rule* rule, std::ostream& out) {
    std::string tname = types_.type_name(rule->name);
    std::string fname = types_.read_func(rule->name);

    if (dynamic_cast<Struct*>(rule->body) ||
        dynamic_cast<Union*>(rule->body) ||
        dynamic_cast<Alternatives*>(rule->body) ||
        dynamic_cast<Array*>(rule->body) ||
        dynamic_cast<Optional*>(rule->body) ||
        dynamic_cast<Bitfield*>(rule->body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << tname << "* out);\n";
    } else if (auto* sw = dynamic_cast<Switch*>(rule->body)) {
        std::string disc_type = has_string_cases(sw) ? "const char*" : "int64_t";
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << tname << "* out, "
            << disc_type << " discriminator);\n";
    } else if (auto* prim = dynamic_cast<Primitive*>(rule->body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, "
            << types_.primitive_c_type(prim->kind) << "* out);\n";
    } else if (auto* rr = dynamic_cast<RuleRef*>(rule->body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, "
            << types_.type_name(rr->name) << "* out);\n";
    } else if (auto* ext = dynamic_cast<Extern*>(rule->body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << ext->cpp_type << "* out);\n";
    }
}

// ── Implementation ─────────────────────────────────────────

void CReaderEmitter::emit_impl(Rule* rule, std::ostream& out) {
    auto* body = rule->body;
    current_rule_name_ = rule->name;
    std::string tname = types_.type_name(rule->name);
    std::string fname = types_.read_func(rule->name);

    if (auto* st = dynamic_cast<Struct*>(body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << tname << "* out) {\n";
        scopes_.clear();
        CEmitScope top_scope;
        top_scope.prefix = "out->";
        for (auto* f : st->fields)
            if (!dynamic_cast<EndianSwitch*>(f->body))
                top_scope.fields.insert(f->name);
        scopes_.push_back(std::move(top_scope));
        emit_struct_body(rule->name, st, out, 1);
        scopes_.clear();
        out << ind(1) << "return true;\n";
        out << "}\n";

    } else if (auto* u = dynamic_cast<Union*>(body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << tname << "* out) {\n";
        emit_union_body(rule->name, u, "out", out, 1);
        out << "}\n";

    } else if (auto* sw = dynamic_cast<Switch*>(body)) {
        std::string disc_type = has_string_cases(sw) ? "const char*" : "int64_t";
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << tname << "* out, "
            << disc_type << " discriminator) {\n";
        emit_switch_body(rule->name, sw, "discriminator", "out", out, 1);
        out << "}\n";

    } else if (auto* alts = dynamic_cast<Alternatives*>(body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << tname << "* out) {\n";
        emit_alternatives_body(rule->name, alts, "out", out, 1);
        out << "}\n";

    } else if (auto* arr = dynamic_cast<Array*>(body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << tname << "* out) {\n";
        emit_array_body(rule->name, arr, "out", out, 1);
        out << "}\n";

    } else if (auto* opt = dynamic_cast<Optional*>(body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << tname << "* out) {\n";
        emit_optional_body(rule->name, opt, "out", out, 1);
        out << "}\n";

    } else if (auto* prim = dynamic_cast<Primitive*>(body)) {
        std::string ptype = types_.primitive_c_type(prim->kind);
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << ptype << "* out) {\n";
        out << ind(1) << "if (!" << read_func_name(prim->kind) << "(ctx, out))\n";
        out << ind(2) << "return bbq_fail(ctx, \"" << rule->name << ": read failed\");\n";
        if (prim->constraint) {
            out << ind(1) << "if (!(" << compile_expr(*prim->constraint) << "))\n";
            out << ind(2) << "return bbq_fail(ctx, \"" << rule->name << ": constraint failed\");\n";
        }
        out << ind(1) << "return true;\n";
        out << "}\n";

    } else if (auto* rr = dynamic_cast<RuleRef*>(body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, "
            << types_.type_name(rr->name) << "* out) {\n";
        out << ind(1) << "return " << types_.read_func(rr->name) << "(ctx, out);\n";
        out << "}\n";

    } else if (auto* ext = dynamic_cast<Extern*>(body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << ext->cpp_type << "* out) {\n";
        out << ind(1) << "if (!" << ext->func_name << "(ctx, out))\n";
        out << ind(2) << "return bbq_fail(ctx, \"" << rule->name << ": read failed\");\n";
        if (ext->constraint) {
            out << ind(1) << "if (!(" << compile_expr(*ext->constraint) << "))\n";
            out << ind(2) << "return bbq_fail(ctx, \"" << rule->name << ": constraint failed\");\n";
        }
        out << ind(1) << "return true;\n";
        out << "}\n";

    } else if (auto* bf = dynamic_cast<Bitfield*>(body)) {
        out << "bool " << fname << "(bbq_ctx_t* ctx, " << tname << "* out) {\n";
        emit_bitfield_read(rule->name, bf, "(*out)", out, 1);
        out << ind(1) << "return true;\n";
        out << "}\n";
    }
}

// ── Struct body ────────────────────────────────────────────

void CReaderEmitter::emit_struct_body(const std::string& rule_name, Struct* st,
                                       std::ostream& out, int indent) {
    for (auto* field : st->fields)
        emit_field_read(rule_name, field, out, indent);
}

void CReaderEmitter::emit_field_read(const std::string& rule_name, Field* field,
                                      std::ostream& out, int indent) {
    if (auto* es = dynamic_cast<EndianSwitch*>(field->body)) {
        out << ind(indent) << "bbq_set_endian(ctx, " << compile_expr(es->endian_expr) << ");\n";
        return;
    }

    std::string prefix = scopes_.empty() ? "out->" : scopes_.back().prefix;
    std::string target = prefix + field->name;
    std::string ctx_name = rule_name + "." + field->name;

    // Interval handling
    Interval* interval_node = nullptr;
    if (field->interval.has_value()) {
        interval_node = *field->interval;
    } else if (auto* rr = dynamic_cast<RuleRef*>(field->body)) {
        if (rr->interval.has_value()) interval_node = *rr->interval;
    } else if (auto* ext = dynamic_cast<Extern*>(field->body)) {
        if (ext->interval.has_value()) interval_node = *ext->interval;
    }

    bool has_interval = false;
    if (interval_node) {
        if (auto* se = dynamic_cast<StartEnd*>(interval_node)) {
            has_interval = true;
            out << ind(indent) << "{\n";
            out << ind(indent + 1) << "bbq_push_interval(ctx, (size_t)("
                << compile_expr(se->end) << "));\n";
            indent++;
        }
    }

    if (auto* comp = dynamic_cast<Compute*>(field->body)) {
        out << ind(indent) << target << " = " << compile_expr(comp->expression) << ";\n";
    } else {
        emit_read_call(target, field->body, ctx_name, out, indent);
    }

    if (field->constraint) {
        out << ind(indent) << "if (!(" << compile_expr(*field->constraint) << "))\n";
        out << ind(indent + 1) << "return bbq_fail(ctx, \"" << ctx_name << ": constraint failed\");\n";
    }

    if (has_interval) {
        indent--;
        out << ind(indent + 1) << "bbq_pop_interval(ctx);\n";
        out << ind(indent) << "}\n";
    }
}

void CReaderEmitter::emit_read_call(const std::string& target, TypeExpr* type,
                                     const std::string& ctx, std::ostream& out, int indent) {
    if (auto* prim = dynamic_cast<Primitive*>(type)) {
        if (dynamic_cast<BytesKind*>(prim->kind)) {
            if (prim->interval) {
                if (auto* len = dynamic_cast<Length*>(*prim->interval)) {
                    out << ind(indent) << "if (!bbq_read_bytes(ctx, &" << target << ".data, &"
                        << target << ".length, (size_t)(" << compile_expr(len->length) << ")))\n";
                    out << ind(indent + 1) << "return bbq_fail(ctx, \"" << ctx << ": read bytes failed\");\n";
                    return;
                }
            }
            out << ind(indent) << "if (!bbq_read_bytes(ctx, &" << target << ".data, &"
                << target << ".length, bbq_remaining(ctx)))\n";
            out << ind(indent + 1) << "return bbq_fail(ctx, \"" << ctx << ": read bytes failed\");\n";
            return;
        }
        if (dynamic_cast<StringKind*>(prim->kind)) {
            if (prim->interval) {
                if (auto* len = dynamic_cast<Length*>(*prim->interval)) {
                    out << ind(indent) << "if (!bbq_read_string(ctx, &" << target << ".data, &"
                        << target << ".length, (size_t)(" << compile_expr(len->length) << ")))\n";
                    out << ind(indent + 1) << "return bbq_fail(ctx, \"" << ctx << ": read string failed\");\n";
                    return;
                }
            }
            out << ind(indent) << "if (!bbq_read_string(ctx, &" << target << ".data, &"
                << target << ".length, bbq_remaining(ctx)))\n";
            out << ind(indent + 1) << "return bbq_fail(ctx, \"" << ctx << ": read string failed\");\n";
            return;
        }
        out << ind(indent) << "if (!" << read_func_name(prim->kind) << "(ctx, &" << target << "))\n";
        out << ind(indent + 1) << "return bbq_fail(ctx, \"" << ctx << ": read failed\");\n";
        if (prim->constraint) {
            out << ind(indent) << "if (!(" << compile_expr(*prim->constraint) << "))\n";
            out << ind(indent + 1) << "return bbq_fail(ctx, \"" << ctx << ": constraint failed\");\n";
        }
    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        bool has_parent = !scopes_.empty();
        if (has_parent) {
            parent_contexts_.push_back({types_.type_name(current_rule_name_), scopes_.front().fields});
            out << ind(indent) << "{ bbq_push_scope(ctx, out);\n";
            indent++;
        }
        auto* target_rule = sema_.lookup_rule(rr->name);
        if (target_rule && dynamic_cast<Switch*>(target_rule->body)) {
            auto* sw = dynamic_cast<Switch*>(target_rule->body);
            std::string disc = compile_expr(sw->discriminator);
            out << ind(indent) << "if (!" << types_.read_func(rr->name) << "(ctx, &" << target << ", "
                << disc << "))\n";
        } else {
            out << ind(indent) << "if (!" << types_.read_func(rr->name) << "(ctx, &" << target << "))\n";
        }
        out << ind(indent + 1) << "return bbq_fail(ctx, \"" << ctx << "\");\n";
        if (has_parent) {
            indent--;
            out << ind(indent + 1) << "bbq_pop_scope(ctx);\n";
            out << ind(indent) << "}\n";
            parent_contexts_.pop_back();
        }
    } else if (auto* arr = dynamic_cast<Array*>(type)) {
        emit_array_body(ctx, arr, target, out, indent);
    } else if (auto* opt = dynamic_cast<Optional*>(type)) {
        emit_optional_body(ctx, opt, target, out, indent);
    } else if (auto* ext = dynamic_cast<Extern*>(type)) {
        out << ind(indent) << "if (!" << ext->func_name << "(ctx, &" << target << "))\n";
        out << ind(indent + 1) << "return bbq_fail(ctx, \"" << ctx << ": read failed\");\n";
    } else if (auto* sw = dynamic_cast<Switch*>(type)) {
        emit_switch_body(ctx, sw, compile_expr(sw->discriminator), "(&" + target + ")", out, indent, /*is_inline=*/true);
    } else if (auto* nested_st = dynamic_cast<Struct*>(type)) {
        CEmitScope nested_scope;
        nested_scope.prefix = target + ".";
        for (auto* f : nested_st->fields)
            if (!dynamic_cast<EndianSwitch*>(f->body))
                nested_scope.fields.insert(f->name);
        scopes_.push_back(std::move(nested_scope));
        emit_struct_body(ctx, nested_st, out, indent);
        scopes_.pop_back();
    } else if (auto* bf = dynamic_cast<Bitfield*>(type)) {
        emit_bitfield_read(ctx, bf, target, out, indent);
    }
}

// ── Union body ─────────────────────────────────────────────

void CReaderEmitter::emit_union_body(const std::string& rule_name, Union* u,
                                      const std::string& target, std::ostream& out, int indent) {
    for (size_t i = 0; i < u->variants.size(); i++) {
        std::string vname = u->variants[i]->name.empty()
            ? "alt_" + std::to_string(i) : u->variants[i]->name;
        out << ind(indent) << "{\n";
        out << ind(indent + 1) << "bbq_checkpoint_t saved = bbq_save(ctx);\n";
        out << ind(indent + 1) << target << "->tag = " << i << ";\n";
        emit_try_parse_block(u->variants[i]->body, target + "->u." + vname,
                             rule_name, out, indent + 1);
        out << ind(indent + 1) << "bbq_restore(ctx, saved);\n";
        out << ind(indent) << "}\n";
    }
    out << ind(indent) << "return bbq_fail(ctx, \"" << rule_name << ": no variant matched\");\n";
}

// ── Switch body ────────────────────────────────────────────

bool CReaderEmitter::has_string_cases(Switch* sw) {
    for (auto* c : sw->cases)
        if (dynamic_cast<StrValue*>(c->value)) return true;
    return false;
}

void CReaderEmitter::emit_switch_body(const std::string& rule_name, Switch* sw,
                                       const std::string& disc_expr, const std::string& target,
                                       std::ostream& out, int indent, bool is_inline) {
    // When inline (switch is a field inside a struct), use break instead of
    // return so that subsequent struct fields are parsed after the switch.
    const char* exit_stmt = is_inline ? "break" : "return true";
    if (has_string_cases(sw)) {
        bool first = true;
        size_t variant_idx = 0;
        for (auto* c : sw->cases) {
            auto* sv = dynamic_cast<StrValue*>(c->value);
            if (!sv) { variant_idx++; continue; }
            out << ind(indent);
            if (!first) out << "} else ";
            out << "if (strcmp(" << disc_expr << ", \"" << sv->value << "\") == 0) {\n";
            out << ind(indent + 1) << target << "->tag = " << variant_idx << ";\n";
            std::string case_name = "case_" + std::to_string(variant_idx);
            emit_read_call(target + "->u." + case_name, c->target, rule_name, out, indent + 1);
            out << ind(indent + 1) << exit_stmt << ";\n";
            first = false;
            variant_idx++;
        }
        if (sw->default_) {
            out << ind(indent) << "} else {\n";
            out << ind(indent + 1) << target << "->tag = " << variant_idx << ";\n";
            emit_read_call(target + "->u.default_val", (*sw->default_)->target, rule_name, out, indent + 1);
            out << ind(indent + 1) << exit_stmt << ";\n";
            out << ind(indent) << "}\n";
        } else {
            out << ind(indent) << "}\n";
            out << ind(indent) << "return bbq_fail(ctx, \"" << rule_name << ": unhandled case\");\n";
        }
        return;
    }

    // Integer switch — tag stores the actual discriminator value, not ordinal
    out << ind(indent) << "switch ((int64_t)(" << disc_expr << ")) {\n";
    size_t variant_idx = 0;
    for (auto* c : sw->cases) {
        std::string case_value_expr;
        if (auto* iv = dynamic_cast<IntValue*>(c->value)) {
            out << ind(indent) << "case " << iv->value << ": {\n";
            case_value_expr = std::to_string(iv->value);
        } else if (auto* idv = dynamic_cast<IdentValue*>(c->value)) {
            out << ind(indent) << "case " << idv->name << ": {\n";
            case_value_expr = idv->name;
        } else if (auto* rv = dynamic_cast<RefValue*>(c->value)) {
            std::string path;
            for (size_t j = 0; j < rv->path.size(); j++) {
                if (j > 0) path += "_";
                path += rv->path[j];
            }
            out << ind(indent) << "case " << path << ": {\n";
            case_value_expr = path;
        }
        std::string case_name = "case_" + std::to_string(variant_idx);
        out << ind(indent + 1) << target << "->tag = " << case_value_expr << ";\n";
        emit_read_call(target + "->u." + case_name, c->target, rule_name, out, indent + 1);
        out << ind(indent + 1) << exit_stmt << ";\n";
        out << ind(indent) << "}\n";
        variant_idx++;
    }
    if (sw->default_) {
        out << ind(indent) << "default: {\n";
        out << ind(indent + 1) << target << "->tag = -1;\n";
        emit_read_call(target + "->u.default_val", (*sw->default_)->target, rule_name, out, indent + 1);
        out << ind(indent + 1) << exit_stmt << ";\n";
        out << ind(indent) << "}\n";
    }
    if (!sw->default_) {
        out << ind(indent) << "default:\n";
        out << ind(indent + 1) << "return bbq_fail(ctx, \"" << rule_name << ": unhandled case\");\n";
    }
    out << ind(indent) << "}\n";
}

// ── Alternatives body ──────────────────────────────────────

void CReaderEmitter::emit_alternatives_body(const std::string& rule_name, Alternatives* alts,
                                              const std::string& target, std::ostream& out, int indent) {
    for (size_t i = 0; i < alts->alts.size(); i++) {
        std::string alt_name = "alt_" + std::to_string(i);
        out << ind(indent) << "{\n";
        out << ind(indent + 1) << "bbq_checkpoint_t saved = bbq_save(ctx);\n";
        out << ind(indent + 1) << target << "->tag = " << i << ";\n";
        emit_try_parse_block(alts->alts[i], target + "->u." + alt_name,
                             rule_name, out, indent + 1);
        out << ind(indent + 1) << "bbq_restore(ctx, saved);\n";
        out << ind(indent) << "}\n";
    }
    out << ind(indent) << "return bbq_fail(ctx, \"" << rule_name << ": no alternative matched\");\n";
}

// ── Array body ─────────────────────────────────────────────

void CReaderEmitter::emit_array_body(const std::string& rule_name, Array* arr,
                                      const std::string& target, std::ostream& out, int indent) {
    std::string elem_type = c_type(arr->element);

    if (auto* fc = dynamic_cast<FixedCount*>(arr->spec)) {
        std::string count_expr = compile_expr(fc->count);
        out << ind(indent) << "{ bbq_push_loop(ctx);\n";
        out << ind(indent + 1) << "size_t count = (size_t)(" << count_expr << ");\n";
        out << ind(indent + 1) << target << ".count = count;\n";
        out << ind(indent + 1) << target << ".items = (" << elem_type << "*)calloc(count, sizeof("
            << elem_type << "));\n";
        out << ind(indent + 1) << "for (size_t i = 0; i < count; i++) {\n";
        out << ind(indent + 2) << "bbq_set_loop_index(ctx, (int64_t)i);\n";
        array_nesting_depth_++;
        emit_read_call(target + ".items[i]", arr->element, rule_name + "[]", out, indent + 2);
        array_nesting_depth_--;
        out << ind(indent + 1) << "}\n";
        out << ind(indent + 1) << "bbq_pop_loop(ctx);\n";
        out << ind(indent) << "}\n";

    } else if (auto* st = dynamic_cast<SepTerm*>(arr->spec)) {
        out << ind(indent) << target << ".count = 0;\n";
        out << ind(indent) << target << ".items = NULL;\n";

        if (dynamic_cast<EofTerm*>(st->term)) {
            out << ind(indent) << "{ bbq_push_loop(ctx);\n";
            out << ind(indent + 1) << "size_t i = 0;\n";
            out << ind(indent + 1) << "while (!bbq_at_end(ctx)) {\n";
            out << ind(indent + 2) << "bbq_set_loop_index(ctx, (int64_t)i);\n";
            out << ind(indent + 2) << target << ".count++;\n";
            out << ind(indent + 2) << target << ".items = (" << elem_type << "*)realloc("
                << target << ".items, " << target << ".count * sizeof(" << elem_type << "));\n";
            out << ind(indent + 2) << "memset(&" << target << ".items[i], 0, sizeof(" << elem_type << "));\n";
            array_nesting_depth_++;
            emit_read_call(target + ".items[i]", arr->element, rule_name + "[]", out, indent + 2);
            array_nesting_depth_--;
            out << ind(indent + 2) << "i++;\n";
            out << ind(indent + 1) << "}\n";
            out << ind(indent + 1) << "bbq_pop_loop(ctx);\n";
            out << ind(indent) << "}\n";

        } else if (auto* ct = dynamic_cast<CountTerm*>(st->term)) {
            std::string count_expr = compile_expr(ct->count);
            out << ind(indent) << "{ bbq_push_loop(ctx);\n";
            out << ind(indent + 1) << "size_t count = (size_t)(" << count_expr << ");\n";
            out << ind(indent + 1) << target << ".count = count;\n";
            out << ind(indent + 1) << target << ".items = (" << elem_type << "*)calloc(count, sizeof("
                << elem_type << "));\n";
            out << ind(indent + 1) << "for (size_t i = 0; i < count; i++) {\n";
            out << ind(indent + 2) << "bbq_set_loop_index(ctx, (int64_t)i);\n";
            array_nesting_depth_++;
            emit_read_call(target + ".items[i]", arr->element, rule_name + "[]", out, indent + 2);
            array_nesting_depth_--;
            out << ind(indent + 1) << "}\n";
            out << ind(indent + 1) << "bbq_pop_loop(ctx);\n";
            out << ind(indent) << "}\n";

        } else if (auto* ut = dynamic_cast<UntilTerm*>(st->term)) {
            std::string cond = compile_expr(ut->condition);
            out << ind(indent) << "{ bbq_push_loop(ctx);\n";
            out << ind(indent + 1) << "size_t i = 0;\n";
            out << ind(indent + 1) << "while (!(" << cond << ")) {\n";
            out << ind(indent + 2) << "bbq_set_loop_index(ctx, (int64_t)i);\n";
            out << ind(indent + 2) << target << ".count++;\n";
            out << ind(indent + 2) << target << ".items = (" << elem_type << "*)realloc("
                << target << ".items, " << target << ".count * sizeof(" << elem_type << "));\n";
            out << ind(indent + 2) << "memset(&" << target << ".items[i], 0, sizeof(" << elem_type << "));\n";
            array_nesting_depth_++;
            emit_read_call(target + ".items[i]", arr->element, rule_name + "[]", out, indent + 2);
            array_nesting_depth_--;
            out << ind(indent + 2) << "i++;\n";
            out << ind(indent + 1) << "}\n";
            out << ind(indent + 1) << "bbq_pop_loop(ctx);\n";
            out << ind(indent) << "}\n";
        }
    }
}

// ── Optional body ──────────────────────────────────────────

void CReaderEmitter::emit_optional_body(const std::string& rule_name, Optional* opt,
                                          const std::string& target, std::ostream& out, int indent) {
    if (opt->constraint) {
        out << ind(indent) << "if (" << compile_expr(*opt->constraint) << ") {\n";
        out << ind(indent + 1) << target << ".has_value = true;\n";
        emit_read_call(target + ".value", opt->element, rule_name, out, indent + 1);
        out << ind(indent) << "} else {\n";
        out << ind(indent + 1) << target << ".has_value = false;\n";
        out << ind(indent) << "}\n";
    } else {
        out << ind(indent) << "{\n";
        out << ind(indent + 1) << "bbq_checkpoint_t saved = bbq_save(ctx);\n";
        out << ind(indent + 1) << target << ".has_value = true;\n";
        out << ind(indent + 1) << "if (!(";
        emit_try_parse(opt->element, target + ".value", out);
        out << ")) {\n";
        out << ind(indent + 2) << "bbq_restore(ctx, saved);\n";
        out << ind(indent + 2) << target << ".has_value = false;\n";
        out << ind(indent + 1) << "}\n";
        out << ind(indent) << "}\n";
    }
}

// ── Bitfield read ──────────────────────────────────────────

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

void CReaderEmitter::emit_bitfield_read(const std::string& ctx_name, Bitfield* bf,
                                          const std::string& target, std::ostream& out, int indent) {
    auto* ik = dynamic_cast<IntegerKind*>(bf->container);
    std::string container_type = types_.primitive_c_type(bf->container);
    std::string method = read_func_name(bf->container);

    out << ind(indent) << "{\n";
    out << ind(indent + 1) << container_type << " _bf;\n";
    out << ind(indent + 1) << "if (!" << method << "(ctx, &_bf))\n";
    out << ind(indent + 2) << "return bbq_fail(ctx, \"" << ctx_name << ": read failed\");\n";

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
            out << ind(indent + 1) << target << "." << entry->name
                << " = (" << entry_type << ")(";
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
            out << ind(indent + 1) << target << "." << entry->name
                << " = (" << entry_type << ")(";
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

// ── Expression compilation (C output) ──────────────────────

std::string CReaderEmitter::compile_expr(Expr* expr) {
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
            case Unaryop::Abs: return "bbq_abs(" + compile_expr(un->operand) + ")";
        }
    } else if (auto* tern = dynamic_cast<Ternary*>(expr)) {
        return "(" + compile_expr(tern->cond) + " ? "
             + compile_expr(tern->then_branch) + " : "
             + compile_expr(tern->else_branch) + ")";
    } else if (auto* il = dynamic_cast<IntLit*>(expr)) {
        if (il->value > INT32_MAX) {
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
        if (call->func == "peek") {
            return "({ uint8_t _p; bbq_peek(ctx, &_p) ? _p : 0; })";
        }
        std::string result = "bbq_" + call->func + "(";
        for (size_t i = 0; i < call->args.size(); i++) {
            if (i > 0) result += ", ";
            result += compile_expr(call->args[i]);
        }
        result += ")";
        return result;
    } else if (auto* el = dynamic_cast<EndianLit*>(expr)) {
        return (el->value == Endianness::Little) ? "true" : "false";
    } else if (dynamic_cast<EOI*>(expr)) {
        return "bbq_total_size(ctx)";
    }
    return "/* unknown expr */";
}

std::string CReaderEmitter::compile_ref_path(RefPath* path) {
    if (auto* simple = dynamic_cast<Simple*>(path)) {
        if (simple->name == "remaining") return "bbq_remaining(ctx)";
        if (simple->name == "pos") return "bbq_pos(ctx)";
        if (simple->name == "at_end") return "bbq_at_end(ctx)";
        if (simple->name == "i") {
            return (array_nesting_depth_ > 0) ? "i" : "bbq_loop_index(ctx)";
        }
        // Compile-time scope chain
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->fields.count(simple->name))
                return it->prefix + simple->name;
        }
        // Sema-resolved cross-rule reference (field lives in parent struct)
        if (simple->resolved_path.has_value() && simple->resolved_parent.has_value()) {
            return "((" + types_.type_name(*simple->resolved_parent)
                 + "*)bbq_scope_ptr(ctx, 0))->" + *simple->resolved_path;
        }
        // Parent context stack (runtime)
        for (int idx = (int)parent_contexts_.size() - 1; idx >= 0; idx--) {
            if (parent_contexts_[idx].fields.count(simple->name)) {
                int levels_up = (int)parent_contexts_.size() - 1 - idx;
                return "((" + parent_contexts_[idx].type_name
                     + "*)bbq_scope_ptr(ctx, " + std::to_string(levels_up)
                     + "))->" + simple->name;
            }
        }
        return simple->name;
    } else if (auto* fa = dynamic_cast<FieldAcc*>(path)) {
        return compile_ref_path(fa->base) + "." + fa->field;
    } else if (auto* ia = dynamic_cast<IndexAcc*>(path)) {
        return compile_ref_path(ia->base) + "[" + compile_expr(ia->index) + "]";
    }
    return "/* unknown path */";
}

// ── Read function name ─────────────────────────────────────

std::string CReaderEmitter::read_func_name(PrimitiveKind* kind) {
    if (auto* ik = dynamic_cast<IntegerKind*>(kind)) {
        std::string base = (ik->sign == Signedness::Unsigned) ? "bbq_read_u" : "bbq_read_i";
        std::string width;
        switch (ik->width) {
            case Width::W8:  width = "8"; break;
            case Width::W16: width = "16"; break;
            case Width::W32: width = "32"; break;
            case Width::W64: width = "64"; break;
        }
        if (ik->width == Width::W8) return base + width;
        Endianness e = ik->endian;
        if (e == Endianness::Default) e = sema_.default_endian();
        std::string suffix = (e == Endianness::Big) ? "be" : "le";
        return base + width + suffix;
    } else if (auto* fk = dynamic_cast<FloatKind*>(kind)) {
        std::string width = (fk->width == Width::W64) ? "64" : "32";
        Endianness e = fk->endian;
        if (e == Endianness::Default) e = sema_.default_endian();
        std::string suffix = (e == Endianness::Big) ? "be" : "le";
        return "bbq_read_f" + width + suffix;
    } else if (dynamic_cast<BoolKind*>(kind)) {
        return "bbq_read_bool";
    }
    return "bbq_read_u8";
}

// ── Try-parse helpers ──────────────────────────────────────

void CReaderEmitter::emit_try_parse(TypeExpr* type, const std::string& target, std::ostream& out) {
    if (auto* prim = dynamic_cast<Primitive*>(type)) {
        out << read_func_name(prim->kind) << "(ctx, &" << target << ")";
    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        out << types_.read_func(rr->name) << "(ctx, &" << target << ")";
    } else if (auto* ext = dynamic_cast<Extern*>(type)) {
        out << ext->func_name << "(ctx, &" << target << ")";
    } else {
        out << "false /* complex inline type */";
    }
}

void CReaderEmitter::emit_try_parse_block(TypeExpr* type, const std::string& target,
                                            const std::string& rule_name, std::ostream& out, int indent) {
    if (auto* st = dynamic_cast<Struct*>(type)) {
        out << ind(indent) << "do {\n";
        out << ind(indent + 1) << "bool ok_ = true;\n";
        for (auto* field : st->fields) {
            if (auto* comp = dynamic_cast<Compute*>(field->body)) {
                out << ind(indent + 1) << target << "." << field->name
                    << " = " << compile_expr(comp->expression) << ";\n";
            } else {
                std::string ftarget = target + "." + field->name;
                out << ind(indent + 1) << "if (!(";
                emit_try_parse(field->body, ftarget, out);
                out << ")) { ok_ = false; break; }\n";
            }
            if (field->constraint) {
                out << ind(indent + 1) << "if (!(" << compile_expr(*field->constraint) << ")) { ok_ = false; break; }\n";
            }
        }
        out << ind(indent + 1) << "if (ok_) return true;\n";
        out << ind(indent) << "} while (0);\n";
    } else {
        out << ind(indent) << "if (";
        emit_try_parse(type, target, out);
        out << ")\n";
        out << ind(indent + 1) << "return true;\n";
    }
}

} // namespace bbqgen_c
