/* bbq_lite.h — c-lite: the zero-copy index runtime for the read-only C view backend.
 *
 * The C port of the C++ read-only index core (backends/cpp/runtime/Capture.h +
 * CaptureDecode + bbq_node): the generated c-lite reader advances the shared cursor
 * (bbq_read.h) and records [start,end)+type spans into a bbq_capture_builder, then
 * the index is read decode-on-access. No backing store, no copy-on-write — a pure
 * span index over the input buffer (the embedded-VM read model).
 *
 * std::vector -> bbq_vec (crt), ParseArena -> bbq_arena (crt). NOT header-only:
 * unlike bbq_read.h it is never inlined into a JIT stencil, so it is a proper
 * .h-declares / .c-implements lib.
 */
#ifndef BBQ_LITE_H
#define BBQ_LITE_H

#include "bbq_read.h"      /* cursor + decoders (shared core) */
#include "bbq_arena.h"     /* crt arena — owns the resolved index */
#include "bbq_vec.h"       /* crt growable arrays — build-phase storage */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* A computed field's value (compute(...), LEB128, bitfield run): a small tagged
 * scalar, arena-owned. Kind ordinals match the C++ ComputedValue::Kind. */
typedef struct {
    enum { BBQ_CV_INT = 0, BBQ_CV_FLOAT = 1, BBQ_CV_BOOL = 2, BBQ_CV_STRING = 3 } kind;
    union {
        int64_t i;
        double  f;
        bool    b;
        const char* s;
    };
} bbq_computed_value;

/* Capture leaf/aggregate type. Ordinals match the C++ bbq::CaptureType enum so a
 * c-lite index dump and a CEK index dump line up cell-for-cell. */
typedef enum {
    BBQ_CT_UInt8 = 0, BBQ_CT_UInt16LE, BBQ_CT_UInt16BE, BBQ_CT_UInt32LE, BBQ_CT_UInt32BE,
    BBQ_CT_UInt64LE, BBQ_CT_UInt64BE,
    BBQ_CT_Int8, BBQ_CT_Int16LE, BBQ_CT_Int16BE, BBQ_CT_Int32LE, BBQ_CT_Int32BE,
    BBQ_CT_Int64LE, BBQ_CT_Int64BE,
    BBQ_CT_Float32LE, BBQ_CT_Float32BE, BBQ_CT_Float64LE, BBQ_CT_Float64BE,
    BBQ_CT_Bool, BBQ_CT_Bytes, BBQ_CT_String,
    BBQ_CT_Struct, BBQ_CT_Array, BBQ_CT_Computed, BBQ_CT_External
} bbq_capture_type;

/* A node in the span index. No `parent` pointer (that is the C++ CoW machinery;
 * c-lite is read-only). `children` is NULL during build — bbq_cap_finish resolves
 * it into the arena; until then navigation uses build_buf_index. */
typedef struct bbq_field_capture {
    const char* name;
    size_t start_offset;
    size_t end_offset;
    bbq_capture_type type;
    struct bbq_field_capture* children;
    int child_count;
    int build_buf_index;      /* build-phase only; -1 once resolved */
    int variant_tag;          /* switch arm / union variant taken; -1 if none */
    bbq_computed_value* computed_value;
} bbq_field_capture;

typedef struct {
    bool success;
    size_t bytes_consumed;
    bbq_field_capture* root;
    const char* error_message;
    size_t error_offset;
} bbq_capture_metadata;

/* Build-phase scope frame (open struct/array/variant). */
typedef struct {
    const char* name;
    size_t start_pos;
    int first_child_index;
    bbq_capture_type type;
    int variant_tag;
} bbq_capture_scope;

/* The index builder: append-ordered fields_, a scope stack, and a child_buf_ that
 * closed scopes' children are packed into (the C++ CaptureBuilder, field for field). */
typedef struct {
    bbq_field_capture* fields;     /* bbq_vec */
    bbq_capture_scope* scopes;     /* bbq_vec */
    bbq_field_capture* child_buf;  /* bbq_vec — packed children of closed scopes */
    int pending_variant_tag;       /* armed at switch dispatch, consumed by the arm */
    bbq_arena* arena;              /* computed values during build + finish resolution */
} bbq_capture_builder;

/* A backtrack snapshot of the builder (the dual of bbq_checkpoint_t for the index). */
typedef struct {
    int depth;
    int child_index;
    int child_buf_index;
} bbq_capture_mark_t;

/* The full view backtrack snapshot: cursor + builder mark (the C++ reader::checkpoint).
 * A failed optional/choice arm/resync element restores both — dropping partial captures
 * and rewinding the cursor/interval/loop/endian state. */
typedef struct {
    bbq_checkpoint_t cur;
    bbq_capture_mark_t mark;
} bbq_view_checkpoint_t;

/* The c-lite parse context: the shared cursor + the index builder. The arena that
 * owns the resolved index is CALLER-supplied (the dual of the C++ view's ParseArena&
 * and the CEK), so metadata.root outlives the read call. The generated reader's
 * stencils take a bbq_view_ctx_t*. */
typedef struct {
    bbq_ctx_t cur;
    bbq_capture_builder builder;
} bbq_view_ctx_t;

/* ── builder lifecycle ── */
/* `arena` is caller-owned and must outlive the returned metadata's index. */
void bbq_view_ctx_init(bbq_view_ctx_t* c, const uint8_t* data, size_t len, bbq_arena* arena);
/* Free the build-phase scratch vectors (NOT the caller's arena). Call after finish. */
void bbq_view_build_free(bbq_view_ctx_t* c);
bbq_capture_metadata bbq_view_finish(bbq_view_ctx_t* c, bool ok);

/* ── builder scope/field ops (the bbq::CaptureBuilder API, C-spelled) ── */
void bbq_cap_begin_struct(bbq_capture_builder* b, const char* name, size_t pos);
void bbq_cap_end_struct(bbq_capture_builder* b, size_t pos);
void bbq_cap_begin_array(bbq_capture_builder* b, const char* name, size_t pos);
void bbq_cap_end_array(bbq_capture_builder* b, size_t pos);
void bbq_cap_begin_variant(bbq_capture_builder* b, const char* name, size_t pos, int tag);
void bbq_cap_add_field(bbq_capture_builder* b, const char* name, size_t start,
                       size_t end, bbq_capture_type type, bbq_computed_value* cv);
void bbq_cap_set_pending_variant_tag(bbq_capture_builder* b, int tag);
bbq_capture_mark_t bbq_cap_mark(const bbq_capture_builder* b);
void bbq_cap_restore(bbq_capture_builder* b, bbq_capture_mark_t m);
size_t bbq_cap_current_scope_start(const bbq_capture_builder* b);

/* ── build-phase content-keyed lookup (string literals; outward lexical scan) ── */
const bbq_field_capture* bbq_cap_find_field_str(const bbq_capture_builder* b, const char* name);
const bbq_field_capture* bbq_cap_find_child_str(const bbq_capture_builder* b,
                                                const bbq_field_capture* parent, const char* name);
const bbq_field_capture* bbq_cap_find_child_at(const bbq_capture_builder* b,
                                               const bbq_field_capture* parent, int index);

/* ── backtrack (optional / choice arm / resync element) ── */
bbq_view_checkpoint_t bbq_view_save(bbq_view_ctx_t* c);
void bbq_view_restore(bbq_view_ctx_t* c, bbq_view_checkpoint_t cp);

/* ── decode-on-access ── */
/* Decode an integer-typed (or Computed) leaf; fills *bits + *sgn. Returns false if
 * the type isn't integer-convertible. */
bool bbq_decode_int(const bbq_field_capture* cap, const uint8_t* buf, int64_t* bits, bool* sgn);
/* Decode a float leaf (Float32/64 LE/BE). Returns false if not a float. */
bool bbq_decode_float(const bbq_field_capture* cap, const uint8_t* buf, double* out);
/* node_int / node_float: decode through the RECORDED capture type (the path_value /
 * compare spellings). */
int64_t bbq_node_int(const bbq_field_capture* cap, const uint8_t* buf);
double  bbq_node_float(const bbq_field_capture* cap, const uint8_t* buf);
/* Decode a prior field in scope as int by name (the field_ref spelling: array count
 * `[n]`, where-clause, switch back-reference). */
int64_t bbq_view_i64(bbq_view_ctx_t* c, const char* name);
/* Recover a prior non-scalar (Bytes/String/External) field in scope as its span —
 * the field_ref spelling for a where-clause over such a field. */
bbq_bytes_t bbq_view_bytes(bbq_view_ctx_t* c, const char* name);

/* ── view-layer read points (the stencil sink: advance cursor + record span) ── */
/* Record a computed value at the cursor (compute(...)/bitfield entry). The _val macro
 * picks the kind from the C expression type (C11 _Generic), the dual of the C++
 * reader's add_computed_val<T>. */
void bbq_view_add_computed_int(bbq_view_ctx_t* c, const char* name, int64_t v);
void bbq_view_add_computed_float(bbq_view_ctx_t* c, const char* name, double v);
void bbq_view_add_computed_bool(bbq_view_ctx_t* c, const char* name, bool v);
#define bbq_view_add_computed_val(c, name, val) _Generic((val), \
    _Bool:  bbq_view_add_computed_bool,  \
    float:  bbq_view_add_computed_float, \
    double: bbq_view_add_computed_float, \
    default: bbq_view_add_computed_int)((c), (name), (val))

/* LEB128 leaf: decode (advancing the cursor) and record a Computed over the real span. */
bool bbq_view_uleb_capture(bbq_view_ctx_t* c, const char* name, int bits);
bool bbq_view_sleb_capture(bbq_view_ctx_t* c, const char* name, int bits);

/* Read an unsigned integer of `width_bytes` at an absolute offset (no advance) — for a
 * bitfield container whose members are computed by shift/mask. native resolves against
 * the live endian register. */
uint64_t bbq_view_read_at(const bbq_view_ctx_t* c, size_t offset, int width_bytes,
                          bool be, bool native);

/* Resolve a fixed-width prim's capture type at read time (native follows the live
 * endian register; suffixed bakes be/le). `width` is in BITS. */
bbq_capture_type bbq_view_int_capture(const bbq_view_ctx_t* c, int width, bool sgn, bool be, bool native);
bbq_capture_type bbq_view_float_capture(const bbq_view_ctx_t* c, int width, bool be, bool native);

#endif /* BBQ_LITE_H */
