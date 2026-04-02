/*
 * bbq_buf.h — Growable byte/string buffer for BBQ generated code.
 *
 * Usage:
 *   bbq_buf b;
 *   bbq_buf_init(&b);
 *   bbq_buf_append(&b, data, len);
 *   bbq_buf_free(&b);
 */
#ifndef BBQ_BUF_H
#define BBQ_BUF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
} bbq_buf;

void bbq_buf_init(bbq_buf* b);
void bbq_buf_append(bbq_buf* b, const void* src, size_t n);
void bbq_buf_reserve(bbq_buf* b, size_t total);
void bbq_buf_clear(bbq_buf* b);
void bbq_buf_free(bbq_buf* b);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_BUF_H */
