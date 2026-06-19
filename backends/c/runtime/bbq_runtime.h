/* bbq_runtime.h — owning C runtime for BBQ-generated readers and writers.
 *
 * The OWNING half over the shared cursor+decoder core (bbq_read.h): the malloc'd
 * leaf copies (bytes/string), unbounded-array backing-store growth, and the whole
 * write context (bbq_write_ctx_t + primitives + the dual machine state). The
 * generated owning reader/writer include this; c-lite includes only bbq_read.h.
 *
 * Header-only (static inline), the bbq_read.h idiom — see the note there: the
 * decoders must inline into copy-and-patch JIT stencils, so the C runtime ships
 * header-only by design, not as a deferred .c split.
 */
#ifndef BBQ_RUNTIME_H
#define BBQ_RUNTIME_H

#include "bbq_read.h"

#include <stdlib.h>   /* malloc / realloc / free — owning leaves + array growth */
#include <string.h>

/* ── Unbounded-array backing store (owning) ──────────────────
 * Ensure the innermost unbounded array's backing store holds the current index;
 * doubles capacity and zeroes new slots. Returns the (possibly realloc'd) pointer,
 * or NULL on allocation failure. (c-lite records spans instead of materializing
 * elements, so it never grows a backing store — owning-only.) */
static inline void* bbq_array_grow(bbq_ctx_t* ctx, void* items, size_t elem) {
    int d = ctx->loop_depth - 1;
    if (d < 0) return items;
    size_t need = (size_t)ctx->loop_indices[d] + 1;
    if (need <= ctx->loop_caps[d]) return items;
    size_t newcap = ctx->loop_caps[d] ? ctx->loop_caps[d] * 2 : 4;
    if (newcap < need) newcap = need;
    void* p = realloc(items, newcap * elem);
    if (!p) return NULL;
    memset((char*)p + ctx->loop_caps[d] * elem, 0, (newcap - ctx->loop_caps[d]) * elem);
    ctx->loop_caps[d] = newcap;
    return p;
}

/* Owning bytes read: copies into malloc'd storage, so the struct is self-contained
 * and outlives the input buffer — the same semantics as the C++ backend's owning
 * std::vector<uint8_t>. <type>_free releases it. (Borrow semantics proved unsound
 * for any reader whose input is transient — e.g. a parse IR decoded from scratch
 * buffers; structs aliased freed memory.) */
static inline bool bbq_read_bytes(bbq_ctx_t* ctx, const uint8_t** out,
                                   size_t* out_len, size_t len) {
    if (ctx->pos + len > bbq_effective_end(ctx)) return bbq_fail(ctx, "bytes: overrun");
    uint8_t* copy = (uint8_t*)malloc(len ? len : 1);
    if (!copy) return bbq_fail(ctx, "bytes: out of memory");
    memcpy(copy, ctx->data + ctx->pos, len);
    *out = copy;
    *out_len = len;
    ctx->pos += len;
    return true;
}

/* Owning string read: a malloc'd, NUL-terminated copy (the NUL is not counted in
 * out_len) — the same semantics as the C++ backend's owning std::string. */
static inline bool bbq_read_string(bbq_ctx_t* ctx, const char** out,
                                    size_t* out_len, size_t len) {
    if (ctx->pos + len > bbq_effective_end(ctx)) return bbq_fail(ctx, "string: overrun");
    char* copy = (char*)malloc(len + 1);
    if (!copy) return bbq_fail(ctx, "string: out of memory");
    memcpy(copy, ctx->data + ctx->pos, len);
    copy[len] = '\0';
    *out = copy;
    *out_len = len;
    ctx->pos += len;
    return true;
}

/* ── Write context ──────────────────────────────────────── */

/* A write buffer is either caller-supplied and fixed (bbq_write_ctx_init: overflow
 * fails the write) or runtime-owned and growable (bbq_write_ctx_init_growable: the
 * buffer is malloc'd and doubles on demand). The growable variant keeps the runtime
 * header standalone — no arena / crt dependency — and must be released with
 * bbq_write_ctx_free. */
typedef struct {
    uint8_t* data;
    size_t capacity;
    size_t pos;
    bool little_endian;
    bool owns;          /* true: `data` is malloc'd here, grows on demand, freed by
                           bbq_write_ctx_free. false: fixed caller buffer, overflow fails. */
    bool endian_seeded; /* latch: the outermost write seeds little_endian from the
                           grammar default; nested rule calls inherit the live register
                           so `@endian:` switches persist (the reader's endian_seeded dual). */
    /* Dual machine state — the writer refunctionalizes the SAME kont graph under a
     * forward-serialize algebra, so it carries the duals of the reader's machine
     * state: a loop stack (the writer knows each count from the stored struct, so no
     * caps/grow/unbounded), an interval window stack (random-access placement + size
     * windows), and a scope stack (an outer-rule field referenced from a sub-rule). */
    int64_t loop_indices[BBQ_MAX_LOOPS];
    int64_t loop_limits[BBQ_MAX_LOOPS];
    int loop_depth;
    int loop_bases[BBQ_MAX_SCOPES];
    int loop_base_depth;
    size_t interval_starts[BBQ_MAX_SCOPES];
    size_t interval_ends[BBQ_MAX_SCOPES];
    int interval_depth;
    const void* scopes[BBQ_MAX_SCOPES];
    int scope_depth;
    /* @rest size holes: the owning writer never trusts a stored @rest size field — it
     * records where the size goes, writes the rest, then computes the size from the bytes
     * written and inserts the prefix. So a modified (re-sized) body serializes a correct
     * size every time. */
    size_t rest_holes[BBQ_MAX_SCOPES];
    int rest_depth;
    size_t count_holes[BBQ_MAX_LOOPS];  /* `[n]` count prefixes, indexed by id (see above) */
} bbq_write_ctx_t;

/* Zero the dual machine state. Shared by both init paths. */
static inline void bbq_write_ctx_reset_state(bbq_write_ctx_t* ctx) {
    ctx->endian_seeded = false;
    ctx->loop_depth = 0;
    ctx->loop_base_depth = 0;
    ctx->interval_depth = 0;
    ctx->scope_depth = 0;
    ctx->rest_depth = 0;
}

static inline void bbq_write_ctx_init(bbq_write_ctx_t* ctx,
                                       uint8_t* buf, size_t cap) {
    ctx->data = buf;
    ctx->capacity = cap;
    ctx->pos = 0;
    ctx->little_endian = false;
    ctx->owns = false;
    bbq_write_ctx_reset_state(ctx);
}

/* Growable, runtime-owned write buffer. Returns false if the initial allocation
 * fails. Release with bbq_write_ctx_free once the serialized bytes have been copied
 * out (or on any error path). */
static inline bool bbq_write_ctx_init_growable(bbq_write_ctx_t* ctx, size_t initial) {
    if (initial < 64) initial = 64;
    ctx->data = (uint8_t*)malloc(initial);
    if (!ctx->data) return false;
    ctx->capacity = initial;
    ctx->pos = 0;
    ctx->little_endian = false;
    ctx->owns = true;
    bbq_write_ctx_reset_state(ctx);
    return true;
}

/* Release a growable buffer's storage. No-op for a fixed caller buffer. */
static inline void bbq_write_ctx_free(bbq_write_ctx_t* ctx) {
    if (ctx->owns) free(ctx->data);
    ctx->data = NULL;
    ctx->capacity = 0;
    ctx->owns = false;
}

/* Ensure room for `need` more bytes. Grows an owned buffer (doubling realloc); a
 * fixed caller buffer just reports overflow. */
static inline bool bbq_write_reserve(bbq_write_ctx_t* ctx, size_t need) {
    if (ctx->pos + need <= ctx->capacity) return true;
    if (!ctx->owns) return false;
    size_t newcap = ctx->capacity ? ctx->capacity : 64;
    while (newcap < ctx->pos + need) newcap *= 2;
    uint8_t* nd = (uint8_t*)realloc(ctx->data, newcap);
    if (!nd) return false;
    ctx->data = nd;
    ctx->capacity = newcap;
    return true;
}

static inline size_t bbq_write_pos(const bbq_write_ctx_t* ctx) { return ctx->pos; }

static inline void bbq_write_set_endian(bbq_write_ctx_t* ctx, bool little) {
    ctx->little_endian = little;
}

/* ── Write primitives ───────────────────────────────────── */

static inline bool bbq_write_u8(bbq_write_ctx_t* ctx, uint8_t val) {
    if (!bbq_write_reserve(ctx, 1)) return false;
    ctx->data[ctx->pos++] = val;
    return true;
}

static inline bool bbq_write_i8(bbq_write_ctx_t* ctx, int8_t val) {
    return bbq_write_u8(ctx, (uint8_t)val);
}

static inline bool bbq_write_u16be(bbq_write_ctx_t* ctx, uint16_t val) {
    if (!bbq_write_reserve(ctx, 2)) return false;
    uint16_t v = bbq_host_is_little_endian() ? bbq_bswap16(val) : val;
    memcpy(ctx->data + ctx->pos, &v, 2);
    ctx->pos += 2;
    return true;
}

static inline bool bbq_write_u16le(bbq_write_ctx_t* ctx, uint16_t val) {
    if (!bbq_write_reserve(ctx, 2)) return false;
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
    if (!bbq_write_reserve(ctx, 4)) return false;
    uint32_t v = bbq_host_is_little_endian() ? bbq_bswap32(val) : val;
    memcpy(ctx->data + ctx->pos, &v, 4);
    ctx->pos += 4;
    return true;
}

static inline bool bbq_write_u32le(bbq_write_ctx_t* ctx, uint32_t val) {
    if (!bbq_write_reserve(ctx, 4)) return false;
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
    if (!bbq_write_reserve(ctx, 8)) return false;
    uint64_t v = bbq_host_is_little_endian() ? bbq_bswap64(val) : val;
    memcpy(ctx->data + ctx->pos, &v, 8);
    ctx->pos += 8;
    return true;
}

static inline bool bbq_write_u64le(bbq_write_ctx_t* ctx, uint64_t val) {
    if (!bbq_write_reserve(ctx, 8)) return false;
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

/* LEB128 writers — canonical (minimal-length) encoding, the inverse of the
 * bbq_read_uleb128/sleb128 readers above. A canonical encoding of an N-bit
 * value never exceeds ceil(N/7) bytes, so those strict readers accept the
 * output. The _u32/_i32 wrappers zero-/sign-extend into the 64-bit encoder. */
static inline bool bbq_write_uleb128_u64(bbq_write_ctx_t* ctx, uint64_t val) {
    do {
        uint8_t byte = (uint8_t)(val & 0x7f);
        val >>= 7;
        if (val != 0) byte |= 0x80;
        if (!bbq_write_u8(ctx, byte)) return false;
    } while (val != 0);
    return true;
}

static inline bool bbq_write_uleb128_u32(bbq_write_ctx_t* ctx, uint32_t val) {
    return bbq_write_uleb128_u64(ctx, (uint64_t)val);
}

/* Encode `val` as canonical ULEB128 into `buf` (caller guarantees >= 10 bytes) and
 * return the byte count. Used by the @rest size-prefix backpatch, which must know the
 * encoded length before shifting the payload to make room for it. */
static inline size_t bbq_uleb128_encode(uint8_t* buf, uint64_t val) {
    size_t n = 0;
    do {
        uint8_t byte = (uint8_t)(val & 0x7f);
        val >>= 7;
        if (val != 0) byte |= 0x80;
        buf[n++] = byte;
    } while (val != 0);
    return n;
}

static inline bool bbq_write_sleb128_i64(bbq_write_ctx_t* ctx, int64_t val) {
    for (;;) {
        uint8_t byte = (uint8_t)(val & 0x7f);
        val >>= 7;                                   /* arithmetic shift */
        bool sign = (byte & 0x40) != 0;
        if ((val == 0 && !sign) || (val == -1 && sign)) {
            return bbq_write_u8(ctx, byte);
        }
        if (!bbq_write_u8(ctx, (uint8_t)(byte | 0x80))) return false;
    }
}

static inline bool bbq_write_sleb128_i32(bbq_write_ctx_t* ctx, int32_t val) {
    return bbq_write_sleb128_i64(ctx, (int64_t)val);
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

/* Native (suffix-less) writers — the dual of the suffix-less readers: they follow
 * the live endian register (`@endian:` switches), not a baked suffix. A mid-struct
 * `@endian: big` therefore writes `b` big while `a` stayed little (the `Es` shape). */
static inline bool bbq_write_u16(bbq_write_ctx_t* ctx, uint16_t v) {
    return ctx->little_endian ? bbq_write_u16le(ctx, v) : bbq_write_u16be(ctx, v);
}
static inline bool bbq_write_i16(bbq_write_ctx_t* ctx, int16_t v) {
    return ctx->little_endian ? bbq_write_i16le(ctx, v) : bbq_write_i16be(ctx, v);
}
static inline bool bbq_write_u32(bbq_write_ctx_t* ctx, uint32_t v) {
    return ctx->little_endian ? bbq_write_u32le(ctx, v) : bbq_write_u32be(ctx, v);
}
static inline bool bbq_write_i32(bbq_write_ctx_t* ctx, int32_t v) {
    return ctx->little_endian ? bbq_write_i32le(ctx, v) : bbq_write_i32be(ctx, v);
}
static inline bool bbq_write_u64(bbq_write_ctx_t* ctx, uint64_t v) {
    return ctx->little_endian ? bbq_write_u64le(ctx, v) : bbq_write_u64be(ctx, v);
}
static inline bool bbq_write_i64(bbq_write_ctx_t* ctx, int64_t v) {
    return ctx->little_endian ? bbq_write_i64le(ctx, v) : bbq_write_i64be(ctx, v);
}
static inline bool bbq_write_f32(bbq_write_ctx_t* ctx, float v) {
    return ctx->little_endian ? bbq_write_f32le(ctx, v) : bbq_write_f32be(ctx, v);
}
static inline bool bbq_write_f64(bbq_write_ctx_t* ctx, double v) {
    return ctx->little_endian ? bbq_write_f64le(ctx, v) : bbq_write_f64be(ctx, v);
}

static inline bool bbq_write_bool(bbq_write_ctx_t* ctx, bool val) {
    return bbq_write_u8(ctx, val ? 1 : 0);
}

static inline bool bbq_write_bytes(bbq_write_ctx_t* ctx,
                                    const uint8_t* data, size_t len) {
    if (!bbq_write_reserve(ctx, len)) return false;
    memcpy(ctx->data + ctx->pos, data, len);
    ctx->pos += len;
    return true;
}

static inline bool bbq_write_string(bbq_write_ctx_t* ctx,
                                     const char* data, size_t len) {
    return bbq_write_bytes(ctx, (const uint8_t*)data, len);
}

/* ── Write-side machine state (dual of the reader's cursor machine) ──────────
 * The owning writer is the flat-stencil refunctionalization of the same kont
 * graph, so it carries the duals of the reader's loop / scope / interval state.
 * The control is structure-driven (the stored count/tag/has_value decides), so
 * loops never grow and never need caps — the writer knows the count up front. */

/* Per-rule loop base: the loop depth at the rule's entry, so an array's own index
 * is loop_indices[base + nesting_level] (its own frame, undisturbed by callees). */
static inline void bbq_w_push_loop_base(bbq_write_ctx_t* ctx, int base) {
    if (ctx->loop_base_depth < BBQ_MAX_SCOPES) ctx->loop_bases[ctx->loop_base_depth++] = base;
}
static inline void bbq_w_pop_loop_base(bbq_write_ctx_t* ctx) {
    if (ctx->loop_base_depth > 0) ctx->loop_base_depth--;
}
static inline int bbq_w_cur_loop_base(const bbq_write_ctx_t* ctx) {
    return (ctx->loop_base_depth > 0) ? ctx->loop_bases[ctx->loop_base_depth - 1] : 0;
}
static inline int bbq_w_loop_depth(const bbq_write_ctx_t* ctx) { return ctx->loop_depth; }

/* Counted loop (the writer always knows the count): push with the stored limit. */
static inline void bbq_w_push_loop_n(bbq_write_ctx_t* ctx, int64_t limit) {
    if (ctx->loop_depth < BBQ_MAX_LOOPS) {
        ctx->loop_indices[ctx->loop_depth] = 0;
        ctx->loop_limits[ctx->loop_depth] = limit;
        ctx->loop_depth++;
    }
}
static inline void bbq_w_pop_loop(bbq_write_ctx_t* ctx) {
    if (ctx->loop_depth > 0) ctx->loop_depth--;
}
/* Advance the innermost loop; true while more elements remain. */
static inline bool bbq_w_loop_next(bbq_write_ctx_t* ctx) {
    if (ctx->loop_depth <= 0) return false;
    int64_t i = ++ctx->loop_indices[ctx->loop_depth - 1];
    return i < ctx->loop_limits[ctx->loop_depth - 1];
}
static inline int64_t bbq_w_loop_index(const bbq_write_ctx_t* ctx) {
    return (ctx->loop_depth > 0) ? ctx->loop_indices[ctx->loop_depth - 1] : 0;
}
static inline int64_t bbq_w_loop_index_at(const bbq_write_ctx_t* ctx, int depth) {
    return (depth >= 0 && depth < ctx->loop_depth) ? ctx->loop_indices[depth] : 0;
}

/* Scope stack: an invoke pushes the caller's struct so a sub-rule can resolve a
 * field in the enclosing scope (the dual of the reader's bbq_scope_ptr). */
static inline void bbq_w_push_scope(bbq_write_ctx_t* ctx, const void* ptr) {
    if (ctx->scope_depth < BBQ_MAX_SCOPES) ctx->scopes[ctx->scope_depth++] = ptr;
}
static inline void bbq_w_pop_scope(bbq_write_ctx_t* ctx) {
    if (ctx->scope_depth > 0) ctx->scope_depth--;
}
static inline const void* bbq_w_scope_ptr(const bbq_write_ctx_t* ctx, int levels_up) {
    int idx = ctx->scope_depth - 1 - levels_up;
    return (idx >= 0) ? ctx->scopes[idx] : NULL;
}

/* Seek the output cursor to an absolute offset, zero-extending forward placement
 * (a field interval @[s,e] lands its value at s; the gap before it is zero-filled,
 * which the reader skips). `pos` is the buffer's logical length — intervals place
 * forward, so it only grows. */
static inline bool bbq_w_seek(bbq_write_ctx_t* ctx, size_t target) {
    if (target > ctx->pos) {
        if (!bbq_write_reserve(ctx, target - ctx->pos)) return false;
        memset(ctx->data + ctx->pos, 0, target - ctx->pos);
    }
    ctx->pos = target;
    return true;
}
static inline bool bbq_w_advance(bbq_write_ctx_t* ctx, size_t by) {
    return bbq_w_seek(ctx, ctx->pos + by);
}

/* Interval window: record [pos, end). pop seeks the cursor to the window end so the
 * outer continues after it (matching the reader, which leaves pos==end on pop). */
static inline bool bbq_w_push_interval(bbq_write_ctx_t* ctx, size_t end) {
    if (ctx->interval_depth >= BBQ_MAX_SCOPES) return false;
    ctx->interval_starts[ctx->interval_depth] = ctx->pos;
    ctx->interval_ends[ctx->interval_depth] = end;
    ctx->interval_depth++;
    return true;
}
static inline bool bbq_w_pop_interval(bbq_write_ctx_t* ctx) {
    if (ctx->interval_depth <= 0) return false;
    size_t end = ctx->interval_ends[--ctx->interval_depth];
    return bbq_w_seek(ctx, end);
}

/* @rest: record the offset where the (computed) size prefix will be inserted — the rest
 * is then written starting here, with no size yet. */
static inline void bbq_w_push_rest(bbq_write_ctx_t* ctx) {
    if (ctx->rest_depth < BBQ_MAX_SCOPES) ctx->rest_holes[ctx->rest_depth++] = ctx->pos;
}
/* A `[n]` / count(n) prefix: reserve a fixed-width placeholder where the count goes (so
 * later fields don't clobber it), recording the hole by id — the writer never serializes
 * the stored count field, only the array's actual length. Indexed (not a stack) because a
 * rule can read several counts before their arrays (e.g. nested array<array[cols]>[rows]),
 * so each count field links to its own array by id, not LIFO order. */
static inline bool bbq_w_reserve_count(bbq_write_ctx_t* ctx, int id, int width) {
    size_t w = (size_t)(width / 8);
    if (!bbq_write_reserve(ctx, w)) return false;
    if (id >= 0 && id < BBQ_MAX_LOOPS) ctx->count_holes[id] = ctx->pos;
    memset(ctx->data + ctx->pos, 0, w);
    ctx->pos += w;
    return true;
}
/* Patch the reserved count placeholder (by id) with the array's real element count (known
 * up front from the vec), in the count field's width/endian — a valid binary by
 * construction, never a stale prefix. */
static inline bool bbq_w_patch_count(bbq_write_ctx_t* ctx, int id, uint64_t count, int width, bool little) {
    if (id < 0 || id >= BBQ_MAX_LOOPS) return false;
    size_t hole = ctx->count_holes[id];
    size_t w = (size_t)(width / 8);
    for (size_t i = 0; i < w; i++)
        ctx->data[hole + i] = (uint8_t)(count >> (8 * (little ? i : (w - 1 - i))));
    return true;
}

/* @rest: the rest is fully written — size = bytes written since the hole. Encode it
 * (uleb or fixed-width/endian), shift the rest forward to make room, and insert. The
 * size is DERIVED from content, never the stored field, so a re-sized body is correct. */
static inline bool bbq_w_patch_rest(bbq_write_ctx_t* ctx, int width, bool is_uleb, bool little) {
    if (ctx->rest_depth <= 0) return false;
    size_t hole = ctx->rest_holes[--ctx->rest_depth];
    size_t rest_len = ctx->pos - hole;
    uint8_t tmp[10];
    size_t n;
    if (is_uleb) {
        n = bbq_uleb128_encode(tmp, (uint64_t)rest_len);
    } else {
        n = (size_t)(width / 8);
        uint64_t v = (uint64_t)rest_len;
        for (size_t i = 0; i < n; i++) tmp[i] = (uint8_t)(v >> (8 * (little ? i : (n - 1 - i))));
    }
    if (!bbq_write_reserve(ctx, n)) return false;
    memmove(ctx->data + hole + n, ctx->data + hole, rest_len);
    memcpy(ctx->data + hole, tmp, n);
    ctx->pos += n;
    return true;
}

#endif /* BBQ_RUNTIME_H */
