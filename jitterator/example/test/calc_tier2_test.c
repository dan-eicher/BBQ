/*
 * calc_tier2_test.c — the STACK CACHE itself.
 *
 * calc_vm_test.c and calc_e2e_test.cpp already run every program on both tiers
 * and demand the same answer. They cannot tell a cached run from an uncached one
 * — which is the entire point of a cache — so a tiling that silently degraded to
 * state 0 everywhere would leave them all green. This file asks the questions
 * they cannot:
 *
 *   A. the tables opgen publishes are consistent with each other and with the
 *      stencil table that names them;
 *   B. the driver's state choice obeys its own contract, asked of every opcode
 *      at every state rather than of the ones a program happens to reach;
 *   C. programs agree with the interpreter at EVERY cache depth from 0 to
 *      CALC_TIER2_N, so a tiling bug cannot hide behind one lucky depth;
 *   D. the tiling did what it claims — counted, because the answer cannot say;
 *   E. the transition stencils move a value AND its tag, in the right order;
 *   F. the walk refuses what it cannot stamp instead of stamping something else.
 *
 * It builds its own host seam (calc_host.h) rather than linking calc_vm_host.o,
 * because the questions above are about the driver's internals — the cache, the
 * state walk, the statistics — and those are file-scope to whatever translation
 * unit includes calc_jit_driver.h.
 */
#include "calc_host.h"          /* the opgen seam: calc_next, calc_trap, calc_reset, tier 0 */
#include "calc_jit_driver.h"    /* tier 1 + the tier-2 tiling, internals and all */
#include "calc_natives.h"       /* calc_br / calc_call / calc_ret, shared by both tiers */
#include "opcodes.h"            /* OP_* */
#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...) do {                                                 \
    checks++;                                                                 \
    if (!(cond)) { fails++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void ok(const char* what) { printf("  ok   %s\n", what); }

/* ════════════════════════════════════════════════════════════════════════════
 * A. The published tables
 *
 * Every one of these is a law the GENERATOR owes its consumers, asked of the
 * whole vocabulary rather than of the handful of opcodes a calc program uses.
 * A driver reads these tables and stamps what they name; a row that names a
 * stencil the emission never produced, or a variant with no exit state, is a
 * mis-stamp waiting for the first program that reaches it.
 * ══════════════════════════════════════════════════════════════════════════ */
static void table_laws(void) {
    int present = 0, families = 0, mem_forms = 0;

    for (int op = 0; op < 256; op++) {
        if (!calc_opcode_sig[op].present) continue;
        present++;
        int base = calc_jit_meta[op].stencil;
        CHECK(base >= 0 && base < STENCIL_COUNT,
              "A-1 op %02X: meta names stencil %d, outside the table", op, base);

        /* State 0 is the one every family must carry: it is what a walk descends
         * to when nothing else fits, and the descent has to terminate. */
        CHECK(calc_variant[base][0] >= 0 && calc_variant[base][0] < STENCIL_COUNT,
              "A-2 op %02X: no state-0 variant", op);
        CHECK(calc_variant_fs[base][0] >= 0,
              "A-3 op %02X: the state-0 variant reports no exit state", op);

        if (calc_variant[base][0] != base) families++;

        for (int st = 0; st <= CALC_TIER2_N; st++) {
            int v = calc_variant[base][st], fs = calc_variant_fs[base][st];
            int m = calc_variant_m[base][st];
            if (v >= 0) {
                CHECK(v < STENCIL_COUNT,
                      "A-4 op %02X state %d: variant %d outside the stencil table", op, st, v);
                /* The exit state is what the walk carries to the next
                 * instruction. Without it the walk has no state at all. */
                CHECK(fs >= 0 && fs <= CALC_TIER2_N,
                      "A-5 op %02X state %d: exit state %d out of range", op, st, fs);
            } else {
                CHECK(fs < 0, "A-6 op %02X state %d: no variant, yet an exit state %d",
                      op, st, fs);
                CHECK(m < 0, "A-7 op %02X state %d: no variant, yet a memory form %d",
                      op, st, m);
            }
            if (m >= 0) {
                mem_forms++;
                CHECK(m < STENCIL_COUNT,
                      "A-8 op %02X state %d: memory form %d outside the table", op, st, m);
                /* A memory form is the SAME cached operands with the result
                 * pushed inline, so it only exists where the cached form does. */
                CHECK(v >= 0, "A-9 op %02X state %d: a memory form with no cached form",
                      op, st);
                CHECK(st > 0, "A-10 op %02X: a memory form at state 0, where the plain "
                      "stencil already is one", op);
            }
        }
    }
    CHECK(present > 0, "A-0: the signature table declares no opcodes at all");
#if CALC_TIER2_N > 0
    /* Without this the whole file could pass against a build where `-tier2` did
     * nothing: every row would name the plain stencil and every law above would
     * hold vacuously. */
    CHECK(families > 0, "A-11: no opcode has a variant distinct from its plain stencil "
          "— the family was never generated");
    CHECK(mem_forms > 0, "A-12: no opcode has a memory-result form");
#else
    CHECK(families == 0, "A-11: -tier2 0 emitted variants distinct from the plain stencils");
#endif

    /* The transitions. `calc_spill[c][s]` and `calc_fill[c][s]` are inverses, so
     * one without the other is a state the walk can enter and not leave. */
    for (int c = 0; c <= JSC_REF; c++) {
        for (int s = 0; s < (CALC_TIER2_N ? CALC_TIER2_N : 1); s++) {
            int sp_ = calc_spill[c][s], fl = calc_fill[c][s];
            CHECK((sp_ < 0) == (fl < 0),
                  "A-13 class %d slot %d: spill %d and fill %d disagree about existing",
                  c, s, sp_, fl);
            if (sp_ < 0) continue;
            CHECK(sp_ < STENCIL_COUNT && fl < STENCIL_COUNT,
                  "A-14 class %d slot %d: transition outside the stencil table", c, s);
            CHECK(calc_class_cacheable[c],
                  "A-15 class %d: a transition for a class that cannot be cached", c);
            CHECK(s + calc_class_width[c] <= CALC_TIER2_N,
                  "A-16 class %d slot %d: a %d-slot value does not fit from here",
                  c, s, calc_class_width[c]);
        }
    }

    /* …and the other direction: a class this vocabulary can actually PRODUCE in
     * a register must have somewhere to put it back. The calc declares no f32,
     * so f32 is legitimately absent — which is why this asks which classes the
     * signatures name rather than iterating all six. */
    for (int op = 0; op < 256; op++) {
        if (!calc_opcode_sig[op].present) continue;
        const calc_sig_t* sg = &calc_sigtab[calc_opcode_sig[op].sig];
        if (sg->nresults != 1) continue;
        int c = sg->results[0];
        if (c > JSC_REF || !calc_class_cacheable[c]) continue;
        if (calc_class_width[c] > CALC_TIER2_N) continue;
        CHECK(CALC_TIER2_N == 0 || calc_spill[c][0] >= 0,
              "A-17 op %02X: produces class %d, which has no spill at slot 0", op, c);
    }
    ok("A: the variant, memory-form and transition tables are self-consistent");
}

/* ════════════════════════════════════════════════════════════════════════════
 * B. The driver's state contract
 *
 * calc_state_ok is the whole of the driver's judgement, and a program only ever
 * asks it about the states it happens to reach. Ask it about every opcode at
 * every state, against a cache stuffed with each class in turn.
 * ══════════════════════════════════════════════════════════════════════════ */
static void state_laws(void) {
    calc_cache_t c;

    for (int op = 0; op < 256; op++) {
        if (!calc_opcode_sig[op].present) continue;
        const calc_sig_t* sg = &calc_sigtab[calc_opcode_sig[op].sig];
        int base = calc_jit_meta[op].stencil;

        for (int cls = 0; cls <= JSC_REF; cls++) {
            if (!calc_class_cacheable[cls] || calc_class_width[cls] != 1) continue;
            for (int st = 0; st <= CALC_TIER2_N; st++) {
                c.n = st;
                for (int i = 0; i < st; i++) c.cls[i] = (uint8_t)cls;

                int yes = calc_state_ok(op, base, st, &c, 0);
                if (yes) {
                    /* Whatever it says yes to, it is about to stamp. */
                    CHECK(calc_variant[base][st] >= 0,
                          "B-1 op %02X state %d cls %d: approved a state with no variant",
                          op, st, cls);
                    CHECK(st <= calc_jit_cap,
                          "B-2 op %02X state %d: approved a state deeper than the cap", op, st);

                    /* The law the javelina episode wrote down: a value only rides a
                     * register when the SIGNATURE names its class outright. `addr`,
                     * `word`, `any` and the variadic group are resolved by a burg
                     * cover, and this walk is not one — it has no tile to ask, so a
                     * class it cannot name is a spill it cannot re-tag. */
                    int pops = calc_jit_meta[op].pop_slots;
                    int left = st > pops ? st - pops : 0;
                    if (calc_variant_fs[base][st] > left) {
                        CHECK(sg->nresults == 1,
                              "B-3 op %02X state %d: caches a result from a %d-result "
                              "signature", op, st, sg->nresults);
                        CHECK(sg->results[0] <= JSC_REF && calc_class_cacheable[sg->results[0]],
                              "B-4 op %02X state %d: caches a result of class %d, which "
                              "this walk cannot name", op, st, sg->results[0]);
                    }
                    if (st > 0)
                        for (int k = 0; k < sg->nparams; k++)
                            CHECK(sg->params[k] < CALC_SCLASS_FINAL && sg->params[k] != JSC_STK,
                                  "B-5 op %02X state %d: reads operand %d of class %d from "
                                  "a register", op, st, k, sg->params[k]);
                }
                /* A control transfer must LEAVE at state 0, or it lands somewhere
                 * that agreed to arrive at one. */
                if (calc_state_ok(op, base, st, &c, 1))
                    CHECK(calc_variant_fs[base][st] == 0,
                          "B-6 op %02X state %d: approved as a control transfer, yet it "
                          "exits at state %d", op, st, calc_variant_fs[base][st]);
            }
        }
        /* State 0 is the floor the descent stops at, so it must always be
         * reachable — with the cap at 0 or anywhere else. */
        c.n = 0;
        CHECK(calc_state_ok(op, base, 0, &c, 0) || calc_variant[base][0] >= 0,
              "B-7 op %02X: neither a state-0 variant nor a plain fallback", op);
    }
    ok("B: calc_state_ok never approves a state it cannot stamp or a class it cannot name");
}

/* ════════════════════════════════════════════════════════════════════════════
 * C + D. Execution at every cache depth, and what the tiling actually did
 * ══════════════════════════════════════════════════════════════════════════ */

/* What a program must do. `trap` and `value` are separate because a trapped
 * program delivers no result — comparing the slot it left behind is comparing
 * whatever the last run put there. */
typedef struct {
    const char* name;
    const u1*   code;
    size_t      len;
    int         trap;
    s4          value;
    const opgen_st_entry_t* sidetable;
} prog_t;

/* Run one program on the interpreter, then on the JIT at EVERY cache depth from
 * 0 (tier-1-equivalent) to CALC_TIER2_N. One depth agreeing proves one depth. */
static void run_every_cap(const prog_t* p) {
    static vm_t vi, vj;
    unsigned stamped0 = 0;
    calc_g_sidetable = p->sidetable;
    s4 want = calc_interp_run(&vi, p->code, p->len);
    int want_trap = vi.trapped;

    CHECK(want_trap == p->trap, "C-0 %s: the interpreter %s, expected %s", p->name,
          want_trap ? "trapped" : "did not trap", p->trap ? "a trap" : "no trap");
    if (!p->trap)
        CHECK(want == p->value, "C-0 %s: the interpreter gave %d, expected %d",
              p->name, want, p->value);

    for (int cap = 0; cap <= CALC_TIER2_N; cap++) {
        calc_jit_set_cap(cap);
        calc_g_sidetable = p->sidetable;
        s4 got = calc_jit_run(&vj, p->code, p->len);
        CHECK(vj.trapped == want_trap,
              "C-1 %s @cap %d: jit %s where the interpreter %s", p->name, cap,
              vj.trapped ? "trapped" : "did not trap",
              want_trap ? "trapped" : "did not");
        if (!want_trap)
            CHECK(got == want, "C-2 %s @cap %d: jit %d, interp %d", p->name, cap, got, want);

        /* D. The falsifier for the whole file. At cap 0 the walk must behave
         * exactly as tier 1 did: no state above 0, so no cached operand and no
         * transition to reach one. Without this, "agrees at every cap" would hold
         * just as well for a walk that never cached anything at any cap.
         *
         * What it does NOT claim is that every stencil is the PLAIN one: the
         * family's own `__s0` is a legitimate choice at state 0 — it reads its
         * operands off the stack and pushes its result there, which is what the
         * plain stencil does — and the walk takes it wherever the exit state is
         * still 0. The claim is about the STATE, which is what the cache is. */
        if (cap == 0) {
            CHECK(calc_jit_stats.cached == 0,
                  "D-1 %s: %u instruction(s) entered a cached state at cap 0",
                  p->name, calc_jit_stats.cached);
            CHECK(calc_jit_stats.spills == 0,
                  "D-2 %s: %u spill(s) at cap 0", p->name, calc_jit_stats.spills);
            CHECK(calc_jit_stats.max_state == 0,
                  "D-3 %s: reached state %u at cap 0", p->name, calc_jit_stats.max_state);
            stamped0 = calc_jit_stats.stamped;
        }
        /* The cap changes how instructions are stamped, never how many there are.
         * A walk that lost or duplicated one would still deliver the right answer
         * whenever the lost one was dead code. */
        CHECK(calc_jit_stats.stamped == stamped0,
              "D-4 %s @cap %d: stamped %u instruction stencils, %u at cap 0",
              p->name, cap, calc_jit_stats.stamped, stamped0);
        CHECK(calc_jit_stats.max_state <= (unsigned)cap,
              "D-5 %s @cap %d: reached state %u", p->name, cap, calc_jit_stats.max_state);
        if (cap == CALC_TIER2_N)
            printf("  ok   %-52s caps 0..%d  [state<=%u, %u cached, %u spilled, %u plain]\n",
                   p->name, CALC_TIER2_N, calc_jit_stats.max_state,
                   calc_jit_stats.cached, calc_jit_stats.spills, calc_jit_stats.plain);
    }
    calc_jit_set_cap(CALC_TIER2_N);
    calc_g_sidetable = NULL;
}

/* ── sleb128, for the operands a byte literal cannot spell ── */
typedef struct { u1 b[256]; size_t n; } buf_t;
static void b_op(buf_t* b, u1 v)  { b->b[b->n++] = v; }
static void b_f64(buf_t* b, double v) { memcpy(&b->b[b->n], &v, 8); b->n += 8; }
static void b_sleb(buf_t* b, int64_t v) {
    for (;;) {
        u1 byte = (u1)(v & 0x7f);
        v >>= 7;
        int done = (v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40));
        b->b[b->n++] = done ? byte : (u1)(byte | 0x80);
        if (done) return;
    }
}
static void b_const(buf_t* b, int32_t v)  { b_op(b, OP_CONST);  b_sleb(b, v); }
static void b_lconst(buf_t* b, int64_t v) { b_op(b, OP_LCONST); b_sleb(b, v); }
static void b_dconst(buf_t* b, double v)  { b_op(b, OP_DCONST); b_f64(b, v); }

/* ════════════════════════════════════════════════════════════════════════════
 * E. The transition stencils, driven directly
 *
 * The walk here DESCENDS only: it spills to reach a state a family carries, and
 * never fills, because state 0 is the plain stencil and always exists. A filling
 * tiler is a cover-driven one — javelina's, where a rule asks for an operand in
 * a register before it is there. So `calc_fill` ships untested by any program
 * this driver compiles, and the pair is exercised here by hand instead: a chain
 * of stencils stamped straight into a buffer, with a fill and its spill in the
 * middle of it. A value that survives the round trip with its TAG intact is the
 * whole claim, and the tag is what a wrong class would quietly change.
 * ══════════════════════════════════════════════════════════════════════════ */
/* One stencil in a hand-built chain. `op` is the opcode it belongs to, or -1 for
 * a transition, which belongs to no opcode: it names the meta row whose JOP_CONST
 * operands have to be baked. Those are the float bounds a guard compares against,
 * and left at 0 a guard compares against 0.0 and fires on everything. */
typedef struct { int sid; int op; uint64_t imm; const char* imm_hole; } step_t;

static calc_jit_func_t* stamp_chain(const step_t* steps, int nsteps, size_t code_len) {
    calc_jit_func_t* fn = (calc_jit_func_t*)malloc(sizeof *fn);
    jit_codebuf_t buf; jcb_init(&buf, 4096);
    size_t* offs = (size_t*)malloc((size_t)(nsteps + 2) * sizeof *offs);
    calc_jit_addr_t* offmap = (calc_jit_addr_t*)calloc(code_len + 1, sizeof *offmap);
    calc_data_hole_t* recs = (calc_data_hole_t*)malloc((size_t)(nsteps + 8) * 8 * sizeof *recs);
    int nrec = 0, rh;

    size_t entry_off = calc_emit_stencil(&buf, &stencil_table[STENCIL_ENTRY], NULL, recs, &nrec);
    const StencilDef* rd = &stencil_table[STENCIL_RESYNC];
    uint64_t rvals[16] = {0};
    if ((rh = calc_find_hole(rd, "_HOLE_offmap"))  >= 0) rvals[rh] = (uint64_t)(uintptr_t)offmap;
    if ((rh = calc_find_hole(rd, "_HOLE_codelen")) >= 0) rvals[rh] = code_len;
    size_t resync_off = calc_emit_stencil(&buf, rd, rvals, recs, &nrec);
    size_t trap_off   = calc_emit_stencil(&buf, &stencil_table[STENCIL_TRAP], NULL, recs, &nrec);

    for (int i = 0; i < nsteps; i++) {
        const StencilDef* d = &stencil_table[steps[i].sid];
        uint64_t vals[16] = {0};
        calc_fill_native_holes(d, vals);
        if (steps[i].op >= 0) {
            calc_jit_meta_t m = calc_jit_meta[steps[i].op];
            for (int k = 0; k < m.operand_count; k++) {
                if (m.operands[k].kind != JOP_CONST) continue;
                int h = calc_find_hole(d, m.operands[k].hole);
                if (h >= 0) vals[h] = m.operands[k].value;
            }
        }
        if (steps[i].imm_hole) {
            int h = calc_find_hole(d, steps[i].imm_hole);
            if (h >= 0) vals[h] = steps[i].imm;
        }
        int hip = calc_find_hole(d, "_HOLE_ip");
        if (hip >= 0) vals[hip] = code_len;      /* everything resyncs to the halt */
        offs[i] = calc_emit_stencil(&buf, d, vals, recs, &nrec);
    }
    offs[nsteps] = calc_emit_stencil(&buf, &stencil_table[STENCIL_GEN_ST_HALT], NULL, recs, &nrec);

    for (int i = 0; i < nrec; i++) {
        size_t foff = (size_t)-1;
        for (int j = 0; j < i; j++) if (recs[j].value == recs[i].value) { foff = recs[j].foff; break; }
        if (foff == (size_t)-1) { foff = buf.size; jcb_emit(&buf, (const uint8_t*)&recs[i].value, 8); }
        recs[i].foff = foff;
    }
    for (int i = 0; i < nrec; i++)
        jcb_patch32(&buf, recs[i].patch_addr,
                    (int32_t)((long)recs[i].foff - (long)(recs[i].patch_addr + 4)));

    uint8_t* base = buf.base;
    const StencilDef* ed = &stencil_table[STENCIL_ENTRY];
    calc_backpatch(&buf, entry_off, ed, calc_find_hole(ed, "_HOLE_cont"),
                   (uint64_t)(base + offs[0]));
    for (int i = 0; i <= nsteps; i++) {
        const StencilDef* d = &stencil_table[i == nsteps ? STENCIL_GEN_ST_HALT : steps[i].sid];
        int h;
        if (i < nsteps && (h = calc_find_hole(d, "_HOLE_cont")) >= 0)
            calc_backpatch(&buf, offs[i], d, h, (uint64_t)(base + offs[i + 1]));
        if ((h = calc_find_hole(d, "_HOLE_resync")) >= 0)
            calc_backpatch(&buf, offs[i], d, h, (uint64_t)(base + resync_off));
        if ((h = calc_find_hole(d, "_HOLE_trap")) >= 0)
            calc_backpatch(&buf, offs[i], d, h, (uint64_t)(base + trap_off));
    }

    void* exec = jcb_finalize(&buf);
    if (exec) offmap[code_len] = (calc_jit_addr_t)((uint8_t*)exec + offs[nsteps]);
    free(offs); free(recs);
    if (!exec) { free(offmap); jcb_free(&buf); free(fn); return NULL; }
    fn->buf = buf; fn->entry_off = entry_off; fn->offmap = offmap;
    return fn;
}

static void run_chain(const char* what, const step_t* steps, int nsteps, s4 want) {
#if CALC_TIER2_N > 0
    static vm_t vm;
    static const u1 nothing[1] = { 0 };
    calc_reset(&vm, nothing, 0);
    calc_jit_func_t* fn = stamp_chain(steps, nsteps, 0);
    if (!fn) { fails++; checks++; printf("  FAIL E %s: the chain would not stamp\n", what); return; }
    calc_jit_enter(fn, &vm);
    calc_jit_free(fn);
    CHECK(vm.result.i == want, "E %s: got %d, want %d", what, vm.result.i, want);
#else
    (void)steps; (void)nsteps; (void)want; (void)what;
#endif
}

static void transition_laws(void) {
#if CALC_TIER2_N > 0
    /* i32 through slot 0 and back. `const` PLAIN pushes to memory; the fill takes
     * it into CACHE_R0; the spill puts it back; `ret` PLAIN pops it. */
    {
        const step_t s[] = {
            { calc_jit_meta[OP_CONST].stencil, OP_CONST, 42, "_HOLE_v" },
            { calc_fill[JSC_I32][0],  -1, 0, NULL },
            { calc_spill[JSC_I32][0], -1, 0, NULL },
            { calc_jit_meta[OP_RET].stencil, OP_RET, 0, NULL },
        };
        run_chain("i32 survives fill+spill through slot 0", s, 4, 42);
    }
    /* The same at the DEEPEST slot the cache has. Slot 0 working proves slot 0. */
    if (CALC_TIER2_N >= 2) {
        int last = CALC_TIER2_N - 1;
        const step_t s[] = {
            { calc_jit_meta[OP_CONST].stencil, OP_CONST, 7, "_HOLE_v" },
            { calc_fill[JSC_I32][last],  -1, 0, NULL },
            { calc_spill[JSC_I32][last], -1, 0, NULL },
            { calc_jit_meta[OP_RET].stencil, OP_RET, 0, NULL },
        };
        run_chain("i32 survives fill+spill through the deepest slot", s, 4, 7);
    }
    /* i64: the TAG is the half a value check would miss. `lconst` pushes T_LONG;
     * the round trip must not come back tagged as an int, which is exactly what a
     * wrong class in the spill would do. box/unbox/tagof reads it back. */
    {
        const step_t s[] = {
            { calc_jit_meta[OP_LCONST].stencil, OP_LCONST, 5, "_HOLE_v" },
            { calc_fill[JSC_I64][0],  -1, 0, NULL },
            { calc_spill[JSC_I64][0], -1, 0, NULL },
            { calc_jit_meta[OP_BOX].stencil,   OP_BOX,   0, NULL },
            { calc_jit_meta[OP_UNBOX].stencil, OP_UNBOX, 0, NULL },
            { calc_jit_meta[OP_TAGOF].stencil, OP_TAGOF, 0, NULL },
            { calc_jit_meta[OP_RET].stencil,   OP_RET,   0, NULL },
        };
        run_chain("i64 keeps its T_LONG tag across fill+spill", s, 7, T_LONG);
    }
    /* f64: a double reinterpreted through a pointer-width slot, which a spill for
     * any other class would mangle. Its VALUE comes back through `trunc` and its
     * TAG through box/unbox/tagof — a slot that round-tripped as an integer would
     * still truncate to -8 and report T_LONG. */
    {
        uint64_t bits; double d = -8.0; memcpy(&bits, &d, 8);
        const step_t s[] = {
            { calc_jit_meta[OP_DCONST].stencil, OP_DCONST, bits, "_HOLE_v" },
            { calc_fill[JSC_F64][0],  -1, 0, NULL },
            { calc_spill[JSC_F64][0], -1, 0, NULL },
            { calc_jit_meta[OP_TRUNC].stencil, OP_TRUNC, 0, NULL },
            { calc_jit_meta[OP_RET].stencil,   OP_RET,   0, NULL },
        };
        run_chain("f64 survives fill+spill as a double, not as an integer", s, 5, -8);
    }
    {
        uint64_t bits; double d = -8.0; memcpy(&bits, &d, 8);
        const step_t s[] = {
            { calc_jit_meta[OP_DCONST].stencil, OP_DCONST, bits, "_HOLE_v" },
            { calc_fill[JSC_F64][0],  -1, 0, NULL },
            { calc_spill[JSC_F64][0], -1, 0, NULL },
            { calc_jit_meta[OP_BOX].stencil,   OP_BOX,   0, NULL },
            { calc_jit_meta[OP_UNBOX].stencil, OP_UNBOX, 0, NULL },
            { calc_jit_meta[OP_TAGOF].stencil, OP_TAGOF, 0, NULL },
            { calc_jit_meta[OP_RET].stencil,   OP_RET,   0, NULL },
        };
        run_chain("f64 comes back tagged T_DOUBLE, not T_LONG", s, 7, T_DOUBLE);
    }
    /* v128 is the one class that spends TWO slots, so its transition moves a pair
     * and the slot it names is where the pair STARTS. Nothing else in this file
     * reaches it: the walk only caches a v128 where `vmake` leaves one. */
    if (CALC_TIER2_N >= 2) {
        const step_t s[] = {
            { calc_jit_meta[OP_LCONST].stencil, OP_LCONST, 0x1111, "_HOLE_v" },
            { calc_jit_meta[OP_LCONST].stencil, OP_LCONST, 0x2222, "_HOLE_v" },
            { calc_jit_meta[OP_VMAKE].stencil,  OP_VMAKE,  0, NULL },
            { calc_fill[JSC_V128][0],  -1, 0, NULL },
            { calc_spill[JSC_V128][0], -1, 0, NULL },
            { calc_jit_meta[OP_BOX].stencil,   OP_BOX,   0, NULL },
            { calc_jit_meta[OP_UNBOX].stencil, OP_UNBOX, 0, NULL },
            { calc_jit_meta[OP_VLANE].stencil, OP_VLANE, 1, "_HOLE_i" },
            { calc_jit_meta[OP_L2I].stencil,   OP_L2I,   0, NULL },
            { calc_jit_meta[OP_RET].stencil,   OP_RET,   0, NULL },
        };
        run_chain("v128 keeps BOTH halves across a two-slot fill+spill", s, 10, 0x2222);
    }
    ok("E: the transition stencils move a value and its tag, at slot 0 and at the deepest slot");
#else
    ok("E: skipped — this build has no cache to transition through");
#endif
}

/* ════════════════════════════════════════════════════════════════════════════
 * F. What the walk refuses
 * ══════════════════════════════════════════════════════════════════════════ */
static int compiles(const u1* code, size_t len) {
    bbq_ctx_t c; bbq_ctx_init(&c, code, len);
    calc_jit_func_t* fn = calc_jit_compile(c);
    if (fn) { calc_jit_free(fn); return 1; }
    return 0;
}

static void refusal_laws(void) {
    /* An opcode calc.def never declared. The meta row for it is zero-filled, so
     * the stencil it names is 0 — a real stencil belonging to some other opcode —
     * and its operand length is unknown, so the walk cannot even find the next
     * instruction. Stamping anyway is the one outcome that must not happen. */
    {
        static const u1 c[] = { OP_CONST, 0x05, 0xBE, OP_RET };
        CHECK(!compiles(c, sizeof c), "F-1: an undeclared opcode compiled instead of refusing");
    }
    /* …and the same byte at the very start, before anything is cached. */
    {
        static const u1 c[] = { 0xBE };
        CHECK(!compiles(c, sizeof c), "F-2: a lone undeclared opcode compiled");
    }
    /* A declared opcode whose operand is cut short. bbq_read_* fails, the decode
     * leaves the cursor at the end, and the walk must terminate rather than run
     * off the buffer — the halt stencil is the only thing that can follow. */
    {
        static const u1 c[] = { OP_CONST };
        CHECK(compiles(c, sizeof c), "F-3: a truncated operand should still stamp a halt");
    }
    {
        static const u1 c[] = { OP_DCONST, 0x01, 0x02 };   /* 2 bytes of an 8-byte double */
        CHECK(compiles(c, sizeof c), "F-4: a truncated f64 operand did not terminate");
    }
    /* Empty bytecode: nothing to stamp but the halt. */
    {
        static const u1 c[1] = { 0 };
        CHECK(compiles(c, 0), "F-5: empty bytecode did not compile to a bare halt");
    }
    /* The cap is Ertl's depth knob, not an index — out of range clamps rather
     * than indexing a variant row that is not there. */
    calc_jit_set_cap(-3);
    CHECK(calc_jit_cap == 0, "F-6: a negative cap became %d", calc_jit_cap);
    calc_jit_set_cap(CALC_TIER2_N + 99);
    CHECK(calc_jit_cap == CALC_TIER2_N, "F-7: an oversized cap became %d", calc_jit_cap);
    calc_jit_set_cap(CALC_TIER2_N);
    ok("F: the walk refuses undeclared opcodes and terminates on truncated ones");
}

/* ════════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("calc tier-2 (stack cache depth %d)\n", CALC_TIER2_N);

    table_laws();
    state_laws();

    /* ── C + D: programs, at every depth ────────────────────────────────── */

    /* A chain deep enough to fill the cache and then some: each `const` adds a
     * slot, each `add` gives one back. Without it nothing here would ever reach
     * the deepest state. */
    buf_t deep = {{0}, 0};
    for (int i = 1; i <= CALC_TIER2_N + 3; i++) b_const(&deep, i);
    for (int i = 0; i < CALC_TIER2_N + 2; i++) b_op(&deep, OP_ADD);
    b_op(&deep, OP_RET);
    {
        s4 want = 0;
        for (int i = 1; i <= CALC_TIER2_N + 3; i++) want += i;
        prog_t p = { "deep arithmetic chain", deep.b, deep.n, 0, want, NULL };
        run_every_cap(&p);
#if CALC_TIER2_N > 0
        calc_jit_set_cap(CALC_TIER2_N);
        static vm_t scratch;
        calc_jit_run(&scratch, deep.b, deep.n);
        CHECK(calc_jit_stats.max_state == (unsigned)CALC_TIER2_N,
              "D-6: a chain of %d live values only reached state %u of %d",
              CALC_TIER2_N + 3, calc_jit_stats.max_state, CALC_TIER2_N);
        CHECK(calc_jit_stats.cached > 0, "D-7: nothing ran with a cached operand");
#endif
    }

    /* SPILL ORDER. The flush at a branch target has to put the cached values back
     * in the order they came in — deepest first. Addition would hide a reversal;
     * subtraction cannot. 20 and 4 are cached, the `goto` forces the flush, and
     * the `sub` that follows reads them off the stack: 16 if the order held, -16
     * if the spills went top-first. */
    {
        static const u1 c[] = {
            OP_CONST, 20, OP_CONST, 4,      /* 0: two values into the cache      */
            OP_GOTO, 6,                     /* 4: forces a flush; 6 is a target  */
            OP_SUB, OP_RET                  /* 6: 20 - 4, off the memory stack   */
        };
        prog_t p = { "spill order (non-commutative)", c, sizeof c, 0, 16, NULL };
        run_every_cap(&p);
    }

    /* HALT with a live cache. `gen_st_halt` keeps nothing — the results ARE the
     * top of the frame stack — so a value still in a register when the code runs
     * out is a value the caller never sees. No trailing `ret`, so the answer is
     * read off the stack rather than from vm->result. */
    {
        static const u1 c[] = { OP_CONST, 3, OP_CONST, 4, OP_ADD };
        static vm_t vi, vj;
        calc_interp_run(&vi, c, sizeof c);
        for (int cap = 0; cap <= CALC_TIER2_N; cap++) {
            calc_jit_set_cap(cap);
            calc_jit_run(&vj, c, sizeof c);
            CHECK(vj.frame.sp == vi.frame.sp && vj.frame.stack[0].i == vi.frame.stack[0].i,
                  "C-3 run off the end @cap %d: jit sp=%u top=%d, interp sp=%u top=%d",
                  cap, vj.frame.sp, vj.frame.stack[0].i, vi.frame.sp, vi.frame.stack[0].i);
        }
        calc_jit_set_cap(CALC_TIER2_N);
        ok("C: a value still in a register at the end reaches the frame stack");
    }

    /* A MERGE the two paths reach in different states. This is the case the
     * target flush exists for, and the only shape that witnesses it.
     *
     * A well-formed program has the same abstract stack DEPTH on both paths into
     * a merge, but not the same cache state: the branch path came through a
     * control transfer, which is tiled to leave at state 0 with its values
     * spilled to memory, while the fall-through arrives with the top of the stack
     * still in a register. Stamp the merge for the fall-through's state and the
     * branch arrival runs a stencil reading a register nothing wrote.
     *
     *   0: const 7 · const 9 · const 1        7 and 9 survive; 1 is the condition
     *   6: br_if 11                           consumes the 1, spills 7 and 9, jumps
     *   8: pop · const 9                      the not-taken path, rebuilding [7, 9]
     *  11: sub                                the merge: 7 - 9 on either path
     *
     * Both paths compute -2. Stamped at the fall-through's state 1, `sub` reads
     * its right operand from CACHE_R0 — which on the taken path still holds the
     * condition the br_if consumed, giving 9 - 1. A wrong answer, not a crash,
     * which is why it needs asking for by name. */
    {
        static const u1 c[] = {
            OP_CONST, 7, OP_CONST, 9, OP_CONST, 1,
            OP_BR_IF, 11,
            OP_POP, OP_CONST, 9,
            OP_SUB, OP_RET
        };
        prog_t p = { "merge reached in two different states", c, sizeof c, 0, -2, NULL };
        run_every_cap(&p);
    }

    /* Control flow: a backward edge, a forward one, and a call/return — each an
     * arrival the tiling has to enter at state 0, and each with values cached
     * when it gets there. */
    {
        /* let a = 0; while (a < 5) a = a + 1; a   — the compiler's own shape */
        static const u1 c[] = {
            OP_CONST, 0, OP_STORE, 0,
            OP_LOAD, 0, OP_CONST, 5, OP_LT,
            OP_BR_IF, 14,
            OP_LOAD, 0, OP_RET,
            OP_LOAD, 0, OP_CONST, 1, OP_ADD, OP_STORE, 0,
            OP_GOTO, 4
        };
        prog_t p = { "while loop (backward edge)", c, sizeof c, 0, 5, NULL };
        run_every_cap(&p);
    }
    {
        /* fn dbl(x) = x + x; dbl(21) — a call is a control transfer whose return
         * site is the byte after it, and both must be at state 0. */
        static const u1 c[] = {
            0x01,0x15, 0x0D,0x00, 0x0C,0x00,
            0x13,0x8E,0x80,0x80,0x80,0x00, 0x01,
            0x10,
            0x0C,0x00, 0x0D,0x01, 0x0C,0x00, 0x0D,0x02, 0x0C,0x01, 0x0C,0x02, 0x02, 0x10
        };
        prog_t p = { "call dbl(21)", c, sizeof c, 0, 42, NULL };
        run_every_cap(&p);
    }
    {
        /* fac(5): recursion, so a return site is reached from more than one depth. */
        static const u1 c[] = {
            0x01,0x05,0x0D,0x00,0x0C,0x00,0x13,0x8E,0x80,0x80,0x80,0x00,0x01,0x10,
            0x0C,0x00,0x0D,0x06,0x01,0x02,0x0D,0x07,0x0C,0x06,0x0C,0x07,0x08,
            0x0E,0xC5,0x80,0x80,0x80,0x00,
            0x0C,0x00,0x0D,0x01,0x0C,0x00,0x0D,0x04,0x01,0x01,0x0D,0x05,
            0x0C,0x04,0x0C,0x05,0x03,0x0D,0x03,0x0C,0x03,
            0x13,0x8E,0x80,0x80,0x80,0x00,0x01,0x0D,0x02,0x0C,0x01,0x0C,0x02,0x04,0x10,
            0x01,0x01,0x10
        };
        prog_t p = { "recursion fac(5)", c, sizeof c, 0, 120, NULL };
        run_every_cap(&p);
    }

    /* A GUARD firing with the cache live. `div` carries calc.def's two declared
     * error: guards; the trap path abandons whatever is in the registers, so the
     * only thing that must agree is that both tiers trapped. */
    {
        static const u1 c[] = { OP_CONST, 7, OP_CONST, 9, OP_CONST, 42, OP_CONST, 0,
                                OP_DIV, OP_ADD, OP_ADD, OP_RET };
        prog_t p = { "div by zero under a full cache", c, sizeof c, 1, 0, NULL };
        run_every_cap(&p);
    }
    {
        /* …and the guard is not simply always firing. */
        static const u1 c[] = { OP_CONST, 7, OP_CONST, 9, OP_CONST, 42, OP_CONST, 2,
                                OP_DIV, OP_ADD, OP_ADD, OP_RET };
        prog_t p = { "div under a full cache, guard quiet", c, sizeof c, 0, 37, NULL };
        run_every_cap(&p);
    }

    /* br_table: its targets live in a side table the HOST fills at run time, so
     * nothing in the walk can read them and every offset becomes a possible
     * arrival. The whole function drops to state 0 — correctly, and loudly. */
    {
        static const opgen_st_entry_t tbl[] = { {0,0,0,0}, {3,0,0,0}, {6,0,0,0} };
        static const u1 c[] = { OP_CONST, 1, OP_BR_TABLE, 2, 0, 1, 2,
                                OP_CONST, 10, OP_RET,
                                OP_CONST, 20, OP_RET,
                                OP_CONST, 30, OP_RET };
        prog_t p = { "br_table (runtime targets)", c, sizeof c, 0, 20, tbl };
        run_every_cap(&p);
        static vm_t scratch;
        calc_jit_set_cap(CALC_TIER2_N);
        calc_g_sidetable = tbl;
        calc_jit_run(&scratch, c, sizeof c);
        calc_g_sidetable = NULL;
        CHECK(calc_jit_stats.dynamic > 0,
              "D-8: br_table was not recognised as having runtime targets");
        CHECK(calc_jit_stats.cached == 0,
              "D-9: %u instruction(s) ran cached in a function with runtime targets",
              calc_jit_stats.cached);
        ok("D: a side-table branch puts the whole function back at state 0");
    }

    /* ── The ops that must NOT ride the cache ────────────────────────────────
     *
     * Each of these carries something a register does not: a tag the body reads,
     * a runtime-typed carrier, a stack effect the signature cannot express, a
     * variadic group read in place. Each is run with the cache ALREADY FULL, so
     * the walk has to spill down to state 0 to reach it — which is the case a
     * program that happened to start with one would never produce. */
    {
        /* `anyroll` moves a slot with the push/pop intrinsics: "a signature cannot
         * express this op". At any cached state the top of the stack is in a
         * register and sp points below it, so the body would move the wrong slot.
         * Red before opgen refused it a cached variant. */
        buf_t b = {{0}, 0};
        b_const(&b, 1); b_const(&b, 2);
        b_lconst(&b, 5); b_op(&b, OP_ANYROLL); b_op(&b, OP_L2I);
        b_op(&b, OP_ADD); b_op(&b, OP_ADD); b_op(&b, OP_RET);
        prog_t p = { "anyroll under a full cache", b.b, b.n, 0, 8, NULL };
        run_every_cap(&p);
    }
    {
        /* The same, reading the TAG back rather than the value: push(pop()) has to
         * carry the runtime type across, cache or no cache. */
        buf_t b = {{0}, 0};
        b_const(&b, 1); b_const(&b, 2);
        b_lconst(&b, 5); b_op(&b, OP_ANYROLL); b_op(&b, OP_TAGOF);
        b_op(&b, OP_ADD); b_op(&b, OP_ADD); b_op(&b, OP_RET);
        prog_t p = { "anyroll keeps the tag under a full cache", b.b, b.n, 0, T_LONG + 3, NULL };
        run_every_cap(&p);
    }
    {
        /* `depth` READS sp. Under a cache the top values are in registers and sp
         * points beneath them, so the depth a cached state would report is short
         * by exactly the number of slots it is holding — 2 here, not 4. This is
         * the observable half of a stack-open body: `anyroll` above puts its slot
         * back where it found it and is an identity even at the wrong state, so
         * nothing it does can be seen. */
        static const u1 c[] = { OP_CONST, 7, OP_CONST, 9, OP_CONST, 5, OP_CONST, 3,
                                OP_DEPTH,
                                OP_ADD, OP_ADD, OP_ADD, OP_ADD, OP_RET };
        prog_t p = { "depth reads sp, not the cache", c, sizeof c, 0, 7 + 9 + 5 + 3 + 4, NULL };
        run_every_cap(&p);
    }
    {
        /* `unbox` produces an `any` — (bits, hi, kind), three fields and no class
         * this walk can name. A v128 through the carrier loses its high lane the
         * moment it is treated as one register. */
        buf_t b = {{0}, 0};
        b_const(&b, 1); b_const(&b, 2);
        b_lconst(&b, 0x1111); b_lconst(&b, 0x2222);
        b_op(&b, OP_VMAKE); b_op(&b, OP_BOX); b_op(&b, OP_UNBOX);
        b_op(&b, OP_VLANE); b_op(&b, 1);
        b_op(&b, OP_L2I); b_op(&b, OP_ADD); b_op(&b, OP_ADD); b_op(&b, OP_RET);
        prog_t p = { "v128 high lane through any_t, cache full", b.b, b.n, 0, 0x2222 + 3, NULL };
        run_every_cap(&p);
    }
    {
        /* …and the tag it carries. */
        buf_t b = {{0}, 0};
        b_const(&b, 1); b_const(&b, 2);
        b_lconst(&b, 1); b_lconst(&b, 2);
        b_op(&b, OP_VMAKE); b_op(&b, OP_BOX); b_op(&b, OP_UNBOX);
        b_op(&b, OP_TAGOF); b_op(&b, OP_ADD); b_op(&b, OP_ADD); b_op(&b, OP_RET);
        prog_t p = { "any_t keeps the v128 tag, cache full", b.b, b.n, 0, T_V128 + 3, NULL };
        run_every_cap(&p);
    }
    {
        /* `msize` computes its own tag from the addressed memory's addrtype — a
         * runtime fact no storage class recovers. Memory 1 is 64-bit, so the tag
         * must come back T_LONG with three values already cached. */
        static const u1 c[] = { OP_CONST, 1, OP_CONST, 2,
                                OP_MSIZE, 1, OP_BOX, OP_UNBOX, OP_TAGOF,
                                OP_ADD, OP_ADD, OP_RET };
        prog_t p = { "msize's addrtype tag, cache full", c, sizeof c, 0, T_LONG + 3, NULL };
        run_every_cap(&p);
    }
    {
        /* `select`'s arms are `word` slots and its body propagates their tags. */
        static const u1 c[] = { OP_CONST, 1, OP_CONST, 2,
                                OP_CONST, 11, OP_CONST, 22, OP_CONST, 1, OP_SELECT,
                                OP_ADD, OP_ADD, OP_RET };
        prog_t p = { "select under a full cache", c, sizeof c, 0, 14, NULL };
        run_every_cap(&p);
    }
    {
        /* A variadic group is read IN PLACE off the stack, so every one of its
         * operands has to be there — not in a register. */
        static const u1 c[] = { OP_CONST, 9, OP_CONST, 1, OP_CONST, 2, OP_CONST, 3,
                                OP_SUM_N, 3, OP_ADD, OP_RET };
        prog_t p = { "sum_n reads its operands in place", c, sizeof c, 0, 15, NULL };
        run_every_cap(&p);
    }
    {
        /* A variadic RESULT is a count, not a value: `expose` raises sp over
         * values a native left there, which a cached state would sit on top of. */
        static const u1 c[] = { OP_CONST, 5, OP_EXPOSE, 3, OP_POP, OP_POP,
                                OP_ADD, OP_RET };
        prog_t p = { "expose raises sp under a cached value", c, sizeof c, 0, 105, NULL };
        run_every_cap(&p);
    }
    {
        /* An `addr` operand's width is DECLARED, and the declaration is resolved
         * per memory — not a class this walk can name either. */
        static const u1 c[] = { OP_CONST, 1, OP_CONST, 2,
                                OP_CONST, 4, OP_CONST, 42, OP_STORE32, 0,
                                OP_CONST, 4, OP_LOAD32, 0,
                                OP_ADD, OP_ADD, OP_RET };
        prog_t p = { "store32/load32 through a declared addrtype, cache full",
                     c, sizeof c, 0, 45, NULL };
        run_every_cap(&p);
    }
    {
        /* A managed reference never enters a slot at all. */
        static const u1 c[] = { OP_CONST, 1, OP_CONST, 2,
                                OP_LCONST, 5, OP_REFTAG, OP_LCONST, 5, OP_REFTAG, OP_REFEQ,
                                OP_ADD, OP_ADD, OP_RET };
        prog_t p = { "reftag/refeq under a full cache", c, sizeof c, 0, 4, NULL };
        run_every_cap(&p);
    }
#if CALC_TIER2_N > 0
    /* The falsifier for that whole block: every one of those programs must have
     * spilled, or "under a full cache" was never true and they proved nothing. */
    {
        static vm_t scratch;
        static const u1 c[] = { OP_CONST, 9, OP_CONST, 1, OP_CONST, 2, OP_CONST, 3,
                                OP_SUM_N, 3, OP_ADD, OP_RET };
        calc_jit_set_cap(CALC_TIER2_N);
        calc_jit_run(&scratch, c, sizeof c);
        CHECK(calc_jit_stats.spills > 0,
              "D-10: a variadic group that must be read off the stack forced no spill "
              "— the cache was never full when it ran");
    }
    {
        /* …and the other half: an op whose RESULT has no class this walk can name
         * must land on the plain stencil, which is the only form that pushes the
         * value with its tag. `msize` computes its own tag at run time, so a
         * `__s0` that kept the result in a register would keep the bits and lose
         * the tag. The count is what says the walk actually declined. */
        static vm_t scratch;
        static const u1 c[] = { OP_MSIZE, 1, OP_BOX, OP_UNBOX, OP_TAGOF, OP_RET };
        calc_jit_set_cap(CALC_TIER2_N);
        calc_jit_run(&scratch, c, sizeof c);
        CHECK(calc_jit_stats.plain > 0,
              "D-11: nothing was forced onto the plain stencil, though `msize` has no "
              "nameable result class");
    }
#endif
    ok("C: every program agrees with the interpreter at every cache depth");

    /* A double-precision guard whose constants ride the footer pool, with the
     * cache live: the pool is shared across the whole buffer, and a spill in the
     * middle of it must not disturb a rip-relative load stamped either side. */
    {
        buf_t b = {{0}, 0};
        b_const(&b, 1); b_const(&b, 2);
        b_dconst(&b, -8.0); b_dconst(&b, 2.0);
        b_op(&b, OP_DDIV); b_op(&b, OP_TRUNC);
        b_op(&b, OP_ADD); b_op(&b, OP_ADD); b_op(&b, OP_RET);
        prog_t p = { "ddiv float holes with a live cache", b.b, b.n, 0, -1, NULL };
        run_every_cap(&p);
    }
    {
        buf_t b = {{0}, 0};
        b_const(&b, 1); b_dconst(&b, 1.0); b_dconst(&b, 0.0);
        b_op(&b, OP_DDIV); b_op(&b, OP_TRUNC); b_op(&b, OP_ADD); b_op(&b, OP_RET);
        prog_t p = { "ddiv by zero traps with a live cache", b.b, b.n, 1, 0, NULL };
        run_every_cap(&p);
    }

    transition_laws();
    refusal_laws();

    printf("\n%d checks", checks);
    if (!fails) { printf(", all passed\n"); return 0; }
    printf(", %d FAILED\n", fails);
    return 1;
}
