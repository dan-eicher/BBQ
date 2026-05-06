// C-mode runtime for the ddcgc kitchensink e2e test.
//
// The source-AST and target-IR struct definitions come from the
// asdl-generated headers (kitchensink_ast.h, kitchensink_target.h),
// not hand-rolled — they carry the canonical asdl-c layout (tag enum
// + per-variant union arm + arena-allocating factory per ctor).
//
// What lives here:
//   - dest tagged unions (delta_t / gamma_t) and their constructor
//     fns. Tag-enum constants follow the c_backend match convention:
//     <DEST_UPPER>_<VARIANT_UPPER> (e.g. DELTA_EFFECT, GAMMA_JUMP).
//   - rho_t (env_dest payload) and label_t.
//   - SourceLoc — alias of asdl's per-module srcloc so MEMBERS can
//     declare `SourceLoc current_loc;` without committing to a
//     particular module's name.
//
// Aux/pred bodies live in kitchensink_main.c (they need
// `kitchensink_ctx_t` from the generated header).

#ifndef KITCHENSINK_RUNTIME_H
#define KITCHENSINK_RUNTIME_H

#include "kitchensink_ast.h"      // ast_expr_t, ast_op_t, ast_*-factories
#include "kitchensink_target.h"   // target_value_t, target_*-factories

typedef int label_t;

// MEMBERS uses SourceLoc; the asdl header names it ast_srcloc.
// Both modules emit identical srcloc layouts; aliasing the AST one
// keeps consumer code free of module-specific names.
typedef ast_srcloc SourceLoc;

// ─── data_dest delta — paper's δ ───────────────────────────────
//
// Constructors take `ctx` as first arg (ddcgc's calling convention
// for dest variants — see chained-rho-pangolin Phase 1). ctx is
// unused here; recursive variants in real consumers use it for
// arena allocation.
struct kitchensink_ctx;
typedef enum { DELTA_EFFECT, DELTA_AC, DELTA_SPILL } delta_tag_t;
typedef struct { delta_tag_t tag; int slot; } delta_t;
static inline delta_t effect(struct kitchensink_ctx* ctx) {
    (void)ctx; return (delta_t){DELTA_EFFECT, 0};
}
static inline delta_t ac(struct kitchensink_ctx* ctx) {
    (void)ctx; return (delta_t){DELTA_AC, 0};
}
static inline delta_t spill(struct kitchensink_ctx* ctx, int s) {
    (void)ctx; return (delta_t){DELTA_SPILL, s};
}

// ─── ctrl_dest gamma — paper's γ ───────────────────────────────
typedef enum { GAMMA_FAIL, GAMMA_JUMP } gamma_tag_t;
typedef struct { gamma_tag_t tag; label_t target; } gamma_t;
static inline gamma_t fail(struct kitchensink_ctx* ctx) {
    (void)ctx; return (gamma_t){GAMMA_FAIL, 0};
}
static inline gamma_t jump(struct kitchensink_ctx* ctx, label_t L) {
    (void)ctx; return (gamma_t){GAMMA_JUMP, L};
}

// ─── env_dest rho — paper's ρ ──────────────────────────────────
typedef enum { RHO_FRAME, RHO_SCOPED } rho_tag_t;
typedef struct { rho_tag_t tag; int depth; int label; } rho_t;
static inline rho_t frame(struct kitchensink_ctx* ctx, int d) {
    (void)ctx; return (rho_t){RHO_FRAME, d, 0};
}
static inline rho_t scoped(struct kitchensink_ctx* ctx, int d, int l) {
    (void)ctx; return (rho_t){RHO_SCOPED, d, l};
}

// ─── sum demo_op — exercises user-defined sum support ──────────
typedef enum {
    DEMO_OP_OPNOP, DEMO_OP_OPSTEP, DEMO_OP_OPRANGE
} demo_op_tag_t;
typedef struct { demo_op_tag_t tag; int delta; int lo; int hi; } demo_op_t;
static inline demo_op_t OpNop(struct kitchensink_ctx* ctx) {
    (void)ctx; return (demo_op_t){DEMO_OP_OPNOP, 0, 0, 0};
}
static inline demo_op_t OpStep(struct kitchensink_ctx* ctx, int d) {
    (void)ctx; return (demo_op_t){DEMO_OP_OPSTEP, d, 0, 0};
}
static inline demo_op_t OpRange(struct kitchensink_ctx* ctx, int lo, int hi) {
    (void)ctx; return (demo_op_t){DEMO_OP_OPRANGE, 0, lo, hi};
}

#endif
