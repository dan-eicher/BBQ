/*
 * jit_codebuf.h — pure-C executable code buffer for copy-and-patch JIT.
 *
 * The C counterpart of jitterator.h's C++ CodeBuffer. A copy-and-patch JIT's
 * consumers are runtime code linked into the host (an interpreter/VM), which is
 * normally C — so the buffer that mmaps, emits, patches, and mprotects the
 * stamped stencils ships as C, not C++. Header-only (static inline), the
 * bbq_runtime.h idiom: include it, no extra TU to build.
 *
 * POSIX (mmap/mprotect); add a _WIN32 arm here if ever needed — the JIT buffer
 * is inherently OS-specific, which is orthogonal to the host staying portable C.
 */
#ifndef JIT_CODEBUF_H
#define JIT_CODEBUF_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    uint8_t* base;
    size_t   cap;
    size_t   size;
    int      finalized;
} jit_codebuf_t;

static inline size_t jcb_page_align(size_t n) {
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    return (n + ps - 1) & ~(ps - 1);
}

/* Returns 0 on success, -1 on mmap failure. */
static inline int jcb_init(jit_codebuf_t* b, size_t initial_cap) {
    b->cap = jcb_page_align(initial_cap ? initial_cap : 4096);
    b->size = 0;
    b->finalized = 0;
    b->base = (uint8_t*)mmap(NULL, b->cap, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b->base == MAP_FAILED) { b->base = NULL; return -1; }
    return 0;
}

static inline void jcb_free(jit_codebuf_t* b) {
    if (b->base) munmap(b->base, b->cap);
    b->base = NULL; b->cap = b->size = 0;
}

/* Grow to hold `extra` more bytes (mmap a bigger region, copy, unmap old).
 * Returns 0 on success, -1 on failure (the buffer is left intact). */
static inline int jcb_reserve(jit_codebuf_t* b, size_t extra) {
    if (b->size + extra <= b->cap) return 0;
    size_t want = b->cap * 2 > b->size + extra ? b->cap * 2 : b->size + extra + 4096;
    size_t new_cap = jcb_page_align(want);
    uint8_t* nb = (uint8_t*)mmap(NULL, new_cap, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nb == MAP_FAILED) return -1;
    memcpy(nb, b->base, b->size);
    munmap(b->base, b->cap);
    b->base = nb; b->cap = new_cap;
    return 0;
}

static inline void jcb_emit(jit_codebuf_t* b, const uint8_t* data, size_t len) {
    if (jcb_reserve(b, len) != 0) return;   /* OOM: caller sees a short buffer */
    memcpy(b->base + b->size, data, len);
    b->size += len;
}

static inline void jcb_patch32(jit_codebuf_t* b, size_t off, int32_t v) { memcpy(b->base + off, &v, 4); }
static inline void jcb_patch64(jit_codebuf_t* b, size_t off, uint64_t v) { memcpy(b->base + off, &v, 8); }

/* Make the buffer executable, flush the icache, return the base pointer. After
 * this no further emit/patch is allowed. Returns NULL on mprotect failure. */
static inline void* jcb_finalize(jit_codebuf_t* b) {
    if (b->finalized) return b->base;
    if (mprotect(b->base, b->cap, PROT_READ | PROT_EXEC) != 0) return NULL;
    b->finalized = 1;
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)b->base, (char*)(b->base + b->size));
#endif
    return b->base;
}

#endif /* JIT_CODEBUF_H */
