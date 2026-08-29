#ifndef OPGEN_REFEMIT_H
#define OPGEN_REFEMIT_H

#include "semlower.h"
#include <cstdio>
#include <string>

namespace opgen {

// Emits `<prefix>_ref.h`: one pure C reference kernel per opcode with a CLOSED
// body — the spec's semantics as a standalone function of its declared stack
// inputs and decoded operands, independent of any VM substrate. Verification
// and differential harnesses compute expected values with these and compare a
// real implementation against them.
class RefEmitter {
public:
    RefEmitter(const Module* mod, std::string prefix)
        : mod_(mod), prefix_(std::move(prefix)) {}

    void emit_ref_h(FILE* o);

    // Kernel eligibility: nullptr when the op's body is CLOSED (a pure
    // function of its declared inputs); otherwise a static string naming the
    // first reason it is open. Shared with the VC emitter — the two consumers
    // of "is this body a pure function?" must agree, so it lives once.
    static const char* open_reason(const SemLowerer& low, const Opcode* op);

private:
    const Module* mod_;
    std::string   prefix_;

    static const char* param_open_reason(const SemLowerer& low, ValueType ty);
    static const char* expr_open_reason(const SemLowerer& low, const SemExpr* e);
    static const char* stmt_open_reason(const SemLowerer& low, const SemStmt* s);
    void emit_kernel(SemLowerer& low, const Opcode* op, FILE* o,
                     const std::string& up);
};

} // namespace opgen

#endif
