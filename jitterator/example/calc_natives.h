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
#include <string.h>         /* memcpy — unaligned linear-memory access */

/* Reposition the code cursor to an absolute byte PC (branch/goto). */
calc_status_t calc_br(vm_t* vm, heap_t* h, s4 off) {
    (void)h;
    bbq_seek(&vm->frame.code, (size_t)off);
    return CALC_OK;
}

/* Enter a callee: the args were on top of the eval stack. calc.def declares them as a
 * §3(b) variadic operand group with `flag: pops_first`, so opgen has ALREADY dropped sp
 * past them by the time this runs — they sit just above sp, where the callee frame gets
 * carved. That is the whole point of pops_first: the marshaling is opgen's, and this
 * native no longer re-implements the count. */
calc_status_t calc_call(vm_t* vm, heap_t* h, s4 entry, s4 nargs) {
    (void)h;
    frame_t* f = &vm->frame;
    slot_t argv[CALC_MAX_LOCALS];
    u1     argt[CALC_MAX_LOCALS];
    for (s4 i = 0; i < nargs; i++) {
        argv[i] = f->stack[f->sp + (u4)i];
        argt[i] = f->stack_types[f->sp + (u4)i];
    }
    vm->callstack[vm->depth++] = *f;         /* the reified continuation */
    f->sp = 0;                               /* fresh callee operand stack */
    for (s4 i = 0; i < nargs; i++) { f->local_slots[i] = argv[i]; f->local_slot_types[i] = argt[i]; }
    bbq_seek(&f->code, (size_t)entry);
    return CALC_OK;
}

/* Abort the program: flag the trap and seek to the end so the threaded loop
 * stops (the same halt mechanism as calc_ret's top level). Returns the CALC_TRAP
 * extra status — the calc_status_t value opgen widened the enum to include. */
calc_status_t calc_abort(vm_t* vm, heap_t* h, s4 code) {
    (void)h; (void)code;
    vm->trapped = 1;
    bbq_seek(&vm->frame.code, bbq_total_size(&vm->frame.code));
    return CALC_TRAP;
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

/* ── Structured control ───────────────────────────────────────────────────────
 *
 * The WALK is opgen's (opgen_do_transfer, generated into runtime_api.h from the
 * `sidetable` directive) — this only says "do it", and returns a status so both tiers
 * route through their control path: the interpreter re-dispatches at the new pc, and
 * the JIT stencil bakes _HOLE_ip, calls, then resyncs. A `void` transfer would move the
 * cursor and then fall straight into the NEXT stencil, which is the wrong one. */
calc_status_t transfer(vm_t* vm, heap_t* h) {
    (void)h;
    opgen_do_transfer(&vm->frame);
    return CALC_OK;
}

/* ── A native reachable only from a guard (dcheck) ───────────────────────────── */
s4 calc_is_bad(vm_t* vm, heap_t* h, f8 x) { (void)vm; (void)h; return x == 13.0; }

/* ── The runtime-typed carrier ────────────────────────────────────────────────
 *
 * calc_vpush builds a v128 slot from two 64-bit halves; box/unbox move it through
 * any_t, which is where the high half used to be dropped; calc_vlane reads a lane back
 * out of the carrier. Together they are the round trip a v128 struct field takes. */
void calc_vpush(vm_t* vm, heap_t* h, s8 lo, s8 hi) {
    (void)h;
    frame_t* f = &vm->frame;
    f->stack[f->sp].v.i64[0] = lo;
    f->stack[f->sp].v.i64[1] = hi;
    JV_NPUSH(f, f->stack[f->sp], T_V128);
}
void calc_box(vm_t* vm, heap_t* h, any_t v) {
    (void)h;
    vm->cell.v.i64[0] = v.bits;
    vm->cell.v.i64[1] = v.hi;
    vm->cell_type = v.kind;
}
any_t calc_unbox(vm_t* vm, heap_t* h) {
    (void)h;
    any_t a;
    a.bits = vm->cell.v.i64[0];
    a.hi   = vm->cell.v.i64[1];
    a.kind = vm->cell_type;
    return a;
}
s8 calc_vlane(vm_t* vm, heap_t* h, any_t v, s4 i) {
    (void)vm; (void)h;
    return i == 0 ? v.bits : v.hi;
}
s4 calc_tag(vm_t* vm, heap_t* h, any_t v) { (void)vm; (void)h; return v.kind; }

/* Push a 64-bit value under a CHOSEN tag: the shape a value reconstructed out of an
 * aggregate has, where the tag describes the container rather than the value's declared
 * width. Written through the i64 slot member, so the full 64 bits are present whatever
 * the tag claims — which is exactly the disagreement `addr(...)` exists to ignore. */
void calc_push_tagged(vm_t* vm, heap_t* h, s8 v, s4 tag) {
    (void)h;
    frame_t* f = &vm->frame;
    f->stack[f->sp].l = v;
    f->stack_types[f->sp] = (u1)tag;
    f->sp++;
}

/* ── Linear memory ────────────────────────────────────────────────────────────
 *
 * Memory 0 has a 32-bit declared addrtype, memory 1 a 64-bit one. Nothing here inspects
 * a stack tag: the pop width was already decided by `addr(calc_mem_is64(m))`, from the
 * module, before these ever run. */
s4 calc_mem_is64(vm_t* vm, heap_t* h, s4 m)   { (void)vm; (void)h; return m == 1; }
s4 calc_mem_pages(vm_t* vm, heap_t* h, s4 m)  { (void)vm; (void)h; (void)m; return CALC_MEM_BYTES; }

s4 calc_mem_in_bounds(vm_t* vm, heap_t* h, s4 m, u8 a, s4 n) {
    (void)vm; (void)h;
    if (m < 0 || m >= CALC_NUM_MEM || n < 0) return 0;
    return a <= (u8)CALC_MEM_BYTES && (u8)n <= (u8)CALC_MEM_BYTES - a;
}
s4 calc_load32(vm_t* vm, heap_t* h, s4 m, u8 a) {
    (void)h;
    s4 v = 0;
    memcpy(&v, &vm->mem[m][a], 4);
    return v;
}
void calc_store32(vm_t* vm, heap_t* h, s4 m, u8 a, s4 v) {
    (void)h;
    memcpy(&vm->mem[m][a], &v, 4);
}

/* ── §3(b) variadic support ───────────────────────────────────────────────────
 *
 * calc_accum takes an any_t because a variadic operand is read IN PLACE, as a
 * runtime-typed value — the same carrier as box/unbox. calc_produce writes above sp
 * without moving it; opgen raises sp afterwards, which is the whole marshaling. */
void calc_acc_reset(vm_t* vm, heap_t* h)       { (void)h; vm->acc = 0; }
void calc_accum(vm_t* vm, heap_t* h, any_t v)  { (void)h; vm->acc += (s4)v.bits; }
s4   calc_acc(vm_t* vm, heap_t* h)             { (void)h; return vm->acc; }

void calc_produce(vm_t* vm, heap_t* h, s4 n) {
    (void)h;
    frame_t* f = &vm->frame;
    for (s4 i = 0; i < n; i++) {
        f->stack[f->sp + (u4)i].i = 100 + i;
        f->stack_types[f->sp + (u4)i] = T_INT;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* CALC_NATIVES_H */
