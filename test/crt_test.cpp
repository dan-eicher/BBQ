// Tests for the BBQ C Runtime Library (bbq_arena, bbq_vec, bbq_buf, bbq_htree)

#include <gtest/gtest.h>

extern "C" {
#include "bbq_arena.h"
#include "bbq_vec.h"
#include "bbq_buf.h"
#include "bbq_htree.h"
}

// ── Arena tests ────────────────────────────────────────────

TEST(BbqArena, InitAndAlloc) {
    bbq_arena a;
    bbq_arena_init(&a, 4096);

    void* p1 = bbq_arena_alloc(&a, 64);
    ASSERT_NE(p1, nullptr);

    void* p2 = bbq_arena_alloc(&a, 128);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);

    bbq_arena_free(&a);
}

TEST(BbqArena, ResetReusesPages) {
    bbq_arena a;
    bbq_arena_init(&a, 256);

    void* p1 = bbq_arena_alloc(&a, 64);
    int pages_before = a.page_count;

    bbq_arena_reset(&a);

    void* p2 = bbq_arena_alloc(&a, 64);
    EXPECT_EQ(a.page_count, pages_before); // no new pages allocated
    EXPECT_EQ(p1, p2); // same address reused

    bbq_arena_free(&a);
}

TEST(BbqArena, LargeAllocation) {
    bbq_arena a;
    bbq_arena_init(&a, 64);

    // Allocate more than one page worth
    void* p = bbq_arena_alloc(&a, 256);
    ASSERT_NE(p, nullptr);

    bbq_arena_free(&a);
}

TEST(BbqArena, ManySmallAllocs) {
    bbq_arena a;
    bbq_arena_init(&a, 256);

    for (int i = 0; i < 1000; i++) {
        void* p = bbq_arena_alloc(&a, 16);
        ASSERT_NE(p, nullptr);
        // Check 8-byte alignment
        EXPECT_EQ((uintptr_t)p % 8, 0u);
    }

    bbq_arena_free(&a);
}

TEST(BbqArena, ZeroAllocReturnsNull) {
    bbq_arena a;
    bbq_arena_init(&a, 4096);
    EXPECT_EQ(bbq_arena_alloc(&a, 0), nullptr);
    bbq_arena_free(&a);
}

// ── Vec tests ──────────────────────────────────────────────

TEST(BbqVec, PushAndAccess) {
    int* v = NULL;

    for (int i = 0; i < 100; i++)
        bbq_vec_push(v, i);

    EXPECT_EQ(bbq_vec_len(v), 100);
    for (int i = 0; i < 100; i++)
        EXPECT_EQ(v[i], i);

    bbq_vec_free(v);
    EXPECT_EQ(v, nullptr);
}

TEST(BbqVec, NullIsEmpty) {
    int* v = NULL;
    EXPECT_EQ(bbq_vec_len(v), 0);
    EXPECT_EQ(bbq_vec_cap(v), 0);
    bbq_vec_free(v); // should be a no-op
}

TEST(BbqVec, PopAndLast) {
    int* v = NULL;
    bbq_vec_push(v, 10);
    bbq_vec_push(v, 20);
    bbq_vec_push(v, 30);

    EXPECT_EQ(bbq_vec_last(v), 30);
    EXPECT_EQ(bbq_vec_pop(v), 30);
    EXPECT_EQ(bbq_vec_len(v), 2);
    EXPECT_EQ(bbq_vec_last(v), 20);

    bbq_vec_free(v);
}

TEST(BbqVec, Clear) {
    int* v = NULL;
    for (int i = 0; i < 50; i++)
        bbq_vec_push(v, i);

    bbq_vec_clear(v);
    EXPECT_EQ(bbq_vec_len(v), 0);
    EXPECT_GE(bbq_vec_cap(v), 50); // capacity retained

    bbq_vec_free(v);
}

TEST(BbqVec, Reserve) {
    int* v = NULL;
    bbq_vec_reserve(v, 1000);
    EXPECT_GE(bbq_vec_cap(v), 1000);
    EXPECT_EQ(bbq_vec_len(v), 0);

    bbq_vec_push(v, 42);
    EXPECT_EQ(v[0], 42);

    bbq_vec_free(v);
}

TEST(BbqVec, Reverse) {
    int* v = NULL;
    for (int i = 0; i < 5; i++)
        bbq_vec_push(v, i);

    bbq_vec_reverse(v);
    EXPECT_EQ(v[0], 4);
    EXPECT_EQ(v[1], 3);
    EXPECT_EQ(v[2], 2);
    EXPECT_EQ(v[3], 1);
    EXPECT_EQ(v[4], 0);

    bbq_vec_free(v);
}

TEST(BbqVec, Pointers) {
    const char** v = NULL;
    bbq_vec_push(v, "hello");
    bbq_vec_push(v, "world");
    EXPECT_EQ(bbq_vec_len(v), 2);
    EXPECT_STREQ(v[0], "hello");
    EXPECT_STREQ(v[1], "world");
    bbq_vec_free(v);
}

// ── Buf tests ──────────────────────────────────────────────

TEST(BbqBuf, AppendAndRead) {
    bbq_buf b;
    bbq_buf_init(&b);

    const uint8_t data1[] = {1, 2, 3};
    const uint8_t data2[] = {4, 5};
    bbq_buf_append(&b, data1, 3);
    bbq_buf_append(&b, data2, 2);

    EXPECT_EQ(b.len, 5u);
    EXPECT_EQ(b.data[0], 1);
    EXPECT_EQ(b.data[4], 5);

    bbq_buf_free(&b);
}

TEST(BbqBuf, ClearRetainsCapacity) {
    bbq_buf b;
    bbq_buf_init(&b);

    uint8_t data[100] = {};
    bbq_buf_append(&b, data, 100);
    size_t cap = b.cap;

    bbq_buf_clear(&b);
    EXPECT_EQ(b.len, 0u);
    EXPECT_EQ(b.cap, cap);

    bbq_buf_free(&b);
}

// ── Htree tests ────────────────────────────────────────────

TEST(BbqHtree, CreateDestroy) {
    bbq_htree* t = bbq_htree_create();
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(bbq_htree_is_empty(t));
    bbq_htree_destroy(t);
}

TEST(BbqHtree, InsertAndSearch) {
    bbq_htree* t = bbq_htree_create();
    int values[] = {10, 20, 30};

    EXPECT_TRUE(bbq_htree_insert(t, 100, &values[0]));
    EXPECT_TRUE(bbq_htree_insert(t, 200, &values[1]));
    EXPECT_TRUE(bbq_htree_insert(t, 300, &values[2]));

    EXPECT_EQ(bbq_htree_search(t, 100), &values[0]);
    EXPECT_EQ(bbq_htree_search(t, 200), &values[1]);
    EXPECT_EQ(bbq_htree_search(t, 300), &values[2]);
    EXPECT_EQ(bbq_htree_search(t, 999), nullptr);

    bbq_htree_destroy(t);
}

TEST(BbqHtree, Contains) {
    bbq_htree* t = bbq_htree_create();
    int val = 42;

    EXPECT_FALSE(bbq_htree_contains(t, 1));
    bbq_htree_insert(t, 1, &val);
    EXPECT_TRUE(bbq_htree_contains(t, 1));
    EXPECT_FALSE(bbq_htree_contains(t, 2));

    bbq_htree_destroy(t);
}

TEST(BbqHtree, Delete) {
    bbq_htree* t = bbq_htree_create();
    int val = 42;

    bbq_htree_insert(t, 1, &val);
    EXPECT_TRUE(bbq_htree_contains(t, 1));

    void* deleted = bbq_htree_delete(t, 1);
    EXPECT_EQ(deleted, &val);
    EXPECT_FALSE(bbq_htree_contains(t, 1));

    EXPECT_EQ(bbq_htree_delete(t, 999), nullptr);

    bbq_htree_destroy(t);
}

TEST(BbqHtree, UpdateExisting) {
    bbq_htree* t = bbq_htree_create();
    int v1 = 1, v2 = 2;

    bbq_htree_insert(t, 42, &v1);
    EXPECT_EQ(bbq_htree_search(t, 42), &v1);

    bbq_htree_insert(t, 42, &v2);
    EXPECT_EQ(bbq_htree_search(t, 42), &v2);
    EXPECT_EQ(bbq_htree_size(t), 1);

    bbq_htree_destroy(t);
}

TEST(BbqHtree, Clear) {
    bbq_htree* t = bbq_htree_create();
    int val = 1;

    for (uint32_t i = 0; i < 100; i++)
        bbq_htree_insert(t, i, &val);

    EXPECT_EQ(bbq_htree_size(t), 100);
    bbq_htree_clear(t);
    EXPECT_TRUE(bbq_htree_is_empty(t));

    bbq_htree_destroy(t);
}

TEST(BbqHtree, ManyKeys) {
    bbq_htree* t = bbq_htree_create();
    int val = 1;

    for (uint32_t i = 0; i < 10000; i++)
        EXPECT_TRUE(bbq_htree_insert(t, i * 7 + 13, &val));

    for (uint32_t i = 0; i < 10000; i++)
        EXPECT_TRUE(bbq_htree_contains(t, i * 7 + 13));

    EXPECT_EQ(bbq_htree_size(t), 10000);

    bbq_htree_destroy(t);
}

TEST(BbqHtree, Clone) {
    bbq_htree* t = bbq_htree_create();
    int values[] = {1, 2, 3};

    bbq_htree_insert(t, 10, &values[0]);
    bbq_htree_insert(t, 20, &values[1]);
    bbq_htree_insert(t, 30, &values[2]);

    bbq_htree* t2 = bbq_htree_clone(t);
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(bbq_htree_size(t2), 3);
    EXPECT_EQ(bbq_htree_search(t2, 10), &values[0]);
    EXPECT_EQ(bbq_htree_search(t2, 20), &values[1]);
    EXPECT_EQ(bbq_htree_search(t2, 30), &values[2]);

    bbq_htree_destroy(t);
    bbq_htree_destroy(t2);
}

TEST(BbqHtree, IterationOrder) {
    bbq_htree* t = bbq_htree_create();
    int val = 1;

    // Insert in random order
    uint32_t keys[] = {500, 100, 300, 200, 400};
    for (int i = 0; i < 5; i++)
        bbq_htree_insert(t, keys[i], &val);

    // Iteration should be in ascending key order
    bbq_htree_iter it;
    bbq_htree_iter_init(t, &it);

    uint32_t prev = 0;
    bbq_htree_leaf* leaf;
    int count = 0;
    while ((leaf = bbq_htree_next(&it))) {
        EXPECT_GT(leaf->key, prev);
        prev = leaf->key;
        count++;
    }
    EXPECT_EQ(count, 5);

    bbq_htree_destroy(t);
}

// Sentinel-value pattern used for visited sets
TEST(BbqHtree, VisitedSetPattern) {
    bbq_htree* t = bbq_htree_create();

    // Use (void*)1 as sentinel — NULL would be ambiguous with "not found"
    void* const VISITED = (void*)(uintptr_t)1;

    for (uint32_t i = 0; i < 50; i++)
        bbq_htree_insert(t, i, VISITED);

    for (uint32_t i = 0; i < 50; i++)
        EXPECT_TRUE(bbq_htree_contains(t, i));

    for (uint32_t i = 50; i < 100; i++)
        EXPECT_FALSE(bbq_htree_contains(t, i));

    bbq_htree_destroy(t);
}
