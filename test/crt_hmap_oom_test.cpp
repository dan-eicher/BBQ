// Allocation-failure tests for bbq_hmap.
//
// These live in their own binary because they are linked with `-Wl,--wrap=calloc`, which
// would otherwise apply to every other test in the suite. The wrapper is inert unless a
// test arms it, and every test disarms it before asserting — a live wrapper during a
// failing EXPECT would starve gtest's own reporting allocations.
//
// What is under test is one invariant: the probe loop in hmap_slot terminates only
// because the table is never allowed to fill. Growth is the only thing that enforces
// that, so a growth that fails MUST refuse the put. The pre-fix code returned from a
// failed grow and inserted anyway, which walked the table to 100% full and hung the next
// lookup in an infinite probe.

#include <gtest/gtest.h>

#include <cstdint>

extern "C" {
#include "bbq_hmap.h"
}

// ── calloc interposition ───────────────────────────────────

static bool g_calloc_fails = false;

extern "C" void* __real_calloc(size_t n, size_t size);

extern "C" void* __wrap_calloc(size_t n, size_t size) {
    if (g_calloc_fails) return nullptr;
    return __real_calloc(n, size);
}

// The table grows when (len + 1) * 4 >= cap * 3. At cap 16 that is len >= 11, so 11
// entries sit one put short of forcing a grow.
static constexpr uint64_t kCap = 16;
static constexpr uint64_t kFillToBrink = 11;

static void fill_to_brink(bbq_hmap* m) {
    ASSERT_TRUE(bbq_hmap_init(m, kCap));
    for (uint64_t i = 1; i <= kFillToBrink; i++)
        ASSERT_TRUE(bbq_hmap_put(m, i, (void*)(uintptr_t)i));
    ASSERT_EQ(bbq_hmap_len(m), kFillToBrink);
}

// A put that needs to grow, and cannot, must store NOTHING and say so. The entry is
// absent rather than silently dropped: `contains` must agree with the false return.
TEST(BbqHmapOom, FailedGrowRefusesThePut) {
    bbq_hmap m;
    fill_to_brink(&m);

    g_calloc_fails = true;
    bool ok = bbq_hmap_put(&m, 999, (void*)(uintptr_t)999);
    g_calloc_fails = false;

    EXPECT_FALSE(ok);
    EXPECT_EQ(bbq_hmap_len(&m), kFillToBrink);
    EXPECT_FALSE(bbq_hmap_contains(&m, 999));
    bbq_hmap_free(&m);
}

// A failed grow must leave the existing table untouched — same entries, same capacity.
// hmap_grow allocates into locals and only publishes on success, so a failure cannot
// half-migrate the map.
TEST(BbqHmapOom, FailedGrowLeavesExistingEntriesIntact) {
    bbq_hmap m;
    fill_to_brink(&m);

    g_calloc_fails = true;
    (void)bbq_hmap_put(&m, 999, (void*)(uintptr_t)999);
    g_calloc_fails = false;

    for (uint64_t i = 1; i <= kFillToBrink; i++)
        EXPECT_EQ(bbq_hmap_get(&m, i), (void*)(uintptr_t)i) << "lost key " << i;
    bbq_hmap_free(&m);
}

// THE regression test. Under sustained allocation failure, thousands of puts must all be
// refused and the table must stay at its pre-failure occupancy. The pre-fix code inserted
// each one, and the run ended in an unbounded probe loop rather than a failed assertion —
// so a hang here IS the failure signal, which is why this test carries a ctest timeout.
TEST(BbqHmapOom, SustainedAllocFailureNeverFillsTheTable) {
    bbq_hmap m;
    fill_to_brink(&m);

    g_calloc_fails = true;
    int accepted = 0;
    for (uint64_t i = 0; i < 10000; i++)
        if (bbq_hmap_put(&m, 0x100000 + i, (void*)(uintptr_t)i)) accepted++;
    size_t len_under_failure = bbq_hmap_len(&m);
    g_calloc_fails = false;

    EXPECT_EQ(accepted, 0) << "a put was accepted while every allocation was failing";
    EXPECT_EQ(len_under_failure, kFillToBrink);
    bbq_hmap_free(&m);
}

// Refusal is not a latch: once allocation works again the very next put succeeds and the
// map grows normally. A map that stayed poisoned after one transient failure would be
// just as wrong as one that corrupted itself.
TEST(BbqHmapOom, RecoversOnceAllocationSucceedsAgain) {
    bbq_hmap m;
    fill_to_brink(&m);

    g_calloc_fails = true;
    (void)bbq_hmap_put(&m, 999, (void*)(uintptr_t)999);
    g_calloc_fails = false;

    ASSERT_TRUE(bbq_hmap_put(&m, 999, (void*)(uintptr_t)999));
    EXPECT_EQ(bbq_hmap_get(&m, 999), (void*)(uintptr_t)999);
    EXPECT_EQ(bbq_hmap_len(&m), kFillToBrink + 1);
    for (uint64_t i = 1; i <= kFillToBrink; i++)
        EXPECT_EQ(bbq_hmap_get(&m, i), (void*)(uintptr_t)i);
    bbq_hmap_free(&m);
}

// A failed init reports failure and leaves an empty-but-valid map: reads answer, and the
// first put re-attempts the allocation instead of indexing a NULL array through a
// cap-1 == SIZE_MAX mask.
TEST(BbqHmapOom, FailedInitLeavesAUsableMap) {
    bbq_hmap m;
    g_calloc_fails = true;
    bool ok = bbq_hmap_init(&m, 0);
    g_calloc_fails = false;

    EXPECT_FALSE(ok);
    EXPECT_EQ(bbq_hmap_len(&m), 0u);
    EXPECT_EQ(bbq_hmap_get(&m, 5), nullptr);
    EXPECT_FALSE(bbq_hmap_contains(&m, 5));

    ASSERT_TRUE(bbq_hmap_put(&m, 5, (void*)(uintptr_t)5));
    EXPECT_EQ(bbq_hmap_get(&m, 5), (void*)(uintptr_t)5);
    bbq_hmap_free(&m);
}

// A put on a freed map re-allocates rather than dereferencing the NULLed arrays.
TEST(BbqHmapOom, PutAfterFreeReallocates) {
    bbq_hmap m;
    ASSERT_TRUE(bbq_hmap_init(&m, 0));
    ASSERT_TRUE(bbq_hmap_put(&m, 1, (void*)(uintptr_t)1));
    bbq_hmap_free(&m);

    ASSERT_TRUE(bbq_hmap_put(&m, 2, (void*)(uintptr_t)2));
    EXPECT_EQ(bbq_hmap_get(&m, 2), (void*)(uintptr_t)2);
    EXPECT_FALSE(bbq_hmap_contains(&m, 1));
    EXPECT_EQ(bbq_hmap_len(&m), 1u);
    bbq_hmap_free(&m);
}
