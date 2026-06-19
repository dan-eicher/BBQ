/*
 * calc_backend.h — the calc VM substrate, supplied to opgen's generated
 * runtime_api.h via -DOPGEN_BACKEND_TYPES. The value model (slot_t,
 * calc_status_t, the T_* tags) is GENERATED above this header's include point
 * (opgen's emit_value_model), so this is a fragment: only ever included by
 * runtime_api.h, after the value model exists.
 *
 * Same shape as javelina's wasm_frame.h, minimal: the in-place code cursor is a
 * bbq_ctx_t (pos IS the pc), so opcode dispatch AND operand decode both go
 * through bbq_runtime's readers — the JIT-capable model. The frame is at vm
 * offset 0 (the copy-and-patch JIT ABI).
 */
#ifndef CALC_BACKEND_H
#define CALC_BACKEND_H

#include <stddef.h>
#include "bbq_runtime.h"   /* bbq_ctx_t + bbq_read_* — the shared decoders */

#define CALC_MAX_STACK  256
#define CALC_MAX_LOCALS 256
#define CALC_MAX_DEPTH  256               /* call nesting — a deep-recursion ceiling */

typedef struct {
    bbq_ctx_t code;                       /* in-place code cursor; code.pos IS the pc */
    slot_t    stack[CALC_MAX_STACK];      /* the operand stack */
    u1        stack_types[CALC_MAX_STACK];
    u4        sp;
    slot_t    locals[CALC_MAX_LOCALS];
    u1        local_types[CALC_MAX_LOCALS];
    u4        num_locals;
} frame_t;

typedef struct heap_t heap_t;             /* the calc has no heap natives; the handle is unused */

typedef struct {
    frame_t frame;                        /* MUST be first (copy-and-patch JIT ABI) */
    heap_t* heap;
    slot_t  result;                       /* the program's result (set by calc_ret) */
    u1      result_type;
    u1      trapped;
    /* The reified continuations: each calc_call saves the caller frame
     * (return pc, sp, locals, stack) here; calc_ret pops back. The frame
     * cursor IS the return pc, so a return is just frame restore + seek. */
    frame_t callstack[CALC_MAX_DEPTH];
    u4      depth;
} vm_t;

_Static_assert(offsetof(vm_t, frame) == 0, "frame must be at offset 0 (JIT ABI)");

#endif /* CALC_BACKEND_H */
