/*
 * calc_vm_test.c — runs calc bytecode through BOTH tiers of the opgen-generated
 * VM and asserts they agree. opgen turned calc.def into gen_interp.c (the
 * threaded interpreter) AND wasm_stencils.c (the copy-and-patch JIT stencils +
 * the generated machinery stencils). This drives:
 *   tier 0 — interp_run    (javelina's interp.c, smaller)
 *   tier 1 — jit_run        (javelina's jit_driver.c, stripped to the calc)
 * interp == JIT by construction: both run the same generated opcode bodies.
 */
#include "gen_interp.h"            /* dispatch-table accessor + runtime_api.h (vm_t, slot_t) */
#include "opcodes.h"               /* OP_* */
#include "bbq_runtime.h"           /* bbq_ctx_t + bbq_read_* */
#include "wasm_stencil_table.h"    /* jitterator: stencil_table[], STENCIL_*, StencilDef, PatchEntry */
#include "wasm_jit_meta.h"         /* opgen: wasm_jit_meta[256], jit_operand_kind_t */
#include "wasm_jit_symbols.h"      /* opgen: _HOLE_<native> -> address */
#include "jit_codebuf.h"           /* jitterator's executable code buffer */
#include "calc_natives.h"          /* calc_br / calc_call / calc_ret (shared, both tiers) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define TAIL __attribute__((musttail))

/* ── Tier 0: the threaded interpreter. ── */
static const opcode_handler_t* g_table;
void wasm_next(vm_t* vm) {
    u1 op;
    if (!bbq_read_u8(&vm->frame.code, &op)) return;
    TAIL return g_table[op](vm);
}
void wasm_trap(vm_t* vm) { vm->trapped = 1; }

static s4 interp_run(vm_t* vm, const u1* code, size_t len) {
    g_table = gen_interp_dispatch_table();
    bbq_ctx_init(&vm->frame.code, code, len);
    vm->frame.sp = 0; vm->depth = 0; vm->trapped = 0; vm->result.i = 0; vm->result_type = T_INT;
    wasm_next(vm);
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
    for (int i = 0; i < wasm_jit_symbols_count; i++) if (!strcmp(wasm_jit_symbols[i].name, n)) return wasm_jit_symbols[i].addr;
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
    default: return 0;
    }
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
        wasm_jit_meta_t m = wasm_jit_meta[op];
        const StencilDef* def = &stencil_table[m.stencil];
        uint64_t vals[16] = {0};
        fill_native_holes(def, vals);
        for (int k = 0; k < m.operand_count; k++) {
            uint64_t imm = (m.operands[k].kind == JOP_CONST) ? m.operands[k].value
                                                            : decode_operand(&cur, m.operands[k].kind);
            int h = find_hole(def, m.operands[k].hole);
            if (h >= 0) vals[h] = imm;
        }
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
    bbq_ctx_init(&vm->frame.code, code, len);
    vm->frame.sp = 0; vm->depth = 0; vm->trapped = 0; vm->result.i = 0; vm->result_type = T_INT;
    jit_func_t* fn = jit_compile(vm->frame.code);
    jit_enter(fn, vm);
    jit_free(fn);
    return vm->result.i;
}

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

    if (!fails) { printf("\ncalc VM (interp == JIT): all checks passed\n"); return 0; }
    printf("\ncalc VM: %d FAILED\n", fails);
    return 1;
}
