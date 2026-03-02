#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bbq::cek {

class StringPool {
public:
    explicit StringPool(size_t block_size = 4096)
        : default_block_size_(block_size) {}

    ~StringPool() {
        for (auto* b : blocks_) std::free(b);
    }

    StringPool(const StringPool&) = delete;
    StringPool& operator=(const StringPool&) = delete;

    const char* intern(std::string_view sv) {
        auto it = table_.find(sv);
        if (it != table_.end()) return it->second;

        // Allocate space for string bytes + null terminator
        char* buf = alloc_bytes(sv.size() + 1);
        std::memcpy(buf, sv.data(), sv.size());
        buf[sv.size()] = '\0';

        std::string_view key(buf, sv.size());
        table_.emplace(key, buf);
        return buf;
    }

private:
    char* alloc_bytes(size_t size) {
        if (!current_ || offset_ + size > capacity_) {
            size_t sz = std::max(size, default_block_size_);
            auto* blk = static_cast<char*>(std::malloc(sz));
            if (!blk) throw std::bad_alloc();
            blocks_.push_back(blk);
            current_ = blk;
            offset_ = 0;
            capacity_ = sz;
        }
        char* ptr = current_ + offset_;
        offset_ += size;
        return ptr;
    }

    std::unordered_map<std::string_view, const char*> table_;
    std::vector<char*> blocks_;
    char*  current_  = nullptr;
    size_t offset_   = 0;
    size_t capacity_ = 0;
    size_t default_block_size_;
};

} // namespace bbq::cek
