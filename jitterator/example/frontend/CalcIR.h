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
struct LoadConst;
struct LoadLocal;
struct Add;
struct Sub;
struct Mul;
struct Div;
struct Neg;
struct CmpEq;
struct CmpNeq;
struct CmpLt;
struct CmpLe;
struct CmpGt;
struct CmpGe;
struct Call;
struct StoreLocal;
struct ExprEffect;
struct Branch;
struct Goto;
struct Leave;
struct Halt;
struct Nop;

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

    virtual void visit(LoadConst* node) {}
    virtual void visit(LoadLocal* node) {}
    virtual void visit(Add* node) {}
    virtual void visit(Sub* node) {}
    virtual void visit(Mul* node) {}
    virtual void visit(Div* node) {}
    virtual void visit(Neg* node) {}
    virtual void visit(CmpEq* node) {}
    virtual void visit(CmpNeq* node) {}
    virtual void visit(CmpLt* node) {}
    virtual void visit(CmpLe* node) {}
    virtual void visit(CmpGt* node) {}
    virtual void visit(CmpGe* node) {}
    virtual void visit(Call* node) {}
    virtual void visit(StoreLocal* node) {}
    virtual void visit(ExprEffect* node) {}
    virtual void visit(Branch* node) {}
    virtual void visit(Goto* node) {}
    virtual void visit(Leave* node) {}
    virtual void visit(Halt* node) {}
    virtual void visit(Nop* node) {}
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
//
// Per-sum Tag enum + non-virtual `tag` field on the base class give
// O(1) dispatch via `switch (node->tag)` and let callers `static_cast`
// instead of `dynamic_cast` once the tag is matched. Each derived
// stamps a `static constexpr NodeTag kind` (the
// canonical value for that subclass, used to initialise the base's
// tag) and a `static constexpr int kind_value` (plain-int form for
// burgc TERM declarations and any other place that wants the
// discriminator as an integer literal).
enum class NodeTag : int {
    LoadConst,
    LoadLocal,
    Add,
    Sub,
    Mul,
    Div,
    Neg,
    CmpEq,
    CmpNeq,
    CmpLt,
    CmpLe,
    CmpGt,
    CmpGe,
    Call,
    StoreLocal,
    ExprEffect,
    Branch,
    Goto,
    Leave,
    Halt,
    Nop
};

struct Node : public ASTNode {
    NodeTag tag;
    explicit Node(NodeTag t) : tag(t) {}
    virtual ~Node() = default;
};

struct LoadConst : public Node {
    static constexpr NodeTag kind = NodeTag::LoadConst;
    static constexpr int kind_value = static_cast<int>(NodeTag::LoadConst);

    LoadConst(
        int64_t value
    )
        : Node(kind), value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t value;
};

struct LoadLocal : public Node {
    static constexpr NodeTag kind = NodeTag::LoadLocal;
    static constexpr int kind_value = static_cast<int>(NodeTag::LoadLocal);

    LoadLocal(
        int64_t src
    )
        : Node(kind), src(src) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t src;
};

struct Add : public Node {
    static constexpr NodeTag kind = NodeTag::Add;
    static constexpr int kind_value = static_cast<int>(NodeTag::Add);

    Add(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct Sub : public Node {
    static constexpr NodeTag kind = NodeTag::Sub;
    static constexpr int kind_value = static_cast<int>(NodeTag::Sub);

    Sub(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct Mul : public Node {
    static constexpr NodeTag kind = NodeTag::Mul;
    static constexpr int kind_value = static_cast<int>(NodeTag::Mul);

    Mul(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct Div : public Node {
    static constexpr NodeTag kind = NodeTag::Div;
    static constexpr int kind_value = static_cast<int>(NodeTag::Div);

    Div(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct Neg : public Node {
    static constexpr NodeTag kind = NodeTag::Neg;
    static constexpr int kind_value = static_cast<int>(NodeTag::Neg);

    Neg(
        Node* operand
    )
        : Node(kind), operand(operand) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* operand;
};

struct CmpEq : public Node {
    static constexpr NodeTag kind = NodeTag::CmpEq;
    static constexpr int kind_value = static_cast<int>(NodeTag::CmpEq);

    CmpEq(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct CmpNeq : public Node {
    static constexpr NodeTag kind = NodeTag::CmpNeq;
    static constexpr int kind_value = static_cast<int>(NodeTag::CmpNeq);

    CmpNeq(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct CmpLt : public Node {
    static constexpr NodeTag kind = NodeTag::CmpLt;
    static constexpr int kind_value = static_cast<int>(NodeTag::CmpLt);

    CmpLt(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct CmpLe : public Node {
    static constexpr NodeTag kind = NodeTag::CmpLe;
    static constexpr int kind_value = static_cast<int>(NodeTag::CmpLe);

    CmpLe(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct CmpGt : public Node {
    static constexpr NodeTag kind = NodeTag::CmpGt;
    static constexpr int kind_value = static_cast<int>(NodeTag::CmpGt);

    CmpGt(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct CmpGe : public Node {
    static constexpr NodeTag kind = NodeTag::CmpGe;
    static constexpr int kind_value = static_cast<int>(NodeTag::CmpGe);

    CmpGe(
        Node* left,
        Node* right
    )
        : Node(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* left;
    Node* right;
};

struct Call : public Node {
    static constexpr NodeTag kind = NodeTag::Call;
    static constexpr int kind_value = static_cast<int>(NodeTag::Call);

    Call(
        Node* target,
        std::vector<Node*> args,
        int64_t frame_size
    )
        : Node(kind), target(target), args(std::move(args)), frame_size(frame_size) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* target;
    std::vector<Node*> args;
    int64_t frame_size;
};

struct StoreLocal : public Node {
    static constexpr NodeTag kind = NodeTag::StoreLocal;
    static constexpr int kind_value = static_cast<int>(NodeTag::StoreLocal);

    StoreLocal(
        int64_t dest,
        Node* value,
        Node* next
    )
        : Node(kind), dest(dest), value(value), next(next) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t dest;
    Node* value;
    Node* next;
};

struct ExprEffect : public Node {
    static constexpr NodeTag kind = NodeTag::ExprEffect;
    static constexpr int kind_value = static_cast<int>(NodeTag::ExprEffect);

    ExprEffect(
        Node* value,
        Node* next
    )
        : Node(kind), value(value), next(next) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* value;
    Node* next;
};

struct Branch : public Node {
    static constexpr NodeTag kind = NodeTag::Branch;
    static constexpr int kind_value = static_cast<int>(NodeTag::Branch);

    Branch(
        Node* test,
        Node* then_,
        Node* else_
    )
        : Node(kind), test(test), then_(then_), else_(else_) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* test;
    Node* then_;
    Node* else_;
};

struct Goto : public Node {
    static constexpr NodeTag kind = NodeTag::Goto;
    static constexpr int kind_value = static_cast<int>(NodeTag::Goto);

    Goto(
        Node* target
    )
        : Node(kind), target(target) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* target;
};

struct Leave : public Node {
    static constexpr NodeTag kind = NodeTag::Leave;
    static constexpr int kind_value = static_cast<int>(NodeTag::Leave);

    Leave(
        Node* value
    )
        : Node(kind), value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* value;
};

struct Halt : public Node {
    static constexpr NodeTag kind = NodeTag::Halt;
    static constexpr int kind_value = static_cast<int>(NodeTag::Halt);

    Halt(
        Node* value
    )
        : Node(kind), value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* value;
};

struct Nop : public Node {
    static constexpr NodeTag kind = NodeTag::Nop;
    static constexpr int kind_value = static_cast<int>(NodeTag::Nop);

    Nop(
        Node* next
    )
        : Node(kind), next(next) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Node* next;
};

// ── Product types ───────────────────────────────────────────

} // namespace calc_ir

#endif // CALC_IR_AST_H
