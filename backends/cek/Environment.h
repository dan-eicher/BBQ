#pragma once

#include <cstddef>
#include <cstdint>

#include "Capture.h"

namespace bbq::cek {

struct Binding {
    const char* name = nullptr;     // Interned string
    Value* value = nullptr;         // Typed value for expressions
    size_t start_offset = 0;        // Capture interval start
    size_t end_offset = 0;          // Capture interval end
    CaptureType type = CaptureType::UInt8;
};

struct Environment {
    Binding binding;
    Environment* parent = nullptr;

    // Walk chain with pointer comparison (names interned)
    const Binding* lookup(const char* name) const {
        for (auto* e = this; e; e = e->parent) {
            if (e->binding.name == name) return &e->binding;
        }
        return nullptr;
    }
};

} // namespace bbq::cek
