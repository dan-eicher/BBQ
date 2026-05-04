#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ParseArena.h"

namespace bbq::cek {

// Value is declared in KontNode.h. Forward-declare to avoid an include
// cycle — FieldCapture stores `Value*` for Computed fields.
struct Value;

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

    Value* computed_value = nullptr;    // For Computed fields (typed Value)

    const FieldCapture* child(const char* n) const {
        // Pointer comparison — names must be interned
        for (int i = 0; i < child_count; i++) {
            if (children[i].name == n) return &children[i];
        }
        return nullptr;
    }
};

struct CaptureMetadata {
    bool success = false;
    size_t bytes_consumed = 0;
    FieldCapture* root = nullptr;

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
                   CaptureType type, Value* computed_value = nullptr) {
        fields_.push_back({name, start, end, type, nullptr, 0, -1, computed_value});
    }

    void add_computed(const char* name, Value* value, size_t pos) {
        fields_.push_back({name, pos, pos, CaptureType::Computed, nullptr, 0, -1, value});
    }

    Mark mark() const {
        return {static_cast<int>(scopes_.size()),
                fields_.size(),
                child_buf_.size()};
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

    CaptureMetadata finish(ParseArena& arena, bool success, size_t bytes_consumed,
                           const char* error_message = nullptr,
                           size_t error_offset = 0) {
        CaptureMetadata meta;
        meta.success = success;
        meta.bytes_consumed = bytes_consumed;
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
            meta.root = root;
        }

        return meta;
    }

    const std::vector<FieldCapture>& fields() const { return fields_; }

    // Mid-parse field lookup — search current scope for a named field.
    // Valid during eval() when fields_/child_buf_ are stable.
    const FieldCapture* find_field(const char* name) const {
        size_t start = scopes_.empty() ? 0 : scopes_.back().first_child_index;
        for (size_t i = fields_.size(); i > start; --i) {
            if (fields_[i - 1].name == name) return &fields_[i - 1];
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
    }

private:
    struct Scope {
        const char* name;
        size_t start_pos;
        size_t first_child_index;
        CaptureType type;
    };

    void begin_scope(const char* name, size_t pos, CaptureType type) {
        scopes_.push_back({name, pos, fields_.size(), type});
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
            }
        }
        return dst;
    }

    std::vector<FieldCapture> fields_;
    std::vector<Scope> scopes_;
    std::vector<FieldCapture> child_buf_;   // Temporary storage for struct children
};

} // namespace bbq::cek
