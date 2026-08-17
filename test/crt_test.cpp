// Tests for the BBQ C Runtime Library (bbq_arena, bbq_vec, bbq_buf, bbq_htree, bbq_hmap)

#include <gtest/gtest.h>

extern "C" {
#include "bbq_arena.h"
#include "bbq_vec.h"
#include "bbq_buf.h"
#include "bbq_htree.h"
#include "bbq_hmap.h"
#include "bbq_dict.h"
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

// ── Dict tests ─────────────────────────────────────────────
//
// The reason this container exists is the collision case, so that is the first
// test rather than an afterthought. djb2-33 collides structurally on short keys
// — any pair whose character deltas satisfy d1*33 + d2 == 0 — so these fixtures
// are constructed, not lucky, and the test cannot silently stop exercising the
// path it is here for.

TEST(BbqDict, CollidingKeysAreDistinctEntries) {
    // 'c'-'a' == 2, 2*33 == 66, 'r'-'0' == 66  =>  same digest, different keys.
    ASSERT_EQ(bbq_dict_hash("ar", 2), bbq_dict_hash("c0", 2));

    bbq_dict* d = bbq_dict_create();
    ASSERT_NE(d, nullptr);
    ASSERT_TRUE(bbq_dict_puts(d, "ar", (void*)(uintptr_t)1));
    ASSERT_TRUE(bbq_dict_puts(d, "c0", (void*)(uintptr_t)2));

    EXPECT_EQ(bbq_dict_gets(d, "ar"), (void*)(uintptr_t)1);
    EXPECT_EQ(bbq_dict_gets(d, "c0"), (void*)(uintptr_t)2);
    EXPECT_EQ(bbq_dict_len(d), 2u);
    bbq_dict_destroy(d);
}

// A key that collides with a stored one but was never inserted is a MISS. This
// is the half that a hash-and-trust table gets wrong in the other direction:
// it would answer with the resident key's value.
TEST(BbqDict, CollidingAbsentKeyIsAMiss) {
    bbq_dict* d = bbq_dict_create();
    ASSERT_TRUE(bbq_dict_puts(d, "ar", (void*)(uintptr_t)1));
    EXPECT_EQ(bbq_dict_gets(d, "c0"), nullptr);
    EXPECT_FALSE(bbq_dict_contains(d, "c0", 2));
    bbq_dict_destroy(d);
}

TEST(BbqDict, DeleteUnlinksFromTheMiddleOfAChain) {
    // Three keys on one digest: 33x + y is constant along the collision line.
    const char* k[3] = { "ar", "c0", "bQ" };
    ASSERT_EQ(bbq_dict_hash(k[0], 2), bbq_dict_hash(k[1], 2));
    ASSERT_EQ(bbq_dict_hash(k[0], 2), bbq_dict_hash(k[2], 2));

    bbq_dict* d = bbq_dict_create();
    for (int i = 0; i < 3; i++) ASSERT_TRUE(bbq_dict_puts(d, k[i], (void*)(uintptr_t)(i + 1)));
    EXPECT_EQ(bbq_dict_delete(d, k[1], 2), (void*)(uintptr_t)2);
    EXPECT_EQ(bbq_dict_gets(d, k[0]), (void*)(uintptr_t)1);
    EXPECT_EQ(bbq_dict_gets(d, k[1]), nullptr);
    EXPECT_EQ(bbq_dict_gets(d, k[2]), (void*)(uintptr_t)3);
    EXPECT_EQ(bbq_dict_len(d), 2u);
    // ...and emptying the chain removes the tree entry without stranding it.
    EXPECT_EQ(bbq_dict_delete(d, k[0], 2), (void*)(uintptr_t)1);
    EXPECT_EQ(bbq_dict_delete(d, k[2], 2), (void*)(uintptr_t)3);
    EXPECT_EQ(bbq_dict_len(d), 0u);
    EXPECT_EQ(bbq_dict_gets(d, k[0]), nullptr);
    bbq_dict_destroy(d);
}

// Keys are bytes, not C strings: pooled UTF-16LE runs carry a 0x00 every other
// byte, which is why bytes_hash had to exist beside str_hash in the first place.
TEST(BbqDict, KeysWithEmbeddedNulAreDistinct) {
    bbq_dict* d = bbq_dict_create();
    const unsigned char a[4] = { 'A', 0, 'B', 0 };
    const unsigned char b[4] = { 'A', 0, 'C', 0 };
    ASSERT_TRUE(bbq_dict_put(d, a, 4, (void*)(uintptr_t)1));
    ASSERT_TRUE(bbq_dict_put(d, b, 4, (void*)(uintptr_t)2));
    EXPECT_EQ(bbq_dict_get(d, a, 4), (void*)(uintptr_t)1);
    EXPECT_EQ(bbq_dict_get(d, b, 4), (void*)(uintptr_t)2);
    // A prefix is not the key.
    EXPECT_EQ(bbq_dict_get(d, a, 2), nullptr);
    bbq_dict_destroy(d);
}

// The dict owns its keys, so a caller may key on a buffer it then overwrites.
TEST(BbqDict, KeyIsCopiedNotBorrowed) {
    bbq_dict* d = bbq_dict_create();
    char scratch[8];
    memcpy(scratch, "volatile", 8);
    ASSERT_TRUE(bbq_dict_put(d, scratch, 8, (void*)(uintptr_t)42));
    memset(scratch, 'X', sizeof scratch);
    EXPECT_EQ(bbq_dict_get(d, "volatile", 8), (void*)(uintptr_t)42);
    bbq_dict_destroy(d);
}

TEST(BbqDict, PutOverwritesAndDoesNotGrow) {
    bbq_dict* d = bbq_dict_create();
    ASSERT_TRUE(bbq_dict_puts(d, "k", (void*)(uintptr_t)1));
    ASSERT_TRUE(bbq_dict_puts(d, "k", (void*)(uintptr_t)2));
    EXPECT_EQ(bbq_dict_gets(d, "k"), (void*)(uintptr_t)2);
    EXPECT_EQ(bbq_dict_len(d), 1u);
    bbq_dict_destroy(d);
}

TEST(BbqDict, EmptyKeyIsAKey) {
    bbq_dict* d = bbq_dict_create();
    ASSERT_TRUE(bbq_dict_put(d, "", 0, nullptr));
    EXPECT_TRUE(bbq_dict_contains(d, "", 0));
    EXPECT_FALSE(bbq_dict_contains(d, "x", 1));
    EXPECT_EQ(bbq_dict_len(d), 1u);
    bbq_dict_destroy(d);
}

TEST(BbqDict, ManyKeysNoCrossTalk) {
    bbq_dict* d = bbq_dict_create();
    char buf[32];
    for (int i = 0; i < 5000; i++) {
        int n = snprintf(buf, sizeof buf, "sym_%d_%d", i, i * 7);
        ASSERT_TRUE(bbq_dict_put(d, buf, (size_t)n, (void*)(uintptr_t)(i + 1)));
    }
    EXPECT_EQ(bbq_dict_len(d), 5000u);
    for (int i = 0; i < 5000; i++) {
        int n = snprintf(buf, sizeof buf, "sym_%d_%d", i, i * 7);
        EXPECT_EQ(bbq_dict_get(d, buf, (size_t)n), (void*)(uintptr_t)(i + 1));
    }
    EXPECT_EQ(bbq_dict_gets(d, "sym_5000_35000"), nullptr);
    bbq_dict_destroy(d);
}

TEST(BbqDict, IterationVisitsEveryEntryOnce) {
    bbq_dict* d = bbq_dict_create();
    ASSERT_TRUE(bbq_dict_puts(d, "ar", (void*)(uintptr_t)1));   // colliding pair,
    ASSERT_TRUE(bbq_dict_puts(d, "c0", (void*)(uintptr_t)2));   // so a chain is walked
    ASSERT_TRUE(bbq_dict_puts(d, "solo", (void*)(uintptr_t)3));

    bbq_dict_iter it; bbq_dict_entry e;
    uintptr_t sum = 0; int n = 0;
    bbq_dict_iter_init(d, &it);
    while (bbq_dict_next(&it, &e)) { sum += (uintptr_t)e.value; n++; }
    EXPECT_EQ(n, 3);
    EXPECT_EQ(sum, 6u);
    bbq_dict_destroy(d);
}
