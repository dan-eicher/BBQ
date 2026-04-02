#include "CWriterEmitter.h"
#include "FrameProcessor.h"
#include <cinttypes>

using namespace BBQ;
using bbqgen::FrameProcessor;

namespace bbqgen_c {

std::string CWriterEmitter::ind(int n) {
    return std::string(n * 4, ' ');
}

// ── Public ─────────────────────────────────────────────────

std::string CWriterEmitter::emit_decls(Grammar* grammar) {
    std::ostringstream out;
    for (auto* rule : sema_.sorted_rules())
        emit_decl(rule, out);
    return out.str();
}

std::string CWriterEmitter::emit_impls(Grammar* grammar) {
    std::ostringstream out;
    for (auto* rule : sema_.sorted_rules()) {
        emit_impl(rule, out);
        out << "\n";
    }
    return out.str();
}

bool CWriterEmitter::emit_header_to_frame(Grammar* grammar,
                                            const std::string& frame_path,
                                            const std::string& types_include,
                                            std::ostream& out) {
    FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;
    fp.CopyFramePart("types_include");
    fp.EmitLine("#include \"" + types_include + "\"");
    fp.CopyFramePart("write_declarations");
    {
        std::ostringstream decls;
        for (auto* rule : sema_.sorted_rules())
            emit_decl(rule, decls);
        fp.Emit(decls.str());
    }
    fp.CopyFramePart("");
    return true;
}

bool CWriterEmitter::emit_impl_to_frame(Grammar* grammar,
                                          const std::string& frame_path,
                                          const std::string& writer_include,
                                          std::ostream& out) {
    FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;
    fp.CopyFramePart("writer_include");
    fp.EmitLine("#include \"" + writer_include + "\"");
    fp.CopyFramePart("write_implementations");
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

void CWriterEmitter::emit_decl(Rule* rule, std::ostream& out) {
    std::string tname = types_.type_name(rule->name);
    std::string fname = types_.write_func(rule->name);

    if (dynamic_cast<Struct*>(rule->body) ||
        dynamic_cast<Union*>(rule->body) ||
        dynamic_cast<Alternatives*>(rule->body) ||
        dynamic_cast<Array*>(rule->body) ||
        dynamic_cast<Optional*>(rule->body) ||
        dynamic_cast<Bitfield*>(rule->body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << tname << "* in);\n";
    } else if (dynamic_cast<Switch*>(rule->body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << tname << "* in);\n";
    } else if (auto* prim = dynamic_cast<Primitive*>(rule->body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, "
            << types_.primitive_c_type(prim->kind) << " in);\n";
    } else if (auto* rr = dynamic_cast<RuleRef*>(rule->body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const "
            << types_.type_name(rr->name) << "* in);\n";
    } else if (auto* ext = dynamic_cast<Extern*>(rule->body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << ext->cpp_type << "* in);\n";
    }
}

// ── Implementation ─────────────────────────────────────────

void CWriterEmitter::emit_impl(Rule* rule, std::ostream& out) {
    auto* body = rule->body;
    std::string tname = types_.type_name(rule->name);
    std::string fname = types_.write_func(rule->name);

    if (auto* st = dynamic_cast<Struct*>(body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << tname << "* in) {\n";
        emit_struct_write(rule->name, st, "in->", out, 1);
        out << ind(1) << "return true;\n";
        out << "}\n";

    } else if (auto* u = dynamic_cast<Union*>(body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << tname << "* in) {\n";
        out << ind(1) << "switch (in->tag) {\n";
        for (size_t i = 0; i < u->variants.size(); i++) {
            std::string vname = u->variants[i]->name.empty()
                ? "alt_" + std::to_string(i) : u->variants[i]->name;
            out << ind(1) << "case " << i << ":\n";
            emit_write_call("in->u." + vname, u->variants[i]->body, rule->name, out, 2);
            out << ind(2) << "return true;\n";
        }
        out << ind(1) << "}\n";
        out << ind(1) << "return false;\n";
        out << "}\n";

    } else if (auto* sw = dynamic_cast<Switch*>(body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << tname << "* in) {\n";
        out << ind(1) << "switch (in->tag) {\n";
        for (size_t i = 0; i < sw->cases.size(); i++) {
            std::string case_name = "case_" + std::to_string(i);
            if (auto* iv = dynamic_cast<IntValue*>(sw->cases[i]->value)) {
                out << ind(1) << "case " << iv->value << ":\n";
            } else if (auto* idv = dynamic_cast<IdentValue*>(sw->cases[i]->value)) {
                out << ind(1) << "case " << idv->name << ":\n";
            } else if (auto* rv = dynamic_cast<RefValue*>(sw->cases[i]->value)) {
                std::string path;
                for (size_t j = 0; j < rv->path.size(); j++) {
                    if (j > 0) path += "_";
                    path += rv->path[j];
                }
                out << ind(1) << "case " << path << ":\n";
            }
            emit_write_call("in->u." + case_name, sw->cases[i]->target, rule->name, out, 2);
            out << ind(2) << "return true;\n";
        }
        if (sw->default_) {
            out << ind(1) << "default:\n";
            emit_write_call("in->u.default_val", (*sw->default_)->target, rule->name, out, 2);
            out << ind(2) << "return true;\n";
        }
        out << ind(1) << "}\n";
        out << ind(1) << "return false;\n";
        out << "}\n";

    } else if (auto* alts = dynamic_cast<Alternatives*>(body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << tname << "* in) {\n";
        out << ind(1) << "switch (in->tag) {\n";
        for (size_t i = 0; i < alts->alts.size(); i++) {
            std::string alt_name = "alt_" + std::to_string(i);
            out << ind(1) << "case " << i << ":\n";
            emit_write_call("in->u." + alt_name, alts->alts[i], rule->name, out, 2);
            out << ind(2) << "return true;\n";
        }
        out << ind(1) << "}\n";
        out << ind(1) << "return false;\n";
        out << "}\n";

    } else if (auto* arr = dynamic_cast<Array*>(body)) {
        std::string elem_type = types_.c_type(arr->element);
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << tname << "* in) {\n";
        out << ind(1) << "for (size_t i = 0; i < in->count; i++) {\n";
        emit_write_call("in->items[i]", arr->element, rule->name + "[]", out, 2);
        out << ind(1) << "}\n";
        out << ind(1) << "return true;\n";
        out << "}\n";

    } else if (auto* opt = dynamic_cast<Optional*>(body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << tname << "* in) {\n";
        out << ind(1) << "if (in->has_value) {\n";
        emit_write_call("in->value", opt->element, rule->name, out, 2);
        out << ind(1) << "}\n";
        out << ind(1) << "return true;\n";
        out << "}\n";

    } else if (auto* prim = dynamic_cast<Primitive*>(body)) {
        std::string ptype = types_.primitive_c_type(prim->kind);
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, " << ptype << " in) {\n";
        out << ind(1) << "return " << write_func_name(prim->kind) << "(ctx, in);\n";
        out << "}\n";

    } else if (auto* rr = dynamic_cast<RuleRef*>(body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const "
            << types_.type_name(rr->name) << "* in) {\n";
        out << ind(1) << "return " << types_.write_func(rr->name) << "(ctx, in);\n";
        out << "}\n";

    } else if (auto* bf = dynamic_cast<Bitfield*>(body)) {
        out << "bool " << fname << "(bbq_write_ctx_t* ctx, const " << tname << "* in) {\n";
        emit_bitfield_write(rule->name, bf, "in->", out, 1);
        out << ind(1) << "return true;\n";
        out << "}\n";
    }
}

// ── Struct write ───────────────────────────────────────────

void CWriterEmitter::emit_struct_write(const std::string& rule_name, Struct* st,
                                        const std::string& src, std::ostream& out, int indent) {
    for (auto* field : st->fields)
        emit_field_write(rule_name, field, src, out, indent);
}

void CWriterEmitter::emit_field_write(const std::string& rule_name, Field* field,
                                       const std::string& src, std::ostream& out, int indent) {
    // Skip endian switches (reader-side only, writer uses same endianness)
    if (dynamic_cast<EndianSwitch*>(field->body)) {
        // If we need to track endianness for the writer too:
        // out << ind(indent) << "bbq_write_set_endian(ctx, ...);\n";
        return;
    }

    // Skip compute fields (derived values, not written)
    if (dynamic_cast<Compute*>(field->body)) return;

    std::string val = src + field->name;
    emit_write_call(val, field->body, rule_name + "." + field->name, out, indent);
}

void CWriterEmitter::emit_write_call(const std::string& src, TypeExpr* type,
                                      const std::string& ctx, std::ostream& out, int indent) {
    if (auto* prim = dynamic_cast<Primitive*>(type)) {
        if (dynamic_cast<BytesKind*>(prim->kind)) {
            out << ind(indent) << "if (!bbq_write_bytes(ctx, " << src << ".data, " << src << ".length))\n";
            out << ind(indent + 1) << "return false;\n";
            return;
        }
        if (dynamic_cast<StringKind*>(prim->kind)) {
            out << ind(indent) << "if (!bbq_write_string(ctx, " << src << ".data, " << src << ".length))\n";
            out << ind(indent + 1) << "return false;\n";
            return;
        }
        out << ind(indent) << "if (!" << write_func_name(prim->kind) << "(ctx, " << src << "))\n";
        out << ind(indent + 1) << "return false;\n";

    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        out << ind(indent) << "if (!" << types_.write_func(rr->name) << "(ctx, &" << src << "))\n";
        out << ind(indent + 1) << "return false;\n";

    } else if (auto* arr = dynamic_cast<Array*>(type)) {
        out << ind(indent) << "for (size_t i = 0; i < " << src << ".count; i++) {\n";
        emit_write_call(src + ".items[i]", arr->element, ctx + "[]", out, indent + 1);
        out << ind(indent) << "}\n";

    } else if (auto* opt = dynamic_cast<Optional*>(type)) {
        out << ind(indent) << "if (" << src << ".has_value) {\n";
        emit_write_call(src + ".value", opt->element, ctx, out, indent + 1);
        out << ind(indent) << "}\n";

    } else if (auto* nested_st = dynamic_cast<Struct*>(type)) {
        emit_struct_write(ctx, nested_st, src + ".", out, indent);

    } else if (auto* bf = dynamic_cast<Bitfield*>(type)) {
        emit_bitfield_write(ctx, bf, src + ".", out, indent);
    }
}

// ── Bitfield write ─────────────────────────────────────────

static int width_to_bits_w(Width w) {
    switch (w) {
        case Width::W8:  return 8;
        case Width::W16: return 16;
        case Width::W32: return 32;
        case Width::W64: return 64;
    }
    return 0;
}

void CWriterEmitter::emit_bitfield_write(const std::string& ctx_name, Bitfield* bf,
                                           const std::string& src, std::ostream& out, int indent) {
    auto* ik = dynamic_cast<IntegerKind*>(bf->container);
    std::string container_type = types_.primitive_c_type(bf->container);

    out << ind(indent) << "{\n";
    out << ind(indent + 1) << container_type << " _bf = 0;\n";

    Endianness e = ik->endian;
    if (e == Endianness::Default) e = sema_.default_endian();
    bool msb_first = (e == Endianness::Big);
    int container_bits = width_to_bits_w(ik->width);

    if (msb_first) {
        int bit_offset = container_bits;
        for (auto* entry : bf->entries) {
            bit_offset -= entry->width;
            uint64_t mask = (1ULL << entry->width) - 1;
            out << ind(indent + 1) << "_bf |= ((" << container_type << ")"
                << src << entry->name << " & 0x";
            char buf[32]; snprintf(buf, sizeof(buf), "%" PRIX64, mask);
            out << buf << ")";
            if (bit_offset > 0) out << " << " << bit_offset;
            out << ";\n";
        }
    } else {
        int bit_offset = 0;
        for (auto* entry : bf->entries) {
            uint64_t mask = (1ULL << entry->width) - 1;
            out << ind(indent + 1) << "_bf |= ((" << container_type << ")"
                << src << entry->name << " & 0x";
            char buf[32]; snprintf(buf, sizeof(buf), "%" PRIX64, mask);
            out << buf << ")";
            if (bit_offset > 0) out << " << " << bit_offset;
            out << ";\n";
            bit_offset += entry->width;
        }
    }

    out << ind(indent + 1) << "if (!" << write_func_name(bf->container) << "(ctx, _bf))\n";
    out << ind(indent + 2) << "return false;\n";
    out << ind(indent) << "}\n";
}

// ── Write function name ────────────────────────────────────

std::string CWriterEmitter::write_func_name(PrimitiveKind* kind) {
    if (auto* ik = dynamic_cast<IntegerKind*>(kind)) {
        std::string base = (ik->sign == Signedness::Unsigned) ? "bbq_write_u" : "bbq_write_i";
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
        return "bbq_write_f" + width + suffix;
    } else if (dynamic_cast<BoolKind*>(kind)) {
        return "bbq_write_bool";
    }
    return "bbq_write_u8";
}

} // namespace bbqgen_c
