/* bbq_runtime.h — C runtime for BBQ-generated readers and writers
 *
 * Header-only. Provides read/write contexts and primitive I/O helpers.
 * This file ships as-is — it is NOT generated.
 */
#ifndef BBQ_RUNTIME_H
#define BBQ_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ── Zero-copy byte/string types ────────────────────────── */

typedef struct {
    const uint8_t* data;
    size_t length;
} bbq_bytes_t;

typedef struct {
    const char* data;
    size_t length;
} bbq_string_t;

/* ── Byte swapping ──────────────────────────────────────── */

static inline uint16_t bbq_bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t bbq_bswap32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
#endif
}

static inline uint64_t bbq_bswap64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);
#else
    return ((v >> 56)) | ((v >> 40) & 0xFF00ULL) |
           ((v >> 24) & 0xFF0000ULL) | ((v >> 8) & 0xFF000000ULL) |
           ((v << 8) & 0xFF00000000ULL) | ((v << 24) & 0xFF0000000000ULL) |
           ((v << 40) & 0xFF000000000000ULL) | ((v << 56));
#endif
}

static inline bool bbq_host_is_little_endian(void) {
    uint16_t x = 1;
    return *(uint8_t*)&x == 1;
}

/* ── Read context ───────────────────────────────────────── */

#define BBQ_MAX_SCOPES 16
#define BBQ_MAX_LOOPS  8
#define BBQ_ERROR_LEN  256

typedef struct {
    const uint8_t* data;
    size_t length;
    size_t pos;
    bool little_endian;
    /* Error */
    char error[BBQ_ERROR_LEN];
    bool has_error;
    /* Scope stack (for cross-rule references) */
    void* scopes[BBQ_MAX_SCOPES];
    int scope_depth;
    /* Loop index stack */
    int64_t loop_indices[BBQ_MAX_LOOPS];
    int loop_depth;
    /* Interval stack */
    size_t interval_ends[BBQ_MAX_SCOPES];
    int interval_depth;
} bbq_ctx_t;

static inline void bbq_ctx_init(bbq_ctx_t* ctx, const uint8_t* data, size_t len) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->data = data;
    ctx->length = len;
}

static inline size_t bbq_pos(const bbq_ctx_t* ctx) { return ctx->pos; }
static inline size_t bbq_total_size(const bbq_ctx_t* ctx) { return ctx->length; }
static inline bool bbq_at_end(const bbq_ctx_t* ctx) { return ctx->pos >= ctx->length; }

static inline size_t bbq_effective_end(const bbq_ctx_t* ctx) {
    if (ctx->interval_depth > 0) return ctx->interval_ends[ctx->interval_depth - 1];
    return ctx->length;
}

static inline size_t bbq_remaining(const bbq_ctx_t* ctx) {
    size_t end = bbq_effective_end(ctx);
    return ctx->pos < end ? end - ctx->pos : 0;
}

static inline bool bbq_fail(bbq_ctx_t* ctx, const char* msg) {
    if (!ctx->has_error) {
        snprintf(ctx->error, BBQ_ERROR_LEN, "offset %zu: %s", ctx->pos, msg);
        ctx->has_error = true;
    }
    return false;
}

/* Checkpoints (for backtracking) */
typedef struct { size_t pos; } bbq_checkpoint_t;

static inline bbq_checkpoint_t bbq_save(const bbq_ctx_t* ctx) {
    bbq_checkpoint_t cp = { ctx->pos };
    return cp;
}

static inline void bbq_restore(bbq_ctx_t* ctx, bbq_checkpoint_t cp) {
    ctx->pos = cp.pos;
    ctx->has_error = false;
    ctx->error[0] = '\0';
}

/* Scope stack */
static inline void bbq_push_scope(bbq_ctx_t* ctx, void* ptr) {
    if (ctx->scope_depth < BBQ_MAX_SCOPES)
        ctx->scopes[ctx->scope_depth++] = ptr;
}

static inline void bbq_pop_scope(bbq_ctx_t* ctx) {
    if (ctx->scope_depth > 0) ctx->scope_depth--;
}

static inline void* bbq_scope_ptr(const bbq_ctx_t* ctx, int levels_up) {
    int idx = ctx->scope_depth - 1 - levels_up;
    return (idx >= 0) ? ctx->scopes[idx] : NULL;
}

/* Loop index stack */
static inline void bbq_push_loop(bbq_ctx_t* ctx) {
    if (ctx->loop_depth < BBQ_MAX_LOOPS)
        ctx->loop_indices[ctx->loop_depth++] = 0;
}

static inline void bbq_pop_loop(bbq_ctx_t* ctx) {
    if (ctx->loop_depth > 0) ctx->loop_depth--;
}

static inline void bbq_set_loop_index(bbq_ctx_t* ctx, int64_t idx) {
    if (ctx->loop_depth > 0)
        ctx->loop_indices[ctx->loop_depth - 1] = idx;
}

static inline int64_t bbq_loop_index(const bbq_ctx_t* ctx) {
    return (ctx->loop_depth > 0) ? ctx->loop_indices[ctx->loop_depth - 1] : 0;
}

/* Interval stack */
static inline void bbq_push_interval(bbq_ctx_t* ctx, size_t end) {
    if (ctx->interval_depth < BBQ_MAX_SCOPES)
        ctx->interval_ends[ctx->interval_depth++] = end;
}

static inline void bbq_pop_interval(bbq_ctx_t* ctx) {
    if (ctx->interval_depth > 0) ctx->interval_depth--;
}

/* Endianness */
static inline void bbq_set_endian(bbq_ctx_t* ctx, bool little) {
    ctx->little_endian = little;
}

/* Peek */
static inline bool bbq_peek(const bbq_ctx_t* ctx, uint8_t* out) {
    if (ctx->pos >= bbq_effective_end(ctx)) return false;
    *out = ctx->data[ctx->pos];
    return true;
}

/* ── Read primitives ────────────────────────────────────── */

static inline bool bbq_read_u8(bbq_ctx_t* ctx, uint8_t* out) {
    if (ctx->pos + 1 > bbq_effective_end(ctx)) return bbq_fail(ctx, "u8: overrun");
    *out = ctx->data[ctx->pos++];
    return true;
}

static inline bool bbq_read_i8(bbq_ctx_t* ctx, int8_t* out) {
    uint8_t v;
    if (!bbq_read_u8(ctx, &v)) return false;
    *out = (int8_t)v;
    return true;
}

static inline bool bbq_read_u16be(bbq_ctx_t* ctx, uint16_t* out) {
    if (ctx->pos + 2 > bbq_effective_end(ctx)) return bbq_fail(ctx, "u16be: overrun");
    uint16_t v;
    memcpy(&v, ctx->data + ctx->pos, 2);
    *out = bbq_host_is_little_endian() ? bbq_bswap16(v) : v;
    ctx->pos += 2;
    return true;
}

static inline bool bbq_read_u16le(bbq_ctx_t* ctx, uint16_t* out) {
    if (ctx->pos + 2 > bbq_effective_end(ctx)) return bbq_fail(ctx, "u16le: overrun");
    uint16_t v;
    memcpy(&v, ctx->data + ctx->pos, 2);
    *out = bbq_host_is_little_endian() ? v : bbq_bswap16(v);
    ctx->pos += 2;
    return true;
}

static inline bool bbq_read_i16be(bbq_ctx_t* ctx, int16_t* out) {
    uint16_t v; if (!bbq_read_u16be(ctx, &v)) return false;
    *out = (int16_t)v; return true;
}

static inline bool bbq_read_i16le(bbq_ctx_t* ctx, int16_t* out) {
    uint16_t v; if (!bbq_read_u16le(ctx, &v)) return false;
    *out = (int16_t)v; return true;
}

static inline bool bbq_read_u32be(bbq_ctx_t* ctx, uint32_t* out) {
    if (ctx->pos + 4 > bbq_effective_end(ctx)) return bbq_fail(ctx, "u32be: overrun");
    uint32_t v;
    memcpy(&v, ctx->data + ctx->pos, 4);
    *out = bbq_host_is_little_endian() ? bbq_bswap32(v) : v;
    ctx->pos += 4;
    return true;
}

static inline bool bbq_read_u32le(bbq_ctx_t* ctx, uint32_t* out) {
    if (ctx->pos + 4 > bbq_effective_end(ctx)) return bbq_fail(ctx, "u32le: overrun");
    uint32_t v;
    memcpy(&v, ctx->data + ctx->pos, 4);
    *out = bbq_host_is_little_endian() ? v : bbq_bswap32(v);
    ctx->pos += 4;
    return true;
}

static inline bool bbq_read_i32be(bbq_ctx_t* ctx, int32_t* out) {
    uint32_t v; if (!bbq_read_u32be(ctx, &v)) return false;
    *out = (int32_t)v; return true;
}

static inline bool bbq_read_i32le(bbq_ctx_t* ctx, int32_t* out) {
    uint32_t v; if (!bbq_read_u32le(ctx, &v)) return false;
    *out = (int32_t)v; return true;
}

static inline bool bbq_read_u64be(bbq_ctx_t* ctx, uint64_t* out) {
    if (ctx->pos + 8 > bbq_effective_end(ctx)) return bbq_fail(ctx, "u64be: overrun");
    uint64_t v;
    memcpy(&v, ctx->data + ctx->pos, 8);
    *out = bbq_host_is_little_endian() ? bbq_bswap64(v) : v;
    ctx->pos += 8;
    return true;
}

static inline bool bbq_read_u64le(bbq_ctx_t* ctx, uint64_t* out) {
    if (ctx->pos + 8 > bbq_effective_end(ctx)) return bbq_fail(ctx, "u64le: overrun");
    uint64_t v;
    memcpy(&v, ctx->data + ctx->pos, 8);
    *out = bbq_host_is_little_endian() ? v : bbq_bswap64(v);
    ctx->pos += 8;
    return true;
}

static inline bool bbq_read_i64be(bbq_ctx_t* ctx, int64_t* out) {
    uint64_t v; if (!bbq_read_u64be(ctx, &v)) return false;
    *out = (int64_t)v; return true;
}

static inline bool bbq_read_i64le(bbq_ctx_t* ctx, int64_t* out) {
    uint64_t v; if (!bbq_read_u64le(ctx, &v)) return false;
    *out = (int64_t)v; return true;
}

static inline bool bbq_read_f32be(bbq_ctx_t* ctx, float* out) {
    uint32_t v; if (!bbq_read_u32be(ctx, &v)) return false;
    memcpy(out, &v, 4); return true;
}

static inline bool bbq_read_f32le(bbq_ctx_t* ctx, float* out) {
    uint32_t v; if (!bbq_read_u32le(ctx, &v)) return false;
    memcpy(out, &v, 4); return true;
}

static inline bool bbq_read_f64be(bbq_ctx_t* ctx, double* out) {
    uint64_t v; if (!bbq_read_u64be(ctx, &v)) return false;
    memcpy(out, &v, 8); return true;
}

static inline bool bbq_read_f64le(bbq_ctx_t* ctx, double* out) {
    uint64_t v; if (!bbq_read_u64le(ctx, &v)) return false;
    memcpy(out, &v, 8); return true;
}

static inline bool bbq_read_bool(bbq_ctx_t* ctx, bool* out) {
    uint8_t v; if (!bbq_read_u8(ctx, &v)) return false;
    *out = v != 0; return true;
}

/* Zero-copy bytes/string reads */
static inline bool bbq_read_bytes(bbq_ctx_t* ctx, const uint8_t** out,
                                   size_t* out_len, size_t len) {
    if (ctx->pos + len > bbq_effective_end(ctx)) return bbq_fail(ctx, "bytes: overrun");
    *out = ctx->data + ctx->pos;
    *out_len = len;
    ctx->pos += len;
    return true;
}

static inline bool bbq_read_string(bbq_ctx_t* ctx, const char** out,
                                    size_t* out_len, size_t len) {
    if (ctx->pos + len > bbq_effective_end(ctx)) return bbq_fail(ctx, "string: overrun");
    *out = (const char*)(ctx->data + ctx->pos);
    *out_len = len;
    ctx->pos += len;
    return true;
}

/* ── Write context ──────────────────────────────────────── */

typedef struct {
    uint8_t* data;
    size_t capacity;
    size_t pos;
    bool little_endian;
} bbq_write_ctx_t;

static inline void bbq_write_ctx_init(bbq_write_ctx_t* ctx,
                                       uint8_t* buf, size_t cap) {
    ctx->data = buf;
    ctx->capacity = cap;
    ctx->pos = 0;
    ctx->little_endian = false;
}

static inline size_t bbq_write_pos(const bbq_write_ctx_t* ctx) { return ctx->pos; }

static inline void bbq_write_set_endian(bbq_write_ctx_t* ctx, bool little) {
    ctx->little_endian = little;
}

/* ── Write primitives ───────────────────────────────────── */

static inline bool bbq_write_u8(bbq_write_ctx_t* ctx, uint8_t val) {
    if (ctx->pos + 1 > ctx->capacity) return false;
    ctx->data[ctx->pos++] = val;
    return true;
}

static inline bool bbq_write_i8(bbq_write_ctx_t* ctx, int8_t val) {
    return bbq_write_u8(ctx, (uint8_t)val);
}

static inline bool bbq_write_u16be(bbq_write_ctx_t* ctx, uint16_t val) {
    if (ctx->pos + 2 > ctx->capacity) return false;
    uint16_t v = bbq_host_is_little_endian() ? bbq_bswap16(val) : val;
    memcpy(ctx->data + ctx->pos, &v, 2);
    ctx->pos += 2;
    return true;
}

static inline bool bbq_write_u16le(bbq_write_ctx_t* ctx, uint16_t val) {
    if (ctx->pos + 2 > ctx->capacity) return false;
    uint16_t v = bbq_host_is_little_endian() ? val : bbq_bswap16(val);
    memcpy(ctx->data + ctx->pos, &v, 2);
    ctx->pos += 2;
    return true;
}

static inline bool bbq_write_i16be(bbq_write_ctx_t* ctx, int16_t val) {
    return bbq_write_u16be(ctx, (uint16_t)val);
}

static inline bool bbq_write_i16le(bbq_write_ctx_t* ctx, int16_t val) {
    return bbq_write_u16le(ctx, (uint16_t)val);
}

static inline bool bbq_write_u32be(bbq_write_ctx_t* ctx, uint32_t val) {
    if (ctx->pos + 4 > ctx->capacity) return false;
    uint32_t v = bbq_host_is_little_endian() ? bbq_bswap32(val) : val;
    memcpy(ctx->data + ctx->pos, &v, 4);
    ctx->pos += 4;
    return true;
}

static inline bool bbq_write_u32le(bbq_write_ctx_t* ctx, uint32_t val) {
    if (ctx->pos + 4 > ctx->capacity) return false;
    uint32_t v = bbq_host_is_little_endian() ? val : bbq_bswap32(val);
    memcpy(ctx->data + ctx->pos, &v, 4);
    ctx->pos += 4;
    return true;
}

static inline bool bbq_write_i32be(bbq_write_ctx_t* ctx, int32_t val) {
    return bbq_write_u32be(ctx, (uint32_t)val);
}

static inline bool bbq_write_i32le(bbq_write_ctx_t* ctx, int32_t val) {
    return bbq_write_u32le(ctx, (uint32_t)val);
}

static inline bool bbq_write_u64be(bbq_write_ctx_t* ctx, uint64_t val) {
    if (ctx->pos + 8 > ctx->capacity) return false;
    uint64_t v = bbq_host_is_little_endian() ? bbq_bswap64(val) : val;
    memcpy(ctx->data + ctx->pos, &v, 8);
    ctx->pos += 8;
    return true;
}

static inline bool bbq_write_u64le(bbq_write_ctx_t* ctx, uint64_t val) {
    if (ctx->pos + 8 > ctx->capacity) return false;
    uint64_t v = bbq_host_is_little_endian() ? val : bbq_bswap64(val);
    memcpy(ctx->data + ctx->pos, &v, 8);
    ctx->pos += 8;
    return true;
}

static inline bool bbq_write_i64be(bbq_write_ctx_t* ctx, int64_t val) {
    return bbq_write_u64be(ctx, (uint64_t)val);
}

static inline bool bbq_write_i64le(bbq_write_ctx_t* ctx, int64_t val) {
    return bbq_write_u64le(ctx, (uint64_t)val);
}

static inline bool bbq_write_f32be(bbq_write_ctx_t* ctx, float val) {
    uint32_t v; memcpy(&v, &val, 4);
    return bbq_write_u32be(ctx, v);
}

static inline bool bbq_write_f32le(bbq_write_ctx_t* ctx, float val) {
    uint32_t v; memcpy(&v, &val, 4);
    return bbq_write_u32le(ctx, v);
}

static inline bool bbq_write_f64be(bbq_write_ctx_t* ctx, double val) {
    uint64_t v; memcpy(&v, &val, 8);
    return bbq_write_u64be(ctx, v);
}

static inline bool bbq_write_f64le(bbq_write_ctx_t* ctx, double val) {
    uint64_t v; memcpy(&v, &val, 8);
    return bbq_write_u64le(ctx, v);
}

static inline bool bbq_write_bool(bbq_write_ctx_t* ctx, bool val) {
    return bbq_write_u8(ctx, val ? 1 : 0);
}

static inline bool bbq_write_bytes(bbq_write_ctx_t* ctx,
                                    const uint8_t* data, size_t len) {
    if (ctx->pos + len > ctx->capacity) return false;
    memcpy(ctx->data + ctx->pos, data, len);
    ctx->pos += len;
    return true;
}

static inline bool bbq_write_string(bbq_write_ctx_t* ctx,
                                     const char* data, size_t len) {
    return bbq_write_bytes(ctx, (const uint8_t*)data, len);
}

/* ── Utility: abs for expressions ───────────────────────── */

static inline int64_t bbq_abs(int64_t v) { return v < 0 ? -v : v; }

#endif /* BBQ_RUNTIME_H */
