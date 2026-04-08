// ============================================================
// BBQ AST — Auto-generated from BBQ.asdl
//
// Do not edit by hand. Re-generate with:
//   ./asdl -i BBQ.asdl -t templates/bbq_ast.inja -o BBQ_AST.h
// ============================================================
#ifndef BBQ_AST_H
#define BBQ_AST_H

#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace BBQ {

// ── Enum types ──────────────────────────────────────────────

enum class Width {
    W8,
    W16,
    W32,
    W64
};

enum class Signedness {
    Unsigned,
    Signed
};

enum class Endianness {
    Little,
    Big,
    Default
};

enum class Binop {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    And,
    Or,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge
};

enum class Unaryop {
    Neg,
    BitNot,
    Not,
    Abs
};

// ── Forward declarations ────────────────────────────────────

struct Grammar;
struct EndianDirective;
struct Rule;
struct TypeExpr;
struct BitfieldEntry;
struct Field;
struct Variant;
struct ArraySpec;
struct Separator;
struct Terminator;
struct SwitchCase;
struct SwitchDefault;
struct CaseValue;
struct Interval;
struct PrimitiveKind;
struct Expr;
struct RefPath;
struct Grammar;
struct EndianDirective;
struct Rule;
struct Alternatives;
struct Struct;
struct Union;
struct Array;
struct Optional;
struct Switch;
struct Compute;
struct Primitive;
struct RuleRef;
struct Extern;
struct Bitfield;
struct EndianSwitch;
struct BitfieldEntry;
struct Field;
struct Variant;
struct FixedCount;
struct SepTerm;
struct TypeSep;
struct NoneSep;
struct EofTerm;
struct CountTerm;
struct UntilTerm;
struct TypeTerm;
struct SwitchCase;
struct SwitchDefault;
struct IntValue;
struct StrValue;
struct IdentValue;
struct RefValue;
struct StartEnd;
struct Length;
struct IntegerKind;
struct FloatKind;
struct BytesKind;
struct StringKind;
struct BoolKind;
struct BinOp;
struct UnaryOp;
struct Ternary;
struct IntLit;
struct FloatLit;
struct StrLit;
struct BoolLit;
struct Ref;
struct Call;
struct EndianLit;
struct EOI;
struct Simple;
struct FieldAcc;
struct IndexAcc;

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

    virtual void visit(Grammar* node) {}
    virtual void visit(EndianDirective* node) {}
    virtual void visit(Rule* node) {}
    virtual void visit(Alternatives* node) {}
    virtual void visit(Struct* node) {}
    virtual void visit(Union* node) {}
    virtual void visit(Array* node) {}
    virtual void visit(Optional* node) {}
    virtual void visit(Switch* node) {}
    virtual void visit(Compute* node) {}
    virtual void visit(Primitive* node) {}
    virtual void visit(RuleRef* node) {}
    virtual void visit(Extern* node) {}
    virtual void visit(Bitfield* node) {}
    virtual void visit(EndianSwitch* node) {}
    virtual void visit(BitfieldEntry* node) {}
    virtual void visit(Field* node) {}
    virtual void visit(Variant* node) {}
    virtual void visit(FixedCount* node) {}
    virtual void visit(SepTerm* node) {}
    virtual void visit(TypeSep* node) {}
    virtual void visit(NoneSep* node) {}
    virtual void visit(EofTerm* node) {}
    virtual void visit(CountTerm* node) {}
    virtual void visit(UntilTerm* node) {}
    virtual void visit(TypeTerm* node) {}
    virtual void visit(SwitchCase* node) {}
    virtual void visit(SwitchDefault* node) {}
    virtual void visit(IntValue* node) {}
    virtual void visit(StrValue* node) {}
    virtual void visit(IdentValue* node) {}
    virtual void visit(RefValue* node) {}
    virtual void visit(StartEnd* node) {}
    virtual void visit(Length* node) {}
    virtual void visit(IntegerKind* node) {}
    virtual void visit(FloatKind* node) {}
    virtual void visit(BytesKind* node) {}
    virtual void visit(StringKind* node) {}
    virtual void visit(BoolKind* node) {}
    virtual void visit(BinOp* node) {}
    virtual void visit(UnaryOp* node) {}
    virtual void visit(Ternary* node) {}
    virtual void visit(IntLit* node) {}
    virtual void visit(FloatLit* node) {}
    virtual void visit(StrLit* node) {}
    virtual void visit(BoolLit* node) {}
    virtual void visit(Ref* node) {}
    virtual void visit(Call* node) {}
    virtual void visit(EndianLit* node) {}
    virtual void visit(EOI* node) {}
    virtual void visit(Simple* node) {}
    virtual void visit(FieldAcc* node) {}
    virtual void visit(IndexAcc* node) {}
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

struct Grammar : public ASTNode {
    Grammar(
        std::vector<EndianDirective*> directives,
        std::vector<Rule*> rules
    )
        : directives(std::move(directives)), rules(std::move(rules)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<EndianDirective*> directives;
    std::vector<Rule*> rules;
};

struct EndianDirective : public ASTNode {
    EndianDirective(
        Endianness endian
    )
        : endian(endian) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Endianness endian;
};

struct Rule : public ASTNode {
    Rule(
        std::string name,
        TypeExpr* body
    )
        : name(name), body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    TypeExpr* body;
};

// type_expr
struct TypeExpr : public ASTNode {
    virtual ~TypeExpr() = default;
};

struct Alternatives : public TypeExpr {
    Alternatives(
        std::vector<TypeExpr*> alts
    )
        : alts(std::move(alts)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<TypeExpr*> alts;
};

struct Struct : public TypeExpr {
    Struct(
        std::vector<Field*> fields
    )
        : fields(std::move(fields)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<Field*> fields;
};

struct Union : public TypeExpr {
    Union(
        std::vector<Variant*> variants
    )
        : variants(std::move(variants)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<Variant*> variants;
};

struct Array : public TypeExpr {
    Array(
        TypeExpr* element,
        ArraySpec* spec,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval,
        std::optional<Interval*> element_interval
    )
        : element(element), spec(spec), constraint(std::move(constraint)), interval(std::move(interval)), element_interval(std::move(element_interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    TypeExpr* element;
    ArraySpec* spec;
    std::optional<Expr*> constraint;
    std::optional<Interval*> interval;
    std::optional<Interval*> element_interval;
};

struct Optional : public TypeExpr {
    Optional(
        TypeExpr* element,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval
    )
        : element(element), constraint(std::move(constraint)), interval(std::move(interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    TypeExpr* element;
    std::optional<Expr*> constraint;
    std::optional<Interval*> interval;
};

struct Switch : public TypeExpr {
    Switch(
        Expr* discriminator,
        std::vector<SwitchCase*> cases,
        std::optional<SwitchDefault*> default_
    )
        : discriminator(discriminator), cases(std::move(cases)), default_(std::move(default_)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* discriminator;
    std::vector<SwitchCase*> cases;
    std::optional<SwitchDefault*> default_;
};

struct Compute : public TypeExpr {
    Compute(
        Expr* expression,
        PrimitiveKind* result_type
    )
        : expression(expression), result_type(result_type) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* expression;
    PrimitiveKind* result_type;
};

struct Primitive : public TypeExpr {
    Primitive(
        PrimitiveKind* kind,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval
    )
        : kind(kind), constraint(std::move(constraint)), interval(std::move(interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    PrimitiveKind* kind;
    std::optional<Expr*> constraint;
    std::optional<Interval*> interval;
};

struct RuleRef : public TypeExpr {
    RuleRef(
        std::string name,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval
    )
        : name(name), constraint(std::move(constraint)), interval(std::move(interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::optional<Expr*> constraint;
    std::optional<Interval*> interval;
};

struct Extern : public TypeExpr {
    Extern(
        std::string func_name,
        std::string cpp_type,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval
    )
        : func_name(func_name), cpp_type(cpp_type), constraint(std::move(constraint)), interval(std::move(interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string func_name;
    std::string cpp_type;
    std::optional<Expr*> constraint;
    std::optional<Interval*> interval;
};

struct Bitfield : public TypeExpr {
    Bitfield(
        PrimitiveKind* container,
        std::vector<BitfieldEntry*> entries
    )
        : container(container), entries(std::move(entries)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    PrimitiveKind* container;
    std::vector<BitfieldEntry*> entries;
};

struct EndianSwitch : public TypeExpr {
    EndianSwitch(
        Expr* endian_expr
    )
        : endian_expr(endian_expr) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* endian_expr;
};

struct BitfieldEntry : public ASTNode {
    BitfieldEntry(
        std::string name,
        int64_t width,
        std::optional<Expr*> constraint
    )
        : name(name), width(width), constraint(std::move(constraint)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    int64_t width;
    std::optional<Expr*> constraint;
};

struct Field : public ASTNode {
    Field(
        std::string name,
        TypeExpr* body,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval,
        bool scopes_rest
    )
        : name(name), body(body), constraint(std::move(constraint)), interval(std::move(interval)), scopes_rest(scopes_rest) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    TypeExpr* body;
    std::optional<Expr*> constraint;
    std::optional<Interval*> interval;
    bool scopes_rest;
};

struct Variant : public ASTNode {
    Variant(
        std::string name,
        TypeExpr* body,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval
    )
        : name(name), body(body), constraint(std::move(constraint)), interval(std::move(interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    TypeExpr* body;
    std::optional<Expr*> constraint;
    std::optional<Interval*> interval;
};

// array_spec
struct ArraySpec : public ASTNode {
    virtual ~ArraySpec() = default;
};

struct FixedCount : public ArraySpec {
    FixedCount(
        Expr* count
    )
        : count(count) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* count;
};

struct SepTerm : public ArraySpec {
    SepTerm(
        Separator* sep,
        Terminator* term
    )
        : sep(sep), term(term) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Separator* sep;
    Terminator* term;
};

// separator
struct Separator : public ASTNode {
    virtual ~Separator() = default;
};

struct TypeSep : public Separator {
    TypeSep(
        TypeExpr* sep_type
    )
        : sep_type(sep_type) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    TypeExpr* sep_type;
};

struct NoneSep : public Separator {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

// terminator
struct Terminator : public ASTNode {
    virtual ~Terminator() = default;
};

struct EofTerm : public Terminator {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct CountTerm : public Terminator {
    CountTerm(
        Expr* count
    )
        : count(count) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* count;
};

struct UntilTerm : public Terminator {
    UntilTerm(
        Expr* condition
    )
        : condition(condition) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* condition;
};

struct TypeTerm : public Terminator {
    TypeTerm(
        TypeExpr* term_type
    )
        : term_type(term_type) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    TypeExpr* term_type;
};

struct SwitchCase : public ASTNode {
    SwitchCase(
        CaseValue* value,
        TypeExpr* target,
        std::optional<Interval*> interval
    )
        : value(value), target(target), interval(std::move(interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    CaseValue* value;
    TypeExpr* target;
    std::optional<Interval*> interval;
};

struct SwitchDefault : public ASTNode {
    SwitchDefault(
        TypeExpr* target,
        std::optional<Interval*> interval
    )
        : target(target), interval(std::move(interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    TypeExpr* target;
    std::optional<Interval*> interval;
};

// case_value
struct CaseValue : public ASTNode {
    virtual ~CaseValue() = default;
};

struct IntValue : public CaseValue {
    IntValue(
        int64_t value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t value;
};

struct StrValue : public CaseValue {
    StrValue(
        std::string value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string value;
};

struct IdentValue : public CaseValue {
    IdentValue(
        std::string name
    )
        : name(name) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct RefValue : public CaseValue {
    RefValue(
        std::vector<std::string> path
    )
        : path(std::move(path)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<std::string> path;
};

// interval
struct Interval : public ASTNode {
    virtual ~Interval() = default;
};

struct StartEnd : public Interval {
    StartEnd(
        Expr* start,
        Expr* end
    )
        : start(start), end(end) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* start;
    Expr* end;
};

struct Length : public Interval {
    Length(
        Expr* length
    )
        : length(length) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* length;
};

// primitive_kind
struct PrimitiveKind : public ASTNode {
    virtual ~PrimitiveKind() = default;
};

struct IntegerKind : public PrimitiveKind {
    IntegerKind(
        Width width,
        Signedness sign,
        Endianness endian
    )
        : width(width), sign(sign), endian(endian) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Width width;
    Signedness sign;
    Endianness endian;
};

struct FloatKind : public PrimitiveKind {
    FloatKind(
        Width width,
        Endianness endian
    )
        : width(width), endian(endian) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Width width;
    Endianness endian;
};

struct BytesKind : public PrimitiveKind {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct StringKind : public PrimitiveKind {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct BoolKind : public PrimitiveKind {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

// expr
struct Expr : public ASTNode {
    virtual ~Expr() = default;
};

struct BinOp : public Expr {
    BinOp(
        Expr* left,
        Binop op,
        Expr* right
    )
        : left(left), op(op), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* left;
    Binop op;
    Expr* right;
};

struct UnaryOp : public Expr {
    UnaryOp(
        Unaryop op,
        Expr* operand
    )
        : op(op), operand(operand) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Unaryop op;
    Expr* operand;
};

struct Ternary : public Expr {
    Ternary(
        Expr* cond,
        Expr* then_branch,
        Expr* else_branch
    )
        : cond(cond), then_branch(then_branch), else_branch(else_branch) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Expr* cond;
    Expr* then_branch;
    Expr* else_branch;
};

struct IntLit : public Expr {
    IntLit(
        int64_t value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int64_t value;
};

struct FloatLit : public Expr {
    FloatLit(
        float value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    float value;
};

struct StrLit : public Expr {
    StrLit(
        std::string value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string value;
};

struct BoolLit : public Expr {
    BoolLit(
        bool value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    bool value;
};

struct Ref : public Expr {
    Ref(
        RefPath* path
    )
        : path(path) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    RefPath* path;
};

struct Call : public Expr {
    Call(
        std::string func,
        std::vector<Expr*> args
    )
        : func(func), args(std::move(args)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string func;
    std::vector<Expr*> args;
};

struct EndianLit : public Expr {
    EndianLit(
        Endianness value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Endianness value;
};

struct EOI : public Expr {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

// ref_path
struct RefPath : public ASTNode {
    virtual ~RefPath() = default;
};

struct Simple : public RefPath {
    Simple(
        std::string name,
        std::optional<std::string> resolved_path,
        std::optional<std::string> resolved_parent,
        int64_t resolved_depth
    )
        : name(name), resolved_path(std::move(resolved_path)), resolved_parent(std::move(resolved_parent)), resolved_depth(resolved_depth) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::optional<std::string> resolved_path;
    std::optional<std::string> resolved_parent;
    int64_t resolved_depth;
};

struct FieldAcc : public RefPath {
    FieldAcc(
        RefPath* base,
        std::string field
    )
        : base(base), field(field) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    RefPath* base;
    std::string field;
};

struct IndexAcc : public RefPath {
    IndexAcc(
        RefPath* base,
        Expr* index
    )
        : base(base), index(index) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    RefPath* base;
    Expr* index;
};

// ── Product types ───────────────────────────────────────────

} // namespace BBQ

#endif // BBQ_AST_H
