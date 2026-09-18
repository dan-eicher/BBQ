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

/* How much address space a buffer reserves up front. Stamped code holds
 * PC-relative displacements computed from `base`, so the buffer MUST NOT MOVE
 * once anything has been patched into it — an mmap-copy-munmap grow would leave
 * every one of them pointing at where the buffer used to be, silently. So the
 * whole range is reserved at init and grown into. Only address space is taken;
 * pages arrive when they are written. */
#ifndef JCB_RESERVE
#define JCB_RESERVE (64u * 1024u * 1024u)
#endif

typedef struct {
    uint8_t* base;
    size_t   cap;
    size_t   size;
    int      finalized;
    /* Sticky: an emit that did not fit, or a displacement that did not fit its
     * field. Either means the stamped code is not what was asked for, so
     * jcb_finalize refuses to hand it back rather than letting it be called. */
    int      failed;
} jit_codebuf_t;

static inline size_t jcb_page_align(size_t n) {
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    return (n + ps - 1) & ~(ps - 1);
}

/* Returns 0 on success, -1 on mmap failure. */
static inline int jcb_init(jit_codebuf_t* b, size_t initial_cap) {
    size_t want = initial_cap > JCB_RESERVE ? initial_cap : JCB_RESERVE;
    b->cap = jcb_page_align(want);
    b->size = 0;
    b->finalized = 0;
    b->failed = 0;
    b->base = (uint8_t*)mmap(NULL, b->cap, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b->base == MAP_FAILED) { b->base = NULL; return -1; }
    return 0;
}

/* Did everything stamped so far actually fit? */
static inline int jcb_ok(const jit_codebuf_t* b) { return !b->failed; }

/* Drop everything stamped so far and stamp again from the start, keeping the
 * mapping. For a driver that has to RE-emit a body — a second pass under
 * different assumptions — rather than compile a different one. */
static inline void jcb_reset(jit_codebuf_t* b) {
    b->size = 0;
    b->finalized = 0;
    b->failed = 0;
}

static inline void jcb_free(jit_codebuf_t* b) {
    if (b->base) munmap(b->base, b->cap);
    b->base = NULL; b->cap = b->size = 0;
}

/* Room for `extra` more bytes? Returns 0 if so, -1 if the reservation is used up.
 * Never moves the buffer: see JCB_RESERVE. */
static inline int jcb_reserve(jit_codebuf_t* b, size_t extra) {
    if (b->size + extra <= b->cap) return 0;
    b->failed = 1;
    return -1;
}

static inline void jcb_emit(jit_codebuf_t* b, const uint8_t* data, size_t len) {
    if (jcb_reserve(b, len) != 0) return;   /* marked failed; finalize will refuse */
    memcpy(b->base + b->size, data, len);
    b->size += len;
}

static inline void jcb_patch32(jit_codebuf_t* b, size_t off, int32_t v) { memcpy(b->base + off, &v, 4); }
static inline void jcb_patch64(jit_codebuf_t* b, size_t off, uint64_t v) { memcpy(b->base + off, &v, 8); }

/* Patch a 32-bit PC-relative field at `off` so it reaches `target`.
 *
 * The displacement is `target - (address_after_the_field)`, and it has to FIT in
 * a signed 32-bit field — x86-64's rel32 reaches +-2 GB. An in-buffer target
 * always fits; an address outside the buffer need not, and a JIT buffer sits
 * wherever mmap put it, which on Linux is typically ~140,000 GB from the
 * executable's text. Casting that to int32_t silently produces a branch into
 * nowhere, which is why copy-and-patch JITs that DO branch to native code
 * (CPython's, on AArch64 especially) route it through a trampoline instead.
 *
 * jitterator's stencils call natives through a pointer held in the constant pool,
 * so every rel32 here targets the buffer itself — this check says so out loud
 * rather than leaving it an unwritten assumption. Returns 0, or -1 (and marks the
 * buffer failed) when the target is out of reach. */
static inline int jcb_patch_rel32(jit_codebuf_t* b, size_t off, uint64_t target) {
    int64_t from = (int64_t)(intptr_t)(b->base + off) + 4;
    int64_t disp = (int64_t)target - from;
    if (disp < INT32_MIN || disp > INT32_MAX) { b->failed = 1; return -1; }
    int32_t v = (int32_t)disp;
    memcpy(b->base + off, &v, 4);
    return 0;
}

/* Make the buffer executable, flush the icache, return the base pointer. After
 * this no further emit/patch is allowed. Returns NULL on mprotect failure. */
static inline void* jcb_finalize(jit_codebuf_t* b) {
    /* Something did not fit. Handing the buffer back would hand back code that is
     * not what was stamped — a short body, or a branch to the wrong place. */
    if (b->failed) return NULL;
    if (b->finalized) return b->base;
    if (mprotect(b->base, b->cap, PROT_READ | PROT_EXEC) != 0) return NULL;
    b->finalized = 1;
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)b->base, (char*)(b->base + b->size));
#endif
    return b->base;
}

#endif /* JIT_CODEBUF_H */
