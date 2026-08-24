#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ParseArena.h"

namespace bbq {

// A computed field's value (compute(...), LEB128, bitfield run): a small tagged
// scalar, arena-owned. Neutral on purpose — the index runtime does NOT depend on
// the CEK machine's Value IR (KontNode.h). The machine lowers its Value into this
// at the capture boundary; readers crack it via decode_int (CaptureDecode.cpp).
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

struct FieldCapture {
    const char* name = nullptr;     // Interned field name
    size_t start_offset = 0;
    size_t end_offset = 0;
    CaptureType type = CaptureType::UInt8;

    // children is nullptr during the build phase. CaptureBuilder::finish()
    // resolves it to a real pointer into the arena. During build, navigation
    // into children happens via build_buf_index (an offset into child_buf_).
    FieldCapture* children = nullptr;
    int child_count = 0;
    int build_buf_index = -1;       // Build-phase only; -1 once resolved

    // For a tagged capture (union/alternatives variant, switch arm) this records
    // which arm the parser took — the discriminator DECISION, kept here so readers
    // read it instead of re-deriving the discriminant. -1 = not a tagged capture.
    int variant_tag = -1;

    ComputedValue* computed_value = nullptr;  // For Computed fields (arena-owned)

    // Parent in the resolved tree (set by finish()); null at the root. Lets a
    // mutation walk to the root to mark the path dirty (copy-on-write path
    // copying), so the writer never blits a stale ancestor range. Build-phase
    // captures leave it null; it is filled when copy_to_arena resolves children.
    const FieldCapture* parent = nullptr;

    const FieldCapture* child(const char* n) const {
        // Pointer comparison — names must be interned
        for (int i = 0; i < child_count; i++) {
            if (children[i].name == n) return &children[i];
        }
        return nullptr;
    }
};

// The result of a parse: the document, plus how the parse went. `bytes_consumed` and the
// error fields are facts about the PARSE; the data itself is reached as a `bbq::Document`
// (bbq_document.h, `document_of`). `root`/`buf` are how that document is minted — a
// consumer navigates the Document rather than the index, so that it never has to carry the
// buffer alongside the index and keep them in step by hand.
struct CaptureMetadata {
    bool success = false;
    size_t bytes_consumed = 0;
    FieldCapture* root = nullptr;
    const uint8_t* buf = nullptr;   // what the captures' offsets point into
    size_t len = 0;

    const char* error_message = nullptr;
    size_t error_offset = 0;
};

class CaptureBuilder {
public:
    struct Mark {
        int depth;
        size_t child_index;
        size_t child_buf_index;
    };

    void begin_struct(const char* name, size_t pos) {
        begin_scope(name, pos, CaptureType::Struct);
    }

    void end_struct(size_t pos) {
        end_scope(pos, CaptureType::Struct);
    }

    void begin_array(const char* name, size_t pos) {
        begin_scope(name, pos, CaptureType::Array);
    }

    void end_array(size_t pos) {
        end_scope(pos, CaptureType::Array);
    }

    void add_field(const char* name, size_t start, size_t end,
                   CaptureType type, ComputedValue* computed_value = nullptr) {
        fields_.push_back({name, start, end, type, nullptr, 0, -1, take_pending_tag(),
                           computed_value});
    }

    void add_computed(const char* name, ComputedValue* value, size_t pos) {
        fields_.push_back({name, pos, pos, CaptureType::Computed, nullptr, 0, -1,
                           take_pending_tag(), value});
    }

    // Computed over an explicit [start, end) span — a varint records the bytes it
    // consumed (its value is data-derived, but the span is the real LEB extent).
    void add_computed(const char* name, ComputedValue* value, size_t start, size_t end) {
        fields_.push_back({name, start, end, CaptureType::Computed, nullptr, 0, -1,
                           take_pending_tag(), value});
    }

    // A union/alternatives variant scope: a named struct scope that also records
    // which arm matched (variant_tag), so readers read the decision, not re-derive it.
    void begin_variant(const char* name, size_t pos, int variant_tag) {
        begin_scope(name, pos, CaptureType::Struct, variant_tag);
    }

    // A switch arm captures under the field name (no variant scope), so the chosen
    // case ordinal can't ride a begin_variant. The CEK arms it here at dispatch; the
    // arm's first capture (add_field for a scalar arm, begin_struct for a rule/struct
    // arm) consumes it as that capture's variant_tag — recording the decision.
    void set_pending_variant_tag(int tag) { pending_variant_tag_ = tag; }

    Mark mark() const {
        return {static_cast<int>(scopes_.size()),
                fields_.size(),
                child_buf_.size()};
    }

    // Start offset of the innermost open struct/array scope — backs the `buffer`
    // builtin (the current construct's [start, pos)). With no open scope we are in
    // an inlined top-level struct, whose parse origin is 0. Backtrack-safe: scopes_
    // is rewound by restore().
    size_t current_scope_start() const {
        return scopes_.empty() ? 0 : scopes_.back().start_pos;
    }

    void restore(Mark m) {
        scopes_.resize(static_cast<size_t>(m.depth));
        if (m.child_index < fields_.size()) {
            fields_.resize(m.child_index);
        }
        if (m.child_buf_index < child_buf_.size()) {
            child_buf_.resize(m.child_buf_index);
        }
    }

    // `buf`/`len` are the bytes the captures index. They are recorded here so the result
    // can hand out a Document; without them a consumer has to carry the buffer separately
    // and pair it with the index at every access, which is what this replaces.
    CaptureMetadata finish(ParseArena& arena, bool success, size_t bytes_consumed,
                           const uint8_t* buf, size_t len,
                           const char* error_message = nullptr,
                           size_t error_offset = 0) {
        CaptureMetadata meta;
        meta.success = success;
        meta.bytes_consumed = bytes_consumed;
        meta.buf = buf;
        meta.len = len;
        meta.error_message = error_message;
        meta.error_offset = error_offset;

        if (!fields_.empty()) {
            // Resolve all captures into arena, recursively fixing up children
            auto* top_captures = copy_to_arena(arena, fields_.data(),
                                               static_cast<int>(fields_.size()));

            // Create root capture wrapping all top-level fields
            auto* root = arena.alloc<FieldCapture>();
            root->name = nullptr;
            root->start_offset = 0;
            root->end_offset = bytes_consumed;
            root->type = CaptureType::Struct;
            root->children = top_captures;
            root->child_count = static_cast<int>(fields_.size());
            root->computed_value = nullptr;
            for (int i = 0; i < root->child_count; i++)
                top_captures[i].parent = root;
            meta.root = root;
        }

        return meta;
    }

    const std::vector<FieldCapture>& fields() const { return fields_; }

    // Mid-parse field lookup. Searches OUTWARD through the enclosing scopes — an
    // expression may name a field in any enclosing scope, not just the innermost
    // (e.g. a per-array-element interval `@[hdr.off, ...]` referencing a header field
    // parsed before the array opened). fields_ is append-ordered with the innermost
    // scope's fields at the highest indices and closed siblings moved to child_buf_,
    // so a single backward scan resolves innermost-first (correct lexical shadowing).
    const FieldCapture* find_field(const char* name) const {
        for (size_t i = fields_.size(); i > 0; --i) {
            if (fields_[i - 1].name == name) return &fields_[i - 1];
        }
        return nullptr;
    }

    // Content-based sibling of find_field — the generated view parser passes string
    // literals (not interned pointers), so it resolves a field (an array count `[n]`,
    // a nested path base `ehdr.e_shoff`) by name content. Same outward lexical walk.
    const FieldCapture* find_field_str(const char* name) const {
        for (size_t i = fields_.size(); i > 0; --i) {
            if (fields_[i - 1].name && std::strcmp(fields_[i - 1].name, name) == 0)
                return &fields_[i - 1];
        }
        return nullptr;
    }

    // Navigate into a struct/array capture's children by name. Build-phase
    // only — uses build_buf_index, which becomes invalid once finish() runs.
    const FieldCapture* find_child(const FieldCapture* parent, const char* name) const {
        if (parent->child_count <= 0 || parent->build_buf_index < 0) return nullptr;
        size_t buf_start = static_cast<size_t>(parent->build_buf_index);
        for (int i = 0; i < parent->child_count; i++) {
            if (child_buf_[buf_start + i].name == name)
                return &child_buf_[buf_start + i];
        }
        return nullptr;
    }

    // Content-based child lookup (mirrors find_child) — the generated view parser
    // passes string literals, so a nested-path step (e.g. `ehdr.e_shoff`) resolves
    // its field by name content rather than interned-pointer identity.
    const FieldCapture* find_child_str(const FieldCapture* parent, const char* name) const {
        if (parent == nullptr || parent->child_count <= 0 || parent->build_buf_index < 0)
            return nullptr;
        size_t buf_start = static_cast<size_t>(parent->build_buf_index);
        for (int i = 0; i < parent->child_count; i++) {
            if (child_buf_[buf_start + i].name && std::strcmp(child_buf_[buf_start + i].name, name) == 0)
                return &child_buf_[buf_start + i];
        }
        return nullptr;
    }

    // Navigate into an array capture's children by index. Build-phase only.
    const FieldCapture* find_child_at(const FieldCapture* parent, int index) const {
        if (index < 0 || index >= parent->child_count) return nullptr;
        if (parent->build_buf_index < 0) return nullptr;
        size_t buf_start = static_cast<size_t>(parent->build_buf_index);
        return &child_buf_[buf_start + index];
    }

    void clear() {
        fields_.clear();
        scopes_.clear();
        child_buf_.clear();
        pending_variant_tag_ = -1;
    }

private:
    struct Scope {
        const char* name;
        size_t start_pos;
        size_t first_child_index;
        CaptureType type;
        int variant_tag;
    };

    // A pending switch-arm tag (set at dispatch) is consumed by the first capture
    // of the arm, unless this scope already carries an explicit variant_tag (union).
    void begin_scope(const char* name, size_t pos, CaptureType type,
                     int variant_tag = -1) {
        int tag = variant_tag != -1 ? variant_tag : take_pending_tag();
        scopes_.push_back({name, pos, fields_.size(), type, tag});
    }

    int take_pending_tag() {
        int t = pending_variant_tag_;
        pending_variant_tag_ = -1;
        return t;
    }

    void end_scope(size_t pos, CaptureType type) {
        (void)type; // Scope already records its type
        auto& scope = scopes_.back();

        // Children are fields_[first_child_index .. fields_.size())
        size_t first = scope.first_child_index;
        size_t count = fields_.size() - first;

        // Build the scope capture
        FieldCapture fc;
        fc.name = scope.name;
        fc.start_offset = scope.start_pos;
        fc.end_offset = pos;
        fc.type = scope.type;
        fc.children = nullptr;
        fc.child_count = static_cast<int>(count);
        fc.variant_tag = scope.variant_tag;
        fc.computed_value = nullptr;

        // Move children into child_buf_ as a packed range. The scope's
        // FieldCapture records the range via build_buf_index until finish()
        // resolves it to a real arena pointer.
        if (count > 0) {
            size_t buf_start = child_buf_.size();
            for (size_t i = first; i < fields_.size(); i++) {
                child_buf_.push_back(fields_[i]);
            }
            fc.build_buf_index = static_cast<int>(buf_start);
        }

        // Remove children from fields_ and replace with the scope capture
        fields_.resize(first);
        fields_.push_back(fc);

        scopes_.pop_back();
    }

    // Recursively copy fields to arena, resolving build_buf_index → children.
    FieldCapture* copy_to_arena(ParseArena& arena, const FieldCapture* src, int count) {
        if (count == 0) return nullptr;
        auto* dst = static_cast<FieldCapture*>(
            arena.allocate(static_cast<size_t>(count) * sizeof(FieldCapture),
                           alignof(FieldCapture)));
        std::memcpy(dst, src, static_cast<size_t>(count) * sizeof(FieldCapture));

        for (int i = 0; i < count; i++) {
            if ((dst[i].type == CaptureType::Struct || dst[i].type == CaptureType::Array)
                && dst[i].child_count > 0 && dst[i].build_buf_index >= 0) {
                size_t buf_start = static_cast<size_t>(dst[i].build_buf_index);
                dst[i].children = copy_to_arena(arena,
                                                child_buf_.data() + buf_start,
                                                dst[i].child_count);
                dst[i].build_buf_index = -1;
                for (int j = 0; j < dst[i].child_count; j++)
                    dst[i].children[j].parent = &dst[i];
            }
        }
        return dst;
    }

    std::vector<FieldCapture> fields_;
    std::vector<Scope> scopes_;
    std::vector<FieldCapture> child_buf_;   // Temporary storage for struct children
    int pending_variant_tag_ = -1;          // armed at switch dispatch, consumed by the arm
};

} // namespace bbq
