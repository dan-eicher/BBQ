/*
 * calc_interp.c — functional end-to-end test of opgen's generated interpreter.
 *
 * opgen turns test/data/calc.def into gen_interp.c (the threaded-dispatch VM);
 * this driver compiles against it, builds real bytecode programs, runs them
 * through the generated handlers, and checks the operand stack holds the
 * specified result. It exercises the whole pipeline: the sem-body lowering,
 * the spec-derived slot model, operand decode (FETCH), threaded dispatch, and
 * locals — by EXECUTION, not by inspecting the generated text.
 */
#include "gen_interp.h"   /* the dispatch-table accessor (pulls in runtime_api.h) */
#include "opcodes.h"      /* OP_* constants */
#include <stdio.h>
#include <string.h>

/* The threaded-dispatch driver: each handler does its work and musttail-jumps
 * back here for the next opcode; we stop when the cursor runs off the program. */
static u2 g_len;
void calc_trap(vm_t* vm) { (void)vm; }   /* the calc opcodes never trap */
void calc_next(vm_t* vm) {
    frame_t* f = &vm->frame;
    if (f->pc >= g_len) return;
    const opcode_handler_t* table = gen_interp_dispatch_table();
    opcode_handler_t h = table[f->code[f->pc++]];
    if (h) h(vm);
}

/* A growable bytecode program. */
typedef struct { u1 b[128]; u2 n; } prog_t;
static void op(prog_t* p, u1 o)  { p->b[p->n++] = o; }
static void u8op(prog_t* p, u1 x) { p->b[p->n++] = x; }
static void iconst(prog_t* p, s4 v) {        /* iconst <i32 big-endian> */
    op(p, OP_ICONST);
    p->b[p->n++] = (u1)(v >> 24); p->b[p->n++] = (u1)(v >> 16);
    p->b[p->n++] = (u1)(v >> 8);  p->b[p->n++] = (u1)v;
}

static vm_t g_vm;
/* Run a program from a clean VM; return the bottom stack slot's int value. */
static s4 run(const prog_t* p) {
    memset(&g_vm, 0, sizeof g_vm);
    g_vm.frame.code = p->b; g_vm.frame.pc = 0; g_vm.frame.sp = 0;
    g_len = p->n;
    calc_next(&g_vm);
    return g_vm.frame.stack[0].i;
}
static u1 final_sp(void) { return g_vm.frame.sp; }

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
                              else { printf("ok:   %s\n", msg); } } while (0)

/* result of a binary op applied to (a, b) */
static s4 bin(u1 opcode, s4 a, s4 b) {
    prog_t p = {0}; iconst(&p, a); iconst(&p, b); op(&p, opcode); return run(&p);
}

int main(void) {
    /* arithmetic */
    CHECK(bin(OP_IADD, 5, 7) == 12,            "iadd 5+7 = 12");
    CHECK(bin(OP_IADD, 2147483647, 1) == (s4)0x80000000, "iadd overflow wraps (fwrapv)");
    CHECK(bin(OP_ISUB, 10, 3) == 7,            "isub 10-3 = 7");
    CHECK(bin(OP_IMUL, 6, 7) == 42,            "imul 6*7 = 42");
    { prog_t p = {0}; iconst(&p, 5); op(&p, OP_INEG);
      CHECK(run(&p) == -5 && final_sp() == 1,  "ineg 5 = -5"); }

    /* bitwise */
    CHECK(bin(OP_IAND, 0x0F0F, 0x3C3C) == 0x0C0C, "iand 0x0F0F & 0x3C3C = 0x0C0C");
    CHECK(bin(OP_IOR,  0x0F00, 0x00F0) == 0x0FF0, "ior 0x0F00 | 0x00F0 = 0x0FF0");

    /* shifts: count masks to the value's bit width - 1 (= 31 for i32) */
    CHECK(bin(OP_ISHL, 1, 3)  == 8,            "ishl 1<<3 = 8");
    CHECK(bin(OP_ISHL, 1, 33) == 2,            "ishl 1<<33 masks to <<1 = 2");
    CHECK(bin(OP_ISHR, -8, 1) == -4,           "ishr -8>>1 = -4 (arithmetic)");
    CHECK(bin(OP_IUSHR, -1, 1) == (s4)0x7FFFFFFF, "iushr -1>>>1 = 0x7FFFFFFF (logical)");

    /* a small program: (3 + 4) * 5 = 35 */
    { prog_t p = {0}; iconst(&p, 3); iconst(&p, 4); op(&p, OP_IADD);
      iconst(&p, 5); op(&p, OP_IMUL);
      CHECK(run(&p) == 35 && final_sp() == 1,  "(3+4)*5 = 35 via a multi-op program"); }

    /* locals: store then load round-trips through a frame slot */
    { prog_t p = {0}; iconst(&p, 0x1234ABCD); op(&p, OP_ISTORE); u8op(&p, 5);
      op(&p, OP_ILOAD); u8op(&p, 5);
      CHECK(run(&p) == (s4)0x1234ABCD && final_sp() == 1, "istore/iload local 5 round-trips"); }
    /* locals are independent slots */
    { prog_t p = {0};
      iconst(&p, 11); op(&p, OP_ISTORE); u8op(&p, 1);
      iconst(&p, 22); op(&p, OP_ISTORE); u8op(&p, 2);
      op(&p, OP_ILOAD); u8op(&p, 1);     /* push local 1 (11) */
      op(&p, OP_ILOAD); u8op(&p, 2);     /* push local 2 (22) */
      op(&p, OP_IADD);
      CHECK(run(&p) == 33 && final_sp() == 1, "local1(11) + local2(22) = 33"); }

    if (failures == 0) { printf("\nopgen interp e2e: all checks passed\n"); return 0; }
    printf("\nopgen interp e2e: %d FAILED\n", failures);
    return 1;
}
