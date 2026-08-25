#pragma once
//
// The two things every layer agrees on about a parsed value: what KIND it is, and how a
// value the grammar computed rather than read is carried. Both are neutral — the index
// runtime does not depend on the CEK machine's Value IR, and the machine lowers into
// these at the capture boundary.
//
// What a parsed document IS lives in CaptureCow.h (bbq::zcow). There is one tree.
//
#include <cstddef>
#include <cstdint>

#include "ParseArena.h"

namespace bbq {

// A computed field's value (compute(...), LEB128, bitfield run): a small tagged
// scalar, arena-owned.
struct ComputedValue {
    enum class Kind : uint8_t { Int, Float, Bool, String };
    Kind kind = Kind::Int;
    union {
        int64_t i;
        double f;
        bool b;
        const char* s;
    };
};

enum class CaptureType : uint8_t {
    UInt8, UInt16LE, UInt16BE, UInt32LE, UInt32BE, UInt64LE, UInt64BE,
    Int8,  Int16LE,  Int16BE,  Int32LE,  Int32BE,  Int64LE,  Int64BE,
    Float32LE, Float32BE, Float64LE, Float64BE,
    Bool, Bytes, String,
    Struct, Array, Computed, External
};

} // namespace bbq
