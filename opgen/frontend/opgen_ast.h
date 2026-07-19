// ============================================================
// opgen AST — Auto-generated from opgen.asdl
//
// Do not edit by hand. Re-generate with:
//   asdl -i opgen.asdl -t templates/opgen_ast.inja -o opgen_ast.h
//
// Tag-dispatched variant of the grammar AST: multi-constructor
// sum types carry a `…Tag tag` discriminator (set per subtype via
// `static constexpr kind`), so consumers `switch (n->tag)` and
// `as<T>(n)` rather than walk a visitor / dynamic_cast chain.
// ============================================================
#ifndef OPGEN_AST_H
#define OPGEN_AST_H

#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace opgen {

// ── Enum types ──────────────────────────────────────────────

enum class ValueType {
    TyI8,
    TyI16,
    TyI32,
    TyU8,
    TyU16,
    TyRef,
    TyWord,
    TyDword,
    TyI64,
    TyF32,
    TyF64,
    TyV128,
    TyI8x16,
    TyI16x8,
    TyI32x4,
    TyI64x2,
    TyF32x4,
    TyF64x2,
    TyFuncRef,
    TyExternRef,
    TyI31Ref,
    TyUleb32,
    TyUleb64,
    TySleb32,
    TySleb64,
    TyMemarg,
    TyBlockType,
    TyBrTable,
    TyTryTable,
    TySelectVec,
    TyAny,
    TyAddr
};

enum class LocalValueKind {
    LvShort,
    LvInt,
    LvRef,
    LvLong,
    LvFloat,
    LvDouble
};

// ── Forward declarations ────────────────────────────────────

struct StackParam;
struct OperandParam;
struct ErrorCase;
struct LocalValueDecl;
struct SemExpr;
struct SemStmt;
struct MethodParam;
struct MethodDecl;
struct StatusDecl;
struct TypeDecl;
struct Opcode;
struct Module;
struct StackParam;
struct OperandParam;
struct ErrorCase;
struct LocalValueDecl;
struct SBinOp;
struct SUnary;
struct STernary;
struct SCast;
struct SIndex;
struct SCall;
struct SIdent;
struct SInt;
struct SFloat;
struct SAssign;
struct SExprStmt;
struct SLocalDecl;
struct SIf;
struct SWhile;
struct SFor;
struct SBlock;
struct STrap;
struct MethodParam;
struct MethodDecl;
struct StatusDecl;
struct TypeDecl;
struct Opcode;
struct Module;

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

// ── AST node base ───────────────────────────────────────────
//
// Non-polymorphic: dispatch is tag-based (see `as<T>` below), so
// no vtable is needed. Arena-allocated; never deleted.

struct ASTNode {
    SourceLoc loc;

    void* operator new(size_t size) {
        return get_arena().allocate(size);
    }
    void operator delete(void*) noexcept {}

    static Arena& get_arena() {
        static Arena arena;
        return arena;
    }
};

// Tag-checked downcast: returns the derived pointer when `n`'s tag
// matches `T::kind`, else nullptr. `T` is a sum subtype; `Node` is
// its sum base.
template <class T, class Node>
inline T* as(Node* n) {
    return (n && n->tag == T::kind) ? static_cast<T*>(n) : nullptr;
}

// ── Sum types (tag-dispatched hierarchies) ──────────────────

struct StackParam : public ASTNode {
    StackParam(ValueType ty, const char* name, std::optional<const char*> sem_expr, std::optional<SemExpr*> count)
        : ty(ty), name(name), sem_expr(std::move(sem_expr)), count(std::move(count)){}

    ValueType ty;
    const char* name;
    std::optional<const char*> sem_expr;
    std::optional<SemExpr*> count;
};

struct OperandParam : public ASTNode {
    OperandParam(ValueType ty, const char* name, bool cp_ref)
        : ty(ty), name(name), cp_ref(cp_ref){}

    ValueType ty;
    const char* name;
    bool cp_ref;
};

struct ErrorCase : public ASTNode {
    ErrorCase(std::optional<SemExpr*> condition, const char* error_name)
        : condition(std::move(condition)), error_name(error_name){}

    // The guard predicate, in the same closed action language as an opcode
    // body — this IS what gets emitted, not a description of it.
    std::optional<SemExpr*> condition;
    const char* error_name;
};

struct LocalValueDecl : public ASTNode {
    LocalValueDecl(LocalValueKind kind)
        : kind(kind){}

    LocalValueKind kind;
};

// sem_expr
enum class SemExprTag {
    SBinOp,
    SUnary,
    STernary,
    SCast,
    SIndex,
    SCall,
    SIdent,
    SInt,
    SFloat
};

struct SemExpr : public ASTNode {
    SemExprTag tag;
    explicit SemExpr(SemExprTag t) : tag(t) {}
};

struct SBinOp : public SemExpr {
    static constexpr SemExprTag kind = SemExprTag::SBinOp;

    SBinOp(const char* op, SemExpr* left, SemExpr* right)
        : SemExpr(kind), op(op), left(left), right(right){}

    const char* op;
    SemExpr* left;
    SemExpr* right;
};

struct SUnary : public SemExpr {
    static constexpr SemExprTag kind = SemExprTag::SUnary;

    SUnary(const char* op, SemExpr* operand)
        : SemExpr(kind), op(op), operand(operand){}

    const char* op;
    SemExpr* operand;
};

struct STernary : public SemExpr {
    static constexpr SemExprTag kind = SemExprTag::STernary;

    STernary(SemExpr* cond, SemExpr* then_, SemExpr* else_)
        : SemExpr(kind), cond(cond), then_(then_), else_(else_){}

    SemExpr* cond;
    SemExpr* then_;
    SemExpr* else_;
};

struct SCast : public SemExpr {
    static constexpr SemExprTag kind = SemExprTag::SCast;

    SCast(const char* ty, SemExpr* operand)
        : SemExpr(kind), ty(ty), operand(operand){}

    const char* ty;
    SemExpr* operand;
};

struct SIndex : public SemExpr {
    static constexpr SemExprTag kind = SemExprTag::SIndex;

    SIndex(SemExpr* base, SemExpr* index)
        : SemExpr(kind), base(base), index(index){}

    SemExpr* base;
    SemExpr* index;
};

struct SCall : public SemExpr {
    static constexpr SemExprTag kind = SemExprTag::SCall;

    SCall(const char* name, std::vector<SemExpr*> args)
        : SemExpr(kind), name(name), args(std::move(args)){}

    const char* name;
    std::vector<SemExpr*> args;
};

struct SIdent : public SemExpr {
    static constexpr SemExprTag kind = SemExprTag::SIdent;

    SIdent(const char* name)
        : SemExpr(kind), name(name){}

    const char* name;
};

struct SInt : public SemExpr {
    static constexpr SemExprTag kind = SemExprTag::SInt;

    SInt(int64_t value)
        : SemExpr(kind), value(value){}

    int64_t value;
};

struct SFloat : public SemExpr {
    static constexpr SemExprTag kind = SemExprTag::SFloat;

    SFloat(double value)
        : SemExpr(kind), value(value){}

    double value;
};

// sem_stmt
enum class SemStmtTag {
    SAssign,
    SExprStmt,
    SLocalDecl,
    SIf,
    SWhile,
    SFor,
    SBlock,
    STrap
};

struct SemStmt : public ASTNode {
    SemStmtTag tag;
    explicit SemStmt(SemStmtTag t) : tag(t) {}
};

struct SAssign : public SemStmt {
    static constexpr SemStmtTag kind = SemStmtTag::SAssign;

    SAssign(SemExpr* target, SemExpr* value)
        : SemStmt(kind), target(target), value(value){}

    SemExpr* target;
    SemExpr* value;
};

struct SExprStmt : public SemStmt {
    static constexpr SemStmtTag kind = SemStmtTag::SExprStmt;

    SExprStmt(SemExpr* value)
        : SemStmt(kind), value(value){}

    SemExpr* value;
};

struct SLocalDecl : public SemStmt {
    static constexpr SemStmtTag kind = SemStmtTag::SLocalDecl;

    SLocalDecl(const char* ty, const char* name, std::optional<SemExpr*> init)
        : SemStmt(kind), ty(ty), name(name), init(std::move(init)){}

    const char* ty;
    const char* name;
    std::optional<SemExpr*> init;
};

struct SIf : public SemStmt {
    static constexpr SemStmtTag kind = SemStmtTag::SIf;

    SIf(SemExpr* cond, SemStmt* then_, std::optional<SemStmt*> else_)
        : SemStmt(kind), cond(cond), then_(then_), else_(std::move(else_)){}

    SemExpr* cond;
    SemStmt* then_;
    std::optional<SemStmt*> else_;
};

struct SWhile : public SemStmt {
    static constexpr SemStmtTag kind = SemStmtTag::SWhile;

    SWhile(SemExpr* cond, SemStmt* body)
        : SemStmt(kind), cond(cond), body(body){}

    SemExpr* cond;
    SemStmt* body;
};

struct SFor : public SemStmt {
    static constexpr SemStmtTag kind = SemStmtTag::SFor;

    SFor(std::optional<SemStmt*> init, std::optional<SemExpr*> cond, std::optional<SemStmt*> update, SemStmt* body)
        : SemStmt(kind), init(std::move(init)), cond(std::move(cond)), update(std::move(update)), body(body){}

    std::optional<SemStmt*> init;
    std::optional<SemExpr*> cond;
    std::optional<SemStmt*> update;
    SemStmt* body;
};

struct SBlock : public SemStmt {
    static constexpr SemStmtTag kind = SemStmtTag::SBlock;

    SBlock(std::vector<SemStmt*> stmts)
        : SemStmt(kind), stmts(std::move(stmts)){}

    std::vector<SemStmt*> stmts;
};

struct STrap : public SemStmt {
    static constexpr SemStmtTag kind = SemStmtTag::STrap;

    STrap()
        : SemStmt(kind){}

};

struct MethodParam : public ASTNode {
    MethodParam(const char* ty, const char* name)
        : ty(ty), name(name){}

    const char* ty;
    const char* name;
};

struct MethodDecl : public ASTNode {
    MethodDecl(bool native, bool inl, const char* ret_ty, const char* name, std::vector<MethodParam*> params, std::vector<SemStmt*> body)
        : native(native), inl(inl), ret_ty(ret_ty), name(name), params(std::move(params)), body(std::move(body)){}

    bool native;
    bool inl;
    const char* ret_ty;
    const char* name;
    std::vector<MethodParam*> params;
    std::vector<SemStmt*> body;
};

struct StatusDecl : public ASTNode {
    StatusDecl(const char* type, const char* ok, const char* err, const char* halt, std::vector<const char*> extra)
        : type(type), ok(ok), err(err), halt(halt), extra(std::move(extra)){}

    const char* type;
    const char* ok;
    const char* err;
    const char* halt;
    std::vector<const char*> extra;
};

struct TypeDecl : public ASTNode {
    TypeDecl(ValueType ty, const char* scalar, const char* uscalar, int32_t slots, const char* field)
        : ty(ty), scalar(scalar), uscalar(uscalar), slots(slots), field(field){}

    ValueType ty;
    const char* scalar;
    const char* uscalar;
    int32_t slots;
    const char* field;
};

struct Opcode : public ASTNode {
    Opcode(const char* mnemonic, int32_t opcode_val, int32_t subop, std::vector<OperandParam*> operands, std::vector<StackParam*> stack_in, std::vector<StackParam*> stack_out, std::vector<ErrorCase*> errors, std::vector<const char*> flags, std::optional<LocalValueDecl*> local_value, std::vector<SemStmt*> sem_body)
        : mnemonic(mnemonic), opcode_val(opcode_val), subop(subop), operands(std::move(operands)), stack_in(std::move(stack_in)), stack_out(std::move(stack_out)), errors(std::move(errors)), flags(std::move(flags)), local_value(std::move(local_value)), sem_body(std::move(sem_body)){}

    const char* mnemonic;
    int32_t opcode_val;
    int32_t subop;
    std::vector<OperandParam*> operands;
    std::vector<StackParam*> stack_in;
    std::vector<StackParam*> stack_out;
    std::vector<ErrorCase*> errors;
    std::vector<const char*> flags;
    std::optional<LocalValueDecl*> local_value;
    std::vector<SemStmt*> sem_body;
};

struct Module : public ASTNode {
    Module(std::vector<Opcode*> opcodes, std::vector<MethodDecl*> methods, std::optional<StatusDecl*> status, std::vector<TypeDecl*> types, std::vector<const char*> headers, std::optional<const char*> body_c, std::optional<const char*> backend)
        : opcodes(std::move(opcodes)), methods(std::move(methods)), status(std::move(status)), types(std::move(types)), headers(std::move(headers)), body_c(std::move(body_c)), backend(std::move(backend)){}

    std::vector<Opcode*> opcodes;
    std::vector<MethodDecl*> methods;
    std::optional<StatusDecl*> status;
    std::vector<TypeDecl*> types;
    std::vector<const char*> headers;
    std::optional<const char*> body_c;
    std::optional<const char*> backend;
};

// ── Product types ───────────────────────────────────────────

} // namespace opgen

#endif // OPGEN_AST_H
