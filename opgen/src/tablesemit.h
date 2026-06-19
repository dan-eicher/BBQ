#ifndef OPGEN_TABLESEMIT_H
#define OPGEN_TABLESEMIT_H

#include "spec.h"
#include <cstdio>

namespace opgen {

// Emits the two opcode-table outputs: opcodes.h (OP_* constants + flag bits +
// table externs + user headers) and opcode_tables.c (the seven 256-entry
// tables + the user `%%` body). Reads the per-opcode facts from the Spec.
class TablesEmitter {
public:
    explicit TablesEmitter(const Spec& spec) : spec_(spec), mod_(spec.module()) {}

    void emit_opcodes_h(FILE* o);        // opcodes.h
    void emit_opcode_tables_c(FILE* o);  // opcode_tables.c

private:
    const Spec&   spec_;
    const Module* mod_;
};

} // namespace opgen

#endif
