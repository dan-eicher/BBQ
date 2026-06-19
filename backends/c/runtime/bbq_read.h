/* bbq_read.h — shared C cursor + decoder core for BBQ-generated readers.
 *
 * The backend-neutral half of the C runtime: the input cursor (bbq_ctx_t —
 * position / interval window / loop & scope stacks / endian register /
 * checkpoint) and the full primitive decoder family (fixed-width, native-endian,
 * LEB128, float, bool). Both C backends ride this:
 *   - the OWNING reader (bbq_runtime.h) decodes into struct fields;
 *   - c-lite (bbq_lite.h) advances the cursor and records spans into an index.
 *
 * Header-only `static inline` — and this is load-bearing, not debt: the
 * copy-and-patch JIT (jitterator) requires a stencil to be self-contained
 * machine code, so a decoder a stencil calls (e.g. bbq_read_uleb128_*) must
 * INLINE into the stencil; an `extern` call would be a non-_HOLE_ relocation
 * jitterator hard-rejects. So the decoders stay inline.
 */
#ifndef BBQ_READ_H
#define BBQ_READ_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ── Byte/string field types (owning bytes copies live in bbq_runtime.h) ── */

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
    int64_t loop_limits[BBQ_MAX_LOOPS];
    /* Backing-store capacity per loop, for unbounded (eof/until) arrays whose count
     * is unknown up front and grown as elements are parsed. Counted arrays pre-size
     * and never grow (cap == limit). */
    size_t loop_caps[BBQ_MAX_LOOPS];
    int loop_depth;
    /* Per-rule loop base: the loop_depth at a rule's entry (pushed at each invoke),
     * so an array's own index is loop_indices[base + nesting_level] — its own frame,
     * mirroring the CEK's per-LoopFrame counter rather than "innermost". */
    int loop_bases[BBQ_MAX_SCOPES];
    int loop_base_depth;
    /* Interval stack. IPG confinement is bidirectional: a nested window must fall
     * entirely within its parent, so we track BOTH edges (the CEK keeps
     * interval_starts/interval_ends and effective_start/effective_end). */
    size_t interval_starts[BBQ_MAX_SCOPES];
    size_t interval_ends[BBQ_MAX_SCOPES];
    int interval_depth;
    /* The endian register is seeded from the grammar default once at parse start
     * (the CEK seeds little_endian at machine init); this latch makes the outermost
     * rule entry do that seeding exactly once, so `@endian:` switches then persist
     * across rule calls and backtrack-restore like the CEK register. */
    bool endian_seeded;
} bbq_ctx_t;

static inline void bbq_ctx_init(bbq_ctx_t* ctx, const uint8_t* data, size_t len) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->data = data;
    ctx->length = len;
}

static inline size_t bbq_pos(const bbq_ctx_t* ctx) { return ctx->pos; }
static inline void bbq_seek(bbq_ctx_t* ctx, size_t pos) { ctx->pos = pos; }
static inline size_t bbq_total_size(const bbq_ctx_t* ctx) { return ctx->length; }
static inline bool bbq_at_end(const bbq_ctx_t* ctx) { return ctx->pos >= ctx->length; }

static inline size_t bbq_effective_end(const bbq_ctx_t* ctx) {
    if (ctx->interval_depth > 0) return ctx->interval_ends[ctx->interval_depth - 1];
    return ctx->length;
}

static inline size_t bbq_effective_start(const bbq_ctx_t* ctx) {
    if (ctx->interval_depth > 0) return ctx->interval_starts[ctx->interval_depth - 1];
    return 0;
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

/* Random-access seek (`@[start,end]`). The target must land inside the input;
 * the CEK fails the parse on an out-of-range seek rather than moving the cursor
 * to nowhere. (Plain `@pos`-relative advance stays unchecked, like AdvancePos.) */
static inline bool bbq_seek_checked(bbq_ctx_t* ctx, size_t pos) {
    if (pos > ctx->length) return bbq_fail(ctx, "seek out of range");
    ctx->pos = pos;
    return true;
}

/* Bounds-checked cursor advance within the active window — the C analogue of the
 * C++ view reader's r.advance(n). c-lite uses it to skip a leaf's bytes after
 * recording its [start, pos) span (the owning reader advances inside bbq_read_*). */
static inline bool bbq_advance(bbq_ctx_t* ctx, size_t n) {
    if (ctx->pos + n > bbq_effective_end(ctx)) return bbq_fail(ctx, "unexpected end of input");
    ctx->pos += n;
    return true;
}

/* Checkpoints (for backtracking). The interval depth is part of the parse state:
 * a failed alternative may abandon open @rest windows, so restoring unwinds them
 * (mirrors the CEK backend's savepoint). */
/* A backtrack snapshot of the full parse state the CEK savepoint restores: cursor,
 * interval stack, endian register, and the scope/loop stack depths (a caught
 * failure — choice arm, optional, resync element — may have pushed scopes via an
 * invoke or loop frames before failing; restoring the depths unwinds them). */
typedef struct {
    size_t pos;
    int interval_depth;
    bool little_endian;
    int scope_depth;
    int loop_depth;
    int loop_base_depth;
} bbq_checkpoint_t;

static inline bbq_checkpoint_t bbq_save(const bbq_ctx_t* ctx) {
    bbq_checkpoint_t cp = { ctx->pos, ctx->interval_depth, ctx->little_endian,
                            ctx->scope_depth, ctx->loop_depth, ctx->loop_base_depth };
    return cp;
}

static inline void bbq_restore(bbq_ctx_t* ctx, bbq_checkpoint_t cp) {
    ctx->pos = cp.pos;
    ctx->interval_depth = cp.interval_depth;
    ctx->little_endian = cp.little_endian;
    ctx->scope_depth = cp.scope_depth;
    ctx->loop_depth = cp.loop_depth;
    ctx->loop_base_depth = cp.loop_base_depth;
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

// Counted-array loop: push with the iteration limit (the CEK's LoopFrame.limit).
static inline void bbq_push_loop_n(bbq_ctx_t* ctx, int64_t limit) {
    if (ctx->loop_depth < BBQ_MAX_LOOPS) {
        ctx->loop_indices[ctx->loop_depth] = 0;
        ctx->loop_limits[ctx->loop_depth] = limit;
        ctx->loop_caps[ctx->loop_depth] = (size_t)(limit > 0 ? limit : 0);
        ctx->loop_depth++;
    }
}

// Unbounded-array loop (eof/until): no fixed limit; the count is discovered as
// elements are parsed, so the backing store grows (cap starts 0). Mirrors the
// CEK's LoopFrame with limit = -1.
static inline void bbq_push_loop_unbounded(bbq_ctx_t* ctx) {
    if (ctx->loop_depth < BBQ_MAX_LOOPS) {
        ctx->loop_indices[ctx->loop_depth] = 0;
        ctx->loop_limits[ctx->loop_depth] = -1;
        ctx->loop_caps[ctx->loop_depth] = 0;
        ctx->loop_depth++;
    }
}

// Advance the innermost loop's counter without a limit test (the eof/until loops
// decide continuation by cursor/predicate, not the counter).
static inline void bbq_loop_advance(bbq_ctx_t* ctx) {
    if (ctx->loop_depth > 0) ctx->loop_indices[ctx->loop_depth - 1]++;
}

static inline int64_t bbq_loop_limit(const bbq_ctx_t* ctx) {
    return (ctx->loop_depth > 0) ? ctx->loop_limits[ctx->loop_depth - 1] : 0;
}
// Advance to the next element; returns true while another element remains.
static inline bool bbq_loop_next(bbq_ctx_t* ctx) {
    if (ctx->loop_depth <= 0) return false;
    int64_t i = ++ctx->loop_indices[ctx->loop_depth - 1];
    return i < ctx->loop_limits[ctx->loop_depth - 1];
}

static inline void bbq_set_loop_index(bbq_ctx_t* ctx, int64_t idx) {
    if (ctx->loop_depth > 0)
        ctx->loop_indices[ctx->loop_depth - 1] = idx;
}

static inline int64_t bbq_loop_index(const bbq_ctx_t* ctx) {
    return (ctx->loop_depth > 0) ? ctx->loop_indices[ctx->loop_depth - 1] : 0;
}
static inline int bbq_loop_depth(const bbq_ctx_t* ctx) { return ctx->loop_depth; }
// The index of the loop at an absolute stack depth (its own frame).
static inline int64_t bbq_loop_index_at(const bbq_ctx_t* ctx, int depth) {
    return (depth >= 0 && depth < ctx->loop_depth) ? ctx->loop_indices[depth] : 0;
}
// Per-rule loop base (loop_depth at the rule's entry). Pushed at each invoke; the
// top rule's base is 0 (empty stack). Array slots index by base + nesting level.
static inline void bbq_push_loop_base(bbq_ctx_t* ctx, int base) {
    if (ctx->loop_base_depth < BBQ_MAX_SCOPES) ctx->loop_bases[ctx->loop_base_depth++] = base;
}
static inline void bbq_pop_loop_base(bbq_ctx_t* ctx) {
    if (ctx->loop_base_depth > 0) ctx->loop_base_depth--;
}
static inline int bbq_cur_loop_base(const bbq_ctx_t* ctx) {
    return (ctx->loop_base_depth > 0) ? ctx->loop_bases[ctx->loop_base_depth - 1] : 0;
}

/* Interval stack */
static inline void bbq_pop_interval(bbq_ctx_t* ctx) {
    if (ctx->interval_depth > 0) ctx->interval_depth--;
}

/* Confined @rest interval: the declared window [pos, end) must lie within the
 * enclosing window — a length that escapes its parent (or the input) is malformed,
 * never silently clamped — and on close the construct must have consumed it
 * EXACTLY (a `size` field means the rest is size bytes, not "at most"). Same
 * [l,r) confinement the CEK backend enforces. Used for `@rest` fields; the
 * seek-based @[start,end] windows keep the unchecked push/pop above (a random-
 * access view is not a consumed span). */
static inline bool bbq_push_interval_checked(bbq_ctx_t* ctx, size_t end) {
    size_t start = ctx->pos;
    /* IPG confinement (both edges): the new window [start, end) must fall entirely
     * within the enclosing window [effective_start, effective_end), and be well
     * formed (start <= end). Mirrors the CEK's PushIntervalApplyKont. */
    if (start > end || start < bbq_effective_start(ctx) || end > bbq_effective_end(ctx))
        return bbq_fail(ctx, "interval out of range");
    if (ctx->interval_depth >= BBQ_MAX_SCOPES)
        return bbq_fail(ctx, "interval: nesting too deep");
    ctx->interval_starts[ctx->interval_depth] = start;
    ctx->interval_ends[ctx->interval_depth] = end;
    ctx->interval_depth++;
    return true;
}

static inline bool bbq_pop_interval_checked(bbq_ctx_t* ctx) {
    if (ctx->interval_depth <= 0) return bbq_fail(ctx, "interval: unbalanced pop");
    size_t end = ctx->interval_ends[--ctx->interval_depth];
    if (ctx->pos != end) return bbq_fail(ctx, "interval: size mismatch");
    return true;
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

/* ── Native-endian reads (unsuffixed types) ───────────────────
 *
 * An unsuffixed `uint16` etc. has no baked byte order — it follows the live
 * endian register (`@endian` directive default, mutated by `@endian: <expr>`
 * switches), exactly like the CEK's PrimEndian::Native consulting
 * m->little_endian at read time. A suffixed `uint16le`/`be` bypasses this. */
static inline bool bbq_read_u16(bbq_ctx_t* ctx, uint16_t* out) {
    return ctx->little_endian ? bbq_read_u16le(ctx, out) : bbq_read_u16be(ctx, out);
}
static inline bool bbq_read_i16(bbq_ctx_t* ctx, int16_t* out) {
    return ctx->little_endian ? bbq_read_i16le(ctx, out) : bbq_read_i16be(ctx, out);
}
static inline bool bbq_read_u32(bbq_ctx_t* ctx, uint32_t* out) {
    return ctx->little_endian ? bbq_read_u32le(ctx, out) : bbq_read_u32be(ctx, out);
}
static inline bool bbq_read_i32(bbq_ctx_t* ctx, int32_t* out) {
    return ctx->little_endian ? bbq_read_i32le(ctx, out) : bbq_read_i32be(ctx, out);
}
static inline bool bbq_read_u64(bbq_ctx_t* ctx, uint64_t* out) {
    return ctx->little_endian ? bbq_read_u64le(ctx, out) : bbq_read_u64be(ctx, out);
}
static inline bool bbq_read_i64(bbq_ctx_t* ctx, int64_t* out) {
    return ctx->little_endian ? bbq_read_i64le(ctx, out) : bbq_read_i64be(ctx, out);
}

/* ── LEB128 variable-length integers ──────────────────────────
 *
 * LSB-first base-128: each byte carries 7 value bits; bit 7 is the
 * continuation flag. Bounded to ceil(bits/7) bytes; the final byte's
 * unused high bits must be 0 (unsigned) or a clean sign-extension
 * (signed). Overlong / over-wide encodings are rejected — strict,
 * matching WebAssembly validation. `bits` is the value width (the
 * carrier into which it is decoded); 32 and 64 are wired today. */

static inline bool bbq_read_uleb128(bbq_ctx_t* ctx, uint64_t* out, int bits) {
    uint64_t result = 0;
    int shift = 0;
    int maxbytes = (bits + 6) / 7;
    for (int i = 0; i < maxbytes; i++) {
        uint8_t byte;
        if (!bbq_read_u8(ctx, &byte)) return false;
        if (i == maxbytes - 1) {
            if (byte & 0x80) return bbq_fail(ctx, "uleb128: too long");
            int avail = bits - shift;                 /* 1..7 value bits left */
            if (avail < 7 && (byte >> avail))
                return bbq_fail(ctx, "uleb128: value too large");
        }
        result |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) { *out = result; return true; }
        shift += 7;
    }
    return bbq_fail(ctx, "uleb128: unterminated");
}

static inline bool bbq_read_sleb128(bbq_ctx_t* ctx, int64_t* out, int bits) {
    uint64_t result = 0;
    int shift = 0;
    int maxbytes = (bits + 6) / 7;
    for (int i = 0; i < maxbytes; i++) {
        uint8_t byte;
        if (!bbq_read_u8(ctx, &byte)) return false;
        if (i == maxbytes - 1) {
            if (byte & 0x80) return bbq_fail(ctx, "sleb128: too long");
            /* Bits above the value width must equal the sign bit. `avail` value
             * bits remain; the MSB is bit (avail-1), bits [avail..6] must match it. */
            int avail = bits - shift;                 /* 1..7 */
            uint8_t low_mask = (uint8_t)(0x7f >> (7 - avail));     /* value bits */
            uint8_t hi = (uint8_t)((byte & 0x7f) & ~low_mask);     /* padding bits */
            uint8_t expect = (byte & (1u << (avail - 1)))
                           ? (uint8_t)(0x7f & ~low_mask) : 0;
            if (hi != expect) return bbq_fail(ctx, "sleb128: value out of range");
        }
        result |= (uint64_t)(byte & 0x7f) << shift;
        shift += 7;
        if (!(byte & 0x80)) {
            if (shift < 64 && (byte & 0x40)) result |= ~(uint64_t)0 << shift;
            *out = (int64_t)result;
            return true;
        }
    }
    return bbq_fail(ctx, "sleb128: unterminated");
}

static inline bool bbq_read_uleb128_u32(bbq_ctx_t* ctx, uint32_t* out) {
    uint64_t v; if (!bbq_read_uleb128(ctx, &v, 32)) return false;
    *out = (uint32_t)v; return true;
}
static inline bool bbq_read_uleb128_u64(bbq_ctx_t* ctx, uint64_t* out) {
    return bbq_read_uleb128(ctx, out, 64);
}
static inline bool bbq_read_sleb128_i32(bbq_ctx_t* ctx, int32_t* out) {
    int64_t v; if (!bbq_read_sleb128(ctx, &v, 32)) return false;
    *out = (int32_t)v; return true;
}
static inline bool bbq_read_sleb128_i64(bbq_ctx_t* ctx, int64_t* out) {
    return bbq_read_sleb128(ctx, out, 64);
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

static inline bool bbq_read_f32(bbq_ctx_t* ctx, float* out) {
    return ctx->little_endian ? bbq_read_f32le(ctx, out) : bbq_read_f32be(ctx, out);
}
static inline bool bbq_read_f64(bbq_ctx_t* ctx, double* out) {
    return ctx->little_endian ? bbq_read_f64le(ctx, out) : bbq_read_f64be(ctx, out);
}

static inline bool bbq_read_bool(bbq_ctx_t* ctx, bool* out) {
    uint8_t v; if (!bbq_read_u8(ctx, &v)) return false;
    *out = v != 0; return true;
}

/* ── Utility: abs for expressions (reader and writer both spell abs()) ── */
static inline int64_t bbq_abs(int64_t v) { return v < 0 ? -v : v; }

#endif /* BBQ_READ_H */
