/*
 * bbq_arena.h — Page-based arena allocator for BBQ generated code.
 *
 * Usage:
 *   bbq_arena a;
 *   bbq_arena_init(&a, 4096);
 *   void* p = bbq_arena_alloc(&a, sizeof(MyStruct));
 *   bbq_arena_reset(&a);   // rewind, keep pages
 *   bbq_arena_free(&a);    // release all pages
 */
#ifndef BBQ_ARENA_H
#define BBQ_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bbq_arena {
    char** pages;
    int    page_count;
    int    page_cap;
    int    cur_page;
    size_t pos;
    size_t page_size;
} bbq_arena;

/* Initialize an arena with the given page size (0 = default 4096). */
void  bbq_arena_init(bbq_arena* a, size_t page_size);

/* Allocate size bytes (8-byte aligned). Returns NULL on failure. */
void* bbq_arena_alloc(bbq_arena* a, size_t size);

/* Reset the arena: rewind to start, keep allocated pages for reuse. */
void  bbq_arena_reset(bbq_arena* a);

/* Free all pages and reset the arena to empty. */
void  bbq_arena_free(bbq_arena* a);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_ARENA_H */
