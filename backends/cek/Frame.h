#pragma once

#include <cstddef>

#include "KontNode.h"
#include "Environment.h"
#include "Capture.h"

namespace bbq::cek {

enum class FrameType : uint8_t {
    Return,
    Alternative,
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

    ReturnFrame() { type = FrameType::Return; }
};

struct AlternativeFrame : Frame {
    KontNode** remaining = nullptr;     // Array of untried alternative entry points
    int remaining_count = 0;
    KontNode* join = nullptr;           // Where to continue after success
    Environment* saved_env = nullptr;
    size_t saved_pos = 0;
    bool saved_endian = true;
    size_t saved_kont_size = 0;         // Truncate kont_stack to this on restore
    CaptureBuilder::Mark capture_mark{};

    AlternativeFrame() { type = FrameType::Alternative; }
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
