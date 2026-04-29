// ============================================================
// Grammar AST — Auto-generated from grammar.asdl
//
// Do not edit by hand. Re-generate with:
//   ./asdl -i grammar.asdl -t templates/grammar_ast.inja -o GrammarAST.h
// ============================================================
#ifndef DDCGAST_AST_H
#define DDCGAST_AST_H

#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace DdcgAst {

// ── Enum types ──────────────────────────────────────────────

// ── Forward declarations ────────────────────────────────────

struct File;
struct Header;
struct Import;
struct DestDecl;
struct DestVariant;
struct DestField;
struct Aux;
struct Pred;
struct TypeRef;
struct Rule;
struct Pattern;
struct FieldPat;
struct Stmt;
struct Bind;
struct Expr;
struct File;
struct Header;
struct Import;
struct DataDest;
struct CtrlDest;
struct DestVariant;
struct DestField;
struct Aux;
struct Pred;
struct TypeName;
struct TypeList;
struct TypeTuple;
struct Rule;
struct ConstructorPat;
struct BindPat;
struct WildcardPat;
struct NilPat;
struct IntPat;
struct StringPat;
struct NamedFieldPat;
struct RestFieldPat;
struct LetStmt;
struct MutAssign;
struct ForStmt;
struct LabelStmt;
struct ExprStmt;
struct SimpleBind;
struct WildcardBind;
struct TernaryExpr;
struct BinOp;
struct UnaryOp;
struct CallExpr;
struct FieldAccExpr;
struct IndexAccExpr;
struct IdentExpr;
struct IntLit;
struct StringLit;
struct BoolLit;
struct NilLit;
struct ListLit;
struct TupleLit;
struct DestPattern;
struct GenCall;

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

    virtual void visit(File* node) {}
    virtual void visit(Header* node) {}
    virtual void visit(Import* node) {}
    virtual void visit(DataDest* node) {}
    virtual void visit(CtrlDest* node) {}
    virtual void visit(DestVariant* node) {}
    virtual void visit(DestField* node) {}
    virtual void visit(Aux* node) {}
    virtual void visit(Pred* node) {}
    virtual void visit(TypeName* node) {}
    virtual void visit(TypeList* node) {}
    virtual void visit(TypeTuple* node) {}
    virtual void visit(Rule* node) {}
    virtual void visit(ConstructorPat* node) {}
    virtual void visit(BindPat* node) {}
    virtual void visit(WildcardPat* node) {}
    virtual void visit(NilPat* node) {}
    virtual void visit(IntPat* node) {}
    virtual void visit(StringPat* node) {}
    virtual void visit(NamedFieldPat* node) {}
    virtual void visit(RestFieldPat* node) {}
    virtual void visit(LetStmt* node) {}
    virtual void visit(MutAssign* node) {}
    virtual void visit(ForStmt* node) {}
    virtual void visit(LabelStmt* node) {}
    virtual void visit(ExprStmt* node) {}
    virtual void visit(SimpleBind* node) {}
    virtual void visit(WildcardBind* node) {}
    virtual void visit(TernaryExpr* node) {}
    virtual void visit(BinOp* node) {}
    virtual void visit(UnaryOp* node) {}
    virtual void visit(CallExpr* node) {}
    virtual void visit(FieldAccExpr* node) {}
    virtual void visit(IndexAccExpr* node) {}
    virtual void visit(IdentExpr* node) {}
    virtual void visit(IntLit* node) {}
    virtual void visit(StringLit* node) {}
    virtual void visit(BoolLit* node) {}
    virtual void visit(NilLit* node) {}
    virtual void visit(ListLit* node) {}
    virtual void visit(TupleLit* node) {}
    virtual void visit(DestPattern* node) {}
    virtual void visit(GenCall* node) {}
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

struct File : public ASTNode {
    File(
        std::string compiler_name,
        std::vector<Header*> headers,
        std::vector<Import*> imports,
        std::vector<DestDecl*> destinations,
        TypeRef* ir_root,
        std::vector<Aux*> auxiliaries,
        std::vector<Pred*> predicates,
        std::string members,
        std::string private_helpers,
        std::vector<std::string> desugared_by_normalization,
        std::vector<Rule*> rules
    )
        : compiler_name(compiler_name), headers(std::move(headers)), imports(std::move(imports)), destinations(std::move(destinations)), ir_root(ir_root), auxiliaries(std::move(auxiliaries)), predicates(std::move(predicates)), members(members), private_helpers(private_helpers), desugared_by_normalization(std::move(desugared_by_normalization)), rules(std::move(rules)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string compiler_name;
    std::vector<Header*> headers;
    std::vector<Import*> imports;
    std::vector<DestDecl*> destinations;
    TypeRef* ir_root;
    std::vector<Aux*> auxiliaries;
    std::vector<Pred*> predicates;
    std::string members;
    std::string private_helpers;
    std::vector<std::string> desugared_by_normalization;
    std::vector<Rule*> rules;
};

struct Header : public ASTNode {
    Header(
        std::string code
    )
        : code(code){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string code;
};

struct Import : public ASTNode {
    Import(
        std::string name,
        std::string path
    )
        : name(name), path(path){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::string path;
};

// dest_decl
struct DestDecl : public ASTNode {
    virtual ~DestDecl() = default;
};

struct DataDest : public DestDecl {
    DataDest(
        std::string name,
        std::vector<DestVariant*> variants
    )
        : name(name), variants(std::move(variants)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<DestVariant*> variants;
};

struct CtrlDest : public DestDecl {
    CtrlDest(
        std::string name,
        std::vector<DestVariant*> variants
    )
        : name(name), variants(std::move(variants)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<DestVariant*> variants;
};

struct DestVariant : public ASTNode {
    DestVariant(
        std::string name,
        std::vector<DestField*> fields
    )
        : name(name), fields(std::move(fields)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<DestField*> fields;
};

struct DestField : public ASTNode {
    DestField(
        std::string name,
        TypeRef* ty
    )
        : name(name), ty(ty){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    TypeRef* ty;
};

struct Aux : public ASTNode {
    Aux(
        std::string name,
        std::vector<TypeRef*> param_types,
        TypeRef* return_ty
    )
        : name(name), param_types(std::move(param_types)), return_ty(return_ty){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<TypeRef*> param_types;
    TypeRef* return_ty;
};

struct Pred : public ASTNode {
    Pred(
        std::string name,
        std::vector<TypeRef*> param_types
    )
        : name(name), param_types(std::move(param_types)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<TypeRef*> param_types;
};

// type_ref
struct TypeRef : public ASTNode {
    virtual ~TypeRef() = default;
};

struct TypeName : public TypeRef {
    TypeName(
        std::string name,
        bool is_pointer
    )
        : name(name), is_pointer(is_pointer){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    bool is_pointer;
};

struct TypeList : public TypeRef {
    TypeList(
        TypeRef* elem
    )
        : elem(elem){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    TypeRef* elem;
};

struct TypeTuple : public TypeRef {
    TypeTuple(
        std::vector<TypeRef*> elems
    )
        : elems(std::move(elems)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<TypeRef*> elems;
};

struct Rule : public ASTNode {
    Rule(
        std::vector<Pattern*> heads,
        std::optional<Expr*> guard,
        std::vector<Stmt*> body
    )
        : heads(std::move(heads)), guard(std::move(guard)), body(std::move(body)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<Pattern*> heads;
    std::optional<Expr*> guard;
    std::vector<Stmt*> body;
};

// pattern
struct Pattern : public ASTNode {
    virtual ~Pattern() = default;
};

struct ConstructorPat : public Pattern {
    ConstructorPat(
        std::string module,
        std::string ctor,
        std::vector<FieldPat*> fields
    )
        : module(module), ctor(ctor), fields(std::move(fields)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string module;
    std::string ctor;
    std::vector<FieldPat*> fields;
};

struct BindPat : public Pattern {
    BindPat(
        std::string name
    )
        : name(name){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct WildcardPat : public Pattern {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct NilPat : public Pattern {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct IntPat : public Pattern {
    IntPat(
        int value
    )
        : value(value){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int value;
};

struct StringPat : public Pattern {
    StringPat(
        std::string value
    )
        : value(value){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string value;
};

// field_pat
struct FieldPat : public ASTNode {
    virtual ~FieldPat() = default;
};

struct NamedFieldPat : public FieldPat {
    NamedFieldPat(
        std::string name,
        Pattern* pat
    )
        : name(name), pat(pat){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    Pattern* pat;
};

struct RestFieldPat : public FieldPat {
    RestFieldPat(
        std::string name
    )
        : name(name){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

// stmt
struct Stmt : public ASTNode {
    virtual ~Stmt() = default;
};

struct LetStmt : public Stmt {
    LetStmt(
        std::vector<Bind*> binds,
        Expr* value
    )
        : binds(std::move(binds)), value(value){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<Bind*> binds;
    Expr* value;
};

struct MutAssign : public Stmt {
    MutAssign(
        std::string name,
        Expr* value
    )
        : name(name), value(value){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    Expr* value;
};

struct ForStmt : public Stmt {
    ForStmt(
        Bind* iter,
        Expr* seq,
        std::vector<Stmt*> body
    )
        : iter(iter), seq(seq), body(std::move(body)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Bind* iter;
    Expr* seq;
    std::vector<Stmt*> body;
};

struct LabelStmt : public Stmt {
    LabelStmt(
        std::string name
    )
        : name(name){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct ExprStmt : public Stmt {
    ExprStmt(
        Expr* value
    )
        : value(value){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* value;
};

// bind
struct Bind : public ASTNode {
    virtual ~Bind() = default;
};

struct SimpleBind : public Bind {
    SimpleBind(
        std::string name,
        std::optional<TypeRef*> ty
    )
        : name(name), ty(std::move(ty)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::optional<TypeRef*> ty;
};

struct WildcardBind : public Bind {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

// expr
struct Expr : public ASTNode {
    virtual ~Expr() = default;
};

struct TernaryExpr : public Expr {
    TernaryExpr(
        Expr* cond,
        Expr* then_,
        Expr* else_
    )
        : cond(cond), then_(then_), else_(else_){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* cond;
    Expr* then_;
    Expr* else_;
};

struct BinOp : public Expr {
    BinOp(
        std::string op,
        Expr* left,
        Expr* right
    )
        : op(op), left(left), right(right){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string op;
    Expr* left;
    Expr* right;
};

struct UnaryOp : public Expr {
    UnaryOp(
        std::string op,
        Expr* operand
    )
        : op(op), operand(operand){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string op;
    Expr* operand;
};

struct CallExpr : public Expr {
    CallExpr(
        Expr* fn,
        std::vector<Expr*> args
    )
        : fn(fn), args(std::move(args)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* fn;
    std::vector<Expr*> args;
};

struct FieldAccExpr : public Expr {
    FieldAccExpr(
        Expr* base,
        std::string field
    )
        : base(base), field(field){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* base;
    std::string field;
};

struct IndexAccExpr : public Expr {
    IndexAccExpr(
        Expr* base,
        Expr* index
    )
        : base(base), index(index){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* base;
    Expr* index;
};

struct IdentExpr : public Expr {
    IdentExpr(
        std::string name
    )
        : name(name){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct IntLit : public Expr {
    IntLit(
        int value
    )
        : value(value){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int value;
};

struct StringLit : public Expr {
    StringLit(
        std::string value
    )
        : value(value){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string value;
};

struct BoolLit : public Expr {
    BoolLit(
        bool value
    )
        : value(value){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    bool value;
};

struct NilLit : public Expr {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct ListLit : public Expr {
    ListLit(
        std::vector<Expr*> elems
    )
        : elems(std::move(elems)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<Expr*> elems;
};

struct TupleLit : public Expr {
    TupleLit(
        std::vector<Expr*> elems
    )
        : elems(std::move(elems)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<Expr*> elems;
};

struct DestPattern : public Expr {
    DestPattern(
        std::string name,
        std::vector<Expr*> args
    )
        : name(name), args(std::move(args)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<Expr*> args;
};

struct GenCall : public Expr {
    GenCall(
        Expr* subject,
        Expr* data_dest,
        Expr* ctrl_dest,
        std::optional<Expr*> lnext
    )
        : subject(subject), data_dest(data_dest), ctrl_dest(ctrl_dest), lnext(std::move(lnext)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* subject;
    Expr* data_dest;
    Expr* ctrl_dest;
    std::optional<Expr*> lnext;
};

// ── Product types ───────────────────────────────────────────

} // namespace DdcgAst

#endif // DDCGAST_AST_H
