#include "bbq_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── The four entry points ─────────────────────────────────────────────────── */

void* bbq_mem_alloc(bbq_alloc* a, size_t n) {
    if (n == 0) return NULL;
    return a ? a->alloc(a->ud, n) : malloc(n);
}

void* bbq_mem_zalloc(bbq_alloc* a, size_t n) {
    void* p;
    if (n == 0) return NULL;
    if (!a) return calloc(1, n);
    p = a->alloc(a->ud, n);
    if (p) memset(p, 0, n);
    return p;
}

void* bbq_mem_resize(bbq_alloc* a, void* p, size_t old_n, size_t new_n) {
    if (new_n == 0) { bbq_mem_release(a, p, old_n); return NULL; }
    if (!a) return realloc(p, new_n);
    /* A custom allocator is not obliged to treat resize(NULL, …) as alloc, so the
     * split happens here rather than in every implementation. */
    return p ? a->resize(a->ud, p, old_n, new_n) : a->alloc(a->ud, new_n);
}

void bbq_mem_release(bbq_alloc* a, void* p, size_t n) {
    if (!p) return;
    if (a) a->release(a->ud, p, n);
    else   free(p);
}

size_t bbq_mem_array_bytes(size_t n, size_t size) {
    if (n == 0 || size == 0) return 0;
    if (n > SIZE_MAX / size) return 0;          /* refuse, never wrap */
    return n * size;
}

/* ── Budget ───────────────────────────────────────────────────────────────── */

static void budget_note(bbq_budget* b, size_t added) {
    b->used += added;
    if (b->used > b->peak) b->peak = b->used;
}

static void* budget_alloc(void* ud, size_t n) {
    bbq_budget* b = (bbq_budget*)ud;
    void* p;
    /* The comparison is written so it cannot itself overflow. */
    if (n > b->limit - b->used || b->used > b->limit) { b->denials++; return NULL; }
    p = bbq_mem_alloc(b->under, n);
    if (!p) { b->denials++; return NULL; }
    budget_note(b, n);
    return p;
}

static void* budget_resize(void* ud, void* p, size_t old_n, size_t new_n) {
    bbq_budget* b = (bbq_budget*)ud;
    void* q;
    if (new_n > old_n) {
        size_t grow = new_n - old_n;
        if (grow > b->limit - b->used || b->used > b->limit) { b->denials++; return NULL; }
    }
    q = bbq_mem_resize(b->under, p, old_n, new_n);
    if (!q) { b->denials++; return NULL; }
    /* Charged only on success: a refused resize leaves the old block live, and
     * the old block is still accounted for. */
    b->used -= old_n;
    budget_note(b, new_n);
    return q;
}

static void budget_release(void* ud, void* p, size_t n) {
    bbq_budget* b = (bbq_budget*)ud;
    bbq_mem_release(b->under, p, n);
    b->used = (n > b->used) ? 0 : b->used - n;
}

void bbq_budget_init(bbq_budget* b, size_t limit, bbq_alloc* under) {
    b->limit = limit;
    b->used = b->peak = b->denials = 0;
    b->under = under;
    b->iface.alloc   = budget_alloc;
    b->iface.resize  = budget_resize;
    b->iface.release = budget_release;
    b->iface.ud      = b;
}

bbq_alloc* bbq_budget_handle(bbq_budget* b) { return &b->iface; }

/* ── Fault injection ──────────────────────────────────────────────────────── */

static int faulty_bites(bbq_faulty* f) {
    size_t n;
    if (f->fail_at == 0) return 0;
    n = f->served + 1;                       /* the request about to be served */
    return f->once ? (n == f->fail_at) : (n >= f->fail_at);
}

static void* faulty_alloc(void* ud, size_t n) {
    bbq_faulty* f = (bbq_faulty*)ud;
    void* p;
    if (faulty_bites(f)) { f->served++; return NULL; }
    p = bbq_mem_alloc(f->under, n);
    if (p) f->served++;
    return p;
}

static void* faulty_resize(void* ud, void* p, size_t old_n, size_t new_n) {
    bbq_faulty* f = (bbq_faulty*)ud;
    void* q;
    if (faulty_bites(f)) { f->served++; return NULL; }
    q = bbq_mem_resize(f->under, p, old_n, new_n);
    if (q) f->served++;
    return q;
}

static void faulty_release(void* ud, void* p, size_t n) {
    bbq_faulty* f = (bbq_faulty*)ud;
    bbq_mem_release(f->under, p, n);
}

void bbq_faulty_init(bbq_faulty* f, size_t fail_at, int once, bbq_alloc* under) {
    f->fail_at = fail_at;
    f->served  = 0;
    f->once    = once;
    f->under   = under;
    f->iface.alloc   = faulty_alloc;
    f->iface.resize  = faulty_resize;
    f->iface.release = faulty_release;
    f->iface.ud      = f;
}

bbq_alloc* bbq_faulty_handle(bbq_faulty* f) { return &f->iface; }

/* ── Hash seed ────────────────────────────────────────────────────────────── */

static uint64_t g_seed;
static int      g_seed_ready;

/* Drawn from the OS once. getrandom/arc4random are the right sources; the
 * address-and-clock fallback is there for a platform with neither, and it is
 * weak — it makes collisions harder to aim, not impossible. Anything relying on
 * the strong property should check that it got a real source. */
static uint64_t seed_from_os(void) {
    uint64_t s = 0;
#if defined(__linux__)
    {
        FILE* f = fopen("/dev/urandom", "rb");
        if (f) {
            size_t got = fread(&s, 1, sizeof s, f);
            fclose(f);
            if (got == sizeof s && s) return s;
        }
    }
#endif
    {
        /* ASLR gives the address some entropy; the clock gives the rest. */
        uintptr_t here = (uintptr_t)(void*)&g_seed;
        s = (uint64_t)here * 0x9E3779B97F4A7C15ULL;
        s ^= (uint64_t)(uintptr_t)&s << 17;
        s ^= (uint64_t)clock() * 0xBF58476D1CE4E5B9ULL;
        s ^= (uint64_t)time(NULL);
    }
    return s ? s : 0x9E3779B97F4A7C15ULL;
}

uint64_t bbq_hash_seed(void) {
    if (!g_seed_ready) { g_seed = seed_from_os(); g_seed_ready = 1; }
    return g_seed;
}

void bbq_hash_seed_set(uint64_t seed) {
    g_seed = seed;
    g_seed_ready = 1;
}
