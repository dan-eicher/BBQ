// jitterator.h — Platform code buffer for Copy-and-Patch JIT
//
// Provides mmap/mprotect buffer management and icache flushing.
// This file ships as-is — it is NOT generated.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace jitterator {

class CodeBuffer {
public:
    explicit CodeBuffer(size_t initial_capacity = 4096)
        : capacity_(initial_capacity), size_(0) {
        capacity_ = align_to_page(capacity_);
#ifdef _WIN32
        base_ = (uint8_t*)VirtualAlloc(nullptr, capacity_,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!base_) throw std::runtime_error("VirtualAlloc failed");
#else
        base_ = (uint8_t*)mmap(nullptr, capacity_, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base_ == MAP_FAILED) throw std::runtime_error("mmap failed");
#endif
    }

    ~CodeBuffer() {
        if (base_) {
#ifdef _WIN32
            VirtualFree(base_, 0, MEM_RELEASE);
#else
            munmap(base_, capacity_);
#endif
        }
    }

    CodeBuffer(const CodeBuffer&) = delete;
    CodeBuffer& operator=(const CodeBuffer&) = delete;

    CodeBuffer(CodeBuffer&& o) noexcept
        : base_(o.base_), capacity_(o.capacity_), size_(o.size_), finalized_(o.finalized_) {
        o.base_ = nullptr;
        o.capacity_ = 0;
        o.size_ = 0;
        o.finalized_ = false;
    }

    // Append raw bytes to the buffer
    void emit_bytes(const uint8_t* data, size_t len) {
        ensure_capacity(len);
        std::memcpy(base_ + size_, data, len);
        size_ += len;
    }

    // Overwrite 4 bytes at an absolute offset (for patching)
    void patch_32(size_t offset, int32_t value) {
        std::memcpy(base_ + offset, &value, 4);
    }

    // Overwrite 8 bytes at an absolute offset (for patching)
    void patch_64(size_t offset, uint64_t value) {
        std::memcpy(base_ + offset, &value, 8);
    }

    // Finalize: make executable, flush icache, return base pointer.
    // After this call, no further emit/patch calls are allowed.
    void* finalize() {
        if (finalized_) return base_;
        finalized_ = true;
#ifdef _WIN32
        DWORD old;
        if (!VirtualProtect(base_, capacity_, PAGE_EXECUTE_READ, &old))
            throw std::runtime_error("VirtualProtect failed");
#else
        if (mprotect(base_, capacity_, PROT_READ | PROT_EXEC) != 0)
            throw std::runtime_error("mprotect failed");
#endif
        icache_flush();
        return base_;
    }

    uint8_t* data() { return base_; }
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }

    void reset() {
        if (finalized_) {
#ifdef _WIN32
            DWORD old;
            if (!VirtualProtect(base_, capacity_, PAGE_READWRITE, &old))
                throw std::runtime_error("VirtualProtect failed");
#else
            if (mprotect(base_, capacity_, PROT_READ | PROT_WRITE) != 0)
                throw std::runtime_error("mprotect failed");
#endif
            finalized_ = false;
        }
        size_ = 0;
    }

private:
    uint8_t* base_ = nullptr;
    size_t capacity_ = 0;
    size_t size_ = 0;
    bool finalized_ = false;

    void ensure_capacity(size_t extra) {
        if (size_ + extra <= capacity_) return;
        // Grow: allocate new buffer, copy, release old
        size_t new_cap = align_to_page((capacity_ * 2 > size_ + extra)
                                       ? capacity_ * 2
                                       : size_ + extra + 4096);
#ifdef _WIN32
        auto* nb = (uint8_t*)VirtualAlloc(nullptr, new_cap,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!nb) throw std::runtime_error("VirtualAlloc grow failed");
        std::memcpy(nb, base_, size_);
        VirtualFree(base_, 0, MEM_RELEASE);
#else
        auto* nb = (uint8_t*)mmap(nullptr, new_cap, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (nb == MAP_FAILED) throw std::runtime_error("mmap grow failed");
        std::memcpy(nb, base_, size_);
        munmap(base_, capacity_);
#endif
        base_ = nb;
        capacity_ = new_cap;
    }

    void icache_flush() {
#if defined(__GNUC__) || defined(__clang__)
        __builtin___clear_cache(reinterpret_cast<char*>(base_),
                                reinterpret_cast<char*>(base_ + size_));
#elif defined(_WIN32)
        FlushInstructionCache(GetCurrentProcess(), base_, size_);
#endif
    }

    static size_t align_to_page(size_t n) {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        size_t ps = si.dwPageSize;
#else
        size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
        return (n + ps - 1) & ~(ps - 1);
    }
};

} // namespace jitterator
