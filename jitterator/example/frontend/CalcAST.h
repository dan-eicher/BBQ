// ============================================================
// BBQ AST — Auto-generated from BBQ.asdl
//
// Do not edit by hand. Re-generate with:
//   ./asdl -i BBQ.asdl -t templates/bbq_ast.inja -o BBQ_AST.h
// ============================================================
#ifndef CALC_AST_H
#define CALC_AST_H

#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace calc {

// ── Enum types ──────────────────────────────────────────────

enum class Op {
    Add,
    Sub,
    Mul
};

// ── Forward declarations ────────────────────────────────────

struct Program;
struct Expr;
struct Program;
struct IntLit;
struct SlotRef;
struct BinOp;
struct NegOp;

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

    virtual void visit(Program* node) {}
    virtual void visit(IntLit* node) {}
    virtual void visit(SlotRef* node) {}
    virtual void visit(BinOp* node) {}
    virtual void visit(NegOp* node) {}
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

struct Program : public ASTNode {
    Program(
        Expr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* body;
};

// expr
struct Expr : public ASTNode {
    virtual ~Expr() = default;
};

struct IntLit : public Expr {
    IntLit(
        int64_t value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t value;
};

struct SlotRef : public Expr {
    SlotRef(
        int64_t slot
    )
        : slot(slot) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t slot;
};

struct BinOp : public Expr {
    BinOp(
        Op op,
        Expr* left,
        Expr* right
    )
        : op(op), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Op op;
    Expr* left;
    Expr* right;
};

struct NegOp : public Expr {
    NegOp(
        Expr* operand
    )
        : operand(operand) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* operand;
};

// ── Product types ───────────────────────────────────────────

} // namespace calc

#endif // CALC_AST_H
