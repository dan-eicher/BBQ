/* bbq_lite.c — c-lite index runtime implementation (see bbq_lite.h). */
#include "bbq_lite.h"

#include <string.h>

/* ── byte-level decoders at an offset (the dual of bbq_read.h's cursor reads;
 *    decode-on-access needs offset-addressed, no-advance atoms) ── */
static uint8_t  rd_u8 (const uint8_t* b, size_t o) { return b[o]; }
static uint16_t rd_u16le(const uint8_t* b, size_t o) {
    return (uint16_t)((uint16_t)b[o] | ((uint16_t)b[o + 1] << 8));
}
static uint16_t rd_u16be(const uint8_t* b, size_t o) {
    return (uint16_t)(((uint16_t)b[o] << 8) | (uint16_t)b[o + 1]);
}
static uint32_t rd_u32le(const uint8_t* b, size_t o) {
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
           ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}
static uint32_t rd_u32be(const uint8_t* b, size_t o) {
    return ((uint32_t)b[o] << 24) | ((uint32_t)b[o + 1] << 16) |
           ((uint32_t)b[o + 2] << 8) | (uint32_t)b[o + 3];
}
static uint64_t rd_u64le(const uint8_t* b, size_t o) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)b[o + i] << (i * 8); return v;
}
static uint64_t rd_u64be(const uint8_t* b, size_t o) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)b[o + i] << ((7 - i) * 8); return v;
}
static float  rd_f32le(const uint8_t* b, size_t o) { uint32_t r = rd_u32le(b, o); float f; memcpy(&f, &r, 4); return f; }
static float  rd_f32be(const uint8_t* b, size_t o) { uint32_t r = rd_u32be(b, o); float f; memcpy(&f, &r, 4); return f; }
static double rd_f64le(const uint8_t* b, size_t o) { uint64_t r = rd_u64le(b, o); double d; memcpy(&d, &r, 8); return d; }
static double rd_f64be(const uint8_t* b, size_t o) { uint64_t r = rd_u64be(b, o); double d; memcpy(&d, &r, 8); return d; }

/* ── builder internals ── */

static int take_pending(bbq_capture_builder* b) {
    int t = b->pending_variant_tag;
    b->pending_variant_tag = -1;
    return t;
}

static void begin_scope(bbq_capture_builder* b, const char* name, size_t pos,
                        bbq_capture_type type, int variant_tag) {
    int tag = (variant_tag != -1) ? variant_tag : take_pending(b);
    bbq_capture_scope s;
    s.name = name; s.start_pos = pos; s.first_child_index = bbq_vec_len(b->fields);
    s.type = type; s.variant_tag = tag;
    bbq_vec_push(b->scopes, s);
}

static void end_scope(bbq_capture_builder* b, size_t pos) {
    bbq_capture_scope scope = bbq_vec_last(b->scopes);
    int first = scope.first_child_index;
    int count = bbq_vec_len(b->fields) - first;

    bbq_field_capture fc;
    fc.name = scope.name;
    fc.start_offset = scope.start_pos;
    fc.end_offset = pos;
    fc.type = scope.type;
    fc.children = NULL;
    fc.child_count = count;
    fc.build_buf_index = -1;
    fc.variant_tag = scope.variant_tag;
    fc.computed_value = NULL;

    if (count > 0) {
        int buf_start = bbq_vec_len(b->child_buf);
        for (int i = first; i < bbq_vec_len(b->fields); i++)
            bbq_vec_push(b->child_buf, b->fields[i]);
        fc.build_buf_index = buf_start;
    }
    bbq_vec_truncate(b->fields, first);
    bbq_vec_push(b->fields, fc);
    bbq_vec_truncate(b->scopes, bbq_vec_len(b->scopes) - 1);
}

static void add_computed_span(bbq_capture_builder* b, const char* name,
                              bbq_computed_value* cv, size_t start, size_t end) {
    bbq_field_capture fc;
    fc.name = name; fc.start_offset = start; fc.end_offset = end;
    fc.type = BBQ_CT_Computed; fc.children = NULL; fc.child_count = 0;
    fc.build_buf_index = -1; fc.variant_tag = take_pending(b); fc.computed_value = cv;
    bbq_vec_push(b->fields, fc);
}

/* ── builder public API ── */

void bbq_cap_begin_struct(bbq_capture_builder* b, const char* name, size_t pos) {
    begin_scope(b, name, pos, BBQ_CT_Struct, -1);
}
void bbq_cap_end_struct(bbq_capture_builder* b, size_t pos) { end_scope(b, pos); }
void bbq_cap_begin_array(bbq_capture_builder* b, const char* name, size_t pos) {
    begin_scope(b, name, pos, BBQ_CT_Array, -1);
}
void bbq_cap_end_array(bbq_capture_builder* b, size_t pos) { end_scope(b, pos); }
void bbq_cap_begin_variant(bbq_capture_builder* b, const char* name, size_t pos, int tag) {
    begin_scope(b, name, pos, BBQ_CT_Struct, tag);
}

void bbq_cap_add_field(bbq_capture_builder* b, const char* name, size_t start,
                       size_t end, bbq_capture_type type, bbq_computed_value* cv) {
    bbq_field_capture fc;
    fc.name = name; fc.start_offset = start; fc.end_offset = end; fc.type = type;
    fc.children = NULL; fc.child_count = 0; fc.build_buf_index = -1;
    fc.variant_tag = take_pending(b); fc.computed_value = cv;
    bbq_vec_push(b->fields, fc);
}

void bbq_cap_set_pending_variant_tag(bbq_capture_builder* b, int tag) {
    b->pending_variant_tag = tag;
}

bbq_capture_mark_t bbq_cap_mark(const bbq_capture_builder* b) {
    bbq_capture_mark_t m = { bbq_vec_len(b->scopes), bbq_vec_len(b->fields),
                             bbq_vec_len(b->child_buf) };
    return m;
}

void bbq_cap_restore(bbq_capture_builder* b, bbq_capture_mark_t m) {
    bbq_vec_truncate(b->scopes, m.depth);
    if (m.child_index < bbq_vec_len(b->fields)) bbq_vec_truncate(b->fields, m.child_index);
    if (m.child_buf_index < bbq_vec_len(b->child_buf)) bbq_vec_truncate(b->child_buf, m.child_buf_index);
}

size_t bbq_cap_current_scope_start(const bbq_capture_builder* b) {
    int n = bbq_vec_len(b->scopes);
    return n == 0 ? 0 : b->scopes[n - 1].start_pos;
}

const bbq_field_capture* bbq_cap_find_field_str(const bbq_capture_builder* b, const char* name) {
    for (int i = bbq_vec_len(b->fields); i > 0; --i)
        if (b->fields[i - 1].name && strcmp(b->fields[i - 1].name, name) == 0)
            return &b->fields[i - 1];
    return NULL;
}

const bbq_field_capture* bbq_cap_find_child_str(const bbq_capture_builder* b,
                                                const bbq_field_capture* parent, const char* name) {
    if (!parent || parent->child_count <= 0 || parent->build_buf_index < 0) return NULL;
    int s = parent->build_buf_index;
    for (int i = 0; i < parent->child_count; i++)
        if (b->child_buf[s + i].name && strcmp(b->child_buf[s + i].name, name) == 0)
            return &b->child_buf[s + i];
    return NULL;
}

const bbq_field_capture* bbq_cap_find_child_at(const bbq_capture_builder* b,
                                               const bbq_field_capture* parent, int index) {
    if (!parent || index < 0 || index >= parent->child_count || parent->build_buf_index < 0)
        return NULL;
    return &b->child_buf[parent->build_buf_index + index];
}

/* Recursively copy build-phase captures into the arena, resolving build_buf_index
 * → a real children pointer (the dual of the C++ CaptureBuilder::copy_to_arena). */
static bbq_field_capture* copy_to_arena(bbq_capture_builder* b, const bbq_field_capture* src, int count) {
    if (count == 0) return NULL;
    bbq_field_capture* dst = (bbq_field_capture*)bbq_arena_alloc(b->arena,
                                 (size_t)count * sizeof(bbq_field_capture));
    memcpy(dst, src, (size_t)count * sizeof(bbq_field_capture));
    for (int i = 0; i < count; i++) {
        if ((dst[i].type == BBQ_CT_Struct || dst[i].type == BBQ_CT_Array)
            && dst[i].child_count > 0 && dst[i].build_buf_index >= 0) {
            dst[i].children = copy_to_arena(b, b->child_buf + dst[i].build_buf_index, dst[i].child_count);
            dst[i].build_buf_index = -1;
        }
    }
    return dst;
}

/* ── backtrack ── */

bbq_view_checkpoint_t bbq_view_save(bbq_view_ctx_t* c) {
    bbq_view_checkpoint_t cp;
    cp.cur = bbq_save(&c->cur);
    cp.mark = bbq_cap_mark(&c->builder);
    return cp;
}

void bbq_view_restore(bbq_view_ctx_t* c, bbq_view_checkpoint_t cp) {
    bbq_restore(&c->cur, cp.cur);
    bbq_cap_restore(&c->builder, cp.mark);
}

/* ── view ctx lifecycle ── */

void bbq_view_ctx_init(bbq_view_ctx_t* c, const uint8_t* data, size_t len, bbq_arena* arena) {
    bbq_ctx_init(&c->cur, data, len);
    c->builder.fields = NULL;
    c->builder.scopes = NULL;
    c->builder.child_buf = NULL;
    c->builder.pending_variant_tag = -1;
    c->builder.arena = arena;   /* caller-owned; outlives the returned metadata */
}

void bbq_view_build_free(bbq_view_ctx_t* c) {
    bbq_vec_free(c->builder.fields);
    bbq_vec_free(c->builder.scopes);
    bbq_vec_free(c->builder.child_buf);
    bbq_ctx_free(&c->cur);   /* the cursor's growable parse stacks */
}

bbq_capture_metadata bbq_view_finish(bbq_view_ctx_t* c, bool ok) {
    bbq_capture_builder* b = &c->builder;
    bbq_capture_metadata meta;
    meta.success = ok;
    meta.bytes_consumed = c->cur.pos;
    /* The cursor's error buffer lives on the caller's frame and dies with it,
     * so the message has to be copied into the arena the metadata outlives. */
    meta.error_message = NULL;
    if (!ok && c->cur.has_error) {
        size_t n = strlen(c->cur.error) + 1;
        char* copy = (char*)bbq_arena_alloc(b->arena, n);
        if (copy) { memcpy(copy, c->cur.error, n); meta.error_message = copy; }
    }
    meta.error_offset = c->cur.pos;
    meta.root = NULL;

    int n = bbq_vec_len(b->fields);
    if (n > 0) {
        bbq_field_capture* top = copy_to_arena(b, b->fields, n);
        bbq_field_capture* root = (bbq_field_capture*)bbq_arena_alloc(b->arena, sizeof(bbq_field_capture));
        root->name = NULL;
        root->start_offset = 0;
        root->end_offset = c->cur.pos;
        root->type = BBQ_CT_Struct;
        root->children = top;
        root->child_count = n;
        root->build_buf_index = -1;
        root->variant_tag = -1;
        root->computed_value = NULL;
        meta.root = root;
    }
    return meta;
}

/* ── decode-on-access ── */

bool bbq_decode_int(const bbq_field_capture* cap, const uint8_t* buf, int64_t* bits, bool* sgn) {
    size_t o = cap->start_offset;
    switch (cap->type) {
        case BBQ_CT_UInt8:    *bits = (int64_t)(uint64_t)rd_u8(buf, o);    *sgn = false; return true;
        case BBQ_CT_UInt16LE: *bits = (int64_t)(uint64_t)rd_u16le(buf, o); *sgn = false; return true;
        case BBQ_CT_UInt16BE: *bits = (int64_t)(uint64_t)rd_u16be(buf, o); *sgn = false; return true;
        case BBQ_CT_UInt32LE: *bits = (int64_t)(uint64_t)rd_u32le(buf, o); *sgn = false; return true;
        case BBQ_CT_UInt32BE: *bits = (int64_t)(uint64_t)rd_u32be(buf, o); *sgn = false; return true;
        case BBQ_CT_UInt64LE: *bits = (int64_t)rd_u64le(buf, o);           *sgn = false; return true;
        case BBQ_CT_UInt64BE: *bits = (int64_t)rd_u64be(buf, o);           *sgn = false; return true;
        case BBQ_CT_Int8:     *bits = (int8_t)rd_u8(buf, o);    *sgn = true; return true;
        case BBQ_CT_Int16LE:  *bits = (int16_t)rd_u16le(buf, o); *sgn = true; return true;
        case BBQ_CT_Int16BE:  *bits = (int16_t)rd_u16be(buf, o); *sgn = true; return true;
        case BBQ_CT_Int32LE:  *bits = (int32_t)rd_u32le(buf, o); *sgn = true; return true;
        case BBQ_CT_Int32BE:  *bits = (int32_t)rd_u32be(buf, o); *sgn = true; return true;
        case BBQ_CT_Int64LE:  *bits = (int64_t)rd_u64le(buf, o); *sgn = true; return true;
        case BBQ_CT_Int64BE:  *bits = (int64_t)rd_u64be(buf, o); *sgn = true; return true;
        case BBQ_CT_Bool:     *bits = buf[o] ? 1 : 0; *sgn = true; return true;
        case BBQ_CT_Computed:
            *sgn = true;
            if (!cap->computed_value) { *bits = 0; return true; }
            switch (cap->computed_value->kind) {
                case BBQ_CV_INT:   *bits = cap->computed_value->i; return true;
                case BBQ_CV_BOOL:  *bits = cap->computed_value->b ? 1 : 0; return true;
                case BBQ_CV_FLOAT: *bits = (int64_t)cap->computed_value->f; return true;
                default: return false;
            }
        default:
            return false;
    }
}

bool bbq_decode_float(const bbq_field_capture* cap, const uint8_t* buf, double* out) {
    size_t o = cap->start_offset;
    switch (cap->type) {
        case BBQ_CT_Float32LE: *out = rd_f32le(buf, o); return true;
        case BBQ_CT_Float32BE: *out = rd_f32be(buf, o); return true;
        case BBQ_CT_Float64LE: *out = rd_f64le(buf, o); return true;
        case BBQ_CT_Float64BE: *out = rd_f64be(buf, o); return true;
        default: return false;
    }
}

int64_t bbq_node_int(const bbq_field_capture* cap, const uint8_t* buf) {
    int64_t bits = 0; bool sgn = false;
    if (cap) bbq_decode_int(cap, buf, &bits, &sgn);
    return bits;
}

double bbq_node_float(const bbq_field_capture* cap, const uint8_t* buf) {
    double d = 0; if (cap) bbq_decode_float(cap, buf, &d); return d;
}

int64_t bbq_view_i64(bbq_view_ctx_t* c, const char* name) {
    const bbq_field_capture* cap = bbq_cap_find_field_str(&c->builder, name);
    int64_t bits = 0; bool sgn = false;
    if (cap) bbq_decode_int(cap, c->cur.data, &bits, &sgn);
    return bits;
}

/* Recover a prior NON-SCALAR (Bytes/String/External) field in scope as its
 * [start,end) span — the field_ref spelling for a where-clause over such a field
 * (the dual of bbq_view_i64; a Bytes capture has no integer value, only a span). */
bbq_bytes_t bbq_view_bytes(bbq_view_ctx_t* c, const char* name) {
    const bbq_field_capture* cap = bbq_cap_find_field_str(&c->builder, name);
    if (!cap) { bbq_bytes_t z = { 0, 0 }; return z; }
    bbq_bytes_t b = { c->cur.data + cap->start_offset,
                      (size_t)(cap->end_offset - cap->start_offset) };
    return b;
}

/* ── view read points ── */

static bbq_computed_value* alloc_cv(bbq_view_ctx_t* c) {
    return (bbq_computed_value*)bbq_arena_alloc(c->builder.arena, sizeof(bbq_computed_value));
}

void bbq_view_add_computed_int(bbq_view_ctx_t* c, const char* name, int64_t v) {
    bbq_computed_value* cv = alloc_cv(c);
    cv->kind = BBQ_CV_INT; cv->i = v;
    add_computed_span(&c->builder, name, cv, c->cur.pos, c->cur.pos);
}
void bbq_view_add_computed_float(bbq_view_ctx_t* c, const char* name, double v) {
    bbq_computed_value* cv = alloc_cv(c);
    cv->kind = BBQ_CV_FLOAT; cv->f = v;
    add_computed_span(&c->builder, name, cv, c->cur.pos, c->cur.pos);
}
void bbq_view_add_computed_bool(bbq_view_ctx_t* c, const char* name, bool v) {
    bbq_computed_value* cv = alloc_cv(c);
    cv->kind = BBQ_CV_BOOL; cv->b = v;
    add_computed_span(&c->builder, name, cv, c->cur.pos, c->cur.pos);
}

bool bbq_view_uleb_capture(bbq_view_ctx_t* c, const char* name, int bits) {
    size_t start = c->cur.pos;
    uint64_t v;
    if (!bbq_read_uleb128(&c->cur, &v, bits)) return false;
    bbq_computed_value* cv = alloc_cv(c);
    cv->kind = BBQ_CV_INT; cv->i = (int64_t)v;
    add_computed_span(&c->builder, name, cv, start, c->cur.pos);
    return true;
}

bool bbq_view_sleb_capture(bbq_view_ctx_t* c, const char* name, int bits) {
    size_t start = c->cur.pos;
    int64_t v;
    if (!bbq_read_sleb128(&c->cur, &v, bits)) return false;
    bbq_computed_value* cv = alloc_cv(c);
    cv->kind = BBQ_CV_INT; cv->i = v;
    add_computed_span(&c->builder, name, cv, start, c->cur.pos);
    return true;
}

uint64_t bbq_view_read_at(const bbq_view_ctx_t* c, size_t offset, int width_bytes,
                          bool be, bool native) {
    bool b = native ? !c->cur.little_endian : be;
    const uint8_t* d = c->cur.data;
    uint64_t v = 0;
    if (b) for (int i = 0; i < width_bytes; i++) v = (v << 8) | d[offset + i];
    else   for (int i = 0; i < width_bytes; i++) v |= (uint64_t)d[offset + i] << (8 * i);
    return v;
}

bbq_capture_type bbq_view_int_capture(const bbq_view_ctx_t* c, int width, bool sgn, bool be, bool native) {
    bool b = native ? !c->cur.little_endian : be;
    switch (width) {
        case 8:  return sgn ? BBQ_CT_Int8 : BBQ_CT_UInt8;
        case 16: return sgn ? (b ? BBQ_CT_Int16BE : BBQ_CT_Int16LE)
                            : (b ? BBQ_CT_UInt16BE : BBQ_CT_UInt16LE);
        case 32: return sgn ? (b ? BBQ_CT_Int32BE : BBQ_CT_Int32LE)
                            : (b ? BBQ_CT_UInt32BE : BBQ_CT_UInt32LE);
        default: return sgn ? (b ? BBQ_CT_Int64BE : BBQ_CT_Int64LE)
                            : (b ? BBQ_CT_UInt64BE : BBQ_CT_UInt64LE);
    }
}

bbq_capture_type bbq_view_float_capture(const bbq_view_ctx_t* c, int width, bool be, bool native) {
    bool b = native ? !c->cur.little_endian : be;
    if (width == 64) return b ? BBQ_CT_Float64BE : BBQ_CT_Float64LE;
    return b ? BBQ_CT_Float32BE : BBQ_CT_Float32LE;
}
