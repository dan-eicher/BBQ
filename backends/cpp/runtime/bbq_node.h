#pragma once
//
// bbq_node — the C++ view runtime over the shared zero-copy index.
//
// A parsed value is a node in the FieldCapture index (offsets into the input
// buffer — the same index the CEK Machine builds; the index runtime is the
// shared lib in this directory, depended on by the machine and generated code
// alike). The generated typed handle classes are a typed face over that index:
// each field accessor finds its child node and decodes it lazily through the
// shared decoders (CaptureDecode.h). Read-only / Borrowed for now; the Owned +
// copy-on-write + emit() faces land with their consumers.
//
#include "Capture.h"        // bbq::FieldCapture / CaptureType — the index
#include "CaptureDecode.h"  // inline endian atoms (bbq_rd_u32le, ...)
#include "CaptureCow.h"     // bbq::zcow — the shared copy-on-write overlay

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bbq {

// A borrowed span of bytes/utf-8 in the input buffer (zero-copy).
struct bytes_view {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// Find a struct child by field name. Names in the index are interned C strings;
// generated accessors pass string literals, so compare by content.
inline const FieldCapture* node_child(const FieldCapture* n, const char* name) {
    if (!n || !n->children) return nullptr;
    for (int i = 0; i < n->child_count; i++) {
        const char* cn = n->children[i].name;
        if (cn && std::string_view(cn) == name) return &n->children[i];
    }
    return nullptr;
}

// Computed-field value (compute(...)/leb/bitfield): the parser stored a typed
// Value on the Computed capture; read it as an integer.
inline int64_t node_computed_int(const FieldCapture* c) {
    int64_t bits = 0; bool sgn = false;
    decode_int(c, nullptr, &bits, &sgn);  // Computed path ignores the buffer
    return bits;
}

// Read a computed value at its REAL stored kind, converted to T — so a float/bool
// compute isn't flattened through the int path (the dual of reader::add_computed_val).
template <class T>
inline T node_computed_val(const FieldCapture* c) {
    const ComputedValue* cv = c ? c->computed_value : nullptr;
    if (!cv) return T{};
    switch (cv->kind) {
    case ComputedValue::Kind::Float: return static_cast<T>(cv->f);
    case ComputedValue::Kind::Bool:  return static_cast<T>(cv->b);
    default:                         return static_cast<T>(cv->i);
    }
}

// Decode a scalar field through its RECORDED capture type (not a static atom), so
// it follows a runtime-resolved endian (e.g. an unsuffixed prim after a mid-struct
// `@endian` switch) — the same decode the CEK/seq use.
inline int64_t node_int(const FieldCapture* c, const uint8_t* buf) {
    int64_t bits = 0; bool sgn = false;
    decode_int(c, buf, &bits, &sgn);
    return bits;
}
inline double node_float(const FieldCapture* c, const uint8_t* buf) {
    double d = 0; decode_float(c, buf, &d);
    return d;
}

// The same three over a document node. The generated parser reads back its own
// captures mid-parse — an array's count before its elements — and what it holds by
// then is a zcow::node, so these keep the emitted spelling the same either side.
inline const zcow::node* node_child(const zcow::node* n, const char* name) {
    return n ? zcow::child_named(n, name) : nullptr;
}
inline int64_t node_int(const zcow::node* c, const uint8_t* buf) {
    if (!c) return 0;
    if (c->type == CaptureType::Computed) {
        int64_t v = 0;
        return zcow::computed_int(c, &v) ? v : 0;
    }
    int64_t bits = 0; bool sgn = false;
    zcow::decode_int(c, buf, &bits, &sgn);
    return bits;
}
// A computed value needs no buffer: it is carried, not read back out of a span.
inline int64_t node_computed_int(const zcow::node* c) {
    int64_t v = 0;
    return (c && zcow::computed_int(c, &v)) ? v : 0;
}
inline double node_float(const zcow::node* c, const uint8_t* buf) {
    if (!c) return 0;
    double d = 0;
    zcow::decode_float(c, buf, &d);
    return d;
}

// The typed containers now live in CaptureCow.h, over a cursor rather than over
// (node, buffer, overlay). Keeping a second set here that read size() from one place
// and elements from another is what let a view report contents it would never
// serialize.

}  // namespace bbq
