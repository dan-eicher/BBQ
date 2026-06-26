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
    VmEmitter(const Module* mod, std::string prefix)
        : mod_(mod), prefix_(std::move(prefix)) {
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

    // Handler emission (the interp/stencil body wrapper around the lowering).
    void emit_one_opcode(SemLowerer& low, const Opcode* op);
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
    static int  expr_refs_name(const SemExpr* e, const char* name);
    static int  stmt_refs_name(const SemStmt* s, const char* name);
    static int  stack_in_live(const Opcode* op, int k);
    static const char* out_sem_expr(const StackParam* sp);   // optional<const char*> → ptr
    static const SemExpr* out_count(const StackParam* sp);   // §3b variadic pop-count expr → ptr (or null)
};

} // namespace opgen

#endif
