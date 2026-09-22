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
#include "calc_jit_meta.h"         /* opgen: calc_jit_meta[256], the tier-2 variant tables */
#include "calc_sigtab.h"           /* opgen: storage classes — calc_sigtab[], calc_class_* */
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
            /* 0 = not known yet, which is every branch hole: they name another
             * stencil in this buffer and are backpatched after the seal. A
             * non-zero one here would be a displacement written while the buffer
             * can still move, so jcb_patch_rel32 refuses it (pre-seal) and
             * finalize hands back nothing — loudly, rather than a branch to
             * wherever the buffer used to be. */
            uint64_t t = vals ? vals[p->hole_index] : 0;
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
 * when it was stamped, because it is the stencil that comes after it.
 *
 * `hi` < 0 means the caller looked the hole up and did not find it. Returning
 * early says so; falling into the loop would compare it against an unsigned
 * hole_index, match nothing, and patch nothing — the same silence either way,
 * except that this one is the answer rather than an accident. */
static inline void calc_backpatch(jit_codebuf_t* buf, size_t base,
                                  const StencilDef* s, int hi, uint64_t t) {
    if (hi < 0) return;
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
/* The room a stamp has for one stencil's hole values. It bounds what the
 * GENERATED table may contain, and the table is toolchain output whose holes
 * track the C compiler that produced the stencils — so exceeding it is a refusal
 * here, never a write past the array. */
#define CALC_JIT_MAX_HOLES 16

/* Returns 0 for a stencil with more holes than the caller's array holds. */
static inline int calc_fill_native_holes(const StencilDef* d, uint64_t* v, int nv) {
    if (d->hole_count > nv) return 0;
    for (int i = 0; i < d->hole_count; i++) {
        void* a = calc_jit_sym(d->hole_names[i]);
        if (a) v[i] = (uint64_t)(uintptr_t)a;
    }
    return 1;
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

/* One decoded instruction — everything both walks below need. Decoding happens
 * once, in calc_decode_insn, so the pass that finds branch targets and the pass
 * that stamps cannot disagree about where an instruction ends.
 *
 * The operands are kept as VALUES, not as a filled-in hole array: which hole
 * index a name has is a property of the stencil, and tier 2 stamps a different
 * stencil from the one the meta names. calc_insn_vals resolves them against
 * whichever variant the walk picked. */
typedef struct {
    size_t   pos;        /* its first byte */
    size_t   next;       /* the byte after it, operands and label vector included */
    uint8_t  op;
    uint64_t imm[8];     /* decoded operand values, in the meta's own order */
    int64_t  branch;     /* the absolute code offset it can transfer to, or -1 */
    int      ctl;        /* it can redirect the ip (it carries _HOLE_resync) */
    int      dynamic;    /* ...to somewhere only the RUNTIME knows */
    int      unknown;    /* calc.def declares no such opcode */
} calc_insn_t;

static inline int calc_decode_insn(bbq_ctx_t* c, calc_insn_t* in) {
    in->pos = c->pos; in->branch = -1; in->ctl = in->dynamic = in->unknown = 0;
    memset(in->imm, 0, sizeof in->imm);
    if (!bbq_read_u8(c, &in->op)) { in->next = c->pos; return 0; }
    /* A byte calc.def never declared. There is no meta to read and no operand
     * length to skip, so the walk cannot even find the next instruction — and the
     * zero-filled meta row names stencil 0, which is a real stencil belonging to
     * some other opcode. Stop here and let the caller refuse: the calc ships no
     * validator, so bytecode reaching the JIT has not been checked by anything. */
    if (!calc_opcode_sig[in->op].present) { in->unknown = 1; in->next = c->pos; return 1; }
    calc_jit_meta_t m = calc_jit_meta[in->op];
    const StencilDef* def = &stencil_table[m.stencil];
    uint64_t brtable_count = 0;
    for (int k = 0; k < m.operand_count; k++) {
        in->imm[k] = (m.operands[k].kind == JOP_CONST)
                       ? m.operands[k].value
                       : calc_decode_operand(c, m.operands[k].kind);
        if (m.operands[k].kind == JOP_BRTABLE_COUNT) brtable_count = in->imm[k];
        /* calc.def's two ABSOLUTE code offsets: `off` (br_if/br_ifz/goto) and
         * `entry` (call). Both natives bbq_seek straight to them, so the operand
         * IS the target and the target scan below needs no second decoder. */
        if (!strcmp(m.operands[k].hole, "_HOLE_off") ||
            !strcmp(m.operands[k].hole, "_HOLE_entry"))
            in->branch = (int64_t)(int32_t)(uint32_t)in->imm[k];
    }
    calc_skip_tail(c, (jit_tail_kind_t)m.tail, brtable_count);
    in->next = c->pos;
    /* An instruction that can redirect the ip carries `_HOLE_resync`; where it
     * redirects TO is the question. `off`/`entry` answer it above, and `ret`/`trap`
     * land on a fall-through — a return site, or the byte after the trap — which
     * invariant (2) already holds at state 0. br_table's targets live in a side
     * table the HOST fills at run time, which nothing here can read, and so would
     * any op calc.def grows that resyncs without naming where: both are `dynamic`,
     * and a dynamic target puts the whole function back at state 0 throughout. */
    in->ctl = calc_find_hole(def, "_HOLE_resync") >= 0;
    if (in->ctl && in->branch < 0 && in->op != OP_RET && in->op != OP_TRAP)
        in->dynamic = 1;
    return 1;
}

/* This instruction's holes, resolved against the stencil actually being stamped.
 * Returns 0 if the stencil has more holes than `vals` holds, which the caller
 * turns into a refusal rather than stamping a stencil it could not fill. */
static inline int calc_insn_vals(const calc_insn_t* in, const StencilDef* def,
                                 uint64_t* vals) {
    calc_jit_meta_t m = calc_jit_meta[in->op];
    memset(vals, 0, CALC_JIT_MAX_HOLES * sizeof *vals);
    if (!calc_fill_native_holes(def, vals, CALC_JIT_MAX_HOLES)) return 0;
    for (int k = 0; k < m.operand_count; k++) {
        int h = calc_find_hole(def, m.operands[k].hole);
        if (h >= 0) vals[h] = in->imm[k];
    }
    int hip = calc_find_hole(def, "_HOLE_ip");
    if (hip >= 0) vals[hip] = in->next;
    return 1;
}

/* ── Tier 2: Ertl's stack cache ─────────────────────────────────────────────
 *
 * At `-tier2 N` opgen emits, per opcode, a FAMILY of stencils instead of one:
 * `__sK` for each entry state K in 0..N, where state K says the top K SLOTS of
 * the operand stack are not on the stack at all — they are live in CACHE_R0..
 * R(K-1), arguments of CACHE_ARGS that preserve_none keeps in registers across
 * the tail call from one stencil to the next. CACHE_R0 is the TOP, R(K-1) the
 * deepest, which is why a spill moves R(K-1) first: it is the one that has to
 * land on the stack under the others. A state counts SLOTS and not values — a
 * v128 spends two of them — so `calc_class_width` is what steps an index.
 *
 * Six published tables drive it, and not one of them is recomputable here:
 *   calc_variant[base][st]     the variant to stamp at entry state `st`, or -1
 *   calc_variant_fs[base][st]  the state it LEAVES. READ IT. It is not the
 *                              arity: a result that went to memory moves the
 *                              stack exactly like one that stayed in a register
 *   calc_variant_m[base][st]   the same operands, result pushed inline
 *   calc_spill[class][slot]    push CACHE_R<slot> onto the stack; the inverse is
 *   calc_fill[class][slot]
 *   calc_sigtab / calc_class_* the storage class of every slot, and its width
 *
 * ── The two invariants ──
 *
 * (1) Every instruction runs at the state the machine is actually in. A state is
 *     a property of a PROGRAM POINT, so the walk carries one and the instruction
 *     either has a variant for it or the walk spills down until it does. Nothing
 *     here ever fills: a descend-only tiler has no reason to, because state 0 is
 *     the plain stencil and always exists. javelina's driver fills, because its
 *     burg cover asks for operands in registers before they are there — that is
 *     what a cover buys and what this walk does without.
 *
 * (2) Control arrives at state 0, and leaves at state 0. `resync` forwards
 *     CACHE_PASS to whatever offmap[ip] names, so the branch and its target must
 *     agree; requiring 0 at both ends is the cheapest agreement there is. It
 *     costs nothing extra in practice because a branch CONSUMES its condition:
 *     `br_if` entered at state 1 leaves at state 0 on both paths, so the walk
 *     spills until `calc_variant_fs` says 0 rather than flushing on sight.
 *
 * The one thing the driver may not do is name a class opgen did not: a cache
 * register holds a value's bits and no tag, so a slot goes in only when the
 * signature says which of the six classes it is. Everything else — every `addr`,
 * `word` and `any` slot, the whole variadic group — runs at state 0, where the
 * value is on the stack with its tag beside it.
 */

/* The live cache: how many slots, and what class each holds. Index 0 is the TOP.
 * The array is never empty, so a `-tier2 0` build compiles unchanged — there the
 * tables degenerate to `calc_variant[base][0] == <the plain stencil>` and
 * `calc_variant_fs[base][0] == 0`, and this whole walk is tier 1 again. */
typedef struct { int n; uint8_t cls[CALC_TIER2_N ? CALC_TIER2_N : 1]; } calc_cache_t;

/* The deepest state a tiling will use. Lowering it is Ertl's own knob (§2.3: the
 * cache depth is a tuning parameter), and it lets one generated VM be tiled at
 * every depth from tier-1-equivalent up to what opgen emitted. */
static int calc_jit_cap = CALC_TIER2_N;
static inline void calc_jit_set_cap(int k) {
    calc_jit_cap = k < 0 ? 0 : (k > CALC_TIER2_N ? CALC_TIER2_N : k);
}

/* What the tiling did. The result of a program cannot tell a cached run from an
 * uncached one — that is the whole point of the cache — so the only way to claim
 * the stack cache ran is to count what was stamped. */
typedef struct {
    unsigned stamped;       /* instruction stencils */
    unsigned cached;        /* ...of those, entered at a state above 0 */
    unsigned plain;         /* ...of those, forced onto the uncached tier-1 form */
    unsigned spills;        /* transition stencils */
    unsigned max_state;     /* the deepest state the walk reached */
    unsigned targets;       /* code offsets control can arrive at */
    unsigned dynamic;       /* instructions whose target only the runtime knows */
} calc_jit_stats_t;
static calc_jit_stats_t calc_jit_stats;

static inline const calc_sig_t* calc_op_sig(uint8_t op) {
    return calc_opcode_sig[op].present ? &calc_sigtab[calc_opcode_sig[op].sig] : NULL;
}

/* Slots above signature parameter k — its slot index, counted from the top. -1
 * where a parameter above it has no width to count: `calc_class_width` is indexed
 * by a FINAL class, and an `addr` or `poly` slot is resolved by a tile rather than
 * named by the signature. Every such signature in this vocabulary happens to put
 * its unresolved parameter first, where the caller's own range check catches it,
 * so this is the case that has never run — which is exactly the case to answer
 * rather than index past the end of a seven-entry table for. */
static inline int calc_param_slot(const calc_sig_t* sg, int k) {
    int b = 0;
    for (int j = sg->nparams - 1; j > k; j--) {
        if (sg->params[j] >= CALC_SCLASS_FINAL) return -1;
        b += calc_class_width[sg->params[j]];
    }
    return b;
}

/* Can this instruction run at entry state `st` with this cache? A no to any of
 * the questions below means the walk spills and asks again one slot down.
 * `exit0` is invariant (2) asking as well: a control transfer must LEAVE at
 * state 0, because where it lands agreed to arrive at one. */
static inline int calc_state_ok(uint8_t op, int base, int st,
                                const calc_cache_t* c, int exit0) {
    const calc_sig_t* sg = calc_op_sig(op);
    if (!sg || st > calc_jit_cap) return 0;
    if (exit0 && calc_variant_fs[base][st] != 0) return 0;
    if (st > 0) {
        if (calc_variant[base][st] < 0) return 0;
        /* Every operand this state reads from a register must BE the class the
         * stencil reads it as. `addr`, `word`, `any` and the variadic group have
         * no class here to check against — the tile resolves those, and this walk
         * is not a tile — so they keep the instruction at state 0. */
        for (int k = 0; k < sg->nparams; k++) {
            int cl = sg->params[k], w, slot, q;
            if (cl >= CALC_SCLASS_FINAL || cl == JSC_STK) return 0;
            w = calc_class_width[cl];
            slot = calc_param_slot(sg, k);
            if (slot < 0) return 0;                 /* an unresolved slot sits above it */
            if (slot + w > st) continue;            /* it comes off the stack */
            for (q = 0; q < w; q++) if (c->cls[slot + q] != cl) return 0;
        }
    }
    /* A result the variant KEEPS must be a class this walk can name, or the spill
     * that eventually returns it to memory would re-tag it as something else. */
    {
        int pops = calc_jit_meta[op].pop_slots;
        int left = st > pops ? st - pops : 0;
        int fs   = calc_variant_fs[base][st], cl;
        if (fs < 0) return 0;
        if (fs == left) return 1;                   /* it went to the stack */
        if (sg->nresults != 1) return 0;
        cl = sg->results[0];
        if (cl >= CALC_SCLASS_FINAL || !calc_class_cacheable[cl]) return 0;
        if (fs - left != calc_class_width[cl]) return 0;
        if (fs > calc_jit_cap) return 0;
    }
    return 1;
}

/* The cache the variant leaves behind. The survivors keep their order and shift
 * down under the result — which is exactly what the variant's own
 * `CACHE_R1 = CACHE_R2` does, so this reads the shift off the same fs the
 * stencil was emitted from rather than re-deriving it. */
static inline void calc_cache_after(uint8_t op, int base, int st, calc_cache_t* c) {
    const calc_sig_t* sg = calc_op_sig(op);
    int pops = calc_jit_meta[op].pop_slots;
    int left = st > pops ? st - pops : 0;
    int fs   = calc_variant_fs[base][st];
    int rw   = fs - left, i;
    for (i = left - 1; i >= 0; i--) c->cls[rw + i] = c->cls[(st - left) + i];
    for (i = 0; i < rw; i++) c->cls[i] = (uint8_t)sg->results[0];
    c->n = fs;
}

typedef void* calc_jit_addr_t;
typedef struct { jit_codebuf_t buf; size_t entry_off; calc_jit_addr_t* offmap; } calc_jit_func_t;

/* The stamping cursor: where a stencil goes and what is remembered about it. The
 * spill helper and the instruction walk share it, so a transition is recorded
 * exactly like an instruction and the chaining pass below needs no second case. */
typedef struct {
    jit_codebuf_t*    buf;
    size_t*           offs;    /* where each stamped stencil landed in the buffer */
    int*              sids;    /* which stencil it was */
    size_t*           boffs;   /* the bytecode offset it belongs to */
    calc_data_hole_t* recs;
    int*              nrec;
    size_t            n;
} calc_stamp_t;

static inline size_t calc_stamp(calc_stamp_t* s, int sid, const uint64_t* vals,
                                size_t bpos) {
    s->boffs[s->n] = bpos;
    s->offs[s->n] = calc_emit_stencil(s->buf, &stencil_table[sid], vals, s->recs, s->nrec);
    s->sids[s->n] = sid;
    return s->n++;
}

/* Move the DEEPEST cached value to the stack — the one that has to land under
 * the rest. `calc_spill[class][slot]` is the stencil; the slot is where the value
 * starts, which for a two-slot v128 is one below the top of the cache.
 *
 * The spill is stamped under the PREVIOUS instruction's byte offset, never the
 * one it is making room for: an ip means where its instruction BEGINS, and
 * control arriving there must run the instruction, not a transition stamped
 * ahead of it. Returns 0 when the class has no spill at that slot — a cache this
 * walk filled and cannot empty, which fails the compile rather than quietly
 * dropping the value. */
static inline int calc_spill_one(calc_stamp_t* s, calc_cache_t* c, size_t bpos) {
    int cl   = c->cls[c->n - 1];
    int w    = (cl <= JSC_REF) ? calc_class_width[cl] : 0;
    int slot = c->n - w;
    int sid  = (w && slot >= 0) ? calc_spill[cl][slot] : -1;
    if (sid < 0) return 0;
    calc_stamp(s, sid, NULL, bpos);
    calc_jit_stats.spills++;
    c->n -= w;
    return 1;
}

/* Bytecode in, a chain of stamped stencils out. NULL if the buffer did not come
 * out whole — a short emit, a branch that did not reach, or a cached slot with no
 * spill to move it. */
static inline calc_jit_func_t* calc_jit_compile(bbq_ctx_t code) {
    calc_jit_func_t* fn = (calc_jit_func_t*)malloc(sizeof *fn);
    if (!fn) return NULL;
    /* One page to start; the buffer grows (and moves) during the emit phase. */
    jit_codebuf_t buf;
    if (jcb_init(&buf, 4096) != 0) { free(fn); return NULL; }
    size_t code_len = code.length;
    /* A tiled instruction can imply spills as well as itself: one per live slot,
     * at most the cache depth. */
    size_t cap = (code_len + 2) * (size_t)(CALC_TIER2_N + 1);
    size_t* offs = (size_t*)malloc(cap * sizeof *offs);
    int*    sids = (int*)malloc(cap * sizeof *sids);
    size_t* boffs = (size_t*)malloc(cap * sizeof *boffs);
    uint8_t* is_target = (uint8_t*)calloc(code_len + 1, 1);
    calc_jit_addr_t* offmap = (calc_jit_addr_t*)calloc(code_len + 1, sizeof *offmap);
    calc_data_hole_t* recs = (calc_data_hole_t*)malloc((cap + 4) * 8 * sizeof *recs);
    int nrec = 0, bad = 0;
    calc_stamp_t sp; sp.buf = &buf; sp.offs = offs; sp.sids = sids; sp.boffs = boffs;
    sp.recs = recs; sp.nrec = &nrec; sp.n = 0;
    calc_cache_t cache; cache.n = 0;
    calc_insn_t in;

    memset(&calc_jit_stats, 0, sizeof calc_jit_stats);

    /* Pass 1 — where control can ARRIVE. Invariant (2) needs the set before the
     * stamping starts, because a backward branch names a target the walk has
     * already gone past. A fall-through is not in it: every control transfer is
     * tiled to LEAVE at state 0, so the byte after one is already there. */
    {
        bbq_ctx_t scan = code;
        while (calc_decode_insn(&scan, &in)) {
            if (in.unknown) { bad = 1; break; }
            if (in.branch >= 0 && (size_t)in.branch <= code_len)
                is_target[in.branch] = 1;
            if (in.dynamic) calc_jit_stats.dynamic++;
        }
        for (size_t i = 0; i <= code_len; i++) calc_jit_stats.targets += is_target[i];
    }

    size_t entry_off = calc_emit_stencil(&buf, &stencil_table[STENCIL_ENTRY], NULL, recs, &nrec);
    const StencilDef* rd = &stencil_table[STENCIL_RESYNC];
    uint64_t rvals[16] = {0}; int rh;
    if ((rh = calc_find_hole(rd, "_HOLE_offmap"))  >= 0) rvals[rh] = (uint64_t)(uintptr_t)offmap;
    if ((rh = calc_find_hole(rd, "_HOLE_codelen")) >= 0) rvals[rh] = code_len;
    size_t resync_off = calc_emit_stencil(&buf, rd, rvals, recs, &nrec);
    size_t trap_off   = calc_emit_stencil(&buf, &stencil_table[STENCIL_TRAP], NULL, recs, &nrec);

    /* Pass 2 — stamp. */
    bbq_ctx_t cur = code;
    size_t last_bpos = 0;
    for (;;) {
        int at_end = !calc_decode_insn(&cur, &in);
        if (in.unknown) { bad = 1; break; }
        /* Three reasons the cache must be empty before the next stencil runs:
         * control arrives here (invariant 2); a side table could send it anywhere
         * in this function, so every offset is an arrival; or the code has run out
         * and `halt` keeps nothing — the results ARE the top of the frame stack,
         * and a value still in a register is a value the caller never sees. */
        if (at_end || is_target[in.pos] || calc_jit_stats.dynamic)
            while (cache.n > 0)
                if (!calc_spill_one(&sp, &cache, last_bpos)) { bad = 1; break; }
        if (bad) break;
        if (at_end) { calc_stamp(&sp, STENCIL_GEN_ST_HALT, NULL, in.pos); break; }

        int base = calc_jit_meta[in.op].stencil;
        /* Descend to a state this instruction's family carries with the classes
         * the cache is actually holding. State 0 is the plain stencil and always
         * exists, so this terminates. */
        while (cache.n > 0 && !calc_state_ok(in.op, base, cache.n, &cache, in.ctl))
            if (!calc_spill_one(&sp, &cache, last_bpos)) { bad = 1; break; }
        if (bad) break;

        /* At state 0 the family's own `__s0` may still keep its result in a
         * register. Where that result is a class this walk cannot name — a
         * `word`, an `any`, a variadic group — the PLAIN stencil is the form that
         * puts it on the stack with its tag, and the meta already names it. */
        int st = cache.n, keeps = calc_state_ok(in.op, base, st, &cache, in.ctl);
        int chosen = keeps ? calc_variant[base][st] : base;
        uint64_t vals[CALC_JIT_MAX_HOLES];
        if (!calc_insn_vals(&in, &stencil_table[chosen], vals)) { bad = 1; break; }
        calc_stamp(&sp, chosen, vals, in.pos);
        calc_jit_stats.stamped++;
        if (st > 0) calc_jit_stats.cached++;
        if (!keeps) calc_jit_stats.plain++;
        if (keeps) calc_cache_after(in.op, base, st, &cache);
        if ((unsigned)cache.n > calc_jit_stats.max_state)
            calc_jit_stats.max_state = (unsigned)cache.n;
        last_bpos = in.pos;
    }
    size_t n = sp.n;

    /* Shared footer pool for the rip-relative data loads (operands, consts). */
    for (int i = 0; i < nrec; i++) {
        size_t foff = (size_t)-1;
        for (int j = 0; j < i; j++) if (recs[j].value == recs[i].value) { foff = recs[j].foff; break; }
        if (foff == (size_t)-1) { foff = buf.size; jcb_emit(&buf, (const uint8_t*)&recs[i].value, 8); }
        recs[i].foff = foff;
    }
    /* The pool was the last thing emitted, so the layout is final: seal, and every
     * displacement below is computed against the address the code will run at. */
    jcb_seal(&buf);

    for (int i = 0; i < nrec; i++)
        jcb_patch_rel32(&buf, recs[i].patch_addr,
                        (uint64_t)(uintptr_t)(buf.base + recs[i].foff));

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

    void* exec = bad ? NULL : jcb_finalize(&buf);
    /* NULL means something did not fit — a short emit, a branch out of rel32
     * range, or a cached slot with no spill to move it. Handing it back would
     * hand back code that was never stamped. */
    if (exec) {
        /* First write wins. An ip means where its instruction BEGINS, so a spill
         * stamped after it under the same offset must not claim it — arriving
         * there from a branch would then re-run a transition the branch already
         * satisfied, and on a loop back edge skip the body's own instruction
         * forever.
         *
         * boffs and offs are read HERE, which is why they are freed below and not
         * beside the buffer: the frees used to sit above this loop, and it read
         * them back out of the freed blocks. It survived on an allocator that had
         * not yet reused them — until the walk asked for a bigger scratch. */
        for (size_t i = 0; i < n; i++)
            if (boffs[i] <= code_len && !offmap[boffs[i]])
                offmap[boffs[i]] = (calc_jit_addr_t)((uint8_t*)exec + offs[i]);
        offmap[code_len] = (calc_jit_addr_t)((uint8_t*)exec + offs[n - 1]);  /* halt is last */
    }
    free(offs); free(sids); free(boffs); free(recs); free(is_target);
    if (!exec) { free(offmap); jcb_free(&buf); free(fn); return NULL; }

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
