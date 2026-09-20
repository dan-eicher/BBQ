/*
 * crt_vec_alloc_test.cpp — BBQ_VEC_ALLOC, which is the embedder's only way to
 * put a ceiling on a vector.
 *
 * It needs a translation unit of its own. The hook is a macro read where the
 * vector is PUSHED, so a file that defines it changes every vector it creates —
 * which is the point, and which is why it cannot share crt_test.cpp with the
 * tests that want the libc default.
 *
 * The bug this pins: bbq_vec_push grew through an out-of-line helper that
 * resolved BBQ_VEC_ALLOC() in bbq_vec.c, so it always read the DEFAULT. Every
 * vector born through a push — nearly all of them — silently ignored the
 * embedder's allocator, while bbq_vec_reserve honoured it. A budget handed to
 * such a vector bounded nothing, and the header said otherwise.
 */
#include "bbq_alloc.h"

static bbq_budget g_budget;
static bbq_alloc* test_vec_alloc(void) { return bbq_budget_handle(&g_budget); }
#define BBQ_VEC_ALLOC() test_vec_alloc()

#include "bbq_vec.h"

#include <gtest/gtest.h>

namespace {

struct VecAlloc : public ::testing::Test {
    void SetUp() override { bbq_budget_init(&g_budget, SIZE_MAX, nullptr); }
};

}  // namespace

TEST_F(VecAlloc, PushIsChargedToTheEmbeddersAllocator) {
    int* v = nullptr;
    bbq_vec_push(v, 1);
    ASSERT_EQ(bbq_vec_len(v), 1);
    EXPECT_GT(g_budget.used, 0u) << "the push did not go through BBQ_VEC_ALLOC()";
    bbq_vec_free(v);
    EXPECT_EQ(g_budget.used, 0u) << "freed through an allocator other than the one that allocated";
}

TEST_F(VecAlloc, AReserveAndAPushShareTheSameAllocator) {
    /* The two growth paths must agree, or a vector reserved in one file and
     * pushed in another is freed through whichever one lost the race. */
    int* a = nullptr;
    bbq_vec_reserve(a, 4);
    size_t after_reserve = g_budget.used;
    ASSERT_GT(after_reserve, 0u);
    for (int i = 0; i < 64; i++) bbq_vec_push(a, i);   /* forces several regrowths */
    ASSERT_EQ(bbq_vec_len(a), 64);
    EXPECT_FALSE(bbq_vec_oom(a));
    bbq_vec_free(a);
    EXPECT_EQ(g_budget.used, 0u);
}

TEST_F(VecAlloc, ACeilingStopsAPushLoopInsteadOfTheProcess) {
    bbq_budget_init(&g_budget, 256, nullptr);
    int* v = nullptr;
    for (int i = 0; i < 100000; i++) bbq_vec_push(v, i);
    EXPECT_TRUE(bbq_vec_oom(v)) << "a bounded vector must poison, not keep growing";
    EXPECT_LE((size_t)bbq_vec_len(v), 256u / sizeof(int));
    EXPECT_GT(g_budget.denials, 0u);
    /* Everything it did take is still readable and still correct. */
    for (int i = 0; i < bbq_vec_len(v); i++) EXPECT_EQ(v[i], i);
    bbq_vec_free(v);
    EXPECT_EQ(g_budget.used, 0u);
}

TEST_F(VecAlloc, EveryCeilingGivesBackExactlyWhatItTook) {
    size_t total_peak = 0, total_denials = 0;
    for (size_t ceiling = 0; ceiling <= 2048; ceiling += 8) {
        bbq_budget_init(&g_budget, ceiling, nullptr);
        int* v = nullptr;
        for (int i = 0; i < 500; i++) bbq_vec_push(v, i);
        bbq_vec_free(v);
        ASSERT_EQ(g_budget.used, 0u) << "ceiling " << ceiling;
        total_peak    += g_budget.peak;
        total_denials += g_budget.denials;
    }
    /* Without these the whole sweep passes vacuously the moment the hook stops
     * reaching the push path: an allocator nothing allocates through balances at
     * zero for free. */
    EXPECT_GT(total_peak, 0u)    << "nothing was ever allocated through the hook";
    EXPECT_GT(total_denials, 0u) << "no ceiling ever refused — the sweep proves nothing";
}
