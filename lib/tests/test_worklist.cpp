#include <gtest/gtest.h>
#include "Worklist.h"

#include <string>
#include <unordered_set>

TEST(Worklist, EmptyOnConstruction) {
    bbq::Worklist<int> wl;
    EXPECT_TRUE(wl.empty());
    EXPECT_EQ(wl.size(), 0u);
}

TEST(Worklist, PushReturnsTrueForNew) {
    bbq::Worklist<int> wl;
    EXPECT_TRUE(wl.push(1));
    EXPECT_TRUE(wl.push(2));
    EXPECT_EQ(wl.size(), 2u);
}

TEST(Worklist, PushReturnsFalseForDuplicate) {
    bbq::Worklist<int> wl;
    EXPECT_TRUE(wl.push(1));
    EXPECT_FALSE(wl.push(1));
    EXPECT_EQ(wl.size(), 1u);
}

TEST(Worklist, PopRemovesFromOnQueueSet) {
    bbq::Worklist<int> wl;
    wl.push(1);
    wl.push(2);
    EXPECT_EQ(wl.pop(), 1);
    // After popping, 1 can be re-pushed and counts as new.
    EXPECT_TRUE(wl.push(1));
}

TEST(Worklist, FIFOOrder) {
    bbq::Worklist<int> wl;
    wl.push(1); wl.push(2); wl.push(3);
    EXPECT_EQ(wl.pop(), 1);
    EXPECT_EQ(wl.pop(), 2);
    EXPECT_EQ(wl.pop(), 3);
}

TEST(Worklist, EmptyAfterAllPopped) {
    bbq::Worklist<int> wl;
    wl.push(1); wl.push(2);
    wl.pop(); wl.pop();
    EXPECT_TRUE(wl.empty());
}

TEST(Worklist, WorksWithStringItems) {
    bbq::Worklist<std::string> wl;
    EXPECT_TRUE(wl.push("foo"));
    EXPECT_FALSE(wl.push("foo"));
    EXPECT_TRUE(wl.push("bar"));
    EXPECT_EQ(wl.pop(), "foo");
    EXPECT_EQ(wl.pop(), "bar");
}

// Discover-as-you-go pattern with caller-side interning. The Worklist
// only dedups items currently on the queue — for "have I ever seen
// this" semantics the caller maintains its own set (which is how the
// burgc tree-automaton analysis uses the primitive: states are interned
// in a separate map, and the Worklist just batches what's pending).
TEST(Worklist, DiscoverAsYouGoPattern) {
    bbq::Worklist<int> wl;
    std::unordered_set<int> seen;
    auto enqueue_new = [&](int x) {
        if (seen.insert(x).second) wl.push(x);
    };

    enqueue_new(1);
    int processed = 0;
    while (!wl.empty()) {
        int x = wl.pop();
        ++processed;
        // Each item discovers two successors; caller's `seen` set
        // ensures we never re-discover the same one.
        if (x < 5) {
            enqueue_new(x * 2);
            enqueue_new(x * 2 + 1);
        }
    }
    // Discovery tree: 1 → 2,3 → 4,5,6,7 → 8,9 (only 4 expands; 5,6,7
    // don't because 5 is not <5). Total processed: 1,2,3,4,5,6,7,8,9 = 9.
    EXPECT_EQ(processed, 9);
}
