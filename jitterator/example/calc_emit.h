/*
 * calc_emit.h — the calc.def opcode set. The bytecode emitter itself (op
 * buffer, label anchors, branch backpatch, the cg_jump redundant-goto
 * peephole) is the generic `bbq::Emit` in opgen/runtime/bbq_emit.h; this
 * header only names the calc's opcodes so the burg actions and cg_jump
 * can reference them. Opcode bytes must match grammar/calc.def.
 */
#ifndef CALC_EMIT_H
#define CALC_EMIT_H

#include <cstdint>

namespace calc {

enum CalcOp : uint8_t {
    OP_CONST = 0x01, OP_ADD = 0x02, OP_SUB = 0x03, OP_MUL = 0x04, OP_NEG = 0x05,
    OP_EQ = 0x06, OP_NE = 0x07, OP_LT = 0x08, OP_LE = 0x09, OP_GT = 0x0A, OP_GE = 0x0B,
    OP_LOAD = 0x0C, OP_STORE = 0x0D, OP_BR_IF = 0x0E, OP_GOTO = 0x0F, OP_RET = 0x10, OP_POP = 0x11,
    OP_BR_IFZ = 0x12, OP_CALL = 0x13,
};

} // namespace calc

#endif
