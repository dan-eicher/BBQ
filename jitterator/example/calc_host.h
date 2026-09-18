/*
 * calc_host.h — the seam an opgen-generated VM leaves for its host to fill.
 *
 * opgen emits the opcode bodies and a dispatch table; it does NOT emit the
 * dispatcher, the trap sink, or the code that puts a VM back to a known state.
 * Those are the embedder's, because only the embedder knows what a frame is made
 * of. gen_interp.c declares `calc_next` and `calc_trap` and tail-calls them, so
 * they need external linkage: include this header in exactly ONE translation unit
 * per binary.
 *
 * It lives here rather than in a test because it is not test scaffolding — it is
 * the part of the example that shows what embedding costs. It used to be copied
 * into each test, and the copies had drifted: one reset five fields, the other
 * eleven, so the shorter one carried memory, the accumulator and the side table
 * from each case into the next.
 */
#ifndef CALC_HOST_H
#define CALC_HOST_H

#include "gen_interp.h"      /* dispatch-table accessor + runtime_api.h (vm_t, slot_t) */
#include "bbq_runtime.h"     /* bbq_ctx_t + bbq_read_* */
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CALC_TAIL __attribute__((musttail))

/* The table the dispatcher indexes, and the side table a br_table walks. opgen
 * emits the entry TYPE and the walk over it; filling the entries is a validator's
 * job, which the calc does not have — the tests that need one build it by hand. */
static const opcode_handler_t* calc_g_table;
static const opgen_st_entry_t* calc_g_sidetable;

/* The dispatcher. Read one opcode, tail-call its body; running off the end of the
 * code is how a program stops. */
void calc_next(vm_t* vm) {
    u1 op;
    if (!bbq_read_u8(&vm->frame.code, &op)) return;
    CALC_TAIL return calc_g_table[op](vm);
}

void calc_trap(vm_t* vm) { vm->trapped = 1; }

/* Every field a program can observe, back to its starting value. A VM reused
 * across runs without this carries the previous program's state into the next,
 * which is a test that passes for the wrong reason. */
static inline void calc_reset(vm_t* vm, const u1* code, size_t len) {
    bbq_ctx_init(&vm->frame.code, code, len);
    vm->frame.sp = 0; vm->depth = 0; vm->trapped = 0;
    vm->result.i = 0; vm->result_type = T_INT;
    vm->frame.sidetable = calc_g_sidetable; vm->frame.stp = 0;
    memset(vm->mem, 0, sizeof vm->mem);
    vm->cell.l = 0; vm->cell_type = T_VOID; vm->acc = 0;
}

/* Tier 0: the threaded interpreter. */
static inline s4 calc_interp_run(vm_t* vm, const u1* code, size_t len) {
    calc_g_table = gen_interp_dispatch_table();
    calc_reset(vm, code, len);
    calc_next(vm);
    return vm->result.i;
}

#ifdef __cplusplus
}
#endif

#endif /* CALC_HOST_H */
