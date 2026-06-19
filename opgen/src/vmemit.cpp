// The VM/JIT/validator emitters (gen_interp.c, wasm_stencils.c, the jit/type
// metadata + headers + runtime_api.h), ported from emit_interp_c.cpp. Every
// emitted byte matches the reference VM; g_mod/g_mode/g_lane are gone — the
// Module is a member, the mode/lane live in the SemLowerer.
#include "vmemit.h"
#include <cstring>
#include <cctype>

namespace opgen {

using Mode = SemLowerer::Mode;

// libm symbols the float intrinsics call; in a stencil these are indirect calls
// through _HOLE_<sym> fn-pointer holes the jit_driver fills.
static const char* g_libm_syms[] = {
    "sqrtf","sqrt","fabsf","fabs","ceilf","ceil","floorf","floor",
    "truncf","trunc","rintf","rint","copysignf","copysign", nullptr };

const char* VmEmitter::out_sem_expr(const StackParam* sp) {
    return sp->sem_expr.has_value() ? *sp->sem_expr : nullptr;
}

// ── Guard synthesis ─────────────────────────────────────────

void VmEmitter::emit_guards(SemLowerer& low, const Opcode* op) {
    FILE* o = low.out();
    if (SemLowerer::has_error(op, "DivByZero")) {
        const SemExpr* d = SemLowerer::body_divisor(op);
        if (d) {
            fputs("    if ((", o);
            low.lower_expr(d, op, ValueType::TyI32);
            fputs(") == 0)", o);
            low.guard_trap("throw_div_by_zero");
        }
    }
    if (SemLowerer::has_error(op, "Overflow")) {
        const SemExpr* div = SemLowerer::body_divide(op);
        if (div) {
            const SemExpr* num = static_cast<const SBinOp*>(div)->left;
            ValueType nt = (num->tag == SemExprTag::SIdent)
                ? low.type_of_name(op, static_cast<const SIdent*>(num)->name) : ValueType::TyI32;
            int W = SemLowerer::scalar_bits(low.c_scalar(nt));
            fputs("    if ((", o); low.lower_expr(static_cast<const SBinOp*>(div)->right, op, nt);
            fputs(") == -1 && (", o); low.lower_expr(num, op, nt);
            fprintf(o, ") == (%s)((%s)1 << %d))", low.c_scalar(nt), low.c_unsigned(nt), W - 1);
            low.guard_trap("throw_div_by_zero");
        }
    }
    const SemExpr* call = SemLowerer::body_call(op);
    if (SemLowerer::has_error(op, "NullPointer") && call) {
        auto* sc = static_cast<const SCall*>(call);
        const MethodDecl* md = low.find_method(sc->name);
        if (md && !md->params.empty() && !strcmp(md->params[0]->ty, "ref")
            && !sc->args.empty()) {
            fputs("    if ((", o);
            low.lower_expr(sc->args[0], op, ValueType::TyI32);
            fputs(") == 0) return throw_null_pointer(vm, vm->heap);\n", o);
        }
    }
    if (SemLowerer::has_error(op, "ArrayBounds") && call) {
        auto* sc = static_cast<const SCall*>(call);
        if (sc->args.size() >= 2 &&
            (!strncmp(sc->name, "array_load", 10) ||
             !strncmp(sc->name, "array_store", 11))) {
            fputs("    if ((", o);
            low.lower_expr(sc->args[1], op, ValueType::TyI32);
            fputs(") < 0 || (", o);
            low.lower_expr(sc->args[1], op, ValueType::TyI32);
            fputs(") >= array_length(vm, vm->heap, ", o);
            low.lower_expr(sc->args[0], op, ValueType::TyI32);
            fputs(")) return throw_array_bounds(vm, vm->heap);\n", o);
        }
    }
    if (SemLowerer::has_error(op, "NegativeSize") && call) {
        auto* sc = static_cast<const SCall*>(call);
        if (sc->args.size() >= 2 &&
            (!strncmp(sc->name, "new_array", 9) ||
             !strncmp(sc->name, "new_ref_array", 13))) {
            fputs("    if ((", o);
            low.lower_expr(sc->args[1], op, ValueType::TyI32);
            fputs(") < 0) return throw_negative_array_size(vm, vm->heap);\n", o);
        }
    }
    if (SemLowerer::has_error(op, "ClassCast") && !op->stack_in.empty() && op->operands.size() >= 1) {
        fprintf(o, "    if ((%s) != 0 && instance_of(vm, vm->heap, %s",
                op->stack_in[0]->name, op->stack_in[0]->name);
        for (auto* od : op->operands)
            fprintf(o, ", %s", od->name);
        fputs(") == 0) return throw_class_cast(vm, vm->heap);\n", o);
    }
}

void VmEmitter::emit_post_guards(SemLowerer& low, const Opcode* op) {
    FILE* o = low.out();
    if (SemLowerer::has_error(op, "OutOfMemory") && !op->stack_out.empty())
        fprintf(o, "    if ((%s) == 0) return throw_out_of_memory(vm, vm->heap);\n",
                op->stack_out[0]->name);
}

// ── Name-occurrence + liveness ──────────────────────────────

int VmEmitter::expr_refs_name(const SemExpr* e, const char* name) {
    if (!e) return 0;
    switch (e->tag) {
    case SemExprTag::SBinOp: {
        auto* b = static_cast<const SBinOp*>(e);
        return expr_refs_name(b->left, name) || expr_refs_name(b->right, name);
    }
    case SemExprTag::SUnary:   return expr_refs_name(static_cast<const SUnary*>(e)->operand, name);
    case SemExprTag::STernary: {
        auto* t = static_cast<const STernary*>(e);
        return expr_refs_name(t->cond, name) || expr_refs_name(t->then_, name) ||
               expr_refs_name(t->else_, name);
    }
    case SemExprTag::SCast:    return expr_refs_name(static_cast<const SCast*>(e)->operand, name);
    case SemExprTag::SIndex: {
        auto* ix = static_cast<const SIndex*>(e);
        return expr_refs_name(ix->base, name) || expr_refs_name(ix->index, name);
    }
    case SemExprTag::SCall: {
        for (auto* a : static_cast<const SCall*>(e)->args)
            if (expr_refs_name(a, name)) return 1;
        return 0;
    }
    case SemExprTag::SIdent:   return !strcmp(static_cast<const SIdent*>(e)->name, name);
    case SemExprTag::SInt:     return 0;
    case SemExprTag::SFloat:   return 0;
    }
    return 0;
}

int VmEmitter::stmt_refs_name(const SemStmt* s, const char* name) {
    if (!s) return 0;
    switch (s->tag) {
    case SemStmtTag::SAssign: {
        auto* a = static_cast<const SAssign*>(s);
        return expr_refs_name(a->target, name) || expr_refs_name(a->value, name);
    }
    case SemStmtTag::SExprStmt: return expr_refs_name(static_cast<const SExprStmt*>(s)->value, name);
    case SemStmtTag::SLocalDecl: {
        auto* d = static_cast<const SLocalDecl*>(s);
        return d->init.has_value() ? expr_refs_name(*d->init, name) : 0;
    }
    case SemStmtTag::SIf: {
        auto* sif = static_cast<const SIf*>(s);
        return expr_refs_name(sif->cond, name) || stmt_refs_name(sif->then_, name) ||
               (sif->else_.has_value() && stmt_refs_name(*sif->else_, name));
    }
    case SemStmtTag::SWhile: {
        auto* w = static_cast<const SWhile*>(s);
        return expr_refs_name(w->cond, name) || stmt_refs_name(w->body, name);
    }
    case SemStmtTag::SFor: {
        auto* f = static_cast<const SFor*>(s);
        return (f->init.has_value() && stmt_refs_name(*f->init, name)) ||
               (f->cond.has_value() && expr_refs_name(*f->cond, name)) ||
               (f->update.has_value() && stmt_refs_name(*f->update, name)) ||
               stmt_refs_name(f->body, name);
    }
    case SemStmtTag::SBlock: {
        for (auto* st : static_cast<const SBlock*>(s)->stmts)
            if (stmt_refs_name(st, name)) return 1;
        return 0;
    }
    case SemStmtTag::STrap:    return 0;
    }
    return 0;
}

int VmEmitter::stack_in_live(const Opcode* op, int k) {
    const char* name = op->stack_in[k]->name;
    for (auto* s : op->sem_body)
        if (stmt_refs_name(s, name)) return 1;
    for (auto* so : op->stack_out) {
        const char* se = out_sem_expr(so);
        size_t l = se ? strlen(se) : 0;
        if (se && se[0] == '"' && l >= 2 && strlen(name) == l - 2 &&
            !strncmp(se + 1, name, l - 2)) return 1;
    }
    if (k == 0 && SemLowerer::has_error(op, "ClassCast")) return 1;
    return 0;
}

bool VmEmitter::is_emittable(const Opcode* op) {
    if (!op->sem_body.empty()) return true;
    for (auto* f : op->flags)
        if (!strncmp(f, "special_", 8)) return false;
    return true;
}

void VmEmitter::put_upper(FILE* o, const char* s) {
    for (; *s; s++) fputc(toupper((unsigned char)*s), o);
}

const char* VmEmitter::tag_for(jrow r) {
    if (r.field == 'r') return "T_REF";
    if (r.field == 'v') return "T_V128";
    if (r.field == 'l') return "T_LONG";
    if (r.field == 'd') return "T_DOUBLE";
    if (r.field == 'f') return r.slots == 1 ? "T_FLOAT" : "T_DOUBLE";
    if (r.field == 'i') return r.slots == 1 ? "T_INT"   : "T_LONG";
    return r.slots == 1 ? "T_SHORT" : "T_INT";
}

// ── Slot macros + prologue ──────────────────────────────────

void VmEmitter::emit_slot_macros(SemLowerer& low) {
    FILE* o = low.out();
    char ic; const char* islot; const char* uw; int w;
    low.int_slot_view(&ic, &islot, &uw, &w);
    const char* seen[32]; int ns = 0;
    for (auto* td : mod_->types) {
        const char* c = SemLowerer::slot_class(td->ty);
        int dup = 0;
        for (int k = 0; k < ns; k++) { if (!strcmp(seen[k], c)) { dup = 1; break; } }
        if (dup) continue;
        seen[ns++] = c;
        jrow r = low.jtype(td->ty);
        if (r.slots == 1 && r.field == 'r') {
            fprintf(o, "#define GPOP_%s(name) %s name = f->stack[--f->sp].%c\n",
                    c, r.scalar, r.field);
            fprintf(o, "#define GPUSH_%s(v) do { f->stack[f->sp].l = (s8)(%s)(v);"
                       " f->stack_types[f->sp] = %s; f->sp++; } while (0)\n",
                    c, r.scalar, tag_for(r));
        } else if (r.slots == 1) {
            fprintf(o, "#define GPOP_%s(name) %s name = f->stack[--f->sp].%c\n",
                    c, r.scalar, r.field);
            fprintf(o, "#define GPUSH_%s(_gv) do { f->stack[f->sp].%c = (%s)(_gv);"
                       " f->stack_types[f->sp] = %s; f->sp++; } while (0)\n",
                    c, r.field, r.scalar, tag_for(r));
        } else if (r.scalar[0] == 'f') {
            fprintf(o, "#define GPOP_%s(name) %s name; do { f->sp -= 2;"
                       " union { %s w; %s u; } _x;"
                       " _x.u = ((%s)(%s)f->stack[f->sp].%c) | (((%s)(%s)f->stack[f->sp+1].%c) << %d);"
                       " name = _x.w; } while (0)\n",
                    c, r.scalar, r.scalar, r.uscalar, r.uscalar, uw, ic, r.uscalar, uw, ic, w);
            fprintf(o, "#define GPUSH_%s(v) do { union { %s w; %s u; } _x; _x.w = (v);"
                       " f->stack[f->sp].%c = (%s)(_x.u); f->stack_types[f->sp] = %s;"
                       " f->stack[f->sp+1].%c = (%s)(_x.u >> %d); f->stack_types[f->sp+1] = T_VOID;"
                       " f->sp += 2; } while (0)\n",
                    c, r.scalar, r.uscalar, ic, islot, tag_for(r), ic, islot, w);
        } else {
            fprintf(o, "#define GPOP_%s(name) %s name; do { f->sp -= 2;"
                       " name = ((%s)(%s)f->stack[f->sp].%c) | (((%s)f->stack[f->sp+1].%c) << %d); } while (0)\n",
                    c, r.scalar, r.scalar, uw, ic, r.scalar, ic, w);
            fprintf(o, "#define GPUSH_%s(v) do { %s _v = (v);"
                       " f->stack[f->sp].%c = (%s)(_v); f->stack_types[f->sp] = %s;"
                       " f->stack[f->sp+1].%c = (%s)(_v >> %d); f->stack_types[f->sp+1] = T_VOID;"
                       " f->sp += 2; } while (0)\n",
                    c, r.scalar, ic, islot, tag_for(r), ic, islot, w);
        }
    }
    fputs(
"#define GPOP_WORD(name) slot_t name; u1 name##_wt;"
" do { f->sp--; name = f->stack[f->sp]; name##_wt = f->stack_types[f->sp]; } while (0)\n"
"#define GPUSH_WORD(name) do { f->stack[f->sp] = (name);"
" f->stack_types[f->sp] = name##_wt; f->sp++; } while (0)\n", o);
    fputs(
"#define GPOP_ANY(name) any_t name; do { if (f->stack_types[f->sp-1] == T_VOID) { f->sp -= 2; name.bits = ((s8)(u4)f->stack[f->sp].i) | (((s8)f->stack[f->sp+1].i) << 32); name.kind = f->stack_types[f->sp]; } else { f->sp -= 1; name.bits = (s8)(u4)f->stack[f->sp].i; name.kind = f->stack_types[f->sp]; } } while (0)\n"
"#define GPUSH_ANY(v) do { any_t _a = (v); if (_a.kind == T_LONG || _a.kind == T_DOUBLE) { f->stack[f->sp].i = (s4)_a.bits; f->stack_types[f->sp] = _a.kind; f->stack[f->sp+1].i = (s4)(_a.bits >> 32); f->stack_types[f->sp+1] = T_VOID; f->sp += 2; } else { f->stack[f->sp].i = (s4)_a.bits; f->stack_types[f->sp] = _a.kind; f->sp += 1; } } while (0)\n", o);
    fputs("\n", o);
}

void VmEmitter::emit_prologue(SemLowerer& low) {
    FILE* o = low.out();
    fputs(
"#include \"gen_interp.h\"\n"
"#include \"opcodes.h\"\n"
"\n"
"/* Threaded dispatch: each handler does its work and musttail-jumps to the next\n"
" * opcode (wasm_next) or, on a guard, to the trap continuation (wasm_trap) —\n"
" * the same continuation shape as the JIT stencils. Both are supplied by the\n"
" * runtime backend. */\n"
"#define TAIL __attribute__((musttail))\n"
"void wasm_next(vm_t* vm);\n"
"void wasm_trap(vm_t* vm);\n"
"\n"
"#define FETCH_U1() (f->code[f->pc++])\n"
"#define FETCH_S1() ((s1)f->code[f->pc++])\n"
"#define FETCH_U2() (f->pc += 2, (u2)((f->code[f->pc-2] << 8) | f->code[f->pc-1]))\n"
"#define FETCH_S2() ((s2)FETCH_U2())\n"
"#define FETCH_U4() (f->pc += 4, ((u4)f->code[f->pc-4] << 24) | ((u4)f->code[f->pc-3] << 16) | ((u4)f->code[f->pc-2] << 8) | (u4)f->code[f->pc-1])\n"
"#define FETCH_S4() ((s4)FETCH_U4())\n"
"\n", o);
    emit_slot_macros(low);
}

// ── One opcode (interp handler / stencil) ───────────────────

void VmEmitter::emit_one_opcode(SemLowerer& low, const Opcode* op) {
    FILE* o = low.out();
    int stencil = (low.mode() == Mode::Stencil);
    if (stencil) {
        fprintf(o, "void STENCIL gen_st_%s(vm_t* vm) {\n", op->mnemonic);
        fputs("    frame_t* f = &vm->frame; (void)f;\n", o);
    } else {
        fprintf(o, "static void gen_op_%s(vm_t* vm) {\n", op->mnemonic);
        fputs("    frame_t* f = &vm->frame; (void)f;\n", o);
    }

    int uses_scratch = 0;
    for (auto* s : op->sem_body)
        if (stmt_refs_name(s, "scratch")) { uses_scratch = 1; break; }
    if (uses_scratch)
        fputs("    slot_t scratch[4]; u1 scratch_t[4];\n", o);

    for (auto* od : op->operands) {
        const char* rf = SemLowerer::fetch_fn(od->ty);
        ValueType oty = od->ty;
        const char* onm = od->name;
        if (oty == ValueType::TyMemarg) {
            if (stencil) {
                fprintf(o, "    u4 memidx = (u4)_HOLE_memidx;\n");
                fprintf(o, "    u8 %s = (u8)_HOLE_%s;\n", onm, onm);
            } else {
                fprintf(o, "    u4 %s_flags = 0; (void)bbq_read_uleb128_u32(&f->code, &%s_flags);\n", onm, onm);
                fprintf(o, "    u4 memidx = 0; if (%s_flags & 0x40) (void)bbq_read_uleb128_u32(&f->code, &memidx);\n", onm);
                fprintf(o, "    u8 %s = 0; (void)bbq_read_uleb128_u64(&f->code, &%s);\n", onm, onm);
            }
            int uo = 0, um = 0;
            for (auto* s : op->sem_body) {
                if (stmt_refs_name(s, onm))      uo = 1;
                if (stmt_refs_name(s, "memidx")) um = 1;
            }
            if (!uo) fprintf(o, "    (void)%s;\n", onm);
            if (!um) fprintf(o, "    (void)memidx;\n");
            continue;
        }
        if (stencil && oty == ValueType::TyF32)
            fprintf(o, "    f4 %s = (__extension__({ union { uint32_t i; f4 f; } _u; _u.i = (uint32_t)_HOLE_%s; _u.f; }));\n", onm, onm);
        else if (stencil && oty == ValueType::TyF64)
            fprintf(o, "    f8 %s = (__extension__({ union { uint64_t i; f8 f; } _u; _u.i = (uint64_t)_HOLE_%s; _u.f; }));\n", onm, onm);
        else if (stencil)
            fprintf(o, "    %s %s = (%s)_HOLE_%s;\n",
                    low.c_scalar(oty), onm, low.c_scalar(oty), onm);
        else if (rf)
            fprintf(o, "    %s %s = 0; (void)%s(&f->code, &%s);\n",
                    low.c_scalar(od->ty), od->name, rf, od->name);
        else
            fprintf(o, "    %s %s = %s;\n",
                    low.c_scalar(od->ty), od->name, SemLowerer::fetch_expr(od->ty));
        int used = 0;
        for (auto* s : op->sem_body)
            if (stmt_refs_name(s, od->name)) { used = 1; break; }
        if (!used) fprintf(o, "    (void)%s;\n", od->name);
    }

    for (int k = (int)op->stack_in.size() - 1; k >= 0; k--) {
        if (stack_in_live(op, k))
            fprintf(o, "    GPOP_%s(%s);\n",
                    SemLowerer::slot_class(op->stack_in[k]->ty), op->stack_in[k]->name);
        else
            fprintf(o, "    f->sp -= %d;\n", low.jtype(op->stack_in[k]->ty).slots);
    }

    emit_guards(low, op);

    for (auto* so : op->stack_out)
        if (!out_sem_expr(so)) {
            if (so->ty == ValueType::TyWord)
                fprintf(o, "    slot_t %s; u1 %s_wt;\n", so->name, so->name);
            else
                fprintf(o, "    %s %s;\n", low.c_scalar(so->ty), so->name);
        }

    int lane_n = 0; const char* lane_f = nullptr;
    int reduce_n = 0; const char* reduce_f = nullptr;
    int has_laneidx = 0;
    for (auto* od : op->operands)
        if (od->ty == ValueType::TyU8) has_laneidx = 1;
    if (!has_laneidx) {
        for (size_t k = 0; k < op->stack_out.size() && !lane_n; k++) SemLowerer::lane_elem(op->stack_out[k]->ty, &lane_n, &lane_f);
        if (!lane_n)
            for (size_t k = 0; k < op->stack_in.size() && !reduce_n; k++) SemLowerer::lane_elem(op->stack_in[k]->ty, &reduce_n, &reduce_f);
    }
    if (lane_n) {
        low.set_lane(1);
        fprintf(o, "    for (int _k = 0; _k < %d; _k++) {\n", lane_n);
        for (auto* s : op->sem_body) low.lower_stmt(op, s);
        fputs("    }\n", o);
        low.set_lane(0);
    } else if (reduce_n) {
        if (!op->sem_body.empty()) low.lower_stmt(op, op->sem_body[0]);
        low.set_lane(1);
        fprintf(o, "    for (int _k = 0; _k < %d; _k++) {\n", reduce_n);
        for (size_t k = 1; k < op->sem_body.size(); k++) low.lower_stmt(op, op->sem_body[k]);
        fputs("    }\n", o);
        low.set_lane(0);
    } else {
        for (auto* s : op->sem_body) low.lower_stmt(op, s);
    }

    emit_post_guards(low, op);

    for (auto* so : op->stack_out) {
        const char* se = out_sem_expr(so);
        fprintf(o, "    GPUSH_%s(", SemLowerer::slot_class(so->ty));
        if (se && se[0] == '"') {
            fprintf(o, "%.*s", (int)strlen(se) - 2, se + 1);
        } else {
            fputs(se ? se : so->name, o);
        }
        fputs(");\n", o);
    }

    if (!low.body_tail_returns(op)) {
        if (stencil) fputs("    TAIL return _HOLE_cont(vm);\n", o);
        else         fputs("    TAIL return wasm_next(vm);\n", o);
    }
    fputs("}\n\n", o);
}

// ── gen_interp.c ────────────────────────────────────────────

void VmEmitter::emit_interp_c(FILE* o) {
    SemLowerer low(mod_, o, Mode::Interp);
    fputs("/* AUTO-GENERATED by opgen — do not edit. */\n"
          "/* Source: opcodes.def */\n\n", o);
    emit_prologue(low);

    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op)) continue;
        emit_one_opcode(low, op);
    }

    int prefixes[8]; int npfx = 0;
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || op->subop < 0) continue;
        int seen = 0; for (int p = 0; p < npfx; p++) if (prefixes[p] == op->opcode_val) seen = 1;
        if (!seen && npfx < 8) prefixes[npfx++] = op->opcode_val;
    }
    for (int p = 0; p < npfx; p++) {
        int pv = prefixes[p], maxsub = 0;
        for (auto* op : mod_->opcodes)
            if (is_emittable(op) && op->subop >= 0 && op->opcode_val == pv && op->subop > maxsub) maxsub = op->subop;
        fprintf(o, "static const opcode_handler_t gen_sub_%02x[%d] = {\n", pv, maxsub + 1);
        for (auto* op : mod_->opcodes)
            if (is_emittable(op) && op->subop >= 0 && op->opcode_val == pv)
                fprintf(o, "    [%d] = gen_op_%s,\n", op->subop, op->mnemonic);
        fprintf(o, "};\nstatic void gen_dispatch_%02x(vm_t* vm) {\n"
                   "    u4 sub = 0; (void)bbq_read_uleb128_u32(&vm->frame.code, &sub);\n"
                   "    TAIL return gen_sub_%02x[sub](vm);\n}\n\n", pv, pv);
    }

    fputs("static const opcode_handler_t gen_dispatch[256] = {\n", o);
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || op->subop >= 0) continue;
        fputs("    [OP_", o); put_upper(o, op->mnemonic);
        fprintf(o, "] = gen_op_%s,\n", op->mnemonic);
    }
    for (int p = 0; p < npfx; p++)
        fprintf(o, "    [0x%02x] = gen_dispatch_%02x,\n", prefixes[p], prefixes[p]);
    fputs("};\n\n"
          "const opcode_handler_t* gen_interp_dispatch_table(void) {\n"
          "    return gen_dispatch;\n"
          "}\n", o);
}

// ── wasm_stencils.c ─────────────────────────────────────────

void VmEmitter::emit_stencil_c(FILE* o) {
    SemLowerer low(mod_, o, Mode::Stencil);
    fputs("/* AUTO-GENERATED by opgen — jitterator stencils. Source: opcodes.def */\n", o);
    fputs("#include \"runtime_api.h\"\n\n", o);
    fputs("#define STENCIL __attribute__((preserve_none))\n"
          "#define TAIL    __attribute__((musttail))\n\n", o);
    emit_slot_macros(low);
    fputs("extern void STENCIL _HOLE_cont(vm_t* vm);\n"
          "extern void STENCIL _HOLE_trap(vm_t* vm);\n"
          "extern void STENCIL _HOLE_resync(vm_t* vm);  /* the in-buffer IP-dispatch stencil */\n"
          "extern uint64_t _HOLE_ip;                    /* a control op's own post-operand position */\n", o);

    {
        char cseen[256][64]; int cnn = 0;
        for (auto* cop : mod_->opcodes) {
            if (!is_emittable(cop) || SemLowerer::stencil_excluded(cop)) continue;
            const char* hn[8]; uint64_t hv[8];
            int nh = low.op_const_holes(cop, hn, hv);
            for (int k = 0; k < nh; k++) {
                int dup = 0;
                for (int s = 0; s < cnn; s++) if (!strcmp(cseen[s], hn[k])) { dup = 1; break; }
                if (dup || cnn >= 256) continue;
                fprintf(o, "extern uint64_t %s;\n", hn[k]);
                snprintf(cseen[cnn++], 64, "%s", hn[k]);
            }
        }
    }

    const char* seen[256]; int ns = 0;
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || SemLowerer::stencil_excluded(op)) continue;
        for (auto* od : op->operands) {
            const char* nm = od->name;
            int dup = 0; for (int s = 0; s < ns; s++) if (!strcmp(seen[s], nm)) dup = 1;
            if (dup) continue;
            seen[ns++] = nm;
            fprintf(o, "extern uint64_t _HOLE_%s;\n", nm);
        }
    }

    const char* nat[256]; int nn = 0;
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || SemLowerer::stencil_excluded(op)) continue;
        for (auto* s : op->sem_body)
            low.collect_natives_stmt(s, nat, &nn);
    }
    for (int i = 0; i < nn; i++)
        fprintf(o, "extern uint64_t _HOLE_%s;\n", nat[i]);
    for (int i = 0; g_libm_syms[i]; i++)
        fprintf(o, "extern uint64_t _HOLE_%s;\n", g_libm_syms[i]);
    fputs("\n", o);

    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || SemLowerer::stencil_excluded(op)) continue;
        emit_one_opcode(low, op);
    }

    // Machinery stencils — the machine boundary + control continuations, the
    // SAME for any opgen VM (not per-spec), so they are generated here rather
    // than hand-written per consumer. `entry` crosses the C → preserve_none
    // boundary; `trap` records a trap; `resync` maps an IP to the stamped
    // successor through the driver-built offmap; `gen_st_halt` is the off-the-
    // end terminator (a guarded result stash — works whether the function
    // returns implicitly off the end or via an explicit return opcode).
    fputs(
"\n/* ── Machinery stencils (generated): the machine boundary + control\n"
" * continuations, identical for every opgen VM (the JIT requires a bbq_ctx\n"
" * code cursor in the backend frame). ── */\n"
"void STENCIL gen_st_halt(vm_t* vm) {\n"
"    frame_t* f = &vm->frame;\n"
"    if (f->sp > 0) { vm->result = f->stack[f->sp - 1]; vm->result_type = f->stack_types[f->sp - 1]; }\n"
"}\n"
"typedef void STENCIL (*opgen_kont_t)(vm_t*);\n"
"extern uint64_t _HOLE_offmap;\n"
"extern uint64_t _HOLE_codelen;\n"
"void STENCIL resync(vm_t* vm) {\n"
"    size_t _p = vm->frame.code.pos;\n"
"    size_t _len = (size_t)_HOLE_codelen;\n"
"    opgen_kont_t* _map = (opgen_kont_t*)(uintptr_t)_HOLE_offmap;\n"
"    opgen_kont_t _fn = (_p <= _len) ? _map[_p] : _map[_len];\n"
"    if (!_fn) _fn = _map[_len];\n"
"    TAIL return _fn(vm);\n"
"}\n"
"void STENCIL trap(vm_t* vm) { vm->trapped = 1; }\n"
"void entry(vm_t* vm) {\n"
"    return ((void STENCIL (*)(vm_t*))_HOLE_cont)(vm);\n"
"}\n", o);
}

// ── wasm_jit_symbols.h ──────────────────────────────────────

void VmEmitter::emit_jit_symbols(FILE* o) {
    fputs("/* AUTO-GENERATED by opgen — do not edit. Source: opcodes.def */\n", o);
    fputs("#ifndef WASM_JIT_SYMBOLS_H\n#define WASM_JIT_SYMBOLS_H\n", o);
    fputs("#include \"runtime_api.h\"   /* the native prototypes */\n", o);
    fputs("#include <math.h>           /* the libm intrinsic prototypes */\n\n", o);
    fputs("typedef struct { const char* name; void* addr; } wasm_jit_sym_t;\n\n", o);
    fputs("static const wasm_jit_sym_t wasm_jit_symbols[] = {\n", o);
    for (auto* md : mod_->methods) {
        if (!md->native) continue;
        fprintf(o, "    { \"_HOLE_%s\", (void*)%s },\n", md->name, md->name);
    }
    for (int i = 0; g_libm_syms[i]; i++)
        fprintf(o, "    { \"_HOLE_%s\", (void*)%s },\n", g_libm_syms[i], g_libm_syms[i]);
    fputs("};\n", o);
    fputs("static const int wasm_jit_symbols_count =\n"
          "    (int)(sizeof(wasm_jit_symbols) / sizeof(wasm_jit_symbols[0]));\n\n", o);
    fputs("#endif /* WASM_JIT_SYMBOLS_H */\n", o);
}

const char* VmEmitter::jop_kind(ValueType t) {
    switch (t) {
    case ValueType::TyU8:     return "JOP_U8";
    case ValueType::TyUleb32: return "JOP_ULEB32";
    case ValueType::TyUleb64: return "JOP_ULEB64";
    case ValueType::TySleb32: return "JOP_SLEB32";
    case ValueType::TySleb64: return "JOP_SLEB64";
    case ValueType::TyF32:    return "JOP_F32";
    case ValueType::TyF64:    return "JOP_F64";
    case ValueType::TyMemarg: return "JOP_MEMARG";
    default:                  return "JOP_NONE";
    }
}

// ── wasm_jit_meta.h ─────────────────────────────────────────

void VmEmitter::emit_jit_meta(FILE* o) {
    SemLowerer low(mod_, o, Mode::Interp);
    fputs("/* AUTO-GENERATED by opgen — do not edit. JIT opcode metadata. */\n"
          "#ifndef WASM_JIT_META_H\n#define WASM_JIT_META_H\n"
          "#include \"opcodes.h\"\n#include \"wasm_stencil_table.h\"\n\n"
          "typedef enum { JOP_NONE, JOP_ULEB32, JOP_ULEB64, JOP_SLEB32, JOP_SLEB64,\n"
          "               JOP_F32, JOP_F64, JOP_U8, JOP_MEMARG, JOP_CONST } jit_operand_kind_t;\n"
          "typedef struct { const char* hole; jit_operand_kind_t kind; uint64_t value; } jit_operand_t;\n"
          "typedef struct { int stencil; const jit_operand_t* operands;\n"
          "                 unsigned char operand_count, pop, push; } wasm_jit_meta_t;\n\n", o);

    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op)) continue;
        const char* cn[8]; uint64_t cv[8];
        int nc = low.op_const_holes(op, cn, cv);
        if (op->operands.empty() && nc == 0) continue;
        fprintf(o, "static const jit_operand_t jitops_%s[] = {", op->mnemonic);
        for (auto* od : op->operands)
            fprintf(o, " {\"_HOLE_%s\", %s, 0},", od->name, jop_kind(od->ty));
        for (int k = 0; k < nc; k++)
            fprintf(o, " {\"%s\", JOP_CONST, 0x%llxULL},", cn[k], (unsigned long long)cv[k]);
        fputs(" };\n", o);
    }

    auto emit_meta_row = [&](const Opcode* op) {
        const char* cn[8]; uint64_t cv[8];
        int pop = (int)op->stack_in.size(), push = (int)op->stack_out.size();
        int nops = (int)op->operands.size() + low.op_const_holes(op, cn, cv);
        if (SemLowerer::stencil_excluded(op)) fputs("-1", o);
        else { fputs("STENCIL_GEN_ST_", o); put_upper(o, op->mnemonic); }
        if (nops) fprintf(o, ", jitops_%s, %d, %d, %d },\n", op->mnemonic, nops, pop, push);
        else      fprintf(o, ", NULL, 0, %d, %d },\n", pop, push);
    };

    int prefixes[8]; int npfx = 0;
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || op->subop < 0) continue;
        int seen = 0; for (int p = 0; p < npfx; p++) if (prefixes[p] == op->opcode_val) seen = 1;
        if (!seen && npfx < 8) prefixes[npfx++] = op->opcode_val;
    }
    for (int p = 0; p < npfx; p++) {
        int pv = prefixes[p], maxsub = 0;
        for (auto* op : mod_->opcodes)
            if (is_emittable(op) && op->subop >= 0 && op->opcode_val == pv && op->subop > maxsub) maxsub = op->subop;
        fprintf(o, "\nstatic const wasm_jit_meta_t wasm_jit_meta_sub_%02x[%d] = {\n", pv, maxsub + 1);
        for (auto* op : mod_->opcodes) {
            if (!is_emittable(op) || op->subop < 0 || op->opcode_val != pv) continue;
            fprintf(o, "    [%d] = { ", op->subop); emit_meta_row(op);
        }
        fputs("};\n", o);
    }

    fputs("\nstatic const wasm_jit_meta_t wasm_jit_meta[256] = {\n", o);
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || op->subop >= 0) continue;
        fputs("    [OP_", o); put_upper(o, op->mnemonic); fputs("] = { ", o);
        emit_meta_row(op);
    }
    fputs("};\n", o);

    fputs("\nstatic const wasm_jit_meta_t* const wasm_jit_meta_sub[256] = {\n", o);
    for (int p = 0; p < npfx; p++)
        fprintf(o, "    [0x%02x] = wasm_jit_meta_sub_%02x,\n", prefixes[p], prefixes[p]);
    fputs("};\n#endif /* WASM_JIT_META_H */\n", o);
}

const char* VmEmitter::wvt_name(ValueType t) {
    switch (t) {
    case ValueType::TyI32:       return "WVT_I32";
    case ValueType::TyI64:       return "WVT_I64";
    case ValueType::TyF32:       return "WVT_F32";
    case ValueType::TyF64:       return "WVT_F64";
    case ValueType::TyV128:      return "WVT_V128";
    case ValueType::TyI8x16: case ValueType::TyI16x8: case ValueType::TyI32x4:
    case ValueType::TyI64x2: case ValueType::TyF32x4: case ValueType::TyF64x2: return "WVT_V128";
    case ValueType::TyFuncRef:   return "WVT_FUNCREF";
    case ValueType::TyExternRef: return "WVT_EXTERNREF";
    case ValueType::TyI31Ref:    return "WVT_I31REF";
    default:                     return "WVT_BOT";
    }
}

// ── wasm_valtype.h ──────────────────────────────────────────

void VmEmitter::emit_valtype_h(FILE* o) {
    fputs("/* AUTO-GENERATED by opgen — do not edit. WASM value-type tags. */\n"
          "#ifndef WASM_VALTYPE_H\n#define WASM_VALTYPE_H\n"
          "#include <stdint.h>\n\n"
          "typedef enum { WVT_BOT=0, WVT_I32, WVT_I64, WVT_F32, WVT_F64, WVT_V128,\n"
          "               WVT_FUNCREF, WVT_EXTERNREF, WVT_I31REF,    /* nullable abstract refs */\n"
          "               WVT_FUNCREF_NN, WVT_EXTERNREF_NN, WVT_I31REF_NN, /* non-null: (ref ht) <: (ref null ht) */\n"
          "               WVT_STRUCTREF, WVT_ARRAYREF,  /* (ref null $t) struct/array — typeidx on a parallel stack */\n"
          "               WVT_STRUCTREF_NN, WVT_ARRAYREF_NN,  /* (ref $t) — non-null struct/array */\n"
          "               WVT_EXNREF, WVT_EXNREF_NN   /* (ref null exn) / (ref exn) — exception references */\n"
          "             } wasm_valtype_t;\n"
          "typedef struct { const wasm_valtype_t* pops; uint8_t npop;\n"
          "                 const wasm_valtype_t* pushes; uint8_t npush;\n"
          "                 uint8_t present; } wasm_opsig_t;\n\n"
          "/* Storage width/alignment of a value of type t when it lives OUTSIDE the\n"
          " * 16-byte operand slot — i.e. in a heap field/element, a global, or a JIT\n"
          " * footer constant. The single source of truth that keeps those sites from\n"
          " * each re-deriving (and mis-deriving) the size: every scalar/ref is its\n"
          " * natural width, v128 is the only 16-byte member. (BOT never stores.) */\n"
          "static inline uint8_t wasm_valtype_size(wasm_valtype_t t) {\n"
          "    switch (t) {\n"
          "    case WVT_BOT:  return 0;\n"
          "    case WVT_I32:  case WVT_F32:  return 4;\n"
          "    case WVT_V128: return 16;\n"
          "    default:       return 8;  /* i64/f64 + every ref kind (8-byte handle/pointer) */\n"
          "    }\n"
          "}\n"
          "static inline uint8_t wasm_valtype_align(wasm_valtype_t t) {\n"
          "    uint8_t s = wasm_valtype_size(t); return s ? s : 1;\n"
          "}\n"
          "#endif /* WASM_VALTYPE_H */\n", o);
}

// ── wasm_type_meta.h ────────────────────────────────────────

void VmEmitter::emit_type_meta(FILE* o) {
    fputs("/* AUTO-GENERATED by opgen — do not edit. §7.6 validator type signatures. */\n"
          "#ifndef WASM_TYPE_META_H\n#define WASM_TYPE_META_H\n"
          "#include \"opcodes.h\"\n#include \"wasm_valtype.h\"\n\n", o);

    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op)) continue;
        if (!op->stack_in.empty()) {
            fprintf(o, "static const wasm_valtype_t sigp_%s[] = {", op->mnemonic);
            for (auto* s : op->stack_in) fprintf(o, " %s,", wvt_name(s->ty));
            fputs(" };\n", o);
        }
        if (!op->stack_out.empty()) {
            fprintf(o, "static const wasm_valtype_t sigr_%s[] = {", op->mnemonic);
            for (auto* s : op->stack_out) fprintf(o, " %s,", wvt_name(s->ty));
            fputs(" };\n", o);
        }
    }

    auto emit_sig_row = [&](const Opcode* op) {
        if (!op->stack_in.empty())  fprintf(o, "sigp_%s, %d, ", op->mnemonic, (int)op->stack_in.size());
        else fputs("NULL, 0, ", o);
        if (!op->stack_out.empty()) fprintf(o, "sigr_%s, %d, 1 },\n", op->mnemonic, (int)op->stack_out.size());
        else fputs("NULL, 0, 1 },\n", o);
    };

    int prefixes[8]; int npfx = 0;
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || op->subop < 0) continue;
        int seen = 0; for (int p = 0; p < npfx; p++) if (prefixes[p] == op->opcode_val) seen = 1;
        if (!seen && npfx < 8) prefixes[npfx++] = op->opcode_val;
    }
    for (int p = 0; p < npfx; p++) {
        int pv = prefixes[p], maxsub = 0;
        for (auto* op : mod_->opcodes)
            if (is_emittable(op) && op->subop >= 0 && op->opcode_val == pv && op->subop > maxsub) maxsub = op->subop;
        fprintf(o, "\nstatic const wasm_opsig_t wasm_opsig_sub_%02x[%d] = {\n", pv, maxsub + 1);
        for (auto* op : mod_->opcodes) {
            if (!is_emittable(op) || op->subop < 0 || op->opcode_val != pv) continue;
            fprintf(o, "    [%d] = { ", op->subop); emit_sig_row(op);
        }
        fputs("};\n", o);
    }

    fputs("\nstatic const wasm_opsig_t wasm_opsig[256] = {\n", o);
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || op->subop >= 0) continue;
        fputs("    [OP_", o); put_upper(o, op->mnemonic); fputs("] = { ", o);
        emit_sig_row(op);
    }
    fputs("};\n", o);

    fputs("\nstatic const wasm_opsig_t* const wasm_opsig_sub[256] = {\n", o);
    for (int p = 0; p < npfx; p++)
        fprintf(o, "    [0x%02x] = wasm_opsig_sub_%02x,\n", prefixes[p], prefixes[p]);
    fputs("};\n#endif /* WASM_TYPE_META_H */\n", o);
}

// ── gen_interp.h ────────────────────────────────────────────

void VmEmitter::emit_interp_h(FILE* o) {
    fputs("/* AUTO-GENERATED by opgen — do not edit. */\n"
          "/* Source: opcodes.def */\n\n"
          "#ifndef GEN_INTERP_H\n"
          "#define GEN_INTERP_H\n\n"
          "#include \"runtime_api.h\"\n\n"
          "/* An opcode handler: runs one instruction, then musttail-jumps to\n"
          " * its successor (the next opcode, or the trap continuation). */\n", o);
    fputs("typedef void (*opcode_handler_t)(vm_t* vm);  /* threaded: TAILs to the next */\n\n", o);
    fputs("/* The 256-entry opcode -> handler table (unfilled entries are NULL). */\n"
          "const opcode_handler_t* gen_interp_dispatch_table(void);\n\n"
          "#endif /* GEN_INTERP_H */\n", o);
}

// ── runtime_api.h (value model + native prototypes) ─────────

void VmEmitter::emit_value_model(SemLowerer& low, FILE* o) {
    char ic; const char* islot; const char* uw; int w;
    low.int_slot_view(&ic, &islot, &uw, &w);
    (void)ic; (void)islot; (void)w;

    fputs("#include <stdint.h>\n\n"
"typedef int8_t  s1; typedef int16_t s2; typedef int32_t s4; typedef int64_t s8;\n"
"typedef uint8_t u1; typedef uint16_t u2; typedef uint32_t u4; typedef uint64_t u8;\n"
"typedef float   f4; typedef double  f8;\n", o);

    for (auto* td : mod_->types)
        if (low.jtype(td->ty).field == 'v') {
            fputs("typedef union {            /* 128-bit SIMD: one slot, many lane views */\n"
                  "    u8 u64[2];  s8 i64[2];  f8 f64[2];\n"
                  "    u4 u32[4];  s4 i32[4];  f4 f32[4];\n"
                  "    u2 u16[8];  s2 i16[8];\n"
                  "    u1 u8[16];  s1 i8[16];\n"
                  "} v128_t;\n", o);
            break;
        }

    fprintf(o, "typedef %s ref_t;   /* object reference; 0 == null (word-sized handle) */\n", uw);

    fputs("\ntypedef union {", o);
    char seen_f[8]; int nf = 0;
    for (auto* td : mod_->types) {
        jrow r = low.jtype(td->ty);
        if (r.slots != 1) continue;
        int dup = 0; for (int k = 0; k < nf; k++) if (seen_f[k] == r.field) dup = 1;
        if (dup) continue;
        seen_f[nf++] = r.field;
        fprintf(o, " %s %c;", r.field == 'r' ? "ref_t" : r.scalar, r.field);
    }
    fputs(" } slot_t;\n", o);

    fputs("typedef struct { s8 bits; u1 kind; } any_t;\n", o);

    fputs("enum { T_VOID = 0", o);
    const char* seen_t[16]; int nt = 0;
    for (auto* td : mod_->types) {
        const char* tg = tag_for(low.jtype(td->ty));
        if (!strcmp(tg, "T_VOID")) continue;
        int dup = 0; for (int k = 0; k < nt; k++) if (!strcmp(seen_t[k], tg)) dup = 1;
        if (dup) continue;
        seen_t[nt++] = tg;
        fprintf(o, ", %s", tg);
    }
    fputs(" };\n", o);

    fprintf(o, "typedef enum { %s = 0, %s = 1, %s = 2 } %s;\n",
            low.status_ok(), low.status_err(), low.status_halt(), low.status_type());
}

void VmEmitter::emit_runtime_api_h(FILE* o) {
    SemLowerer low(mod_, o, Mode::Interp);
    fputs(
"/* AUTO-GENERATED by opgen — do not edit. Source: opcodes.def */\n"
"/*\n"
" * runtime_api.h — the generated runtime-service API: the contract between the\n"
" * generated interpreter and a runtime backend (heap, GC, classpool, frames,\n"
" * exceptions). The value types + operand-stack/locals slot model are CONCRETE\n"
" * (the handlers manipulate the frame directly, and invoke reads the same\n"
" * stack); the heap is OPAQUE (services are reached only through the operation\n"
" * prototypes below). Backends are swappable: a consumer names its own\n"
" * frame/vm/heap substrate with the spec's `backend \"<header>\"` directive, or\n"
" * the self-test takes the default frame.\n"
" */\n"
"#ifndef OPGEN_RUNTIME_API_H\n"
"#define OPGEN_RUNTIME_API_H\n"
"\n"
"/* The value model (scalars/ref_t/slot_t/any_t/tags/status) is GENERATED below\n"
" * from the spec's type table (emit_value_model) — one source, no hardcoding.\n"
" * The frame/vm/heap is the SUBSTRATE: the spec's `backend \"<header>\"` directive\n"
" * names it (included here, after the value model), or the self-test default. */\n", o);

    emit_value_model(low, o);

    if (mod_->backend.has_value()) {
        /* Spec-declared substrate: include the consumer's frame header verbatim
         * at this point — the value model it builds on is already in scope. One
         * source (the .def), so no per-translation-unit -D macro to keep in sync. */
        fprintf(o, "\n#include %s\n", *mod_->backend);
    } else {
        fputs(
"\n/* No `backend` directive — the self-test default frame (the FETCH pc/code\n"
" * model, not JIT-capable; a real VM declares its own bbq_ctx-cursor frame). */\n"
"#define MAX_STACK  255\n"
"#define MAX_LOCALS 255\n"
"typedef struct {\n"
"    const u1* code;\n"
"    u2        pc;\n"
"    u2        instr_pc;       /* start pc of the executing instruction (branch base) */\n"
"    slot_t    stack[MAX_STACK];\n"
"    u1        stack_types[MAX_STACK];\n"
"    u1        sp;\n"
"    slot_t    locals[MAX_LOCALS];\n"
"    u1        local_types[MAX_LOCALS];\n"
"    u1        num_locals;\n"
"} frame_t;\n"
"typedef struct { frame_t frame; } vm_t;\n"
"typedef struct heap_t heap_t;   /* OPAQUE: handlers pass it through, never inspect it */\n", o);
    }
    fputs(
"\n/* ── Runtime-service operations (a backend defines all of these). Every op\n"
" * takes the full runtime context (vm = execution state, h = object memory)\n"
" * and uses whichever half it needs; control-transfer ops return a status. ── */\n",
    o);

    for (auto* md : mod_->methods) {
        if (!md->native) continue;
        fprintf(o, "%-13s %s(%s", low.api_c_type(md->ret_ty), md->name,
                "vm_t*, heap_t*");
        for (auto* p : md->params)
            fprintf(o, ", %s", low.api_c_type(p->ty));
        fputs(");\n", o);
    }

    fputs(
"\n/* Synthesized-guard throws: opgen emits calls to these from the contract\n"
" * facts; a backend creates + raises the exception. */\n", o);
    static const char* throws[] = {
        "throw_div_by_zero", "throw_null_pointer", "throw_array_bounds",
        "throw_negative_array_size", "throw_out_of_memory", "throw_class_cast",
    };
    for (size_t i = 0; i < sizeof(throws) / sizeof(throws[0]); i++)
        fprintf(o, "%s %s(vm_t*, heap_t*);\n", low.status_type(), throws[i]);

    fputs("\n#endif /* OPGEN_RUNTIME_API_H */\n", o);
}

} // namespace opgen
