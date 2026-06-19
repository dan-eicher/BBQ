#pragma once
//
// bbq_machine — the CEK machine state shared by both refunctionalizations.
//
// ddcg defunctionalizes the grammar into the kont graph; the CEK interprets it; the
// reader and writer stencils are the two refunctionalizations of that graph. Both ride
// the SAME machine state — the endian register and the array-loop stack — so it lives
// here once. The reader (bbq::reader) adds an input cursor + interval CONFINEMENT + the
// CaptureBuilder; the writer (bbq::writer) adds an output cursor + interval size-BACKPATCH
// + the graph cursor. Those are genuinely dual (not shared); this base is only what is
// identical between them.
//
#include <cstdint>
#include <vector>

namespace bbq {

struct machine {
    // Endian register: seeded once from the grammar default at start; `@endian:` switches
    // then persist (the reader reads with it, the writer writes with it).
    bool little_endian = true;
    bool endian_seeded = false;

    // Array loop stack — one (index, limit) per open array. The reader bounds iteration by
    // the consumed elements; the writer knows the limit (the graph's child count).
    std::vector<int64_t> loop_index_stack;
    std::vector<int64_t> loop_limit_stack;

    void push_loop(int64_t n) { loop_index_stack.push_back(0); loop_limit_stack.push_back(n); }
    bool loop_next() { return ++loop_index_stack.back() < loop_limit_stack.back(); }
    int64_t loop_index() const { return loop_index_stack.back(); }
    void pop_loop() { loop_index_stack.pop_back(); loop_limit_stack.pop_back(); }
    // Uncounted (eof/until) arrays: no limit — the continuation check decides each
    // iteration; loop_advance bumps @index.
    void push_loop_unbounded() { loop_index_stack.push_back(0); loop_limit_stack.push_back(-1); }
    void loop_advance() { ++loop_index_stack.back(); }
};

}  // namespace bbq
