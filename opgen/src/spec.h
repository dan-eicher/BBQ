#ifndef OPGEN_SPEC_H
#define OPGEN_SPEC_H

#include "opgen_ast.h"
#include <array>

namespace opgen {

// Bit positions in opcode_flags[] (matches OPCF_* in the generated opcodes.h).
constexpr unsigned OPCF_BRANCH      = 0x01u;   // has a branch target (cond or uncond)
constexpr unsigned OPCF_RETURN      = 0x02u;   // {s,i,a,}return
constexpr unsigned OPCF_INVOKE      = 0x04u;   // mnemonic starts with "invoke"
constexpr unsigned OPCF_SWITCH      = 0x08u;   // {s,i}{table,lookup}switch
constexpr unsigned OPCF_HAS_CP_REF  = 0x10u;   // any operand is a CP reference
constexpr unsigned OPCF_ENDS_BB     = 0x20u;   // control flow does not fall through
constexpr unsigned OPCF_LOCAL_LOAD  = 0x40u;   // pushes a local-variable value
constexpr unsigned OPCF_LOCAL_STORE = 0x80u;   // writes a local variable

// Sentinel for opcode_stack_delta[]: the per-opcode delta cannot be encoded
// statically (invokes, dup_x, swap_x, switches, jsr, ret) — computed at the
// call site (CP signature, payload, …).
constexpr int OPCD_VARIABLE = -128;
// Sentinel for opcode_length[]: length depends on payload (switches).
constexpr int OPCL_VARIABLE = 0;

// The opcode-spec model: the parsed Module plus the per-opcode facts derived
// from it (length, stack effect, flags, …), indexed by opcode byte 0..255.
// Owns both — replaces the C global `derived_table` + `g_dmod`.
class Spec {
public:
    // Per-opcode derived fields; `present` is false for unallocated slots.
    struct Derived {
        bool          present = false;
        const Opcode* ast = nullptr;
        int           length = 0;          // OPCL_VARIABLE for switches
        int           sp_delta = 0;        // OPCD_VARIABLE for specials
        unsigned      flags = 0;           // OPCF_* bitmask
        int           cp_ref_offset = 0;   // 0 if no CP-ref operand
        int           sp_pops = 0;         // cells popped; OPCD_VARIABLE for per-call-site
    };

    explicit Spec(const Module* m);

    const Module*                     module() const { return mod_; }
    const Derived&                    derived(unsigned idx) const { return derived_[idx & 0xff]; }
    const std::array<Derived, 256>&   table() const { return derived_; }

    // Byte width of a value type (the decoded carrier width for LEB/memarg).
    static int type_bytes(ValueType ty);
    // Stack-slot count, following the spec's `type` table where declared.
    int stack_slots(ValueType ty) const;

private:
    const Module*            mod_;
    std::array<Derived, 256> derived_{};

    int      compute_length(const Opcode* op) const;
    int      compute_sp_delta(const Opcode* op) const;
    int      compute_sp_pops(const Opcode* op) const;
    unsigned compute_flags(const Opcode* op) const;
    int      compute_cp_ref_offset(const Opcode* op) const;
};

} // namespace opgen

#endif
