#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <string_view>
#include <stdexcept>

namespace bbq {

// --- Checkpoint for save/restore backtracking ---

struct Checkpoint {
    size_t pos;
    size_t interval_depth;
    size_t error_count;
    size_t scope_depth;
    size_t loop_depth;
    bool little_endian;
};

// --- Interval (byte range scope) ---

struct Interval {
    size_t start;
    size_t end;
};

// --- Parse error info ---

struct ParseError {
    size_t pos;
    std::string message;
};

// --- Forward declaration ---
class BBQContext;

// --- RAII scope guard (parent pointer stack) ---

class ScopeGuard {
public:
    ScopeGuard(BBQContext& ctx, void* ptr);
    ~ScopeGuard();
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
private:
    BBQContext& ctx_;
    size_t saved_;
};

// --- RAII loop guard (loop index stack) ---

class LoopGuard {
public:
    explicit LoopGuard(BBQContext& ctx);
    ~LoopGuard();
    LoopGuard(const LoopGuard&) = delete;
    LoopGuard& operator=(const LoopGuard&) = delete;
private:
    BBQContext& ctx_;
    size_t saved_;
};

// --- RAII interval scope guard ---

class IntervalScope {
public:
    IntervalScope(BBQContext& ctx, size_t start, size_t end);
    ~IntervalScope();
    IntervalScope(IntervalScope&& o) noexcept : ctx_(o.ctx_), active_(o.active_) { o.active_ = false; }
    IntervalScope(const IntervalScope&) = delete;
    IntervalScope& operator=(const IntervalScope&) = delete;
private:
    BBQContext& ctx_;
    bool active_ = true;
};

// --- Main context ---

class BBQContext {
    friend class IntervalScope;
    friend class ScopeGuard;
    friend class LoopGuard;
public:
    BBQContext(const uint8_t* data, size_t length, bool little_endian = true)
        : data_(data), length_(length), pos_(0), interval_depth_(0),
          little_endian_(little_endian) {}

    // -- Position --
    size_t pos() const { return pos_; }
    size_t remaining() const { return effective_end() - pos_; }
    size_t total_size() const { return length_; }
    bool at_end() const { return pos_ >= effective_end(); }
    uint8_t peek() const { return at_end() ? 0 : data_[pos_]; }

    // -- Scope stack (parent pointers for cross-rule references) --
    void push_scope(void* ptr) {
        if (scope_depth_ >= MAX_SCOPE_DEPTH)
            throw std::runtime_error("BBQ: scope stack overflow");
        scope_ptrs_[scope_depth_++] = ptr;
    }
    void pop_scope() { if (scope_depth_ > 0) --scope_depth_; }
    void* scope_ptr(size_t levels_up = 0) const {
        if (levels_up >= scope_depth_) return nullptr;
        return scope_ptrs_[scope_depth_ - 1 - levels_up];
    }
    size_t scope_depth() const { return scope_depth_; }

    // -- Loop index stack (for cross-rule 'i' access) --
    void push_loop() {
        if (loop_depth_ >= MAX_SCOPE_DEPTH)
            throw std::runtime_error("BBQ: loop stack overflow");
        loop_indices_[loop_depth_++] = 0;
    }
    void pop_loop() { if (loop_depth_ > 0) --loop_depth_; }
    void set_loop_index(int64_t i) {
        if (loop_depth_ > 0) loop_indices_[loop_depth_ - 1] = i;
    }
    int64_t loop_index() const {
        if (loop_depth_ == 0) return 0;
        return loop_indices_[loop_depth_ - 1];
    }
    size_t loop_depth() const { return loop_depth_; }

    // -- Typed reads --
    bool read_uint8(uint8_t& out) {
        if (!bounds_check(1)) return false;
        out = data_[pos_++];
        return true;
    }

    bool read_int8(int8_t& out) {
        if (!bounds_check(1)) return false;
        out = static_cast<int8_t>(data_[pos_++]);
        return true;
    }

    bool read_bool(bool& out) {
        if (!bounds_check(1)) return false;
        out = data_[pos_++] != 0;
        return true;
    }

    bool read_uint16le(uint16_t& out) { return read_le<uint16_t>(out); }
    bool read_uint16be(uint16_t& out) { return read_be<uint16_t>(out); }
    bool read_int16le(int16_t& out)   { return read_le_signed<int16_t, uint16_t>(out); }
    bool read_int16be(int16_t& out)   { return read_be_signed<int16_t, uint16_t>(out); }

    bool read_uint32le(uint32_t& out) { return read_le<uint32_t>(out); }
    bool read_uint32be(uint32_t& out) { return read_be<uint32_t>(out); }
    bool read_int32le(int32_t& out)   { return read_le_signed<int32_t, uint32_t>(out); }
    bool read_int32be(int32_t& out)   { return read_be_signed<int32_t, uint32_t>(out); }

    bool read_uint64le(uint64_t& out) { return read_le<uint64_t>(out); }
    bool read_uint64be(uint64_t& out) { return read_be<uint64_t>(out); }
    bool read_int64le(int64_t& out)   { return read_le_signed<int64_t, uint64_t>(out); }
    bool read_int64be(int64_t& out)   { return read_be_signed<int64_t, uint64_t>(out); }

    bool read_float32le(float& out) {
        uint32_t raw;
        if (!read_le<uint32_t>(raw)) return false;
        memcpy(&out, &raw, 4);
        return true;
    }
    bool read_float32be(float& out) {
        uint32_t raw;
        if (!read_be<uint32_t>(raw)) return false;
        memcpy(&out, &raw, 4);
        return true;
    }
    bool read_float64le(double& out) {
        uint64_t raw;
        if (!read_le<uint64_t>(raw)) return false;
        memcpy(&out, &raw, 8);
        return true;
    }
    bool read_float64be(double& out) {
        uint64_t raw;
        if (!read_be<uint64_t>(raw)) return false;
        memcpy(&out, &raw, 8);
        return true;
    }

    // -- Runtime endianness --
    void set_endian(bool little_endian) { little_endian_ = little_endian; }
    bool is_runtime_little_endian() const { return little_endian_; }

    // -- Dispatching reads (use runtime endianness) --
    bool read_uint16(uint16_t& out) { return little_endian_ ? read_uint16le(out) : read_uint16be(out); }
    bool read_int16(int16_t& out)   { return little_endian_ ? read_int16le(out)  : read_int16be(out); }
    bool read_uint32(uint32_t& out) { return little_endian_ ? read_uint32le(out) : read_uint32be(out); }
    bool read_int32(int32_t& out)   { return little_endian_ ? read_int32le(out)  : read_int32be(out); }
    bool read_uint64(uint64_t& out) { return little_endian_ ? read_uint64le(out) : read_uint64be(out); }
    bool read_int64(int64_t& out)   { return little_endian_ ? read_int64le(out)  : read_int64be(out); }
    bool read_float32(float& out)   { return little_endian_ ? read_float32le(out): read_float32be(out); }
    bool read_float64(double& out)  { return little_endian_ ? read_float64le(out): read_float64be(out); }

    bool read_bytes(std::vector<uint8_t>& out, size_t count) {
        if (!bounds_check(count)) return false;
        out.assign(data_ + pos_, data_ + pos_ + count);
        pos_ += count;
        return true;
    }

    bool read_string(std::string& out, size_t count) {
        if (!bounds_check(count)) return false;
        out.assign(reinterpret_cast<const char*>(data_ + pos_), count);
        pos_ += count;
        return true;
    }

    // -- Interval management --
    IntervalScope push_interval(size_t start, size_t end) {
        return IntervalScope(*this, start, end);
    }

    // -- Save / restore --
    Checkpoint save() const {
        return {pos_, interval_depth_, errors_.size(), scope_depth_, loop_depth_, little_endian_};
    }
    void restore(const Checkpoint& cp) {
        pos_ = cp.pos;
        interval_depth_ = cp.interval_depth;
        errors_.resize(cp.error_count);
        scope_depth_ = cp.scope_depth;
        loop_depth_ = cp.loop_depth;
        little_endian_ = cp.little_endian;
    }

    // -- Error reporting --
    bool fail(const char* fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        errors_.push_back({pos_, std::string(buf)});
        return false;
    }

    const std::vector<ParseError>& errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }

    std::string format_errors() const {
        std::string result;
        for (auto& e : errors_) {
            result += "offset " + std::to_string(e.pos) + ": " + e.message + "\n";
        }
        return result;
    }

private:
    const uint8_t* data_;
    size_t length_;
    size_t pos_;

    static constexpr size_t MAX_INTERVAL_DEPTH = 64;
    Interval intervals_[MAX_INTERVAL_DEPTH];
    size_t interval_depth_;

    std::vector<ParseError> errors_;
    bool little_endian_ = true;

    static constexpr size_t MAX_SCOPE_DEPTH = 64;
    void* scope_ptrs_[MAX_SCOPE_DEPTH] = {};
    size_t scope_depth_ = 0;
    int64_t loop_indices_[MAX_SCOPE_DEPTH] = {};
    size_t loop_depth_ = 0;

    bool bounds_check(size_t needed) const {
        return pos_ + needed <= effective_end();
    }

    size_t effective_end() const {
        if (interval_depth_ > 0)
            return intervals_[interval_depth_ - 1].end;
        return length_;
    }

    // -- Compile-time endian detection --
    static constexpr bool is_little_endian() {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return true;
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return false;
#else
        // Assume little-endian as fallback (x86/ARM)
        return true;
#endif
    }

    // -- Endian read helpers --
    template<typename T>
    bool read_le(T& out) {
        constexpr size_t N = sizeof(T);
        if (!bounds_check(N)) return false;
        T raw;
        memcpy(&raw, data_ + pos_, N);
        pos_ += N;
        if constexpr (is_little_endian()) {
            out = raw;
        } else {
            out = bswap(raw);
        }
        return true;
    }

    template<typename T>
    bool read_be(T& out) {
        constexpr size_t N = sizeof(T);
        if (!bounds_check(N)) return false;
        T raw;
        memcpy(&raw, data_ + pos_, N);
        pos_ += N;
        if constexpr (is_little_endian()) {
            out = bswap(raw);
        } else {
            out = raw;
        }
        return true;
    }

    template<typename S, typename U>
    bool read_le_signed(S& out) {
        U raw;
        if (!read_le<U>(raw)) return false;
        out = static_cast<S>(raw);
        return true;
    }

    template<typename S, typename U>
    bool read_be_signed(S& out) {
        U raw;
        if (!read_be<U>(raw)) return false;
        out = static_cast<S>(raw);
        return true;
    }

    static uint16_t bswap(uint16_t v) { return __builtin_bswap16(v); }
    static uint32_t bswap(uint32_t v) { return __builtin_bswap32(v); }
    static uint64_t bswap(uint64_t v) { return __builtin_bswap64(v); }
};

// --- IntervalScope implementation ---

inline IntervalScope::IntervalScope(BBQContext& ctx, size_t start, size_t end)
    : ctx_(ctx) {
    if (ctx_.interval_depth_ >= BBQContext::MAX_INTERVAL_DEPTH)
        throw std::runtime_error("BBQ: interval stack overflow");
    // Clamp end to data length
    if (end > ctx_.length_)
        end = ctx_.length_;
    ctx_.intervals_[ctx_.interval_depth_++] = {start, end};
    ctx_.pos_ = start;
}

inline IntervalScope::~IntervalScope() {
    if (active_)
        ctx_.interval_depth_--;
}

// --- ScopeGuard implementation ---

inline ScopeGuard::ScopeGuard(BBQContext& ctx, void* ptr)
    : ctx_(ctx), saved_(ctx.scope_depth()) { ctx_.push_scope(ptr); }

inline ScopeGuard::~ScopeGuard() {
    if (ctx_.scope_depth() > saved_) ctx_.pop_scope();
}

// --- LoopGuard implementation ---

inline LoopGuard::LoopGuard(BBQContext& ctx)
    : ctx_(ctx), saved_(ctx.loop_depth()) { ctx_.push_loop(); }

inline LoopGuard::~LoopGuard() {
    if (ctx_.loop_depth() > saved_) ctx_.pop_loop();
}

// --- Built-in functions ---

inline uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

inline uint8_t checksum_xor(const uint8_t* data, size_t length) {
    uint8_t result = 0;
    for (size_t i = 0; i < length; i++) result ^= data[i];
    return result;
}

} // namespace bbq
