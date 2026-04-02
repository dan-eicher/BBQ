#include "bbq_arena.h"
#include <stdlib.h>
#include <string.h>

#define BBQ_ARENA_DEFAULT_PAGE 4096
#define BBQ_ARENA_ALIGN       8
#define BBQ_ARENA_INIT_PAGES  4

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

void bbq_arena_init(bbq_arena* a, size_t page_size) {
    a->page_size  = page_size > 0 ? align_up(page_size, BBQ_ARENA_ALIGN)
                                  : BBQ_ARENA_DEFAULT_PAGE;
    a->pages      = NULL;
    a->page_count = 0;
    a->page_cap   = 0;
    a->cur_page   = -1;
    a->pos        = 0;
}

void* bbq_arena_alloc(bbq_arena* a, size_t size) {
    if (size == 0) return NULL;
    size = align_up(size, BBQ_ARENA_ALIGN);

    /* Need a new page? */
    if (a->cur_page < 0 || a->pos + size > a->page_size) {
        a->cur_page++;

        /* Reuse an existing page if available */
        if (a->cur_page < a->page_count) {
            a->pos = 0;
        } else {
            /* Grow the page pointer array if needed */
            if (a->page_count >= a->page_cap) {
                int newcap = a->page_cap > 0 ? a->page_cap * 2 : BBQ_ARENA_INIT_PAGES;
                char** np = (char**)realloc(a->pages, (size_t)newcap * sizeof(char*));
                if (!np) return NULL;
                a->pages    = np;
                a->page_cap = newcap;
            }

            /* Allocate the page — use the larger of page_size and requested size */
            size_t alloc_size = size > a->page_size ? align_up(size, BBQ_ARENA_ALIGN)
                                                    : a->page_size;
            char* page = (char*)malloc(alloc_size);
            if (!page) return NULL;
            a->pages[a->page_count++] = page;
            a->pos = 0;
        }
    }

    void* ptr = a->pages[a->cur_page] + a->pos;
    a->pos += size;
    return ptr;
}

void bbq_arena_reset(bbq_arena* a) {
    a->cur_page = -1;
    a->pos      = 0;
}

void bbq_arena_free(bbq_arena* a) {
    for (int i = 0; i < a->page_count; i++)
        free(a->pages[i]);
    free(a->pages);
    a->pages      = NULL;
    a->page_count = 0;
    a->page_cap   = 0;
    a->cur_page   = -1;
    a->pos        = 0;
}
