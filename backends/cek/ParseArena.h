#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>
#include <vector>

namespace bbq::cek {

class ParseArena {
public:
    explicit ParseArena(size_t block_size = 256 * 1024)
        : default_block_size_(align_up(block_size, kAlignment)) {}

    ~ParseArena() {
        for (auto* b : blocks_) std::free(b);
    }

    ParseArena(const ParseArena&) = delete;
    ParseArena& operator=(const ParseArena&) = delete;

    void* allocate(size_t size, size_t alignment = kAlignment) {
        if (size == 0) return nullptr;
        size_t aligned = align_up(offset_, alignment);
        if (!current_ || aligned + size > capacity_) {
            grow(std::max(size + alignment, default_block_size_));
            aligned = align_up(offset_, alignment);
        }
        void* ptr = current_ + aligned;
        offset_ = aligned + size;
        bytes_used_ += size;
        return ptr;
    }

    template<typename T, typename... Args>
    T* alloc(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    void reset() {
        for (auto* b : blocks_) std::free(b);
        blocks_.clear();
        current_ = nullptr;
        offset_ = capacity_ = 0;
        bytes_used_ = 0;
    }

    size_t bytes_used() const { return bytes_used_; }

private:
    static constexpr size_t kAlignment = alignof(std::max_align_t);

    static size_t align_up(size_t v, size_t a) noexcept {
        return (v + a - 1) & ~(a - 1);
    }

    void grow(size_t min_size) {
        size_t sz = align_up(min_size, kAlignment);
        auto* blk = static_cast<char*>(std::malloc(sz));
        if (!blk) throw std::bad_alloc();
        blocks_.push_back(blk);
        current_ = blk;
        offset_ = 0;
        capacity_ = sz;
    }

    std::vector<char*> blocks_;
    char*  current_  = nullptr;
    size_t offset_   = 0;
    size_t capacity_ = 0;
    size_t default_block_size_;
    size_t bytes_used_ = 0;
};

} // namespace bbq::cek
