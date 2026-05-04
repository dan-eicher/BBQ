// Test/CLI API for the calc end-to-end pipeline.
//
// `calc_compile()` runs parse + DDCG-compile and returns the IR head;
// callers can walk it with `count_node_kind()` for structural assertions.
// `calc_run()` runs the full pipeline (parse → compile → JIT → execute)
// and returns the value left in `ctx.ac` at exit — every program
// delivers its result there via the driver's top-level δ=ac wrap.

#pragma once

#include "frontend/CalcIR.h"

#include <cstdint>

namespace calc_runner {

// Build the IR continuation graph for `input`. Returns nullptr on parse
// error. If `main_frame_size` is non-null, the post-compile size of
// main's frame (params + locals + case-4 spill slots) is written there
// — the caller uses it to initialize `ctx.sp` before invoking the
// JIT'd code so eval-stack pushes don't clobber main's locals.
calc_ir::Node* calc_compile(const char* input, int* main_frame_size = nullptr);

// Parse → compile → JIT → execute. Returns ctx.ac after the program
// finishes. Returns 0 on parse error and stamps `*ok = false`.
int64_t calc_run(const char* input, bool* ok = nullptr);

// Count nodes of a given tag reachable from `ir`. Branch's two
// successors are both followed; cycles are guarded.
int count_node_kind(calc_ir::Node* ir, calc_ir::NodeTag kind);

} // namespace calc_runner
