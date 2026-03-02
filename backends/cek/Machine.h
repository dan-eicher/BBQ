#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ParseArena.h"
#include "Capture.h"
#include "KontNode.h"
#include "Environment.h"
#include "Frame.h"
#include "Expr.h"

namespace bbq::cek {

// --- External parser function signature ---
// Returns true on success, false on failure.
// On success, *bytes_consumed is set to the number of bytes consumed.
using ExternalParseFn = bool (*)(
    const uint8_t* data,    // Input at current position
    size_t length,          // Remaining bytes
    size_t* bytes_consumed, // Out: how many bytes were consumed
    ParseArena* arena,      // For allocating output data
    void* user_data         // User context
);

// --- External parser registry ---
// Linear-scan lookup by interned pointer equality.
struct ExternalParserTable {
    struct Entry {
        const char* name;       // Interned
        ExternalParseFn fn;
        void* user_data;        // Per-entry context
    };
    Entry* entries = nullptr;   // User-allocated array
    int count = 0;

    const Entry* lookup(const char* name) const {
        for (int i = 0; i < count; i++) {
            if (entries[i].name == name) return &entries[i];
        }
        return nullptr;
    }
};

// --- User expression function signature ---
// Called during expression evaluation (e.g., custom functions in where/compute).
using ExprFunction = int64_t (*)(const int64_t* args, int arg_count,
                                  const CEKMachine* machine);

// --- User expression function registry ---
struct FunctionTable {
    struct Entry {
        const char* name;       // Interned
        ExprFunction fn;
    };
    Entry* entries = nullptr;   // User-allocated array
    int count = 0;

    ExprFunction lookup(const char* name) const {
        for (int i = 0; i < count; i++) {
            if (entries[i].name == name ||
                std::strcmp(entries[i].name, name) == 0)
                return entries[i].fn;
        }
        return nullptr;
    }
};

struct CEKMachine {
    // --- CEK state ---
    KontNode* control = nullptr;
    Environment* env = nullptr;
    Frame* frame_top = nullptr;

    // --- Input ---
    const uint8_t* input = nullptr;
    size_t input_length = 0;
    size_t pos = 0;

    // --- Endianness ---
    bool little_endian = true;

    // --- Output ---
    CaptureBuilder builder;

    // --- Memory ---
    ParseArena* arena = nullptr;

    // --- Extensibility ---
    ExternalParserTable* ext_parsers = nullptr;
    FunctionTable* func_table = nullptr;

    // --- Error state ---
    bool failed = false;
    size_t best_error_pos = 0;
    const char* best_error_msg = nullptr;

    // --- Entry point ---
    CaptureMetadata execute_from(KontNode* entry, const uint8_t* data,
                                 size_t length, bool default_little_endian = true);

    // --- Error ---
    void fail(const char* message);
};

// --- Free functions ---
int64_t read_primitive(const uint8_t* data, size_t pos, PrimitiveInfo prim,
                       bool little_endian);
CaptureType primitive_capture_type(PrimitiveInfo prim, bool little_endian);

} // namespace bbq::cek
