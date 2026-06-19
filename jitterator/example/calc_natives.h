/*
 * calc_natives.h — the calc VM's control natives, shared by the interp
 * e2e harness and the JIT harness. These are the C functions the
 * opgen-generated handlers AND the copy-and-patch JIT stencils call by
 * name (calc.def's `native status` decls); both tiers use the same
 * functions, so calls behave identically interpreted or JITed.
 *
 * A call is a control transfer whose continuation is reified: calc_call
 * saves the caller frame (its code cursor IS the return pc) onto the
 * vm's call stack and seeks to the callee entry — the threaded loop just
 * runs on into the callee. calc_ret restores the caller frame and pushes
 * the result, or, at the top level, delivers the program's result.
 *
 * Non-static definitions in a header: each harness is its own
 * executable and includes this exactly once, so there is one definition
 * per link — no ODR clash, and gen_interp.o's external references
 * resolve against it.
 */
#ifndef CALC_NATIVES_H
#define CALC_NATIVES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gen_interp.h"     /* vm_t / frame_t / slot_t / calc_status_t / T_* */
#include "bbq_runtime.h"    /* bbq_seek / bbq_total_size */

/* Reposition the code cursor to an absolute byte PC (branch/goto). */
calc_status_t calc_br(vm_t* vm, heap_t* h, s4 off) {
    (void)h;
    bbq_seek(&vm->frame.code, (size_t)off);
    return CALC_OK;
}

/* Enter a callee: the args are on top of the eval stack. Snapshot them,
 * save the caller frame (return pc = the cursor just past the call),
 * then bind the args as the callee's first locals and seek to entry. */
calc_status_t calc_call(vm_t* vm, heap_t* h, s4 entry, s4 nargs) {
    (void)h;
    frame_t* f = &vm->frame;
    slot_t argv[CALC_MAX_LOCALS];
    u1     argt[CALC_MAX_LOCALS];
    for (s4 i = 0; i < nargs; i++) {
        argv[i] = f->stack[f->sp - (u4)nargs + (u4)i];
        argt[i] = f->stack_types[f->sp - (u4)nargs + (u4)i];
    }
    f->sp -= (u4)nargs;                      /* consume args; result returns here */
    vm->callstack[vm->depth++] = *f;         /* the reified continuation */
    f->sp = 0;                               /* fresh callee operand stack */
    for (s4 i = 0; i < nargs; i++) { f->locals[i] = argv[i]; f->local_types[i] = argt[i]; }
    bbq_seek(&f->code, (size_t)entry);
    return CALC_OK;
}

/* Return v to the caller (restore frame, push result), or deliver the
 * program result + halt at the top level. */
calc_status_t calc_ret(vm_t* vm, heap_t* h, s4 v) {
    (void)h;
    if (vm->depth == 0) {
        vm->result.i = v; vm->result_type = T_INT;
        bbq_seek(&vm->frame.code, bbq_total_size(&vm->frame.code));
        return CALC_HALT;
    }
    vm->frame = vm->callstack[--vm->depth];   /* restore caller pc/sp/locals/stack */
    vm->frame.stack[vm->frame.sp].i = v;      /* push the result */
    vm->frame.stack_types[vm->frame.sp] = T_INT;
    vm->frame.sp++;
    return CALC_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* CALC_NATIVES_H */
