// The opcode-body lowering (sem_expr/sem_stmt → C), ported from the C
// emit_interp_c.cpp. The algorithm is unchanged — every emitted byte matches
// the reference VM; the g_mod/g_mode/g_lane globals are now members.
#include <cstdlib>
#include "semlower.h"
#include <cstring>
#include <cctype>

namespace opgen {

// Unwrap an optional<T*> to a plain pointer (nullptr when empty) — preserves
// the C "NULL means absent" tests on `?` AST fields.
template <class T>
static T* opt_ptr(const std::optional<T*>& o) { return o ? *o : nullptr; }

// ── Type table ──────────────────────────────────────────────

jrow SemLowerer::jtype(ValueType t) const {
    // Prefer the spec's `type` declarations so the slot model swaps per target;
    // fall back to the built-in JCVM rows.
    if (mod_) {
        for (auto* d : mod_->types)
            if (d->ty == t)
                return jrow{ d->scalar, d->uscalar, d->slots, d->field[0] };
    }
    switch (t) {
    case ValueType::TyI8:    return jrow{"s1",    "u1",    1, 's'};
    case ValueType::TyU8:    return jrow{"u1",    "u1",    1, 's'};
    case ValueType::TyI16:   return jrow{"s2",    "u2",    1, 's'};
    case ValueType::TyU16:   return jrow{"u2",    "u2",    1, 's'};
    case ValueType::TyWord:  return jrow{"s2",    "u2",    1, 's'};
    case ValueType::TyI32:   return jrow{"s4",    "u4",    2, 's'};
    case ValueType::TyDword: return jrow{"s4",    "u4",    2, 's'};
    case ValueType::TyRef:   return jrow{"ref_t", "ref_t", 1, 'r'};
    case ValueType::TyI64:   return jrow{"s8",    "u8",    2, 'i'};  // JVM long
    case ValueType::TyF32:   return jrow{"f4",    "f4",    1, 'f'};  // JVM float
    case ValueType::TyF64:   return jrow{"f8",    "f8",    2, 'f'};  // JVM double
    case ValueType::TyAny:   return jrow{"any_t", "any_t", 1, 'i'};  // runtime-typed
    case ValueType::TyAddr:  return jrow{"u8",    "u8",    1, 'l'};  // WASM addrtype: 1 slot; GPOP_ADDR (custom) picks .i/.l from the DECLARED addrtype the signature's addr(...) source names
    case ValueType::TyV128:  return jrow{"v128_t", "v128_t", 1, 'v'};  // WASM SIMD
    case ValueType::TyI8x16: case ValueType::TyI16x8: case ValueType::TyI32x4:
    case ValueType::TyI64x2: case ValueType::TyF32x4: case ValueType::TyF64x2:
        return jrow{"v128_t", "v128_t", 1, 'v'};
    case ValueType::TyFuncRef:   return jrow{"ref_t",  "ref_t",  1, 'r'};
    case ValueType::TyExternRef: return jrow{"ref_t",  "ref_t",  1, 'r'};
    case ValueType::TyI31Ref:    return jrow{"ref_t",  "ref_t",  1, 'r'};
    case ValueType::TyUleb32:    return jrow{"u4", "u4", 1, 's'};
    case ValueType::TyUleb64:    return jrow{"u8", "u8", 1, 's'};
    case ValueType::TySleb32:    return jrow{"s4", "u4", 1, 's'};
    case ValueType::TySleb64:    return jrow{"s8", "u8", 1, 's'};
    case ValueType::TyMemarg:    return jrow{"u8", "u8", 1, 's'};
    case ValueType::TyBlockType: return jrow{"s4", "u4", 1, 's'};   // s33 carrier; the trailing heaptype (0x63/0x64) is skipped by the reader, not bound
    // Variable-length tail immediates (br_table / try_table / select type vector):
    // not value-stack slots — the op's native decodes them, so no slot carrier applies.
    case ValueType::TyBrTable: case ValueType::TyTryTable: case ValueType::TySelectVec:
        break;
    }
    return jrow{"s4", "u4", 2, 's'};
}

const char* SemLowerer::java_c(const char* jt) {
    if (!strcmp(jt, "byte"))    return "s1";
    if (!strcmp(jt, "short"))   return "s2";
    if (!strcmp(jt, "char"))    return "u2";
    if (!strcmp(jt, "int"))     return "s4";
    if (!strcmp(jt, "long"))    return "s8";
    if (!strcmp(jt, "uint"))    return "u4";
    if (!strcmp(jt, "ulong"))   return "u8";
    if (!strcmp(jt, "float"))   return "f4";
    if (!strcmp(jt, "double"))  return "f8";
    if (!strcmp(jt, "boolean")) return "s1";
    if (!strcmp(jt, "any"))     return "any_t";
    /* The action language's type names are a closed set. An unlisted one is a
     * spec error, and answering `s4` narrows or widens it silently — which is
     * how a `ulong` native once got an s4 prototype. */
    fprintf(stderr, "opgen: `%s` is not an action-language type\n", jt);
    std::exit(1);
}

ValueType SemLowerer::local_vt(const Opcode* op) const {
    if (op->local_value.has_value()) {
        switch ((*op->local_value)->kind) {
        case LocalValueKind::LvShort:  return ValueType::TyI16;
        case LocalValueKind::LvInt:    return ValueType::TyI32;
        case LocalValueKind::LvRef:    return ValueType::TyRef;
        case LocalValueKind::LvLong:   return ValueType::TyI64;
        case LocalValueKind::LvFloat:  return ValueType::TyF32;
        case LocalValueKind::LvDouble: return ValueType::TyF64;
        }
    }
    // No explicit local_value: infer from the stack param a load pushes / a
    // store pops.
    if (!op->stack_out.empty()) return op->stack_out[0]->ty;
    if (!op->stack_in.empty())  return op->stack_in[0]->ty;
    return ValueType::TyI16;
}

const char* SemLowerer::fetch_expr(ValueType t) {
    switch (t) {
    case ValueType::TyI8:  return "FETCH_S1()";
    case ValueType::TyU8:  return "FETCH_U1()";
    case ValueType::TyI16: return "FETCH_S2()";
    case ValueType::TyU16: return "FETCH_U2()";
    case ValueType::TyI32: return "FETCH_S4()";
    default:               return "FETCH_S2()";
    }
}

const char* SemLowerer::fetch_fn(ValueType t) {
    switch (t) {
    case ValueType::TyU8:     return "bbq_read_u8";
    case ValueType::TyUleb32: return "bbq_read_uleb128_u32";
    case ValueType::TyUleb64: return "bbq_read_uleb128_u64";
    case ValueType::TySleb32: return "bbq_read_sleb128_i32";
    case ValueType::TySleb64: return "bbq_read_sleb128_i64";
    case ValueType::TyBlockType: return "bbq_read_sleb128_i32";   // base s33 read; trailing heaptype skipped separately (vmemit)
    case ValueType::TyF32:    return "bbq_read_f32le";
    case ValueType::TyF64:    return "bbq_read_f64le";
    default:                  return nullptr;
    }
}

ValueType SemLowerer::lane_elem(ValueType t, int* count, const char** ufield) {
    switch (t) {
    case ValueType::TyI8x16: *count = 16; *ufield = "i8";  return ValueType::TyI8;
    case ValueType::TyI16x8: *count = 8;  *ufield = "i16"; return ValueType::TyI16;
    case ValueType::TyI32x4: *count = 4;  *ufield = "i32"; return ValueType::TyI32;
    case ValueType::TyI64x2: *count = 2;  *ufield = "i64"; return ValueType::TyI64;
    case ValueType::TyF32x4: *count = 4;  *ufield = "f32"; return ValueType::TyF32;
    case ValueType::TyF64x2: *count = 2;  *ufield = "f64"; return ValueType::TyF64;
    default:                 *count = 0;  *ufield = nullptr;  return ValueType::TyV128;
    }
}

bool SemLowerer::is_lane_type(ValueType t) {
    int c; const char* f; lane_elem(t, &c, &f); return c != 0;
}

const char* SemLowerer::slot_class(ValueType t) {
    if (is_lane_type(t)) return "V128";
    if (t == ValueType::TyRef) return "REF";
    if (t == ValueType::TyI64) return "LONG";
    if (t == ValueType::TyF32) return "FLOAT";
    if (t == ValueType::TyF64) return "DOUBLE";
    if (t == ValueType::TyV128) return "V128";
    if (t == ValueType::TyFuncRef || t == ValueType::TyExternRef || t == ValueType::TyI31Ref) return "REF";
    if (t == ValueType::TyI32 || t == ValueType::TyDword) return "INT";
    if (t == ValueType::TyWord) return "WORD";
    if (t == ValueType::TyAny) return "ANY";
    if (t == ValueType::TyAddr) return "ADDR";
    return "I16";
}

int SemLowerer::scalar_bits(const char* s) const {
    if (!strcmp(s, "s1") || !strcmp(s, "u1")) return 8;
    if (!strcmp(s, "s2") || !strcmp(s, "u2")) return 16;
    if (!strcmp(s, "s4") || !strcmp(s, "u4") || !strcmp(s, "f4")) return 32;
    if (!strcmp(s, "s8") || !strcmp(s, "u8") || !strcmp(s, "f8")) return 64;
    /* A reference carries no width in its NAME, but it is word-sized by
     * construction: the value model emits `typedef <uw> ref_t` off the slot
     * view, so the width reads back off that same view rather than off the
     * spelling. int_slot_view only consults integer rows, so this does not
     * re-enter. Answering 32 here — as the old fallback did — masks a shift
     * count against 31 on a 64-bit handle. */
    if (!strcmp(s, "ref_t")) {
        char fld; const char* islot; const char* uw; int w;
        int_slot_view(&fld, &islot, &uw, &w);
        return w;
    }
    /* The spellings are a closed set and Spec rejects a type row outside it,
     * so reaching here means an internal caller synthesised a scalar name.
     * Defaulting to 32 would emit a plausible wrong width silently. */
    fprintf(stderr, "opgen: internal: no width for scalar `%s`\n", s);
    std::exit(1);
}

void SemLowerer::int_slot_view(char* fld, const char** islot, const char** uw, int* w) const {
    /* Derived from the widest single-slot integer row the spec declares. Spec
     * refuses a spec that declares none, so the scan always finds one; the
     * initial values are overwritten rather than used as a fallback. */
    *fld = 's'; *islot = "s2"; *uw = "u2"; *w = 16;
    int best = -1;
    if (mod_) for (auto* td : mod_->types) {
        jrow r = jtype(td->ty);
        if (r.slots == 1 && (r.scalar[0] == 's' || r.scalar[0] == 'u')) {
            int bits = scalar_bits(r.scalar);
            if (bits > best) {
                best = bits; *fld = r.field; *islot = r.scalar; *w = bits;
                *uw = (bits <= 16) ? "u2" : (bits <= 32) ? "u4" : "u8";
            }
        }
    }
}

// Does this expression already denote a whole SLOT (value + tag), as opposed to a
// scalar? Only slot-valued sources can be assigned to a `word` target wholesale.
int SemLowerer::expr_is_slot_valued(const Opcode* op, const SemExpr* e) const {
    if (!e) return 0;
    switch (e->tag) {
    case SemExprTag::SIdent: {
        ValueType t = type_of_name(op, static_cast<const SIdent*>(e)->name);
        return t == ValueType::TyWord || t == ValueType::TyAny;
    }
    case SemExprTag::STernary: {
        auto* tn = static_cast<const STernary*>(e);
        return expr_is_slot_valued(op, tn->then_) && expr_is_slot_valued(op, tn->else_);
    }
    case SemExprTag::SIndex:            /* locals[i] / globals[i] read a slot */
        return 1;
    case SemExprTag::SCall: {           /* a native returning `any`/`word` */
        const MethodDecl* md = find_method(static_cast<const SCall*>(e)->name);
        return md && md->ret_ty && (!strcmp(md->ret_ty, "any") || !strcmp(md->ret_ty, "word"));
    }
    default: return 0;
    }
}

ValueType SemLowerer::type_of_name(const Opcode* op, const char* name) const {
    for (auto* s : op->stack_out) if (!strcmp(s->name, name)) return s->ty;
    for (auto* s : op->stack_in)  if (!strcmp(s->name, name)) return s->ty;
    for (auto* o : op->operands)  if (!strcmp(o->name, name)) return o->ty;
    return ValueType::TyI32;
}

// ── Expression sub-lowerings ────────────────────────────────

const char* SemLowerer::slot_index_tag_array(const SemExpr* e) const {
    if (e->tag != SemExprTag::SIndex) return nullptr;
    auto* idx = static_cast<const SIndex*>(e);
    if (idx->base->tag != SemExprTag::SIdent) return nullptr;
    const char* b = static_cast<const SIdent*>(idx->base)->name;
    if (!strcmp(b, "locals"))  return "f->local_types";
    if (!strcmp(b, "globals")) return "vm->frame.ctx->global_types";   // §8: globals via the active instance context
    return nullptr;
}

// globalinst access goes through the GLOBAL_GET/SET/TAG_SET vocabulary macros — opgen emits a
// generated default (vm->globals[i][0]; held BY REFERENCE so an entry aliases its defining instance's
// slot, sharing a mutable imported global), which a backend may override (#ifndef) to point the access
// at a different shape (e.g. an instance-context global table) WITHOUT changing this lowering.
void SemLowerer::lower_global_read(const Opcode* op, const SemExpr* idx, ValueType rt) {
    fputs("GLOBAL_GET(", out_); lower_expr(idx, op, rt); fputs(")", out_);
}

void SemLowerer::lower_global_store(const Opcode* op, const SemExpr* idx, const SemExpr* val) {
    fputs("    GLOBAL_SET(", out_); lower_expr(idx, op, ValueType::TyI32); fputs(", ", out_);
    lower_expr(val, op, ValueType::TyWord); fputs(");\n", out_);
    if (val->tag == SemExprTag::SIdent) {
        fputs("    GLOBAL_TAG_SET(", out_); lower_expr(idx, op, ValueType::TyI32);
        fprintf(out_, ", %s_wt);\n", static_cast<const SIdent*>(val)->name);
    }
}

// localinst access goes through LOCAL_SLOT (the generated default `f->locals[i]`, backend-overridable)
// — the value-model field selector (.i/.l/.r) stays here (it IS the value model, parameterized by the
// backend's type rows). LOCAL_TAG_SET records the runtime type tag for whole-slot moves + GC roots.
void SemLowerer::lower_local_read(const Opcode* op, const SemExpr* idx, ValueType rt) {
    if (local_vt(op) == ValueType::TyWord) {
        fputs("(LOCAL_SLOT(", out_); lower_expr(idx, op, rt); fputs("))", out_);
        return;
    }
    jrow r = jtype(local_vt(op));
    if (r.field == 'r') {
        fputs("(LOCAL_SLOT(", out_); lower_expr(idx, op, rt); fputs(").r)", out_);
    } else if (r.slots == 1) {
        fputs("(LOCAL_SLOT(", out_); lower_expr(idx, op, rt); fprintf(out_, ").%c)", r.field);
    } else {
        char ic; const char* islot; const char* uw; int w;
        int_slot_view(&ic, &islot, &uw, &w); (void)islot;
        if (r.scalar[0] == 'f') {
            fprintf(out_, "(__extension__ ({ union { %s w; %s u; } _x; _x.u = ((%s)(%s)LOCAL_SLOT(",
                    r.scalar, r.uscalar, r.uscalar, uw);
            lower_expr(idx, op, rt);
            fprintf(out_, ").%c) | (((%s)(%s)LOCAL_SLOT((", ic, r.uscalar, uw);
            lower_expr(idx, op, rt);
            fprintf(out_, ")+1).%c) << %d); _x.w; }))", ic, w);
        } else {
            fprintf(out_, "(((%s)(%s)LOCAL_SLOT(", r.scalar, uw);
            lower_expr(idx, op, rt);
            fprintf(out_, ").%c) | (((%s)LOCAL_SLOT((", ic, r.scalar);
            lower_expr(idx, op, rt);
            fprintf(out_, ")+1).%c) << %d))", ic, w);
        }
    }
}

// ── Status vocabulary + native resolution ───────────────────

const char* SemLowerer::status_type() const { return (mod_ && mod_->status) ? (*mod_->status)->type : "opgen_status_t"; }
const char* SemLowerer::status_ok() const   { return (mod_ && mod_->status) ? (*mod_->status)->ok   : "OPGEN_OK"; }
const char* SemLowerer::status_err() const  { return (mod_ && mod_->status) ? (*mod_->status)->err  : "OPGEN_ERR"; }
const char* SemLowerer::status_halt() const { return (mod_ && mod_->status) ? (*mod_->status)->halt : "OPGEN_HALT"; }

const MethodDecl* SemLowerer::find_method(const char* name) const {
    if (!mod_) return nullptr;
    for (auto* md : mod_->methods)
        if (!strcmp(md->name, name)) return md;
    return nullptr;
}

ValueType SemLowerer::arg_type(const SemExpr* e, const Opcode* op, ValueType rt) const {
    if (e && e->tag == SemExprTag::SIdent) return type_of_name(op, static_cast<const SIdent*>(e)->name);
    return rt;
}

void SemLowerer::emit_arg(const SemExpr* a, const Opcode* op, ValueType at) {
    fputc('(', out_); lower_expr(a, op, at); fputc(')', out_);
}

bool SemLowerer::is_intrinsic(const char* n) {
    static const char* names[] = { "sqrt","fabs","ceil","floor","ftrunc","nearest",
        "fmin","fmax","copysign","clz","ctz","popcnt","rotl","rotr","reinterpret","push","pop",
        "int_min", nullptr };
    for (int i = 0; names[i]; i++) if (!strcmp(n, names[i])) return true;
    return false;
}

void SemLowerer::lower_intrinsic(const SemExpr* e, const Opcode* op, ValueType rt) {
    auto* call = static_cast<const SCall*>(e);
    const char* n = call->name;
    const SemExpr* a0 = call->args.size() > 0 ? call->args[0] : nullptr;
    const SemExpr* a1 = call->args.size() > 1 ? call->args[1] : nullptr;
    ValueType at = arg_type(a0, op, rt);
    if (lane_) { int _ec; const char* _ef; ValueType le = lane_elem(at, &_ec, &_ef); if (_ec) at = le; }

    // int_min(): the most negative value of the surrounding expression's integer type. Nullary and
    // width-polymorphic — it takes its width from `rt`, so ONE declared guard covers a `( T a, T b -- T )`
    // family across i32 and i64. The overflow guards that need it (INT_MIN / -1) cannot be written any
    // other way without hardcoding a width and losing the family.
    if (!strcmp(n,"int_min")) {
        int rW = scalar_bits(c_scalar(rt));
        fprintf(out_, "(%s)((%s)1 << %d)", c_scalar(rt), c_unsigned(rt), rW - 1);
        return;
    }
    // push(v): a control-conditional value push (the signature can't express a push on only one path,
    // e.g. br_on_null re-pushing the non-null ref). v is an any_t; GPUSH_ANY rides its runtime tag.
    if (!strcmp(n,"push")) {
        fputs("GPUSH_ANY(", out_); lower_expr(a0, op, ValueType::TyAny); fputc(')', out_); return;
    }
    // pop(): pop one slot as an any_t (value + runtime tag) — the mirror of push(), for the variadic
    // constructors (struct.new/array.new_fixed) which pop their fields in the body AFTER allocating, so
    // the fields stay GC-rooted on the stack across the alloc. Decrement sp, then read the slot + tag.
    if (!strcmp(n,"pop")) {
        // Both 64-bit halves when the model carries v128 — an any_t built from .l
        // alone drops lanes 2/3 of a T_V128 slot (the GC-object truncation bug).
        // The slot member letter comes from the spec's type table, never hardcoded.
        char vf = v128_field();
        if (vf) fprintf(out_, "(JV_SP--, (any_t){ .bits = JV_STK[JV_SP].%c.i64[0], .hi = JV_STK[JV_SP].%c.i64[1], .kind = JV_STKT[JV_SP] })", vf, vf);
        else    fputs("(JV_SP--, (any_t){ .bits = JV_STK[JV_SP].l, .kind = JV_STKT[JV_SP] })", out_);
        return;
    }

    // The width the remaining intrinsics select on. Asked for HERE, below the
    // ones that carry a value without computing on it: push/pop move a slot
    // whatever it holds, so their argument need not have an arithmetic width,
    // and asking up front refused them for a number none of them reads.
    int wide = scalar_bits(c_scalar(at)) >= 64;
    int W = scalar_bits(c_scalar(at)), Msk = W - 1;
    const char* u = c_unsigned(at); const char* sc = c_scalar(at);

    const char* fn = nullptr;
    if      (!strcmp(n,"sqrt"))    fn = wide ? "sqrt"  : "sqrtf";
    else if (!strcmp(n,"fabs"))    fn = wide ? "fabs"  : "fabsf";
    else if (!strcmp(n,"ceil"))    fn = wide ? "ceil"  : "ceilf";
    else if (!strcmp(n,"floor"))   fn = wide ? "floor" : "floorf";
    else if (!strcmp(n,"ftrunc"))  fn = wide ? "trunc" : "truncf";
    else if (!strcmp(n,"nearest")) fn = wide ? "rint"  : "rintf";
    if (fn) {
        if (mode_ == Mode::Stencil) fprintf(out_, "((%s(*)(%s))(uintptr_t)_HOLE_%s)", sc, sc, fn);
        else                        fprintf(out_, "%s", fn);
        emit_arg(a0, op, at); return;
    }

    if (!strcmp(n,"copysign")) {
        const char* cs = wide ? "copysign" : "copysignf";
        if (mode_ == Mode::Stencil) fprintf(out_, "((%s(*)(%s, %s))(uintptr_t)_HOLE_%s)(", sc, sc, sc, cs);
        else                        fprintf(out_, "%s(", cs);
        lower_expr(a0, op, at); fputs(", ", out_); lower_expr(a1, op, at); fputc(')', out_); return;
    }
    if (!strcmp(n,"fmin") || !strcmp(n,"fmax")) {
        int mn = !strcmp(n,"fmin"); const char* cmp = mn ? "<" : ">";
        fputs("((isnan", out_); emit_arg(a0,op,at); fputs(" || isnan", out_); emit_arg(a1,op,at);
        fputs(") ? (", out_); lower_expr(a0,op,at); fputs(" + ", out_); lower_expr(a1,op,at);
        fputs(") : (", out_); emit_arg(a0,op,at); fprintf(out_," %s ",cmp); emit_arg(a1,op,at);
        fputs(") ? ", out_); emit_arg(a0,op,at); fputs(" : (", out_);
        emit_arg(a1,op,at); fprintf(out_," %s ",cmp); emit_arg(a0,op,at);
        fputs(") ? ", out_); emit_arg(a1,op,at); fputs(" : (signbit", out_); emit_arg(a0,op,at);
        fputs(") ? ", out_); emit_arg(mn?a0:a1, op, at); fputs(" : ", out_); emit_arg(mn?a1:a0, op, at);
        fputs(")", out_); return;
    }
    if (!strcmp(n,"clz") || !strcmp(n,"ctz")) {
        const char* bi = !strcmp(n,"clz") ? "clz" : "ctz";
        fputc('(', out_); emit_arg(a0,op,at);
        fprintf(out_, " == 0 ? %d : __builtin_%s%s((%s)", W, bi, wide?"ll":"", u);
        emit_arg(a0,op,at); fputs("))", out_); return;
    }
    if (!strcmp(n,"popcnt")) {
        fprintf(out_, "__builtin_popcount%s((%s)", wide?"ll":"", u); emit_arg(a0,op,at); fputc(')', out_); return;
    }
    if (!strcmp(n,"rotl") || !strcmp(n,"rotr")) {
        int left = !strcmp(n,"rotl");
        fprintf(out_, "((%s)(((%s)", sc, u); emit_arg(a0,op,at);
        fprintf(out_, " %s (", left?"<<":">>"); lower_expr(a1,op,at); fprintf(out_, " & %d)) | ((%s)", Msk, u);
        emit_arg(a0,op,at); fprintf(out_, " %s (((%d - (", left?">>":"<<", W);
        lower_expr(a1,op,at); fprintf(out_, " & %d)) & %d)))))", Msk, Msk); return;
    }
    if (!strcmp(n,"reinterpret")) {
        fprintf(out_, "(__extension__({ union { %s s; %s d; } _u; _u.s = ", sc, c_scalar(rt));
        emit_arg(a0,op,at); fputs("; _u.d; }))", out_); return;
    }
}

const char* SemLowerer::api_c_type_for(const char* t, const char* status_ty) {
    if (!strcmp(t, "void"))    return "void";
    if (!strcmp(t, "status"))  return status_ty;
    if (!strcmp(t, "ref"))     return "ref_t";
    if (!strcmp(t, "byte"))    return "s1";
    if (!strcmp(t, "short"))   return "s2";
    if (!strcmp(t, "char"))    return "u2";
    if (!strcmp(t, "int"))     return "s4";
    if (!strcmp(t, "long"))    return "s8";
    // The unsigned widths the action language already accepts (java_c maps them). Left
    // out here, a native declared `ulong` got an s4 prototype — a SILENT narrowing of
    // its signature, which is what a native taking an addrtype value needs to state.
    if (!strcmp(t, "uint"))    return "u4";
    if (!strcmp(t, "ulong"))   return "u8";
    if (!strcmp(t, "float"))   return "f4";
    if (!strcmp(t, "double"))  return "f8";
    if (!strcmp(t, "any"))     return "any_t";
    if (!strcmp(t, "boolean")) return "s1";
    if (!strcmp(t, "v128"))    return "v128_t";
    fprintf(stderr, "opgen: `%s` is not an action-language type\n", t);
    std::exit(1);
}

void SemLowerer::emit_native_fnptr_cast(const MethodDecl* md) {
    fprintf(out_, "(%s(*)(vm_t*, heap_t*", api_c_type(md->ret_ty));
    for (auto* p : md->params)
        fprintf(out_, ", %s", api_c_type(p->ty));
    fputs("))", out_);
}

void SemLowerer::lower_call(const SemExpr* e, const Opcode* op, ValueType rt) {
    auto* call = static_cast<const SCall*>(e);
    if (is_intrinsic(call->name)) { lower_intrinsic(e, op, rt); return; }
    // Domain natives (GC element/field reads, table size, ref tests, …) are NOT special-cased here:
    // they are ordinary `inline native` declarations in the spec, lowered through the generic native
    // path below to a direct `name(NATIVE_ARGS, …)` call that folds a backend `static inline`/macro.
    const MethodDecl* md = find_method(call->name);
    if (!md) {
        fprintf(stderr, "opgen: call to undeclared built-in '%s' in %s\n",
                call->name, op->mnemonic);
        exit(1);   /* dropping the call would emit a body that silently does nothing */
    }
    native_called_ = true;   // a native may set vm->trapped → the stencil must check + bail (see emit_one_opcode)
    if (mode_ == Mode::Stencil && !md->inl) {   // inline natives emit a DIRECT call (folds a static-inline backend fn); regular natives ride a _HOLE_ patch point
        fputs("((", out_); emit_native_fnptr_cast(md);
        fprintf(out_, "(uintptr_t)_HOLE_%s)(NATIVE_ARGS", call->name);
    } else {
        fprintf(out_, "(%s(NATIVE_ARGS", call->name);
    }
    for (auto* a : call->args) { fputs(", ", out_); lower_expr(a, op, rt); }
    fputs("))", out_);
}

// ── Expression lowering ─────────────────────────────────────

void SemLowerer::lower_expr(const SemExpr* e, const Opcode* op, ValueType rt) {
    // A float constant in a stencil reads its named hole instead of being spelled as
    // a C literal, which clang would materialize from the .rodata constant pool — a
    // relocation copy-and-patch cannot carry. Same union pun a literal-init local
    // uses; intercepted BEFORE the switch so `-2147483648.0` is one constant rather
    // than a negation routed through _HOLE_fsignmask.
    uint64_t kbits;
    if (mode_ == Mode::Stencil && in_guard_ && float_const_bits(e, rt, &kbits)) {
        char nm[64]; float_hole_name(kbits, nm, sizeof nm);
        const char* sc = c_scalar(rt);
        const char* ui = scalar_bits(sc) >= 64 ? "u8" : "u4";
        fprintf(out_, "(__extension__({ union { %s i; %s f; } _u; _u.i = (%s)%s; _u.f; }))",
                ui, sc, ui, nm);
        return;
    }
    switch (e->tag) {
    case SemExprTag::SBinOp: {
        auto* b = static_cast<const SBinOp*>(e);
        const char* bop = b->op;
        // Only a shift masks its count, so only a shift asks for the width. A
        // comparison is a legitimate binop over a type that HAS no arithmetic
        // width — `ref_eq`'s `a == b` over two externrefs — and asking up here
        // refused it over a mask the operator never reads.
        int is_shift = !strcmp(bop, ">>>") || !strcmp(bop, "<<") || !strcmp(bop, ">>");
        int shm = is_shift ? scalar_bits(c_scalar(rt)) - 1 : 0;
        if (!strcmp(bop, ">>>")) {
            fprintf(out_, "(((%s)", c_unsigned(rt));
            lower_expr(b->left, op, rt);
            fputs(") >> (", out_); lower_expr(b->right, op, rt); fprintf(out_, " & %d))", shm);
        } else if (!strcmp(bop, "<<") || !strcmp(bop, ">>")) {
            fputc('(', out_); lower_expr(b->left, op, rt);
            fprintf(out_, " %s (", bop); lower_expr(b->right, op, rt); fprintf(out_, " & %d))", shm);
        } else {
            fputc('(', out_); lower_expr(b->left, op, rt);
            fprintf(out_, " %s ", bop);
            lower_expr(b->right, op, rt); fputc(')', out_);
        }
        break;
    }
    case SemExprTag::SUnary: {
        auto* un = static_cast<const SUnary*>(e);
        if (!strcmp(un->op, "-") && c_scalar(rt)[0] == 'f') {
            int W = scalar_bits(c_scalar(rt));
            const char* ui = W >= 64 ? "u8" : "u4";
            fprintf(out_, "(__extension__({ union { %s i; %s f; } _u; _u.f = ", ui, c_scalar(rt));
            lower_expr(un->operand, op, rt);
            if (mode_ == Mode::Stencil) fprintf(out_, "; _u.i ^= (%s)_HOLE_fsignmask%d; _u.f; }))", ui, W);
            else                        fprintf(out_, "; _u.i ^= (%s)1 << %d; _u.f; }))", ui, W - 1);
            break;
        }
        fprintf(out_, "(%s", un->op);
        lower_expr(un->operand, op, rt); fputc(')', out_); break;
    }
    case SemExprTag::STernary: {
        auto* tn = static_cast<const STernary*>(e);
        fputc('(', out_); lower_expr(tn->cond, op, rt);
        fputs(" ? ", out_); lower_expr(tn->then_, op, rt);
        fputs(" : ", out_); lower_expr(tn->else_, op, rt); fputc(')', out_); break;
    }
    case SemExprTag::SCast: {
        auto* ca = static_cast<const SCast*>(e);
        if (!strcmp(ca->ty, "unsigned") || !strcmp(ca->ty, "signed")) {
            ValueType ot = rt;
            if (ca->operand->tag == SemExprTag::SIdent)
                ot = type_of_name(op, static_cast<const SIdent*>(ca->operand)->name);
            if (lane_) { int _ec; const char* _ef; ValueType le = lane_elem(ot, &_ec, &_ef); if (_ec) ot = le; }
            fprintf(out_, "((%s)", ca->ty[0] == 'u' ? c_unsigned(ot) : c_scalar(ot));
            lower_expr(ca->operand, op, ot); fputc(')', out_);
        } else {
            fprintf(out_, "((%s)", java_c(ca->ty));
            lower_expr(ca->operand, op, rt); fputc(')', out_);
        }
        break;
    }
    case SemExprTag::SIndex: {
        auto* ix = static_cast<const SIndex*>(e);
        const SemExpr* base = ix->base;
        const char* bnm = base->tag == SemExprTag::SIdent ? static_cast<const SIdent*>(base)->name : nullptr;
        const StackParam* vp = nullptr;   // §3b variadic stack param? (name[i] reads the in-place operand slice)
        if (bnm) for (auto* si : op->stack_in) if (!strcmp(si->name, bnm) && si->count.has_value()) { vp = si; break; }
        int _lc = 0; const char* _lf = nullptr;
        if (base->tag == SemExprTag::SIdent && !vp)
            lane_elem(type_of_name(op, bnm), &_lc, &_lf);
        if (vp) {   // read the i-th variadic operand in place as an any_t (value + runtime tag);
                    // both halves when the model carries v128 (struct.new of a v128 field / a
                    // v128 tag param must not truncate to the low 64 bits); the slot member
                    // letter comes from the spec's type table
            char vf = v128_field();
            if (vf) {
                fprintf(out_, "((any_t){ .bits = _vb_%s[", bnm);
                lower_expr(ix->index, op, ValueType::TyI32);
                fprintf(out_, "].%c.i64[0], .hi = _vb_%s[", vf, bnm);
                lower_expr(ix->index, op, ValueType::TyI32);
                fprintf(out_, "].%c.i64[1], .kind = _vt_%s[", vf, bnm);
                lower_expr(ix->index, op, ValueType::TyI32);
                fputs("] })", out_);
            } else {
                fprintf(out_, "((any_t){ .bits = _vb_%s[", bnm);
                lower_expr(ix->index, op, ValueType::TyI32);
                fprintf(out_, "].l, .kind = _vt_%s[", bnm);
                lower_expr(ix->index, op, ValueType::TyI32);
                fputs("] })", out_);
            }
        }
        else if (_lc) {
            fprintf(out_, "%s.%s[", static_cast<const SIdent*>(base)->name, _lf);
            lower_expr(ix->index, op, ValueType::TyI32); fputc(']', out_);
        }
        else if (base->tag == SemExprTag::SIdent && !strcmp(static_cast<const SIdent*>(base)->name, "locals"))
            lower_local_read(op, ix->index, rt);
        else if (base->tag == SemExprTag::SIdent && !strcmp(static_cast<const SIdent*>(base)->name, "globals"))
            lower_global_read(op, ix->index, rt);
        else if (base->tag == SemExprTag::SIdent && !strcmp(static_cast<const SIdent*>(base)->name, "code")) {
            fputs("f->code[", out_); lower_expr(ix->index, op, ValueType::TyI32); fputc(']', out_);
        } else {
            fprintf(stderr, "opgen: only locals[]/code[] indexing is supported in %s\n", op->mnemonic);
            exit(1);
        }
        break;
    }
    case SemExprTag::SCall:
        lower_call(e, op, rt); break;
    case SemExprTag::SIdent: {
        auto* id = static_cast<const SIdent*>(e);
        int lc; const char* lf;
        if (!strcmp(id->name, "sp"))      fputs("JV_SP", out_);
        else if (!strcmp(id->name, "stp")) fputs("OPGEN_ST_PTR(f)", out_);   // side-table pointer (backend-relocatable seam): control ops advance it on fall-through
        else if (!strcmp(id->name, "pc")) fputs("f->pc", out_);
        else if (lane_ && !strcmp(id->name, "lane")) fputs("_k", out_);
        else if (lane_ && (lane_elem(type_of_name(op, id->name), &lc, &lf), lc))
            fprintf(out_, "%s.%s[_k]", id->name, lf);
        else                              fputs(id->name, out_);
        break;
    }
    case SemExprTag::SInt: {
        auto* in = static_cast<const SInt*>(e);
        if (in->value > 2147483647LL || in->value < -2147483648LL)
            fprintf(out_, "%lldLL", (long long)in->value);
        else fprintf(out_, "%d", (int)in->value);
        break;
    }
    case SemExprTag::SFloat:
        fprintf(out_, "%.17g", static_cast<const SFloat*>(e)->value); break;
    }
}

// ── Local store + statement-position call ───────────────────

void SemLowerer::lower_local_store(const Opcode* op, const SemExpr* idx, const SemExpr* val) {
    ValueType lt = local_vt(op);
    if (lt == ValueType::TyWord) {
        fputs("    LOCAL_SLOT(", out_); lower_expr(idx, op, lt); fputs(") = ", out_);
        lower_expr(val, op, lt); fputs(";\n", out_);
        if (val->tag == SemExprTag::SIdent) {
            fputs("    LOCAL_TAG_SET(", out_); lower_expr(idx, op, lt);
            fprintf(out_, ", %s_wt);\n", static_cast<const SIdent*>(val)->name);
        }
        return;
    }
    jrow r = jtype(lt);
    if (r.field == 'r') {
        fputs("    LOCAL_SLOT(", out_); lower_expr(idx, op, lt); fputs(").r = (ref_t)(", out_);
        lower_expr(val, op, lt); fputs(");\n", out_);
    } else if (r.slots == 1) {
        fputs("    LOCAL_SLOT(", out_); lower_expr(idx, op, lt);
        fprintf(out_, ").%c = (%s)(", r.field, r.scalar);
        lower_expr(val, op, lt); fputs(");\n", out_);
    } else {
        char ic; const char* islot; const char* uw; int w;
        int_slot_view(&ic, &islot, &uw, &w); (void)uw;
        if (r.scalar[0] == 'f') {
            fprintf(out_, "    { union { %s w; %s u; } _x; _x.w = (", r.scalar, r.uscalar);
            lower_expr(val, op, lt); fputs(");\n", out_);
            fputs("      LOCAL_SLOT(", out_); lower_expr(idx, op, lt);
            fprintf(out_, ").%c = (%s)(_x.u);\n", ic, islot);
            fputs("      LOCAL_SLOT((", out_); lower_expr(idx, op, lt);
            fprintf(out_, ")+1).%c = (%s)(_x.u >> %d); }\n", ic, islot, w);
        } else {
            fprintf(out_, "    { %s _v = (", r.scalar);
            lower_expr(val, op, lt); fputs(");\n", out_);
            fputs("      LOCAL_SLOT(", out_); lower_expr(idx, op, lt);
            fprintf(out_, ").%c = (%s)(_v);\n", ic, islot);
            fputs("      LOCAL_SLOT((", out_); lower_expr(idx, op, lt);
            fprintf(out_, ")+1).%c = (%s)(_v >> %d); }\n", ic, islot, w);
        }
    }
}

void SemLowerer::lower_void_call(const SemExpr* e, const Opcode* op) {
    auto* call = static_cast<const SCall*>(e);
    if (is_intrinsic(call->name)) {   // a statement-level intrinsic (e.g. push(v)) — emit it as a statement
        fputs("    ", out_); lower_intrinsic(e, op, ValueType::TyI32); fputs(";\n", out_);
        return;
    }
    // Statement-level domain natives (GC element/field writes, addr-result push, the control
    // transfers transfer()/func_return()) are ordinary spec declarations: a void `inline native`
    // for the stores/pushes, an `inline native status` for the control transfers (the status return
    // routes them through the generic control path below — bake _HOLE_ip, call, resync). No special
    // case here; the backend supplies each via a `static inline`/macro of the same name.
    const MethodDecl* md = find_method(call->name);
    if (!md) {
        fprintf(stderr, "opgen: call to undeclared built-in '%s' in %s\n",
                call->name, op->mnemonic);
        exit(1);   /* dropping the call would emit a body that silently does nothing */
    }
    native_called_ = true;   // may set vm->trapped; non-status (non-resync) natives need the stencil trap-check
    int ctrl = !strcmp(md->ret_ty, "status");
    if (mode_ == Mode::Stencil && ctrl)
        fputs("    vm->frame.code.pos = (size_t)_HOLE_ip;\n", out_);
    fputs("    ", out_);
    if (mode_ == Mode::Stencil && !md->inl) {   // inline natives emit a DIRECT call (folds a static-inline backend fn); regular natives ride a _HOLE_ patch point
        fputs("((", out_); emit_native_fnptr_cast(md);
        fprintf(out_, "(uintptr_t)_HOLE_%s)(NATIVE_ARGS", call->name);
    } else {
        fprintf(out_, "%s(NATIVE_ARGS", call->name);
    }
    for (auto* a : call->args) { fputs(", ", out_); lower_expr(a, op, ValueType::TyI32); }
    if (mode_ == Mode::Stencil && !md->inl) fputs("));\n", out_);   // close the extra `(` of the _HOLE_ fn-ptr cast; a direct inline-native call has one less
    else                                    fputs(");\n", out_);
    if (ctrl) {
        if (mode_ == Mode::Stencil) fputs("    TAIL return _HOLE_resync(CACHE_PASS);\n", out_);
        else                        fprintf(out_, "    TAIL return %s_next(vm);\n", prefix_.c_str());
    }
}

int SemLowerer::const_eval(const SemExpr* e, const char* ct, uint64_t* bits) const {
    int is_f = ct[0] == 'f', neg = 0;
    const SemExpr* lit = e;
    if (e->tag == SemExprTag::SUnary && !strcmp(static_cast<const SUnary*>(e)->op, "-")) {
        neg = 1; lit = static_cast<const SUnary*>(e)->operand;
    }
    double dv; int64_t iv;
    if      (lit->tag == SemExprTag::SInt)   { iv = static_cast<const SInt*>(lit)->value;   dv = (double)iv; }
    else if (lit->tag == SemExprTag::SFloat) { dv = static_cast<const SFloat*>(lit)->value; iv = (int64_t)dv; }
    else return 0;
    // Only a literal has a width to be taken at. Asking above the tag test
    // refused every NON-literal operand whose type has no arithmetic width,
    // for a width the early return then threw away.
    int w = scalar_bits(ct);
    if (neg) { dv = -dv; iv = -iv; }
    if (is_f) {
        if (w >= 64) memcpy(bits, &dv, 8);
        else { float fv = (float)dv; uint32_t b; memcpy(&b, &fv, 4); *bits = b; }
    } else {
        *bits = (w >= 64) ? (uint64_t)iv : ((uint64_t)iv & 0xffffffffu);
    }
    return 1;
}

// ── Slot-index (stack[]/scratch[]) helpers ──────────────────

bool SemLowerer::is_slot_index(const SemExpr* e) {
    if (e->tag != SemExprTag::SIndex) return false;
    auto* ix = static_cast<const SIndex*>(e);
    if (ix->base->tag != SemExprTag::SIdent) return false;
    const char* b = static_cast<const SIdent*>(ix->base)->name;
    return !strcmp(b, "stack") || !strcmp(b, "scratch");
}
const char* SemLowerer::slot_val_arr(const SemExpr* e) {
    auto* ix = static_cast<const SIndex*>(e);
    return !strcmp(static_cast<const SIdent*>(ix->base)->name, "stack") ? "JV_STK" : "scratch";
}
const char* SemLowerer::slot_tag_arr(const SemExpr* e) {
    auto* ix = static_cast<const SIndex*>(e);
    return !strcmp(static_cast<const SIdent*>(ix->base)->name, "stack") ? "JV_STKT" : "scratch_t";
}

// ── Statement lowering ──────────────────────────────────────

// A literal-init local is a constant (lowered to a read-only `_HOLE_` immediate, e.g. so a float
// literal needn't ride .rodata) ONLY if it is never reassigned. A mutated local — a loop counter
// `k = k - 1` — is a within-stencil temporary that must stay a plain C local (clang allocates it,
// like struct.new's counter); holing it as a baked immediate is wrong, you cannot assign to it.
static bool stmt_assigns(const SemStmt* s, const char* name) {
    if (!s) return false;
    switch (s->tag) {
    case SemStmtTag::SAssign: {
        auto* a = static_cast<const SAssign*>(s);
        return a->target->tag == SemExprTag::SIdent &&
               !strcmp(static_cast<const SIdent*>(a->target)->name, name);
    }
    case SemStmtTag::SIf: {
        auto* i = static_cast<const SIf*>(s);
        return stmt_assigns(i->then_, name) ||
               (i->else_.has_value() && stmt_assigns(*i->else_, name));
    }
    case SemStmtTag::SWhile:
        return stmt_assigns(static_cast<const SWhile*>(s)->body, name);
    case SemStmtTag::SFor: {
        auto* f = static_cast<const SFor*>(s);
        return (f->init.has_value()   && stmt_assigns(*f->init,   name)) ||
               (f->update.has_value() && stmt_assigns(*f->update, name)) ||
               stmt_assigns(f->body, name);
    }
    case SemStmtTag::SBlock: {
        for (auto* st : static_cast<const SBlock*>(s)->stmts)
            if (stmt_assigns(st, name)) return true;
        return false;
    }
    default: return false;
    }
}
static bool body_assigns(const Opcode* op, const char* name) {
    for (size_t i = 0; i < op->sem_body.size(); i++)
        if (stmt_assigns(op->sem_body[i], name)) return true;
    return false;
}

void SemLowerer::lower_stmt(const Opcode* op, const SemStmt* s) {
    switch (s->tag) {
    case SemStmtTag::SAssign: {
        auto* asn = static_cast<const SAssign*>(s);
        const SemExpr* t = asn->target;
        const SemExpr* v = asn->value;
        if (t->tag == SemExprTag::SIdent && !strcmp(static_cast<const SIdent*>(t)->name, "sp")) {
            fputs("    JV_SP = (u1)(", out_); lower_expr(v, op, ValueType::TyI32); fputs(");\n", out_);
        } else if (t->tag == SemExprTag::SIdent && !strcmp(static_cast<const SIdent*>(t)->name, "stp")) {
            fputs("    OPGEN_ST_PTR(f) = (u4)(", out_); lower_expr(v, op, ValueType::TyI32); fputs(");\n", out_);
        } else if (is_slot_index(t)) {
            if (!is_slot_index(v)) {
                fprintf(stderr, "opgen: slot store needs a slot source in %s\n", op->mnemonic);
                exit(1);
                break;
            }
            auto* tx = static_cast<const SIndex*>(t);
            auto* vx = static_cast<const SIndex*>(v);
            fprintf(out_, "    %s[", slot_val_arr(t)); lower_expr(tx->index, op, ValueType::TyI32);
            fprintf(out_, "] = %s[", slot_val_arr(v)); lower_expr(vx->index, op, ValueType::TyI32); fputs("];\n", out_);
            fprintf(out_, "    %s[", slot_tag_arr(t)); lower_expr(tx->index, op, ValueType::TyI32);
            fprintf(out_, "] = %s[", slot_tag_arr(v)); lower_expr(vx->index, op, ValueType::TyI32); fputs("];\n", out_);
        } else if (t->tag == SemExprTag::SIdent) {
            const char* tname = static_cast<const SIdent*>(t)->name;
            ValueType rt = type_of_name(op, tname);
            int _lc; const char* _lf; ValueType et = lane_elem(rt, &_lc, &_lf);
            if (lane_ && _lc) {
                fprintf(out_, "    %s.%s[_k] = (%s)(", tname, _lf, c_scalar(et));
                lower_expr(v, op, et);
                fputs(");\n", out_);
                break;
            }
            // A `word` result is a whole slot. Assigning ANOTHER slot to it (select's
            // `result = c ? v1 : v2`) is a slot move; assigning a SCALAR has to go
            // through the value model's field selector, or C rejects it — that is the
            // "opgen owns the value model but does not expose it" gap. The tag is the
            // body's to set via `<name>_wt`, which is what makes a runtime-width
            // result (memory.size's addrtype) expressible as an honest signature.
            if (rt == ValueType::TyWord && !expr_is_slot_valued(op, v)) {
                fprintf(out_, "    %s.%c = (%s)(", tname, jtype(ValueType::TyI64).field,
                        c_scalar(ValueType::TyI64));
            }
            else if (rt == ValueType::TyAny || rt == ValueType::TyWord || rt == ValueType::TyV128 || is_lane_type(rt))
                fprintf(out_, "    %s = (", tname);
            else
                fprintf(out_, "    %s = (%s)(", tname, c_scalar(rt));
            lower_expr(v, op, rt);
            fputs(");\n", out_);
            const char* src_tag = (rt == ValueType::TyWord) ? slot_index_tag_array(v) : nullptr;
            if (src_tag) {
                fprintf(out_, "    %s_wt = %s[", tname, src_tag);
                lower_expr(static_cast<const SIndex*>(v)->index, op, ValueType::TyI32);
                fputs("];\n", out_);
            }
        } else if (t->tag == SemExprTag::SIndex &&
                   static_cast<const SIndex*>(t)->base->tag == SemExprTag::SIdent &&
                   !strcmp(static_cast<const SIdent*>(static_cast<const SIndex*>(t)->base)->name, "locals")) {
            lower_local_store(op, static_cast<const SIndex*>(t)->index, v);
        } else if (t->tag == SemExprTag::SIndex &&
                   static_cast<const SIndex*>(t)->base->tag == SemExprTag::SIdent &&
                   !strcmp(static_cast<const SIdent*>(static_cast<const SIndex*>(t)->base)->name, "globals")) {
            lower_global_store(op, static_cast<const SIndex*>(t)->index, v);
        } else if (t->tag == SemExprTag::SIndex &&
                   static_cast<const SIndex*>(t)->base->tag == SemExprTag::SIdent &&
                   is_lane_type(type_of_name(op, static_cast<const SIdent*>(static_cast<const SIndex*>(t)->base)->name))) {
            auto* tx = static_cast<const SIndex*>(t);
            const char* bname = static_cast<const SIdent*>(tx->base)->name;
            int _wlc; const char* _wlf;
            ValueType wet = lane_elem(type_of_name(op, bname), &_wlc, &_wlf);
            fprintf(out_, "    %s.%s[", bname, _wlf);
            lower_expr(tx->index, op, ValueType::TyI32);
            fprintf(out_, "] = (%s)(", c_scalar(wet));
            lower_expr(v, op, wet); fputs(");\n", out_);
        } else {
            fprintf(stderr, "opgen: unsupported assignment target in %s\n", op->mnemonic);
            exit(1);
        }
        break;
    }
    case SemStmtTag::SExprStmt: {
        auto* es = static_cast<const SExprStmt*>(s);
        if (es->value->tag == SemExprTag::SCall) {
            lower_void_call(es->value, op);
        } else {
            fprintf(stderr, "opgen: expression statement must be a built-in call in %s\n", op->mnemonic);
            exit(1);
        }
        break;
    }
    case SemStmtTag::SLocalDecl: {
        auto* decl = static_cast<const SLocalDecl*>(s);
        const char* ct = java_c(decl->ty);
        const SemExpr* init = opt_ptr(decl->init);
        uint64_t bits;
        if (mode_ == Mode::Stencil && init && const_eval(init, ct, &bits) && !body_assigns(op, decl->name)) {
            if (ct[0] == 'f') {
                const char* ui = scalar_bits(ct) >= 64 ? "u8" : "u4";
                fprintf(out_, "    %s %s = (__extension__({ union { %s i; %s f; } _u; "
                           "_u.i = (%s)_HOLE_%s; _u.f; }));\n",
                        ct, decl->name, ui, ct, ui, decl->name);
            } else {
                fprintf(out_, "    %s %s = (%s)_HOLE_%s;\n",
                        ct, decl->name, ct, decl->name);
            }
        } else if (init) {
            // The initializer's expected type is the LOCAL's declared type, not a blanket i32 — so a
            // width-sensitive op like reinterpret(f64) into a `long`/`double` local keeps all 64 bits.
            ValueType ivt = ValueType::TyI32;
            if      (!strcmp(decl->ty, "long")  || !strcmp(decl->ty, "ulong")) ivt = ValueType::TyI64;
            else if (!strcmp(decl->ty, "double"))                              ivt = ValueType::TyF64;
            else if (!strcmp(decl->ty, "float"))                               ivt = ValueType::TyF32;
            else if (!strcmp(decl->ty, "any"))                                 ivt = ValueType::TyAny;
            fprintf(out_, "    %s %s = (%s)(", ct, decl->name, ct);
            lower_expr(init, op, ivt);
            fputs(");\n", out_);
        } else {
            fprintf(out_, "    %s %s;\n", ct, decl->name);
        }
        break;
    }
    case SemStmtTag::SIf: {
        auto* sif = static_cast<const SIf*>(s);
        // `(_Bool)` for the same reason the guard emitter uses it: lower_expr already
        // parenthesises a binary operator, so a bare `if (` + expr + `)` yields
        // `if ((c == 0))`, which clang flags as an extraneous-parenthesised equality.
        fputs("    if ((_Bool)", out_); lower_expr(sif->cond, op, ValueType::TyI32); fputs(") {\n", out_);
        lower_stmt(op, sif->then_);
        if (sif->else_.has_value()) { fputs("    } else {\n", out_); lower_stmt(op, *sif->else_); }
        fputs("    }\n", out_);
        break;
    }
    case SemStmtTag::SWhile: {
        auto* sw = static_cast<const SWhile*>(s);
        fputs("    while (", out_); lower_expr(sw->cond, op, ValueType::TyI32); fputs(") {\n", out_);
        lower_stmt(op, sw->body);
        fputs("    }\n", out_);
        break;
    }
    case SemStmtTag::SFor: {
        auto* sf = static_cast<const SFor*>(s);
        fputs("    {\n", out_);
        if (sf->init.has_value()) lower_stmt(op, *sf->init);
        fputs("    while (", out_);
        if (sf->cond.has_value()) lower_expr(*sf->cond, op, ValueType::TyI32); else fputs("1", out_);
        fputs(") {\n", out_);
        lower_stmt(op, sf->body);
        if (sf->update.has_value()) lower_stmt(op, *sf->update);
        fputs("    }\n    }\n", out_);
        break;
    }
    case SemStmtTag::SBlock: {
        auto* sb = static_cast<const SBlock*>(s);
        for (auto* st : sb->stmts) lower_stmt(op, st);
        break;
    }
    case SemStmtTag::STrap:
        if (mode_ == Mode::Stencil) fputs("    TAIL return _HOLE_trap(CACHE_PASS);\n", out_);
        else                        fprintf(out_, "    TAIL return %s_trap(vm);\n", prefix_.c_str());
        break;
    }
}

// ── Analysis the emitters query ─────────────────────────────

int SemLowerer::stmt_returns(const Opcode* op, const SemStmt* s) const {
    switch (s->tag) {
    case SemStmtTag::SExprStmt: {
        const SemExpr* e = static_cast<const SExprStmt*>(s)->value;
        if (e->tag != SemExprTag::SCall) return 0;
        const MethodDecl* md = find_method(static_cast<const SCall*>(e)->name);
        return md && !strcmp(md->ret_ty, "status");
    }
    case SemStmtTag::STrap: return 1;
    case SemStmtTag::SBlock: {
        auto* sb = static_cast<const SBlock*>(s);
        return !sb->stmts.empty() && stmt_returns(op, sb->stmts.back());
    }
    case SemStmtTag::SIf: {
        auto* sif = static_cast<const SIf*>(s);
        return sif->else_.has_value() && stmt_returns(op, sif->then_) &&
               stmt_returns(op, *sif->else_);
    }
    default:
        return 0;
    }
}

int SemLowerer::body_tail_returns(const Opcode* op) const {
    return !op->sem_body.empty() && stmt_returns(op, op->sem_body.back());
}

void SemLowerer::collect_natives_expr(const SemExpr* e, const char** names, int* n) const {
    if (!e) return;
    switch (e->tag) {
    case SemExprTag::SCall: {
        auto* call = static_cast<const SCall*>(e);
        const MethodDecl* md = find_method(call->name);
        if (md && !md->inl) {   // inline natives are direct calls — no _HOLE_ patch point, so no hole to collect/declare
            int dup = 0;
            for (int i = 0; i < *n; i++) if (!strcmp(names[i], call->name)) { dup = 1; break; }
            if (!dup) names[(*n)++] = call->name;
        }
        for (auto* a : call->args) collect_natives_expr(a, names, n);
        return;
    }
    case SemExprTag::SBinOp: {
        auto* b = static_cast<const SBinOp*>(e);
        collect_natives_expr(b->left, names, n);
        collect_natives_expr(b->right, names, n); return;
    }
    case SemExprTag::SUnary:
        collect_natives_expr(static_cast<const SUnary*>(e)->operand, names, n); return;
    case SemExprTag::STernary: {
        auto* tn = static_cast<const STernary*>(e);
        collect_natives_expr(tn->cond, names, n);
        collect_natives_expr(tn->then_, names, n);
        collect_natives_expr(tn->else_, names, n); return;
    }
    case SemExprTag::SCast:
        collect_natives_expr(static_cast<const SCast*>(e)->operand, names, n); return;
    case SemExprTag::SIndex: {
        auto* ix = static_cast<const SIndex*>(e);
        collect_natives_expr(ix->base, names, n);
        collect_natives_expr(ix->index, names, n); return;
    }
    default: return;
    }
}

void SemLowerer::collect_natives_stmt(const SemStmt* s, const char** names, int* n) const {
    if (!s) return;
    switch (s->tag) {
    case SemStmtTag::SAssign: {
        auto* asn = static_cast<const SAssign*>(s);
        collect_natives_expr(asn->target, names, n);
        collect_natives_expr(asn->value, names, n); return;
    }
    case SemStmtTag::SExprStmt:
        collect_natives_expr(static_cast<const SExprStmt*>(s)->value, names, n); return;
    case SemStmtTag::SLocalDecl:
        collect_natives_expr(opt_ptr(static_cast<const SLocalDecl*>(s)->init), names, n); return;
    case SemStmtTag::SIf: {
        auto* sif = static_cast<const SIf*>(s);
        collect_natives_expr(sif->cond, names, n);
        collect_natives_stmt(sif->then_, names, n);
        collect_natives_stmt(opt_ptr(sif->else_), names, n); return;
    }
    case SemStmtTag::SWhile: {
        auto* sw = static_cast<const SWhile*>(s);
        collect_natives_expr(sw->cond, names, n);
        collect_natives_stmt(sw->body, names, n); return;
    }
    case SemStmtTag::SFor: {
        auto* sf = static_cast<const SFor*>(s);
        collect_natives_stmt(opt_ptr(sf->init), names, n);
        collect_natives_expr(opt_ptr(sf->cond), names, n);
        collect_natives_stmt(opt_ptr(sf->update), names, n);
        collect_natives_stmt(sf->body, names, n); return;
    }
    case SemStmtTag::SBlock: {
        auto* sb = static_cast<const SBlock*>(s);
        for (auto* st : sb->stmts) collect_natives_stmt(st, names, n);
        return;
    }
    default: return;
    }
}

int SemLowerer::op_has_flag(const Opcode* op, const char* f) {
    for (auto* fl : op->flags) if (!strcmp(fl, f)) return 1;
    return 0;
}

int SemLowerer::stencil_excluded(const Opcode* op) {
    return op_has_flag(op, "no_jit");
}

// A fired guard reports WHICH error fired, then tails to the trap continuation.
// The declared name reaches the backend as an OPGEN_ERR_* code through the
// OPGEN_GUARD_TRAP seam; the generated default discards it, so a backend with a
// single undifferentiated trap needs no change, and one that distinguishes trap
// causes (wasm's "integer divide by zero" vs "integer overflow") overrides it.
void SemLowerer::guard_trap(const char* error_name) {
    fprintf(out_, " { OPGEN_GUARD_TRAP(vm, OPGEN_ERR_%s);", error_name);
    if (mode_ == Mode::Stencil) fputs(" TAIL return _HOLE_trap(CACHE_PASS); }\n", out_);
    else fprintf(out_, " TAIL return %s_trap(vm); }\n", prefix_.c_str());
}

const SemExpr* SemLowerer::body_call(const Opcode* op) {
    for (auto* s : op->sem_body) {
        const SemExpr* e =
            s->tag == SemStmtTag::SAssign   ? static_cast<const SAssign*>(s)->value :
            s->tag == SemStmtTag::SExprStmt ? static_cast<const SExprStmt*>(s)->value : nullptr;
        if (e && e->tag == SemExprTag::SCall) return e;
    }
    return nullptr;
}

int SemLowerer::expr_has_unary_minus(const SemExpr* e) {
    if (!e) return 0;
    switch (e->tag) {
    case SemExprTag::SUnary: {
        auto* un = static_cast<const SUnary*>(e);
        if (!strcmp(un->op, "-")) return 1;
        return expr_has_unary_minus(un->operand);
    }
    case SemExprTag::SBinOp: {
        auto* b = static_cast<const SBinOp*>(e);
        return expr_has_unary_minus(b->left) || expr_has_unary_minus(b->right);
    }
    case SemExprTag::STernary: {
        auto* tn = static_cast<const STernary*>(e);
        return expr_has_unary_minus(tn->cond) || expr_has_unary_minus(tn->then_)
            || expr_has_unary_minus(tn->else_);
    }
    case SemExprTag::SCast:  return expr_has_unary_minus(static_cast<const SCast*>(e)->operand);
    case SemExprTag::SIndex: {
        auto* ix = static_cast<const SIndex*>(e);
        return expr_has_unary_minus(ix->base) || expr_has_unary_minus(ix->index);
    }
    case SemExprTag::SCall: {
        for (auto* a : static_cast<const SCall*>(e)->args)
            if (expr_has_unary_minus(a)) return 1;
        return 0;
    }
    default: return 0;
    }
}

int SemLowerer::expr_refs_name(const SemExpr* e, const char* name) {
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

// lower_expr threads ONE result type down both sides of an operator — opgen has
// no per-node type inference, for guards or for bodies. So a condition is
// lowered at the type of the first stack input or operand it names, in
// declaration order, defaulting to i32 when it names none (a condition over
// only literals). This is what lets ONE declared condition cover a polymorphic
// `( T a, T b -- T )` family: the family expands to concrete opcodes first, so
// `b == 0` resolves to i32 or i64 per expansion.
ValueType SemLowerer::cond_type(const Opcode* op, const SemExpr* cond) {
    for (auto* s : op->stack_in)
        if (expr_refs_name(cond, s->name)) return s->ty;
    for (auto* od : op->operands)
        if (expr_refs_name(cond, od->name)) return od->ty;
    return ValueType::TyI32;
}

// ── Float constants inside a guard ──────────────────────────
//
// A float literal lowers to a plain C literal, which clang materializes from the
// .rodata constant pool — a relocation a copy-and-patch stencil cannot carry. In a
// BODY that never bites, because a literal-init local becomes a named `_HOLE_`
// (op_const_holes below). A CONDITION has no local to hang a name on, so the
// literal needs a synthesized one.
//
// The name is derived from the VALUE (`_HOLE_k<bit pattern>`) rather than from a
// walk index, so the hole table and the emitter cannot disagree about which
// literal is which — no ordering contract between them, and two occurrences of the
// same constant share one hole.
//
// `-2147483648.0` is matched as ONE constant, not as a negation of one: folding the
// sign in keeps it off the `_HOLE_fsignmask` path, whose hole is only declared for
// unary minus appearing in a body.
int SemLowerer::float_const_bits(const SemExpr* e, ValueType rt, uint64_t* bits) const {
    if (!e) return 0;
    const char* sc = c_scalar(rt);
    if (sc[0] != 'f') return 0;                       // only float constants need a hole
    if (e->tag == SemExprTag::SUnary) {
        auto* u = static_cast<const SUnary*>(e);
        if (strcmp(u->op, "-") || !u->operand || u->operand->tag != SemExprTag::SFloat) return 0;
    } else if (e->tag != SemExprTag::SFloat) {
        return 0;
    }
    return const_eval(e, sc, bits);   // c_scalar ("f4"/"f8") is the form const_eval reads
}

// Does this guard still negate a float at RUNTIME? A negated float LITERAL folds
// into one constant hole (float_const_bits) and never reaches the sign-mask path, so
// asking `expr_has_unary_minus` alone would declare a mask nothing references.
int SemLowerer::expr_has_live_float_negate(const SemExpr* e, ValueType rt) const {
    if (!e) return 0;
    uint64_t bits;
    if (float_const_bits(e, rt, &bits)) return 0;      // folded; not a runtime negate
    switch (e->tag) {
    case SemExprTag::SUnary: {
        auto* u = static_cast<const SUnary*>(e);
        if (!strcmp(u->op, "-")) return 1;
        return expr_has_live_float_negate(u->operand, rt);
    }
    case SemExprTag::SBinOp: {
        auto* b = static_cast<const SBinOp*>(e);
        return expr_has_live_float_negate(b->left, rt) || expr_has_live_float_negate(b->right, rt);
    }
    case SemExprTag::STernary: {
        auto* t = static_cast<const STernary*>(e);
        return expr_has_live_float_negate(t->cond, rt) || expr_has_live_float_negate(t->then_, rt) ||
               expr_has_live_float_negate(t->else_, rt);
    }
    case SemExprTag::SCast:
        return expr_has_live_float_negate(static_cast<const SCast*>(e)->operand, rt);
    case SemExprTag::SIndex: {
        auto* ix = static_cast<const SIndex*>(e);
        return expr_has_live_float_negate(ix->base, rt) || expr_has_live_float_negate(ix->index, rt);
    }
    case SemExprTag::SCall:
        for (auto* a : static_cast<const SCall*>(e)->args)
            if (expr_has_live_float_negate(a, rt)) return 1;
        return 0;
    default: return 0;
    }
}

void SemLowerer::float_hole_name(uint64_t bits, char* out, size_t cap) {
    snprintf(out, cap, "_HOLE_k%016llx", (unsigned long long)bits);
}

// Walk a guard for float constants, in pre-order, calling `hit` on each.
void SemLowerer::walk_float_consts(const SemExpr* e, ValueType rt,
                                   void* ctx, void (*hit)(void*, uint64_t)) const {
    if (!e) return;
    uint64_t bits;
    if (float_const_bits(e, rt, &bits)) { hit(ctx, bits); return; }   // one constant: do not recurse
    switch (e->tag) {
    case SemExprTag::SBinOp: {
        auto* b = static_cast<const SBinOp*>(e);
        walk_float_consts(b->left, rt, ctx, hit); walk_float_consts(b->right, rt, ctx, hit); return;
    }
    case SemExprTag::SUnary:
        walk_float_consts(static_cast<const SUnary*>(e)->operand, rt, ctx, hit); return;
    case SemExprTag::STernary: {
        auto* t = static_cast<const STernary*>(e);
        walk_float_consts(t->cond, rt, ctx, hit); walk_float_consts(t->then_, rt, ctx, hit);
        walk_float_consts(t->else_, rt, ctx, hit); return;
    }
    case SemExprTag::SCast:
        walk_float_consts(static_cast<const SCast*>(e)->operand, rt, ctx, hit); return;
    case SemExprTag::SIndex: {
        auto* ix = static_cast<const SIndex*>(e);
        walk_float_consts(ix->base, rt, ctx, hit); walk_float_consts(ix->index, rt, ctx, hit); return;
    }
    case SemExprTag::SCall:
        for (auto* a : static_cast<const SCall*>(e)->args) walk_float_consts(a, rt, ctx, hit);
        return;
    default: return;
    }
}

/* The hole table is fixed at OP_CONST_HOLES entries because that is the caller's array
 * size. Overflowing it is FATAL, never a silent truncation: the stencil still references
 * the dropped hole by name, and the JIT driver, finding nothing to patch, bakes 0 — a
 * wrong stencil rather than a missing one, and wrong in JIT code only. */
static void hole_room_or_die(const Opcode* op, int n, const char* nm) {
    if (n < SemLowerer::OP_CONST_HOLES) return;
    fprintf(stderr, "opgen: %s: more than %d constant holes (adding '%s') — the stencil "
                    "hole table is full and a dropped hole would be baked as 0\n",
            op->mnemonic, SemLowerer::OP_CONST_HOLES, nm);
    exit(1);
}

int SemLowerer::op_const_holes(const Opcode* op, const char* names[OP_CONST_HOLES],
                               uint64_t vals[OP_CONST_HOLES]) const {
    static char buf[OP_CONST_HOLES][64];
    int n = 0;
    for (size_t i = 0; i < op->sem_body.size(); i++) {
        const SemStmt* s = op->sem_body[i];
        if (s->tag != SemStmtTag::SLocalDecl) continue;
        auto* decl = static_cast<const SLocalDecl*>(s);
        if (!decl->init.has_value()) continue;
        uint64_t bits;
        if (!const_eval(*decl->init, java_c(decl->ty), &bits)) continue;
        if (body_assigns(op, decl->name)) continue;   // a reassigned local is a temp, not a constant hole
        hole_room_or_die(op, n, decl->name);
        snprintf(buf[n], sizeof buf[n], "_HOLE_%s", decl->name);
        names[n] = buf[n]; vals[n] = bits; n++;
    }
    /* Float constants named by a guard. Keyed by value, so a repeated constant
     * costs one hole and the emitter finds it under the same name. */
    static char fbuf[OP_CONST_HOLES][64];
    struct Ctx { const Opcode* op; const char** names; uint64_t* vals; int* n; }
        cx = { op, names, vals, &n };
    for (auto* e : op->errors) {
        if (!e->condition.has_value()) continue;
        walk_float_consts(*e->condition, cond_type(op, *e->condition), &cx,
                          [](void* v, uint64_t bits) {
            auto* c = (Ctx*)v;
            char nm[64]; float_hole_name(bits, nm, sizeof nm);
            for (int i = 0; i < *c->n; i++) if (!strcmp(c->names[i], nm)) return;
            hole_room_or_die(c->op, *c->n, nm);
            snprintf(fbuf[*c->n], sizeof fbuf[0], "%s", nm);
            c->names[*c->n] = fbuf[*c->n]; c->vals[*c->n] = bits; (*c->n)++;
        });
    }

    /* The float sign mask, when a GUARD negates a float. The body scan below cannot
     * cover this: it keys on the RESULT type, and a float compare has an i32 result
     * (i32.trunc_f32_s), so the width has to come from the condition. Without this the
     * stencil references _HOLE_fsignmask32 and nothing declares it — jitterator waves
     * it through on the name prefix and the driver bakes 0, silently dropping the sign
     * flip in JIT code only. */
    for (auto* e : op->errors) {
        if (!e->condition.has_value()) continue;
        ValueType ct = cond_type(op, *e->condition);
        if (c_scalar(ct)[0] != 'f' || !expr_has_live_float_negate(*e->condition, ct)) continue;
        int W = scalar_bits(c_scalar(ct));
        const char* nm = W >= 64 ? "_HOLE_fsignmask64" : "_HOLE_fsignmask32";
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp(names[i], nm)) dup = 1;
        if (dup) continue;
        hole_room_or_die(op, n, nm);
        names[n] = nm; vals[n] = (uint64_t)1 << (W - 1); n++;
    }

    if (!op->stack_out.empty()) {
        int lc; const char* lf;
        ValueType le = lane_elem(op->stack_out[0]->ty, &lc, &lf);
        ValueType et = lc ? le : op->stack_out[0]->ty;
        if (c_scalar(et)[0] == 'f') {
            for (size_t i = 0; i < op->sem_body.size(); i++) {
                const SemStmt* s = op->sem_body[i];
                const SemExpr* v = s->tag == SemStmtTag::SAssign   ? static_cast<const SAssign*>(s)->value
                                 : s->tag == SemStmtTag::SExprStmt ? static_cast<const SExprStmt*>(s)->value : nullptr;
                if (v && expr_has_unary_minus(v)) {
                    int W = scalar_bits(c_scalar(et));
                    const char* nm = W >= 64 ? "_HOLE_fsignmask64" : "_HOLE_fsignmask32";
                    int dup = 0;
                    for (int i2 = 0; i2 < n; i2++) if (!strcmp(names[i2], nm)) dup = 1;
                    if (dup) break;
                    hole_room_or_die(op, n, nm);
                    names[n] = nm; vals[n] = (uint64_t)1 << (W - 1); n++;
                    break;
                }
            }
        }
    }
    return n;
}

} // namespace opgen
