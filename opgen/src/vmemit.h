#ifndef OPGEN_VMEMIT_H
#define OPGEN_VMEMIT_H

#include "opgen_ast.h"
#include "semlower.h"
#include <cstdio>
#include <string>
#include <cctype>

namespace opgen {

// Emits the 8 VM/JIT/validator outputs from emit_interp_c.cpp:
//   gen_interp.c, wasm_stencils.c, wasm_jit_symbols.h, wasm_jit_meta.h,
//   wasm_valtype.h, wasm_type_meta.h, gen_interp.h, runtime_api.h
// Drives a SemLowerer (per-output mode) for the handler/stencil bodies.
class VmEmitter {
public:
    // Does a body mention this name? Public because the emission helpers ask it.
    static int  stmt_refs_name(const SemStmt* s, const char* name);

    // `tier2_n` is Ertl's cache size: how many operand-stack slots ride in
    // registers. 0 emits exactly what it always did — the stencil table is then
    // byte-identical, which is the point of it being a generator flag rather
    // than a #ifdef in the emitted C.
    VmEmitter(const Module* mod, std::string prefix, int tier2_n = 0)
        : mod_(mod), prefix_(std::move(prefix)), tier2_n_(tier2_n) {
        for (char c : prefix_) uprefix_ += (char)toupper((unsigned char)c);
    }

    void emit_interp_c(FILE* o);       // gen_interp.c
    void emit_stencil_c(FILE* o);      // wasm_stencils.c
    void emit_jit_symbols(FILE* o);    // wasm_jit_symbols.h
    void emit_jit_meta(FILE* o);       // wasm_jit_meta.h
    void emit_valtype_h(FILE* o);      // wasm_valtype.h
    void emit_type_meta(FILE* o);      // wasm_type_meta.h
    void emit_interp_h(FILE* o);       // gen_interp.h
    void emit_runtime_api_h(FILE* o);  // runtime_api.h

private:
    const Module* mod_;
    std::string prefix_;
    std::string uprefix_;
    int tier2_n_ = 0;

    // Ertl's MINIMAL organization (§2.3): a state is the NUMBER of operand-stack
    // items held in registers, top-first, so state k means the top k are in
    // r0..r[k-1]. The follow-up state falls out of the arity: the instruction
    // consumes what it consumes and its result becomes the new top.
    //
    // Every stencil carries the same slot list whatever its state, because
    // musttail requires the caller's and callee's signatures to MATCH — a
    // per-state signature is rejected outright by clang. The state is therefore
    // which slots are LIVE, which the tiler tracks; the signature never varies.
    int  cache_slots() const { return tier2_n_; }
    static int follow_state(const Opcode* op, int state, int nslots);
    // Is operand k (counted from the bottom of this instruction's operands) in a
    // register in this state, and if so which one?
    static int operand_slot(const Opcode* op, int k, int state);
    // A class that never enters a slot: a managed reference belongs on the stack,
    // where the collector expects to find it and to be able to relocate it.
    static bool cacheable(ValueType t);
    // Does the family carry this opcode in this entry state? Asked by both the
    // emission loop and the variant table, so they cannot disagree.
    bool emits_variant(const Opcode* op, int state) const;

    // Handler emission (the interp/stencil body wrapper around the lowering).
    // `state` is the entry cache state; -1 is "no cache" (the interp, and every
    // tier-1 stencil), which emits exactly what it always did.
    void emit_one_opcode(SemLowerer& low, const Opcode* op, int state = -1);
    void emit_guards(SemLowerer& low, const Opcode* op);
    void emit_slot_macros(SemLowerer& low);
    void emit_prologue(SemLowerer& low);
    void emit_value_model(SemLowerer& low, FILE* o);

    static bool is_emittable(const Opcode* op);
    static void put_upper(FILE* o, const char* s);
    void put_p(FILE* o, const char* s);
    static const char* tag_for(jrow r);
    static const char* jop_kind(ValueType t);
    static const char* wvt_name(ValueType t);
    static const char* ht_name(ValueType t);
    static int  stack_in_live(const Opcode* op, int k);
    static const char* out_sem_expr(const StackParam* sp);   // optional<const char*> → ptr
    static const SemExpr* out_count(const StackParam* sp);   // §3b variadic pop-count expr → ptr (or null)
};

} // namespace opgen

#endif
