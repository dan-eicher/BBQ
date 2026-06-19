#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ParseArena.h"
#include "Capture.h"
#include "KontNode.h"
#include "Environment.h"
#include "Frame.h"

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

// --- Built-in function registry ---
// Linear-scan lookup by interned pointer equality (same idiom as
// ExternalParserTable). Populated at grammar compile time by interning
// each built-in's name into the grammar's StringPool, so the call
// site's func_name (also interned in that pool) matches by pointer.
struct BuiltinFnTable {
    using Handler = void (*)(CEKMachine*, struct Value**);
    struct Entry {
        const char* name;       // Interned in the owning grammar's pool
        int arity;
        Handler fn;
    };
    Entry* entries = nullptr;
    int count = 0;

    const Entry* lookup(const char* name, int arity) const {
        for (int i = 0; i < count; i++) {
            if (entries[i].name == name && entries[i].arity == arity)
                return &entries[i];
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
    // ── CEK state ──────────────────────────────────────────
    Environment* env = nullptr;
    Frame* frame_top = nullptr;

    // ── ac: typed accumulator ───────────────────────────────
    // Producer konts allocate Values in the per-parse arena and
    // write the pointer here. Consumer konts read it and dispatch
    // on the value's tag (via expect<T> below).
    Value* result = nullptr;

    // ── Input ───────────────────────────────────────────────
    const uint8_t* input = nullptr;
    size_t input_length = 0;
    size_t pos = 0;

    // ── Interval bounds ─────────────────────────────────────
    // A stack of [start,end) offsets for `@[start,end]` / `@[length]` regions. Reads are
    // bounded by effective_end() (the innermost bound, or input_length), not by
    // input_length directly — this is what enforces intervals. Seek/PushInterval/
    // PopInterval konts maintain it; Savepoints snapshot interval_depth so a
    // backtrack unwinds it. The same bound-stack model as the C runtime: the vectors are
    // the backing store and grow on demand (no fixed nesting cap), interval_depth is the
    // live height — slots at/above it are stale and reused on the next push.
    std::vector<size_t> interval_starts;
    std::vector<size_t> interval_ends;
    int interval_depth = 0;
    // The active window [effective_start, effective_end). Per the IPG semantics a
    // nonterminal/terminal may inspect ONLY its interval's slice, so reads are
    // confined to this window and a nested interval that escapes it fails.
    size_t effective_start() const {
        return interval_depth > 0 ? interval_starts[interval_depth - 1] : 0;
    }
    size_t effective_end() const {
        return interval_depth > 0 ? interval_ends[interval_depth - 1] : input_length;
    }

    // ── Endianness ──────────────────────────────────────────
    bool little_endian = true;

    // ── Output ──────────────────────────────────────────────
    CaptureBuilder builder;

    // ── Memory ──────────────────────────────────────────────
    ParseArena* arena = nullptr;

    // ── Extensibility ──────────────────────────────────────
    ExternalParserTable* ext_parsers = nullptr;
    FunctionTable* func_table = nullptr;
    // Built-in function dispatch — populated at grammar compile time.
    // CallApplyKont uses this for builtins (peek/abs/min/max/clamp);
    // matching is by interned pointer equality (entries[i].name ==
    // call_site_name), so this must point at the table from the same
    // CompiledGrammar whose StringPool the call sites came from.
    BuiltinFnTable* builtins = nullptr;

    // ── Error state ─────────────────────────────────────────
    bool failed = false;
    size_t best_error_pos = 0;
    const char* best_error_msg = nullptr;

    // ── Entry point ─────────────────────────────────────────
    CaptureMetadata execute_from(KontNode* entry, const uint8_t* data,
                                 size_t length, bool default_little_endian = true);

    // ── Error ───────────────────────────────────────────────
    void fail(const char* message);

    // ── K-stack accessors ───────────────────────────────────
    // Konts schedule a successor by pushing onto K. The trampoline
    // pops and invokes. Konts must NOT manipulate the stack directly
    // (private), call invoke() on another kont (private + friend), or
    // assign to control (private). The only legal scheduling
    // mechanism is push_kont().
    void push_kont(KontNode* k) { kont_stack.push_back(k); }

    // Read-only accessor for the current K size — needed when pushing
    // a Savepoint so backtracking can restore K to this point.
    size_t kont_stack_size() const { return kont_stack.size(); }

    // Truncate the kont stack to a previously captured depth. Only
    // legal call site is OnFailKont::invoke when restoring from a
    // Savepoint — the captured size is exactly the savepoint's
    // saved_kont_size. fail() also uses this on cross-savepoint walks.
    void truncate_kont_stack(size_t n) { kont_stack.resize(n); }

    // ── expect<T> helper ────────────────────────────────────
    // Type-checked Value* downcast. Returns T* on tag match;
    // otherwise calls fail() with a type error and returns nullptr.
    // Used at apply / consumer boundaries to verify ac's runtime type.
    template<typename T>
    T* expect(Value* v, const char* op) {
        if (v && v->tag == T::kind) return static_cast<T*>(v);
        fail(op);
        return nullptr;
    }

private:
    friend struct CEKMachine_;  // sentinel — only CEKMachine accesses these
    // ── C: control ──────────────────────────────────────────
    // Loaded from kont_stack's top each trampoline iteration. Only
    // the trampoline writes it. Any kont's invoke() body that tries
    // m->control = X is a compile error (private).
    KontNode* control = nullptr;

    // ── K: kontinuation (runtime kont stack) ────────────────
    // LIFO stack of pending konts. Konts append via push_kont();
    // the trampoline pops. Konts cannot pop or otherwise mutate
    // the stack directly (private).
    std::vector<KontNode*> kont_stack;
};

// --- Free functions ---
int64_t read_primitive(const uint8_t* data, size_t pos, PrimitiveInfo prim,
                       bool little_endian);
CaptureType primitive_capture_type(PrimitiveInfo prim, bool little_endian);

// Populate a BuiltinFnTable with handlers for the standard built-in
// functions (peek/abs/min/max/clamp). Names are interned into `pool`
// so call-site func_name pointers (interned in the same pool) match
// by pointer-equality during dispatch. Entries are arena-allocated.
struct StringPool;
void populate_builtins_into(BuiltinFnTable& out, ParseArena& arena,
                            StringPool& pool);

// Convenience for the production path: pre-populate g.builtins from
// g.arena/g.strings. Call once at grammar compile time, before any
// parse uses `m.builtins = &g.builtins`.
struct CompiledGrammar;  // forward
void populate_builtins(CompiledGrammar& g);

} // namespace bbq::cek
