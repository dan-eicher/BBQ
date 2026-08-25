#pragma once
//
// bbq_node — the spellings the generated parser emits for reading back its own captures.
//
// A parsed value is a node in the document (CaptureCow.h), and the accessors here are
// thin names over that: the generated reader looks up an array's count before its
// elements, mid-parse, and this keeps what it emits the same shape as what a consumer
// would write by hand. There is one node type and one place a value comes from.
//
#include "Capture.h"        // bbq::CaptureType
#include "CaptureDecode.h"  // inline endian atoms (bbq_rd_u32le, ...)
#include "CaptureCow.h"     // bbq::zcow — the document

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

// Decode a scalar through its RECORDED type (not a static atom), so it follows a
// runtime-resolved endian — e.g. an unsuffixed prim after a mid-struct `@endian`
// switch. The same decode the CEK does.
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

}  // namespace bbq
