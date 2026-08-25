#pragma once
//
// CaptureDecode — the byte-level atoms: an offset in a buffer -> a C++ scalar, and back.
//
// The single home for the endian/width logic. Everything above it — the document's own
// decoders (CaptureCow.h), the generated readers, the C runtime's twin — goes through
// these, so no two faces of the same bytes can drift.
//
#include "Capture.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bbq {

// ── Endian-typed atoms over a raw buffer ──
// The byte-level readers the dispatch below and the generated accessors share.
inline uint8_t  bbq_rd_u8 (const uint8_t* b, size_t o) { return b[o]; }
inline uint16_t bbq_rd_u16le(const uint8_t* b, size_t o) {
    return (uint16_t)((uint16_t)b[o] | ((uint16_t)b[o + 1] << 8));
}
inline uint16_t bbq_rd_u16be(const uint8_t* b, size_t o) {
    return (uint16_t)(((uint16_t)b[o] << 8) | (uint16_t)b[o + 1]);
}
inline uint32_t bbq_rd_u32le(const uint8_t* b, size_t o) {
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
           ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}
inline uint32_t bbq_rd_u32be(const uint8_t* b, size_t o) {
    return ((uint32_t)b[o] << 24) | ((uint32_t)b[o + 1] << 16) |
           ((uint32_t)b[o + 2] << 8) | (uint32_t)b[o + 3];
}
inline uint64_t bbq_rd_u64le(const uint8_t* b, size_t o) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[o + i] << (i * 8);
    return v;
}
inline uint64_t bbq_rd_u64be(const uint8_t* b, size_t o) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[o + i] << ((7 - i) * 8);
    return v;
}
inline float bbq_rd_f32le(const uint8_t* b, size_t o) {
    uint32_t r = bbq_rd_u32le(b, o); float f; std::memcpy(&f, &r, 4); return f;
}
inline float bbq_rd_f32be(const uint8_t* b, size_t o) {
    uint32_t r = bbq_rd_u32be(b, o); float f; std::memcpy(&f, &r, 4); return f;
}
inline double bbq_rd_f64le(const uint8_t* b, size_t o) {
    uint64_t r = bbq_rd_u64le(b, o); double d; std::memcpy(&d, &r, 8); return d;
}
inline double bbq_rd_f64be(const uint8_t* b, size_t o) {
    uint64_t r = bbq_rd_u64be(b, o); double d; std::memcpy(&d, &r, 8); return d;
}

// ── Endian-typed write atoms (the inverse of the readers) ──
// Patch a value into a buffer at an offset, for emit()/the writer.
inline void bbq_wr_u8 (uint8_t* b, size_t o, uint8_t v)  { b[o] = v; }
inline void bbq_wr_u16le(uint8_t* b, size_t o, uint16_t v) { b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v >> 8); }
inline void bbq_wr_u16be(uint8_t* b, size_t o, uint16_t v) { b[o] = (uint8_t)(v >> 8); b[o+1] = (uint8_t)v; }
inline void bbq_wr_u32le(uint8_t* b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o+i] = (uint8_t)(v >> (i*8)); }
inline void bbq_wr_u32be(uint8_t* b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o+i] = (uint8_t)(v >> ((3-i)*8)); }
inline void bbq_wr_u64le(uint8_t* b, size_t o, uint64_t v) { for (int i = 0; i < 8; i++) b[o+i] = (uint8_t)(v >> (i*8)); }
inline void bbq_wr_u64be(uint8_t* b, size_t o, uint64_t v) { for (int i = 0; i < 8; i++) b[o+i] = (uint8_t)(v >> ((7-i)*8)); }

// Encode an integer-typed leaf (per CaptureType) into `buf` at `off`. The
// inverse of decode_int for fixed-width types; the writer's in-place patch.
// Returns false for non-fixed-width-integer types (Computed/float/bytes).
bool encode_int(uint8_t* buf, size_t off, CaptureType type, int64_t v);

// True for the float leaf types (Float32/64 LE/BE).
inline bool is_float_type(CaptureType t) {
    return t == CaptureType::Float32LE || t == CaptureType::Float32BE ||
           t == CaptureType::Float64LE || t == CaptureType::Float64BE;
}

// Encode a float-typed leaf into `buf` at `off` — the inverse of decode_float.
inline void encode_float(uint8_t* buf, size_t off, CaptureType type, double v) {
    switch (type) {
    case CaptureType::Float32LE: { float f = (float)v; uint32_t b; std::memcpy(&b, &f, 4); bbq_wr_u32le(buf, off, b); break; }
    case CaptureType::Float32BE: { float f = (float)v; uint32_t b; std::memcpy(&b, &f, 4); bbq_wr_u32be(buf, off, b); break; }
    case CaptureType::Float64LE: { uint64_t b; std::memcpy(&b, &v, 8); bbq_wr_u64le(buf, off, b); break; }
    case CaptureType::Float64BE: { uint64_t b; std::memcpy(&b, &v, 8); bbq_wr_u64be(buf, off, b); break; }
    default: break;
    }
}

// ── Borrowed-leaf decode dispatch (the dynamic face) ──

}  // namespace bbq
