/*
 * calc_vm_test.c — runs calc bytecode through BOTH tiers of the opgen-generated
 * VM and asserts they agree. opgen turned calc.def into gen_interp.c (the
 * threaded interpreter) AND calc_stencils.c (the copy-and-patch JIT stencils +
 * the generated machinery stencils). This drives:
 *   tier 0 — interp_run    (javelina's interp.c, smaller)
 *   tier 1 — jit_run        (javelina's jit_driver.c, stripped to the calc)
 * interp == JIT by construction: both run the same generated opcode bodies.
 */
#include "opcodes.h"               /* OP_* */
#include "calc_vm_host.h"          /* both tiers, linked from calc_vm_host.c */
#include "calc_natives.h"          /* calc_br / calc_call / calc_ret (shared, both tiers) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Short names for the two tiers, since every case below names them once each. */
#define interp_run   calc_run_interp
#define jit_run      calc_run_jit

/* ── A tiny program builder. ──
 *
 * The original cases hand-encode their bytes, which stays readable for one-byte
 * operands. The ops below carry sleb128 and IEEE doubles, where a byte literal says
 * nothing about what it encodes — so those cases build their programs instead. */
typedef struct { u1 b[256]; size_t n; } prog_t;

static void p_op(prog_t* p, u1 op)     { p->b[p->n++] = op; }
static void p_u8(prog_t* p, u1 v)      { p->b[p->n++] = v; }
static void p_f64(prog_t* p, double v) { memcpy(&p->b[p->n], &v, 8); p->n += 8; }
static void p_sleb(prog_t* p, int64_t v) {
    for (;;) {
        u1 byte = (u1)(v & 0x7f);
        v >>= 7;
        int done = (v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40));
        p->b[p->n++] = done ? byte : (u1)(byte | 0x80);
        if (done) return;
    }
}
/* `const N` / `lconst N` — the two sleb-carrying constant pushes. */
static void p_iconst(prog_t* p, int32_t v) { p_op(p, 0x01); p_sleb(p, v); }
static void p_lconst(prog_t* p, int64_t v) { p_op(p, 0x17); p_sleb(p, v); }
static void p_dconst(prog_t* p, double v)  { p_op(p, 0x18); p_f64(p, v); }

/* ── The test: each program runs on BOTH tiers; they must agree. ── */
static int fails = 0;
static void check(const char* msg, const u1* code, size_t len, s4 want) {
    static vm_t vi, vj;
    s4 i = interp_run(&vi, code, len);
    s4 j = jit_run(&vj, code, len);
    if (i != want || j != want || i != j) {
        printf("FAIL: %-30s interp=%d jit=%d want=%d\n", msg, i, j, want); fails++;
    } else printf("ok:   %-30s interp==jit==%d\n", msg, i);
}

/* The trap path (the CALC_TRAP status-extra native): assert BOTH tiers flag
 * vm->trapped and stop before the trailing ret, so no result is delivered. */
static void check_trap(const char* msg, const u1* code, size_t len) {
    static vm_t vi, vj;
    interp_run(&vi, code, len);
    jit_run(&vj, code, len);
    if (!vi.trapped || !vj.trapped) {
        printf("FAIL: %-30s interp_trapped=%d jit_trapped=%d want both 1\n",
               msg, vi.trapped, vj.trapped); fails++;
    } else printf("ok:   %-30s interp==jit trapped\n", msg);
}

int main(void) {
    /* ── The `ref` row, on BOTH tiers ───────────────────────────────────────
     * A reference is the one carrier with no arithmetic width in its name —
     * opgen reads it off the slot view — so these are what keep that answer
     * honest, and check() runs each through the interpreter AND the stencils.
     *
     * The last one is the assertion that would have caught javelina. `reftag`
     * shifts by the literal 1, and 1 & 31 == 1 & 63, so a wrong mask is
     * invisible to any execution of it. `refshl` takes the count as an
     * immediate: 1<<33 against 1<<1 is 2^33 vs 2 masked with 63, and 2 vs 2
     * masked with 31. Truncated to i32 for `ret`, 2^33 delivers 0. */
    { static const u1 c[] = {OP_REFNULL, OP_REFNULL, OP_REFEQ, OP_RET};
      check("refnull == refnull", c, sizeof c, 1); }
    { static const u1 c[] = {OP_LCONST,0x05, OP_REFTAG, OP_LCONST,0x05, OP_REFTAG,
                             OP_REFEQ, OP_RET};
      check("reftag 5 == reftag 5", c, sizeof c, 1); }
    { static const u1 c[] = {OP_LCONST,0x05, OP_REFTAG, OP_LCONST,0x06, OP_REFTAG,
                             OP_REFEQ, OP_RET};
      check("reftag 5 != reftag 6", c, sizeof c, 0); }
    { static const u1 c[] = {OP_LCONST,0x05, OP_REFTAG, OP_REFNULL, OP_REFEQ, OP_RET};
      check("reftag 5 != refnull", c, sizeof c, 0); }
    { static const u1 c[] = {OP_LCONST,0x01, OP_REFSHL,33, OP_LCONST,0x01, OP_REFSHL,1,
                             OP_REFEQ, OP_RET};
      check("refshl mask is 63 not 31", c, sizeof c, 0); }

    /* The `any` carrier through push()/pop(). A slot round-trips the intrinsics
     * and must come back with BOTH halves intact: the value (l2i reads it) and
     * the runtime tag (tagof reads it, and subtracting a plain i64's tag gives
     * 0 only if the carrier preserved it). */
    { static const u1 c[] = {OP_LCONST,0x05, OP_ANYROLL, OP_L2I, OP_RET};
      check("push(pop()) keeps the value", c, sizeof c, 5); }
    { static const u1 c[] = {OP_LCONST,0x05, OP_ANYROLL, OP_TAGOF,
                             OP_LCONST,0x07, OP_TAGOF, OP_SUB, OP_RET};
      check("push(pop()) keeps the tag", c, sizeof c, 0); }

    { static const u1 c[] = {0x01,0x03, 0x01,0x04, 0x02, 0x01,0x05, 0x04, 0x10};
      check("(3+4)*5", c, sizeof c, 35); }
    { static const u1 c[] = {0x01,0x08, 0x05, 0x10};
      check("neg 8", c, sizeof c, -8); }
    { static const u1 c[] = {0x01,0x07, 0x0D,0x00, 0x01,0x03, 0x0C,0x00, 0x02, 0x10};
      check("store/load local + add", c, sizeof c, 10); }
    { static const u1 c[] = {0x0F,0x04, 0x01,0x63, 0x01,0x2A, 0x10};
      check("goto skips dead code", c, sizeof c, 42); }
    { static const u1 c[] = {0x01,0x01, 0x0E,0x06, 0x01,0x63, 0x01,0x07, 0x10};
      check("br_if taken", c, sizeof c, 7); }
    { static const u1 c[] = {0x01,0x00, 0x0E,0x06, 0x01,0x07, 0x10};
      check("br_if not taken", c, sizeof c, 7); }
    { static const u1 c[] = {0x01,0x02, 0x01,0x03, 0x08, 0x10};
      check("2 < 3", c, sizeof c, 1); }

    /* Declared `error:` guards on `div` (0x16). Both guard conditions are emitted
     * verbatim from calc.def into BOTH tiers, so these assert the interpreter and
     * the JIT stencil agree on exactly when a guard fires — the parity that a
     * body-shape-inferred guard could never promise.
     *
     * The not-taken cases matter as much as the taken ones: a guard that fired
     * unconditionally would still "trap correctly" on the div-by-zero input. */
    { static const u1 c[] = {0x01,0x14, 0x01,0x04, 0x16, 0x10};
      check("20 / 4", c, sizeof c, 5); }
    { static const u1 c[] = {0x01,0x7F, 0x01,0x02, 0x16, 0x10};
      check("-1 / 2 (neither guard fires)", c, sizeof c, 0); }
    { static const u1 c[] = {0x01,0x2A, 0x01,0x00, 0x16, 0x10};
      check_trap("div by zero", c, sizeof c); }
    /* INT_MIN / -1: the int_min() guard. sleb128 for -2147483648 is 5 bytes. */
    { static const u1 c[] = {0x01,0x80,0x80,0x80,0x80,0x78, 0x01,0x7F, 0x16, 0x10};
      check_trap("INT_MIN / -1 overflow", c, sizeof c); }
    /* INT_MIN / 2 — same numerator, guard must NOT fire. */
    { static const u1 c[] = {0x01,0x80,0x80,0x80,0x80,0x78, 0x01,0x02, 0x16, 0x10};
      check("INT_MIN / 2 (overflow guard not taken)", c, sizeof c, -1073741824); }

    /* Inline native (calc_abs): a DIRECT, folded call in both tiers — no _HOLE_
     * patch, no jit-symbol. const -8 (sleb 0x78); abs; ret. */
    { static const u1 c[] = {0x01,0x78, 0x14, 0x10};
      check("abs(-8)", c, sizeof c, 8); }
    { static const u1 c[] = {0x01,0x08, 0x14, 0x10};
      check("abs(8)", c, sizeof c, 8); }

    /* Status-extra native (calc_abort → CALC_TRAP). trap fires only when c != 0;
     * not-taken falls through to `const 42; ret`. const c; trap; const 42; ret. */
    { static const u1 c[] = {0x01,0x00, 0x15, 0x01,0x2A, 0x10};
      check("trap not taken -> 42", c, sizeof c, 42); }
    { static const u1 c[] = {0x01,0x05, 0x15, 0x01,0x2A, 0x10};
      check_trap("trap taken", c, sizeof c); }

    /* Function calls — the call/ret natives are control transfers; both
     * tiers run the SAME calc_call/calc_ret, and OP_CALL's operands (sleb
     * entry + u8 nargs) are read from the in-place cursor at runtime, so
     * the JIT needs no operand patching. These bytecodes come straight
     * from the runner (lower()) for the named source programs. */
    { /* fn dbl($x) ($x + $x); dbl(21) */
      static const u1 c[] = {0x01,0x15,0x0D,0x00,0x0C,0x00,0x13,0x8E,0x80,0x80,0x80,0x00,0x01,0x10,0x0C,0x00,0x0D,0x01,0x0C,0x00,0x0D,0x02,0x0C,0x01,0x0C,0x02,0x02,0x10};
      check("call dbl(21)", c, sizeof c, 42); }
    { /* fn fac($n) (if ($n < 2) 1 else $n * fac($n - 1)); fac(5) */
      static const u1 c[] = {0x01,0x05,0x0D,0x00,0x0C,0x00,0x13,0x8E,0x80,0x80,0x80,0x00,0x01,0x10,0x0C,0x00,0x0D,0x06,0x01,0x02,0x0D,0x07,0x0C,0x06,0x0C,0x07,0x08,0x0E,0xC5,0x80,0x80,0x80,0x00,0x0C,0x00,0x0D,0x01,0x0C,0x00,0x0D,0x04,0x01,0x01,0x0D,0x05,0x0C,0x04,0x0C,0x05,0x03,0x0D,0x03,0x0C,0x03,0x13,0x8E,0x80,0x80,0x80,0x00,0x01,0x0D,0x02,0x0C,0x01,0x0C,0x02,0x04,0x10,0x01,0x01,0x10};
      check("recursion fac(5)", c, sizeof c, 120); }

    /* ═══ Float constants INSIDE a guard ═══════════════════════════════════════
     *
     * A float literal in a condition lowers to a C literal, which clang materializes
     * out of .rodata — a relocation a copy-and-patch stencil cannot carry. opgen gives
     * each one a `_HOLE_k<bits>` named by its VALUE instead. Get it wrong and the JIT
     * bakes 0: the guard stops firing IN JIT CODE ONLY, which is precisely why every
     * case here runs on both tiers. */
    { prog_t p = {{0}, 0};
      p_dconst(&p, 2147483647.0); p_op(&p, 0x1A); p_op(&p, 0x10);   /* trunc; ret */
      check("trunc 2147483647.0 (in range)", p.b, p.n, 2147483647); }
    { prog_t p = {{0}, 0};
      p_dconst(&p, -2147483648.0); p_op(&p, 0x1A); p_op(&p, 0x10);
      check("trunc -2147483648.0 (lower bound)", p.b, p.n, -2147483648); }
    { prog_t p = {{0}, 0};                                          /* just over 2^31 */
      p_dconst(&p, 2147483648.0); p_op(&p, 0x1A); p_op(&p, 0x10);
      check_trap("trunc 2147483648.0 overflows", p.b, p.n); }
    { prog_t p = {{0}, 0};
      p_dconst(&p, -2147483649.0); p_op(&p, 0x1A); p_op(&p, 0x10);
      check_trap("trunc -2147483649.0 overflows", p.b, p.n); }

    /* A guard that NEGATES a float at run time rather than folding a literal: the sign
     * flip rides _HOLE_fsignmask64, whose width comes from the CONDITION (the body scan
     * keys on the result type and would miss it). Undeclared, jitterator waves the hole
     * through on its name prefix and the driver bakes 0 — a negation by zero. */
    { prog_t p = {{0}, 0};
      p_dconst(&p, -2000000.0); p_dconst(&p, 2.0);                  /* -a = 2000000 > 1e6 */
      p_op(&p, 0x1B); p_op(&p, 0x1A); p_op(&p, 0x10);
      check_trap("ddiv guard negates at run time", p.b, p.n); }
    { prog_t p = {{0}, 0};
      p_dconst(&p, -8.0); p_dconst(&p, 2.0);                        /* -a = 8, guard quiet */
      p_op(&p, 0x1B); p_op(&p, 0x1A); p_op(&p, 0x10);
      check("ddiv -8.0 / 2.0 (guard not taken)", p.b, p.n, -4); }
    { prog_t p = {{0}, 0};
      p_dconst(&p, 1.0); p_dconst(&p, 0.0);
      p_op(&p, 0x1B); p_op(&p, 0x1A); p_op(&p, 0x10);
      check_trap("ddiv by zero", p.b, p.n); }

    /* A native reachable ONLY from a guard still rides a _HOLE_ patch point, so its
     * extern must still be declared — otherwise the stencil TU does not compile, and
     * this case never gets as far as running. */
    { prog_t p = {{0}, 0};
      p_dconst(&p, 13.0); p_op(&p, 0x1C); p_op(&p, 0x1A); p_op(&p, 0x10);
      check_trap("guard-only native fires", p.b, p.n); }
    { prog_t p = {{0}, 0};
      p_dconst(&p, 12.0); p_op(&p, 0x1C); p_op(&p, 0x1A); p_op(&p, 0x10);
      check("guard-only native quiet", p.b, p.n, 12); }

    /* ═══ v128 across the runtime-typed carrier ════════════════════════════════
     *
     * box/unbox move a whole slot through any_t. Shaped for a 64-bit payload, the
     * carrier drops lanes 2/3 — so lane 1 comes back as 0 and the low lane still
     * looks right, which is what made this survive so long. */
    { prog_t p = {{0}, 0};
      p_lconst(&p, 0x1111); p_lconst(&p, 0x2222);
      p_op(&p, 0x1D);                       /* vmake: build the v128 from both halves */
      p_op(&p, 0x1E); p_op(&p, 0x1F);       /* box; unbox */
      p_op(&p, 0x20); p_u8(&p, 1);          /* vlane 1 — the half that used to vanish */
      p_op(&p, 0x29); p_op(&p, 0x10);       /* l2i; ret */
      check("v128 high lane survives any_t", p.b, p.n, 0x2222); }
    { prog_t p = {{0}, 0};
      p_lconst(&p, 0x1111); p_lconst(&p, 0x2222);
      p_op(&p, 0x1D); p_op(&p, 0x1E); p_op(&p, 0x1F);
      p_op(&p, 0x20); p_u8(&p, 0);
      p_op(&p, 0x29); p_op(&p, 0x10);
      check("v128 low lane survives any_t", p.b, p.n, 0x1111); }
    /* The tag has to survive too, or the carrier is only half honest. */
    { prog_t p = {{0}, 0};
      p_lconst(&p, 1); p_lconst(&p, 2);
      p_op(&p, 0x1D); p_op(&p, 0x1E); p_op(&p, 0x1F);
      p_op(&p, 0x2A); p_op(&p, 0x10);       /* tagof; ret */
      check("any_t carries the v128 tag", p.b, p.n, T_V128); }

    /* ═══ `word`: one declared type, two lowerings ═════════════════════════════
     *
     * select's arms are slots, so its assignment is a slot move. msize's value is a
     * SCALAR whose width is decided at run time, so it goes through the value model's
     * field selector and the body sets the tag itself — which is what makes a
     * runtime-width result an honest signature rather than a lie. */
    { static const u1 c[] = {0x01,0x0B, 0x01,0x16, 0x01,0x01, 0x21, 0x10};
      check("select picks v1 when c != 0", c, sizeof c, 11); }
    { static const u1 c[] = {0x01,0x0B, 0x01,0x16, 0x01,0x00, 0x21, 0x10};
      check("select picks v2 when c == 0", c, sizeof c, 22); }
    { static const u1 c[] = {0x22,0x00, 0x10};                       /* msize 0; ret */
      check("msize value (i32 addrtype memory)", c, sizeof c, CALC_MEM_BYTES); }
    /* The TAG is the half a plain value check would miss: memory 1's addrtype is
     * 64-bit, so the same op must push its result tagged as a long. */
    { static const u1 c[] = {0x22,0x00, 0x1E, 0x1F, 0x2A, 0x10};     /* msize 0; box; unbox; tagof */
      check("msize tag follows the i32 addrtype", c, sizeof c, T_INT); }
    { static const u1 c[] = {0x22,0x01, 0x1E, 0x1F, 0x2A, 0x10};
      check("msize tag follows the i64 addrtype", c, sizeof c, T_LONG); }

    /* ═══ addrtype: the width is DECLARED, not sniffed ═════════════════════════ */

    /* Memory 0 has a 32-bit addrtype and an ordinary i32 address — the easy direction. */
    { static const u1 c[] = {0x01,0x04, 0x01,0x2A, 0x24,0x00,      /* const 4; const 42; store32 0 */
                             0x01,0x04, 0x23,0x00, 0x10};          /* const 4; load32 0; ret */
      check("store/load through a 32-bit addrtype", c, sizeof c, 42); }

    /* THE case. The address is 64-bit and out of bounds, but it carries an i32 tag —
     * the shape a value reconstructed out of an aggregate has, where the tag describes
     * the container rather than the value's declared width. Reading the width from the
     * DECLARED addrtype sees 0x1_0000_0002 and traps. Reading it from the tag truncates
     * to 2, lands in bounds, and silently returns a different location's contents. */
    { prog_t p = {{0}, 0};
      p_lconst(&p, 0x100000002LL);
      p_op(&p, 0x25); p_u8(&p, T_INT);      /* pushtag T_INT — the tag now lies */
      p_op(&p, 0x23); p_u8(&p, 1);          /* load32 memory 1 (64-bit addrtype) */
      p_op(&p, 0x10);
      check_trap("64-bit address under an i32 tag still traps", p.b, p.n); }
    /* ...and the guard is not simply always firing. */
    { prog_t p = {{0}, 0};
      p_lconst(&p, 8);
      p_op(&p, 0x25); p_u8(&p, T_INT);
      p_op(&p, 0x23); p_u8(&p, 1);
      p_op(&p, 0x10);
      check("in-bounds 64-bit address does not trap", p.b, p.n, 0); }

    /* ═══ §3(b) variadic ═══════════════════════════════════════════════════════
     *
     * sum_n takes the DEFAULT drop-after-body path: its operands stay on the stack
     * across the body and are read in place as `args[i]` — a runtime-typed read
     * through the same carrier as box/unbox. opgen drops sp afterwards. (The `call`
     * cases below take the other path; see pops_first.) */
    { static const u1 c[] = {0x01,0x01, 0x01,0x02, 0x01,0x03, 0x26,0x03, 0x10};
      check("sum_n over 3 in-place operands", c, sizeof c, 6); }
    /* Count 0: consumes nothing, still pushes its one result. `add` then sees the 9
     * that was already there — so a variadic pop of 0 must not disturb the stack. */
    { static const u1 c[] = {0x01,0x09, 0x26,0x00, 0x02, 0x10};
      check("sum_n over 0 operands consumes nothing", c, sizeof c, 9); }

    /* A variadic RESULT names a count, not a value: the values are already at the frame
     * top and exposing them is a single sp raise. Popping twice and returning the third
     * is what proves sp actually rose by 3. */
    { static const u1 c[] = {0x27,0x03, 0x11, 0x11, 0x10};   /* expose 3; pop; pop; ret */
      check("variadic result raises sp by its count", c, sizeof c, 100); }

    /* ═══ br_table + the side table ════════════════════════════════════════════
     *
     * The label TARGETS live in the side table, so the vector in the bytecode is pure
     * decode: opgen reads its length as `labels_count`, skips the labels, and the body
     * clamps the key against the count. delta_ip is relative to the byte just past the
     * whole instruction, which both tiers agree on — the JIT's compile-walk skips the
     * label vector before baking _HOLE_ip. */
    {
        /*  0: const KEY        2: br_table cnt=2 [0,1,2]
         *  7: const 10; ret   10: const 20; ret   13: const 30; ret  */
        static const opgen_st_entry_t tbl[] = {
            { 0, 0, 0, 0 },   /* case 0    -> 7  */
            { 3, 0, 0, 0 },   /* case 1    -> 10 */
            { 6, 0, 0, 0 },   /* default   -> 13 */
        };
        calc_set_sidetable(tbl);
        u1 c[] = {0x01,0x00, 0x28,0x02,0x00,0x01,0x02,
                  0x01,0x0A,0x10, 0x01,0x14,0x10, 0x01,0x1E,0x10};
        c[1] = 0; check("br_table key 0", c, sizeof c, 10);
        c[1] = 1; check("br_table key 1", c, sizeof c, 20);
        c[1] = 7; check("br_table key clamps to default", c, sizeof c, 30);
        calc_set_sidetable(NULL);
    }

    /* The entry's vals/pop half: a taken branch keeps the top `vals` operands and drops
     * `pop` beneath them. Here 33 is kept and 22 dropped, so the trailing `add` sees
     * 11 + 33; without the drop it would see 22 + 33. */
    {
        static const opgen_st_entry_t tbl[] = { { 0, 0, 1, 1 } };
        calc_set_sidetable(tbl);
        /*  0: const 11  2: const 22  4: const 33  6: const 0(key)
         *  8: br_table cnt=0 [0]     11: add      12: ret */
        static const u1 c[] = {0x01,0x0B, 0x01,0x16, 0x01,0x21, 0x01,0x00,
                               0x28,0x00,0x00, 0x02, 0x10};
        check("br_table entry keeps vals, drops pop", c, sizeof c, 44);
        calc_set_sidetable(NULL);
    }

    /* ═══ Running off the end of a function ════════════════════════════════════
     *
     * No trailing `ret`. The halt stencil stashes nothing — the results ARE the top of
     * the frame stack, where a caller reads them. It used to write vm->result, which
     * named a field of ONE consumer's vm_t: a baked-in target identifier in a generator
     * that promises to have none. */
    {
        static const u1 c[] = {0x01,0x03, 0x01,0x04, 0x02};   /* const 3; const 4; add */
        static vm_t vi, vj;
        interp_run(&vi, c, sizeof c);
        jit_run(&vj, c, sizeof c);
        int ok = vi.frame.sp == 1 && vj.frame.sp == 1 &&
                 vi.frame.stack[0].i == 7 && vj.frame.stack[0].i == 7;
        if (!ok) {
            printf("FAIL: %-30s interp sp=%u top=%d  jit sp=%u top=%d  want sp=1 top=7\n",
                   "run off the end", vi.frame.sp, vi.frame.stack[0].i,
                   vj.frame.sp, vj.frame.stack[0].i);
            fails++;
        } else printf("ok:   %-30s interp==jit results on the frame stack\n", "run off the end");
    }

    if (!fails) { printf("\ncalc VM (interp == JIT): all checks passed\n"); return 0; }
    printf("\ncalc VM: %d FAILED\n", fails);
    return 1;
}
