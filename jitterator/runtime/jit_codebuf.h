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

/* TWO PHASES, and the boundary is the whole safety argument.
 *
 * Stamped code holds PC-relative displacements computed from `base`, so a grow
 * that moves the buffer invalidates every one already written — silently. The
 * rule that makes growth safe is therefore: nothing whose value depends on
 * `base` may be written while the layout can still change. Copy every stencil
 * first, seal, then patch the displacements against the final address.
 *
 * jcb_seal ends the emit phase and the buffer enforces the split: emitting
 * after it, or patching a displacement before it, marks the buffer failed and
 * jcb_finalize refuses the code. So the buffer is free to move while growing,
 * and starts at whatever the caller asked for.
 *
 * This replaces reserving 64 MB of address space per buffer up front and never
 * moving. That bought the same property by making growth impossible, at a cost
 * that only looks free on a 64-bit host: one buffer per compiled function meant
 * 84 GB of executable address space for ~6 MB of code in a real run, it fails
 * outright on a 32-bit target after ~48 functions, and it is inexpressible where
 * there is no MMU. The invariant was never the buffer's to guarantee — it is a
 * property of the driver's write order, which is now checked rather than bought. */

typedef struct {
    uint8_t* base;
    size_t   cap;
    size_t   size;
    int      finalized;
    /* Set by jcb_seal: emission is over, the layout is fixed, displacements may
     * now be written against `base`. */
    int      sealed;
    /* Sticky: an emit that did not fit, a displacement that did not fit its
     * field, or a write on the wrong side of the seal. Any of them means the
     * stamped code is not what was asked for, so jcb_finalize refuses to hand it
     * back rather than letting it be called. */
    int      failed;
} jit_codebuf_t;

static inline size_t jcb_page_align(size_t n) {
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    return (n + ps - 1) & ~(ps - 1);
}

/* Returns 0 on success, -1 on mmap failure. `initial_cap` is honoured — the
 * buffer grows from it as needed, so a caller may ask for one page. */
static inline int jcb_init(jit_codebuf_t* b, size_t initial_cap) {
    b->cap = jcb_page_align(initial_cap ? initial_cap : 4096);
    b->size = 0;
    b->finalized = 0;
    b->sealed = 0;
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
 * different assumptions — rather than compile a different one. Back to the emit
 * phase, because that is what re-emitting means. */
static inline void jcb_reset(jit_codebuf_t* b) {
    b->size = 0;
    b->finalized = 0;
    b->sealed = 0;
    b->failed = 0;
}

/* End the emit phase: the layout is final, so displacements computed from `base`
 * may now be written and the buffer will not move again. */
static inline void jcb_seal(jit_codebuf_t* b) { b->sealed = 1; }
static inline int  jcb_sealed(const jit_codebuf_t* b) { return b->sealed; }

static inline void jcb_free(jit_codebuf_t* b) {
    if (b->base) munmap(b->base, b->cap);
    b->base = NULL; b->cap = b->size = 0;
}

/* Make room for `extra` more bytes, growing (and MOVING) the mapping if needed.
 * Safe only in the emit phase, which is why a sealed buffer refuses instead: the
 * displacements written after the seal are computed from `base`. Returns 0 on
 * success, -1 with the buffer marked failed. */
static inline int jcb_reserve(jit_codebuf_t* b, size_t extra) {
    if (b->size + extra <= b->cap) return 0;
    if (b->sealed || b->finalized) { b->failed = 1; return -1; }
    size_t want = b->cap * 2 > b->size + extra ? b->cap * 2 : b->size + extra + 4096;
    size_t new_cap = jcb_page_align(want);
    uint8_t* nb = (uint8_t*)mmap(NULL, new_cap, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nb == MAP_FAILED) { b->failed = 1; return -1; }
    memcpy(nb, b->base, b->size);
    munmap(b->base, b->cap);
    b->base = nb; b->cap = new_cap;
    return 0;
}

static inline void jcb_emit(jit_codebuf_t* b, const uint8_t* data, size_t len) {
    /* After the seal the layout is what the displacements were computed against. */
    if (b->sealed) { b->failed = 1; return; }
    if (jcb_reserve(b, len) != 0) return;   /* marked failed; finalize will refuse */
    memcpy(b->base + b->size, data, len);
    b->size += len;
}

/* Absolute writes: the VALUE does not depend on where the buffer sits (a native
 * address, an immediate), so these are legal in either phase — a move copies them
 * along unchanged. Only PC-relative fields have to wait for the seal. */
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
 * buffer failed) when the target is out of reach, or when it is called before the
 * seal: a displacement written while the buffer can still move is a displacement
 * to wherever the buffer used to be. */
static inline int jcb_patch_rel32(jit_codebuf_t* b, size_t off, uint64_t target) {
    if (!b->sealed) { b->failed = 1; return -1; }
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
    b->sealed = 1;   /* nothing more may be emitted into executable code */
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char*)b->base, (char*)(b->base + b->size));
#endif
    return b->base;
}

#endif /* JIT_CODEBUF_H */
