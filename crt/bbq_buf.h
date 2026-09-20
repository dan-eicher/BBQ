/*
 * bbq_buf.h — Growable byte buffer.
 *
 *   bbq_buf b;
 *   bbq_buf_init(&b, NULL);          // NULL = libc
 *   if (!bbq_buf_append(&b, data, len)) { ... }
 *   bbq_buf_free(&b);
 *
 * FAILURE CONTRACT, as everywhere in the CRT: an allocation that fails poisons
 * the buffer. Every later append is a no-op, the bytes already in it stay valid
 * and readable, bbq_buf_oom() reports it, and nothing is ever written past the
 * end. The append entry points also RETURN whether they took, because a caller
 * assembling a message usually can stop at the first refusal rather than
 * discovering at the end that it is short.
 *
 * The previous shape was a void bbq_buf_append that called a void bbq_buf_reserve
 * which returned silently when realloc failed — and then memcpy'd into the buffer
 * that had not grown.
 */
#ifndef BBQ_BUF_H
#define BBQ_BUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bbq_alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t*   data;
    size_t     len;
    size_t     cap;
    bbq_alloc* a;
    bool       oom;
} bbq_buf;

/* The buffer is usable immediately; nothing is allocated until the first append
 * or reserve. The `_a` form names the allocator to use for its lifetime. */
void bbq_buf_init(bbq_buf* b);
void bbq_buf_init_a(bbq_buf* b, bbq_alloc* a);

/* Release the bytes. The buffer is left valid and reusable, with its allocator
 * and its OOM state both cleared. */
void bbq_buf_free(bbq_buf* b);

/* Capacity for at least `total` bytes. False if it could not be had, which also
 * poisons the buffer. A `total` that cannot be represented is refused rather
 * than wrapped — the old doubling loop overflowed to zero and never terminated
 * for anything above SIZE_MAX/2. */
bool bbq_buf_reserve(bbq_buf* b, size_t total);

/* Append n bytes. False (and nothing written) if the buffer could not grow or
 * was already poisoned. n == 0 is a no-op and succeeds, whatever `src` is. */
bool bbq_buf_append(bbq_buf* b, const void* src, size_t n);
bool bbq_buf_append_byte(bbq_buf* b, uint8_t byte);

/* Drop the contents, keep the capacity. Does NOT clear the OOM state: the buffer
 * could not hold what it was given, and reusing it without noticing is the bug
 * this flag exists to prevent. bbq_buf_free then bbq_buf_init resets it. */
void bbq_buf_clear(bbq_buf* b);

/* Did this buffer ever fail to grow? Its bytes are valid; they are just not all
 * of what was appended. */
bool bbq_buf_oom(const bbq_buf* b);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_BUF_H */
