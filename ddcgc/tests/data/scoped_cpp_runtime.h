// scoped_cpp_runtime.h — C++-mode recursive ρ test runtime.
//
// Demonstrates the convention for recursive env_dest variants in
// C++: the recursive `parent: rho` field is stored as a pointer
// (the struct can't contain itself by value), and the constructor
// heap-copies the parent on call. ddcgc emits the constructor call
// by-value; the user-supplied constructor handles allocation.
//
// In a real consumer, the heap copy would land in an arena or
// equivalent. For test purposes, plain `new` is fine — process
// lifetime is bounded.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

#include "scoped_cpp_ast.h"
#include "scoped_cpp_target.h"

using SourceLoc = scoped_cpp_ast::SourceLoc;

// label_t is required by every ddcgc-generated header.
using label_t = int;

namespace scoped_cpp {
    // δ — sink (single-variant placeholder; not exercised).
    enum class DeltaTag { Sink };
    struct delta_t { DeltaTag tag; };
    inline delta_t sink() { return {DeltaTag::Sink}; }

    // γ — halt (single-variant placeholder; not exercised).
    enum class GammaTag { Halt };
    struct gamma_t { GammaTag tag; };
    inline gamma_t halt() { return {GammaTag::Halt}; }

    // ρ — recursive: top | in_scope(label, parent: rho).
    enum class RhoTag { Top, InScope };
    struct rho_t {
        RhoTag tag;
        int label;          // InScope only
        rho_t* parent;      // InScope only
    };

    inline rho_t top() {
        return {RhoTag::Top, 0, nullptr};
    }

    inline rho_t in_scope(int label, rho_t parent_val) {
        // Heap-copy parent so it survives the caller's stack frame.
        // Test-only: production runtimes use an arena.
        auto* p = new rho_t(parent_val);
        return {RhoTag::InScope, label, p};
    }
}

using namespace scoped_cpp;
