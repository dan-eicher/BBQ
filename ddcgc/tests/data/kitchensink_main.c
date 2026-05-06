// E2E driver for kitchensink.ddcg compiled via ddcgc -lang c.
//
// Mirrors big_main.cpp: builds a tree designed to fire every rule,
// runs the generated dispatcher, and verifies that the catalogued
// rule-fire markers and action-language markers all show up in the
// trace, plus the integer result matches the expected sum.
//
// Aux/pred bodies live here (not in MEMBERS) — the C-mode contract
// puts function definitions outside the ctx struct.

#include "kitchensink_runtime.h"
#include "kitchensink_compile.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ── Trace helpers ────────────────────────────────────────────

static void trace_push(kitchensink_ctx_t* ctx, const char* s) {
    int cap = (int)(sizeof(ctx->trace_entries) /
                    sizeof(ctx->trace_entries[0]));
    if (ctx->trace_count < cap) {
        ctx->trace_entries[ctx->trace_count++] = s;
    }
}

static const char* trace_fmt(kitchensink_ctx_t* ctx,
                             const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    char* r = (char*)bbq_arena_alloc(ctx->arena, (size_t)n + 1);
    memcpy(r, buf, (size_t)n);
    r[n] = '\0';
    return r;
}

// ── Aux / pred bodies (consumer-supplied per the C-mode contract) ──

int kitchensink_trace_fire(kitchensink_ctx_t* ctx, const char* name) {
    trace_push(ctx, trace_fmt(ctx, "rule:%s", name));
    return 0;
}

int kitchensink_cg_lit(kitchensink_ctx_t* ctx, int n) {
    trace_push(ctx, trace_fmt(ctx, "cg_lit:%d", n));
    return n;
}

int kitchensink_cg_seq(kitchensink_ctx_t* ctx,
                        kitchensink_list_int_t xs) {
    trace_push(ctx, trace_fmt(ctx, "cg_seq:%d", xs.count));
    int s = 0;
    for (int i = 0; i < xs.count; ++i) s += xs.data[i];
    return s;
}

int kitchensink_cg_pick(kitchensink_ctx_t* ctx, int a, int b) {
    trace_push(ctx, trace_fmt(ctx, "cg_pick"));
    return a + b;
}

int kitchensink_cg_str(kitchensink_ctx_t* ctx, const char* s) {
    trace_push(ctx, trace_fmt(ctx, "cg_str:%s", s));
    return (int)strlen(s);
}

kitchensink_tup_int_int_t kitchensink_cg_split(kitchensink_ctx_t* ctx,
                                                int x) {
    trace_push(ctx, trace_fmt(ctx, "cg_split:%d", x));
    kitchensink_tup_int_int_t r = { x / 2, x - x / 2 };
    return r;
}

int kitchensink_place(kitchensink_ctx_t* ctx, label_t L) {
    trace_push(ctx, trace_fmt(ctx, "place:L%d", (int)L));
    return 0;
}

int kitchensink_log_value(kitchensink_ctx_t* ctx, target_value_t* v) {
    if (v->tag == TARGET_IMM) {
        trace_push(ctx, trace_fmt(ctx, "log_value:imm:%d", v->imm.v));
        return v->imm.v;
    }
    trace_push(ctx, trace_fmt(ctx, "log_value:slot:%d", v->slot.s));
    return v->slot.s;
}

int kitchensink_bool_demo(kitchensink_ctx_t* ctx, bool b) {
    trace_push(ctx, trace_fmt(ctx, "bool_demo:%s", b ? "true" : "false"));
    return b ? 1 : 0;
}

int kitchensink_tup_sum(kitchensink_ctx_t* ctx,
                         kitchensink_tup_int_int_t t) {
    trace_push(ctx, trace_fmt(ctx, "tup_sum:%d:%d", t._0, t._1));
    return t._0 + t._1;
}

int kitchensink_inspect_node(kitchensink_ctx_t* ctx, ast_expr_t* n) {
    trace_push(ctx, trace_fmt(ctx, "inspect_node:tag:%d", (int)n->tag));
    return (int)n->tag;
}

int kitchensink_on_dispatch_result(kitchensink_ctx_t* ctx, int v) {
    trace_push(ctx, "dispatch_hook");
    return v;
}

bool kitchensink_is_zero(kitchensink_ctx_t* ctx, int n) {
    (void)ctx; return n == 0;
}

bool kitchensink_is_short_name(kitchensink_ctx_t* ctx, const char* s) {
    (void)ctx; return strlen(s) < 4;
}

// Fresh-label generator — invoked by the DSL `label L;` statement.
label_t kitchensink_fresh_label(kitchensink_ctx_t* ctx) {
    return ctx->next_label++;
}

// ── Feature catalogues (mirror big_main.cpp's tables) ────────

struct entry { const char* marker; const char* desc; };

static const struct entry kFeatures[] = {
    {"rule:add_zero_l", "Nested ConstructorPat in field position"},
    {"rule:lit_zero",   "Where-guard predicate over int field"},
    {"rule:lit",        "Tuple-destructuring let + ternary + tuple-returning aux"},
    {"rule:add",        "Multi-rule fallback after nested specialisation"},
    {"rule:seq",        "For-loop accumulator + list concat + Sequence field"},
    {"rule:pair",       "Label statement + label-payload γ variant + place()"},
    {"rule:pair_x99",   "WildcardPat in rule head + nested ConstructorPat literal"},
    {"rule:sym_x",      "StringPat literal match"},
    {"rule:sym_short",  "Where-guard predicate over string field"},
    {"rule:sym",        "Generic string-field fallback"},
    {"rule:maybe_none", "NilPat against optional field"},
    {"rule:maybe_some", "BindPat against optional field carrying a value"},
    {"rule:binop_add",  "Enum-value field-pat (OpAdd constant match)"},
    {"rule:binop_sub",  "Enum-value field-pat (OpSub constant match)"},
    {"rule:binop_mul",  "Enum-value field-pat (OpMul constant match)"},
    {"rule:sel",        "match over asdl-enum subject (op_to_int fun)"},
    {"rule:multi",      "RestFieldPat (`...nums`) on sequence-typed field"},
};
static const int kFeatureCount =
    sizeof(kFeatures) / sizeof(kFeatures[0]);

static const struct entry kActionMarkers[] = {
    {"log_value:imm:99",   "fun + match `ac` arm + build target.Imm"},
    {"log_value:slot:7",   "fun + match `spill(slot)` arm + build target.Slot"},
    {"rule:import_demo",   "fun-from-fun trace propagation (mirrors import)"},
    {"bool_demo:true",     "BoolLit `true` literal in expression"},
    {"tup_sum:50:-50",     "TupleLit `(a, b)` constructor + tuple-typed aux arg"},
    {"dispatch_hook",      "on_dispatch_result wrap fires per dispatcher return"},
};
static const int kActionCount =
    sizeof(kActionMarkers) / sizeof(kActionMarkers[0]);

static int trace_contains(kitchensink_ctx_t* ctx, const char* needle) {
    for (int i = 0; i < ctx->trace_count; ++i) {
        if (strcmp(ctx->trace_entries[i], needle) == 0) return 1;
    }
    return 0;
}

static void dump_trace(kitchensink_ctx_t* ctx) {
    fprintf(stderr, "── trace (%d entries) ──\n", ctx->trace_count);
    for (int i = 0; i < ctx->trace_count; ++i) {
        fprintf(stderr, "  %s\n", ctx->trace_entries[i]);
    }
}

int main(void) {
    bbq_arena arena;
    bbq_arena_init(&arena, 0);

    // Same tree shape as big_main.cpp — every rule's catalogued
    // sub-tree gets a turn.
    ast_expr_t* l0 = ast_lit(&arena, 0);
    ast_expr_t* l7 = ast_lit(&arena, 7);
    ast_expr_t* add_zero_left = ast_add(&arena, l0, l7);

    ast_expr_t* lit_zero_node = ast_lit(&arena, 0);
    ast_expr_t* lit_50        = ast_lit(&arena, 50);

    ast_expr_t* l2 = ast_lit(&arena, 2);
    ast_expr_t* l3 = ast_lit(&arena, 3);
    ast_expr_t* add_general = ast_add(&arena, l2, l3);

    ast_expr_t* l10 = ast_lit(&arena, 10);
    ast_expr_t* l20 = ast_lit(&arena, 20);
    ast_expr_t* pair_node = ast_pair(&arena, l10, l20);

    // pair_x99 fires when second is Lit(99) — exercises WildcardPat
    // in rule head + nested ConstructorPat literal match.
    ast_expr_t* l1  = ast_lit(&arena, 1);
    ast_expr_t* l99 = ast_lit(&arena, 99);
    ast_expr_t* pair_99_node = ast_pair(&arena, l1, l99);

    // Multi exercises RestFieldPat (`...nums`) on a sequence field.
    static int multi_nums[] = {10, 20, 30};
    ast_expr_t* multi_node = ast_multi(&arena, multi_nums, 3);

    ast_expr_t* sym_x_node     = ast_sym(&arena, "x");
    ast_expr_t* sym_short_node = ast_sym(&arena, "ab");
    ast_expr_t* sym_long_node  = ast_sym(&arena, "longname");

    ast_expr_t* maybe_none_node = ast_maybe(&arena, NULL);
    ast_expr_t* l5 = ast_lit(&arena, 5);
    ast_expr_t* maybe_some_node = ast_maybe(&arena, l5);

    ast_expr_t* l4 = ast_lit(&arena, 4);
    ast_expr_t* l6 = ast_lit(&arena, 6);
    ast_expr_t* l8 = ast_lit(&arena, 8);
    ast_expr_t* l9 = ast_lit(&arena, 9);
    ast_expr_t* binop_add_node = ast_bin_op(&arena, AST_OPADD, l4, l6);
    ast_expr_t* binop_sub_node = ast_bin_op(&arena, AST_OPSUB, l8, l9);
    ast_expr_t* binop_mul_node = ast_bin_op(&arena, AST_OPMUL, l2, l3);
    ast_expr_t* sel_node       = ast_sel(&arena, AST_OPMUL);

    ast_expr_t* items[] = {
        add_zero_left, lit_zero_node, lit_50, add_general, pair_node,
        pair_99_node, multi_node,
        sym_x_node, sym_short_node, sym_long_node, maybe_none_node,
        maybe_some_node, binop_add_node, binop_sub_node, binop_mul_node,
        sel_node,
    };
    int n_items = (int)(sizeof(items) / sizeof(items[0]));
    ast_expr_t* root = ast_seq(&arena, items, n_items);

    kitchensink_ctx_t ctx = {0};
    ctx.arena = &arena;
    rho_t rho = {0};
    int result = kitchensink_compile_expr(&ctx, root, rho, ac(&ctx), fail(&ctx), 0);

    int rc = 0;
    for (int i = 0; i < kFeatureCount; ++i) {
        if (!trace_contains(&ctx, kFeatures[i].marker)) {
            fprintf(stderr, "FAIL: feature '%s' (%s) never fired\n",
                    kFeatures[i].marker, kFeatures[i].desc);
            rc = 1;
        }
    }
    for (int i = 0; i < kActionCount; ++i) {
        if (!trace_contains(&ctx, kActionMarkers[i].marker)) {
            fprintf(stderr, "FAIL: action marker '%s' (%s) never logged\n",
                    kActionMarkers[i].marker, kActionMarkers[i].desc);
            rc = 1;
        }
    }
    // Expected sum (extends big_main.cpp's 140 with two new feature-
    // exercising rules):
    //   add_zero_l(Lit(0), Lit(7)) → 7
    //   lit_zero(0)                → 0
    //   lit(50)                    → 50
    //   add(Lit(2), Lit(3))        → 5
    //   pair(Lit(10), Lit(20))     → 20
    //   pair_x99(Lit(1), Lit(99))  → 99   (WildcardPat in head)
    //   multi([10,20,30])          → 60   (RestFieldPat)
    //   sym_x                      → 9
    //   sym_short("ab")            → 2
    //   sym("longname")            → 8
    //   maybe_none                 → -1
    //   maybe_some(Lit(5))         → 5
    //   binop_add(4, 6)            → 10
    //   binop_sub(8, 9)            → 17
    //   binop_mul(2, 3)            → 5
    //   sel(OpMul)                 → 3
    //                       Σ items = 299
    enum { kExpected = 299 };
    if (result != kExpected) {
        fprintf(stderr, "FAIL: expected result=%d, got %d\n",
                kExpected, result);
        rc = 1;
    }

    if (rc) {
        dump_trace(&ctx);
    } else {
        printf("ddcgc kitchensink.ddcg — %d features + %d action markers:\n",
               kFeatureCount, kActionCount);
        for (int i = 0; i < kFeatureCount; ++i) {
            printf("  ok %-20s  %s\n",
                   kFeatures[i].marker, kFeatures[i].desc);
        }
        for (int i = 0; i < kActionCount; ++i) {
            printf("  ok %-20s  %s\n",
                   kActionMarkers[i].marker, kActionMarkers[i].desc);
        }
        printf("result=%d, trace=%d entries\n", result, ctx.trace_count);
    }

    bbq_arena_free(&arena);
    return rc;
}
