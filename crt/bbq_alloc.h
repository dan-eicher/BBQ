/*
 * bbq_alloc.h — where every CRT allocation comes from.
 *
 * WHY THIS EXISTS.
 *
 * These containers are linked into compilers and VMs that process input they did
 * not write. Two things follow, and they are the whole reason for this file.
 *
 * First, the library must never call abort(). A library that aborts hands whoever
 * supplies the input a way to kill the process, which is the denial of service
 * the containers are supposed to survive. So allocation failure is a VALUE here,
 * never a fatal event — see each container's OOM contract.
 *
 * Second — and this is the part that is easy to get wrong — handling `malloc`
 * returning NULL is not, by itself, a defence. On Linux with overcommit malloc
 * essentially never returns NULL; the kernel hands out the mapping and the OOM
 * killer arrives later, which is the same process death by another route. What
 * actually bounds a hostile workload is a CEILING: the embedder says "this
 * evaluation gets 64 MB", allocation past that fails deterministically, and every
 * container is required to survive it. That is what bbq_budget is for.
 *
 * The same ceiling is what makes the failure paths testable. Fault injection
 * becomes "fail the Nth allocation", exhaustively, for every N — which is how
 * sqlite covers its OOM paths — instead of one global boolean that can only say
 * "fail everything from here on".
 *
 * A NULL bbq_alloc* means libc malloc/realloc/free. Every container accepts one
 * and keeps it for its lifetime, so a container can never be freed through an
 * allocator other than the one that allocated it.
 */
#ifndef BBQ_ALLOC_H
#define BBQ_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The old byte count is passed to realloc and release so an accounting allocator
 * can balance its books exactly. Every CRT caller knows it: a container's
 * capacity and element size are always in hand at the call. Requiring it here is
 * what lets bbq_budget track usage without a per-block header of its own. */
typedef struct bbq_alloc {
    void* (*alloc)  (void* ud, size_t n);
    void* (*resize) (void* ud, void* p, size_t old_n, size_t new_n);
    void  (*release)(void* ud, void* p, size_t n);
    void*  ud;
} bbq_alloc;

/* The four entry points every container allocates through. `a == NULL` selects
 * libc, so a caller that does not care passes nothing and pays one predictable
 * branch. A zero-sized request returns NULL without consulting the allocator;
 * releasing NULL is a no-op. */
void* bbq_mem_alloc  (bbq_alloc* a, size_t n);
void* bbq_mem_zalloc (bbq_alloc* a, size_t n);                        /* zeroed */
void* bbq_mem_resize (bbq_alloc* a, void* p, size_t old_n, size_t new_n);
void  bbq_mem_release(bbq_alloc* a, void* p, size_t n);

/* n * size, refusing the multiply rather than wrapping it. Returns 0 on overflow,
 * which every caller treats as an allocation failure. */
size_t bbq_mem_array_bytes(size_t n, size_t size);

/* ── A ceiling ───────────────────────────────────────────────────────────────
 *
 * Wraps another allocator (NULL = libc) and refuses anything that would take the
 * live total past `limit`. Refusal is a NULL return — the same outcome the
 * containers already have to handle — so nothing downstream needs a second code
 * path for it.
 *
 * Counters are plain, not atomic: a budget belongs to one evaluation on one
 * thread. Sharing one across threads needs BBQ_BUDGET_ATOMIC.
 */
typedef struct bbq_budget {
    size_t     limit;
    size_t     used;
    size_t     peak;
    size_t     denials;      /* how often the ceiling actually bit */
    bbq_alloc* under;        /* NULL = libc */
    bbq_alloc  iface;        /* the handle handed to containers */
} bbq_budget;

void       bbq_budget_init(bbq_budget* b, size_t limit, bbq_alloc* under);
bbq_alloc* bbq_budget_handle(bbq_budget* b);

/* ── Deterministic fault injection ───────────────────────────────────────────
 *
 * Fails the Nth allocation and every allocation after it, or exactly the Nth when
 * `once` is set. This lives beside the budget rather than in the tests because it
 * is the same mechanism — an allocator that says no on schedule — and because a
 * container's failure paths are not covered by anything else.
 *
 * `served` counts allocations that succeeded, so a test can sweep N from 1 to
 * whatever an operation actually needed rather than guessing.
 */
typedef struct bbq_faulty {
    size_t     fail_at;      /* 1-based; 0 never fails */
    size_t     served;
    int        once;
    bbq_alloc* under;
    bbq_alloc  iface;
} bbq_faulty;

void       bbq_faulty_init(bbq_faulty* f, size_t fail_at, int once, bbq_alloc* under);
bbq_alloc* bbq_faulty_handle(bbq_faulty* f);

/* ── Hash seed ───────────────────────────────────────────────────────────────
 *
 * A FRESH seed from the OS on every call. There is no process-wide seed: every CRT
 * hash container draws its own at init and keeps it, so no state is shared between
 * containers or threads — a library a host calls from several threads has nothing
 * here to race on, and one container's seed says nothing about another's.
 *
 * Seeded at all because an unseeded hash over attacker-supplied keys is a denial of
 * service: colliding keys are cheap to construct, they all land on one chain, and
 * lookup degrades to a linear scan (Klink & Wälde, 28C3 2011). Seeding is the
 * standard answer and the reason Python, Ruby, Rust and Perl all adopted it.
 *
 * A test that needs a reproducible bucket layout seeds the container itself
 * (bbq_hmap_init_seeded, bbq_dict_init_seeded) — and only a test: a fixed seed is
 * the vulnerability this exists to close. */
uint64_t bbq_hash_seed(void);

#ifdef __cplusplus
}
#endif

#endif /* BBQ_ALLOC_H */
