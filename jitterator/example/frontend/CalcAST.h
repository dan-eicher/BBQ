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
    Mul,
    Div
};

// ── Forward declarations ────────────────────────────────────

struct Program;
struct Function;
struct Expr;
struct Program;
struct Function;
struct IntLit;
struct BinOp;
struct NegOp;
struct CmpEq;
struct CmpNeq;
struct CmpLt;
struct CmpLe;
struct CmpGt;
struct CmpGe;
struct Sequence;
struct If;
struct While;
struct Loop;
struct Break;
struct Return;
struct And;
struct Or;
struct Not;
struct Var;
struct VarAssign;
struct LetDecl;
struct Call;
struct LocalRef;
struct LocalAssign;

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
    virtual void visit(Function* node) {}
    virtual void visit(IntLit* node) {}
    virtual void visit(BinOp* node) {}
    virtual void visit(NegOp* node) {}
    virtual void visit(CmpEq* node) {}
    virtual void visit(CmpNeq* node) {}
    virtual void visit(CmpLt* node) {}
    virtual void visit(CmpLe* node) {}
    virtual void visit(CmpGt* node) {}
    virtual void visit(CmpGe* node) {}
    virtual void visit(Sequence* node) {}
    virtual void visit(If* node) {}
    virtual void visit(While* node) {}
    virtual void visit(Loop* node) {}
    virtual void visit(Break* node) {}
    virtual void visit(Return* node) {}
    virtual void visit(And* node) {}
    virtual void visit(Or* node) {}
    virtual void visit(Not* node) {}
    virtual void visit(Var* node) {}
    virtual void visit(VarAssign* node) {}
    virtual void visit(LetDecl* node) {}
    virtual void visit(Call* node) {}
    virtual void visit(LocalRef* node) {}
    virtual void visit(LocalAssign* node) {}
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
        std::vector<Function*> functions,
        Expr* body
    )
        : functions(std::move(functions)), body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<Function*> functions;
    Expr* body;
};

struct Function : public ASTNode {
    Function(
        std::string name,
        std::vector<std::string> params,
        Expr* body
    )
        : name(name), params(std::move(params)), body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<std::string> params;
    Expr* body;
};

// expr
//
// Per-sum Tag enum + non-virtual `tag` field on the base class give
// O(1) dispatch via `switch (node->tag)` and let callers `static_cast`
// instead of `dynamic_cast` once the tag is matched. Each derived
// stamps a `static constexpr ExprTag kind` (the
// canonical value for that subclass, used to initialise the base's
// tag) and a `static constexpr int kind_value` (plain-int form for
// burgc TERM declarations and any other place that wants the
// discriminator as an integer literal).
enum class ExprTag : int {
    IntLit,
    BinOp,
    NegOp,
    CmpEq,
    CmpNeq,
    CmpLt,
    CmpLe,
    CmpGt,
    CmpGe,
    Sequence,
    If,
    While,
    Loop,
    Break,
    Return,
    And,
    Or,
    Not,
    Var,
    VarAssign,
    LetDecl,
    Call,
    LocalRef,
    LocalAssign
};

struct Expr : public ASTNode {
    ExprTag tag;
    explicit Expr(ExprTag t) : tag(t) {}
    virtual ~Expr() = default;
};

struct IntLit : public Expr {
    static constexpr ExprTag kind = ExprTag::IntLit;
    static constexpr int kind_value = static_cast<int>(ExprTag::IntLit);

    IntLit(
        int64_t value
    )
        : Expr(kind), value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t value;
};

struct BinOp : public Expr {
    static constexpr ExprTag kind = ExprTag::BinOp;
    static constexpr int kind_value = static_cast<int>(ExprTag::BinOp);

    BinOp(
        Op op,
        Expr* left,
        Expr* right
    )
        : Expr(kind), op(op), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Op op;
    Expr* left;
    Expr* right;
};

struct NegOp : public Expr {
    static constexpr ExprTag kind = ExprTag::NegOp;
    static constexpr int kind_value = static_cast<int>(ExprTag::NegOp);

    NegOp(
        Expr* operand
    )
        : Expr(kind), operand(operand) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* operand;
};

struct CmpEq : public Expr {
    static constexpr ExprTag kind = ExprTag::CmpEq;
    static constexpr int kind_value = static_cast<int>(ExprTag::CmpEq);

    CmpEq(
        Expr* left,
        Expr* right
    )
        : Expr(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* left;
    Expr* right;
};

struct CmpNeq : public Expr {
    static constexpr ExprTag kind = ExprTag::CmpNeq;
    static constexpr int kind_value = static_cast<int>(ExprTag::CmpNeq);

    CmpNeq(
        Expr* left,
        Expr* right
    )
        : Expr(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* left;
    Expr* right;
};

struct CmpLt : public Expr {
    static constexpr ExprTag kind = ExprTag::CmpLt;
    static constexpr int kind_value = static_cast<int>(ExprTag::CmpLt);

    CmpLt(
        Expr* left,
        Expr* right
    )
        : Expr(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* left;
    Expr* right;
};

struct CmpLe : public Expr {
    static constexpr ExprTag kind = ExprTag::CmpLe;
    static constexpr int kind_value = static_cast<int>(ExprTag::CmpLe);

    CmpLe(
        Expr* left,
        Expr* right
    )
        : Expr(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* left;
    Expr* right;
};

struct CmpGt : public Expr {
    static constexpr ExprTag kind = ExprTag::CmpGt;
    static constexpr int kind_value = static_cast<int>(ExprTag::CmpGt);

    CmpGt(
        Expr* left,
        Expr* right
    )
        : Expr(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* left;
    Expr* right;
};

struct CmpGe : public Expr {
    static constexpr ExprTag kind = ExprTag::CmpGe;
    static constexpr int kind_value = static_cast<int>(ExprTag::CmpGe);

    CmpGe(
        Expr* left,
        Expr* right
    )
        : Expr(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* left;
    Expr* right;
};

struct Sequence : public Expr {
    static constexpr ExprTag kind = ExprTag::Sequence;
    static constexpr int kind_value = static_cast<int>(ExprTag::Sequence);

    Sequence(
        Expr* first,
        Expr* rest
    )
        : Expr(kind), first(first), rest(rest) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* first;
    Expr* rest;
};

struct If : public Expr {
    static constexpr ExprTag kind = ExprTag::If;
    static constexpr int kind_value = static_cast<int>(ExprTag::If);

    If(
        Expr* test,
        Expr* then_,
        std::optional<Expr*> else_
    )
        : Expr(kind), test(test), then_(then_), else_(std::move(else_)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* test;
    Expr* then_;
    std::optional<Expr*> else_;
};

struct While : public Expr {
    static constexpr ExprTag kind = ExprTag::While;
    static constexpr int kind_value = static_cast<int>(ExprTag::While);

    While(
        Expr* test,
        Expr* body
    )
        : Expr(kind), test(test), body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* test;
    Expr* body;
};

struct Loop : public Expr {
    static constexpr ExprTag kind = ExprTag::Loop;
    static constexpr int kind_value = static_cast<int>(ExprTag::Loop);

    Loop(
        Expr* body
    )
        : Expr(kind), body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* body;
};

struct Break : public Expr {
    static constexpr ExprTag kind = ExprTag::Break;
    static constexpr int kind_value = static_cast<int>(ExprTag::Break);

    Break() : Expr(kind) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct Return : public Expr {
    static constexpr ExprTag kind = ExprTag::Return;
    static constexpr int kind_value = static_cast<int>(ExprTag::Return);

    Return(
        Expr* value
    )
        : Expr(kind), value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* value;
};

struct And : public Expr {
    static constexpr ExprTag kind = ExprTag::And;
    static constexpr int kind_value = static_cast<int>(ExprTag::And);

    And(
        Expr* left,
        Expr* right
    )
        : Expr(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* left;
    Expr* right;
};

struct Or : public Expr {
    static constexpr ExprTag kind = ExprTag::Or;
    static constexpr int kind_value = static_cast<int>(ExprTag::Or);

    Or(
        Expr* left,
        Expr* right
    )
        : Expr(kind), left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* left;
    Expr* right;
};

struct Not : public Expr {
    static constexpr ExprTag kind = ExprTag::Not;
    static constexpr int kind_value = static_cast<int>(ExprTag::Not);

    Not(
        Expr* operand
    )
        : Expr(kind), operand(operand) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* operand;
};

struct Var : public Expr {
    static constexpr ExprTag kind = ExprTag::Var;
    static constexpr int kind_value = static_cast<int>(ExprTag::Var);

    Var(
        std::string name
    )
        : Expr(kind), name(name) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct VarAssign : public Expr {
    static constexpr ExprTag kind = ExprTag::VarAssign;
    static constexpr int kind_value = static_cast<int>(ExprTag::VarAssign);

    VarAssign(
        std::string name,
        Expr* value
    )
        : Expr(kind), name(name), value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    Expr* value;
};

struct LetDecl : public Expr {
    static constexpr ExprTag kind = ExprTag::LetDecl;
    static constexpr int kind_value = static_cast<int>(ExprTag::LetDecl);

    LetDecl(
        std::string name,
        Expr* value
    )
        : Expr(kind), name(name), value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    Expr* value;
};

struct Call : public Expr {
    static constexpr ExprTag kind = ExprTag::Call;
    static constexpr int kind_value = static_cast<int>(ExprTag::Call);

    Call(
        std::string name,
        std::vector<Expr*> args
    )
        : Expr(kind), name(name), args(std::move(args)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<Expr*> args;
};

struct LocalRef : public Expr {
    static constexpr ExprTag kind = ExprTag::LocalRef;
    static constexpr int kind_value = static_cast<int>(ExprTag::LocalRef);

    LocalRef(
        int64_t index
    )
        : Expr(kind), index(index) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t index;
};

struct LocalAssign : public Expr {
    static constexpr ExprTag kind = ExprTag::LocalAssign;
    static constexpr int kind_value = static_cast<int>(ExprTag::LocalAssign);

    LocalAssign(
        int64_t index,
        Expr* value
    )
        : Expr(kind), index(index), value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t index;
    Expr* value;
};

// ── Product types ───────────────────────────────────────────

} // namespace calc

#endif // CALC_AST_H
