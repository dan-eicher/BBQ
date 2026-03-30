// ============================================================
// BBQ AST — Auto-generated from BBQ.asdl
//
// Do not edit by hand. Re-generate with:
//   ./asdl -i BBQ.asdl -t templates/bbq_ast.inja -o BBQ_AST.h
// ============================================================
#ifndef CALC_IR_AST_H
#define CALC_IR_AST_H

#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace calc_ir {

// ── Enum types ──────────────────────────────────────────────

// ── Forward declarations ────────────────────────────────────

struct Node;
struct SlotRef;
struct LoadConst;
struct Add;
struct Sub;
struct Mul;
struct Neg;
struct Halt;

// ── Source location ─────────────────────────────────────────

struct SourceLoc {
    const char* file = nullptr;
    int line   = 0;
    int column = 0;
};

// ── Arena allocator ─────────────────────────────────────────

class Arena {
public:
    explicit Arena(size_t block_size = 4096)
        : default_block_size_(align_up(block_size, kAlignment)) {}

    ~Arena() {
        for (auto* b : blocks_) std::free(b);
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(size_t size) {
        if (size == 0) return nullptr;
        size_t aligned = align_up(offset_, kAlignment);
        if (!current_ || aligned + size > capacity_) {
            grow(std::max(size + kAlignment, default_block_size_));
            aligned = align_up(offset_, kAlignment);
        }
        void* ptr = current_ + aligned;
        offset_ = aligned + size;
        return ptr;
    }

    void reset() {
        for (auto* b : blocks_) std::free(b);
        blocks_.clear();
        current_ = nullptr;
        offset_ = capacity_ = 0;
    }

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
};

// ── Visitor ─────────────────────────────────────────────────

struct ASTVisitor {
    virtual ~ASTVisitor() = default;

    virtual void visit(SlotRef* node) {}
    virtual void visit(LoadConst* node) {}
    virtual void visit(Add* node) {}
    virtual void visit(Sub* node) {}
    virtual void visit(Mul* node) {}
    virtual void visit(Neg* node) {}
    virtual void visit(Halt* node) {}
};

// ── AST node base ───────────────────────────────────────────

struct ASTNode {
    SourceLoc loc;

    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor& visitor) = 0;

    void* operator new(size_t size) {
        return get_arena().allocate(size);
    }
    void operator delete(void*) noexcept {}

    static Arena& get_arena() {
        static Arena arena;
        return arena;
    }
};

// ── Sum types (class hierarchies) ───────────────────────────

// node
struct Node : public ASTNode {
    virtual ~Node() = default;
};

struct SlotRef : public Node {
    SlotRef(
        int64_t slot,
        Node* next
    )
        : slot(slot), next(next) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t slot;
    Node* next;
};

struct LoadConst : public Node {
    LoadConst(
        int64_t value,
        int64_t dest,
        Node* next
    )
        : value(value), dest(dest), next(next) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t value;
    int64_t dest;
    Node* next;
};

struct Add : public Node {
    Add(
        int64_t lhs,
        int64_t rhs,
        int64_t dest,
        Node* next
    )
        : lhs(lhs), rhs(rhs), dest(dest), next(next) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t lhs;
    int64_t rhs;
    int64_t dest;
    Node* next;
};

struct Sub : public Node {
    Sub(
        int64_t lhs,
        int64_t rhs,
        int64_t dest,
        Node* next
    )
        : lhs(lhs), rhs(rhs), dest(dest), next(next) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t lhs;
    int64_t rhs;
    int64_t dest;
    Node* next;
};

struct Mul : public Node {
    Mul(
        int64_t lhs,
        int64_t rhs,
        int64_t dest,
        Node* next
    )
        : lhs(lhs), rhs(rhs), dest(dest), next(next) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t lhs;
    int64_t rhs;
    int64_t dest;
    Node* next;
};

struct Neg : public Node {
    Neg(
        int64_t src,
        int64_t dest,
        Node* next
    )
        : src(src), dest(dest), next(next) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t src;
    int64_t dest;
    Node* next;
};

struct Halt : public Node {
    Halt(
        int64_t result_slot
    )
        : result_slot(result_slot) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t result_slot;
};

// ── Product types ───────────────────────────────────────────

} // namespace calc_ir

#endif // CALC_IR_AST_H
