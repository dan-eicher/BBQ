// Tests for the BBQ C Runtime Library (bbq_arena, bbq_vec, bbq_buf, bbq_htree, bbq_hmap)

#include <gtest/gtest.h>

extern "C" {
#include "bbq_arena.h"
#include "bbq_vec.h"
#include "bbq_buf.h"
#include "bbq_htree.h"
#include "bbq_hmap.h"
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

// An allocation LARGER than page_size, made after a reset, must not be served
// out of a recycled normal-size page. The arena records no per-page capacity,
// so the reuse path had nothing to check the request against: it set pos = 0 on
// a 4096-byte page and returned it for an 8192-byte request, and the caller's
// write ran off the end of the heap block.
//
// Asserted by address rather than by writing, so this fails deterministically
// on a plain build instead of only under a sanitizer.
TEST(BbqArena, ResetThenOversizedAllocDoesNotReuseANormalPage) {
    bbq_arena a;
    bbq_arena_init(&a, 4096);

    for (int i = 0; i < 5; i++)               // fills page 0, opens page 1
        ASSERT_NE(bbq_arena_alloc(&a, 1024), nullptr);
    ASSERT_GE(a.page_count, 2);
    char* page0 = a.pages[0];
    char* page1 = a.pages[1];

    bbq_arena_reset(&a);
    ASSERT_NE(bbq_arena_alloc(&a, 64), nullptr);          // back on page 0

    char* big = (char*)bbq_arena_alloc(&a, 8192);         // twice page_size
    ASSERT_NE(big, nullptr);
    EXPECT_FALSE(big >= page0 && big < page0 + 4096)
        << "8192-byte block was served from the recycled 4096-byte page 0";
    EXPECT_FALSE(big >= page1 && big < page1 + 4096)
        << "8192-byte block was served from the recycled 4096-byte page 1";

    memset(big, 0xAB, 8192);                  // the write the bug corrupted
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

// ── bbq_hmap ───────────────────────────────────────────────
//
// The flat, open-addressed map for DENSE 64-bit / POINTER keys. bbq_htree is a nibble
// TRIE: right for sparse integer keys nobody controls (a packed (class, field) cell key),
// but a lookup is up to 8 DEPENDENT pointer loads, which is the wrong shape for "what
// index did I give this node?" — a question asked millions of times per compile against
// keys we minted ourselves. This is one array probe instead.

TEST(BbqHmap, GetOnEmptyIsNull) {
    bbq_hmap m;
    ASSERT_TRUE(bbq_hmap_init(&m, 0));
    EXPECT_EQ(bbq_hmap_get(&m, 0x1234), nullptr);
    bbq_hmap_free(&m);
}

TEST(BbqHmap, PutThenGet) {
    bbq_hmap m;
    ASSERT_TRUE(bbq_hmap_init(&m, 0));
    ASSERT_TRUE(bbq_hmap_put(&m, 0x1000, (void*)(uintptr_t)1));
    ASSERT_TRUE(bbq_hmap_put(&m, 0x2000, (void*)(uintptr_t)2));
    EXPECT_EQ(bbq_hmap_get(&m, 0x1000), (void*)(uintptr_t)1);
    EXPECT_EQ(bbq_hmap_get(&m, 0x2000), (void*)(uintptr_t)2);
    EXPECT_EQ(bbq_hmap_get(&m, 0x3000), nullptr);
    bbq_hmap_free(&m);
}

// A re-put REPLACES; it must not insert a second entry for the same key.
TEST(BbqHmap, PutReplaces) {
    bbq_hmap m;
    ASSERT_TRUE(bbq_hmap_init(&m, 0));
    ASSERT_TRUE(bbq_hmap_put(&m, 42, (void*)(uintptr_t)1));
    ASSERT_TRUE(bbq_hmap_put(&m, 42, (void*)(uintptr_t)9));
    EXPECT_EQ(bbq_hmap_get(&m, 42), (void*)(uintptr_t)9);
    EXPECT_EQ(bbq_hmap_len(&m), 1u);
    bbq_hmap_free(&m);
}

// Growth: every key inserted must still be findable after the table rehashes. This is the
// one that catches a broken probe/rehash — the failure mode is a key that silently
// vanishes, which in the optimizer would be a node with no vnode.
TEST(BbqHmap, GrowsAndKeepsEveryKey) {
    bbq_hmap m;
    ASSERT_TRUE(bbq_hmap_init(&m, 4));            // deliberately tiny: forces several rehashes
    const uint64_t N = 5000;
    for (uint64_t i = 1; i <= N; i++)
        ASSERT_TRUE(bbq_hmap_put(&m, i * 8, (void*)(uintptr_t)i));   // 8-aligned, like real pointers
    EXPECT_EQ(bbq_hmap_len(&m), N);
    for (uint64_t i = 1; i <= N; i++)
        EXPECT_EQ(bbq_hmap_get(&m, i * 8), (void*)(uintptr_t)i) << "lost key " << i;
    for (uint64_t i = 1; i <= N; i++)
        EXPECT_EQ(bbq_hmap_get(&m, i * 8 + 4), nullptr);   // absent keys stay absent
    bbq_hmap_free(&m);
}

// A NULL value is a legitimate value, and must be distinguishable from "absent".
TEST(BbqHmap, NullValueIsNotAbsent) {
    bbq_hmap m;
    ASSERT_TRUE(bbq_hmap_init(&m, 0));
    ASSERT_TRUE(bbq_hmap_put(&m, 7, nullptr));
    EXPECT_TRUE(bbq_hmap_contains(&m, 7));
    EXPECT_EQ(bbq_hmap_get(&m, 7), nullptr);
    EXPECT_FALSE(bbq_hmap_contains(&m, 8));
    bbq_hmap_free(&m);
}

// Key 0 must be storable: the empty slot cannot be signalled by a zero KEY, or a real
// key of 0 reads as absent. (Pointers are never 0 in the optimizer's use, but a map that
// quietly loses one key value is a trap for the next caller.)
TEST(BbqHmap, ZeroKeyIsAValidKey) {
    bbq_hmap m;
    ASSERT_TRUE(bbq_hmap_init(&m, 0));
    ASSERT_TRUE(bbq_hmap_put(&m, 0, (void*)(uintptr_t)77));
    EXPECT_TRUE(bbq_hmap_contains(&m, 0));
    EXPECT_EQ(bbq_hmap_get(&m, 0), (void*)(uintptr_t)77);
    bbq_hmap_free(&m);
}

// Clustered keys — real pointers from an arena are 8-aligned and near each other, so the
// low bits are a terrible hash. The map must not degenerate into a linear scan.
TEST(BbqHmap, ClusteredAlignedKeys) {
    bbq_hmap m;
    ASSERT_TRUE(bbq_hmap_init(&m, 0));
    uint64_t base = 0x7f0000001000ull;
    for (uint64_t i = 0; i < 2000; i++)
        ASSERT_TRUE(bbq_hmap_put(&m, base + i * 64, (void*)(uintptr_t)(i + 1)));
    for (uint64_t i = 0; i < 2000; i++)
        EXPECT_EQ(bbq_hmap_get(&m, base + i * 64), (void*)(uintptr_t)(i + 1));
    bbq_hmap_free(&m);
}
