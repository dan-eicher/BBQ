#include "bbq_buf.h"
#include <stdlib.h>
#include <string.h>

void bbq_buf_init(bbq_buf* b) {
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

void bbq_buf_reserve(bbq_buf* b, size_t total) {
    if (total <= b->cap) return;
    size_t newcap = b->cap > 0 ? b->cap : 64;
    while (newcap < total) newcap *= 2;
    uint8_t* nd = (uint8_t*)realloc(b->data, newcap);
    if (!nd) return;
    b->data = nd;
    b->cap  = newcap;
}

void bbq_buf_append(bbq_buf* b, const void* src, size_t n) {
    if (n == 0) return;
    bbq_buf_reserve(b, b->len + n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

void bbq_buf_clear(bbq_buf* b) {
    b->len = 0;
}

void bbq_buf_free(bbq_buf* b) {
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}
