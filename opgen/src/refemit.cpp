// The reference-kernel emitter. A kernel is the spec body reused verbatim
// through SemLowerer's Interp-mode lowering: for a CLOSED body every lowered
// name is a plain C identifier, so the same walk that fills a handler fills a
// pure function — one lowering, two consumers, no second rendering of the
// semantics to drift.
#include "refemit.h"
#include <cctype>
#include <cstring>
#include <vector>

namespace opgen {

template <class T>
static T* opt_ptr(const std::optional<T*>& o) { return o ? *o : nullptr; }

// ── Eligibility ─────────────────────────────────────────────
//
// CLOSED means: a pure function of the declared stack inputs and decoded
// operands. Anything that reaches VM state (frame slots, code, sp/pc/stp),
// calls out of the action language, or traps, is OPEN — the op keeps its
// handler and its corpus coverage, and the skip is REPORTED, never silent.

const char* RefEmitter::param_open_reason(const SemLowerer& low, ValueType ty) {
    switch (ty) {
    case ValueType::TyWord: case ValueType::TyAny: case ValueType::TyV128:
    case ValueType::TyI8x16: case ValueType::TyI16x8: case ValueType::TyI32x4:
    case ValueType::TyI64x2: case ValueType::TyF32x4: case ValueType::TyF64x2:
        return "slot-typed parameter (word/any/v128)";
    case ValueType::TyBrTable: case ValueType::TyTryTable: case ValueType::TySelectVec:
        return "operand without a scalar carrier";
    default: break;
    }
    if (low.jtype(ty).scalar[0] == 'f') return "floating-point type";
    return nullptr;
}

const char* RefEmitter::expr_open_reason(const SemLowerer& low, const SemExpr* e) {
    if (!e) return nullptr;
    switch (e->tag) {
    case SemExprTag::SIndex:
        return "accesses locals[]/globals[]/code[]/stack[]";
    case SemExprTag::SCall: {
        auto* call = static_cast<const SCall*>(e);
        if (!strcmp(call->name, "push") || !strcmp(call->name, "pop"))
            return "stack-effect intrinsic (push/pop)";
        if (!SemLowerer::is_intrinsic(call->name))
            return "calls a native";
        for (auto* a : call->args)
            if (const char* r = expr_open_reason(low, a)) return r;
        return nullptr;
    }
    case SemExprTag::SIdent: {
        const char* n = static_cast<const SIdent*>(e)->name;
        if (!strcmp(n, "sp") || !strcmp(n, "pc") || !strcmp(n, "stp") ||
            !strcmp(n, "lane"))
            return "reads VM state (sp/pc/stp/lane)";
        return nullptr;
    }
    case SemExprTag::SBinOp: {
        auto* b = static_cast<const SBinOp*>(e);
        if (const char* r = expr_open_reason(low, b->left)) return r;
        return expr_open_reason(low, b->right);
    }
    case SemExprTag::SUnary:
        return expr_open_reason(low, static_cast<const SUnary*>(e)->operand);
    case SemExprTag::STernary: {
        auto* t = static_cast<const STernary*>(e);
        if (const char* r = expr_open_reason(low, t->cond)) return r;
        if (const char* r = expr_open_reason(low, t->then_)) return r;
        return expr_open_reason(low, t->else_);
    }
    case SemExprTag::SCast:
        return expr_open_reason(low, static_cast<const SCast*>(e)->operand);
    case SemExprTag::SInt:
    case SemExprTag::SFloat:
        return nullptr;
    }
    return nullptr;
}

const char* RefEmitter::stmt_open_reason(const SemLowerer& low, const SemStmt* s) {
    if (!s) return nullptr;
    switch (s->tag) {
    case SemStmtTag::SAssign: {
        auto* a = static_cast<const SAssign*>(s);
        if (const char* r = expr_open_reason(low, a->target)) return r;
        return expr_open_reason(low, a->value);
    }
    case SemStmtTag::SExprStmt:
        return "statement-level built-in call";
    case SemStmtTag::SLocalDecl:
        return expr_open_reason(low, opt_ptr(static_cast<const SLocalDecl*>(s)->init));
    case SemStmtTag::SIf: {
        auto* i = static_cast<const SIf*>(s);
        if (const char* r = expr_open_reason(low, i->cond)) return r;
        if (const char* r = stmt_open_reason(low, i->then_)) return r;
        return stmt_open_reason(low, opt_ptr(i->else_));
    }
    case SemStmtTag::SWhile: {
        auto* w = static_cast<const SWhile*>(s);
        if (const char* r = expr_open_reason(low, w->cond)) return r;
        return stmt_open_reason(low, w->body);
    }
    case SemStmtTag::SFor: {
        auto* f = static_cast<const SFor*>(s);
        if (const char* r = stmt_open_reason(low, opt_ptr(f->init))) return r;
        if (const char* r = expr_open_reason(low, opt_ptr(f->cond))) return r;
        if (const char* r = stmt_open_reason(low, opt_ptr(f->update))) return r;
        return stmt_open_reason(low, f->body);
    }
    case SemStmtTag::SBlock: {
        for (auto* st : static_cast<const SBlock*>(s)->stmts)
            if (const char* r = stmt_open_reason(low, st)) return r;
        return nullptr;
    }
    case SemStmtTag::STrap:
        return "explicit trap";
    }
    return nullptr;
}

const char* RefEmitter::open_reason(const SemLowerer& low, const Opcode* op) {
    if (op->sem_body.empty()) return "no body";
    for (auto* p : op->stack_in) {
        if (p->count.has_value())  return "variadic stack parameter";
        if (p->at_src.has_value()) return "addrtype-sourced parameter";
        if (const char* r = param_open_reason(low, p->ty)) return r;
    }
    for (auto* p : op->stack_out) {
        if (const char* r = param_open_reason(low, p->ty)) return r;
    }
    for (auto* p : op->operands)
        if (const char* r = param_open_reason(low, p->ty)) return r;
    for (auto* e : op->errors)
        if (e->condition.has_value())
            if (const char* r = expr_open_reason(low, *e->condition)) return r;
    for (auto* s : op->sem_body)
        if (const char* r = stmt_open_reason(low, s)) return r;
    return nullptr;
}

// ── Emission ────────────────────────────────────────────────

// Is `name` already a kernel parameter (an input or an operand)? A pass-through
// out then needs no local — declaring one would shadow the parameter.
static bool is_param_name(const Opcode* op, const char* name) {
    for (auto* p : op->stack_in)  if (!strcmp(p->name, name)) return true;
    for (auto* p : op->operands)  if (!strcmp(p->name, name)) return true;
    return false;
}

void RefEmitter::emit_kernel(SemLowerer& low, const Opcode* op, FILE* o,
                             const std::string& up) {
    fprintf(o, "/* %s 0x%02X  (", op->mnemonic, (unsigned)op->opcode_val);
    for (size_t i = 0; i < op->stack_in.size(); i++)
        fprintf(o, "%s%s %s", i ? ", " : "",
                low.c_scalar(op->stack_in[i]->ty), op->stack_in[i]->name);
    fputs(" -- ", o);
    for (size_t i = 0; i < op->stack_out.size(); i++)
        fprintf(o, "%s%s %s", i ? ", " : "",
                low.c_scalar(op->stack_out[i]->ty), op->stack_out[i]->name);
    fputs(") */\n", o);

    fprintf(o, "static inline int %s_ref_%s(", prefix_.c_str(), op->mnemonic);
    int narg = 0;
    for (auto* p : op->stack_in)
        fprintf(o, "%s%s %s", narg++ ? ", " : "", low.c_scalar(p->ty), p->name);
    for (auto* p : op->operands)
        fprintf(o, "%s%s %s", narg++ ? ", " : "", low.c_scalar(p->ty), p->name);
    for (auto* p : op->stack_out)
        fprintf(o, "%s%s* out_%s", narg++ ? ", " : "", low.c_scalar(p->ty), p->name);
    if (!narg) fputs("void", o);
    fputs(") {\n", o);

    for (auto* p : op->stack_out)
        if (!is_param_name(op, p->name))
            fprintf(o, "    %s %s = 0;\n", low.c_scalar(p->ty), p->name);

    // Guards, in declaration order, exactly as the handler runs them — the
    // `(_Bool)` for the same clang parenthesised-equality reason emit_guards
    // states.
    for (auto* e : op->errors) {
        if (!e->condition.has_value()) continue;
        fputs("    if ((_Bool)", o);
        low.lower_expr(*e->condition, op, SemLowerer::cond_type(op, *e->condition));
        fprintf(o, ") return %s_REF_ERR_%s;\n", up.c_str(), e->error_name);
    }

    for (auto* s : op->sem_body)
        low.lower_stmt(op, s);

    for (auto* p : op->stack_out)
        fprintf(o, "    *out_%s = %s;\n", p->name, p->name);
    fprintf(o, "    return %s_REF_OK;\n}\n\n", up.c_str());
}

void RefEmitter::emit_ref_h(FILE* o) {
    SemLowerer low(mod_, o, SemLowerer::Mode::Interp, prefix_);
    std::string up;
    for (char c : prefix_) up.push_back((char)std::toupper((unsigned char)c));

    fprintf(o,
"/* AUTO-GENERATED by opgen — do not edit. Source: opcodes.def */\n"
"/*\n"
" * %s_ref.h — pure reference kernels, one per opcode with a CLOSED body.\n"
" * Each kernel is the spec's semantics as a standalone C function: declared\n"
" * stack inputs and decoded operands by value, outputs through pointers,\n"
" * `error:` guards as early returns. No VM state is touched — these are the\n"
" * oracle a differential or verification harness compares an implementation\n"
" * against.\n"
" *\n"
" * Compile consumers with -fwrapv: the generated interpreter is compiled\n"
" * that way, and the kernels state the same wrapping arithmetic.\n"
" *\n"
" * Return value: %s_REF_OK (-1) when no guard fired, else the fired\n"
" * guard's code — numerically EQUAL to the opgen_err_t code the generated\n"
" * runtime_api.h assigns the same error name.\n"
" */\n"
"#ifndef OPGEN_%s_REF_H\n"
"#define OPGEN_%s_REF_H\n"
"\n"
"#include <stdint.h>\n"
"\n"
"typedef int8_t  s1; typedef int16_t s2; typedef int32_t s4; typedef int64_t s8;\n"
"typedef uint8_t u1; typedef uint16_t u2; typedef uint32_t u4; typedef uint64_t u8;\n"
"typedef float   f4; typedef double  f8;\n",
            prefix_.c_str(), up.c_str(), up.c_str(), up.c_str());
    {
        char ic; const char* islot; const char* uw; int w;
        low.int_slot_view(&ic, &islot, &uw, &w);
        (void)ic; (void)islot; (void)w;
        fprintf(o, "typedef %s ref_t;\n", uw);
    }

    // Status codes: the SAME numbering runtime_api.h gives opgen_err_t —
    // every distinct `error:` name, first-declaration order, zero-based.
    fputs("\nenum {\n", o);
    fprintf(o, "    %s_REF_OK = -1", up.c_str());
    {
        std::vector<const char*> seen;
        for (auto* op : mod_->opcodes) {
            for (auto* e : op->errors) {
                if (!e->error_name) continue;
                bool dup = false;
                for (const char* s : seen) if (!strcmp(s, e->error_name)) { dup = true; break; }
                if (dup) continue;
                fprintf(o, ",\n    %s_REF_ERR_%s = %d", up.c_str(), e->error_name,
                        (int)seen.size());
                seen.push_back(e->error_name);
            }
        }
    }
    fputs("\n};\n\n", o);

    std::vector<const Opcode*> skipped;
    std::vector<const char*>   why;
    for (auto* op : mod_->opcodes) {
        if (const char* r = open_reason(low, op)) {
            skipped.push_back(op);
            why.push_back(r);
            continue;
        }
        emit_kernel(low, op, o, up);
    }

    if (!skipped.empty()) {
        fputs("/* Not kernel-eligible (no reference kernel emitted):\n", o);
        for (size_t i = 0; i < skipped.size(); i++)
            fprintf(o, " *   %s — %s\n", skipped[i]->mnemonic, why[i]);
        fputs(" */\n\n", o);
    }

    fprintf(o, "#endif /* OPGEN_%s_REF_H */\n", up.c_str());
}

} // namespace opgen
