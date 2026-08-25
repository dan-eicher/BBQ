#include "CaptureDecode.h"

namespace bbq {

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

}  // namespace bbq
