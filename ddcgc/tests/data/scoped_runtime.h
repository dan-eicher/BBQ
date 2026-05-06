/* scoped_runtime.h — recursive ρ test runtime.
 *
 * Demonstrates the convention for recursive env_dest variants:
 * the recursive `parent: rho` field is stored as a pointer (C
 * structs can't contain themselves by value), and the constructor
 * heap-copies the parent on call. ddcgc emits the constructor
 * call by-value (`rho_in_scope(label, parent_rho)`), so the user-
 * supplied constructor handles the malloc + copy. */

#pragma once

#include <stdlib.h>

#include "scoped_ast.h"      /* ast_expr_t, ast_*-factories */
#include "scoped_target.h"   /* target_value_t (unused but keeps schemas paired) */

/* label_t is required by every ddcgc-generated header (the
 * fresh_label aux is forward-declared even when unused). */
typedef int label_t;

/* Forward decl so dest constructors can take ctx as first arg. */
struct scoped_ctx;

/* δ = sink (single-variant placeholder; not exercised). */
typedef enum { DELTA_SINK } delta_tag_t;
typedef struct { delta_tag_t tag; } delta_t;
static inline delta_t sink(struct scoped_ctx* ctx) {
    (void)ctx; return (delta_t){DELTA_SINK};
}

/* γ = halt (single-variant placeholder; not exercised). */
typedef enum { GAMMA_HALT } gamma_tag_t;
typedef struct { gamma_tag_t tag; } gamma_t;
static inline gamma_t halt(struct scoped_ctx* ctx) {
    (void)ctx; return (gamma_t){GAMMA_HALT};
}

/* ρ = top | in_scope(label, parent: rho) — recursive.
 *
 * Layout: kitchensink-style flat struct (tag + all variant fields
 * at top level). The `parent` field is a pointer because rho_t
 * can't contain itself by value. The `in_scope` constructor heap-
 * copies its parent argument so the pointer remains valid after
 * the caller's stack frame goes away (test-only convention; real
 * consumers use an arena). */
typedef enum { RHO_TOP, RHO_IN_SCOPE } rho_tag_t;
struct scoped_rho;
typedef struct scoped_rho rho_t;
struct scoped_rho {
    rho_tag_t tag;
    int       label;       /* IN_SCOPE only */
    rho_t*    parent;      /* IN_SCOPE only — heap-allocated copy of caller's ρ */
};

static inline rho_t top(struct scoped_ctx* ctx) {
    (void)ctx;
    rho_t r;
    r.tag = RHO_TOP;
    r.label = 0;
    r.parent = NULL;
    return r;
}

static inline rho_t in_scope(struct scoped_ctx* ctx, int label, rho_t parent) {
    (void)ctx;  /* test runtime — malloc is fine; production uses arena */
    rho_t* p = (rho_t*)malloc(sizeof(rho_t));
    *p = parent;
    rho_t r;
    r.tag = RHO_IN_SCOPE;
    r.label = label;
    r.parent = p;
    return r;
}

/* ir_root int — the .ddcg declares ir_root as int, so dispatcher
 * return type is int. No SIR ctor wiring needed. */
