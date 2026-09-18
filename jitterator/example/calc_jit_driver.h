/*
 * calc_jit_driver.h — tier 1: stamping jitterator's stencils into running code.
 *
 * This is the consumer side of the whole toolchain, and the shortest statement of
 * what copy-and-patch costs a host. For each opcode in the bytecode: look up the
 * stencil opgen assigned it, copy its bytes into the buffer, fill its holes with
 * this instruction's operands and the addresses of the natives it calls, and
 * remember where it landed. Then thread them together — each stencil's `_HOLE_cont`
 * to the next one's address — and the whole function is a chain of tail calls with
 * no dispatch left in it.
 *
 * Three hole kinds and three ways to fill them:
 *   branch  (_HOLE_cont/_HOLE_resync/_HOLE_trap)  a rel32 to another stencil HERE
 *   data    (operands, float constants, natives)  a slot in the footer pool, which
 *                                                 the stencil loads RIP-relatively
 *   absolute                                      written in place
 * A native is reached through the pool rather than branched to: the pool holds a
 * full 64-bit address, which is why this needs no trampolines.
 *
 * It lives beside the example rather than inside a test because it is the example.
 * Header-only, C, `extern "C"` for the C++ leg.
 */
#ifndef CALC_JIT_DRIVER_H
#define CALC_JIT_DRIVER_H

#include "calc_host.h"             /* vm_t, calc_reset */
#include "calc_stencil_table.h"    /* jitterator: stencil_table[], STENCIL_*, StencilDef */
#include "calc_jit_meta.h"         /* opgen: calc_jit_meta[256], jit_operand_kind_t */
#include "calc_jit_symbols.h"      /* opgen: _HOLE_<native> -> address */
#include "jit_codebuf.h"           /* jitterator's executable code buffer */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A RIP-relative load whose target goes in the footer pool: where to patch, what
 * value, and (once the pool is laid out) which slot it got. */
typedef struct { size_t patch_addr; uint64_t value; size_t foff; } calc_data_hole_t;

static inline size_t calc_emit_stencil(jit_codebuf_t* buf, const StencilDef* s,
                                       const uint64_t* vals,
                                       calc_data_hole_t* recs, int* nrec) {
    size_t base = buf->size;
    jcb_emit(buf, s->code, s->code_size);
    for (uint32_t i = 0; i < s->patch_count; i++) {
        const PatchEntry* p = &s->patches[i];
        size_t pa = base + p->offset;
        switch (p->type) {
        case PATCH_REL_BRANCH: {
            uint64_t t = vals ? vals[p->hole_index] : 0;
            /* rel32 reaches +-2 GB. Every branch hole here targets another stencil
             * in this same buffer, so it always does — the check is what keeps
             * that true if a stencil ever tail-calls a native instead. */
            if (t) jcb_patch_rel32(buf, pa, t);
            break;
        }
        case PATCH_REL_DATA:
            recs[*nrec].patch_addr = pa;
            recs[*nrec].value = vals ? vals[p->hole_index] : 0;
            (*nrec)++;
            break;
        case PATCH_ABS64:  jcb_patch64(buf, pa, vals ? vals[p->hole_index] : 0); break;
        case PATCH_ABS32S: jcb_patch32(buf, pa, (int32_t)(vals ? vals[p->hole_index] : 0)); break;
        }
    }
    return base;
}

/* Fill one branch hole of an already-stamped stencil — the target was not known
 * when it was stamped, because it is the stencil that comes after it. */
static inline void calc_backpatch(jit_codebuf_t* buf, size_t base,
                                  const StencilDef* s, int hi, uint64_t t) {
    for (uint32_t i = 0; i < s->patch_count; i++) {
        const PatchEntry* p = &s->patches[i];
        if ((int)p->hole_index != hi || p->type != PATCH_REL_BRANCH) continue;
        jcb_patch_rel32(buf, base + p->offset, t);
    }
}

static inline int calc_find_hole(const StencilDef* d, const char* n) {
    for (int i = 0; i < d->hole_count; i++) if (!strcmp(d->hole_names[i], n)) return i;
    return -1;
}

/* The name -> address lookup is opgen's own (calc_jit_symbols.h); the test used to
 * carry a second copy of it. */
static inline void calc_fill_native_holes(const StencilDef* d, uint64_t* v) {
    for (int i = 0; i < d->hole_count; i++) {
        void* a = calc_jit_sym(d->hole_names[i]);
        if (a) v[i] = (uint64_t)(uintptr_t)a;
    }
}

static inline uint64_t calc_decode_operand(bbq_ctx_t* c, jit_operand_kind_t k) {
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
static inline void calc_skip_tail(bbq_ctx_t* c, jit_tail_kind_t k, uint64_t brtable_count) {
    if (k != JTAIL_BRTABLE) return;
    for (uint64_t i = 0; i <= brtable_count; i++) { uint32_t l = 0; bbq_read_uleb128_u32(c, &l); }
}

typedef void* calc_jit_addr_t;
typedef struct { jit_codebuf_t buf; size_t entry_off; calc_jit_addr_t* offmap; } calc_jit_func_t;

/* Bytecode in, a chain of stamped stencils out. NULL if the buffer did not come
 * out whole — a short emit, or a branch that did not reach. */
static inline calc_jit_func_t* calc_jit_compile(bbq_ctx_t code) {
    calc_jit_func_t* fn = (calc_jit_func_t*)malloc(sizeof *fn);
    jit_codebuf_t buf; jcb_init(&buf, 4096);
    size_t code_len = code.length, cap = code_len + 2;
    size_t* offs = (size_t*)malloc(cap * sizeof *offs);
    int*    sids = (int*)malloc(cap * sizeof *sids);
    size_t* boffs = (size_t*)malloc(cap * sizeof *boffs);
    calc_jit_addr_t* offmap = (calc_jit_addr_t*)calloc(code_len + 1, sizeof *offmap);
    calc_data_hole_t* recs = (calc_data_hole_t*)malloc((cap + 4) * 8 * sizeof *recs);
    int nrec = 0; size_t n = 0;

    size_t entry_off = calc_emit_stencil(&buf, &stencil_table[STENCIL_ENTRY], NULL, recs, &nrec);
    const StencilDef* rd = &stencil_table[STENCIL_RESYNC];
    uint64_t rvals[16] = {0}; int rh;
    if ((rh = calc_find_hole(rd, "_HOLE_offmap"))  >= 0) rvals[rh] = (uint64_t)(uintptr_t)offmap;
    if ((rh = calc_find_hole(rd, "_HOLE_codelen")) >= 0) rvals[rh] = code_len;
    size_t resync_off = calc_emit_stencil(&buf, rd, rvals, recs, &nrec);
    size_t trap_off   = calc_emit_stencil(&buf, &stencil_table[STENCIL_TRAP], NULL, recs, &nrec);

    bbq_ctx_t cur = code;
    for (;;) {
        size_t bpos = cur.pos;
        uint8_t op;
        if (!bbq_read_u8(&cur, &op)) {
            boffs[n] = bpos;
            offs[n] = calc_emit_stencil(&buf, &stencil_table[STENCIL_GEN_ST_HALT], NULL, recs, &nrec);
            sids[n] = STENCIL_GEN_ST_HALT; n++;
            break;
        }
        calc_jit_meta_t m = calc_jit_meta[op];
        const StencilDef* def = &stencil_table[m.stencil];
        uint64_t vals[16] = {0};
        uint64_t brtable_count = 0;
        calc_fill_native_holes(def, vals);
        for (int k = 0; k < m.operand_count; k++) {
            uint64_t imm = (m.operands[k].kind == JOP_CONST)
                             ? m.operands[k].value
                             : calc_decode_operand(&cur, m.operands[k].kind);
            if (m.operands[k].kind == JOP_BRTABLE_COUNT) brtable_count = imm;
            int h = calc_find_hole(def, m.operands[k].hole);
            if (h >= 0) vals[h] = imm;
        }
        calc_skip_tail(&cur, (jit_tail_kind_t)m.tail, brtable_count);
        int hip = calc_find_hole(def, "_HOLE_ip");
        if (hip >= 0) vals[hip] = cur.pos;
        boffs[n] = bpos;
        offs[n] = calc_emit_stencil(&buf, def, vals, recs, &nrec);
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
        jcb_patch32(&buf, recs[i].patch_addr,
                    (int32_t)((long)recs[i].foff - (long)(recs[i].patch_addr + 4)));

    /* Linear fall-through chain + control -> the one resync; guards -> trap. */
    uint8_t* base = buf.base;
    const StencilDef* ed = &stencil_table[STENCIL_ENTRY];
    calc_backpatch(&buf, entry_off, ed, calc_find_hole(ed, "_HOLE_cont"), (uint64_t)(base + offs[0]));
    for (size_t i = 0; i < n; i++) {
        const StencilDef* d = &stencil_table[sids[i]];
        if (i + 1 < n) {
            int hc = calc_find_hole(d, "_HOLE_cont");
            if (hc >= 0) calc_backpatch(&buf, offs[i], d, hc, (uint64_t)(base + offs[i + 1]));
        }
        int hr = calc_find_hole(d, "_HOLE_resync");
        if (hr >= 0) calc_backpatch(&buf, offs[i], d, hr, (uint64_t)(base + resync_off));
        int ht = calc_find_hole(d, "_HOLE_trap");
        if (ht >= 0) calc_backpatch(&buf, offs[i], d, ht, (uint64_t)(base + trap_off));
    }

    void* exec = jcb_finalize(&buf);
    free(offs); free(sids); free(boffs); free(recs);
    /* NULL means something did not fit — a short emit, or a branch out of rel32
     * range. Handing it back would hand back code that was never stamped. */
    if (!exec) { free(offmap); jcb_free(&buf); free(fn); return NULL; }

    for (size_t i = 0; i < n; i++)
        if (boffs[i] <= code_len) offmap[boffs[i]] = (calc_jit_addr_t)((uint8_t*)exec + offs[i]);
    offmap[code_len] = (calc_jit_addr_t)((uint8_t*)exec + offs[n - 1]);   /* halt stamped last */

    fn->buf = buf; fn->entry_off = entry_off; fn->offmap = offmap;
    return fn;
}

static inline void calc_jit_enter(const calc_jit_func_t* fn, vm_t* vm) {
    ((void (*)(vm_t*))((uint8_t*)fn->buf.base + fn->entry_off))(vm);
}

static inline void calc_jit_free(calc_jit_func_t* fn) {
    jcb_free(&fn->buf); free(fn->offmap); free(fn);
}

/* Tier 1: compile the bytecode and run it. Aborts if the buffer did not come out
 * whole — there is nothing sensible to return, and running it anyway would run
 * code that was never stamped. */
static inline s4 calc_jit_run(vm_t* vm, const u1* code, size_t len) {
    calc_reset(vm, code, len);
    calc_jit_func_t* fn = calc_jit_compile(vm->frame.code);
    if (!fn) { fprintf(stderr, "jit: code buffer did not come out whole\n"); abort(); }
    calc_jit_enter(fn, vm);
    calc_jit_free(fn);
    return vm->result.i;
}

#ifdef __cplusplus
}
#endif

#endif /* CALC_JIT_DRIVER_H */
