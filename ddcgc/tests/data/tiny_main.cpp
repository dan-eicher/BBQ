// End-to-end test driver for the ddcgc-generated tiny dispatcher.
//
// Builds an Add(Lit(2), Lit(3)) tree, runs the generated tiny::Compiler's
// compile_expr method against it, and asserts the result is 5 plus three
// aux-call invocations (one per Lit and one for the Add).

#include "tiny_runtime.h"
#include "tiny_compile.h"

#include <cassert>
#include <cstdio>

int main() {
    using namespace tiny;
    Lit two(2);
    Lit three(3);
    Add add_expr(&two, &three);

    Compiler compiler;
    rho_t rho = frame(0);
    int result = compiler.compile_expr(&add_expr, rho, ac(), fail());

    if (result != 5) {
        std::fprintf(stderr, "expected 5, got %d\n", result);
        return 1;
    }
    if (compiler.aux_calls != 3) {
        std::fprintf(stderr, "expected 3 aux_calls, got %d\n", compiler.aux_calls);
        return 1;
    }
    return 0;
}
