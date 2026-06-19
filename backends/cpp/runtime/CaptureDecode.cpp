#include "CaptureDecode.h"

namespace bbq {

bool decode_int(const FieldCapture* cap, const uint8_t* buf,
                int64_t* bits, bool* is_signed) {
    size_t o = cap->start_offset;
    switch (cap->type) {
        case CaptureType::UInt8:    *bits = (int64_t)(uint64_t)bbq_rd_u8(buf, o);    *is_signed = false; return true;
        case CaptureType::UInt16LE: *bits = (int64_t)(uint64_t)bbq_rd_u16le(buf, o); *is_signed = false; return true;
        case CaptureType::UInt16BE: *bits = (int64_t)(uint64_t)bbq_rd_u16be(buf, o); *is_signed = false; return true;
        case CaptureType::UInt32LE: *bits = (int64_t)(uint64_t)bbq_rd_u32le(buf, o); *is_signed = false; return true;
        case CaptureType::UInt32BE: *bits = (int64_t)(uint64_t)bbq_rd_u32be(buf, o); *is_signed = false; return true;
        case CaptureType::UInt64LE: *bits = (int64_t)bbq_rd_u64le(buf, o);           *is_signed = false; return true;
        case CaptureType::UInt64BE: *bits = (int64_t)bbq_rd_u64be(buf, o);           *is_signed = false; return true;
        case CaptureType::Int8:     *bits = (int8_t)bbq_rd_u8(buf, o);    *is_signed = true; return true;
        case CaptureType::Int16LE:  *bits = (int16_t)bbq_rd_u16le(buf, o); *is_signed = true; return true;
        case CaptureType::Int16BE:  *bits = (int16_t)bbq_rd_u16be(buf, o); *is_signed = true; return true;
        case CaptureType::Int32LE:  *bits = (int32_t)bbq_rd_u32le(buf, o); *is_signed = true; return true;
        case CaptureType::Int32BE:  *bits = (int32_t)bbq_rd_u32be(buf, o); *is_signed = true; return true;
        case CaptureType::Int64LE:  *bits = (int64_t)bbq_rd_u64le(buf, o); *is_signed = true; return true;
        case CaptureType::Int64BE:  *bits = (int64_t)bbq_rd_u64be(buf, o); *is_signed = true; return true;
        case CaptureType::Bool:     *bits = buf[o] ? 1 : 0; *is_signed = true; return true;
        case CaptureType::Computed: {
            // computed_value is a neutral tagged scalar. Int/Bool → int,
            // Float → truncated int (the int-typed path). null → 0.
            *is_signed = true;
            if (!cap->computed_value) { *bits = 0; return true; }
            switch (cap->computed_value->kind) {
                case ComputedValue::Kind::Int:
                    *bits = cap->computed_value->i; return true;
                case ComputedValue::Kind::Bool:
                    *bits = cap->computed_value->b ? 1 : 0; return true;
                case ComputedValue::Kind::Float:
                    *bits = static_cast<int64_t>(cap->computed_value->f); return true;
                default:
                    return false;
            }
        }
        default:
            return false;
    }
}

bool encode_int(uint8_t* buf, size_t off, CaptureType type, int64_t v) {
    switch (type) {
        case CaptureType::UInt8:    case CaptureType::Int8:    bbq_wr_u8(buf, off, (uint8_t)v);   return true;
        case CaptureType::UInt16LE: case CaptureType::Int16LE: bbq_wr_u16le(buf, off, (uint16_t)v); return true;
        case CaptureType::UInt16BE: case CaptureType::Int16BE: bbq_wr_u16be(buf, off, (uint16_t)v); return true;
        case CaptureType::UInt32LE: case CaptureType::Int32LE: bbq_wr_u32le(buf, off, (uint32_t)v); return true;
        case CaptureType::UInt32BE: case CaptureType::Int32BE: bbq_wr_u32be(buf, off, (uint32_t)v); return true;
        case CaptureType::UInt64LE: case CaptureType::Int64LE: bbq_wr_u64le(buf, off, (uint64_t)v); return true;
        case CaptureType::UInt64BE: case CaptureType::Int64BE: bbq_wr_u64be(buf, off, (uint64_t)v); return true;
        case CaptureType::Bool:     bbq_wr_u8(buf, off, v ? 1 : 0); return true;
        default:                    return false;
    }
}

bool decode_float(const FieldCapture* cap, const uint8_t* buf, double* out) {
    size_t o = cap->start_offset;
    switch (cap->type) {
        case CaptureType::Float32LE: *out = bbq_rd_f32le(buf, o); return true;
        case CaptureType::Float32BE: *out = bbq_rd_f32be(buf, o); return true;
        case CaptureType::Float64LE: *out = bbq_rd_f64le(buf, o); return true;
        case CaptureType::Float64BE: *out = bbq_rd_f64be(buf, o); return true;
        default:
            return false;
    }
}

}  // namespace bbq
