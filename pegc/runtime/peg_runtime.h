/* ============================================================
 * peg_runtime.h — Public types for pegc-generated C parsers
 *
 * Generated parsers embed their own copy of the runtime functions
 * (file-scope static, see CParserImpl.frame). This header carries only
 * the public types so callers can declare peg_state, inspect peg_error,
 * etc. without pulling the function bodies into every TU.
 * ============================================================ */
#ifndef PEG_RUNTIME_H
#define PEG_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
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

    void* user_data;
} peg_state;

#ifdef __cplusplus
}
#endif

#endif /* PEG_RUNTIME_H */
