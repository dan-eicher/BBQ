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

// A variable-length "tail" immediate the op's native reads at runtime (br_table / try_table /
// select_t). NOT a fixed operand: no interp decode, no JIT operand hole — it sets the meta's
// tail-skip kind so the JIT compile-walk steps over it (after _HOLE_ip) to stay synced.
static bool is_tail_operand(ValueType t) {
    return t == ValueType::TyBrTable || t == ValueType::TyTryTable || t == ValueType::TySelectVec;
}
static const char* tail_kind_name(const Opcode* op) {
    for (auto* od : op->operands)
        switch (od->ty) {
        case ValueType::TyBrTable:   return "JTAIL_BRTABLE";
        case ValueType::TyTryTable:  return "JTAIL_TRYTABLE";
        case ValueType::TySelectVec: return "JTAIL_SELECTVEC";
        default: break;
        }
    return "JTAIL_NONE";
}
static int n_fixed_operands(const Opcode* op) {
    int n = 0;
    for (auto* od : op->operands) if (!is_tail_operand(od->ty)) n++;
    return n;
}

const char* VmEmitter::out_sem_expr(const StackParam* sp) {
    return sp->sem_expr.has_value() ? *sp->sem_expr : nullptr;
}
const SemExpr* VmEmitter::out_count(const StackParam* sp) {   // §3b variadic: the pop-count expr, or null
    return sp->count.has_value() ? *sp->count : nullptr;
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

// Emit `s`, rewriting the namespace tokens: "wasm_" -> "<prefix>_", "WASM_" ->
// "<PREFIX>_". The single seam that keeps opgen's emitted machinery (jit_meta,
// valtype, opsig, the next/trap continuations, the header guards and the cross
// includes like wasm_stencil_table.h) in the consumer's chosen namespace.
void VmEmitter::put_p(FILE* o, const char* s) {
    for (const char* c = s; *c; ) {
        if (std::strncmp(c, "wasm_", 5) == 0) { fputs(prefix_.c_str(),  o); fputc('_', o); c += 5; }
        else if (std::strncmp(c, "WASM_", 5) == 0) { fputs(uprefix_.c_str(), o); fputc('_', o); c += 5; }
        else fputc(*c++, o);
    }
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
    // operand-stack PRIMITIVES — the GENERATED DEFAULT (the value array, the parallel tag array, and the
    // stack pointer). The GPOP_*/GPUSH_* macros + the entry/return code build on these, so a backend
    // relocates the operand stack by #define-ing them first (the #ifndef then skips the default).
    fputs(
"#ifndef JV_STK\n#define JV_STK  (f->stack)\n#endif\n"
"#ifndef JV_STKT\n#define JV_STKT (f->stack_types)\n#endif\n"
"#ifndef JV_SP\n#define JV_SP   (f->sp)\n#endif\n"
// native calling convention — the leading args every runtime native receives (the vm + its heap).
"#ifndef NATIVE_ARGS\n#define NATIVE_ARGS vm, vm->heap\n#endif\n"
// per-stencil source byte offset: the GENERATED DEFAULT discards it (no trap-frame recording, and
// so _HOLE_pc never appears → no JIT hole). A backend that records a trap site #defines this to bake
// the baked offset into its frame (e.g. f->instr_pc = (u4)(pc)).
"#ifndef STENCIL_TRAP_PC\n#define STENCIL_TRAP_PC(pc)\n#endif\n", o);
    const char* seen[32]; int ns = 0;
    for (auto* td : mod_->types) {
        const char* c = SemLowerer::slot_class(td->ty);
        int dup = 0;
        for (int k = 0; k < ns; k++) { if (!strcmp(seen[k], c)) { dup = 1; break; } }
        if (dup) continue;
        seen[ns++] = c;
        jrow r = low.jtype(td->ty);
        if (r.slots == 1 && r.field == 'r') {
            fprintf(o, "#define GPOP_%s(name) %s name = JV_STK[--JV_SP].%c\n",
                    c, r.scalar, r.field);
            fprintf(o, "#define GPUSH_%s(v) do { JV_STK[JV_SP].l = (s8)(%s)(v);"
                       " JV_STKT[JV_SP] = %s; JV_SP++; } while (0)\n",
                    c, r.scalar, tag_for(r));
        } else if (r.slots == 1) {
            fprintf(o, "#define GPOP_%s(name) %s name = JV_STK[--JV_SP].%c\n",
                    c, r.scalar, r.field);
            fprintf(o, "#define GPUSH_%s(_gv) do { JV_STK[JV_SP].%c = (%s)(_gv);"
                       " JV_STKT[JV_SP] = %s; JV_SP++; } while (0)\n",
                    c, r.field, r.scalar, tag_for(r));
        } else if (r.scalar[0] == 'f') {
            fprintf(o, "#define GPOP_%s(name) %s name; do { JV_SP -= 2;"
                       " union { %s w; %s u; } _x;"
                       " _x.u = ((%s)(%s)JV_STK[JV_SP].%c) | (((%s)(%s)JV_STK[JV_SP+1].%c) << %d);"
                       " name = _x.w; } while (0)\n",
                    c, r.scalar, r.scalar, r.uscalar, r.uscalar, uw, ic, r.uscalar, uw, ic, w);
            fprintf(o, "#define GPUSH_%s(v) do { union { %s w; %s u; } _x; _x.w = (v);"
                       " JV_STK[JV_SP].%c = (%s)(_x.u); JV_STKT[JV_SP] = %s;"
                       " JV_STK[JV_SP+1].%c = (%s)(_x.u >> %d); JV_STKT[JV_SP+1] = T_VOID;"
                       " JV_SP += 2; } while (0)\n",
                    c, r.scalar, r.uscalar, ic, islot, tag_for(r), ic, islot, w);
        } else {
            fprintf(o, "#define GPOP_%s(name) %s name; do { JV_SP -= 2;"
                       " name = ((%s)(%s)JV_STK[JV_SP].%c) | (((%s)JV_STK[JV_SP+1].%c) << %d); } while (0)\n",
                    c, r.scalar, r.scalar, uw, ic, r.scalar, ic, w);
            fprintf(o, "#define GPUSH_%s(v) do { %s _v = (v);"
                       " JV_STK[JV_SP].%c = (%s)(_v); JV_STKT[JV_SP] = %s;"
                       " JV_STK[JV_SP+1].%c = (%s)(_v >> %d); JV_STKT[JV_SP+1] = T_VOID;"
                       " JV_SP += 2; } while (0)\n",
                    c, r.scalar, ic, islot, tag_for(r), ic, islot, w);
        }
    }
    fputs(
"#define GPOP_WORD(name) slot_t name; u1 name##_wt;"
" do { JV_SP--; name = JV_STK[JV_SP]; name##_wt = JV_STKT[JV_SP]; } while (0)\n"
"#define GPUSH_WORD(name) do { JV_STK[JV_SP] = (name);"
" JV_STKT[JV_SP] = name##_wt; JV_SP++; } while (0)\n", o);
    // GPOP_ANY/GPUSH_ANY: the runtime-typed carrier (any_t = {bits, kind}) — a value whose type tag is
    // known only at run time (table/struct/array.get's element type, ref.test's operand). The native packs
    // (value, tag) into one any_t it can return; the push macro unpacks it, so the opcode body stays
    // value-only (it never constructs a slot). Shape follows the slot model: a uniform single-slot backend
    // (i64 in one slot) rides the full 8-byte value in .l; the JVM two-slot model splits long/double.
    if (low.jtype(ValueType::TyI64).slots == 1) {
        fputs(
"#define GPOP_ANY(name) any_t name; do { JV_SP--; name.bits = JV_STK[JV_SP].l; name.kind = JV_STKT[JV_SP]; } while (0)\n"
"#define GPUSH_ANY(v) do { any_t _a = (v); JV_STK[JV_SP].l = _a.bits; JV_STKT[JV_SP] = _a.kind; JV_SP++; } while (0)\n", o);
    } else {
        fputs(
"#define GPOP_ANY(name) any_t name; do { if (JV_STKT[JV_SP-1] == T_VOID) { JV_SP -= 2; name.bits = ((s8)(u4)JV_STK[JV_SP].i) | (((s8)JV_STK[JV_SP+1].i) << 32); name.kind = JV_STKT[JV_SP]; } else { JV_SP -= 1; name.bits = (s8)(u4)JV_STK[JV_SP].i; name.kind = JV_STKT[JV_SP]; } } while (0)\n"
"#define GPUSH_ANY(v) do { any_t _a = (v); if (_a.kind == T_LONG || _a.kind == T_DOUBLE) { JV_STK[JV_SP].i = (s4)_a.bits; JV_STKT[JV_SP] = _a.kind; JV_STK[JV_SP+1].i = (s4)(_a.bits >> 32); JV_STKT[JV_SP+1] = T_VOID; JV_SP += 2; } else { JV_STK[JV_SP].i = (s4)_a.bits; JV_STKT[JV_SP] = _a.kind; JV_SP += 1; } } while (0)\n", o);
    }
    // GPOP_ADDR (WASM memory64/table64): pop one addrtype-width integer as u8 — full width when the
    // top slot is tagged i64, else a zero-extended i32. Pop-only (no GPUSH_ADDR; size/grow push via a
    // stack-driven native). Fields/tag derived from the declared i32/i64 rows (single-slot int model).
    {
        jrow i32r = low.jtype(ValueType::TyI32), i64r = low.jtype(ValueType::TyI64);
        fprintf(o,
"#define GPOP_ADDR(name) u8 name; do { if (JV_STKT[JV_SP-1] == %s) name = (u8)JV_STK[--JV_SP].%c;"
" else name = (u8)(u4)JV_STK[--JV_SP].%c; } while (0)\n",
                tag_for(i64r), i64r.field, i32r.field);
    }
    // globalinst access vocabulary — the GENERATED DEFAULT (a backend may #define these first to point
    // the access at a different shape; the #ifndef then skips the default). GET reads, SET writes the
    // by-reference slot, TAG_SET records the runtime type tag (whole-slot moves + GC roots).
    fputs(
"#ifndef GLOBAL_GET\n#define GLOBAL_GET(i)        (vm->globals[(i)][0])\n#endif\n"
"#ifndef GLOBAL_SET\n#define GLOBAL_SET(i, v)     (vm->globals[(i)][0] = (v))\n#endif\n"
"#ifndef GLOBAL_TAG_SET\n#define GLOBAL_TAG_SET(i, t) (vm->global_types[(i)] = (t))\n#endif\n", o);
    // localinst access vocabulary — the GENERATED DEFAULT (LOCAL_SLOT yields the slot lvalue; a backend
    // may #define these first to relocate the locals storage). The value-model field selection stays in
    // the lowering; LOCAL_TAG_SET records the runtime type tag.
    fputs(
"#ifndef LOCAL_SLOT\n#define LOCAL_SLOT(i)        (f->locals[(i)])\n#endif\n"
"#ifndef LOCAL_TAG_SET\n#define LOCAL_TAG_SET(i, t)  (f->local_types[(i)] = (t))\n#endif\n", o);
    fputs("\n", o);
}

void VmEmitter::emit_prologue(SemLowerer& low) {
    FILE* o = low.out();
    fputs(
"#include \"gen_interp.h\"\n"
"#include \"opcodes.h\"\n"
"\n"
"/* Threaded dispatch: each handler does its work and musttail-jumps to the next\n"
" * opcode continuation or, on a guard, to the trap continuation —\n"
" * the same continuation shape as the JIT stencils. Both are supplied by the\n"
" * runtime backend. */\n"
"#define TAIL __attribute__((musttail))\n", o);
    fprintf(o, "void %s_next(vm_t* vm);\nvoid %s_trap(vm_t* vm);\n", prefix_.c_str(), prefix_.c_str());
    fputs(
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
        fputs("    STENCIL_TRAP_PC(_HOLE_pc);   /* per-stencil source byte offset: the default discards it (no trap-frame); a backend that records a trap site (e.g. §7.1.8) #defines STENCIL_TRAP_PC to bake _HOLE_pc into its frame */\n", o);
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
        if (is_tail_operand(od->ty)) {
            // br_table/try_table: their native reads the tail (and transfers / advances into the
            // body), so emit nothing. select_t FALLS THROUGH with no native reading its
            // vec(valtype), so the INTERP must skip it to advance the cursor (the JIT walk skips
            // it via meta.tail). A valtype is one byte, or 0x63/0x64 + a heaptype for a concrete ref.
            if (!stencil && od->ty == ValueType::TySelectVec) {
                fputs("    { u4 _sv_n = 0; (void)bbq_read_uleb128_u32(&f->code, &_sv_n);\n"
                      "      for (u4 _sv_i = 0; _sv_i < _sv_n; _sv_i++) { u1 _sv_vt = 0; (void)bbq_read_u8(&f->code, &_sv_vt);\n"
                      "        if (_sv_vt == 0x63 || _sv_vt == 0x64) { s4 _sv_ht = 0; (void)bbq_read_sleb128_i32(&f->code, &_sv_ht); } } }\n", o);
            }
            continue;
        }
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
        // §5.3.3 blocktype: the (ref null? ht) forms 0x63/0x64 (sleb −29/−28) carry a TRAILING
        // heaptype — the interp must consume it to advance the cursor (the JIT walk does so by
        // JOP_BLOCKTYPE). Without this it was mis-read as the next opcode.
        if (!stencil && oty == ValueType::TyBlockType)
            fprintf(o, "    if (%s == -29 || %s == -28) { s4 _bt_ht = 0; (void)bbq_read_sleb128_i32(&f->code, &_bt_ht); }\n",
                    od->name, od->name);
    }

    for (int k = (int)op->stack_in.size() - 1; k >= 0; k--) {
        const StackParam* si = op->stack_in[k];
        const SemExpr* vc = out_count(si);
        if (vc) {
            // §3b VARIADIC: compute the pop count + capture the operand slice IN PLACE; do NOT pop yet —
            // the operands stay on the value stack (GC roots) for the body to read as `name[i]`, and opgen
            // drops them (JV_SP -= count) AFTER the body, before the result push. The count is a lowered
            // DSL expression (a native call gets its _HOLE_ in stencil mode; immediates resolve directly).
            fprintf(o, "    s4 _vc_%s = (s4)(", si->name);
            low.lower_expr(vc, op, ValueType::TyI32);
            fputs(");\n", o);
            fprintf(o, "    slot_t* _vb_%s = &JV_STK[JV_SP - _vc_%s]; u1* _vt_%s = &JV_STKT[JV_SP - _vc_%s];"
                       " (void)_vb_%s; (void)_vt_%s;\n",
                    si->name, si->name, si->name, si->name, si->name, si->name);
        } else if (stack_in_live(op, k)) {
            fprintf(o, "    GPOP_%s(%s);\n", SemLowerer::slot_class(si->ty), si->name);
        } else {
            fprintf(o, "    JV_SP -= %d;\n", low.jtype(si->ty).slots);
        }
    }

    emit_guards(low, op);

    for (auto* so : op->stack_out)
        if (!out_sem_expr(so)) {
            if (so->ty == ValueType::TyWord)
                fprintf(o, "    slot_t %s; u1 %s_wt;\n", so->name, so->name);
            else
                fprintf(o, "    %s %s;\n", low.c_scalar(so->ty), so->name);
        }

    low.clear_native_called();   // track whether the body calls a native (which may trap) → stencil trap-check below
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

    // §3b variadic: now consume the operands the body read in-place (sp was held up so they stayed rooted).
    for (auto* si : op->stack_in)
        if (out_count(si)) fprintf(o, "    JV_SP -= _vc_%s;\n", si->name);

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
        if (stencil) {
            // The JIT has no implicit per-op trap check (unlike the interp's next-dispatch, which sees
            // the trapping native's code.pos = code.length and ends). A native that set vm->trapped
            // must bail HERE, or the stamped chain runs on into the next stencil — corrupting memory
            // (a store after a trapped load) or underflowing the stack (a value-less trap like
            // table.get followed by a consumer). Status/control natives already bail via _HOLE_resync.
            if (low.native_called())
                fputs("    if (vm->trapped) TAIL return _HOLE_trap(vm);\n", o);
            fputs("    TAIL return _HOLE_cont(vm);\n", o);
        } else {
            put_p(o, "    TAIL return wasm_next(vm);\n");
        }
    }
    fputs("}\n\n", o);
}

// ── gen_interp.c ────────────────────────────────────────────

void VmEmitter::emit_interp_c(FILE* o) {
    SemLowerer low(mod_, o, Mode::Interp, prefix_);
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
    SemLowerer low(mod_, o, Mode::Stencil, prefix_);
    fputs("/* AUTO-GENERATED by opgen — jitterator stencils. Source: opcodes.def */\n", o);
    fputs("#include \"runtime_api.h\"\n\n", o);
    fputs("#define STENCIL __attribute__((preserve_none))\n"
          "#define TAIL    __attribute__((musttail))\n\n", o);
    emit_slot_macros(low);
    fputs("extern void STENCIL _HOLE_cont(vm_t* vm);\n"
          "extern void STENCIL _HOLE_trap(vm_t* vm);\n"
          "extern void STENCIL _HOLE_resync(vm_t* vm);  /* the in-buffer IP-dispatch stencil */\n"
          "extern uint64_t _HOLE_ip;                    /* a control op's own post-operand position */\n"
          "extern uint64_t _HOLE_pc;                    /* this stencil's source byte offset (§7.1.8 trap-frame offset) */\n", o);

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
"    if (JV_SP > 0) { vm->result = JV_STK[JV_SP - 1]; vm->result_type = JV_STKT[JV_SP - 1]; }\n"
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
    put_p(o, "#ifndef WASM_JIT_SYMBOLS_H\n#define WASM_JIT_SYMBOLS_H\n");
    fputs("#include \"runtime_api.h\"   /* the native prototypes */\n", o);
    fputs("#include <math.h>           /* the libm intrinsic prototypes */\n\n", o);
    put_p(o, "typedef struct { const char* name; void* addr; } wasm_jit_sym_t;\n\n");
    put_p(o, "static const wasm_jit_sym_t wasm_jit_symbols[] = {\n");
    for (auto* md : mod_->methods) {
        if (!md->native || md->inl) continue;   // inline natives are direct calls (folded static-inline), not _HOLE_ patch points
        fprintf(o, "    { \"_HOLE_%s\", (void*)%s },\n", md->name, md->name);
    }
    for (int i = 0; g_libm_syms[i]; i++)
        fprintf(o, "    { \"_HOLE_%s\", (void*)%s },\n", g_libm_syms[i], g_libm_syms[i]);
    fputs("};\n", o);
    put_p(o, "static const int wasm_jit_symbols_count =\n"
          "    (int)(sizeof(wasm_jit_symbols) / sizeof(wasm_jit_symbols[0]));\n\n");
    put_p(o, "#endif /* WASM_JIT_SYMBOLS_H */\n");
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
    case ValueType::TyBlockType: return "JOP_BLOCKTYPE";
    default:                  return "JOP_NONE";
    }
}

// ── wasm_jit_meta.h ─────────────────────────────────────────

void VmEmitter::emit_jit_meta(FILE* o) {
    SemLowerer low(mod_, o, Mode::Interp, prefix_);
    put_p(o, "/* AUTO-GENERATED by opgen — do not edit. JIT opcode metadata. */\n"
          "#ifndef WASM_JIT_META_H\n#define WASM_JIT_META_H\n"
          "#include \"opcodes.h\"\n#include \"wasm_stencil_table.h\"\n\n"
          "typedef enum { JOP_NONE, JOP_ULEB32, JOP_ULEB64, JOP_SLEB32, JOP_SLEB64,\n"
          "               JOP_F32, JOP_F64, JOP_U8, JOP_MEMARG, JOP_BLOCKTYPE, JOP_CONST } jit_operand_kind_t;\n"
          "/* A variable-length trailing immediate the op's native reads at runtime; the JIT\n"
          " * compile-walk skips it (by kind) after capturing _HOLE_ip, to place the next stencil. */\n"
          "typedef enum { JTAIL_NONE, JTAIL_BRTABLE, JTAIL_TRYTABLE, JTAIL_SELECTVEC } jit_tail_kind_t;\n"
          "typedef struct { const char* hole; jit_operand_kind_t kind; uint64_t value; } jit_operand_t;\n"
          "typedef struct { int stencil; const jit_operand_t* operands;\n"
          "                 unsigned char operand_count, pop, push, tail; } wasm_jit_meta_t;\n\n");

    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op)) continue;
        const char* cn[8]; uint64_t cv[8];
        int nc = low.op_const_holes(op, cn, cv);
        if (n_fixed_operands(op) == 0 && nc == 0) continue;   // tail-only ops have no fixed operand array
        fprintf(o, "static const jit_operand_t jitops_%s[] = {", op->mnemonic);
        for (auto* od : op->operands) {
            if (is_tail_operand(od->ty)) continue;            // the tail isn't a fixed JIT operand
            fprintf(o, " {\"_HOLE_%s\", %s, 0},", od->name, jop_kind(od->ty));
        }
        for (int k = 0; k < nc; k++)
            fprintf(o, " {\"%s\", JOP_CONST, 0x%llxULL},", cn[k], (unsigned long long)cv[k]);
        fputs(" };\n", o);
    }

    auto emit_meta_row = [&](const Opcode* op) {
        const char* cn[8]; uint64_t cv[8];
        int pop = (int)op->stack_in.size(), push = (int)op->stack_out.size();
        int nops = n_fixed_operands(op) + low.op_const_holes(op, cn, cv);
        const char* tail = tail_kind_name(op);
        if (SemLowerer::stencil_excluded(op)) fputs("-1", o);
        else { fputs("STENCIL_GEN_ST_", o); put_upper(o, op->mnemonic); }
        if (nops) fprintf(o, ", jitops_%s, %d, %d, %d, %s },\n", op->mnemonic, nops, pop, push, tail);
        else      fprintf(o, ", NULL, 0, %d, %d, %s },\n", pop, push, tail);
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
        fprintf(o, "\nstatic const %s_jit_meta_t %s_jit_meta_sub_%02x[%d] = {\n", prefix_.c_str(), prefix_.c_str(), pv, maxsub + 1);
        for (auto* op : mod_->opcodes) {
            if (!is_emittable(op) || op->subop < 0 || op->opcode_val != pv) continue;
            fprintf(o, "    [%d] = { ", op->subop); emit_meta_row(op);
        }
        fputs("};\n", o);
    }

    put_p(o, "\nstatic const wasm_jit_meta_t wasm_jit_meta[256] = {\n");
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || op->subop >= 0) continue;
        fputs("    [OP_", o); put_upper(o, op->mnemonic); fputs("] = { ", o);
        emit_meta_row(op);
    }
    fputs("};\n", o);

    put_p(o, "\nstatic const wasm_jit_meta_t* const wasm_jit_meta_sub[256] = {\n");
    for (int p = 0; p < npfx; p++)
        fprintf(o, "    [0x%02x] = %s_jit_meta_sub_%02x,\n", prefixes[p], prefix_.c_str(), prefixes[p]);
    put_p(o, "};\n#endif /* WASM_JIT_META_H */\n");
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
    case ValueType::TyRef:
    case ValueType::TyFuncRef:
    case ValueType::TyExternRef:
    case ValueType::TyI31Ref:    return "WVT_REF";   /* one opaque ref tag; the heaptype rides a parallel column */
    case ValueType::TyAddr:      return "WVT_I32";   /* addrtype: i32/i64 is decided by the hand-written tc_mem; this generated sig is unused for memory/table ops */
    default:                     return "WVT_BOT";
    }
}

/* The abstract heaptype a ref operand carries (spec data transcribed from the opcode
 * signature), forwarded opaquely as a symbol the consumer's subtype header defines.
 * Returns nullptr for non-reference operands (no heaptype column entry). */
const char* VmEmitter::ht_name(ValueType t) {
    switch (t) {
    case ValueType::TyFuncRef:   return "HT_FUNC";
    case ValueType::TyExternRef: return "HT_EXTERN";
    case ValueType::TyI31Ref:    return "HT_I31";
    case ValueType::TyRef:       return "HT_ANY";    /* bare ref: the generic top of the any hierarchy */
    default:                     return nullptr;
    }
}

// ── wasm_valtype.h ──────────────────────────────────────────

void VmEmitter::emit_valtype_h(FILE* o) {
    put_p(o, "/* AUTO-GENERATED by opgen — do not edit. WASM value-type tags. */\n"
          "#ifndef WASM_VALTYPE_H\n#define WASM_VALTYPE_H\n"
          "#include <stdint.h>\n\n"
          "typedef enum { WVT_BOT=0, WVT_I32, WVT_I64, WVT_F32, WVT_F64, WVT_V128,\n"
          "               WVT_REF, WVT_REF_NN   /* the only reference tags: a generic (ref null? heaptype).\n"
          "                                        WVT_REF is nullable, WVT_REF_NN non-null; the heaptype\n"
          "                                        (an HT_* abstract code OR a concrete typeidx) rides on a\n"
          "                                        parallel array — this tool carries no reference hierarchy. */\n"
          "             } wasm_valtype_t;\n"
          "/* Per-opcode §7.6 transfer signature. A ref operand's heaptype lives in the parallel\n"
          " * pop_ht/push_ht columns (HT_* code per slot, ignored for non-refs); the column pointer\n"
          " * is NULL when the op has no reference operands. */\n"
          "typedef struct { const wasm_valtype_t* pops; uint8_t npop;\n"
          "                 const wasm_valtype_t* pushes; uint8_t npush;\n"
          "                 const int32_t* pop_ht; const int32_t* push_ht;\n"
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
          "#endif /* WASM_VALTYPE_H */\n");
}

// ── wasm_type_meta.h ────────────────────────────────────────

void VmEmitter::emit_type_meta(FILE* o) {
    put_p(o, "/* AUTO-GENERATED by opgen — do not edit. §7.6 validator type signatures. */\n"
          "#ifndef WASM_TYPE_META_H\n#define WASM_TYPE_META_H\n"
          "#include \"opcodes.h\"\n#include \"wasm_valtype.h\"\n");
    // The heaptype column emits HT_* symbols; pull in the consumer's subtype header so
    // the generated table is self-contained — but only when reference operands exist.
    bool any_ref = false;
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op)) continue;
        for (auto* s : op->stack_in)  if (ht_name(s->ty)) any_ref = true;
        for (auto* s : op->stack_out) if (ht_name(s->ty)) any_ref = true;
    }
    if (any_ref) fprintf(o, "#include \"%s_subtype.h\"  /* HT_* heaptype codes for the ref columns */\n", prefix_.c_str());
    fputs("\n", o);

    auto stack_has_ref = [&](const std::vector<StackParam*>& v) {
        for (auto* s : v) if (ht_name(s->ty)) return true;
        return false;
    };
    auto emit_ht_array = [&](const char* tag, const Opcode* op, const std::vector<StackParam*>& v) {
        fprintf(o, "static const int32_t %s_%s_ht[] = {", tag, op->mnemonic);
        for (auto* s : v) { const char* h = ht_name(s->ty); fprintf(o, " %s,", h ? h : "0"); }
        fputs(" };\n", o);
    };

    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op)) continue;
        if (!op->stack_in.empty()) {
            fprintf(o, "static const %s_valtype_t sigp_%s[] = {", prefix_.c_str(), op->mnemonic);
            for (auto* s : op->stack_in) fprintf(o, " %s,", wvt_name(s->ty));
            fputs(" };\n", o);
            if (stack_has_ref(op->stack_in)) emit_ht_array("sigp", op, op->stack_in);
        }
        if (!op->stack_out.empty()) {
            fprintf(o, "static const %s_valtype_t sigr_%s[] = {", prefix_.c_str(), op->mnemonic);
            for (auto* s : op->stack_out) fprintf(o, " %s,", wvt_name(s->ty));
            fputs(" };\n", o);
            if (stack_has_ref(op->stack_out)) emit_ht_array("sigr", op, op->stack_out);
        }
    }

    auto emit_sig_row = [&](const Opcode* op) {
        if (!op->stack_in.empty())  fprintf(o, "sigp_%s, %d, ", op->mnemonic, (int)op->stack_in.size());
        else fputs("NULL, 0, ", o);
        if (!op->stack_out.empty()) fprintf(o, "sigr_%s, %d, ", op->mnemonic, (int)op->stack_out.size());
        else fputs("NULL, 0, ", o);
        if (stack_has_ref(op->stack_in)) fprintf(o, "sigp_%s_ht, ", op->mnemonic); else fputs("NULL, ", o);
        if (stack_has_ref(op->stack_out)) fprintf(o, "sigr_%s_ht, 1 },\n", op->mnemonic); else fputs("NULL, 1 },\n", o);
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
        fprintf(o, "\nstatic const %s_opsig_t %s_opsig_sub_%02x[%d] = {\n", prefix_.c_str(), prefix_.c_str(), pv, maxsub + 1);
        for (auto* op : mod_->opcodes) {
            if (!is_emittable(op) || op->subop < 0 || op->opcode_val != pv) continue;
            fprintf(o, "    [%d] = { ", op->subop); emit_sig_row(op);
        }
        fputs("};\n", o);
    }

    put_p(o, "\nstatic const wasm_opsig_t wasm_opsig[256] = {\n");
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || op->subop >= 0) continue;
        fputs("    [OP_", o); put_upper(o, op->mnemonic); fputs("] = { ", o);
        emit_sig_row(op);
    }
    fputs("};\n", o);

    put_p(o, "\nstatic const wasm_opsig_t* const wasm_opsig_sub[256] = {\n");
    for (int p = 0; p < npfx; p++)
        fprintf(o, "    [0x%02x] = %s_opsig_sub_%02x,\n", prefixes[p], prefix_.c_str(), prefixes[p]);
    put_p(o, "};\n#endif /* WASM_TYPE_META_H */\n");
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

    fprintf(o, "typedef enum { %s = 0, %s = 1, %s = 2",
            low.status_ok(), low.status_err(), low.status_halt());
    if (mod_->status.has_value()) {
        int idx = 3;
        for (auto* e : (*mod_->status)->extra) fprintf(o, ", %s = %d", e, idx++);
    }
    fprintf(o, " } %s;\n", low.status_type());
}

void VmEmitter::emit_runtime_api_h(FILE* o) {
    SemLowerer low(mod_, o, Mode::Interp, prefix_);
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
    /* ── Value-model OPERATIONS exposed to the backend's natives ──────────────────────────────
     * The generated handlers push/pop through the GPOP_ / GPUSH_ macros (in the handler files); a
     * native that does its own stack work used to hand-roll `f->stack[f->sp].field = v;
     * f->stack_types[i] = tag; f->sp++`, re-implementing the value model the generator owns. These
     * macros expose the SAME model (slot field + parallel tag) to a native holding a `frame_t* f`, so
     * the value model has one owner. Emitted from the i32/i64 jrows — no hand-coded fields/tags. */
    {
        jrow i32r = low.jtype(ValueType::TyI32), i64r = low.jtype(ValueType::TyI64);
        // JV_NPOP_ADDR: pop one addrtype-width integer as u8 — full width when the top slot is tagged
        // i64, else a zero-extended i32 (the native analogue of GPOP_ADDR; memory64/table64 indices).
        fprintf(o,
"\n#define JV_NPOP_ADDR(f, name) u8 name; do { if ((f)->stack_types[(f)->sp-1] == %s)"
" name = (u8)(f)->stack[--(f)->sp].%c; else name = (u8)(u4)(f)->stack[--(f)->sp].%c; } while (0)\n",
                tag_for(i64r), i64r.field, i32r.field);
        // JV_NPUSH_ADDR: push one addrtype-width result (is64-chosen; memory/table size/grow).
        fprintf(o,
"#define JV_NPUSH_ADDR(f, val, is64) do { if (is64) { (f)->stack[(f)->sp].%c = (%s)(val);"
" (f)->stack_types[(f)->sp] = %s; } else { (f)->stack[(f)->sp].%c = (%s)(val);"
" (f)->stack_types[(f)->sp] = %s; } (f)->sp++; } while (0)\n",
                i64r.field, i64r.scalar, tag_for(i64r), i32r.field, i32r.scalar, tag_for(i32r));
    }
    // Generic tagged push/pop: the value-model PRIMITIVE (whole slot + parallel tag, sp). Tag-agnostic —
    // a backend layers its domain conveniences (a managed-ref push, a scalar-ref push) on top of these,
    // supplying its own tag. The natives stop hand-rolling f->stack[f->sp] = …; f->stack_types[…] = ….
    fputs(
"#define JV_NPUSH(f, slotval, tag) do { (f)->stack[(f)->sp] = (slotval); (f)->stack_types[(f)->sp] = (tag); (f)->sp++; } while (0)\n"
"#define JV_NPOP(f, sv, tv) slot_t sv; u1 tv; do { (f)->sp--; sv = (f)->stack[(f)->sp]; tv = (f)->stack_types[(f)->sp]; } while (0)\n", o);
    fputs(
"\n/* ── Runtime-service operations (a backend defines all of these). Every op\n"
" * takes the full runtime context (vm = execution state, h = object memory)\n"
" * and uses whichever half it needs; control-transfer ops return a status. ── */\n",
    o);

    for (auto* md : mod_->methods) {
        if (!md->native || md->inl) continue;   // inline natives are static-inline in a backend header, not extern prototypes
        fprintf(o, "%-13s %s(%s", low.api_c_type(md->ret_ty), md->name,
                "vm_t*, heap_t*");
        for (auto* p : md->params)
            fprintf(o, ", %s", low.api_c_type(p->ty));
        fputs(");\n", o);
    }

    fputs(
"\n/* Synthesized-guard throws: opgen emits calls to these from the contract\n"
" * facts; a backend creates + raises the exception. */\n", o);
    static const char* throws[] = { "throw_div_by_zero" };
    for (size_t i = 0; i < sizeof(throws) / sizeof(throws[0]); i++)
        fprintf(o, "%s %s(vm_t*, heap_t*);\n", low.status_type(), throws[i]);

    fputs("\n#endif /* OPGEN_RUNTIME_API_H */\n", o);
}

} // namespace opgen
