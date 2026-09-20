/*
 * bbq_arena.h — Page-based bump allocator.
 *
 *   bbq_arena a;
 *   bbq_arena_init(&a, 4096, NULL);   // 0 = default page, NULL = libc
 *   void* p = bbq_arena_alloc(&a, sizeof(MyStruct));
 *   bbq_arena_reset(&a);              // rewind, keep pages for reuse
 *   bbq_arena_free(&a);               // release everything
 *
 * Allocation returns NULL when it cannot be served — including when the request
 * is too large to represent, which the previous version quietly turned into a
 * SMALL allocation: it aligned the size before bounds-checking it, so
 * align_up(SIZE_MAX, 8) wrapped to 0 and the caller got a valid pointer to
 * nothing and then wrote SIZE_MAX bytes through it.
 *
 * Blocks are NOT zeroed, and a reset hands the same addresses back. Both are
 * relied on: generated AST constructors zero the fields they care about, and the
 * reuse is what makes reset cheap.
 */
#ifndef BBQ_ARENA_H
#define BBQ_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include "bbq_alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bbq_arena {
    char**  pages;
    /* Capacity of each page. A page is page_size bytes unless a single
     * allocation needed more, so the reuse path after a reset can tell whether
     * a recycled page is actually big enough for the request in hand. */
    size_t* page_sizes;
    /* How much of each page is spent. Per page, not one cursor for the whole
     * arena: with a single cursor, an oversized allocation that moved on to a
     * new page stranded the remainder of every page behind it until the next
     * reset, so an alternating big/small workload grew without bound. */
    size_t* page_used;
    size_t  page_count;
    size_t  page_cap;
    size_t  cur_page;      /* where to look first, for locality */
    size_t  page_size;
    bbq_alloc* a;
    bool    oom;
} bbq_arena;

/* `page_size` 0 selects the default (4096).
 *
 * Two entry points, as everywhere in the CRT. The short form allocates from
 * libc, which is right for a compiler pass, a build tool or a test. The `_a`
 * form takes an allocator, and ANYTHING PROCESSING UNTRUSTED INPUT wants it —
 * that is where a budget goes, and a budget is the only thing that actually
 * bounds what a hostile input can make this allocate. See bbq_alloc.h. */
void  bbq_arena_init(bbq_arena* a, size_t page_size);
void  bbq_arena_init_a(bbq_arena* a, size_t page_size, bbq_alloc* alloc);

/* `size` bytes, aligned. NULL if the request cannot be served — which also
 * poisons the arena, because a caller that ignored one NULL will ignore the
 * next, and bbq_arena_oom() is how it finds out at a boundary instead. */
void* bbq_arena_alloc(bbq_arena* a, size_t size);

/* Rewind to the start, keeping every page for reuse. Pointers handed out before
 * the reset are dangling afterwards; the storage is reissued. */
void  bbq_arena_reset(bbq_arena* a);

/* Release every page. The arena stays valid and reusable, and its OOM state is
 * cleared. */
void  bbq_arena_free(bbq_arena* a);

/* Did any allocation in this arena fail? */
bool  bbq_arena_oom(const bbq_arena* a);

/* Bytes actually handed out since the last reset, and bytes held in pages. For a
 * caller that wants to know what a workload cost it. */
size_t bbq_arena_used(const bbq_arena* a);
size_t bbq_arena_capacity(const bbq_arena* a);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_ARENA_H */
