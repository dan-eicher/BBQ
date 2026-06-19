/*
 * bbq_vec.h — Type-generic growable array for BBQ generated code.
 *
 * A bbq_vec is just a typed pointer (T*) with a hidden header at a negative
 * offset storing length and capacity.  NULL is a valid empty vector.
 *
 * Usage:
 *   int* nums = NULL;
 *   bbq_vec_push(nums, 42);
 *   bbq_vec_push(nums, 99);
 *   for (int i = 0; i < bbq_vec_len(nums); i++)
 *       printf("%d\n", nums[i]);
 *   bbq_vec_free(nums);
 */
#ifndef BBQ_VEC_H
#define BBQ_VEC_H

#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int len;
    int cap;
} bbq_vec_hdr;

/* --- Internal helpers (do not call directly) --- */

#define bbq__vec_hdr(v)  ((bbq_vec_hdr*)((char*)(v) - sizeof(bbq_vec_hdr)))

static inline void* bbq__vec_grow(void* v, size_t elem_size) {
    int len    = v ? bbq__vec_hdr(v)->len : 0;
    int newcap = v ? bbq__vec_hdr(v)->cap * 2 : 8;
    bbq_vec_hdr* h = (bbq_vec_hdr*)realloc(
        v ? bbq__vec_hdr(v) : NULL,
        sizeof(bbq_vec_hdr) + (size_t)newcap * elem_size);
    if (!h) return v; /* allocation failure — leave unchanged */
    h->len = len;
    h->cap = newcap;
    return (char*)h + sizeof(bbq_vec_hdr);
}

/* --- Public API --- */

#define bbq_vec_len(v)   ((v) ? bbq__vec_hdr(v)->len : 0)
#define bbq_vec_cap(v)   ((v) ? bbq__vec_hdr(v)->cap : 0)

#define bbq_vec_free(v)  \
    do { if (v) { free(bbq__vec_hdr(v)); (v) = NULL; } } while(0)

#define bbq_vec_clear(v) \
    do { if (v) bbq__vec_hdr(v)->len = 0; } while(0)

/* Shrink the length to n (n <= current len); keeps capacity. */
#define bbq_vec_truncate(v, n) \
    do { if (v) bbq__vec_hdr(v)->len = (int)(n); } while(0)

/* Cast helper: __typeof__ in C/GCC, decltype in C++ */
#ifdef __cplusplus
#define bbq__vec_cast(v) (decltype(v))
#else
#define bbq__vec_cast(v) (__typeof__(v))
#endif

#define bbq_vec_push(v, val) do {                                     \
    if (bbq_vec_len(v) >= bbq_vec_cap(v))                             \
        (v) = bbq__vec_cast(v) bbq__vec_grow((v), sizeof(*(v)));      \
    (v)[bbq__vec_hdr(v)->len++] = (val);                              \
} while(0)

#define bbq_vec_pop(v) ((v)[--bbq__vec_hdr(v)->len])

#define bbq_vec_last(v) ((v)[bbq__vec_hdr(v)->len - 1])

/* Reserve space for at least n total elements. */
#define bbq_vec_reserve(v, n) do {                                    \
    if (bbq_vec_cap(v) < (int)(n)) {                                  \
        int _newcap = (int)(n);                                       \
        bbq_vec_hdr* _h = (bbq_vec_hdr*)realloc(                     \
            (v) ? bbq__vec_hdr(v) : NULL,                             \
            sizeof(bbq_vec_hdr) + (size_t)_newcap * sizeof(*(v)));    \
        if (_h) {                                                     \
            if (!(v)) _h->len = 0;                                   \
            _h->cap = _newcap;                                        \
            (v) = bbq__vec_cast(v)((char*)_h + sizeof(bbq_vec_hdr)); \
        }                                                             \
    }                                                                 \
} while(0)

/* Reverse a vector in place. */
#define bbq_vec_reverse(v) do {                                       \
    int _n = bbq_vec_len(v);                                          \
    for (int _i = 0, _j = _n - 1; _i < _j; _i++, _j--) {            \
        __typeof__((v)[0]) _tmp = (v)[_i];                            \
        (v)[_i] = (v)[_j];                                            \
        (v)[_j] = _tmp;                                               \
    }                                                                 \
} while(0)

#ifdef __cplusplus
}
#endif

#endif /* BBQ_VEC_H */
