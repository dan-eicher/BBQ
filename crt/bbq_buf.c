#include "bbq_buf.h"

#include <string.h>

#define BBQ_BUF_MIN_CAP 64

void bbq_buf_init(bbq_buf* b) { bbq_buf_init_a(b, NULL); }

void bbq_buf_init_a(bbq_buf* b, bbq_alloc* a) {
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
    b->a    = a;
    b->oom  = false;
}

void bbq_buf_free(bbq_buf* b) {
    bbq_mem_release(b->a, b->data, b->cap);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
    b->oom  = false;
}

bool bbq_buf_oom(const bbq_buf* b) { return b->oom; }

void bbq_buf_clear(bbq_buf* b) { b->len = 0; }

bool bbq_buf_reserve(bbq_buf* b, size_t total) {
    size_t newcap;
    uint8_t* nd;

    if (b->oom) return false;
    if (total <= b->cap) return true;

    /* Double until it covers `total`, in a form that cannot wrap. The previous
     * `while (newcap < total) newcap *= 2` reached 0 for any total above
     * SIZE_MAX/2 and then looped forever — a hang, which for a library fed
     * hostile input is the same denial of service as a crash. */
    newcap = b->cap ? b->cap : BBQ_BUF_MIN_CAP;
    while (newcap < total) {
        if (newcap > SIZE_MAX / 2) { newcap = total; break; }
        newcap *= 2;
    }

    nd = (uint8_t*)bbq_mem_resize(b->a, b->data, b->cap, newcap);
    if (!nd) { b->oom = true; return false; }
    b->data = nd;
    b->cap  = newcap;
    return true;
}

bool bbq_buf_append(bbq_buf* b, const void* src, size_t n) {
    if (n == 0) return !b->oom;
    if (b->oom) return false;
    if (n > SIZE_MAX - b->len) { b->oom = true; return false; }   /* len + n wraps */
    if (!bbq_buf_reserve(b, b->len + n)) return false;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}

bool bbq_buf_append_byte(bbq_buf* b, uint8_t byte) {
    return bbq_buf_append(b, &byte, 1);
}
