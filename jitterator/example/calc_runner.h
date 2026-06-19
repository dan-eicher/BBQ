// Test/CLI API for the calc end-to-end pipeline.
//
// `calc_compile()` runs parse + DDCG-compile and returns the IR head;
// callers can walk it with `count_node_kind()` for structural assertions.
// `lower()` runs the burg matcher (cg_jump) over the IR and returns the
// opgen calc bytecode. A caller then runs that bytecode on the opgen
// two-tier VM (interpreter / copy-and-patch JIT).

#pragma once

#include "frontend/CalcIR.h"

#include <cstdint>
#include <vector>

namespace calc_runner {

// Build the IR continuation graph for `input`. Returns nullptr on parse
// error. If `main_frame_size` is non-null, the post-compile size of
// main's frame (params + locals) is written there — the caller uses it
// to initialize the VM's stack pointer past main's locals.
calc_ir::Node* calc_compile(const char* input, int* main_frame_size = nullptr);

// Lower the IR continuation graph to opgen calc bytecode via the burg
// matcher (CalcMatcher.h, driving Dybvig cg_jump). Returns the
// finalized, backpatched byte stream ready for the opgen VM.
std::vector<uint8_t> lower(calc_ir::Node* ir);

// Count nodes of a given tag reachable from `ir`. Branch's two
// successors are both followed; cycles are guarded.
int count_node_kind(calc_ir::Node* ir, calc_ir::NodeTag kind);

} // namespace calc_runner
