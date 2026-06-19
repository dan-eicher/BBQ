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

// A typed sequence over an array index node: element handle T built per child,
// sharing the CoW store so element mutations are seen through the parent.
template <class T>
struct seq {
    const FieldCapture* n = nullptr;
    const uint8_t* buf = nullptr;
    zcow* z = nullptr;

    size_t size() const { return n ? (size_t)n->child_count : 0; }
    bool empty() const { return size() == 0; }
    // Brace-init so the element T composes uniformly: a handle class (its
    // (n,buf,z) ctor), or a nested container (seq/opt, aggregate {n,buf,z}).
    T operator[](size_t i) const { return T{&n->children[i], buf, z}; }

    struct iterator {
        const FieldCapture* n; const uint8_t* buf; zcow* z; size_t i;
        T operator*() const { return T{&n->children[i], buf, z}; }
        iterator& operator++() { ++i; return *this; }
        bool operator!=(const iterator& o) const { return i != o.i; }
    };
    iterator begin() const { return {n, buf, z, 0}; }
    iterator end() const { return {n, buf, z, size()}; }
};

// A typed sequence over an array of bytes/string elements: each child is a span;
// the view analogue of prim_seq. V is bytes_view/std::string_view, C its element.
template <class V, class C>
struct span_seq {
    const FieldCapture* n = nullptr;
    const uint8_t* buf = nullptr;
    zcow* z = nullptr;

    size_t size() const { return n ? (size_t)n->child_count : 0; }
    bool empty() const { return size() == 0; }
    V at(size_t i) const {
        const FieldCapture* c = &n->children[i];
        if (z) { if (const std::vector<uint8_t>* o = z->get_bytes(c))
                     return V{ (const C*)o->data(), o->size() }; }
        return V{ (const C*)(buf + c->start_offset),
                  (size_t)(c->end_offset - c->start_offset) };
    }
    V operator[](size_t i) const { return at(i); }

    struct iterator {
        const span_seq* s; size_t i;
        V operator*() const { return s->at(i); }
        iterator& operator++() { ++i; return *this; }
        bool operator!=(const iterator& o) const { return i != o.i; }
    };
    iterator begin() const { return {this, 0}; }
    iterator end() const { return {this, size()}; }
};

// A typed sequence over an array of PRIMITIVE elements: each child decodes to a
// scalar T (lazily, or the CoW int override) through the shared decoders — the
// element analogue of the scalar field accessors. Distinct from seq<T> (which
// constructs a handle class per child) because a primitive has no handle ctor.
template <class T>
struct prim_seq {
    const FieldCapture* n = nullptr;
    const uint8_t* buf = nullptr;
    zcow* z = nullptr;

    size_t size() const { return n ? (size_t)n->child_count : 0; }
    bool empty() const { return size() == 0; }

    T at(size_t i) const {
        const FieldCapture* c = &n->children[i];
        if constexpr (std::is_floating_point_v<T>) {
            double d = 0; decode_float(c, buf, &d); return (T)d;
        } else {
            if (z) { if (const int64_t* o = z->get_int(c)) return (T)*o; }
            int64_t bits = 0; bool sgn = false; decode_int(c, buf, &bits, &sgn);
            return (T)bits;
        }
    }
    T operator[](size_t i) const { return at(i); }

    struct iterator {
        const prim_seq* s; size_t i;
        T operator*() const { return s->at(i); }
        iterator& operator++() { ++i; return *this; }
        bool operator!=(const iterator& o) const { return i != o.i; }
    };
    iterator begin() const { return {this, 0}; }
    iterator end() const { return {this, size()}; }
};

// An optional ruleref/struct value: present iff the index holds a child for it
// (absent optionals leave no child). value() is only valid when has_value().
template <class T>
struct opt {
    const FieldCapture* n = nullptr;  // null ⇒ absent
    const uint8_t* buf = nullptr;
    zcow* z = nullptr;

    bool has_value() const { return n != nullptr; }
    explicit operator bool() const { return n != nullptr; }
    T value() const { return T{n, buf, z}; }   // handle ctor or nested container
};

// An optional PRIMITIVE value: present iff the index holds a child; value()
// decodes the scalar (or its CoW override) like prim_seq's element.
template <class T>
struct prim_opt {
    const FieldCapture* n = nullptr;  // null ⇒ absent
    const uint8_t* buf = nullptr;
    zcow* z = nullptr;

    bool has_value() const { return n != nullptr; }
    explicit operator bool() const { return n != nullptr; }
    T value() const {
        if constexpr (std::is_floating_point_v<T>) {
            double d = 0; decode_float(n, buf, &d); return (T)d;
        } else {
            if (z) { if (const int64_t* o = z->get_int(n)) return (T)*o; }
            int64_t bits = 0; bool sgn = false; decode_int(n, buf, &bits, &sgn);
            return (T)bits;
        }
    }
};

// An optional bytes/string span: present iff the index holds a child; value()
// returns the borrowed span (or its CoW override). V is bytes_view or
// std::string_view; C is the byte/char element type.
template <class V, class C>
struct span_opt {
    const FieldCapture* n = nullptr;  // null ⇒ absent
    const uint8_t* buf = nullptr;
    zcow* z = nullptr;

    bool has_value() const { return n != nullptr; }
    explicit operator bool() const { return n != nullptr; }
    V value() const {
        if (z) { if (const std::vector<uint8_t>* o = z->get_bytes(n))
                     return V{ (const C*)o->data(), o->size() }; }
        return V{ (const C*)(buf + n->start_offset),
                  (size_t)(n->end_offset - n->start_offset) };
    }
};

}  // namespace bbq
