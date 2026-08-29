#ifndef OPGEN_VCEMIT_H
#define OPGEN_VCEMIT_H

#include "semlower.h"
#include <cstdio>
#include <string>

namespace opgen {

// Emits per-obligation SMT-LIB2 (QF_BV) verification conditions for every
// opcode with a CLOSED body, plus `<prefix>_vc_manifest.txt` classifying
// EVERY op exactly once: `OB <id> <mnemonic> <kind> <expect> <file>` rows for
// obligations, `UNVERIFIABLE <mnemonic> <reason>` rows for bodies the VC
// layer cannot state, and a `CLOSED <mnemonic>` row for a closed body with no
// obligations — so no op is silently absent from the verify report.
//
// Obligation kinds:
//   totality  — assuming every `error:` guard is false, no partial operator
//               (signed/unsigned division or remainder) is applied outside
//               its C-defined domain. expect unsat.
//   edge      — each `edge: "P" -> "Q"` row: guards false ∧ P ⊨ Q, with P
//               read over the INPUT values and Q over the OUTPUTS. expect
//               unsat.
//   guard-sat — each guard, under the guards declared before it, can fire
//               at all (an unsatisfiable guard is dead spec text). expect
//               sat.
//
// The translation is a MIRROR of SemLowerer's C emission, not a second
// semantics: C integer promotion at every operator, the threaded result
// type for shift masks and `>>>`/int_min widths, truncation at assignment.
// Its fidelity is checked by the seeded-liar fixtures and the reference-
// kernel differential leg.
class VcEmitter {
public:
    VcEmitter(const Module* mod, std::string prefix)
        : mod_(mod), prefix_(std::move(prefix)) {}

    // Returns nonzero on a statically-provable def bug: an output not
    // assigned on every path, an assignment to an undeclared name, or an
    // edge condition that does not parse.
    int emit_all(const char* out_dir);

private:
    const Module* mod_;
    std::string   prefix_;
};

} // namespace opgen

#endif
