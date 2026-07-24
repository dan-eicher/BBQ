#ifndef OPGEN_SEMLOWER_H
#define OPGEN_SEMLOWER_H

#include "opgen_ast.h"
#include <cstdio>
#include <cstdint>
#include <string>

namespace opgen {

// Per value_type facts: the C scalar, its unsigned counterpart, the frame/stack
// slot count, and the slot field char ('s' short slot, 'r' ref, 'i' int, 'f'
// float, 'v' v128). The single source for every width/cast/slot access.
struct jrow { const char* scalar; const char* uscalar; int slots; char field; };

// Lowers an opcode's `sem_expr`/`sem_stmt` body to C, ONCE, for either consumer:
// the in-place interpreter handler (Mode::Interp) or the jitterator stencil
// (Mode::Stencil, _HOLE_ immediates + indirect calls). `mode`/`lane` are state
// (replacing the C globals g_mode/g_lane); `mod_` resolves the type table,
// `status`, and native methods (replacing g_mod). Writes to `out_`.
class SemLowerer {
public:
    enum class Mode { Interp, Stencil };

    // How many baked constant holes one opcode's stencil can carry. Overflowing it is a
    // hard error (see hole_room_or_die) — the stencil names every hole it uses, so a
    // dropped one is patched with 0 rather than left absent.
    static constexpr int OP_CONST_HOLES = 8;

    SemLowerer(const Module* mod, FILE* out, Mode mode, std::string prefix)
        : mod_(mod), out_(out), mode_(mode), prefix_(std::move(prefix)) {}

    Mode mode() const { return mode_; }
    // The v128 row's slot-union member letter, or 0 when the model declares no
    // v128 — the ONE test for "does the any carrier need both 64-bit halves",
    // spelled from the spec's own type table (never a hardcoded member name).
    char v128_field() const {
        for (auto* td : mod_->types)
            if (td->ty == ValueType::TyV128) return jtype(ValueType::TyV128).field;
        return 0;
    }
    void set_in_guard(bool b) { in_guard_ = b; }
    void set_lane(int on) { lane_ = on; }
    int  lane() const { return lane_; }
    FILE* out() const { return out_; }
    // A body that calls a native may set vm->trapped; the stencil emitter uses this to bail (the
    // JIT has no implicit code.pos check, unlike the interp's next-dispatch). Reset per opcode.
    void clear_native_called() { native_called_ = false; }
    bool native_called() const { return native_called_; }

    // ── The lowering ───────────────────────────────────────
    void lower_expr(const SemExpr* e, const Opcode* op, ValueType rt);
    void lower_stmt(const Opcode* op, const SemStmt* s);

    // ── Type model (used by lowering + the emitters) ───────
    jrow        jtype(ValueType t) const;
    const char* c_scalar(ValueType t) const   { return jtype(t).scalar; }
    const char* c_unsigned(ValueType t) const { return jtype(t).uscalar; }
    static const char* java_c(const char* jt);
    ValueType   local_vt(const Opcode* op) const;
    ValueType   type_of_name(const Opcode* op, const char* name) const;
    static const char* fetch_expr(ValueType t);
    static const char* fetch_fn(ValueType t);
    static ValueType lane_elem(ValueType t, int* count, const char** ufield);
    static bool is_lane_type(ValueType t);
    static const char* slot_class(ValueType t);
    static int  scalar_bits(const char* s);
    void int_slot_view(char* fld, const char** islot, const char** uw, int* w) const;

    // ── Status vocabulary + native resolution ──────────────
    const char* status_type() const;
    const char* status_ok() const;
    const char* status_err() const;
    const char* status_halt() const;
    const MethodDecl* find_method(const char* name) const;
    static const char* api_c_type_for(const char* t, const char* status_ty);
    const char* api_c_type(const char* t) const { return api_c_type_for(t, status_type()); }

    // ── Analysis the emitters query ────────────────────────
    int  expr_is_slot_valued(const Opcode* op, const SemExpr* e) const;
    static int  const_eval(const SemExpr* e, const char* ct, uint64_t* bits);
    static int  expr_refs_name(const SemExpr* e, const char* name);
    // The type an `error:` condition is lowered at — lives here because both the
    // guard emitter and op_const_holes need it, and it is an expression question.
    static ValueType cond_type(const Opcode* op, const SemExpr* cond);
    // A float constant inside a guard needs a named `_HOLE_`, or it rides .rodata and
    // the stencil is unpatchable. Name is derived from the value so the hole table and
    // the emitter agree without sharing a walk order.
    int  float_const_bits(const SemExpr* e, ValueType rt, uint64_t* bits) const;
    int  expr_has_live_float_negate(const SemExpr* e, ValueType rt) const;
    static void float_hole_name(uint64_t bits, char* out, size_t cap);
    void walk_float_consts(const SemExpr* e, ValueType rt,
                           void* ctx, void (*hit)(void*, uint64_t)) const;
    int  op_const_holes(const Opcode* op, const char* names[OP_CONST_HOLES],
                        uint64_t vals[OP_CONST_HOLES]) const;
    void collect_natives_expr(const SemExpr* e, const char** names, int* n) const;
    void collect_natives_stmt(const SemStmt* s, const char** names, int* n) const;
    int  stmt_returns(const Opcode* op, const SemStmt* s) const;
    int  body_tail_returns(const Opcode* op) const;
    static int  op_has_flag(const Opcode* op, const char* f);
    static int  stencil_excluded(const Opcode* op);
    static const SemExpr* body_call(const Opcode* op);
    static int  expr_has_unary_minus(const SemExpr* e);
    void guard_trap(const char* error_name);
    void emit_native_fnptr_cast(const MethodDecl* md);

private:
    const Module* mod_;
    FILE*         out_;
    Mode          mode_;
    // Set only while an `error:` condition is being lowered. Float constants take
    // their `_HOLE_` path there; a BODY constant already has one (a literal-init
    // local), so bodies are left exactly as they were.
    bool          in_guard_ = false;
    int           lane_ = 0;
    bool          native_called_ = false;
    std::string   prefix_;

    // expression sub-lowerings
    void lower_global_read(const Opcode* op, const SemExpr* idx, ValueType rt);
    void lower_global_store(const Opcode* op, const SemExpr* idx, const SemExpr* val);
    void lower_local_read(const Opcode* op, const SemExpr* idx, ValueType rt);
    void lower_local_store(const Opcode* op, const SemExpr* idx, const SemExpr* val);
    void lower_call(const SemExpr* e, const Opcode* op, ValueType rt);
    void lower_intrinsic(const SemExpr* e, const Opcode* op, ValueType rt);
    void lower_void_call(const SemExpr* e, const Opcode* op);
    void emit_arg(const SemExpr* a, const Opcode* op, ValueType at);
    ValueType arg_type(const SemExpr* e, const Opcode* op, ValueType rt) const;
    const char* slot_index_tag_array(const SemExpr* e) const;
    static bool is_intrinsic(const char* n);
    static bool is_slot_index(const SemExpr* e);
    static const char* slot_val_arr(const SemExpr* e);
    static const char* slot_tag_arr(const SemExpr* e);
};

} // namespace opgen

#endif
