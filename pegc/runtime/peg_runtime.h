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

/* ── Internal linkage for generated runtime helpers ──────────
 * Generated parsers paste a fixed pool of helpers (file-scope static).
 * Any given grammar exercises only a subset, so we mark them as
 * possibly-unused for compilers that flag dead static functions.
 * Combine `static` + maybe-unused into one macro so frames stay clean
 * and adding/removing attributes touches only this header. */
#if defined(__cplusplus) && __cplusplus >= 201703L
#  define PEG_INTERNAL [[maybe_unused]] static
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#  define PEG_INTERNAL [[maybe_unused]] static
#elif defined(__GNUC__) || defined(__clang__)
#  define PEG_INTERNAL static __attribute__((unused))
#else
#  define PEG_INTERNAL static
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
    int  open_len;     /* strlen(open/close), measured ONCE at peg_add_comment —  */
    int  close_len;    /* the skip loop tries every spec at every token boundary  */
    bool nested;
    bool structured;   /* skip a balanced ()-nested, string-aware token group (annotations); inner-comment awareness is a frame override */
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
    bool ws_tab[256];  /* ws_fn tabulated at peg_set_whitespace — the skip loop had
                        * an indirect call per character; the predicate is pure, so
                        * the table is exact */
    peg_comment_spec comments[PEG_MAX_COMMENTS];
    int comment_count;

    /* The skip memo. PEG backtracking re-enters peg_skip at positions it has
     * already skipped (measured ~20 re-classifications per source character on a
     * real corpus); skipping is DETERMINISTIC in pos, so one (from → to) pair
     * erases nearly all of it. line/col are cached WITH the target — they are a
     * pure function of position, and jumping pos without them would desync
     * diagnostics. */
    const char* skip_from;
    const char* skip_to;
    int skip_to_line;
    int skip_to_col;

    peg_error errors[PEG_MAX_ERRORS];
    int error_count;

    peg_mark furthest;

    void* user_data;
} peg_state;

#ifdef __cplusplus
}
#endif

#endif /* PEG_RUNTIME_H */
