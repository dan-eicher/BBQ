#pragma once

#include <cstddef>

#include "KontNode.h"
#include "Environment.h"
#include "Capture.h"

namespace bbq::cek {

enum class FrameType : uint8_t {
    Return,
    Savepoint,
    Loop,
};

struct Frame {
    FrameType type;
    Frame* prev = nullptr;

protected:
    ~Frame() = default; // Arena-allocated
};

struct ReturnFrame : Frame {
    KontNode* return_to = nullptr;
    Environment* saved_env = nullptr;
    size_t start_pos = 0;
    CaptureBuilder::Mark capture_mark{};
    // Caller's static failure target — copied from the InvokeRuleNode
    // at push time. Non-null when the call sits inside an alt /
    // optional / array-resync scope; null at top-level call sites.
    // Consumed by AbortRuleKont when a where-check walks ρ to root
    // and needs to cross the rule boundary into the caller's scope.
    KontNode* rule_fail_target = nullptr;

    ReturnFrame() { type = FrameType::Return; }
};

// Backtrack savepoint pushed by BeginChoice / BeginOptional. Captures
// machine state so a failure inside a backtrackable scope can roll
// back. The compile-time OnFailKont chain attached via on_fail_target
// statically encodes the alt-arm sequence; each link mutates this
// pointer to advance to the next arm. Null on_fail_target = scope
// exhausted, fail() pops the frame and propagates outward.
struct Savepoint : Frame {
    Environment* saved_env = nullptr;
    size_t saved_pos = 0;
    bool saved_endian = true;
    size_t saved_kont_size = 0;
    int saved_interval_depth = 0;   // unwind any intervals opened inside the arm
    CaptureBuilder::Mark capture_mark{};
    KontNode* on_fail_target = nullptr;

    Savepoint() { type = FrameType::Savepoint; }
};

struct LoopFrame : Frame {
    int64_t counter = 0;
    int64_t limit = -1;                 // -1 = unbounded (EOF/Count/Until)
    KontNode* end_node = nullptr;       // EndArray node for graceful termination
    bool soft_fail = false;             // true = element failure ends the array
    // Outer state — set once in BeginArray, restored by EndArray
    Environment* outer_env = nullptr;
    bool outer_endian = true;
    // PartialCommit state — updated each iteration for backtracking
    Environment* saved_env = nullptr;
    size_t saved_pos = 0;
    bool saved_endian = true;
    CaptureBuilder::Mark capture_mark{};

    LoopFrame() { type = FrameType::Loop; }
};

} // namespace bbq::cek
