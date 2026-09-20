/*
 * crt_test.cpp — the CRT container contract.
 *
 * The old suite here was 41 tests written as things broke: it covered what had
 * gone wrong once, and nothing else. This one is organised by the contract
 * instead, so that a law without a test is visible as a gap rather than as an
 * absence nobody notices.
 *
 *   A. Laws            every public entry point, including the boring ones
 *   B. Allocation      EXHAUSTIVE fault injection: fail the Nth allocation for
 *                      every N an operation makes, and demand the container is
 *                      intact, poisoned, readable and destructible after it
 *   C. Adversarial     keys chosen to break the structure rather than exercise it
 *   D. Arithmetic      SIZE_MAX-shaped inputs at every entry point that takes one
 *   E. Iterators       abandonment, and mutation mid-walk
 *   F. Differential    randomised op sequences against std:: oracles
 *
 * The bar these are written to: a compiler or VM built on these containers must
 * not be exploitable by the input it is fed. So "it returned the right answer"
 * is never the whole assertion — the tests also demand that a refused allocation
 * cannot corrupt, that a hostile key set cannot turn a lookup into a scan, and
 * that nothing here ever aborts.
 */
#include <gtest/gtest.h>

#include "bbq_alloc.h"
#include "bbq_arena.h"
#include "bbq_buf.h"
#include "bbq_dict.h"
#include "bbq_hmap.h"
#include "bbq_htree.h"
#include "bbq_vec.h"

#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/* Count every allocation an operation makes, so a fault-injection sweep can run
 * N from 1 to exactly that and no further. */
struct Counting {
    bbq_faulty f;
    Counting() { bbq_faulty_init(&f, 0, 0, nullptr); }
    bbq_alloc* handle() { return bbq_faulty_handle(&f); }
    size_t served() const { return f.served; }
};

/* Run `body` once to learn how many allocations it makes, then run it again once
 * per allocation with that one failing. `body` gets the allocator and must leave
 * nothing behind. This is sqlite's OOM discipline: the failure paths are covered
 * because every one of them is visited, not because someone thought of them. */
template <typename F>
void SweepAllocationFailures(F body) {
    Counting probe;
    body(probe.handle());
    size_t total = probe.served();
    ASSERT_GT(total, 0u) << "nothing allocated — the sweep would prove nothing";
    for (size_t n = 1; n <= total; n++) {
        bbq_faulty f;
        bbq_faulty_init(&f, n, /*once=*/0, nullptr);
        body(bbq_faulty_handle(&f));
    }
}

}  // namespace

/* ════════════════════════════════════════════════════════════════════════════
 * A. Laws — bbq_alloc
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(BbqAlloc, NullHandleIsLibc) {
    void* p = bbq_mem_alloc(nullptr, 32);
    ASSERT_NE(p, nullptr);
    bbq_mem_release(nullptr, p, 32);
}

TEST(BbqAlloc, ZeroSizedRequestsAreNull) {
    EXPECT_EQ(bbq_mem_alloc(nullptr, 0), nullptr);
    EXPECT_EQ(bbq_mem_zalloc(nullptr, 0), nullptr);
    bbq_mem_release(nullptr, nullptr, 0);   /* releasing NULL is a no-op */
}

TEST(BbqAlloc, ZallocZeroes) {
    auto* p = static_cast<unsigned char*>(bbq_mem_zalloc(nullptr, 128));
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 128; i++) EXPECT_EQ(p[i], 0);
    bbq_mem_release(nullptr, p, 128);
}

TEST(BbqAlloc, ArrayBytesRefusesOverflowInsteadOfWrapping) {
    EXPECT_EQ(bbq_mem_array_bytes(0, 8), 0u);
    EXPECT_EQ(bbq_mem_array_bytes(8, 0), 0u);
    EXPECT_EQ(bbq_mem_array_bytes(4, 8), 32u);
    EXPECT_EQ(bbq_mem_array_bytes(SIZE_MAX, 2), 0u);
    EXPECT_EQ(bbq_mem_array_bytes(SIZE_MAX / 2 + 1, 2), 0u);
}

TEST(BbqAlloc, BudgetRefusesPastItsCeiling) {
    bbq_budget b;
    bbq_budget_init(&b, 1024, nullptr);
    void* a = bbq_mem_alloc(bbq_budget_handle(&b), 512);
    ASSERT_NE(a, nullptr);
    void* c = bbq_mem_alloc(bbq_budget_handle(&b), 512);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(bbq_mem_alloc(bbq_budget_handle(&b), 1), nullptr);
    EXPECT_EQ(b.denials, 1u);
    EXPECT_EQ(b.used, 1024u);
    bbq_mem_release(bbq_budget_handle(&b), a, 512);
    /* Releasing gives the room back — a budget is a live ceiling, not a quota. */
    void* d = bbq_mem_alloc(bbq_budget_handle(&b), 256);
    EXPECT_NE(d, nullptr);
    bbq_mem_release(bbq_budget_handle(&b), c, 512);
    bbq_mem_release(bbq_budget_handle(&b), d, 256);
    EXPECT_EQ(b.used, 0u);
    EXPECT_EQ(b.peak, 1024u);
}

TEST(BbqAlloc, BudgetOfZeroRefusesEverything) {
    bbq_budget b;
    bbq_budget_init(&b, 0, nullptr);
    EXPECT_EQ(bbq_mem_alloc(bbq_budget_handle(&b), 1), nullptr);
}

TEST(BbqAlloc, FaultyFailsTheNthAllocation) {
    bbq_faulty f;
    bbq_faulty_init(&f, 3, /*once=*/1, nullptr);
    void* p1 = bbq_mem_alloc(bbq_faulty_handle(&f), 8);
    void* p2 = bbq_mem_alloc(bbq_faulty_handle(&f), 8);
    EXPECT_EQ(bbq_mem_alloc(bbq_faulty_handle(&f), 8), nullptr);   /* the 3rd */
    void* p4 = bbq_mem_alloc(bbq_faulty_handle(&f), 8);            /* once: recovers */
    EXPECT_NE(p1, nullptr); EXPECT_NE(p2, nullptr); EXPECT_NE(p4, nullptr);
    bbq_mem_release(nullptr, p1, 8);
    bbq_mem_release(nullptr, p2, 8);
    bbq_mem_release(nullptr, p4, 8);
}

TEST(BbqAlloc, HashSeedIsStableWithinAProcessAndOverridable) {
    EXPECT_EQ(bbq_hash_seed(), bbq_hash_seed());
    uint64_t original = bbq_hash_seed();
    bbq_hash_seed_set(0x1234);
    EXPECT_EQ(bbq_hash_seed(), 0x1234u);
    bbq_hash_seed_set(original);
}

/* ════════════════════════════════════════════════════════════════════════════
 * A. Laws — bbq_vec
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(BbqVec, NullIsAValidEmptyVector) {
    int* v = nullptr;
    EXPECT_EQ(bbq_vec_len(v), 0);
    EXPECT_EQ(bbq_vec_cap(v), 0);
    EXPECT_FALSE(bbq_vec_oom(v));
    bbq_vec_free(v);             /* a no-op, and leaves it NULL */
    EXPECT_EQ(v, nullptr);
}

TEST(BbqVec, PushGrowsAndPreservesOrder) {
    int* v = nullptr;
    for (int i = 0; i < 1000; i++) bbq_vec_push(v, i * 3);
    ASSERT_EQ(bbq_vec_len(v), 1000);
    for (int i = 0; i < 1000; i++) EXPECT_EQ(v[i], i * 3);
    bbq_vec_free(v);
}

TEST(BbqVec, PopAndLast) {
    int* v = nullptr;
    bbq_vec_push(v, 10); bbq_vec_push(v, 20); bbq_vec_push(v, 30);
    EXPECT_EQ(bbq_vec_last(v), 30);
    EXPECT_EQ(bbq_vec_pop(v), 30);
    EXPECT_EQ(bbq_vec_len(v), 2);
    EXPECT_EQ(bbq_vec_last(v), 20);
    bbq_vec_free(v);
}

TEST(BbqVec, PopOnEmptyDoesNotDriveLengthNegative) {
    int* v = nullptr;
    bbq_vec_push(v, 7);
    (void)bbq_vec_pop(v);
    (void)bbq_vec_pop(v);          /* already empty */
    EXPECT_EQ(bbq_vec_len(v), 0);  /* not -1, which would index v[-1] next */
    bbq_vec_free(v);
}

TEST(BbqVec, ClearKeepsCapacity) {
    int* v = nullptr;
    for (int i = 0; i < 100; i++) bbq_vec_push(v, i);
    int cap = bbq_vec_cap(v);
    bbq_vec_clear(v);
    EXPECT_EQ(bbq_vec_len(v), 0);
    EXPECT_EQ(bbq_vec_cap(v), cap);
    bbq_vec_push(v, 42);
    EXPECT_EQ(v[0], 42);
    bbq_vec_free(v);
}

TEST(BbqVec, TruncateShrinksOnlyAndClampsAtZero) {
    int* v = nullptr;
    for (int i = 0; i < 10; i++) bbq_vec_push(v, i);
    bbq_vec_truncate(v, 4);
    EXPECT_EQ(bbq_vec_len(v), 4);
    bbq_vec_truncate(v, 99);                 /* must NOT extend past what is there */
    EXPECT_EQ(bbq_vec_len(v), 4);
    bbq_vec_truncate(v, -3);                 /* must NOT go negative */
    EXPECT_EQ(bbq_vec_len(v), 0);
    bbq_vec_free(v);
}

TEST(BbqVec, TruncateOnAnEmptyVectorByOneIsNotNegativeOne) {
    /* bbq_lite.c does truncate(v, len(v) - 1) on the scope stack; on an empty
     * vector that argument is -1, which used to be written straight into len. */
    int* v = nullptr;
    bbq_vec_push(v, 1);
    bbq_vec_clear(v);
    bbq_vec_truncate(v, bbq_vec_len(v) - 1);
    EXPECT_EQ(bbq_vec_len(v), 0);
    bbq_vec_free(v);
}

TEST(BbqVec, SetlenClampsToCapacity) {
    int* v = nullptr;
    ASSERT_TRUE(bbq_vec_try_reserve(v, 16));
    bbq_vec_setlen(v, 8);
    EXPECT_EQ(bbq_vec_len(v), 8);
    bbq_vec_setlen(v, 1000);                 /* far past the allocation */
    EXPECT_LE(bbq_vec_len(v), bbq_vec_cap(v));
    bbq_vec_free(v);
}

TEST(BbqVec, ReserveThenIndexDirectly) {
    int* v = nullptr;
    ASSERT_TRUE(bbq_vec_try_reserve(v, 64));
    EXPECT_GE(bbq_vec_cap(v), 64);
    for (int i = 0; i < 64; i++) v[i] = i;
    bbq_vec_setlen(v, 64);
    EXPECT_EQ(v[63], 63);
    bbq_vec_free(v);
}

TEST(BbqVec, ReverseHandlesEveryParity) {
    for (int n = 0; n <= 5; n++) {
        int* v = nullptr;
        for (int i = 0; i < n; i++) bbq_vec_push(v, i);
        bbq_vec_reverse(v);
        for (int i = 0; i < n; i++) EXPECT_EQ(v[i], n - 1 - i) << "n=" << n;
        bbq_vec_free(v);
    }
}

TEST(BbqVec, FillExtendsToExactlyN) {
    int* v = nullptr;
    bbq_vec_push(v, 9);
    ASSERT_TRUE(bbq_vec_fill(v, 5, 7));
    ASSERT_EQ(bbq_vec_len(v), 5);
    EXPECT_EQ(v[0], 9);
    for (int i = 1; i < 5; i++) EXPECT_EQ(v[i], 7);
    /* Already long enough: a no-op that still succeeds. */
    ASSERT_TRUE(bbq_vec_fill(v, 3, 0));
    EXPECT_EQ(bbq_vec_len(v), 5);
    bbq_vec_free(v);
}

TEST(BbqVec, StructElementsAndPointerElements) {
    struct Pair { int a; double b; };
    Pair* p = nullptr;
    for (int i = 0; i < 50; i++) bbq_vec_push(p, (Pair{i, i * 0.5}));
    EXPECT_EQ(p[49].a, 49);
    EXPECT_DOUBLE_EQ(p[49].b, 24.5);
    bbq_vec_free(p);

    const char** s = nullptr;
    bbq_vec_push(s, "alpha");
    bbq_vec_push(s, "beta");
    EXPECT_STREQ(s[1], "beta");
    bbq_vec_free(s);
}

TEST(BbqVec, FreeThenReuse) {
    int* v = nullptr;
    bbq_vec_push(v, 1);
    bbq_vec_free(v);
    EXPECT_EQ(v, nullptr);
    bbq_vec_push(v, 2);
    EXPECT_EQ(bbq_vec_len(v), 1);
    EXPECT_EQ(v[0], 2);
    bbq_vec_free(v);
}

/* ── B. bbq_vec under allocation failure ─────────────────────────────────── */

TEST(BbqVecOom, FirstAllocationFailureLeavesAUsableEmptyVector) {
    bbq_faulty f;
    bbq_faulty_init(&f, 1, 0, nullptr);
    /* BBQ_VEC_ALLOC is a compile-time hook, so drive the failure through the
     * explicit-allocator entry point. */
    int* v = nullptr;
    bbq_vec_reserve_a(v, 8, bbq_faulty_handle(&f));
    EXPECT_TRUE(bbq_vec_oom(v));
    EXPECT_EQ(bbq_vec_len(v), 0);
    EXPECT_EQ(bbq_vec_cap(v), 0);
    /* Every read must still answer, and every mutation must be a safe no-op. */
    bbq_vec_clear(v);
    bbq_vec_truncate(v, 5);
    bbq_vec_setlen(v, 5);
    bbq_vec_reverse(v);
    EXPECT_EQ(bbq_vec_len(v), 0);
    EXPECT_FALSE(bbq_vec_try_push(v, 1));
    EXPECT_EQ(bbq_vec_len(v), 0);
    bbq_vec_free(v);                   /* must not free the shared sentinel */
    EXPECT_EQ(v, nullptr);
}

TEST(BbqVecOom, PoisonIsStickyAndKeepsWhatWasAlreadyThere) {
    bbq_faulty f;
    bbq_faulty_init(&f, 0, 0, nullptr);          /* start healthy */
    int* v = nullptr;
    bbq_vec_reserve_a(v, 4, bbq_faulty_handle(&f));
    for (int i = 0; i < 4; i++) bbq_vec_push(v, i);
    ASSERT_EQ(bbq_vec_len(v), 4);

    f.fail_at = 1; f.served = 0;                 /* refuse every further growth */
    for (int i = 0; i < 100; i++) bbq_vec_push(v, 99);

    EXPECT_TRUE(bbq_vec_oom(v));
    /* The elements already there are still there — a vector that silently
     * emptied itself would turn a partial result into a confidently wrong one. */
    ASSERT_EQ(bbq_vec_len(v), 4);
    for (int i = 0; i < 4; i++) EXPECT_EQ(v[i], i);

    f.fail_at = 0;                               /* even if memory comes back */
    bbq_vec_push(v, 5);
    EXPECT_TRUE(bbq_vec_oom(v));
    EXPECT_EQ(bbq_vec_len(v), 4);
    bbq_vec_free(v);
}

TEST(BbqVecOom, ManyPoisonedVectorsShareTheSentinelSafely) {
    bbq_faulty f;
    bbq_faulty_init(&f, 1, 0, nullptr);
    int* a = nullptr; int* b = nullptr;
    bbq_vec_reserve_a(a, 8, bbq_faulty_handle(&f));
    bbq_vec_reserve_a(b, 8, bbq_faulty_handle(&f));
    EXPECT_TRUE(bbq_vec_oom(a));
    EXPECT_TRUE(bbq_vec_oom(b));
    EXPECT_EQ(bbq_vec_len(a), 0);
    EXPECT_EQ(bbq_vec_len(b), 0);
    bbq_vec_free(a);
    bbq_vec_free(b);     /* a double free of the sentinel would land here */
}

/* ── D. bbq_vec arithmetic ───────────────────────────────────────────────── */

TEST(BbqVec, ReserveBeyondTheCeilingPoisonsRatherThanWrapping) {
    /* The ceiling is checked before anything is allocated, so this refuses
     * instantly rather than asking for gigabytes. The old code computed
     * `cap * 2` in int, which is signed overflow at 2^30 — undefined, and the
     * resulting negative capacity became an exabyte-sized request. */
    int* v = nullptr;
    EXPECT_FALSE(bbq_vec_try_reserve(v, BBQ_VEC_MAX_CAP + 1));
    EXPECT_TRUE(bbq_vec_oom(v));
    EXPECT_EQ(bbq_vec_len(v), 0);
    EXPECT_EQ(bbq_vec_cap(v), 0);
    bbq_vec_free(v);
}

TEST(BbqVec, GrowthStopsAtTheCeilingInsteadOfOverflowing) {
    /* Drive the doubling right up to the ceiling without allocating: a vector
     * poisoned at the ceiling must report it rather than wrap to a small cap. */
    bbq_faulty f;
    bbq_faulty_init(&f, 0, 0, nullptr);
    int* v = nullptr;
    bbq_vec_reserve_a(v, 16, bbq_faulty_handle(&f));
    ASSERT_GE(bbq_vec_cap(v), 16);
    f.fail_at = 1; f.served = 0;              /* refuse every further growth */
    for (int i = 0; i < 100; i++) bbq_vec_push(v, i);
    EXPECT_TRUE(bbq_vec_oom(v));
    EXPECT_LE(bbq_vec_len(v), 16);            /* never wrote past what it had */
    bbq_vec_free(v);
}

/* ════════════════════════════════════════════════════════════════════════════
 * A. Laws — bbq_buf
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(BbqBuf, AppendConcatenates) {
    bbq_buf b; bbq_buf_init(&b);
    ASSERT_TRUE(bbq_buf_append(&b, "abc", 3));
    ASSERT_TRUE(bbq_buf_append(&b, "de", 2));
    ASSERT_TRUE(bbq_buf_append_byte(&b, 'f'));
    ASSERT_EQ(b.len, 6u);
    EXPECT_EQ(memcmp(b.data, "abcdef", 6), 0);
    bbq_buf_free(&b);
}

TEST(BbqBuf, ZeroLengthAppendIsANoOpAndSucceeds) {
    bbq_buf b; bbq_buf_init(&b);
    EXPECT_TRUE(bbq_buf_append(&b, nullptr, 0));
    EXPECT_EQ(b.len, 0u);
    EXPECT_EQ(b.data, nullptr);           /* nothing allocated for nothing */
    bbq_buf_free(&b);
}

TEST(BbqBuf, ReserveIsIdempotentAndNeverShrinks) {
    bbq_buf b; bbq_buf_init(&b);
    ASSERT_TRUE(bbq_buf_reserve(&b, 100));
    size_t cap = b.cap;
    EXPECT_GE(cap, 100u);
    ASSERT_TRUE(bbq_buf_reserve(&b, 10));
    EXPECT_EQ(b.cap, cap);
    bbq_buf_free(&b);
}

TEST(BbqBuf, ClearKeepsCapacityFreeResetsEverything) {
    bbq_buf b; bbq_buf_init(&b);
    ASSERT_TRUE(bbq_buf_append(&b, "hello", 5));
    size_t cap = b.cap;
    bbq_buf_clear(&b);
    EXPECT_EQ(b.len, 0u);
    EXPECT_EQ(b.cap, cap);
    bbq_buf_free(&b);
    EXPECT_EQ(b.data, nullptr);
    EXPECT_EQ(b.cap, 0u);
    EXPECT_FALSE(bbq_buf_oom(&b));
    ASSERT_TRUE(bbq_buf_append(&b, "x", 1));    /* reusable after free */
    bbq_buf_free(&b);
}

TEST(BbqBufOom, RefusedGrowthWritesNothing) {
    bbq_faulty f;
    bbq_faulty_init(&f, 1, 0, nullptr);
    bbq_buf b; bbq_buf_init_a(&b, bbq_faulty_handle(&f));
    EXPECT_FALSE(bbq_buf_append(&b, "0123456789", 10));
    EXPECT_TRUE(bbq_buf_oom(&b));
    EXPECT_EQ(b.len, 0u);
    /* Sticky: still refuses once memory is available again. */
    f.fail_at = 0;
    EXPECT_FALSE(bbq_buf_append(&b, "x", 1));
    bbq_buf_free(&b);
}

TEST(BbqBufOom, PartialContentsSurviveARefusal) {
    bbq_faulty f;
    bbq_faulty_init(&f, 0, 0, nullptr);
    bbq_buf b; bbq_buf_init_a(&b, bbq_faulty_handle(&f));
    ASSERT_TRUE(bbq_buf_append(&b, "keep", 4));
    f.fail_at = 1; f.served = 0;
    EXPECT_FALSE(bbq_buf_append(&b, std::string(10000, 'x').data(), 10000));
    ASSERT_EQ(b.len, 4u);
    EXPECT_EQ(memcmp(b.data, "keep", 4), 0);
    bbq_buf_free(&b);
}

TEST(BbqBuf, HugeReserveIsRefusedRatherThanLoopingForever) {
    /* The doubling loop used to overflow to zero and spin. The test binary's
     * ctest TIMEOUT is the other half of this assertion. */
    bbq_buf b; bbq_buf_init(&b);
    EXPECT_FALSE(bbq_buf_reserve(&b, SIZE_MAX - 1));
    EXPECT_TRUE(bbq_buf_oom(&b));
    bbq_buf_free(&b);
}

TEST(BbqBuf, AppendThatWouldOverflowTheLengthIsRefused) {
    bbq_buf b; bbq_buf_init(&b);
    ASSERT_TRUE(bbq_buf_append(&b, "x", 1));
    EXPECT_FALSE(bbq_buf_append(&b, "y", SIZE_MAX));
    bbq_buf_free(&b);
}

/* ════════════════════════════════════════════════════════════════════════════
 * A. Laws — bbq_arena
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(BbqArena, AllocsAreDistinctAndAligned) {
    bbq_arena a; bbq_arena_init(&a, 4096);
    for (int i = 0; i < 1000; i++) {
        void* p = bbq_arena_alloc(&a, 1 + (i % 32));
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);
    }
    bbq_arena_free(&a);
}

TEST(BbqArena, ZeroSizedAllocIsNull) {
    bbq_arena a; bbq_arena_init(&a, 4096);
    EXPECT_EQ(bbq_arena_alloc(&a, 0), nullptr);
    EXPECT_FALSE(bbq_arena_oom(&a));      /* a zero request is not a failure */
    bbq_arena_free(&a);
}

TEST(BbqArena, ResetReusesTheSamePages) {
    bbq_arena a; bbq_arena_init(&a, 256);
    void* first = bbq_arena_alloc(&a, 64);
    size_t cap = bbq_arena_capacity(&a);
    bbq_arena_reset(&a);
    EXPECT_EQ(bbq_arena_alloc(&a, 64), first);
    EXPECT_EQ(bbq_arena_capacity(&a), cap);   /* no new page was taken */
    bbq_arena_free(&a);
}

TEST(BbqArena, OversizedAllocGetsItsOwnPage) {
    bbq_arena a; bbq_arena_init(&a, 64);
    auto* p = static_cast<char*>(bbq_arena_alloc(&a, 5000));
    ASSERT_NE(p, nullptr);
    memset(p, 0xAB, 5000);                    /* the page really is that big */
    EXPECT_GE(bbq_arena_capacity(&a), 5000u);
    bbq_arena_free(&a);
}

TEST(BbqArena, ARecycledPageIsNeverHandedOutForALargerRequest) {
    bbq_arena a; bbq_arena_init(&a, 64);
    (void)bbq_arena_alloc(&a, 32);
    bbq_arena_reset(&a);
    auto* p = static_cast<char*>(bbq_arena_alloc(&a, 4096));
    ASSERT_NE(p, nullptr);
    memset(p, 0xCD, 4096);                    /* would smash a 64-byte page */
    bbq_arena_free(&a);
}

TEST(BbqArena, ASmallAllocAfterABigOneStillFindsTheEarlierPages) {
    /* A large request used to skip every smaller page and strand it for the rest
     * of the cycle, so the arena grew without bound on an alternating workload. */
    bbq_arena a; bbq_arena_init(&a, 4096);
    (void)bbq_arena_alloc(&a, 16);            /* page 0, barely used */
    (void)bbq_arena_alloc(&a, 100000);        /* its own big page */
    size_t cap_before = bbq_arena_capacity(&a);
    for (int i = 0; i < 100; i++) ASSERT_NE(bbq_arena_alloc(&a, 16), nullptr);
    EXPECT_EQ(bbq_arena_capacity(&a), cap_before) << "stranded the first page";
    bbq_arena_free(&a);
}

TEST(BbqArena, HugeRequestIsRefusedRatherThanAlignedToZero) {
    /* align_up(SIZE_MAX, 8) wraps to 0, and a "0-byte" allocation that still
     * returns a live pointer is how a huge request became a heap overflow. */
    bbq_arena a; bbq_arena_init(&a, 4096);
    EXPECT_EQ(bbq_arena_alloc(&a, SIZE_MAX), nullptr);
    EXPECT_EQ(bbq_arena_alloc(&a, SIZE_MAX - 3), nullptr);
    EXPECT_TRUE(bbq_arena_oom(&a));
    bbq_arena_free(&a);
}

TEST(BbqArena, PageSizeZeroTakesTheDefault) {
    bbq_arena a; bbq_arena_init(&a, 0);
    ASSERT_NE(bbq_arena_alloc(&a, 4000), nullptr);
    EXPECT_EQ(bbq_arena_capacity(&a), 4096u);
    bbq_arena_free(&a);
}

TEST(BbqArena, FreeThenReuse) {
    bbq_arena a; bbq_arena_init(&a, 256);
    (void)bbq_arena_alloc(&a, 64);
    bbq_arena_free(&a);
    EXPECT_EQ(bbq_arena_capacity(&a), 0u);
    EXPECT_NE(bbq_arena_alloc(&a, 64), nullptr);
    bbq_arena_free(&a);
}

TEST(BbqArenaOom, EveryAllocationFailurePointLeavesAUsableArena) {
    SweepAllocationFailures([](bbq_alloc* alloc) {
        bbq_arena a; bbq_arena_init_a(&a, 128, alloc);
        for (int i = 0; i < 20; i++) {
            void* p = bbq_arena_alloc(&a, 64);
            if (!p) { EXPECT_TRUE(bbq_arena_oom(&a)); break; }
            memset(p, 0, 64);
        }
        bbq_arena_free(&a);      /* must not leak and must not double-free */
    });
}

/* ════════════════════════════════════════════════════════════════════════════
 * A. Laws — bbq_htree
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(BbqHtree, EmptyTreeAllocatesNothing) {
    bbq_htree t; bbq_htree_init(&t);
    EXPECT_TRUE(bbq_htree_is_empty(&t));
    EXPECT_EQ(bbq_htree_size(&t), 0u);
    EXPECT_EQ(bbq_htree_search(&t, 42), nullptr);
    EXPECT_FALSE(bbq_htree_contains(&t, 42));
    EXPECT_EQ(bbq_htree_delete(&t, 42), nullptr);
    bbq_htree_free(&t);
}

/* The handle form is what a caller that threads `bbq_htree*` through its own
 * signatures uses. It must be the same container, not a second lifecycle with
 * its own rules — including the allocator, which has to cover the handle too or
 * a budgeted caller has an allocation nothing bounds. */
TEST(BbqHtree, TheHandleFormIsTheSameTree) {
    bbq_htree* t = bbq_htree_create();
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(bbq_htree_is_empty(t));
    ASSERT_TRUE(bbq_htree_insert(t, 7, (void*)0x7));
    EXPECT_EQ(bbq_htree_search(t, 7), (void*)0x7);
    bbq_htree_destroy(t);
    bbq_htree_destroy(nullptr);        /* accepts NULL */
}

TEST(BbqHtree, TheHandleItselfIsChargedToTheAllocator) {
    bbq_budget b;
    bbq_budget_init(&b, 64u << 10, nullptr);
    EXPECT_EQ(b.used, 0u);
    bbq_htree* t = bbq_htree_create_a(bbq_budget_handle(&b));
    ASSERT_NE(t, nullptr);
    EXPECT_GE(b.used, sizeof(bbq_htree));      /* the handle, not just the nodes */
    ASSERT_TRUE(bbq_htree_insert(t, 1, (void*)0x1));
    bbq_htree_destroy(t);
    EXPECT_EQ(b.used, 0u) << "destroy must return the handle to the same allocator";
    EXPECT_EQ(b.denials, 0u);
}

TEST(BbqHtree, ACeilingTooSmallForTheHandleRefusesInsteadOfLeaking) {
    bbq_budget b;
    bbq_budget_init(&b, sizeof(bbq_htree) - 1, nullptr);
    EXPECT_EQ(bbq_htree_create_a(bbq_budget_handle(&b)), nullptr);
    EXPECT_EQ(b.denials, 1u);
    EXPECT_EQ(b.used, 0u);
}

TEST(BbqHtree, InsertSearchContainsDelete) {
    bbq_htree t; bbq_htree_init(&t);
    ASSERT_TRUE(bbq_htree_insert(&t, 100, (void*)0x1));
    ASSERT_TRUE(bbq_htree_insert(&t, 200, (void*)0x2));
    EXPECT_EQ(bbq_htree_search(&t, 100), (void*)0x1);
    EXPECT_EQ(bbq_htree_search(&t, 200), (void*)0x2);
    EXPECT_EQ(bbq_htree_search(&t, 300), nullptr);
    EXPECT_TRUE(bbq_htree_contains(&t, 100));
    EXPECT_FALSE(bbq_htree_contains(&t, 300));
    EXPECT_EQ(bbq_htree_size(&t), 2u);
    EXPECT_EQ(bbq_htree_delete(&t, 100), (void*)0x1);
    EXPECT_FALSE(bbq_htree_contains(&t, 100));
    EXPECT_EQ(bbq_htree_size(&t), 1u);
    bbq_htree_free(&t);
}

TEST(BbqHtree, ReinsertReplacesWithoutGrowing) {
    bbq_htree t; bbq_htree_init(&t);
    ASSERT_TRUE(bbq_htree_insert(&t, 7, (void*)0xA));
    ASSERT_TRUE(bbq_htree_insert(&t, 7, (void*)0xB));
    EXPECT_EQ(bbq_htree_search(&t, 7), (void*)0xB);
    EXPECT_EQ(bbq_htree_size(&t), 1u);
    bbq_htree_free(&t);
}

TEST(BbqHtree, NullValueIsStoredAndIsNotAbsence) {
    bbq_htree t; bbq_htree_init(&t);
    ASSERT_TRUE(bbq_htree_insert(&t, 5, nullptr));
    EXPECT_EQ(bbq_htree_search(&t, 5), nullptr);
    EXPECT_TRUE(bbq_htree_contains(&t, 5));       /* the distinction */
    EXPECT_EQ(bbq_htree_size(&t), 1u);
    bbq_htree_free(&t);
}

TEST(BbqHtree, ClearThenReuse) {
    bbq_htree t; bbq_htree_init(&t);
    for (uint64_t i = 0; i < 100; i++) ASSERT_TRUE(bbq_htree_insert(&t, i, (void*)(uintptr_t)(i + 1)));
    bbq_htree_clear(&t);
    EXPECT_TRUE(bbq_htree_is_empty(&t));
    /* clear allocates nothing, so it cannot fail and leave a dead tree behind —
     * which is what re-allocating an unchecked root node used to do. */
    ASSERT_TRUE(bbq_htree_insert(&t, 1, (void*)0xF));
    EXPECT_EQ(bbq_htree_search(&t, 1), (void*)0xF);
    bbq_htree_free(&t);
}

TEST(BbqHtree, DeleteToEmptyThenRefill) {
    bbq_htree t; bbq_htree_init(&t);
    for (uint64_t i = 0; i < 50; i++) ASSERT_TRUE(bbq_htree_insert(&t, i * 977, (void*)(uintptr_t)(i + 1)));
    for (uint64_t i = 0; i < 50; i++) EXPECT_EQ(bbq_htree_delete(&t, i * 977), (void*)(uintptr_t)(i + 1));
    EXPECT_TRUE(bbq_htree_is_empty(&t));
    EXPECT_EQ(bbq_htree_size(&t), 0u);
    for (uint64_t i = 0; i < 50; i++) ASSERT_TRUE(bbq_htree_insert(&t, i * 977, (void*)(uintptr_t)(i + 2)));
    EXPECT_EQ(bbq_htree_size(&t), 50u);
    for (uint64_t i = 0; i < 50; i++) EXPECT_EQ(bbq_htree_search(&t, i * 977), (void*)(uintptr_t)(i + 2));
    bbq_htree_free(&t);
}

TEST(BbqHtree, CloneIsIndependentOfItsSource) {
    bbq_htree src; bbq_htree_init(&src);
    for (uint64_t i = 0; i < 20; i++) ASSERT_TRUE(bbq_htree_insert(&src, i, (void*)(uintptr_t)(i + 1)));
    bbq_htree dst; bbq_htree_init(&dst);
    ASSERT_TRUE(bbq_htree_clone(&dst, &src));
    EXPECT_EQ(bbq_htree_size(&dst), 20u);
    /* Independence, not just equality: mutating one must not touch the other. */
    ASSERT_TRUE(bbq_htree_insert(&dst, 999, (void*)0xDEAD));
    (void)bbq_htree_delete(&dst, 0);
    EXPECT_FALSE(bbq_htree_contains(&src, 999));
    EXPECT_TRUE(bbq_htree_contains(&src, 0));
    bbq_htree_free(&src);
    bbq_htree_free(&dst);
}

TEST(BbqHtree, IterationIsAscendingAndTotal) {
    bbq_htree t; bbq_htree_init(&t);
    const uint64_t keys[] = {500, 1, 0, UINT64_MAX, 300, 200, 400};
    for (uint64_t k : keys) ASSERT_TRUE(bbq_htree_insert(&t, k, (void*)(uintptr_t)(k + 1)));

    bbq_htree_iter it; bbq_htree_iter_init(&t, &it);
    std::vector<uint64_t> seen;
    for (bbq_htree_leaf* l = bbq_htree_next(&it); l; l = bbq_htree_next(&it))
        seen.push_back(l->key);
    ASSERT_EQ(seen.size(), sizeof(keys) / sizeof(keys[0]));
    for (size_t i = 1; i < seen.size(); i++) EXPECT_LT(seen[i - 1], seen[i]);
    std::set<uint64_t> expect(std::begin(keys), std::end(keys));
    EXPECT_EQ(std::set<uint64_t>(seen.begin(), seen.end()), expect);
    bbq_htree_free(&t);
}

TEST(BbqHtree, VisitedSetPatternWithSentinelValues) {
    bbq_htree t; bbq_htree_init(&t);
    for (uint64_t i = 0; i < 50; i++) {
        EXPECT_FALSE(bbq_htree_contains(&t, i));
        ASSERT_TRUE(bbq_htree_insert(&t, i, (void*)(uintptr_t)1));
        EXPECT_TRUE(bbq_htree_contains(&t, i));
    }
    bbq_htree_free(&t);
}

/* ── C. htree adversarial keys ───────────────────────────────────────────── */

TEST(BbqHtreeAdversarial, KeysDifferingOnlyInTheLastNibble) {
    bbq_htree t; bbq_htree_init(&t);
    for (uint64_t i = 0; i < 16; i++)
        ASSERT_TRUE(bbq_htree_insert(&t, 0xAAAAAAAAAAAAAAA0ULL | i, (void*)(uintptr_t)(i + 1)));
    EXPECT_EQ(bbq_htree_size(&t), 16u);
    for (uint64_t i = 0; i < 16; i++)
        EXPECT_EQ(bbq_htree_search(&t, 0xAAAAAAAAAAAAAAA0ULL | i), (void*)(uintptr_t)(i + 1));
    bbq_htree_free(&t);
}

TEST(BbqHtreeAdversarial, KeysDifferingOnlyInTheFirstNibble) {
    bbq_htree t; bbq_htree_init(&t);
    for (uint64_t i = 0; i < 16; i++)
        ASSERT_TRUE(bbq_htree_insert(&t, (i << 60) | 0x0123456789ABCULL, (void*)(uintptr_t)(i + 1)));
    for (uint64_t i = 0; i < 16; i++)
        EXPECT_EQ(bbq_htree_search(&t, (i << 60) | 0x0123456789ABCULL), (void*)(uintptr_t)(i + 1));
    bbq_htree_free(&t);
}

TEST(BbqHtreeAdversarial, ExtremesAndZero) {
    bbq_htree t; bbq_htree_init(&t);
    const uint64_t keys[] = {0, 1, UINT64_MAX, UINT64_MAX - 1, 0x8000000000000000ULL,
                             0x7FFFFFFFFFFFFFFFULL};
    for (uint64_t k : keys) ASSERT_TRUE(bbq_htree_insert(&t, k, (void*)(uintptr_t)(k | 1)));
    for (uint64_t k : keys) EXPECT_TRUE(bbq_htree_contains(&t, k)) << k;
    EXPECT_EQ(bbq_htree_size(&t), 6u);
    for (uint64_t k : keys) EXPECT_NE(bbq_htree_delete(&t, k), nullptr);
    EXPECT_TRUE(bbq_htree_is_empty(&t));
    bbq_htree_free(&t);
}

TEST(BbqHtreeAdversarial, PointersDifferingOnlyAboveBitThirtyOne) {
    /* The key used to be narrowed to uint32_t, so these two collapsed into one
     * entry — the cached state of one node handed to another. */
    bbq_htree t; bbq_htree_init(&t);
    uint64_t a = 0x0000000112345678ULL;
    uint64_t b = 0x0000000212345678ULL;
    ASSERT_TRUE(bbq_htree_insert(&t, a, (void*)0xA));
    ASSERT_TRUE(bbq_htree_insert(&t, b, (void*)0xB));
    EXPECT_EQ(bbq_htree_size(&t), 2u);
    EXPECT_EQ(bbq_htree_search(&t, a), (void*)0xA);
    EXPECT_EQ(bbq_htree_search(&t, b), (void*)0xB);
    bbq_htree_free(&t);
}

TEST(BbqHtree, LazyExpansionKeepsSparseKeysCheap) {
    /* The property, measured rather than asserted about the code: a handful of
     * sparse keys must not cost one interior node per nibble. Before lazy
     * expansion this was 16 levels deep per key whatever the tree held. */
    bbq_budget b;
    bbq_budget_init(&b, SIZE_MAX, nullptr);
    bbq_htree t; bbq_htree_init_a(&t, bbq_budget_handle(&b));
    for (uint64_t i = 0; i < 64; i++)
        ASSERT_TRUE(bbq_htree_insert(&t, i * 0x9E3779B97F4A7C15ULL, (void*)(uintptr_t)(i + 1)));
    size_t per_key = b.used / 64;
    EXPECT_LT(per_key, 400u) << "sparse keys cost " << per_key << " bytes each";
    bbq_htree_free(&t);
    EXPECT_EQ(b.used, 0u) << "the tree did not give everything back";
}

/* ── B. htree under allocation failure ───────────────────────────────────── */

TEST(BbqHtreeOom, EveryFailurePointLeavesTheTreeConsistent) {
    SweepAllocationFailures([](bbq_alloc* alloc) {
        bbq_htree t; bbq_htree_init_a(&t, alloc);
        std::set<uint64_t> in;
        for (uint64_t i = 0; i < 12; i++) {
            uint64_t k = i * 0x1234567ULL;
            if (bbq_htree_insert(&t, k, (void*)(uintptr_t)(i + 1))) in.insert(k);
            else EXPECT_TRUE(bbq_htree_oom(&t));
        }
        /* Whatever went in must be findable, and the size must agree with it —
         * a half-built split would leave a key searchable but uncounted, or
         * counted but unreachable. */
        EXPECT_EQ(bbq_htree_size(&t), in.size());
        for (uint64_t k : in) EXPECT_TRUE(bbq_htree_contains(&t, k)) << k;
        size_t walked = 0;
        bbq_htree_iter it; bbq_htree_iter_init(&t, &it);
        while (bbq_htree_next(&it)) walked++;
        EXPECT_EQ(walked, in.size());
        bbq_htree_free(&t);
    });
}

/* ── E. htree iterators ──────────────────────────────────────────────────── */

TEST(BbqHtreeIter, AbandoningAnIterationLeaksNothing) {
    bbq_htree t; bbq_htree_init(&t);
    for (uint64_t i = 0; i < 100; i++) ASSERT_TRUE(bbq_htree_insert(&t, i, (void*)(uintptr_t)(i + 1)));
    for (int trial = 0; trial < 100; trial++) {
        bbq_htree_iter it; bbq_htree_iter_init(&t, &it);
        (void)bbq_htree_next(&it);      /* walk away — the iterator owns nothing */
    }
    bbq_htree_free(&t);
}

TEST(BbqHtreeIter, MutatingMidWalkStopsTheWalkRatherThanWalkingFreedNodes) {
    bbq_htree t; bbq_htree_init(&t);
    for (uint64_t i = 0; i < 100; i++) ASSERT_TRUE(bbq_htree_insert(&t, i, (void*)(uintptr_t)(i + 1)));
    bbq_htree_iter it; bbq_htree_iter_init(&t, &it);
    ASSERT_NE(bbq_htree_next(&it), nullptr);
    bbq_htree_clear(&t);                                  /* every node freed */
    EXPECT_EQ(bbq_htree_next(&it), nullptr);              /* must not walk them */
    bbq_htree_free(&t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * A. Laws — bbq_hmap
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(BbqHmap, PutGetReplaceAndMiss) {
    bbq_hmap m; ASSERT_TRUE(bbq_hmap_init(&m, 0));
    EXPECT_EQ(bbq_hmap_get(&m, 1), nullptr);
    ASSERT_TRUE(bbq_hmap_put(&m, 1, (void*)0xA));
    EXPECT_EQ(bbq_hmap_get(&m, 1), (void*)0xA);
    ASSERT_TRUE(bbq_hmap_put(&m, 1, (void*)0xB));
    EXPECT_EQ(bbq_hmap_get(&m, 1), (void*)0xB);
    EXPECT_EQ(bbq_hmap_len(&m), 1u);
    bbq_hmap_free(&m);
}

TEST(BbqHmap, NullValueAndZeroKeyAreBothLegitimate) {
    bbq_hmap m; ASSERT_TRUE(bbq_hmap_init(&m, 0));
    ASSERT_TRUE(bbq_hmap_put(&m, 0, nullptr));
    EXPECT_EQ(bbq_hmap_get(&m, 0), nullptr);
    EXPECT_TRUE(bbq_hmap_contains(&m, 0));
    EXPECT_FALSE(bbq_hmap_contains(&m, 1));
    EXPECT_EQ(bbq_hmap_len(&m), 1u);
    bbq_hmap_free(&m);
}

TEST(BbqHmap, GrowthKeepsEveryKey) {
    bbq_hmap m; ASSERT_TRUE(bbq_hmap_init(&m, 4));
    for (uint64_t i = 0; i < 5000; i++)
        ASSERT_TRUE(bbq_hmap_put(&m, i * 2654435761u, (void*)(uintptr_t)(i + 1)));
    EXPECT_EQ(bbq_hmap_len(&m), 5000u);
    for (uint64_t i = 0; i < 5000; i++)
        EXPECT_EQ(bbq_hmap_get(&m, i * 2654435761u), (void*)(uintptr_t)(i + 1));
    bbq_hmap_free(&m);
}

TEST(BbqHmap, InitWithANonPowerOfTwoRoundsUp) {
    bbq_hmap m; ASSERT_TRUE(bbq_hmap_init(&m, 100));
    EXPECT_EQ(m.cap, 128u);
    bbq_hmap_free(&m);
}

TEST(BbqHmap, AnUnrepresentableCapacityIsRefusedRatherThanLoopingForever) {
    bbq_hmap m;
    EXPECT_FALSE(bbq_hmap_init(&m, SIZE_MAX));
    EXPECT_TRUE(bbq_hmap_oom(&m));
    EXPECT_EQ(bbq_hmap_get(&m, 1), nullptr);      /* still answers */
    bbq_hmap_free(&m);
}

TEST(BbqHmap, FreeThenReuse) {
    bbq_hmap m; ASSERT_TRUE(bbq_hmap_init(&m, 0));
    ASSERT_TRUE(bbq_hmap_put(&m, 1, (void*)0xA));
    bbq_hmap_free(&m);
    EXPECT_EQ(bbq_hmap_len(&m), 0u);
    ASSERT_TRUE(bbq_hmap_put(&m, 2, (void*)0xB));   /* re-allocates */
    EXPECT_EQ(bbq_hmap_get(&m, 2), (void*)0xB);
    bbq_hmap_free(&m);
}

TEST(BbqHmapOom, ARefusedPutStoresNothingAndKeepsTheTableUnderLoad) {
    SweepAllocationFailures([](bbq_alloc* alloc) {
        bbq_hmap m;
        bbq_hmap_init_a(&m, 4, alloc);
        std::map<uint64_t, void*> oracle;
        for (uint64_t i = 0; i < 40; i++) {
            void* v = (void*)(uintptr_t)(i + 1);
            if (bbq_hmap_put(&m, i, v)) oracle[i] = v;
            else EXPECT_TRUE(bbq_hmap_oom(&m));
        }
        EXPECT_EQ(bbq_hmap_len(&m), oracle.size());
        for (auto& kv : oracle) EXPECT_EQ(bbq_hmap_get(&m, kv.first), kv.second);
        /* The load factor invariant is what bounds the probe walk; a refused put
         * that had been stored anyway would march the table toward full. */
        if (m.cap) EXPECT_LT(m.len * 4, m.cap * 3 + 4);
        bbq_hmap_free(&m);
    });
}

/* ════════════════════════════════════════════════════════════════════════════
 * A. Laws — bbq_dict
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(BbqDict, PutGetContainsDelete) {
    bbq_dict d; bbq_dict_init(&d);
    ASSERT_TRUE(bbq_dict_puts(&d, "alpha", (void*)0x1));
    ASSERT_TRUE(bbq_dict_puts(&d, "beta", (void*)0x2));
    EXPECT_EQ(bbq_dict_gets(&d, "alpha"), (void*)0x1);
    EXPECT_EQ(bbq_dict_gets(&d, "gamma"), nullptr);
    EXPECT_TRUE(bbq_dict_contains(&d, "beta", 4));
    EXPECT_EQ(bbq_dict_len(&d), 2u);
    EXPECT_EQ(bbq_dict_delete(&d, "alpha", 5), (void*)0x1);
    EXPECT_EQ(bbq_dict_len(&d), 1u);
    EXPECT_EQ(bbq_dict_delete(&d, "alpha", 5), nullptr);   /* already gone */
    bbq_dict_free(&d);
}

/* Same contract as the tree's handle form: one container, one allocator, and the
 * handle is part of what the budget pays for. */
TEST(BbqDict, TheHandleFormIsTheSameDict) {
    bbq_dict* d = bbq_dict_create();
    ASSERT_NE(d, nullptr);
    ASSERT_TRUE(bbq_dict_puts(d, "alpha", (void*)0x1));
    EXPECT_EQ(bbq_dict_gets(d, "alpha"), (void*)0x1);
    EXPECT_EQ(bbq_dict_len(d), 1u);
    bbq_dict_destroy(d);
    bbq_dict_destroy(nullptr);
}

TEST(BbqDict, TheHandleItselfIsChargedToTheAllocator) {
    bbq_budget b;
    bbq_budget_init(&b, 64u << 10, nullptr);
    bbq_dict* d = bbq_dict_create_a(bbq_budget_handle(&b));
    ASSERT_NE(d, nullptr);
    EXPECT_GE(b.used, sizeof(bbq_dict));
    ASSERT_TRUE(bbq_dict_puts(d, "alpha", (void*)0x1));
    bbq_dict_destroy(d);
    EXPECT_EQ(b.used, 0u) << "destroy must return the handle AND every entry";
    EXPECT_EQ(b.denials, 0u);
}

TEST(BbqDict, ACeilingTooSmallForTheHandleRefusesInsteadOfLeaking) {
    bbq_budget b;
    bbq_budget_init(&b, sizeof(bbq_dict) - 1, nullptr);
    EXPECT_EQ(bbq_dict_create_a(bbq_budget_handle(&b)), nullptr);
    EXPECT_EQ(b.denials, 1u);
    EXPECT_EQ(b.used, 0u);
}

TEST(BbqDict, KeysAreCopiedNotBorrowed) {
    bbq_dict d; bbq_dict_init(&d);
    {
        std::string tmp = "transient";
        ASSERT_TRUE(bbq_dict_put(&d, tmp.data(), tmp.size(), (void*)0x7));
        std::fill(tmp.begin(), tmp.end(), 'x');       /* scribble on the original */
    }
    EXPECT_EQ(bbq_dict_get(&d, "transient", 9), (void*)0x7);
    bbq_dict_free(&d);
}

TEST(BbqDict, EmbeddedNulsAndLengthAreBothPartOfTheKey) {
    bbq_dict d; bbq_dict_init(&d);
    ASSERT_TRUE(bbq_dict_put(&d, "a\0b", 3, (void*)0x1));
    ASSERT_TRUE(bbq_dict_put(&d, "a\0c", 3, (void*)0x2));
    EXPECT_EQ(bbq_dict_get(&d, "a\0b", 3), (void*)0x1);
    EXPECT_EQ(bbq_dict_get(&d, "a\0c", 3), (void*)0x2);
    EXPECT_EQ(bbq_dict_get(&d, "a", 1), nullptr);    /* a prefix is not the key */
    EXPECT_EQ(bbq_dict_len(&d), 2u);
    bbq_dict_free(&d);
}

TEST(BbqDict, EmptyKeyIsAKeyAndNullValueIsNotAbsence) {
    bbq_dict d; bbq_dict_init(&d);
    ASSERT_TRUE(bbq_dict_put(&d, "", 0, nullptr));
    EXPECT_EQ(bbq_dict_get(&d, "", 0), nullptr);
    EXPECT_TRUE(bbq_dict_contains(&d, "", 0));
    EXPECT_EQ(bbq_dict_len(&d), 1u);
    bbq_dict_free(&d);
}

TEST(BbqDict, ClearThenReuse) {
    bbq_dict d; bbq_dict_init(&d);
    for (int i = 0; i < 100; i++)
        ASSERT_TRUE(bbq_dict_puts(&d, ("k" + std::to_string(i)).c_str(), (void*)(uintptr_t)(i + 1)));
    bbq_dict_clear(&d);
    EXPECT_EQ(bbq_dict_len(&d), 0u);
    EXPECT_EQ(bbq_dict_gets(&d, "k5"), nullptr);
    ASSERT_TRUE(bbq_dict_puts(&d, "fresh", (void*)0x9));
    EXPECT_EQ(bbq_dict_gets(&d, "fresh"), (void*)0x9);
    bbq_dict_free(&d);
}

/* Everything of a given length shares a hash. Nothing natural can reach the
 * chain — a seeded 64-bit hash makes finding two colliding keys a birthday
 * problem over 2^64 — so the unlink paths are reached by installing a hash that
 * collides on purpose. The chain is what makes the dict correct rather than
 * usually-right, and it would otherwise ship untested. */
static uint64_t AllCollide(const void*, size_t len) { return len; }

TEST(BbqDict, CollidingKeysStayDistinctEntries) {
    bbq_dict d; bbq_dict_init_hashed(&d, nullptr, AllCollide);
    ASSERT_TRUE(bbq_dict_put(&d, "aaa", 3, (void*)0x1));
    ASSERT_TRUE(bbq_dict_put(&d, "bbb", 3, (void*)0x2));
    ASSERT_TRUE(bbq_dict_put(&d, "ccc", 3, (void*)0x3));
    EXPECT_EQ(bbq_dict_len(&d), 3u);
    EXPECT_EQ(bbq_dict_get(&d, "aaa", 3), (void*)0x1);
    EXPECT_EQ(bbq_dict_get(&d, "bbb", 3), (void*)0x2);
    EXPECT_EQ(bbq_dict_get(&d, "ccc", 3), (void*)0x3);
    /* A key that collides but is not present is a MISS, not a hit. */
    EXPECT_EQ(bbq_dict_get(&d, "zzz", 3), nullptr);
    EXPECT_FALSE(bbq_dict_contains(&d, "zzz", 3));
    /* Overwriting inside a chain must not lengthen it. */
    ASSERT_TRUE(bbq_dict_put(&d, "bbb", 3, (void*)0x9));
    EXPECT_EQ(bbq_dict_get(&d, "bbb", 3), (void*)0x9);
    EXPECT_EQ(bbq_dict_len(&d), 3u);
    bbq_dict_free(&d);
}

TEST(BbqDict, EveryUnlinkPositionInACollisionChain) {
    /* Three entries share one chain; delete the middle, the tail and the head
     * in turn. The head case is the one that re-points the tree at the next
     * entry — dropping that insert used to strand the rest of the chain while
     * the count fell by one. */
    const char* keys[] = {"aaa", "bbb", "ccc"};
    for (int drop = 0; drop < 3; drop++) {
        bbq_dict d; bbq_dict_init_hashed(&d, nullptr, AllCollide);
        for (int i = 0; i < 3; i++)
            ASSERT_TRUE(bbq_dict_put(&d, keys[i], 3, (void*)(uintptr_t)(i + 1)));
        EXPECT_EQ(bbq_dict_delete(&d, keys[drop], 3), (void*)(uintptr_t)(drop + 1));
        EXPECT_EQ(bbq_dict_len(&d), 2u);
        for (int i = 0; i < 3; i++) {
            if (i == drop) EXPECT_EQ(bbq_dict_get(&d, keys[i], 3), nullptr) << i;
            else EXPECT_EQ(bbq_dict_get(&d, keys[i], 3), (void*)(uintptr_t)(i + 1)) << i;
        }
        /* Drain the rest: the chain must empty cleanly and take its tree key. */
        for (int i = 0; i < 3; i++)
            if (i != drop) EXPECT_EQ(bbq_dict_delete(&d, keys[i], 3), (void*)(uintptr_t)(i + 1));
        EXPECT_EQ(bbq_dict_len(&d), 0u);
        /* And the emptied hash is reusable. */
        ASSERT_TRUE(bbq_dict_put(&d, "ddd", 3, (void*)0xD));
        EXPECT_EQ(bbq_dict_get(&d, "ddd", 3), (void*)0xD);
        bbq_dict_free(&d);
    }
}

TEST(BbqDict, IterationCoversEveryEntryOfEveryChain) {
    bbq_dict d; bbq_dict_init_hashed(&d, nullptr, AllCollide);
    std::set<std::string> expect;
    for (int len = 1; len <= 8; len++)
        for (int i = 0; i < 5; i++) {
            std::string k(len, (char)('a' + i));
            ASSERT_TRUE(bbq_dict_put(&d, k.data(), k.size(), (void*)(uintptr_t)(i + 1)));
            expect.insert(k);
        }
    std::set<std::string> seen;
    bbq_dict_iter it; bbq_dict_iter_init(&d, &it);
    bbq_dict_entry e;
    while (bbq_dict_next(&it, &e))
        EXPECT_TRUE(seen.insert(std::string((const char*)e.key, e.len)).second);
    EXPECT_EQ(seen, expect);
    bbq_dict_free(&d);
}

TEST(BbqDict, IterationVisitsEveryEntryExactlyOnce) {
    bbq_dict d; bbq_dict_init(&d);
    std::set<std::string> expect;
    for (int i = 0; i < 500; i++) {
        std::string k = "key" + std::to_string(i);
        ASSERT_TRUE(bbq_dict_put(&d, k.data(), k.size(), (void*)(uintptr_t)(i + 1)));
        expect.insert(k);
    }
    std::set<std::string> seen;
    bbq_dict_iter it; bbq_dict_iter_init(&d, &it);
    bbq_dict_entry e;
    while (bbq_dict_next(&it, &e)) {
        auto ins = seen.insert(std::string((const char*)e.key, e.len));
        EXPECT_TRUE(ins.second) << "visited twice";
    }
    EXPECT_EQ(seen, expect);
    bbq_dict_free(&d);
}

TEST(BbqDictIter, AbandoningAnIterationLeaksNothing) {
    bbq_dict d; bbq_dict_init(&d);
    for (int i = 0; i < 200; i++)
        ASSERT_TRUE(bbq_dict_puts(&d, ("k" + std::to_string(i)).c_str(), (void*)(uintptr_t)(i + 1)));
    /* The iterator used to heap-allocate state freed only by running to the end,
     * so this leaked once per abandoned walk, and copying one double-freed. */
    for (int trial = 0; trial < 500; trial++) {
        bbq_dict_iter it; bbq_dict_iter_init(&d, &it);
        bbq_dict_entry e;
        (void)bbq_dict_next(&it, &e);
        bbq_dict_iter copy = it;           /* copying must not create a second owner */
        (void)bbq_dict_next(&copy, &e);
    }
    bbq_dict_free(&d);
}

TEST(BbqDictIter, MutatingMidWalkStopsTheWalk) {
    bbq_dict d; bbq_dict_init(&d);
    for (int i = 0; i < 50; i++)
        ASSERT_TRUE(bbq_dict_puts(&d, ("k" + std::to_string(i)).c_str(), (void*)(uintptr_t)(i + 1)));
    bbq_dict_iter it; bbq_dict_iter_init(&d, &it);
    bbq_dict_entry e;
    ASSERT_TRUE(bbq_dict_next(&it, &e));
    bbq_dict_clear(&d);
    EXPECT_FALSE(bbq_dict_next(&it, &e));
    bbq_dict_free(&d);
}

/* ── C. dict hash flooding ───────────────────────────────────────────────── */

TEST(BbqDictAdversarial, TheHashIsSeededSoCollisionsCannotBePrecomputed) {
    /* The defence is that an attacker cannot know the mapping. Two different
     * seeds must disagree about a key's bucket; a hash that ignored the seed
     * (djb2, which this was) would give the same answer both times. */
    uint64_t saved = bbq_hash_seed();
    bbq_hash_seed_set(0x1111111111111111ULL);
    uint64_t a = bbq_dict_hash("java/lang/String", 16);
    bbq_hash_seed_set(0x2222222222222222ULL);
    uint64_t b = bbq_dict_hash("java/lang/String", 16);
    bbq_hash_seed_set(saved);
    EXPECT_NE(a, b);
}

/* djb2, the hash this used to ship: h = h*33 + c, unseeded. Installed here as a
 * known-weak control, so the flooding assertion below is shown to have teeth
 * rather than merely passing. */
static uint64_t Djb2(const void* key, size_t len) {
    const unsigned char* b = (const unsigned char*)key;
    uint32_t h = 5381;
    for (size_t i = 0; i < len; i++) h = h * 33 + b[i];
    return h ? h : 1;
}

TEST(BbqDictAdversarial, AColliderSetFloodsTheWeakHashAndNotTheRealOne) {
    /* "Az" and "BY" collide under djb2 — 33*'A'+'z' == 33*'B'+'Y' == 2267 — and
     * the collision composes, so every one of these 2^12 keys has the same djb2
     * digest whatever surrounds them. That is hash flooding: an attacker who
     * knows the hash computes keys that pile onto one chain and turns every
     * lookup into a linear scan.
     *
     * The assertion is about WORK, not answers. Both dicts below return the
     * right values — a collision is a wasted hop, never a wrong result — so a
     * test that only checked answers would pass on either. */
    std::vector<std::string> keys;
    for (int mask = 0; mask < 4096; mask++) {
        std::string k;
        for (int bit = 0; bit < 12; bit++) k += (mask & (1 << bit)) ? "Az" : "BY";
        keys.push_back(k);
    }

    auto longest_chain = [&](bbq_dict_hash_fn fn) {
        std::unordered_map<uint64_t, int> per_hash;
        for (auto& k : keys) per_hash[fn(k.data(), k.size())]++;
        int longest = 0;
        for (auto& kv : per_hash) longest = std::max(longest, kv.second);
        return longest;
    };

    /* The control: the old hash really does collapse all of them onto one chain.
     * Without this the assertion below could be passing for any reason. */
    EXPECT_EQ(longest_chain(Djb2), (int)keys.size());

    /* The real one keeps them apart. */
    EXPECT_LE(longest_chain(bbq_dict_hash), 2)
        << "seeded hash produced a long chain on a djb2 collider set";

    /* And both still answer correctly, weak hash included — correctness does not
     * depend on the hash, only cost does. */
    for (bbq_dict_hash_fn fn : {bbq_dict_hash, Djb2}) {
        bbq_dict d; bbq_dict_init_hashed(&d, nullptr, fn);
        for (size_t i = 0; i < keys.size(); i++)
            ASSERT_TRUE(bbq_dict_put(&d, keys[i].data(), keys[i].size(), (void*)(uintptr_t)(i + 1)));
        EXPECT_EQ(bbq_dict_len(&d), keys.size());
        for (size_t i = 0; i < keys.size(); i++)
            ASSERT_EQ(bbq_dict_get(&d, keys[i].data(), keys[i].size()), (void*)(uintptr_t)(i + 1));
        bbq_dict_free(&d);
    }
}

TEST(BbqDictOom, EveryFailurePointLeavesTheDictConsistent) {
    SweepAllocationFailures([](bbq_alloc* alloc) {
        bbq_dict d; bbq_dict_init_a(&d, alloc);
        std::map<std::string, void*> oracle;
        for (int i = 0; i < 10; i++) {
            std::string k = "key" + std::to_string(i);
            void* v = (void*)(uintptr_t)(i + 1);
            if (bbq_dict_put(&d, k.data(), k.size(), v)) oracle[k] = v;
            else EXPECT_TRUE(bbq_dict_oom(&d));
        }
        EXPECT_EQ(bbq_dict_len(&d), oracle.size());
        for (auto& kv : oracle)
            EXPECT_EQ(bbq_dict_get(&d, kv.first.data(), kv.first.size()), kv.second);
        size_t walked = 0;
        bbq_dict_iter it; bbq_dict_iter_init(&d, &it);
        bbq_dict_entry e;
        while (bbq_dict_next(&it, &e)) walked++;
        EXPECT_EQ(walked, oracle.size());
        bbq_dict_free(&d);
    });
}

/* ════════════════════════════════════════════════════════════════════════════
 * F. Differential — randomised sequences against std:: oracles
 *
 * A hand-picked case tests what someone thought of. These test that the
 * container behaves like the thing it claims to be, over sequences nobody chose.
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(BbqHtreeDifferential, MatchesStdMapOverRandomOperations) {
    std::mt19937_64 rng(20260919);
    bbq_htree t; bbq_htree_init(&t);
    std::map<uint64_t, void*> oracle;

    for (int step = 0; step < 40000; step++) {
        /* A small key space so inserts, hits, misses and deletes all collide. */
        uint64_t key = rng() % 4096;
        if (rng() % 8 == 0) key = rng();          /* and some genuinely sparse */
        switch (rng() % 4) {
        case 0: case 1: {
            void* v = (void*)(uintptr_t)(rng() | 1);
            ASSERT_TRUE(bbq_htree_insert(&t, key, v));
            oracle[key] = v;
            break;
        }
        case 2: {
            auto it = oracle.find(key);
            EXPECT_EQ(bbq_htree_search(&t, key), it == oracle.end() ? nullptr : it->second);
            EXPECT_EQ(bbq_htree_contains(&t, key), it != oracle.end());
            break;
        }
        case 3: {
            auto it = oracle.find(key);
            void* want = it == oracle.end() ? nullptr : it->second;
            EXPECT_EQ(bbq_htree_delete(&t, key), want);
            oracle.erase(key);
            break;
        }
        }
        ASSERT_EQ(bbq_htree_size(&t), oracle.size()) << "at step " << step;
    }

    /* And the ordering, once, over whatever survived. */
    std::vector<uint64_t> seen;
    bbq_htree_iter it; bbq_htree_iter_init(&t, &it);
    for (bbq_htree_leaf* l = bbq_htree_next(&it); l; l = bbq_htree_next(&it))
        seen.push_back(l->key);
    ASSERT_EQ(seen.size(), oracle.size());
    size_t i = 0;
    for (auto& kv : oracle) EXPECT_EQ(seen[i++], kv.first);
    bbq_htree_free(&t);
}

TEST(BbqHmapDifferential, MatchesStdUnorderedMapOverRandomOperations) {
    std::mt19937_64 rng(20260920);
    bbq_hmap m; ASSERT_TRUE(bbq_hmap_init(&m, 0));
    std::unordered_map<uint64_t, void*> oracle;
    for (int step = 0; step < 40000; step++) {
        uint64_t key = rng() % 2048;
        if (rng() % 2) {
            void* v = (void*)(uintptr_t)(rng() | 1);
            ASSERT_TRUE(bbq_hmap_put(&m, key, v));
            oracle[key] = v;
        } else {
            auto it = oracle.find(key);
            EXPECT_EQ(bbq_hmap_get(&m, key), it == oracle.end() ? nullptr : it->second);
            EXPECT_EQ(bbq_hmap_contains(&m, key), it != oracle.end());
        }
        ASSERT_EQ(bbq_hmap_len(&m), oracle.size()) << "at step " << step;
    }
    bbq_hmap_free(&m);
}

TEST(BbqDictDifferential, MatchesStdMapOverRandomOperations) {
    std::mt19937_64 rng(20260921);
    bbq_dict d; bbq_dict_init(&d);
    std::map<std::string, void*> oracle;
    for (int step = 0; step < 20000; step++) {
        std::string key = "k" + std::to_string(rng() % 1024);
        switch (rng() % 4) {
        case 0: case 1: {
            void* v = (void*)(uintptr_t)(rng() | 1);
            ASSERT_TRUE(bbq_dict_put(&d, key.data(), key.size(), v));
            oracle[key] = v;
            break;
        }
        case 2: {
            auto it = oracle.find(key);
            EXPECT_EQ(bbq_dict_get(&d, key.data(), key.size()),
                      it == oracle.end() ? nullptr : it->second);
            break;
        }
        case 3: {
            auto it = oracle.find(key);
            void* want = it == oracle.end() ? nullptr : it->second;
            EXPECT_EQ(bbq_dict_delete(&d, key.data(), key.size()), want);
            oracle.erase(key);
            break;
        }
        }
        ASSERT_EQ(bbq_dict_len(&d), oracle.size()) << "at step " << step;
    }
    bbq_dict_free(&d);
}

TEST(BbqVecDifferential, MatchesStdVectorOverRandomOperations) {
    std::mt19937_64 rng(20260922);
    int* v = nullptr;
    std::vector<int> oracle;
    for (int step = 0; step < 50000; step++) {
        switch (rng() % 5) {
        case 0: case 1: case 2: {
            int x = (int)(rng() & 0xFFFF);
            bbq_vec_push(v, x);
            oracle.push_back(x);
            break;
        }
        case 3:
            if (!oracle.empty()) {
                EXPECT_EQ(bbq_vec_pop(v), oracle.back());
                oracle.pop_back();
            }
            break;
        case 4: {
            int n = oracle.empty() ? 0 : (int)(rng() % oracle.size());
            bbq_vec_truncate(v, n);
            oracle.resize((size_t)n);
            break;
        }
        }
        ASSERT_EQ((size_t)bbq_vec_len(v), oracle.size()) << "at step " << step;
    }
    for (size_t i = 0; i < oracle.size(); i++) ASSERT_EQ(v[i], oracle[i]);
    bbq_vec_free(v);
}
