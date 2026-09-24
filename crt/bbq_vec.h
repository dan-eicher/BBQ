/*
 * bbq_vec.h — Type-generic growable array for BBQ generated code.
 *
 * A bbq_vec is a typed pointer (T*) with a hidden header at a negative offset.
 * NULL is a valid empty vector.
 *
 *   int* nums = NULL;
 *   bbq_vec_push(nums, 42);
 *   for (int i = 0; i < bbq_vec_len(nums); i++) printf("%d\n", nums[i]);
 *   bbq_vec_free(nums);
 *
 * ── THE FAILURE CONTRACT ────────────────────────────────────────────────────
 *
 * This is linked into compilers and VMs that process input they did not write,
 * so it never aborts and it never writes out of bounds. When an allocation fails
 * the vector is POISONED:
 *
 *   - growth is refused from then on, permanently;
 *   - every later push is a no-op — it does not write, and it does not advance
 *     the length;
 *   - the elements already there, and `len`, stay valid and readable;
 *   - bbq_vec_oom(v) reports it.
 *
 * `len` is deliberately preserved rather than zeroed. A vector that silently
 * becomes empty turns a partial result into a confidently wrong one — a method
 * body that looks complete, a capture list that looks closed — whereas a short
 * vector plus a set OOM bit is a result a caller can recognise as incomplete.
 *
 * A caller that must know whether its data is whole asks bbq_vec_oom() at a
 * boundary that suits it. A caller that never asks gets a short vector, never a
 * corrupted heap.
 *
 * ── THE ONE THING THIS CONTRACT BREAKS ──────────────────────────────────────
 *
 * A loop whose termination depends on a push making progress no longer
 * terminates:
 *
 *     while (bbq_vec_len(v) < need) bbq_vec_push(v, 0);     // HANGS on OOM
 *
 * A hang inside a VM is the same denial of service as an abort, so this shape is
 * a bug and there is no way for the macro to diagnose it. Use bbq_vec_fill, or
 * bbq_vec_try_reserve and check. The same applies wherever an external counter is
 * advanced past a push and then used as an index.
 *
 * ── ALLOCATORS ──────────────────────────────────────────────────────────────
 *
 * A vector captures its allocator at its first successful allocation and keeps it
 * for its lifetime, so it can never be freed through a different one. Every form
 * that can make that first allocation has an _a twin that names the allocator —
 * bbq_vec_push_a, _try_push_a, _reserve_a, _try_reserve_a, _fill_a — and a library
 * should use those, passing its own context's allocator: nothing then depends on
 * state outside the context. The bare forms use BBQ_VEC_ALLOC(), which is NULL
 * (libc) unless the embedder defines it.
 *
 * BBQ_VEC_ALLOC() is expanded in the TRANSLATION UNIT THAT PUSHES, which is why
 * the macros hand it to the out-of-line half rather than letting that half read
 * its own copy. A file that defines it sees it honoured; one that does not gets
 * libc, and the two can share a vector's type but never a vector.
 *
 * sizeof(bbq_vec_hdr) is part of the ABI — an object compiled against a different
 * version of this header computes a different element offset and corrupts the
 * heap on first access. There is no version stamp to check that with: nothing
 * embeds a header by value across a compilation boundary, and a macro nobody
 * compares is not a guard. Rebuild against one header.
 */
#ifndef BBQ_VEC_H
#define BBQ_VEC_H

#include <stddef.h>
#include <limits.h>
#include "bbq_alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The allocator a NEW vector is born with. */
#ifndef BBQ_VEC_ALLOC
#  define BBQ_VEC_ALLOC() ((bbq_alloc*)0)
#endif

/* Capacity ceiling. Chosen so that doubling can never overflow `int`, which is
 * what the old code did at 2^30 — signed overflow, undefined rather than
 * wrapping, so a guard placed after it may legally be deleted. Past the ceiling
 * a vector poisons instead. 2^30-1 elements is 8 GB at 8 bytes each. */
#define BBQ_VEC_MAX_CAP 0x3FFFFFFF

typedef struct bbq_vec_hdr {
    int        len;
    int        cap;     /* >= 0 live; < 0 poisoned, real capacity is -1 - cap */
    bbq_alloc* a;       /* captured at birth; NULL = libc */
} bbq_vec_hdr;

/* --- Internal (do not call directly) --- */

#define bbq__vec_hdr(v) ((bbq_vec_hdr*)(void*)((char*)(v) - sizeof(bbq_vec_hdr)))

/* Grow to at least one more than the current length, or to `want` elements.
 * Both return the pointer to store back: the same block, a new one, or a
 * poisoned one. Neither ever returns NULL for a vector that had elements. */
void* bbq__vec_grow  (void* v, size_t elem_size, bbq_alloc* born_with);
void* bbq__vec_resize(void* v, size_t want, size_t elem_size, bbq_alloc* born_with);
void  bbq__vec_release(void* v, size_t elem_size);

/* Writes to the header that must not touch the shared poison sentinel. */
void bbq__vec_setlen  (void* v, int n);        /* clamped to real capacity */
void bbq__vec_truncate(void* v, int n);        /* shrink only, clamped at 0 */
int  bbq__vec_popi    (void* v);               /* index to pop; never negative */
int  bbq__vec_topi    (const void* v);         /* index of the last element    */

/* __typeof__ in both languages, never decltype. `decltype(e)` on an lvalue that
 * is not a plain identifier yields T&, and the results being cast here are
 * prvalues — `(T*&)some_void_ptr` is not a valid cast, so a vec reached through
 * `ctx->stack` rather than a local would not compile. __typeof__ yields the
 * unreferenced type, which is what both of these want. */
#define bbq__vec_cast(v) (__typeof__(v))
#define bbq__vec_elem(v) __typeof__((v)[0])

/* --- Public API --- */

/* Unchanged from the original, byte for byte: this is on every hot loop in every
 * consumer and its `int` result is compared against `int` counters in hundreds of
 * places. Widening it would silently turn the common `v[bbq_vec_len(v) - 1]` into
 * an index near 2^64 with no diagnostic. */
#define bbq_vec_len(v)  ((v) ? bbq__vec_hdr(v)->len : 0)

/* A poisoned vector reports capacity 0. That is what routes it back through the
 * grow path on every push — where the sticky refusal lives — at no cost to the
 * fast path. */
#define bbq_vec_cap(v)  (((v) && bbq__vec_hdr(v)->cap > 0) ? bbq__vec_hdr(v)->cap : 0)

/* Did this vector ever fail to grow? Its contents are still valid; they are just
 * not all of what was pushed. */
#define bbq_vec_oom(v)  ((v) ? (bbq__vec_hdr(v)->cap < 0) : 0)

#define bbq_vec_free(v) do {                                                  \
    if (v) { bbq__vec_release((void*)(v), sizeof(*(v))); (v) = NULL; }        \
} while (0)

#define bbq_vec_clear(v)         bbq__vec_truncate((void*)(v), 0)

/* Shrink to n. Shrink-only and clamped at zero, so a too-large n can no longer
 * push len past the allocation and `truncate(v, len(v) - 1)` on an empty vector
 * can no longer set len to -1. To extend after a reserve, use bbq_vec_setlen. */
#define bbq_vec_truncate(v, n)   bbq__vec_truncate((void*)(v), (int)(n))

/* Set the length explicitly, clamped to the real capacity. This is the checked
 * replacement for writing bbq__vec_hdr(v)->len by hand after a reserve. */
#define bbq_vec_setlen(v, n)     bbq__vec_setlen((void*)(v), (int)(n))

/* Append. On allocation failure this stores nothing and the length does not
 * advance — see the failure contract above. */
#define bbq_vec_push(v, val)     bbq_vec_push_a((v), (val), BBQ_VEC_ALLOC())

/* …naming the allocator a NEW vector is born with. Every form that can give a vector
 * its first block has an _a twin, so a caller holding its context's allocator never
 * needs BBQ_VEC_ALLOC() — nor the per-thread state an embedder would otherwise have
 * to keep for it. On a vector that already has a block, `alloc` is ignored: the
 * vector keeps the allocator it was born with. */
#define bbq_vec_push_a(v, val, alloc) do {                                    \
    if (bbq_vec_len(v) >= bbq_vec_cap(v))                                     \
        (v) = bbq__vec_cast(v) bbq__vec_grow((v), sizeof(*(v)), (alloc));     \
    if (!bbq_vec_oom(v))                                                      \
        (v)[bbq__vec_hdr(v)->len++] = (val);                                  \
} while (0)

/* Append, reporting whether it happened. Use this wherever the next statement
 * depends on the element being there — an index computed from a separate
 * counter, or a loop that stops when the length reaches a target.
 *
 * A statement expression because bbq_vec_push is a statement and cannot appear
 * in a comma expression. GNU only, which this header already requires for
 * __typeof__, and which the gtest build has been compiling as C++ all along. */
#define bbq_vec_try_push(v, val) bbq_vec_try_push_a((v), (val), BBQ_VEC_ALLOC())
#define bbq_vec_try_push_a(v, val, alloc) __extension__ ({                    \
    bbq_vec_push_a((v), (val), (alloc));                                      \
    !bbq_vec_oom(v);                                                          \
})

/* Reserve capacity for n elements. Silently does nothing on failure; every
 * caller that then writes v[i] directly must use bbq_vec_try_reserve instead. */
#define bbq_vec_reserve(v, n)    bbq_vec_reserve_a((v), (n), BBQ_VEC_ALLOC())

/* `n` is bound once. Callers routinely pass a count derived from the vector
 * itself — `bbq_vec_cap(v) * 2` is the shape every parse stack uses — and `v` is
 * reassigned in the middle of this, so re-reading `n` afterwards would measure
 * the new capacity against a target computed from it. */
#define bbq_vec_reserve_a(v, n, alloc) do {                                   \
    int _rn = (int)(n);                                                       \
    if (bbq_vec_cap(v) < _rn)                                                 \
        (v) = bbq__vec_cast(v) bbq__vec_resize((v), (size_t)_rn,              \
                                               sizeof(*(v)), (alloc));        \
} while (0)

/* Reserve, as an expression: true iff the capacity is now there. */
#define bbq_vec_try_reserve(v, n) bbq_vec_try_reserve_a((v), (n), BBQ_VEC_ALLOC())
#define bbq_vec_try_reserve_a(v, n, alloc) __extension__ ({                   \
    int _rn = (int)(n);                                                       \
    if (bbq_vec_cap(v) < _rn)                                                 \
        (v) = bbq__vec_cast(v) bbq__vec_resize((v), (size_t)_rn,              \
                                               sizeof(*(v)), (alloc));        \
    bbq_vec_cap(v) >= _rn;                                                    \
})

/* Extend to exactly n elements with `val`, or fail and change nothing. This is
 * the safe form of `while (len < n) push(v, val)`, which does not terminate once
 * the vector is poisoned. */
#define bbq_vec_fill(v, n, val)  bbq_vec_fill_a((v), (n), (val), BBQ_VEC_ALLOC())
#define bbq_vec_fill_a(v, n, val, alloc) __extension__ ({                     \
    int _fok = bbq_vec_try_reserve_a((v), (n), (alloc));                      \
    if (_fok) {                                                               \
        for (int _fi = bbq_vec_len(v); _fi < (int)(n); _fi++) (v)[_fi] = (val); \
        if ((int)(n) > bbq_vec_len(v)) bbq_vec_setlen((v), (int)(n));          \
    }                                                                         \
    _fok;                                                                     \
})

/* Precondition: bbq_vec_len(v) > 0. Neither can drive len negative, and neither
 * writes to a poisoned vector's header when it is empty. */
#define bbq_vec_pop(v)  ((v)[bbq__vec_popi((void*)(v))])
#define bbq_vec_last(v) ((v)[bbq__vec_topi((const void*)(v))])

/* Safe on a poisoned vector with no guard of its own: an empty one has len 0, so
 * the loop body never runs and nothing is written. */
#define bbq_vec_reverse(v) do {                                               \
    int _n = bbq_vec_len(v);                                                  \
    for (int _i = 0, _j = _n - 1; _i < _j; _i++, _j--) {                      \
        bbq__vec_elem(v) _tmp = (v)[_i];                                      \
        (v)[_i] = (v)[_j];                                                    \
        (v)[_j] = _tmp;                                                       \
    }                                                                         \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* BBQ_VEC_H */
