/* ============================================================
 * peg_runtime.h — C runtime for pegc-generated parsers
 *
 * Provides cursor-based parsing with save/restore for PEG
 * backtracking, character/string matching, whitespace+comment
 * skipping, and basic error reporting.
 *
 * Shipped with generated parsers (not generated itself).
 * ============================================================ */
#ifndef PEG_RUNTIME_H
#define PEG_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Position mark for backtracking ──────────────────────── */

typedef struct {
    const char* pos;
    int line;
    int col;
} peg_mark;

/* ── Zero-copy span (pointer into input buffer) ─────────── */

typedef struct {
    const char* ptr;
    int len;
} peg_span;

/* ── Comment specification ───────────────────────────────── */

typedef struct {
    const char* open;
    const char* close;
    bool nested;
} peg_comment_spec;

/* ── Parse error ─────────────────────────────────────────── */

#define PEG_MAX_ERRORS   16
#define PEG_MAX_COMMENTS 4
#define PEG_ERROR_LEN    128

typedef struct {
    int line;
    int col;
    char message[PEG_ERROR_LEN];
} peg_error;

/* ── Parser state ────────────────────────────────────────── */

typedef struct {
    const char* input;
    const char* pos;
    const char* end;
    int line;
    int col;

    bool (*ws_fn)(char);
    peg_comment_spec comments[PEG_MAX_COMMENTS];
    int comment_count;

    peg_error errors[PEG_MAX_ERRORS];
    int error_count;

    peg_mark furthest;
} peg_state;

/* ── Init ────────────────────────────────────────────────── */

static inline void peg_init(peg_state* p, const char* input, int length) {
    memset(p, 0, sizeof(*p));
    p->input = input;
    p->pos = input;
    p->end = input + length;
    p->line = 1;
    p->col = 1;
}

/* ── Position ────────────────────────────────────────────── */

static inline int peg_line(const peg_state* p) { return p->line; }
static inline int peg_col(const peg_state* p) { return p->col; }
static inline bool peg_at_end(const peg_state* p) { return p->pos >= p->end; }
static inline const char* peg_pos(const peg_state* p) { return p->pos; }
static inline int peg_remaining(const peg_state* p) { return (int)(p->end - p->pos); }
static inline char peg_peek_char(const peg_state* p) { return p->pos < p->end ? *p->pos : '\0'; }

/* ── Advance (internal) ─────────────────────────────────── */

static inline void peg_advance(peg_state* p) {
    if (p->pos >= p->end) return;
    if (*p->pos == '\n') { p->line++; p->col = 1; }
    else { p->col++; }
    p->pos++;
    if (p->pos > p->furthest.pos) {
        p->furthest.pos = p->pos;
        p->furthest.line = p->line;
        p->furthest.col = p->col;
    }
}

/* ── Save / Restore ──────────────────────────────────────── */

static inline peg_mark peg_save(const peg_state* p) {
    peg_mark m = { p->pos, p->line, p->col };
    return m;
}

static inline void peg_restore(peg_state* p, peg_mark m) {
    p->pos = m.pos;
    p->line = m.line;
    p->col = m.col;
}

/* ── Character matching ──────────────────────────────────── */

static inline bool peg_match_char(peg_state* p, char c) {
    if (p->pos < p->end && *p->pos == c) {
        peg_advance(p);
        return true;
    }
    return false;
}

static inline bool peg_match(peg_state* p, const char* str) {
    int len = (int)strlen(str);
    if (p->pos + len > p->end) return false;
    if (memcmp(p->pos, str, (size_t)len) != 0) return false;
    for (int i = 0; i < len; i++) peg_advance(p);
    return true;
}

static inline bool peg_match_charset(peg_state* p, bool (*fn)(char)) {
    if (p->pos < p->end && fn(*p->pos)) {
        peg_advance(p);
        return true;
    }
    return false;
}

/* ── Lookahead (no consume) ──────────────────────────────── */

static inline bool peg_peek_at(const peg_state* p, const char* str) {
    int len = (int)strlen(str);
    if (p->pos + len > p->end) return false;
    return memcmp(p->pos, str, (size_t)len) == 0;
}

static inline bool peg_peek_at_char(const peg_state* p, char c) {
    return p->pos < p->end && *p->pos == c;
}

static inline bool peg_peek_charset(const peg_state* p, bool (*fn)(char)) {
    return p->pos < p->end && fn(*p->pos);
}

/* ── Span (consume while charset matches) ────────────────── */

static inline void peg_span_chars(peg_state* p, bool (*fn)(char)) {
    while (p->pos < p->end && fn(*p->pos))
        peg_advance(p);
}

/* ── Character classification ────────────────────────────── */

static inline bool peg_is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline bool peg_is_ident_continue(char c) {
    return peg_is_ident_start(c) || (c >= '0' && c <= '9');
}

static inline bool peg_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline bool peg_is_hex_digit(char c) {
    return peg_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* ── Keyword matching ────────────────────────────────────── */

static inline bool peg_keyword(peg_state* p, const char* kw) {
    int len = (int)strlen(kw);
    if (p->pos + len > p->end) return false;
    if (memcmp(p->pos, kw, (size_t)len) != 0) return false;
    if (p->pos + len < p->end && peg_is_ident_continue(p->pos[len])) return false;
    for (int i = 0; i < len; i++) peg_advance(p);
    return true;
}

static inline bool peg_peek_keyword(const peg_state* p, const char* kw) {
    int len = (int)strlen(kw);
    if (p->pos + len > p->end) return false;
    if (memcmp(p->pos, kw, (size_t)len) != 0) return false;
    if (p->pos + len < p->end && peg_is_ident_continue(p->pos[len])) return false;
    return true;
}

/* ── Built-in token patterns ─────────────────────────────── */

static inline bool peg_ident(peg_state* p, peg_span* out) {
    if (p->pos >= p->end || !peg_is_ident_start(*p->pos)) return false;
    const char* start = p->pos;
    peg_advance(p);
    while (p->pos < p->end && peg_is_ident_continue(*p->pos)) peg_advance(p);
    out->ptr = start;
    out->len = (int)(p->pos - start);
    return true;
}

static inline bool peg_integer(peg_state* p, int64_t* out) {
    if (p->pos >= p->end || !peg_is_digit(*p->pos)) return false;
    const char* start = p->pos;
    if (*p->pos == '0' && p->pos + 1 < p->end) {
        char next = p->pos[1];
        if (next == 'x' || next == 'X') {
            peg_advance(p); peg_advance(p);
            if (p->pos >= p->end || !peg_is_hex_digit(*p->pos)) return false;
            while (p->pos < p->end && peg_is_hex_digit(*p->pos)) peg_advance(p);
            *out = strtoll(start, NULL, 16);
            return true;
        }
        if (next == 'b' || next == 'B') {
            peg_advance(p); peg_advance(p);
            if (p->pos >= p->end || (*p->pos != '0' && *p->pos != '1')) return false;
            while (p->pos < p->end && (*p->pos == '0' || *p->pos == '1')) peg_advance(p);
            *out = strtoll(start + 2, NULL, 2);
            return true;
        }
    }
    while (p->pos < p->end && peg_is_digit(*p->pos)) peg_advance(p);
    *out = strtoll(start, NULL, 10);
    return true;
}

static inline bool peg_floating(peg_state* p, double* out) {
    if (p->pos >= p->end || !peg_is_digit(*p->pos)) return false;
    const char* start = p->pos;
    while (p->pos < p->end && peg_is_digit(*p->pos)) peg_advance(p);
    if (p->pos >= p->end || *p->pos != '.') { p->pos = start; return false; }
    peg_advance(p);
    while (p->pos < p->end && peg_is_digit(*p->pos)) peg_advance(p);
    *out = strtod(start, NULL);
    return true;
}

/* Scan forward to delimiter, capturing raw content as span. */
static inline bool peg_scan_to(peg_state* p, const char* delim, peg_span* out) {
    int dlen = (int)strlen(delim);
    const char* start = p->pos;
    while (p->pos + dlen <= p->end) {
        if (memcmp(p->pos, delim, (size_t)dlen) == 0) {
            out->ptr = start;
            out->len = (int)(p->pos - start);
            for (int i = 0; i < dlen; i++) peg_advance(p);
            return true;
        }
        peg_advance(p);
    }
    return false;
}

static inline bool peg_char_lit(peg_state* p, char* out) {
    if (p->pos >= p->end || *p->pos != '\'') return false;
    peg_advance(p);
    if (p->pos >= p->end) return false;
    if (*p->pos == '\\' && p->pos + 1 < p->end) {
        peg_advance(p);
        switch (*p->pos) {
            case 'n': *out = '\n'; break;
            case 'r': *out = '\r'; break;
            case 't': *out = '\t'; break;
            case '\\': *out = '\\'; break;
            case '\'': *out = '\''; break;
            case '0': *out = '\0'; break;
            default: *out = *p->pos; break;
        }
    } else {
        *out = *p->pos;
    }
    peg_advance(p);
    if (p->pos < p->end && *p->pos == '\'') peg_advance(p);
    return true;
}

/* Match string literal: "..." with escape unquoting into a buffer. */
static inline bool peg_string_lit(peg_state* p, peg_span* out) {
    if (p->pos >= p->end || *p->pos != '"') return false;
    peg_advance(p); /* skip opening " */
    const char* start = p->pos;
    while (p->pos < p->end && *p->pos != '"') {
        if (*p->pos == '\\' && p->pos + 1 < p->end)
            peg_advance(p); /* skip escape char */
        peg_advance(p);
    }
    out->ptr = start;
    out->len = (int)(p->pos - start);
    if (p->pos < p->end) peg_advance(p); /* skip closing " */
    return true;
}

/* ── Whitespace + comment skipping ───────────────────────── */

static inline void peg_set_whitespace(peg_state* p, bool (*fn)(char)) {
    p->ws_fn = fn;
}

static inline void peg_add_comment(peg_state* p, const char* open,
                                    const char* close, bool nested) {
    if (p->comment_count < PEG_MAX_COMMENTS) {
        p->comments[p->comment_count].open = open;
        p->comments[p->comment_count].close = close;
        p->comments[p->comment_count].nested = nested;
        p->comment_count++;
    }
}

static inline bool peg_skip_comment(peg_state* p, const peg_comment_spec* spec) {
    int open_len = (int)strlen(spec->open);
    if (p->pos + open_len > p->end) return false;
    if (memcmp(p->pos, spec->open, (size_t)open_len) != 0) return false;
    for (int i = 0; i < open_len; i++) peg_advance(p);
    int close_len = (int)strlen(spec->close);
    int depth = 1;
    while (p->pos < p->end && depth > 0) {
        if (spec->nested && p->pos + open_len <= p->end &&
            memcmp(p->pos, spec->open, (size_t)open_len) == 0) {
            for (int i = 0; i < open_len; i++) peg_advance(p);
            depth++;
        } else if (p->pos + close_len <= p->end &&
                   memcmp(p->pos, spec->close, (size_t)close_len) == 0) {
            for (int i = 0; i < close_len; i++) peg_advance(p);
            depth--;
        } else {
            peg_advance(p);
        }
    }
    return true;
}

static inline void peg_skip(peg_state* p) {
    for (;;) {
        bool skipped = false;
        while (p->pos < p->end && p->ws_fn && p->ws_fn(*p->pos)) {
            peg_advance(p);
            skipped = true;
        }
        bool found_comment = false;
        for (int i = 0; i < p->comment_count; i++) {
            if (peg_skip_comment(p, &p->comments[i])) {
                found_comment = true;
                break;
            }
        }
        if (!found_comment && !skipped) break;
    }
}

/* ── Error reporting ─────────────────────────────────────── */

static inline void peg_expected(peg_state* p, const char* what) {
    if (p->error_count < PEG_MAX_ERRORS) {
        peg_error* e = &p->errors[p->error_count++];
        e->line = p->line;
        e->col = p->col;
        snprintf(e->message, PEG_ERROR_LEN, "expected %s", what);
    }
}

static inline void peg_error_msg(peg_state* p, const char* msg) {
    if (p->error_count < PEG_MAX_ERRORS) {
        peg_error* e = &p->errors[p->error_count++];
        e->line = p->line;
        e->col = p->col;
        snprintf(e->message, PEG_ERROR_LEN, "%s", msg);
    }
}

static inline bool peg_has_errors(const peg_state* p) {
    return p->error_count > 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PEG_RUNTIME_H */
