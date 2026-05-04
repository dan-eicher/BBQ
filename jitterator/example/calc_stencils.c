// calc_stencils.c — CEK-shape stencils for the calculator JIT
//
// Runtime context: paper's accumulator (r0) is `ctx->ac` — a distinct
// addressing class from locals (`ctx->slots[]`). Each stencil is one
// kont (one tail call). Producers write ctx.ac; StoreSlot moves it
// to a slot; Branch/Goto/Halt control flow.

#include <stdint.h>

#define STENCIL __attribute__((preserve_none))
#define TAIL    __attribute__((musttail))

typedef struct {
    int64_t ac;
    int64_t frame[1024];      // params + locals + eval-stack temps
    int32_t fp;               // current frame base
    int32_t sp;               // current eval-stack top
} Ctx;
// Return continuations + saved caller fp/sp ride on the cpu's hardware
// call stack: the call stencil uses a regular C call (not musttail)
// for the function transition, so cpu CALL pushes the return PC and
// clang spills saved fp/sp to the local frame across that call. The
// matching leave stencil is a plain `return;` — cpu RET pops the PC.
// Recursion depth is bounded by cpu stack (Linux default ~500K frames),
// not unlimited like the all-musttail design — TCO would lift that
// for tail-recursive call sites via a separate TailCall IR variant.

extern void STENCIL _HOLE_cont(Ctx *ctx);
extern uint64_t _HOLE_dst;
extern uint64_t _HOLE_lhs;
extern uint64_t _HOLE_src;
extern uint64_t _HOLE_value;
extern uint64_t _HOLE_local_offset;
extern uint64_t _HOLE_n_args;
extern uint64_t _HOLE_frame_size;
extern void STENCIL _HOLE_target(Ctx *ctx);

// ── Producers (write ctx.ac) ────────────────────────────────

void STENCIL load_const(Ctx *ctx) {
    ctx->ac = (int64_t)_HOLE_value;
    TAIL return _HOLE_cont(ctx);
}

void STENCIL add(Ctx *ctx) {
    ctx->ac = ctx->frame[ctx->fp + _HOLE_lhs] + ctx->ac;
    TAIL return _HOLE_cont(ctx);
}

void STENCIL sub(Ctx *ctx) {
    // Paper page 14: subl3 ↑A2, ↑A1, ↓A3 — left to frame (A1), right
    // to ac (A2), result = A1 - A2 = frame[fp+lhs] - ctx.ac.
    ctx->ac = ctx->frame[ctx->fp + _HOLE_lhs] - ctx->ac;
    TAIL return _HOLE_cont(ctx);
}

extern uint64_t _HOLE_rhs;
void STENCIL rev_sub(Ctx *ctx) {
    // Paper figure 8 case 3 with Sub: `E - S` where E is complex and S
    // is a local. Caller has gen'd E into ctx.ac; RevSub computes
    // ctx.ac = E - S without spilling E.
    ctx->ac = ctx->ac - ctx->frame[ctx->fp + _HOLE_rhs];
    TAIL return _HOLE_cont(ctx);
}

void STENCIL mul(Ctx *ctx) {
    ctx->ac = ctx->frame[ctx->fp + _HOLE_lhs] * ctx->ac;
    TAIL return _HOLE_cont(ctx);
}

void STENCIL neg(Ctx *ctx) {
    ctx->ac = -ctx->ac;
    TAIL return _HOLE_cont(ctx);
}

void STENCIL cmp_eq(Ctx *ctx) {
    ctx->ac = (ctx->frame[ctx->fp + _HOLE_lhs] == ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_neq(Ctx *ctx) {
    ctx->ac = (ctx->frame[ctx->fp + _HOLE_lhs] != ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_lt(Ctx *ctx) {
    ctx->ac = (ctx->frame[ctx->fp + _HOLE_lhs] <  ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_le(Ctx *ctx) {
    ctx->ac = (ctx->frame[ctx->fp + _HOLE_lhs] <= ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_gt(Ctx *ctx) {
    ctx->ac = (ctx->frame[ctx->fp + _HOLE_lhs] >  ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_ge(Ctx *ctx) {
    ctx->ac = (ctx->frame[ctx->fp + _HOLE_lhs] >= ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}

// ── Frame-relative addressing (paper §3.1 `local_i` Location class) ─

void STENCIL load_local(Ctx *ctx) {
    ctx->ac = ctx->frame[ctx->fp + (int32_t)_HOLE_local_offset];
    TAIL return _HOLE_cont(ctx);
}

void STENCIL store_local(Ctx *ctx) {
    ctx->frame[ctx->fp + (int32_t)_HOLE_local_offset] = ctx->ac;
    TAIL return _HOLE_cont(ctx);
}

// ── Stack class (paper §3.1 `stack`) ────────────────────────

void STENCIL push_stack(Ctx *ctx) {
    ctx->frame[ctx->sp++] = ctx->ac;
    TAIL return _HOLE_cont(ctx);
}

// ── Paper figure 8 case 4 — pop-stack binops ─────────────────
// Spilled left operand on the eval-stack, right in ac. These pop
// frame[--sp] and combine with ac, leaving the result in ac.
// Used by the BURG catchall `reg: Op(stack_reg, reg)`.

void STENCIL add_pop(Ctx *ctx) {
    ctx->ac = ctx->frame[--ctx->sp] + ctx->ac;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL sub_pop(Ctx *ctx) {
    ctx->ac = ctx->frame[--ctx->sp] - ctx->ac;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL mul_pop(Ctx *ctx) {
    ctx->ac = ctx->frame[--ctx->sp] * ctx->ac;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_eq_pop(Ctx *ctx) {
    ctx->ac = (ctx->frame[--ctx->sp] == ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_neq_pop(Ctx *ctx) {
    ctx->ac = (ctx->frame[--ctx->sp] != ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_lt_pop(Ctx *ctx) {
    ctx->ac = (ctx->frame[--ctx->sp] <  ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_le_pop(Ctx *ctx) {
    ctx->ac = (ctx->frame[--ctx->sp] <= ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_gt_pop(Ctx *ctx) {
    ctx->ac = (ctx->frame[--ctx->sp] >  ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}
void STENCIL cmp_ge_pop(Ctx *ctx) {
    ctx->ac = (ctx->frame[--ctx->sp] >= ctx->ac) ? 1 : 0;
    TAIL return _HOLE_cont(ctx);
}

// ── Function call / return (paper page 14 CG[call]) ─────────

void STENCIL call(Ctx *ctx) {
    int32_t new_fp = ctx->sp - (int32_t)_HOLE_n_args;
    int32_t saved_fp = ctx->fp;     // clang spills across the call
    int32_t saved_sp = new_fp;      // caller's sp before arg pushes
    ctx->fp = new_fp;
    ctx->sp = new_fp + (int32_t)_HOLE_frame_size;
    _HOLE_target(ctx);              // cpu CALL — pushes return PC
    ctx->fp = saved_fp;
    ctx->sp = saved_sp;
    TAIL return _HOLE_cont(ctx);    // tail to caller's post-call kont
}

void STENCIL leave(Ctx *ctx) {
    return;                          // cpu RET pops PC → jumps back into call stencil
}

// ── Consumers / control ─────────────────────────────────────

extern void STENCIL _HOLE_then_target(Ctx *ctx);
extern void STENCIL _HOLE_else_target(Ctx *ctx);
void STENCIL branch(Ctx *ctx) {
    if (ctx->ac) {
        TAIL return _HOLE_then_target(ctx);
    } else {
        TAIL return _HOLE_else_target(ctx);
    }
}

// jbr: unconditional transfer (paper VAX `jbr`). Allocated by Goto IR
// nodes — only when cg_jump's L != Lnext (the L==Lnext peephole skips
// this entirely and chains directly into the next stencil).
void STENCIL jbr(Ctx *ctx) {
    TAIL return _HOLE_cont(ctx);
}

void STENCIL halt(Ctx *ctx) {
    return;
}

// entry: cross C calling convention into preserve_none. No TAIL.
void entry(Ctx *ctx) {
    return ((void STENCIL (*)(Ctx*))_HOLE_cont)(ctx);
}
