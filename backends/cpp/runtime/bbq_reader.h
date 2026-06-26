#pragma once
//
// bbq_reader — the C++ parse context for the ZCow (view) reader.
//
// The generated C++ view parser is the compiled CEK that BUILDS the index: each
// kont becomes a step that advances the cursor and records a span into the
// CaptureBuilder (the same FieldCapture index the CEK machine builds and the
// generated handle classes read). This context is the self-contained analogue of
// the C `bbq_ctx` / the CEK machine's parse state — cursor + interval window +
// endian register + the capture builder — living entirely in the C++ index
// runtime so the view parser depends only on backends/cpp/runtime (no C runtime,
// no bbq::cek). It grows per feature (loops/checkpoints land with arrays/optional).
//
#include "Capture.h"        // bbq::CaptureBuilder / CaptureType / CaptureMetadata
#include "CaptureDecode.h"  // bbq::decode_int (count decode-on-access)
#include "ParseArena.h"     // bbq::ParseArena
#include "bbq_machine.h"    // bbq::machine — the shared endian register + loop stack
#include "bbq_node.h"       // bbq::bytes_view — the non-scalar field_ref span

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace bbq {

// The reader confines an input cursor over the shared machine state, building the index.
struct reader : machine {
    const uint8_t* data = nullptr;
    size_t length = 0;
    size_t pos = 0;

    // Interval (window) stack — IPG confinement is bidirectional, so both edges
    // are tracked (mirrors the CEK's interval_starts/ends + effective_start/end).
    std::vector<size_t> interval_starts;
    std::vector<size_t> interval_ends;

    CaptureBuilder builder;          // builds the FieldCapture index
    ParseArena* arena = nullptr;     // for computed-value allocation during parse
    const char* error = nullptr;

    reader(const uint8_t* d, size_t n, ParseArena& a) : data(d), length(n), arena(&a) {}

    size_t effective_end() const {
        return interval_ends.empty() ? length : interval_ends.back();
    }
    size_t effective_start() const {
        return interval_starts.empty() ? 0 : interval_starts.back();
    }
    // @-builtins (the view spellings the ViewProfile maps to).
    size_t remaining() const { size_t e = effective_end(); return pos < e ? e - pos : 0; }
    bool at_end() const { return pos >= effective_end(); }

    bool fail(const char* msg) { if (!error) error = msg; return false; }

    // Advance the cursor by n bytes, bounded by the active window.
    bool advance(size_t n) {
        if (pos + n > effective_end()) return fail("unexpected end of input");
        pos += n;
        return true;
    }

    // Read an unsigned integer of `width` bytes at `offset` (no advance) — for a
    // bitfield container, whose entries are computed (shift/mask) at parse. native
    // resolves against the live endian register.
    uint64_t read_at(size_t offset, int width, bool be, bool native) const {
        bool b = native ? !little_endian : be;
        uint64_t v = 0;
        if (b) for (int i = 0; i < width; i++) v = (v << 8) | data[offset + i];
        else   for (int i = 0; i < width; i++) v |= (uint64_t)data[offset + i] << (8 * i);
        return v;
    }

    // Resolve the capture type of an N-byte little/big-endian integer at read time.
    // `native` prims follow the live endian register; suffixed ones bake be/le.
    CaptureType int_capture(int width, bool sgn, bool be, bool native) const {
        bool b = native ? !little_endian : be;
        switch (width) {
            case 8:  return sgn ? CaptureType::Int8 : CaptureType::UInt8;
            case 16: return sgn ? (b ? CaptureType::Int16BE : CaptureType::Int16LE)
                                : (b ? CaptureType::UInt16BE : CaptureType::UInt16LE);
            case 32: return sgn ? (b ? CaptureType::Int32BE : CaptureType::Int32LE)
                                : (b ? CaptureType::UInt32BE : CaptureType::UInt32LE);
            default: return sgn ? (b ? CaptureType::Int64BE : CaptureType::Int64LE)
                                : (b ? CaptureType::UInt64BE : CaptureType::UInt64LE);
        }
    }
    CaptureType float_capture(int width, bool be, bool native) const {
        bool b = native ? !little_endian : be;
        if (width == 64) return b ? CaptureType::Float64BE : CaptureType::Float64LE;
        return b ? CaptureType::Float32BE : CaptureType::Float32LE;
    }

    // ── Interval (window) confinement — bidirectional IPG, mirroring the C ctx /
    // the CEK PushIntervalApplyKont. push: [pos, end) must fall within the enclosing
    // window; pop_checked: the cursor must have consumed exactly to the window end. ──
    bool seek_checked(size_t p) {
        if (p > length) return fail("seek out of range");
        pos = p;
        return true;
    }
    bool push_interval_checked(size_t end) {
        size_t start = pos;
        if (start > end || start < effective_start() || end > effective_end())
            return fail("interval out of range");
        interval_starts.push_back(start);
        interval_ends.push_back(end);
        return true;
    }
    void pop_interval() {
        if (!interval_starts.empty()) { interval_starts.pop_back(); interval_ends.pop_back(); }
    }
    bool pop_interval_checked() {
        if (interval_starts.empty()) return fail("interval: unbalanced pop");
        size_t end = interval_ends.back();
        interval_starts.pop_back(); interval_ends.pop_back();
        if (pos != end) return fail("interval: size mismatch");
        return true;
    }

    // ── Array scopes (the loop stack itself is bbq::machine) ──
    void begin_array(const char* name) { builder.begin_array(name, pos); }
    void end_array() { builder.end_array(pos); }

    // Record a computed field (compute(...)/bitfield entry): the evaluated value,
    // arena-owned, decoded on access via node_computed_int.
    void add_computed_int(const char* name, int64_t v) {
        auto* cv = arena->alloc<ComputedValue>();
        cv->kind = ComputedValue::Kind::Int;
        cv->i = v;
        builder.add_computed(name, cv, pos);
    }

    // Record a computed field at the C++ expression's NATURAL type — bool/float/int — so
    // the stored ComputedValue kind matches the CEK (which records the runtime value's
    // kind), not a flattened int. The C/C++ type of the expression is the standard.
    template <class T>
    void add_computed_val(const char* name, T v) {
        auto* cv = arena->alloc<ComputedValue>();
        if constexpr (std::is_same_v<T, bool>)          { cv->kind = ComputedValue::Kind::Bool;  cv->b = v; }
        else if constexpr (std::is_floating_point_v<T>) { cv->kind = ComputedValue::Kind::Float; cv->f = (double)v; }
        else                                            { cv->kind = ComputedValue::Kind::Int;   cv->i = (int64_t)v; }
        builder.add_computed(name, cv, pos);
    }

    // Record a Computed int over an explicit span [start, pos) — the varint readers'
    // capture (the span is the consumed LEB bytes, not a zero-width point).
    void record_computed(const char* name, int64_t v, size_t start) {
        auto* cv = arena->alloc<ComputedValue>();
        cv->kind = ComputedValue::Kind::Int;
        cv->i = v;
        builder.add_computed(name, cv, start, pos);
    }

    // ── LEB128 varints (uleb128 / sleb64) — variable-length, so the byte span is
    // data-dependent: decode at the cursor, advance by the bytes actually consumed,
    // and record a Computed capture over the real span (a fixed span can't be
    // re-decoded as a varint). Byte-exact with the CEK's decode_uleb128/sleb128. ──
    bool uleb_capture(const char* name, int bits) {
        size_t start = pos, end = effective_end();
        uint64_t result = 0; int shift = 0;
        int maxbytes = (bits + 6) / 7;
        for (int i = 0; i < maxbytes; i++) {
            if (start + (size_t)i >= end) return fail("invalid uleb128");      // truncated
            uint8_t byte = data[start + i];
            if (i == maxbytes - 1) {
                if (byte & 0x80) return fail("invalid uleb128");               // too long
                int avail = bits - shift;
                if (avail < 7 && (byte >> avail)) return fail("invalid uleb128"); // too large
            }
            result |= (uint64_t)(byte & 0x7f) << shift;
            if (!(byte & 0x80)) { pos = start + (size_t)i + 1;
                                  record_computed(name, (int64_t)result, start); return true; }
            shift += 7;
        }
        return fail("invalid uleb128");                                        // unterminated
    }
    bool sleb_capture(const char* name, int bits) {
        size_t start = pos, end = effective_end();
        int64_t result = 0; int shift = 0;
        int maxbytes = (bits + 6) / 7;
        for (int i = 0; i < maxbytes; i++) {
            if (start + (size_t)i >= end) return fail("invalid sleb128");
            uint8_t byte = data[start + i];
            if (i == maxbytes - 1 && (byte & 0x80)) return fail("invalid sleb128");
            result |= (int64_t)(byte & 0x7f) << shift;
            shift += 7;
            if (!(byte & 0x80)) {
                if (shift < bits && (byte & 0x40)) result |= -((int64_t)1 << shift);  // sign-extend
                pos = start + (size_t)i + 1;
                record_computed(name, result, start);
                return true;
            }
        }
        return fail("invalid sleb128");
    }

    // ── Backtrack checkpoint (bare optional / resync): snapshots the full parse
    // state — cursor, interval window, endian, loop depth, and the builder mark — so
    // a failed try rewinds cleanly, dropping any partial captures it recorded. ──
    struct checkpoint {
        size_t pos;
        size_t interval_depth;
        size_t loop_depth;
        bool little_endian;
        CaptureBuilder::Mark mark;
    };
    checkpoint save() {
        return { pos, interval_starts.size(), loop_index_stack.size(),
                 little_endian, builder.mark() };
    }
    void restore(const checkpoint& c) {
        pos = c.pos;
        little_endian = c.little_endian;
        interval_starts.resize(c.interval_depth);
        interval_ends.resize(c.interval_depth);
        loop_index_stack.resize(c.loop_depth);
        loop_limit_stack.resize(c.loop_depth);
        builder.restore(c.mark);
    }

    CaptureMetadata finish(bool ok) {
        return builder.finish(*arena, ok, pos, error);
    }
};

// Decode a prior field in the current scope as an integer — the view's decode-on-
// access for expressions (e.g. an array count `[n]`). The generated parser spells
// field references as bbq_view_i64(r, "name") (the ViewProfile field_ref hook).
inline int64_t bbq_view_i64(reader& r, const char* name) {
    const FieldCapture* c = r.builder.find_field_str(name);
    int64_t bits = 0; bool sgn = false;
    if (c) decode_int(c, r.data, &bits, &sgn);
    return bits;
}

// A field reference to a NON-SCALAR (Bytes/String/External) capture as its
// [start,end) span — the ViewProfile field_ref_bytes hook (dual of bbq_view_i64).
inline bytes_view bbq_view_bytes(reader& r, const char* name) {
    const FieldCapture* c = r.builder.find_field_str(name);
    if (!c) return {};
    return { r.data + c->start_offset, (size_t)(c->end_offset - c->start_offset) };
}

}  // namespace bbq
