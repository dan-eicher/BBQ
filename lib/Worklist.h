// Discover-as-you-go worklist primitive.
//
// A small dedup-aware FIFO queue, intended for monotone fixed-point
// analyses where items are discovered as the worklist runs (tree
// automaton determinization, FIRST/FOLLOW set computation, ambiguity
// detection). The caller owns the lattice, the transfer function,
// and the interning — this primitive only handles "what's pending
// and haven't I already enqueued this".
//
// Dedup is "currently on queue", not "have ever seen": once a value
// is popped, push() of the same value will re-enqueue it (matches
// Kildall-style dataflow where re-processing is meaningful).
// Consumers wanting see-once semantics layer their own seen set on
// top.
//
// Items must be hashable and equality-comparable. For dense small-int
// IDs where O(1) array indexing matters more than generality, prefer
// an open-coded vector<bool> + deque<int> instead — that's a
// different perf profile this primitive doesn't try to serve.

#pragma once

#include <deque>
#include <unordered_set>

namespace bbq {

template <typename T>
class Worklist {
    std::deque<T> queue_;
    std::unordered_set<T> on_queue_;
public:
    // Returns true if newly enqueued; false if already pending.
    bool push(const T& item) {
        if (!on_queue_.insert(item).second) return false;
        queue_.push_back(item);
        return true;
    }

    bool empty() const { return queue_.empty(); }
    std::size_t size() const { return queue_.size(); }

    T pop() {
        T item = std::move(queue_.front());
        queue_.pop_front();
        on_queue_.erase(item);
        return item;
    }
};

}  // namespace bbq
