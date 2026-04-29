// ddcgc — C++ emit backend.
//
// Walks a parsed + type-checked DdcgAst::File and emits a compilable
// C++ source file containing one `compile_<sum>` dispatch function per
// source-AST sum that has rules. Each dispatch:
//
//   - sets ctx->current_loc = node->loc
//   - dynamic_cast chains through constructors
//   - runs the matched rule's body
//
// Dispatcher signature follows the paper's CG : E → ρ → δ → γ → Code,
// with ρ folded into ctx_t:
//
//   <ir_root> compile_<sum>(ctx_t*, <Sum>* node, <δ>_t d, <γ>_t g)
//
// Auxiliary calls (cg_*), predicate calls, and dest-variant
// constructors emit as plain function calls with `ctx` threaded as the
// first argument. The runtime provides the implementations.
//
// `gen(subject, δ, γ, [lnext])` calls dispatch to compile_<sum> based
// on the subject's inferred schema type (from CheckResult::expr_types).
// Mutually-recursive dispatchers are forward-declared at the top.

#pragma once

#include "ddcg_ast.h"
#include "schema.h"
#include "typecheck.h"

#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace ddcgc {

// Emit C++ source for the type-checked .ddcg `file` to `out`.
// `check` carries the expression-type side-table populated by
// check_file(); the backend uses it to resolve gen-call dispatch.
//
// Returns true on success, false on emission error.
bool emit_cpp(const DdcgAst::File* file,
              const std::map<std::string, Schema>& schemas,
              const CheckResult& check,
              std::ostream& out,
              std::vector<std::string>& errors);

} // namespace ddcgc
