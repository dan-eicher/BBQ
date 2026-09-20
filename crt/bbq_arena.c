#include "bbq_arena.h"

#include <stdint.h>
#include <string.h>

#define BBQ_ARENA_DEFAULT_PAGE 4096
#define BBQ_ARENA_ALIGN        8
#define BBQ_ARENA_INIT_PAGES   4

/* Refuses rather than wrapping. align_up(SIZE_MAX, 8) is 0 in the obvious
 * formulation, and a zero-byte "allocation" that still returns a live pointer is
 * how a huge request became a heap overflow. */
static bool align_up(size_t v, size_t a, size_t* out) {
    if (v > SIZE_MAX - (a - 1)) return false;
    *out = (v + a - 1) & ~(a - 1);
    return true;
}

void bbq_arena_init(bbq_arena* a, size_t page_size) {
    bbq_arena_init_a(a, page_size, NULL);
}

void bbq_arena_init_a(bbq_arena* a, size_t page_size, bbq_alloc* alloc) {
    size_t ps;
    a->pages      = NULL;
    a->page_sizes = NULL;
    a->page_used  = NULL;
    a->page_count = 0;
    a->page_cap   = 0;
    a->cur_page   = 0;
    a->a          = alloc;
    a->oom        = false;
    if (page_size == 0 || !align_up(page_size, BBQ_ARENA_ALIGN, &ps))
        ps = BBQ_ARENA_DEFAULT_PAGE;
    a->page_size = ps;
}

bool bbq_arena_oom(const bbq_arena* a) { return a->oom; }

size_t bbq_arena_capacity(const bbq_arena* a) {
    size_t total = 0, i;
    for (i = 0; i < a->page_count; i++) total += a->page_sizes[i];
    return total;
}

size_t bbq_arena_used(const bbq_arena* a) {
    size_t used = 0, i;
    for (i = 0; i < a->page_count; i++) used += a->page_used[i];
    return used;
}

/* Room for one more page record, in all three parallel arrays. */
static bool grow_page_table(bbq_arena* a) {
    size_t newcap, old = a->page_cap;
    char**  np;
    size_t* ns;
    size_t* nu;

    if (a->page_count < a->page_cap) return true;

    newcap = old ? old * 2 : BBQ_ARENA_INIT_PAGES;
    if (newcap < old) return false;                                /* wrapped */
    if (!bbq_mem_array_bytes(newcap, sizeof(char*)) ||
        !bbq_mem_array_bytes(newcap, sizeof(size_t))) return false;

    np = (char**)bbq_mem_resize(a->a, a->pages,
                                bbq_mem_array_bytes(old, sizeof(char*)),
                                bbq_mem_array_bytes(newcap, sizeof(char*)));
    if (!np) return false;
    a->pages = np;

    ns = (size_t*)bbq_mem_resize(a->a, a->page_sizes,
                                 bbq_mem_array_bytes(old, sizeof(size_t)),
                                 bbq_mem_array_bytes(newcap, sizeof(size_t)));
    if (!ns) return false;
    a->page_sizes = ns;

    nu = (size_t*)bbq_mem_resize(a->a, a->page_used,
                                 bbq_mem_array_bytes(old, sizeof(size_t)),
                                 bbq_mem_array_bytes(newcap, sizeof(size_t)));
    if (!nu) return false;
    a->page_used = nu;

    /* Only now, when all three took: page_cap is what says how big they are, and
     * a stale one that overstates any of them is a read past the end. */
    a->page_cap = newcap;
    return true;
}

void* bbq_arena_alloc(bbq_arena* a, size_t size) {
    size_t need, i;
    void*  ptr;

    if (size == 0) return NULL;
    if (a->oom) return NULL;
    /* Bounds-check BEFORE aligning, so an unrepresentable request is refused
     * rather than wrapped into a small one. */
    if (!align_up(size, BBQ_ARENA_ALIGN, &need)) { a->oom = true; return NULL; }

    /* The page used last, then any other page with room. Every page is a
     * candidate, so nothing is stranded by an oversized allocation that had to
     * start a page of its own. */
    if (a->cur_page < a->page_count &&
        need <= a->page_sizes[a->cur_page] - a->page_used[a->cur_page]) {
        /* keep cur_page */
    } else {
        size_t found = a->page_count;
        for (i = 0; i < a->page_count; i++)
            if (need <= a->page_sizes[i] - a->page_used[i]) { found = i; break; }

        if (found == a->page_count) {
            size_t alloc_size = need > a->page_size ? need : a->page_size;
            char* page;
            if (!grow_page_table(a)) { a->oom = true; return NULL; }
            page = (char*)bbq_mem_alloc(a->a, alloc_size);
            if (!page) { a->oom = true; return NULL; }
            a->page_sizes[a->page_count] = alloc_size;
            a->page_used [a->page_count] = 0;
            a->pages     [a->page_count] = page;
            found = a->page_count++;
        }
        a->cur_page = found;
    }

    ptr = a->pages[a->cur_page] + a->page_used[a->cur_page];
    a->page_used[a->cur_page] += need;
    return ptr;
}

void bbq_arena_reset(bbq_arena* a) {
    size_t i;
    for (i = 0; i < a->page_count; i++) a->page_used[i] = 0;
    a->cur_page = 0;
    a->oom      = false;
}

void bbq_arena_free(bbq_arena* a) {
    size_t i;
    for (i = 0; i < a->page_count; i++)
        bbq_mem_release(a->a, a->pages[i], a->page_sizes[i]);
    bbq_mem_release(a->a, a->pages,
                    bbq_mem_array_bytes(a->page_cap, sizeof(char*)));
    bbq_mem_release(a->a, a->page_sizes,
                    bbq_mem_array_bytes(a->page_cap, sizeof(size_t)));
    bbq_mem_release(a->a, a->page_used,
                    bbq_mem_array_bytes(a->page_cap, sizeof(size_t)));
    a->pages      = NULL;
    a->page_sizes = NULL;
    a->page_used  = NULL;
    a->page_count = 0;
    a->page_cap   = 0;
    a->cur_page   = 0;
    a->oom        = false;
}
