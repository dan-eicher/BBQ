// The VM/JIT/validator emitters (gen_interp.c, wasm_stencils.c, the jit/type
// metadata + headers + runtime_api.h), ported from emit_interp_c.cpp. Every
// emitted byte matches the reference VM; g_mod/g_mode/g_lane are gone — the
// Module is a member, the mode/lane live in the SemLowerer.
#include "vmemit.h"
#include "sigemit.h"  /* sclass_cacheable — which classes can ride a cache slot */
#include "spec.h"     /* Spec::forwarded_name — which stack slots the spec says are one value */
#include <cstring>
#include <cctype>
#include <algorithm>   /* std::sort — the jit symbol table is emitted ordered */

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
/* Does the body name a tail operand's `<name>_count`? Only then is it decoded and
 * (in the stencil) given a hole — an unused count would be a wasted patch point. */
static int op_body_refs_count(const Opcode* op, const char* name) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s_count", name);
    for (auto* s : op->sem_body) if (VmEmitter::stmt_refs_name(s, buf)) return 1;
    return 0;
}

static int n_fixed_operands(const Opcode* op) {
    int n = 0;
    for (auto* od : op->operands) {
        if (!is_tail_operand(od->ty)) { n++; continue; }
        // br_table's count is tail-shaped but IS a fixed JIT operand (see the meta emitter)
        if (od->ty == ValueType::TyBrTable && op_body_refs_count(op, od->name)) n++;
    }
    return n;
}

const char* VmEmitter::out_sem_expr(const StackParam* sp) {
    return sp->sem_expr.has_value() ? *sp->sem_expr : nullptr;
}
const SemExpr* VmEmitter::out_count(const StackParam* sp) {   // §3b variadic: the pop-count expr, or null
    return sp->count.has_value() ? *sp->count : nullptr;
}

// ── Guard emission ──────────────────────────────────────────
//
// One `error:` declaration, one emitted guard, in declaration order. The
// condition is the predicate the spec author wrote — opgen does not infer a
// guard from the body's shape, so a guard is expressible for any condition the
// action language can state, not just the two shapes a matcher knew how to spot.

void VmEmitter::emit_guards(SemLowerer& low, const Opcode* op) {
    FILE* o = low.out();
    for (auto* e : op->errors) {
        if (!e->condition.has_value()) continue;   // a bare `error: Name` declares no guard
        const SemExpr* cond = *e->condition;
        // The `(_Bool)` is load-bearing, not decoration: lower_expr already wraps a
        // binary operator in its own parens, so a bare `if (` + expr + `)` emits
        // `if ((b == 0))` — which clang flags as an extraneous-parenthesised equality
        // ("use '=' to turn this into an assignment"). The cast separates the two paren
        // levels and states the intent: a guard condition is a predicate.
        fputs("    if ((_Bool)", o);
        low.set_in_guard(true);
        low.lower_expr(cond, op, SemLowerer::cond_type(op, cond));
        low.set_in_guard(false);
        fputc(')', o);
        low.guard_trap(e->error_name);
    }
}

// ── Name-occurrence + liveness ──────────────────────────────

int VmEmitter::stmt_refs_name(const SemStmt* s, const char* name) {
    if (!s) return 0;
    switch (s->tag) {
    case SemStmtTag::SAssign: {
        auto* a = static_cast<const SAssign*>(s);
        return SemLowerer::expr_refs_name(a->target, name) || SemLowerer::expr_refs_name(a->value, name);
    }
    case SemStmtTag::SExprStmt: return SemLowerer::expr_refs_name(static_cast<const SExprStmt*>(s)->value, name);
    case SemStmtTag::SLocalDecl: {
        auto* d = static_cast<const SLocalDecl*>(s);
        return d->init.has_value() ? SemLowerer::expr_refs_name(*d->init, name) : 0;
    }
    case SemStmtTag::SIf: {
        auto* sif = static_cast<const SIf*>(s);
        return SemLowerer::expr_refs_name(sif->cond, name) || stmt_refs_name(sif->then_, name) ||
               (sif->else_.has_value() && stmt_refs_name(*sif->else_, name));
    }
    case SemStmtTag::SWhile: {
        auto* w = static_cast<const SWhile*>(s);
        return SemLowerer::expr_refs_name(w->cond, name) || stmt_refs_name(w->body, name);
    }
    case SemStmtTag::SFor: {
        auto* f = static_cast<const SFor*>(s);
        return (f->init.has_value() && stmt_refs_name(*f->init, name)) ||
               (f->cond.has_value() && SemLowerer::expr_refs_name(*f->cond, name)) ||
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
    for (auto* so : op->stack_out)
        if (Spec::forwarded_name(so) == name) return 1;   // `-- word r = "<name>"`
    // A guard reads its operands too. Without this an input named only by an
    // `error:` condition would be dropped with a bare `JV_SP -=` instead of
    // popped into a variable, and the emitted guard would not compile.
    for (auto* e : op->errors)
        if (e->condition.has_value() && SemLowerer::expr_refs_name(*e->condition, name)) return 1;
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
"#ifndef STENCIL_TRAP_PC\n#define STENCIL_TRAP_PC(pc)\n#endif\n"
// which declared `error:` fired, as an OPGEN_ERR_* code, immediately before the tail to the trap
// continuation. The GENERATED DEFAULT discards it — a backend with one undifferentiated trap needs
// nothing. A backend that reports distinct trap causes (wasm separates "integer divide by zero" from
// "integer overflow") #defines this to record the code before the trap unwinds.
"#ifndef OPGEN_GUARD_TRAP\n#define OPGEN_GUARD_TRAP(vm, err) ((void)(err))\n#endif\n", o);
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
    // GPOP_ANY/GPUSH_ANY: the runtime-typed carrier (any_t = {bits, hi, kind}) — a value whose type tag
    // is known only at run time (table/struct/array.get's element type, ref.test's operand). The native
    // packs (value, tag) into one any_t it can return; the push macro unpacks it, so the opcode body stays
    // value-only (it never constructs a slot). Shape follows the slot model: a uniform single-slot backend
    // (i64 in one slot) rides the full 8-byte value in .l; the JVM two-slot model splits long/double.
    // A spec that declares v128 gets BOTH halves copied unconditionally (bits + hi = the slot's two
    // 64-bit lanes) so a T_V128 value never truncates crossing the carrier — the 8-byte-only variant of
    // these macros silently dropped lanes 2/3 of every v128 struct/array field (found by the javelina
    // compiler-driven parity battery; the official suite has no GC×SIMD coverage).
    char any_vf = low.v128_field();   /* the spec's own slot-member letter for v128, or 0 */
    if (low.jtype(ValueType::TyI64).slots == 1) {
        if (any_vf)
            fprintf(o,
"#define GPOP_ANY(name) any_t name; do { JV_SP--; name.bits = JV_STK[JV_SP].%c.i64[0]; name.hi = JV_STK[JV_SP].%c.i64[1]; name.kind = JV_STKT[JV_SP]; } while (0)\n"
"#define GPUSH_ANY(_av) do { any_t _a = (_av); JV_STK[JV_SP].%c.i64[0] = _a.bits; JV_STK[JV_SP].%c.i64[1] = _a.hi; JV_STKT[JV_SP] = _a.kind; JV_SP++; } while (0)\n",
                any_vf, any_vf, any_vf, any_vf);
        else
            fputs(
"#define GPOP_ANY(name) any_t name; do { JV_SP--; name.bits = JV_STK[JV_SP].l; name.kind = JV_STKT[JV_SP]; } while (0)\n"
"#define GPUSH_ANY(v) do { any_t _a = (v); JV_STK[JV_SP].l = _a.bits; JV_STKT[JV_SP] = _a.kind; JV_SP++; } while (0)\n", o);
    } else {
        fputs(
"#define GPOP_ANY(name) any_t name; do { if (JV_STKT[JV_SP-1] == T_VOID) { JV_SP -= 2; name.bits = ((s8)(u4)JV_STK[JV_SP].i) | (((s8)JV_STK[JV_SP+1].i) << 32); name.kind = JV_STKT[JV_SP]; } else { JV_SP -= 1; name.bits = (s8)(u4)JV_STK[JV_SP].i; name.kind = JV_STKT[JV_SP]; } } while (0)\n"
"#define GPUSH_ANY(v) do { any_t _a = (v); if (_a.kind == T_LONG || _a.kind == T_DOUBLE) { JV_STK[JV_SP].i = (s4)_a.bits; JV_STKT[JV_SP] = _a.kind; JV_STK[JV_SP+1].i = (s4)(_a.bits >> 32); JV_STKT[JV_SP+1] = T_VOID; JV_SP += 2; } else { JV_STK[JV_SP].i = (s4)_a.bits; JV_STKT[JV_SP] = _a.kind; JV_SP += 1; } } while (0)\n", o);
    }
    // GPOP_ADDR (WASM memory64/table64): pop one addrtype-width integer as u8. `is64` is the DECLARED
    // addrtype of the module entry the operand addresses, supplied by the signature's `addr(expr)`
    // source — NOT the operand's runtime value tag.
    //
    // WASM §2.3.11 makes addrtype ::= i32 | i64, and §2.3.15/§2.3.16 make it part of each memory's /
    // table's declared type. Every typing rule therefore reads it from the module
    // (`C.mems[x] = at lim page => C |- nt.load x memarg : at -> nt`, §3.4.5), and §4.6.8 executes it
    // as "ASSERT: Due to validation ... Pop the value (at.const i)". Asserted, never tested.
    //
    // Testing the tag instead was a divergence in MECHANISM: a tested fact can disagree with the
    // declared type, an asserted one cannot. It did disagree — an i64 address read out of a GC
    // aggregate carries that aggregate's reconstructed tag, so the width collapsed to 32 bits and an
    // out-of-bounds memory64 access silently read a different in-bounds location instead of trapping.
    // Pop-only (no GPUSH_ADDR; size/grow push via a stack-driven native).
    {
        jrow i32r = low.jtype(ValueType::TyI32), i64r = low.jtype(ValueType::TyI64);
        fprintf(o,
"#define GPOP_ADDR(name, is64) u8 name; do { if (is64) name = (u8)JV_STK[--JV_SP].%c;"
" else name = (u8)(u4)JV_STK[--JV_SP].%c; } while (0)\n",
                i64r.field, i32r.field);
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

// A managed reference is the one class that never rides a slot. Stack caching
// removes push/pop traffic on computational temporaries; a GC reference is not
// one — it belongs on the operand stack, which is where the collector looks for
// roots and, because Immix evacuates, where it rewrites them. A register has no
// address to rewrite.
//
// Asked of the STORAGE CLASS, through the one routine that assigns it. The
// declared ValueType is the wrong vocabulary for the question: the SIMD lane
// views (`i32x4`, `f64x2`, …) are all held in a v128, and only classify_final
// knows that. The grammar resolves through the same routine, so asking here in
// any other way is a second copy of one mapping.
bool VmEmitter::cacheable(ValueType t) {
    return sclass_cacheable(SigEmitter::classify_final(t, "vmemit", "cache slot"));
}

// In state k the top k operands are cached, so operand `k` of an arity-`a`
// instruction is cached iff its distance from the top is under the state.
// How many slots a value occupies. A v128 is twice a register wide, so it takes
// two — which is why the state counts SLOTS and not items (§2.3's organization is
// over the cache's capacity, and the plan's measurement puts a v128 at 111 -> 47
// bytes across two integer slots, better than a native vector slot at 51).
//
// A `word`/`any` slot answers 1 even though the value it carries could be a v128.
// The width has to be known HERE, at emission, because the register file is a
// fixed shape in every stencil signature — musttail rejects a per-state parameter
// list, which is the constraint that made the signature fixed in the first place.
// A poly slot's width is a property of the tile, not of the opcode, so it cannot
// be known here. The consequence, stated rather than discovered later: a
// `local.get` resolving to v128 is never cached; it reduces at `v128_mem` like
// any other class with no register form. An opcode whose slot classifies as V128
// takes two — which is every SIMD opcode, the lane views included, because the
// class is what the width is a property of.
int VmEmitter::slot_width(ValueType t) {
    return sclass_width(SigEmitter::classify_final(t, "vmemit", "cache slot"));
}

// Where operand k sits, counted in SLOTS from the top: everything above it has
// already spent its own width. Cached only if the whole value fits inside the
// state — half a v128 in a register is not a value.
int VmEmitter::operand_slot(const Opcode* op, int k, int state) {
    int a = (int)op->stack_in.size();
    int base = 0;
    for (int j = a - 1; j > k; j--) base += slot_width(op->stack_in[j]->ty);
    return (base + slot_width(op->stack_in[k]->ty) <= state) ? base : -1;
}

// Do this state's results ride the cache? All of them or none: a pushed result
// sitting above a cached one is not a state the minimal organization can name
// (§2.3), since a state is a COUNT of cached items and says the top ones are the
// cached ones. Three ways to fail it, and each is a real opcode:
//   - the result would not fit alongside what survived (Ertl's OVERFLOW),
//   - it is variadic, and already sits at the frame base where a callee left it,
//   - its class is one no slot holds (ref) — but `word`/`any` pass: CACHE_PUT
//     needs no class because a scalar rides `bits` whole, and the instances
//     wider than that (v128) or unslottable (ref) never reduce at a
//     cached-result rule (the grammar's pw fence / jav_class_cacheable).
// This is the ONE place the question is answered, because the answer is what the
// tiling grammar prices and what the stitcher has to stamp against.
// Slots, not items: everything below counts capacity, and a v128 spends two of it.
int VmEmitter::operand_slots(const Opcode* op) const {
    int n = 0;
    for (auto* si : op->stack_in) n += slot_width(si->ty);
    return n;
}
int VmEmitter::result_slots(const Opcode* op) const {
    int n = 0;
    for (auto* so : op->stack_out) n += slot_width(so->ty);
    return n;
}

bool VmEmitter::results_cached(const Opcode* op, int state) const {
    if (state < 0 || op->stack_out.empty()) return false;
    int a = operand_slots(op);
    int left = state > a ? state - a : 0;
    if (left + result_slots(op) > tier2_n_) return false;
    for (auto* so : op->stack_out)
        if (out_count(so) || !(cacheable(so->ty) || poly_slot(so->ty))) return false;
    return true;
}

// A `word`/`any` slot: no storage class in the SIGNATURE, because the class is a
// property of the tile rather than of the opcode. That does not keep it out of a
// register — the value is a slot either way, and a slot is what a cache register
// holds. What the class decides is which spill re-tags it on the way back to
// memory, and the tiler knows that before the stitcher stamps anything.
//
// The one class that must never reach a slot is `ref`, and the grammar is what
// keeps it out: there are no `ref_reg0` rules, so a ref-resolved tile can only
// reduce at `ref_mem`, which asks for the result in memory. The form that puts it
// there already exists and is the PLAIN stencil — the uncached form at every
// cache size — so this needs no second family, only for the grammar to offer that
// stencil as the memory-result candidate and for the walk to stamp it when the
// tile asks for one.
// Both carriers the spec's value model has for "the class is not in the
// signature": `word` (a slot whose class the tile resolves) and `any` (the
// runtime-TYPED carrier, `{bits, hi, kind}`, whose tag the interpreter reads from
// an RTT). Neither keeps its value out of a register.
//
// For `any` the tag looks like a reason it must — but it is not, because the two
// fields beside `bits` are exactly what the class determines: `hi` is the second
// half of a v128 and `kind` is the class itself. A cached value has no tag and
// needs none; the spill that returns it to memory is the class-specific stencil,
// and re-tagging is what that stencil is for.
//
// Either carrier can still be wider than a register — slot_t's union includes
// v128 — but the grammar decides that, not this. Ref is absent from
// jav_class_cacheable, so no `ref_reg*` location exists at all; a DECLARED v128
// is two slots on both sides of the pipeline and caches whole; and a POLY slot
// that RESOLVED to v128 is fenced by the terminal's `pw` flag — its two-register
// resolution does not match the one declared word this family moves, so
// gen_tile_burg refuses it any rule whose state window touches the slot. The
// cached form therefore only ever runs for the four scalar classes, where
// `bits` is the whole value and `hi` is dead.
bool VmEmitter::poly_slot(ValueType t) {
    return t == ValueType::TyWord || t == ValueType::TyAny;
}

// D3s / §2.5: "The implementation of the same instruction in many states can be
// the same." Two states emit the same BODY when they read the same operands from
// the same slots, place their results the same way, and move the same survivors
// — everything the state can change. An instruction that touches no register at
// all is the common case: `nop` in state 0 and in state 1 differ only in a number
// nothing in the body reads.
//
// Emitting both anyway is not wrong, it is a duplicate: two stencil-table entries
// with identical code, and the table is what the JIT carries in memory. So the
// later state points at the earlier one's stencil instead.
int VmEmitter::same_state_as(const Opcode* op, int state) const {
    for (int prev = 0; prev < state; prev++) {
        if (!(prev == 0 || emits_variant(op, prev))) continue;
        if (results_cached(op, prev) != results_cached(op, state)) continue;
        int a = operand_slots(op), same = 1;
        for (size_t k = 0; k < op->stack_in.size() && same; k++)
            if (operand_slot(op, (int)k, prev) != operand_slot(op, (int)k, state)) same = 0;
        if (!same) continue;
        // The survivor shift is (left, nout) in SLOTS: same pair, same moves.
        int lp = prev  > a ? prev  - a : 0;
        int ls = state > a ? state - a : 0;
        if (lp != ls) continue;
        return prev;
    }
    return -1;
}

// Does the family carry this opcode in this entry state? The emission loop and
// the variant table both ask, and a disagreement between them would have the
// stitcher stamp a stencil that was never generated.
bool VmEmitter::emits_variant(const Opcode* op, int state) const {
    int a = operand_slots(op);
    int left = state > a ? state - a : 0;
    // A state is a COUNT OF SLOTS cached from the top, and a value rides the
    // cache whole or not at all — so a state whose count ends INSIDE a value
    // names no machine and has no variant. With one-slot classes every count is
    // a boundary; a v128 operand is what puts a state mid-value.
    if (state < a) {
        int base = 0, boundary = (state == 0);
        for (int k = (int)op->stack_in.size() - 1; k >= 0 && !boundary; k--) {
            base += slot_width(op->stack_in[k]->ty);
            if (base >= state) { boundary = (base == state); break; }
        }
        if (!boundary) return false;
    }
    // Anything still cached on exit means the results had to be cached too, by
    // the argument above; a state that cannot place them has no variant and the
    // tiler reaches it through a transition instead (§2.5 / C5).
    if (left + result_slots(op) > tier2_n_) return false;
    // D7s. A result may go to MEMORY from a cached state, but only when nothing
    // survives beneath it: a pushed value above cached ones is not a state a
    // count of slots can name. With left == 0 the cached operands were all
    // consumed, so pushing leaves the cache EMPTY — state 0, and nameable.
    //
    // Demanding a cached result in every state was the broad reading and it
    // costs. An instruction whose operand is already in a register but whose
    // result belongs in memory then has no form at all, so the tiler either
    // takes the cached form and spills (2 + 5), or spills the operand first and
    // takes the all-memory form (5 + 4) — where reading the register and pushing
    // inline is 3.
    if (!op->stack_out.empty() && !results_cached(op, state) && left > 0) return false;
    for (size_t k = 0; k < op->stack_in.size(); k++) {
        if (operand_slot(op, (int)k, state) < 0) continue;
        const StackParam* si = op->stack_in[k];
        if (si->count.has_value()) return false;
        // An `addr` operand's VALUE is an i32 or i64 — one slot at either width,
        // and both resolutions are cacheable classes — so the declared Addr rides
        // a slot exactly like the value it resolves to. Gating on the DECLARED
        // type here was the family refusing every load and store in the ISA a
        // register while the grammar, which resolves Addr per module, offered
        // them cached rules: the resolved vocabulary is the one both ends of the
        // pipeline must answer.
        //
        // A `word` operand is admitted on the same argument with the fence moved
        // to the grammar: the instances that DON'T fit one register — v128 (two
        // slots) and ref (no register form at all) — never reduce at a
        // cached-operand rule, because gen_tile_burg refuses any state whose
        // window touches a pw terminal's v128 slot or a non-cacheable class. So
        // every instance that can reach this body is one of the four scalars,
        // for which one register is the whole value. `any` stays refused: its
        // instances are refs, which the same fence keeps in memory, so a variant
        // here would be code no rule ever stamps.
        if (si->ty != ValueType::TyAddr && si->ty != ValueType::TyWord
            && !cacheable(si->ty)) return false;
    }
    return true;
}

// What the machine is in after the variant for (op, state) runs: what survived
// keeps its depth, and the results are on top of it if this state cached them.
//
// This CANNOT be recovered from the arity, which is what it used to be: a `word`
// result pops and pushes exactly like an `i32` one and yet leaves the cache
// empty, because it went to memory. Publishing the arity instead had the grammar
// promise `local.get` a register the stencil never wrote, and the stitcher then
// spilled a slot that was never loaded — the value read as zero. So the emitted
// variant reports its own exit state, and there is nothing left to disagree with.
int VmEmitter::variant_fs(const Opcode* op, int state) const {
    if (state < 0) return -1;
    if (state > 0 && !emits_variant(op, state)) return -1;
    int a = operand_slots(op);
    int left = state > a ? state - a : 0;
    return left + (results_cached(op, state) ? result_slots(op) : 0);
}

// D7s: worth a second stencil only when this state would otherwise cache the
// result, and only when nothing survives beneath it — with survivors, a pushed
// result sits above cached values and the state has no name. At state 0 the
// memory form already exists under the plain name, so there is nothing to add.
bool VmEmitter::has_mem_form(const Opcode* op, int state) const {
    if (state <= 0 || op->stack_out.empty()) return false;
    // The OPERAND side must be legal at this state, which is the whole of what
    // emits_variant decides. Without asking, this emitted a memory-result form
    // for states whose operands cannot be read from the cache at all — a `word`
    // operand has no class to read AS, and it carries a tag a register does not.
    if (!emits_variant(op, state)) return false;
    if (!results_cached(op, state)) return false;      // it is already the mem form
    int a = operand_slots(op);
    return !(state > a);                                // left == 0
}

void VmEmitter::emit_one_opcode(SemLowerer& low, const Opcode* op, int state, int mem_result) {
    FILE* o = low.out();
    int stencil = (low.mode() == Mode::Stencil);
    // D7s: the same entry state has two result placements. `__sK` caches what it
    // produces; `__sKm` reads its operands from the same registers and pushes the
    // result inline, which is what an instruction wants when its consumer is far
    // enough away that the value belongs in memory anyway.
    int cache_res = !mem_result && results_cached(op, state);
    if (stencil && state >= 0) {
        fprintf(o, "void STENCIL gen_st_%s__s%d%s(CACHE_ARGS) {\n", op->mnemonic, state,
                mem_result ? "m" : "");
        fputs("    frame_t* f = &vm->frame; (void)f;\n", o);
        fputs("    STENCIL_TRAP_PC(_HOLE_pc);\n", o);
    } else if (stencil) {
        // The plain name is the NO-CACHE form at every n — tier-1's stencil, and
        // the one a driver stamps when it has no tiling to go on. State 0 is a
        // different thing: it says nothing is cached ON ENTRY, and its result
        // still lands in a register. Naming them alike made the tier-2 build's
        // fallback stamp tier-2 stencils with no transitions between them, so a
        // declined body computed with a cache nothing ever read or spilled.
        fprintf(o, "void STENCIL gen_st_%s(CACHE_ARGS) {\n", op->mnemonic);
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
            // br_table: the label TARGETS live in the side-table, so the vector itself
            // is pure decode — opgen's job. Expose its count as `<name>_count` so the
            // body can clamp the key, and skip the labels. The JIT already reads this
            // count during its compile-walk, so the stencil takes it as a baked hole.
            if (od->ty == ValueType::TyBrTable && op_body_refs_count(op, od->name)) {
                if (stencil) {
                    fprintf(o, "    u4 %s_count = (u4)_HOLE_%s_count;\n", od->name, od->name);
                } else {
                    fprintf(o, "    u4 %s_count = 0; (void)bbq_read_uleb128_u32(&f->code, &%s_count);\n",
                            od->name, od->name);
                    fprintf(o, "    { for (u4 _bt_i = 0; _bt_i <= %s_count; _bt_i++)"
                               " { u4 _bt_l = 0; (void)bbq_read_uleb128_u32(&f->code, &_bt_l); } }\n",
                            od->name);
                }
                continue;
            }
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
            // `flag: pops_first` — drop sp BEFORE the body instead of after. Needed by the
            // call family: the callee frame is carved at sp, so the operands must already
            // be popped when the body hands off. ONLY sound when nothing between here and
            // the hand-off allocates: sp bounds the GC root scan, so an allocation in that
            // window could collect these very operands. The default (drop after) is what
            // keeps struct.new/array.new_fixed safe, whose bodies DO allocate.
            if (SemLowerer::op_has_flag(op, "pops_first"))
                fprintf(o, "    JV_SP -= _vc_%s;\n", si->name);
        } else if (state >= 0 && operand_slot(op, k, state) >= 0) {
            // Cached: the value is already in a slot, so there is no pop and no
            // sp movement for it at all (§2.3 — "the stack pointer need not be
            // updated in instruction implementations that can access all stack
            // items in registers"). The slot is an opaque word; the cast back to
            // the operand's own type is what names it.
            int sl = operand_slot(op, k, state);
            if (si->ty == ValueType::TyAddr) {
                /* §2.3.11: same width selection as the GPOP_ADDR path below — the
                 * DECLARED addrtype of the addressed entry, read from the
                 * signature's `addr(expr)` source. The slot's producer PUT the
                 * value zero-extended at its own class width, so reading either
                 * width back off the carrier is exact. */
                if (!si->at_src) {
                    fprintf(stderr, "opgen: %s: `addr` operand '%s' has no addrtype source — write "
                                    "`addr(<is64-expr>) %s` (WASM §2.3.11: addrtype is declared "
                                    "by the memtype/tabletype, not carried by the value)\n",
                            op->mnemonic, si->name, si->name);
                    exit(1);
                }
                fprintf(o, "    CACHE_GET_ADDR(%s, CACHE_R%d, ", si->name, sl);
                low.lower_expr(*si->at_src, op, ValueType::TyI32);
                fputs(");\n", o);
            } else if (slot_width(si->ty) == 2) { // a v128 spans this slot and the next
                jrow r = low.jtype(si->ty);
                fprintf(o, "    %s %s = CACHE_GET_V128(CACHE_R%d, CACHE_R%d);\n",
                        r.scalar, si->name, sl, sl + 1);
            } else if (si->ty == ValueType::TyWord) {
                /* A cached `word` is one of the four scalar classes — the
                 * grammar's fence (jav_sig_t.pw + jav_class_cacheable) keeps
                 * v128- and ref-resolved instances out of cached-operand rules
                 * — so one register holds the whole value. The tag GPOP_WORD
                 * carries beside it is the one thing a register does not: a
                 * scalar's tag gates nothing (tags exist for the GC's ref
                 * detection, and refs never reach a slot), so the rebuilt value
                 * takes tag 0 exactly as a zero-initialised local does, and the
                 * class-specific spill re-tags anything returning to memory.
                 * `slot_t` verbatim: it is GPOP_WORD's own declared type — the
                 * word carrier is the slot, not jtype's scalar row. */
                fprintf(o, "    slot_t %s = CACHE_GET_WORD(CACHE_R%d); u1 %s_wt = 0;"
                           " (void)%s; (void)%s_wt;\n",
                        si->name, sl, si->name, si->name, si->name);
            } else {
                jrow r = low.jtype(si->ty);
                fprintf(o, "    %s %s = CACHE_GET_%s(CACHE_R%d);\n",
                        r.scalar, si->name, SemLowerer::slot_class(si->ty), sl);
            }
        } else if (stack_in_live(op, k)) {
            if (si->ty == ValueType::TyAddr) {
                /* §2.3.11: the width is the DECLARED addrtype of the module entry this operand
                 * addresses, named by the signature's `addr(expr)` source and read here exactly as
                 * validation reads it (§3.4.4/§3.4.5). A signature that omits the source cannot be
                 * lowered — there is no correct default, and silently falling back to the operand's
                 * runtime tag is the divergence this form exists to remove. */
                if (!si->at_src) {
                    fprintf(stderr, "opgen: %s: `addr` operand '%s' has no addrtype source — write "
                                    "`addr(<is64-expr>) %s` (WASM §2.3.11: addrtype is declared "
                                    "by the memtype/tabletype, not carried by the value)\n",
                            op->mnemonic, si->name, si->name);
                    exit(1);
                }
                fprintf(o, "    GPOP_ADDR(%s, ", si->name);
                low.lower_expr(*si->at_src, op, ValueType::TyI32);
                fprintf(o, ");\n");
            } else
                fprintf(o, "    GPOP_%s(%s);\n", SemLowerer::slot_class(si->ty), si->name);
        } else {
            fprintf(o, "    JV_SP -= %d;\n", low.jtype(si->ty).slots);
        }
    }

    emit_guards(low, op);

    for (auto* so : op->stack_out)
        // A VARIADIC result names a count, not a value: the slots are already on the
        // stack and exposing them is an sp raise, so there is nothing to declare.
        if (!out_sem_expr(so) && !out_count(so)) {
            if (so->ty == ValueType::TyWord) {
                fprintf(o, "    slot_t %s; u1 %s_wt;\n", so->name, so->name);
                // A cached result carries no tag: it is not in memory, so nothing
                // scans it, and the spill that puts it back re-tags it from the
                // class the tile resolved. The body still ASSIGNS the tag (its
                // source is unchanged, D1s), so say the value is deliberately
                // unused rather than let -Wunused-but-set-variable fail the build.
                if (cache_res)
                    fprintf(o, "    (void)%s_wt;\n", so->name);
            }
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

    // §3b variadic: now consume the operands the body read in-place (sp was held up so they
    // stayed rooted). `pops_first` ops already dropped theirs above.
    if (!SemLowerer::op_has_flag(op, "pops_first"))
        for (auto* si : op->stack_in)
            if (out_count(si)) fprintf(o, "    JV_SP -= _vc_%s;\n", si->name);

    // A slot holds the value at that DEPTH from the top, so an instruction that
    // consumes `a` items and produces `nout` renumbers everything under them:
    // a value that was at depth a+j is at depth nout+j afterwards. Ertl's minimal
    // organization pays for its small state count in exactly these moves (§2.3),
    // and they are not optional — leaving them out either strands a live value in
    // a slot the next instruction will not look at, or writes over it.
    //
    // Direction matters: moving up, walk down; moving down, walk up. Either way
    // no slot is read after it has been written. nout == a renumbers nothing.
    if (state >= 0) {
        int a = operand_slots(op);
        int left = state > a ? state - a : 0;
        // Where the survivors end up is where the results DIDN'T go: a result
        // that went to memory occupies no slot, so the survivors close all the
        // way up to slot 0. Counted in SLOTS — a v128 result vacates two.
        int nout = cache_res ? result_slots(op) : 0;
        if (left && nout > a)
            for (int j = left - 1; j >= 0; j--)
                fprintf(o, "    CACHE_R%d = CACHE_R%d;\n", nout + j, a + j);
        else if (left && nout < a)
            for (int j = 0; j < left; j++)
                fprintf(o, "    CACHE_R%d = CACHE_R%d;\n", nout + j, a + j);
    }

    for (auto* so : op->stack_out) {
        // §3b VARIADIC RESULT: an immediate-/type-driven result count. The values are
        // already at the frame base (a nested callee left them there), so exposing them
        // IS the marshaling — raise sp, push nothing. Only sound when the body is NOT
        // terminal; a `status` native tail-returns and would strand this as dead code.
        if (const SemExpr* rc = out_count(so)) {
            fprintf(o, "    { s4 _vr_%s = (s4)(", so->name);
            low.lower_expr(rc, op, ValueType::TyI32);
            fprintf(o, "); JV_SP += _vr_%s; }\n", so->name);
            continue;
        }
        std::string src = Spec::forwarded_name(so);
        const char* val = src.empty() ? so->name : src.c_str();
        // The result is the new top of stack, so in a cached state it lands in
        // slot 0 rather than being pushed. The values that SURVIVED this
        // instruction were moved into place above.
        if (cache_res && slot_width(so->ty) == 2) {
            // A v128 is two registers wide, so it lands in the top TWO slots —
            // low half in reg0, high half in reg1, which is the same order the
            // memory stack holds it in and therefore the order the spill and fill
            // stencils move it back in.
            fprintf(o, "    CACHE_R0 = CACHE_PUT_V128_LO(%s);\n", val);
            fprintf(o, "    CACHE_R1 = CACHE_PUT_V128_HI(%s);\n", val);
            continue;
        }
        if (cache_res) {
            fprintf(o, "    CACHE_R0 = CACHE_PUT_%s(%s);\n",
                    SemLowerer::slot_class(so->ty), val);
            continue;
        }
        fprintf(o, "    GPUSH_%s(%s);\n", SemLowerer::slot_class(so->ty), val);
    }

    if (!low.body_tail_returns(op)) {
        if (stencil) {
            // The JIT has no implicit per-op trap check (unlike the interp's next-dispatch, which sees
            // the trapping native's code.pos = code.length and ends). A native that set vm->trapped
            // must bail HERE, or the stamped chain runs on into the next stencil — corrupting memory
            // (a store after a trapped load) or underflowing the stack (a value-less trap like
            // table.get followed by a consumer). Status/control natives already bail via _HOLE_resync.
            if (low.native_called())
                fputs("    if (vm->trapped) TAIL return _HOLE_trap(CACHE_PASS);\n", o);
            fputs("    TAIL return _HOLE_cont(CACHE_PASS);\n", o);
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
    // The cache register file. It is the SAME slot list in every stencil's
    // signature — musttail requires the caller's and callee's signatures to
    // match, so a per-state signature is rejected by clang outright. Which slots
    // are live is the cache STATE, which the tiler tracks; the signature is
    // fixed. Pointer-typed like every other value a stencil carries, so clang
    // cannot see through it (see the copy-and-patch discipline: WE patch, clang
    // does not). n = 0 is the tier-1 shape and emits the same code as ever.
    fprintf(o, "typedef void* cache_slot_t;\n#define CACHE_ARGS vm_t* vm");
    for (int i = 0; i < tier2_n_; i++) fprintf(o, ", cache_slot_t _r%d", i);
    fprintf(o, "\n#define CACHE_PASS vm");
    for (int i = 0; i < tier2_n_; i++) fprintf(o, ", _r%d", i);
    fputs("\n", o);
    for (int i = 0; i < tier2_n_; i++) fprintf(o, "#define CACHE_R%d _r%d\n", i, i);
    fprintf(o, "#define CACHE_ENTRY vm");
    for (int i = 0; i < tier2_n_; i++) fputs(", 0", o);
    fputs("\n", o);
    if (tier2_n_) {
        // A slot is an opaque word, so reading and writing one is a REINTERPRET,
        // not a conversion: an integer round-trips through uintptr_t, and a float
        // rides its bits the same way a float constant hole already does. Emitted
        // per declared type row, so the set follows the spec's value model rather
        // than a list here.
        fputs("\n/* Slot access: the value's bits, reinterpreted. Never a numeric cast —\n"
              " * a float has no meaning as a pointer, and an int narrower than a slot\n"
              " * would sign-extend differently on the way back. */\n", o);
        for (auto* td : mod_->types) {
            jrow r = low.jtype(td->ty);
            const char* cls = SemLowerer::slot_class(td->ty);
            if (!cacheable(td->ty)) continue;
            // A two-slot class gets bespoke macros below — one slot cannot hold
            // it, so there is no single-slot get/put to emit here.
            if (slot_width(td->ty) == 2) continue;
            if (r.scalar[0] == 'f')
                fprintf(o,
                    "#define CACHE_GET_%s(sl) (__extension__({ union { uintptr_t p; %s v; } _u;"
                    " _u.p = (uintptr_t)(sl); _u.v; }))\n"
                    "#define CACHE_PUT_%s(x)  (__extension__({ union { uintptr_t p; %s v; } _u;"
                    " _u.p = 0; _u.v = (x); (cache_slot_t)_u.p; }))\n",
                    cls, r.scalar, cls, r.scalar);
            else
                fprintf(o,
                    "#define CACHE_GET_%s(sl) ((%s)(uintptr_t)(sl))\n"
                    "#define CACHE_PUT_%s(x)  ((cache_slot_t)(uintptr_t)(%s)(x))\n",
                    cls, r.scalar, cls, r.uscalar);
        }
        // A `word` has no class here — the tile resolved one, and the spill that
        // returns the value to memory is the class-specific stencil that re-tags
        // it. So this moves the SLOT: its low 8 bytes, which is the whole value
        // for every class a rule can put in a register (v128 and ref have no
        // *_reg0 rule, so they never reach this).
        fputs("#define CACHE_GET_ADDR(name, sl, is64) u8 name; do { if (is64)"
              " name = (u8)(uintptr_t)(sl); else name = (u8)(u4)(uintptr_t)(sl); } while (0)\n", o);
        fputs("#define CACHE_GET_WORD(sl) (__extension__({ slot_t _s;"
              " _s.l = (s8)(uintptr_t)(sl); _s; }))\n"
              "#define CACHE_PUT_WORD(x)  ((cache_slot_t)(uintptr_t)(x).l)\n"
              // …and the typed carrier, which is the same move: `bits` is the
              // value, `hi` is the half only a v128 has, and `kind` is the class
              // — so for anything a rule can cache, `bits` is all of it.
              "#define CACHE_PUT_ANY(x)   ((cache_slot_t)(uintptr_t)(x).bits)\n"
              // …and the two-slot value. A v128 is twice a register wide, so it
              // rides two slots — low half then high, the order the memory stack
              // holds it in, so the spill and fill move it back unchanged. The
              // stencils are built -fno-vectorize/-fno-slp-vectorize, so the lane
              // arithmetic is already in GPRs and two integer slots are where it
              // wanted to be anyway.
              "#define CACHE_GET_V128(lo, hi) (__extension__({ v128_t _v;"
              " _v.u64[0] = (u8)(uintptr_t)(lo); _v.u64[1] = (u8)(uintptr_t)(hi); _v; }))\n"
              "#define CACHE_PUT_V128_LO(x)  ((cache_slot_t)(uintptr_t)(x).u64[0])\n"
              "#define CACHE_PUT_V128_HI(x)  ((cache_slot_t)(uintptr_t)(x).u64[1])\n", o);
    }
    fputs("\n", o);
    emit_slot_macros(low);
    fputs("extern void STENCIL _HOLE_cont(CACHE_ARGS);\n"
          "extern void STENCIL _HOLE_trap(CACHE_ARGS);\n"
          "extern void STENCIL _HOLE_resync(CACHE_ARGS);  /* the in-buffer IP-dispatch stencil */\n"
          "extern uint64_t _HOLE_ip;                    /* a control op's own post-operand position */\n"
          "extern uint64_t _HOLE_pc;                    /* this stencil's source byte offset (§7.1.8 trap-frame offset) */\n", o);

    {
        char cseen[256][64]; int cnn = 0;
        for (auto* cop : mod_->opcodes) {
            if (!is_emittable(cop) || SemLowerer::stencil_excluded(cop)) continue;
            const char* hn[SemLowerer::OP_CONST_HOLES]; uint64_t hv[SemLowerer::OP_CONST_HOLES];
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
            // br_table's baked label count is a hole under a DERIVED name; declare it
            // or the stencil references an extern nothing emits. Each derived name
            // needs its own storage — `seen` keeps the pointer.
            static char cbuf[8][64]; static int ncbuf = 0;
            const char* nm = od->name;
            if (od->ty == ValueType::TyBrTable && op_body_refs_count(op, od->name) &&
                ncbuf < (int)(sizeof cbuf / sizeof cbuf[0])) {
                snprintf(cbuf[ncbuf], sizeof cbuf[0], "%s_count", od->name);
                nm = cbuf[ncbuf++];
            }
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
        // A guard is emitted into the stencil too, so a native named ONLY by an
        // `error:` condition still rides a _HOLE_ patch point and still needs its
        // extern declared. Scanning the body alone left that hole undeclared and the
        // stencil failed to compile against it.
        for (auto* e : op->errors)
            if (e->condition.has_value())
                low.collect_natives_expr(*e->condition, nat, &nn);
        // §3b variadic: the pop-count expression is lowered into the stencil ahead of the
        // body, so a native named ONLY by a count (the call family's jav_type_nparams —
        // struct.new's jav_struct_nfields merely happened to appear in its body too) rides
        // a _HOLE_ patch point on the same terms.
        for (auto* si : op->stack_in)  if (const SemExpr* vc = out_count(si)) low.collect_natives_expr(vc, nat, &nn);
        for (auto* so : op->stack_out) if (const SemExpr* vc = out_count(so)) low.collect_natives_expr(vc, nat, &nn);
        // ...and the addrtype source, lowered into the same preamble. THE RULE: every
        // expression the stencil lowers gets scanned, not just the body. An op set whose
        // addrtype source happens to be an `inline native` never notices this one, because
        // an inline native is a direct call with no hole to declare.
        for (auto* si : op->stack_in)  if (si->at_src) low.collect_natives_expr(*si->at_src, nat, &nn);
    }
    for (int i = 0; i < nn; i++)
        fprintf(o, "extern uint64_t _HOLE_%s;\n", nat[i]);
    for (int i = 0; g_libm_syms[i]; i++)
        fprintf(o, "extern uint64_t _HOLE_%s;\n", g_libm_syms[i]);
    fputs("\n", o);

    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || SemLowerer::stencil_excluded(op)) continue;
        // Always the no-cache form first: it is tier-1's stencil, and the tier-2
        // build needs it too as the form to stamp where there is no tiling.
        emit_one_opcode(low, op, -1);
        // The variant family: the same body again per entry state. State 0 always
        // gets one — it is where a cover starts — while a HIGHER state whose
        // operands are not all cacheable gets none, which is §2.5's omission rule,
        // and the tiler reaches it through a transition instead.
        // …and only when there IS a cache: at n=0 an `__s0` would be the no-cache
        // form under a second name, and that table has to stay byte-identical.
        if (tier2_n_)
            for (int st = 0; st <= tier2_n_; st++) {
                if ((st == 0 || emits_variant(op, st)) && same_state_as(op, st) < 0)
                    emit_one_opcode(low, op, st);   // D3s: a duplicate body is not emitted
                // …and D7s' second placement, where it exists.
                if (has_mem_form(op, st)) emit_one_opcode(low, op, st, 1);
            }
    }

    // Machinery stencils — the machine boundary + control continuations, the
    // SAME for any opgen VM (not per-spec), so they are generated here rather
    // than hand-written per consumer. `entry` crosses the C → preserve_none
    // boundary; `trap` records a trap; `resync` maps an IP to the stamped
    // successor through the driver-built offmap; `gen_st_halt` is the off-the-
    // end terminator (a guarded result stash — works whether the function
    // returns implicitly off the end or via an explicit return opcode).
    // The transitions. A spill moves the DEEPEST cached item to memory and a fill
    // takes the top of memory into the first free slot, so neither renumbers
    // anything: reg0 is the top either way, and only the count changes. That is
    // what makes them one stencil per (class, slot) rather than per state.
    //
    // They are what a chain rule in the tiling grammar costs, and what the
    // stitcher stamps in the gap between two instructions whose states disagree.
    if (tier2_n_) {
        fputs("\n/* ── Cache transitions (generated), per class and slot ── */\n", o);
        for (auto* td : mod_->types) {
            if (!cacheable(td->ty)) continue;
            const char* cls = SemLowerer::slot_class(td->ty);
            const char* nm = sclass_stencil_name(
                SigEmitter::classify_final(td->ty, "type row", "cache slot"));
            int w = slot_width(td->ty);
            for (int s = 0; s + w <= tier2_n_; s++) {
                if (w == 2) {
                    // A v128 crosses as one value in two slots. `s` names the slot
                    // it STARTS at, so the pair is (s, s+1) and the memory form is
                    // the single stack slot the whole value already occupies.
                    fprintf(o,
                        "void STENCIL gen_st_spill_%s_%d(CACHE_ARGS) {\n"
                        "    frame_t* f = &vm->frame; (void)f;\n"
                        "    GPUSH_V128(CACHE_GET_V128(CACHE_R%d, CACHE_R%d));\n"
                        "    TAIL return _HOLE_cont(CACHE_PASS);\n}\n", nm, s, s, s + 1);
                    fprintf(o,
                        "void STENCIL gen_st_fill_%s_%d(CACHE_ARGS) {\n"
                        "    frame_t* f = &vm->frame; (void)f;\n"
                        "    GPOP_V128(_v);\n"
                        "    CACHE_R%d = CACHE_PUT_V128_LO(_v);\n"
                        "    CACHE_R%d = CACHE_PUT_V128_HI(_v);\n"
                        "    TAIL return _HOLE_cont(CACHE_PASS);\n}\n", nm, s, s, s + 1);
                    continue;
                }
                fprintf(o,
                    "void STENCIL gen_st_spill_%s_%d(CACHE_ARGS) {\n"
                    "    frame_t* f = &vm->frame; (void)f;\n"
                    "    GPUSH_%s(CACHE_GET_%s(CACHE_R%d));\n"
                    "    TAIL return _HOLE_cont(CACHE_PASS);\n}\n", nm, s, cls, cls, s);
                fprintf(o,
                    "void STENCIL gen_st_fill_%s_%d(CACHE_ARGS) {\n"
                    "    frame_t* f = &vm->frame; (void)f;\n"
                    "    GPOP_%s(_v);\n"
                    "    CACHE_R%d = CACHE_PUT_%s(_v);\n"
                    "    TAIL return _HOLE_cont(CACHE_PASS);\n}\n", nm, s, cls, s, cls);
            }
        }
    }

    fputs(
"\n/* ── Machinery stencils (generated): the machine boundary + control\n"
" * continuations, identical for every opgen VM (the JIT requires a bbq_ctx\n"
" * code cursor in the backend frame). ── */\n"
"/* Run off the function end. The results ARE the top of the frame stack, where the\n"
" * caller reads them — there is nothing to stash into a side channel. */\n"
"void STENCIL gen_st_halt(CACHE_ARGS) { (void)vm; }\n"
"typedef void STENCIL (*opgen_kont_t)(CACHE_ARGS);\n"
"extern uint64_t _HOLE_offmap;\n"
"extern uint64_t _HOLE_codelen;\n"
"void STENCIL resync(CACHE_ARGS) {\n"
"    size_t _p = vm->frame.code.pos;\n"
"    size_t _len = (size_t)_HOLE_codelen;\n"
"    opgen_kont_t* _map = (opgen_kont_t*)(uintptr_t)_HOLE_offmap;\n"
"    opgen_kont_t _fn = (_p <= _len) ? _map[_p] : _map[_len];\n"
"    if (!_fn) _fn = _map[_len];\n"
"    TAIL return _fn(CACHE_PASS);\n"
"}\n"
"void STENCIL trap(CACHE_ARGS) { vm->trapped = 1; }\n"
/* The machine boundary: ordinary C in, preserve_none out. The cache file starts
 * empty, so the entry passes null slots — a value is only in one because a
 * stencil put it there. */
"void entry(vm_t* vm) {\n"
"    return ((void STENCIL (*)(CACHE_ARGS))_HOLE_cont)(CACHE_ENTRY);\n"
"}\n", o);
}

// ── wasm_jit_symbols.h ──────────────────────────────────────

void VmEmitter::emit_jit_symbols(FILE* o) {
    fputs("/* AUTO-GENERATED by opgen — do not edit. Source: opcodes.def */\n", o);
    put_p(o, "#ifndef WASM_JIT_SYMBOLS_H\n#define WASM_JIT_SYMBOLS_H\n");
    fputs("#include \"runtime_api.h\"   /* the native prototypes */\n", o);
    fputs("#include <math.h>           /* the libm intrinsic prototypes */\n", o);
    fputs("#include <string.h>         /* strcmp, for the search below */\n\n", o);
    put_p(o, "typedef struct { const char* name; void* addr; } wasm_jit_sym_t;\n\n");
    // SORTED, and with the search emitted beside it. The JIT resolves a hole name
    // for every hole of every stencil it stamps, and MOST holes are not natives —
    // `_HOLE_cont`, `_HOLE_ip`, the operand holes — so a linear scan runs the
    // whole table to completion and returns nothing. That is the common case, not
    // the rare one: profiling the startup showed 31M strcmp calls out of it, 18%
    // of the time to compile a module.
    std::vector<std::string> syms;
    for (auto* md : mod_->methods) {
        if (!md->native || md->inl) continue;   // inline natives are direct calls (folded static-inline), not _HOLE_ patch points
        syms.push_back(md->name);
    }
    for (int i = 0; g_libm_syms[i]; i++) syms.push_back(g_libm_syms[i]);
    std::sort(syms.begin(), syms.end());        // strcmp order — what the search below assumes

    put_p(o, "static const wasm_jit_sym_t wasm_jit_symbols[] = {\n");
    for (auto& s : syms)
        fprintf(o, "    { \"_HOLE_%s\", (void*)%s },\n", s.c_str(), s.c_str());
    fputs("};\n", o);
    put_p(o, "static const int wasm_jit_symbols_count =\n"
          "    (int)(sizeof(wasm_jit_symbols) / sizeof(wasm_jit_symbols[0]));\n\n");
    // The table is sorted by name, so this is a binary search: ~9 comparisons
    // against the whole table, and a miss costs the same as a hit.
    put_p(o, "static inline void* wasm_jit_sym(const char* name) {\n"
          "    int lo = 0, hi = wasm_jit_symbols_count - 1;\n"
          "    while (lo <= hi) {\n"
          "        int mid = lo + (hi - lo) / 2;\n"
          "        int c = strcmp(wasm_jit_symbols[mid].name, name);\n"
          "        if (c == 0) return wasm_jit_symbols[mid].addr;\n"
          "        if (c < 0) lo = mid + 1; else hi = mid - 1;\n"
          "    }\n"
          "    return 0;\n"
          "}\n\n");
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
          "               JOP_F32, JOP_F64, JOP_U8, JOP_MEMARG, JOP_BLOCKTYPE, JOP_CONST,\n"
          "               JOP_BRTABLE_COUNT } jit_operand_kind_t;   /* the vec(labelidx) length; the\n"
          "                                     tail walk then skips count+1 labels, not the count */\n"
          "/* A variable-length trailing immediate the op's native reads at runtime; the JIT\n"
          " * compile-walk skips it (by kind) after capturing _HOLE_ip, to place the next stencil. */\n"
          "typedef enum { JTAIL_NONE, JTAIL_BRTABLE, JTAIL_TRYTABLE, JTAIL_SELECTVEC } jit_tail_kind_t;\n"
          "typedef struct { const char* hole; jit_operand_kind_t kind; uint64_t value; } jit_operand_t;\n"
          "/* `pop`/`push` are ITEMS — the operand stack's own unit, one entry per value.\n"
          " * `pop_slots`/`push_slots` are the same effect in CACHE SLOTS, where a v128\n"
          " * spends two and every other class spends one. Height arithmetic wants items;\n"
          " * anything indexing the cache wants slots. */\n"
          "typedef struct { int stencil; const jit_operand_t* operands;\n"
          "                 unsigned char operand_count, pop, push, tail,\n"
          "                               pop_slots, push_slots; } wasm_jit_meta_t;\n\n");

    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op)) continue;
        const char* cn[SemLowerer::OP_CONST_HOLES]; uint64_t cv[SemLowerer::OP_CONST_HOLES];
        int nc = low.op_const_holes(op, cn, cv);
        if (n_fixed_operands(op) == 0 && nc == 0) continue;   // tail-only ops have no fixed operand array
        fprintf(o, "static const jit_operand_t jitops_%s[] = {", op->mnemonic);
        for (auto* od : op->operands) {
            if (is_tail_operand(od->ty)) {
                // br_table's COUNT is a fixed operand (the JIT walk reads it anyway);
                // only the label vector itself remains tail-skipped.
                if (od->ty == ValueType::TyBrTable && op_body_refs_count(op, od->name))
                    fprintf(o, " {\"_HOLE_%s_count\", JOP_BRTABLE_COUNT, 0},", od->name);
                continue;
            }
            fprintf(o, " {\"_HOLE_%s\", %s, 0},", od->name, jop_kind(od->ty));
        }
        for (int k = 0; k < nc; k++)
            fprintf(o, " {\"%s\", JOP_CONST, 0x%llxULL},", cn[k], (unsigned long long)cv[k]);
        fputs(" };\n", o);
    }

    auto emit_meta_row = [&](const Opcode* op) {
        const char* cn[SemLowerer::OP_CONST_HOLES]; uint64_t cv[SemLowerer::OP_CONST_HOLES];
        int pop = (int)op->stack_in.size(), push = (int)op->stack_out.size();
        int nops = n_fixed_operands(op) + low.op_const_holes(op, cn, cv);
        const char* tail = tail_kind_name(op);
        if (SemLowerer::stencil_excluded(op)) fputs("-1", o);
        else { fputs("STENCIL_GEN_ST_", o); put_upper(o, op->mnemonic); }
        int pops = operand_slots(op), pushs = result_slots(op);
        if (nops) fprintf(o, ", jitops_%s, %d, %d, %d, %s, %d, %d },\n",
                          op->mnemonic, nops, pop, push, tail, pops, pushs);
        else      fprintf(o, ", NULL, 0, %d, %d, %s, %d, %d },\n", pop, push, tail, pops, pushs);
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
    fputs("};\n", o);

    // The cache-state variant of each stencil: what the stitcher stamps once the
    // tiling has said which state an instruction runs in. Indexed by the BASE
    // stencil so the walk needs no second lookup — it already has that from the
    // opcode's meta. -1 is "no variant for this state", which the grammar never
    // asks for, because a rule only exists where the stencil does.
    fprintf(o, "\n#define %s_TIER2_N %d\n", uprefix_.c_str(), tier2_n_);
    fprintf(o, "static const int %s_variant[][%s_TIER2_N + 1] = {\n",
            prefix_.c_str(), uprefix_.c_str());
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || SemLowerer::stencil_excluded(op)) continue;
        fputs("    [STENCIL_GEN_ST_", o); put_upper(o, op->mnemonic); fputs("] = {", o);
        for (int st = 0; st <= tier2_n_; st++) {
            // Indexed by ENTRY state, so slot 0 is the state-0 variant and NOT the
            // plain stencil — that one is tier-1's, reachable through the meta.
            if (st == 0 && !tier2_n_) {
                fputs(" STENCIL_GEN_ST_", o); put_upper(o, op->mnemonic);
            } else if (st == 0 || emits_variant(op, st)) {
                // D3s: a state sharing an earlier state's body names that stencil.
                int share = same_state_as(op, st);
                fputs(st ? ", STENCIL_GEN_ST_" : " STENCIL_GEN_ST_", o);
                put_upper(o, op->mnemonic);
                fprintf(o, "__S%d", share >= 0 ? share : st);
            } else {
                fputs(", -1", o);
            }
        }
        fputs(" },\n", o);
    }
    fputs("};\n", o);

    // The state each variant leaves behind, in step with the table above. ONE
    // fact with two readers — the tiling grammar prices the transitions its
    // state choices imply, and the stitcher stamps transitions against the same
    // numbers — and it is REPORTED BY THE EMISSION rather than recomputed from
    // the arity, because the arity cannot see a result that went to memory.
    fprintf(o,
        "/* The cache state after the variant above, or -1 where there is none.\n"
        " * Not derivable from pop/push: a `word` result moves the stack exactly\n"
        " * like an `i32` one and yet leaves nothing cached, having no class to\n"
        " * cache AS. Read this; do not recompute it. */\n");
    // D7s' second placement, indexed the same way: the form that reads this
    // state's cached operands and pushes its result inline, or -1 where the state
    // has only one placement. At state 0 that form is the PLAIN stencil, which
    // the meta already names, so the row is -1 there too.
    fprintf(o, "static const int %s_variant_m[][%s_TIER2_N + 1] = {\n",
            prefix_.c_str(), uprefix_.c_str());
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || SemLowerer::stencil_excluded(op)) continue;
        fputs("    [STENCIL_GEN_ST_", o); put_upper(o, op->mnemonic); fputs("] = {", o);
        for (int st = 0; st <= tier2_n_; st++) {
            if (has_mem_form(op, st)) {
                fputs(st ? ", STENCIL_GEN_ST_" : " STENCIL_GEN_ST_", o);
                put_upper(o, op->mnemonic);
                fprintf(o, "__S%dM", st);
            } else fputs(st ? ", -1" : " -1", o);
        }
        fputs(" },\n", o);
    }
    fputs("};\n", o);

    fprintf(o, "static const int %s_variant_fs[][%s_TIER2_N + 1] = {\n",
            prefix_.c_str(), uprefix_.c_str());
    for (auto* op : mod_->opcodes) {
        if (!is_emittable(op) || SemLowerer::stencil_excluded(op)) continue;
        fputs("    [STENCIL_GEN_ST_", o); put_upper(o, op->mnemonic); fputs("] = {", o);
        for (int st = 0; st <= tier2_n_; st++)
            fprintf(o, "%s %d", st ? "," : "", variant_fs(op, st));
        fputs(" },\n", o);
    }
    fputs("};\n", o);

    // The transitions, per storage class and slot. A chain rule in the tiling
    // grammar is costed from these, and the stitcher stamps one when two adjacent
    // instructions disagree about the state. -1 where the class does not cache.
    for (int pass = 0; pass < 2; pass++) {
        const char* what = pass ? "fill" : "spill";
        fprintf(o, "\nstatic const int %s_%s[%u][%s_TIER2_N ? %s_TIER2_N : 1] = {\n",
                prefix_.c_str(), what, SCLASS_FINAL - 1, uprefix_.c_str(), uprefix_.c_str());
        for (unsigned c = 0; c + 1 < SCLASS_FINAL; c++) {
            fputs("    {", o);
            for (int s = 0; s < (tier2_n_ ? tier2_n_ : 1); s++) {
                const char* nm = sclass_stencil_name((SClass)c);
                // A value STARTING at slot s needs its whole width to fit, so a
                // two-slot class has no transition at the last slot — there is no
                // half of a v128 to move.
                if (!tier2_n_ || !nm || s + sclass_width((SClass)c) > tier2_n_) {
                    fputs(" -1,", o); continue;
                }
                fprintf(o, " STENCIL_GEN_ST_%s_%s_%d,", pass ? "FILL" : "SPILL", nm, s);
            }
            fputs(" },\n", o);
        }
        fputs("};\n", o);
    }
    put_p(o, "#endif /* WASM_JIT_META_H */\n");
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

    fputs("typedef struct { s8 bits; s8 hi; u1 kind; } any_t;   /* hi = the SECOND 64-bit half of a\n"
          "    T_V128 value (dead/zero for every other kind — compound literals zero it). Without it a\n"
          "    v128 crossing the runtime-typed carrier lost lanes 2/3. */\n", o);

    /* Structured control (§1: opgen owns the Titzer side-table format). The entry is
     * part of the abstract machine, not the substrate — the same tier as slot_t — so
     * it is emitted HERE, above the backend header, and the backend's validator fills
     * the same struct the generated walk reads. One definition, one owner.
     * Turned on by the spec's `sidetable` directive: opgen bakes in no target
     * identifiers, so it cannot key this off a built-in's NAME. */
    if (mod_->sidetable) {
    fputs("\n/* One side-table entry: a taken branch keeps the top `vals` operands,\n"
          " * drops `pop` beneath them, then advances the code cursor and the\n"
          " * side-table pointer by their deltas. */\n"
          "typedef struct {\n"
          "    s4 delta_ip;\n"
          "    s4 delta_stp;\n"
          "    u2 vals;\n"
          "    u2 pop;\n"
          "} opgen_st_entry_t;\n", o);
    }

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
"#define JV_NPOP(f, sv, tv) slot_t sv; u1 tv; do { (f)->sp--; sv = (f)->stack[(f)->sp]; tv = (f)->stack_types[(f)->sp]; } while (0)\n"
/* The third primitive: relocate a slot WITHIN the stack, value and tag together. Not a
 * push and not a pop, so neither macro above covers it; without it the one caller that
 * needs it (the side-table walk) hand-rolls f->stack[...] = f->stack[...], which is the
 * value-model duplication these macros exist to end. */
"#define JV_NMOVE(f, dst, src) do { (f)->stack[dst] = (f)->stack[src];"
" (f)->stack_types[dst] = (f)->stack_types[src]; } while (0)\n", o);

    /* The three structured-control cursors, as overridable accessors — the same seam
     * shape as LOCAL_SLOT/GLOBAL_GET. Defined unconditionally because an opcode BODY
     * may name `stp` (it lowers to OPGEN_ST_PTR) whether or not opgen is also asked to
     * emit the walk; a macro nothing expands costs nothing. */
    fputs(
"\n/* Structured control: the backend says where the cursors are. */\n"
"#ifndef OPGEN_ST_TABLE\n#define OPGEN_ST_TABLE(f)  ((f)->sidetable)\n#endif\n"
"#ifndef OPGEN_ST_PTR\n#define OPGEN_ST_PTR(f)    ((f)->stp)\n#endif\n"
"#ifndef OPGEN_IP\n#define OPGEN_IP(f)        ((f)->code.pos)\n#endif\n", o);

    /* The §4.4.8 walk. opgen owns the entry format (above), so it owns the walk that
     * reads it. Gated on the spec's `sidetable` directive: a substrate with no side
     * table must not be handed a walk that reads frame fields it does not have. */
    if (mod_->sidetable) {
    fputs(
"static inline void opgen_do_transfer(frame_t* f) {\n"
"    const opgen_st_entry_t* e = &OPGEN_ST_TABLE(f)[OPGEN_ST_PTR(f)];\n"
"    if (e->pop) {\n"
"        for (u2 i = 0; i < e->vals; i++)\n"
"            JV_NMOVE(f, (f)->sp - e->vals - e->pop + i, (f)->sp - e->vals + i);\n"
"        f->sp -= e->pop;\n"
"    }\n"
"    OPGEN_IP(f)      = (size_t)((long)OPGEN_IP(f) + e->delta_ip);\n"
"    OPGEN_ST_PTR(f)  = (u4)((long)OPGEN_ST_PTR(f) + e->delta_stp);\n"
"}\n", o);
    }
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

    // Guard error codes: one per distinct name declared by an `error:` annotation,
    // in first-declaration order. A fired guard passes its code to OPGEN_GUARD_TRAP
    // before tailing to the trap continuation, so a backend can tell the causes apart.
    fputs(
"\n/* ── Guard error codes (from the `error:` declarations in the spec) ──\n"
" * A fired guard reports one of these through OPGEN_GUARD_TRAP before it\n"
" * traps. The default discards it; override the macro to record it. */\n", o);
    {
        // Unbounded on purpose: a fixed cap would drop a declared name, and the
        // guard that names it would then emit an undefined OPGEN_ERR_*.
        std::vector<const char*> seen_e;
        fputs("typedef enum {", o);
        for (auto* op : mod_->opcodes) {
            for (auto* e : op->errors) {
                if (!e->error_name) continue;
                bool dup = false;
                for (const char* s : seen_e) if (!strcmp(s, e->error_name)) { dup = true; break; }
                if (dup) continue;
                fprintf(o, "%s\n    OPGEN_ERR_%s = %d",
                        seen_e.empty() ? "" : ",", e->error_name, (int)seen_e.size());
                seen_e.push_back(e->error_name);
            }
        }
        // A spec with no `error:` declarations still needs the type to exist: the
        // OPGEN_GUARD_TRAP default mentions it only inside guards, but the enum is
        // part of the emitted API surface either way.
        if (seen_e.empty()) fputs("\n    OPGEN_ERR_NONE = 0", o);
        fputs("\n} opgen_err_t;\n", o);
    }

    fputs("\n#endif /* OPGEN_RUNTIME_API_H */\n", o);
}

} // namespace opgen
