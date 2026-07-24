/*
 * calc_vm_test.c — runs calc bytecode through BOTH tiers of the opgen-generated
 * VM and asserts they agree. opgen turned calc.def into gen_interp.c (the
 * threaded interpreter) AND calc_stencils.c (the copy-and-patch JIT stencils +
 * the generated machinery stencils). This drives:
 *   tier 0 — interp_run    (javelina's interp.c, smaller)
 *   tier 1 — jit_run        (javelina's jit_driver.c, stripped to the calc)
 * interp == JIT by construction: both run the same generated opcode bodies.
 */
#include "gen_interp.h"            /* dispatch-table accessor + runtime_api.h (vm_t, slot_t) */
#include "opcodes.h"               /* OP_* */
#include "bbq_runtime.h"           /* bbq_ctx_t + bbq_read_* */
#include "calc_stencil_table.h"    /* jitterator: stencil_table[], STENCIL_*, StencilDef, PatchEntry */
#include "calc_jit_meta.h"         /* opgen: calc_jit_meta[256], jit_operand_kind_t */
#include "calc_jit_symbols.h"      /* opgen: _HOLE_<native> -> address */
#include "jit_codebuf.h"           /* jitterator's executable code buffer */
#include "calc_natives.h"          /* calc_br / calc_call / calc_ret (shared, both tiers) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define TAIL __attribute__((musttail))

/* ── Tier 0: the threaded interpreter. ── */
static const opcode_handler_t* g_table;
void calc_next(vm_t* vm) {
    u1 op;
    if (!bbq_read_u8(&vm->frame.code, &op)) return;
    TAIL return g_table[op](vm);
}
void calc_trap(vm_t* vm) { vm->trapped = 1; }

/* The side table a br_table program branches through. opgen emitted the entry TYPE and
 * the walk over it; a real consumer's validator fills the entries. The tests below build
 * them by hand, which is the same role played by hand-written bytecode elsewhere here. */
static const opgen_st_entry_t* g_sidetable = NULL;

static void reset_vm(vm_t* vm, const u1* code, size_t len) {
    bbq_ctx_init(&vm->frame.code, code, len);
    vm->frame.sp = 0; vm->depth = 0; vm->trapped = 0;
    vm->result.i = 0; vm->result_type = T_INT;
    vm->frame.sidetable = g_sidetable; vm->frame.stp = 0;
    memset(vm->mem, 0, sizeof vm->mem);
    vm->cell.l = 0; vm->cell_type = T_VOID; vm->acc = 0;
}

static s4 interp_run(vm_t* vm, const u1* code, size_t len) {
    g_table = gen_interp_dispatch_table();
    reset_vm(vm, code, len);
    calc_next(vm);
    return vm->result.i;
}

/* ── Tier 1: the copy-and-patch JIT driver (ported from javelina jit_driver.c). ── */
typedef struct { size_t patch_addr; uint64_t value; size_t foff; } data_hole_t;

static size_t emit_stencil(jit_codebuf_t* buf, const StencilDef* s,
                           const uint64_t* vals, data_hole_t* recs, int* nrec) {
    size_t base = buf->size;
    jcb_emit(buf, s->code, s->code_size);
    for (uint32_t i = 0; i < s->patch_count; i++) {
        const PatchEntry* p = &s->patches[i];
        size_t pa = base + p->offset;
        switch (p->type) {
        case PATCH_REL_BRANCH: {
            uint64_t t = vals ? vals[p->hole_index] : 0;
            if (t) { uintptr_t a = (uintptr_t)buf->base + pa; jcb_patch32(buf, pa, (int32_t)(t - (a + 4))); }
            break;
        }
        case PATCH_REL_DATA:
            recs[*nrec].patch_addr = pa; recs[*nrec].value = vals ? vals[p->hole_index] : 0; (*nrec)++; break;
        case PATCH_ABS64:  jcb_patch64(buf, pa, vals ? vals[p->hole_index] : 0); break;
        case PATCH_ABS32S: jcb_patch32(buf, pa, (int32_t)(vals ? vals[p->hole_index] : 0)); break;
        }
    }
    return base;
}

static void backpatch(jit_codebuf_t* buf, size_t base, const StencilDef* s, int hi, uint64_t t) {
    for (uint32_t i = 0; i < s->patch_count; i++) {
        const PatchEntry* p = &s->patches[i];
        if ((int)p->hole_index != hi || p->type != PATCH_REL_BRANCH) continue;
        size_t pa = base + p->offset;
        uintptr_t a = (uintptr_t)buf->base + pa;
        jcb_patch32(buf, pa, (int32_t)(t - (a + 4)));
    }
}

static int find_hole(const StencilDef* d, const char* n) {
    for (int i = 0; i < d->hole_count; i++) if (!strcmp(d->hole_names[i], n)) return i;
    return -1;
}
static void* jit_sym(const char* n) {
    for (int i = 0; i < calc_jit_symbols_count; i++) if (!strcmp(calc_jit_symbols[i].name, n)) return calc_jit_symbols[i].addr;
    return NULL;
}
static void fill_native_holes(const StencilDef* d, uint64_t* v) {
    for (int i = 0; i < d->hole_count; i++) { void* a = jit_sym(d->hole_names[i]); if (a) v[i] = (uint64_t)(uintptr_t)a; }
}
static uint64_t decode_operand(bbq_ctx_t* c, jit_operand_kind_t k) {
    switch (k) {
    case JOP_SLEB32: { int32_t v = 0; bbq_read_sleb128_i32(c, &v); return (uint64_t)(uint32_t)v; }
    case JOP_SLEB64: { int64_t v = 0; bbq_read_sleb128_i64(c, &v); return (uint64_t)v; }
    case JOP_ULEB32: { uint32_t v = 0; bbq_read_uleb128_u32(c, &v); return v; }
    case JOP_ULEB64: { uint64_t v = 0; bbq_read_uleb128_u64(c, &v); return v; }
    case JOP_U8:     { uint8_t v = 0; bbq_read_u8(c, &v); return v; }
    /* The stencil's hole is the double's BIT PATTERN, so read the bits, not the value. */
    case JOP_F64:    { double v = 0; uint64_t b; bbq_read_f64le(c, &v); memcpy(&b, &v, 8); return b; }
    /* br_table's vec(labelidx) LENGTH. Reading it is what lets the tail walk below
     * skip the right number of labels, and the stencil bakes it as a hole because the
     * body clamps the key against it. */
    case JOP_BRTABLE_COUNT: { uint32_t v = 0; bbq_read_uleb128_u32(c, &v); return v; }
    default: return 0;
    }
}

/* A variable-length trailing immediate: the op's meta says what kind, the walk skips it
 * so the NEXT stencil is placed at the right source offset. br_table's count has already
 * been consumed as a fixed operand, so only the count+1 labels remain. */
static void skip_tail(bbq_ctx_t* c, jit_tail_kind_t k, uint64_t brtable_count) {
    if (k != JTAIL_BRTABLE) return;
    for (uint64_t i = 0; i <= brtable_count; i++) { uint32_t l = 0; bbq_read_uleb128_u32(c, &l); }
}

typedef void* jit_addr_t;
typedef struct { jit_codebuf_t buf; size_t entry_off; jit_addr_t* offmap; } jit_func_t;

static jit_func_t* jit_compile(bbq_ctx_t code) {
    jit_func_t* fn = malloc(sizeof *fn);
    jit_codebuf_t buf; jcb_init(&buf, 4096);
    size_t code_len = code.length, cap = code_len + 2;
    size_t* offs = malloc(cap * sizeof *offs);
    int*    sids = malloc(cap * sizeof *sids);
    size_t* boffs = malloc(cap * sizeof *boffs);
    jit_addr_t* offmap = calloc(code_len + 1, sizeof *offmap);
    data_hole_t* recs = malloc((cap + 4) * 8 * sizeof *recs);
    int nrec = 0; size_t n = 0;

    size_t entry_off = emit_stencil(&buf, &stencil_table[STENCIL_ENTRY], NULL, recs, &nrec);
    const StencilDef* rd = &stencil_table[STENCIL_RESYNC];
    uint64_t rvals[16] = {0}; int rh;
    if ((rh = find_hole(rd, "_HOLE_offmap"))  >= 0) rvals[rh] = (uint64_t)(uintptr_t)offmap;
    if ((rh = find_hole(rd, "_HOLE_codelen")) >= 0) rvals[rh] = code_len;
    size_t resync_off = emit_stencil(&buf, rd, rvals, recs, &nrec);
    size_t trap_off   = emit_stencil(&buf, &stencil_table[STENCIL_TRAP], NULL, recs, &nrec);

    bbq_ctx_t cur = code;
    for (;;) {
        size_t bpos = cur.pos;
        uint8_t op;
        if (!bbq_read_u8(&cur, &op)) {
            boffs[n] = bpos;
            offs[n] = emit_stencil(&buf, &stencil_table[STENCIL_GEN_ST_HALT], NULL, recs, &nrec);
            sids[n] = STENCIL_GEN_ST_HALT; n++;
            break;
        }
        calc_jit_meta_t m = calc_jit_meta[op];
        const StencilDef* def = &stencil_table[m.stencil];
        uint64_t vals[16] = {0};
        uint64_t brtable_count = 0;
        fill_native_holes(def, vals);
        for (int k = 0; k < m.operand_count; k++) {
            uint64_t imm = (m.operands[k].kind == JOP_CONST) ? m.operands[k].value
                                                            : decode_operand(&cur, m.operands[k].kind);
            if (m.operands[k].kind == JOP_BRTABLE_COUNT) brtable_count = imm;
            int h = find_hole(def, m.operands[k].hole);
            if (h >= 0) vals[h] = imm;
        }
        skip_tail(&cur, (jit_tail_kind_t)m.tail, brtable_count);
        int hip = find_hole(def, "_HOLE_ip");
        if (hip >= 0) vals[hip] = cur.pos;
        boffs[n] = bpos;
        offs[n] = emit_stencil(&buf, def, vals, recs, &nrec);
        sids[n] = m.stencil; n++;
    }

    /* Shared footer pool for the rip-relative data loads (operands, consts). */
    for (int i = 0; i < nrec; i++) {
        size_t foff = (size_t)-1;
        for (int j = 0; j < i; j++) if (recs[j].value == recs[i].value) { foff = recs[j].foff; break; }
        if (foff == (size_t)-1) { foff = buf.size; jcb_emit(&buf, (const uint8_t*)&recs[i].value, 8); }
        recs[i].foff = foff;
    }
    for (int i = 0; i < nrec; i++)
        jcb_patch32(&buf, recs[i].patch_addr, (int32_t)((long)recs[i].foff - (long)(recs[i].patch_addr + 4)));

    /* Linear fall-through chain + control -> the one resync; guards -> trap. */
    uint8_t* base = buf.base;
    const StencilDef* ed = &stencil_table[STENCIL_ENTRY];
    backpatch(&buf, entry_off, ed, find_hole(ed, "_HOLE_cont"), (uint64_t)(base + offs[0]));
    for (size_t i = 0; i < n; i++) {
        const StencilDef* d = &stencil_table[sids[i]];
        if (i + 1 < n) { int hc = find_hole(d, "_HOLE_cont"); if (hc >= 0) backpatch(&buf, offs[i], d, hc, (uint64_t)(base + offs[i + 1])); }
        int hr = find_hole(d, "_HOLE_resync"); if (hr >= 0) backpatch(&buf, offs[i], d, hr, (uint64_t)(base + resync_off));
        int ht = find_hole(d, "_HOLE_trap");   if (ht >= 0) backpatch(&buf, offs[i], d, ht, (uint64_t)(base + trap_off));
    }

    void* exec = jcb_finalize(&buf);
    for (size_t i = 0; i < n; i++) if (boffs[i] <= code_len) offmap[boffs[i]] = (jit_addr_t)((uint8_t*)exec + offs[i]);
    offmap[code_len] = (jit_addr_t)((uint8_t*)exec + offs[n - 1]);   /* halt stamped last */

    free(offs); free(sids); free(boffs); free(recs);
    fn->buf = buf; fn->entry_off = entry_off; fn->offmap = offmap;
    return fn;
}

static void jit_enter(const jit_func_t* fn, vm_t* vm) {
    ((void (*)(vm_t*))((uint8_t*)fn->buf.base + fn->entry_off))(vm);
}
static void jit_free(jit_func_t* fn) { jcb_free(&fn->buf); free(fn->offmap); free(fn); }

static s4 jit_run(vm_t* vm, const u1* code, size_t len) {
    reset_vm(vm, code, len);
    jit_func_t* fn = jit_compile(vm->frame.code);
    jit_enter(fn, vm);
    jit_free(fn);
    return vm->result.i;
}

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
        g_sidetable = tbl;
        u1 c[] = {0x01,0x00, 0x28,0x02,0x00,0x01,0x02,
                  0x01,0x0A,0x10, 0x01,0x14,0x10, 0x01,0x1E,0x10};
        c[1] = 0; check("br_table key 0", c, sizeof c, 10);
        c[1] = 1; check("br_table key 1", c, sizeof c, 20);
        c[1] = 7; check("br_table key clamps to default", c, sizeof c, 30);
        g_sidetable = NULL;
    }

    /* The entry's vals/pop half: a taken branch keeps the top `vals` operands and drops
     * `pop` beneath them. Here 33 is kept and 22 dropped, so the trailing `add` sees
     * 11 + 33; without the drop it would see 22 + 33. */
    {
        static const opgen_st_entry_t tbl[] = { { 0, 0, 1, 1 } };
        g_sidetable = tbl;
        /*  0: const 11  2: const 22  4: const 33  6: const 0(key)
         *  8: br_table cnt=0 [0]     11: add      12: ret */
        static const u1 c[] = {0x01,0x0B, 0x01,0x16, 0x01,0x21, 0x01,0x00,
                               0x28,0x00,0x00, 0x02, 0x10};
        check("br_table entry keeps vals, drops pop", c, sizeof c, 44);
        g_sidetable = NULL;
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
