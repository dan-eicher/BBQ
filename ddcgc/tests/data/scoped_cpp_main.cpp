// scoped_cpp_main.cpp — exercise the C++-emitted dispatcher with
// a small AST that nests Scope around a Lookup, and assert the
// chain depth the dispatcher returns.

#include "scoped_cpp_runtime.h"
#include "scoped_cpp_compile.h"

#include <cstdio>

int main() {
    using namespace scoped_cpp_ast;

    // AST: Scope(7, Scope(3, Scope(7, Lookup))) — Lookup target is 7,
    // the innermost matching Scope. Expected depth = 0.
    auto* lookup = new Lookup();
    auto* inner  = new Scope(7, lookup);
    auto* mid    = new Scope(3, inner);
    auto* outer  = new Scope(7, mid);

    scoped_cpp::Compiler compiler;
    int result = compiler.compile_expr(
        outer, scoped_cpp::top(),
        scoped_cpp::sink(), scoped_cpp::halt(), 0);

    if (result != 0) {
        std::fprintf(stderr, "FAIL: expected 0, got %d\n", result);
        return 1;
    }
    std::printf("scoped_cpp: OK (innermost match at depth 0)\n");
    return 0;
}
