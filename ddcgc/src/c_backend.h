// ddcgc — C emit backend.
//
// Produces plain C dispatchers from a parsed + type-checked .ddcg.
// Subclasses EmitBackend; inherits the language-agnostic walk
// scaffolding and overrides the syntax-emitting hooks.
//
// User contract for C-mode (different from C++ mode):
//   - MEMBERS — DATA fields only. Becomes the body of
//     `typedef struct <name>_ctx { ... } <name>_ctx_t;`. Function
//     definitions in MEMBERS produce a normal C compile error on
//     the generated file (no ddcgc-side validation).
//   - PRIVATE — file-scope helpers/typedefs in the generated TU.
//   - Auxiliary functions — caller-supplied free functions with
//     signature `<ret> <name>_<aux>(<name>_ctx_t* ctx, <args>);`.
//     ddcgc emits forward decls; consumer provides definitions.
//   - Dispatcher signature: `<ret> <name>_compile_<sum>(<name>_ctx_t*,
//     <module>_<sum>_t* node, <rho>_t, <delta>_t, <gamma>_t,
//     <Lnext-type> Lnext);` — `ctx` is first arg, no defaults.
//
// Source AST and target IR types come from `asdl -l c` schemas
// (tag-enum + tagged-union per sum). Dispatch is `switch (node->tag)`,
// not the C++ chain of dynamic_cast.

#pragma once

#include "backend.h"

#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace ddcgc {

// Convenience entry — wraps create_c_backend() and emit().
// Mirrors emit_cpp() so test harnesses can call either backend
// directly without the factory dance.
bool emit_c(const DdcgAst::File* file,
            const std::map<std::string, Schema>& schemas,
            const CheckResult& check,
            std::ostream& out,
            std::vector<std::string>& errors);

} // namespace ddcgc
