/* scoped_main.c — exercise the generated dispatcher with a small
 * AST that nests Scope around a Lookup, and assert the chain depth
 * the dispatcher returns. */

#include "scoped_runtime.h"
#include "scoped_compile.h"
#include "bbq_arena.h"

#include <stdio.h>
#include <stdlib.h>

/* Aux stubs — never called by this test corpus. */
label_t scoped_fresh_label(scoped_ctx_t* ctx) { (void)ctx; return 0; }
int     scoped_panic_unreachable(scoped_ctx_t* ctx, const char* m) {
    (void)ctx; fprintf(stderr, "panic_unreachable: %s\n", m); abort();
}

int main(void) {
    bbq_arena arena;
    bbq_arena_init(&arena, 256);

    /* AST: Scope(7, Scope(3, Scope(7, Lookup))) — Lookup target is 7,
     * the innermost matching Scope. Expected depth = 0. */
    ast_expr_t* lookup = ast_lookup(&arena);
    ast_expr_t* inner  = ast_scope(&arena, 7, lookup);
    ast_expr_t* mid    = ast_scope(&arena, 3, inner);
    ast_expr_t* outer  = ast_scope(&arena, 7, mid);

    scoped_ctx_t ctx;
    int result = scoped_compile_expr(&ctx, outer,
                                      top(&ctx),
                                      sink(&ctx), halt(&ctx), 0);

    /* Lookup compiles with ρ chain (innermost → outermost):
     *   in_scope(7) -> in_scope(3) -> in_scope(7) -> top.
     * chain_depth_for searches for label=7, returns first match
     * starting from innermost. Expected = 0 (innermost match). */
    if (result != 0) {
        fprintf(stderr, "FAIL: expected 0, got %d\n", result);
        return 1;
    }
    printf("scoped: OK (innermost match at depth 0)\n");
    return 0;
}
