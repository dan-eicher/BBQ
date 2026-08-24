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

enum class CodeSection {
    HeaderBlock,
    SourceBlock,
    WriterBlock
};

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

enum class Encoding {
    Fixed,
    Uleb,
    Sleb
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
struct CodeBlock;
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
struct CodeBlock;
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
struct RangeValue;
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
struct Builtin;
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
    virtual void visit(CodeBlock* node) {}
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
    virtual void visit(RangeValue* node) {}
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
    virtual void visit(Builtin* node) {}
    virtual void visit(Simple* node) {}
    virtual void visit(FieldAcc* node) {}
    virtual void visit(IndexAcc* node) {}
};

// ── Node kind tag ───────────────────────────────────────────
//
// One value per node type, so consumers can `switch (n->kind())` instead of a
// dynamic_cast chain or a one-arm-per-type visitor. (The visitor stays for the
// cases that genuinely want per-type double dispatch.)

enum class NodeKind {
    Grammar,
    EndianDirective,
    CodeBlock,
    Rule,
    Alternatives,
    Struct,
    Union,
    Array,
    Optional,
    Switch,
    Compute,
    Primitive,
    RuleRef,
    Extern,
    Bitfield,
    EndianSwitch,
    BitfieldEntry,
    Field,
    Variant,
    FixedCount,
    SepTerm,
    TypeSep,
    NoneSep,
    EofTerm,
    CountTerm,
    UntilTerm,
    TypeTerm,
    SwitchCase,
    SwitchDefault,
    IntValue,
    RangeValue,
    StrValue,
    IdentValue,
    RefValue,
    StartEnd,
    Length,
    IntegerKind,
    FloatKind,
    BytesKind,
    StringKind,
    BoolKind,
    BinOp,
    UnaryOp,
    Ternary,
    IntLit,
    FloatLit,
    StrLit,
    BoolLit,
    Ref,
    Call,
    EndianLit,
    EOI,
    Builtin,
    Simple,
    FieldAcc,
    IndexAcc,
};

// ── AST node base ───────────────────────────────────────────

struct ASTNode {
    SourceLoc loc;

    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor& visitor) = 0;
    virtual NodeKind node_kind() const = 0;

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
        std::vector<Rule*> rules,
        std::vector<CodeBlock*> codes
    )
        : directives(std::move(directives)), rules(std::move(rules)), codes(std::move(codes)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Grammar; }

    std::vector<EndianDirective*> directives;
    std::vector<Rule*> rules;
    std::vector<CodeBlock*> codes;
};

struct EndianDirective : public ASTNode {
    EndianDirective(
        Endianness endian
    )
        : endian(endian) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::EndianDirective; }

    Endianness endian;
};

struct CodeBlock : public ASTNode {
    CodeBlock(
        CodeSection section,
        std::string code
    )
        : section(section), code(code) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::CodeBlock; }

    CodeSection section;
    std::string code;
};

struct Rule : public ASTNode {
    Rule(
        std::string name,
        TypeExpr* body
    )
        : name(name), body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Rule; }

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
    NodeKind node_kind() const override { return NodeKind::Alternatives; }

    std::vector<TypeExpr*> alts;
};

struct Struct : public TypeExpr {
    Struct(
        std::vector<Field*> fields,
        std::optional<Expr*> constraint
    )
        : fields(std::move(fields)), constraint(std::move(constraint)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Struct; }

    std::vector<Field*> fields;
    std::optional<Expr*> constraint;
};

struct Union : public TypeExpr {
    Union(
        std::vector<Variant*> variants
    )
        : variants(std::move(variants)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Union; }

    std::vector<Variant*> variants;
};

struct Array : public TypeExpr {
    Array(
        TypeExpr* element,
        ArraySpec* spec,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval,
        std::optional<Interval*> element_interval,
        bool resync
    )
        : element(element), spec(spec), constraint(std::move(constraint)), interval(std::move(interval)), element_interval(std::move(element_interval)), resync(resync) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Array; }

    TypeExpr* element;
    ArraySpec* spec;
    std::optional<Expr*> constraint;
    std::optional<Interval*> interval;
    std::optional<Interval*> element_interval;
    bool resync;
};

struct Optional : public TypeExpr {
    Optional(
        TypeExpr* element,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval
    )
        : element(element), constraint(std::move(constraint)), interval(std::move(interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Optional; }

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
    NodeKind node_kind() const override { return NodeKind::Switch; }

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
    NodeKind node_kind() const override { return NodeKind::Compute; }

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
    NodeKind node_kind() const override { return NodeKind::Primitive; }

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
    NodeKind node_kind() const override { return NodeKind::RuleRef; }

    std::string name;
    std::optional<Expr*> constraint;
    std::optional<Interval*> interval;
};

struct Extern : public TypeExpr {
    Extern(
        std::string func_name,
        std::string write_func,
        std::string cpp_type,
        std::optional<Expr*> constraint,
        std::optional<Interval*> interval
    )
        : func_name(func_name), write_func(write_func), cpp_type(cpp_type), constraint(std::move(constraint)), interval(std::move(interval)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Extern; }

    std::string func_name;
    std::string write_func;
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
    NodeKind node_kind() const override { return NodeKind::Bitfield; }

    PrimitiveKind* container;
    std::vector<BitfieldEntry*> entries;
};

struct EndianSwitch : public TypeExpr {
    EndianSwitch(
        Expr* endian_expr
    )
        : endian_expr(endian_expr) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::EndianSwitch; }

    Expr* endian_expr;
};

struct BitfieldEntry : public ASTNode {
    BitfieldEntry(
        std::string name,
        Signedness sign,
        int64_t width,
        std::optional<Expr*> constraint
    )
        : name(name), sign(sign), width(width), constraint(std::move(constraint)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::BitfieldEntry; }

    std::string name;
    Signedness sign;
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
    NodeKind node_kind() const override { return NodeKind::Field; }

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
    NodeKind node_kind() const override { return NodeKind::Variant; }

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
    NodeKind node_kind() const override { return NodeKind::FixedCount; }

    Expr* count;
};

struct SepTerm : public ArraySpec {
    SepTerm(
        Separator* sep,
        Terminator* term
    )
        : sep(sep), term(term) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::SepTerm; }

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
    NodeKind node_kind() const override { return NodeKind::TypeSep; }

    TypeExpr* sep_type;
};

struct NoneSep : public Separator {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::NoneSep; }

};

// terminator
struct Terminator : public ASTNode {
    virtual ~Terminator() = default;
};

struct EofTerm : public Terminator {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::EofTerm; }

};

struct CountTerm : public Terminator {
    CountTerm(
        Expr* count
    )
        : count(count) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::CountTerm; }

    Expr* count;
};

struct UntilTerm : public Terminator {
    UntilTerm(
        Expr* condition
    )
        : condition(condition) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::UntilTerm; }

    Expr* condition;
};

struct TypeTerm : public Terminator {
    TypeTerm(
        TypeExpr* term_type
    )
        : term_type(term_type) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::TypeTerm; }

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
    NodeKind node_kind() const override { return NodeKind::SwitchCase; }

    CaseValue* value;
    TypeExpr* target;
    std::optional<Interval*> interval;
};

struct SwitchDefault : public ASTNode {
    SwitchDefault(
        std::optional<TypeExpr*> target,
        std::optional<Interval*> interval,
        bool is_reject
    )
        : target(std::move(target)), interval(std::move(interval)), is_reject(is_reject) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::SwitchDefault; }

    std::optional<TypeExpr*> target;
    std::optional<Interval*> interval;
    bool is_reject;
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
    NodeKind node_kind() const override { return NodeKind::IntValue; }

    int64_t value;
};

struct RangeValue : public CaseValue {
    RangeValue(
        int64_t lo,
        int64_t hi
    )
        : lo(lo), hi(hi) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::RangeValue; }

    int64_t lo;
    int64_t hi;
};

struct StrValue : public CaseValue {
    StrValue(
        std::string value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::StrValue; }

    std::string value;
};

struct IdentValue : public CaseValue {
    IdentValue(
        std::string name
    )
        : name(name) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::IdentValue; }

    std::string name;
};

struct RefValue : public CaseValue {
    RefValue(
        std::vector<std::string> path
    )
        : path(std::move(path)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::RefValue; }

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
    NodeKind node_kind() const override { return NodeKind::StartEnd; }

    Expr* start;
    Expr* end;
};

struct Length : public Interval {
    Length(
        Expr* length
    )
        : length(length) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Length; }

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
        Endianness endian,
        Encoding encoding
    )
        : width(width), sign(sign), endian(endian), encoding(encoding) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::IntegerKind; }

    Width width;
    Signedness sign;
    Endianness endian;
    Encoding encoding;
};

struct FloatKind : public PrimitiveKind {
    FloatKind(
        Width width,
        Endianness endian
    )
        : width(width), endian(endian) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::FloatKind; }

    Width width;
    Endianness endian;
};

struct BytesKind : public PrimitiveKind {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::BytesKind; }

};

struct StringKind : public PrimitiveKind {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::StringKind; }

};

struct BoolKind : public PrimitiveKind {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::BoolKind; }

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
    NodeKind node_kind() const override { return NodeKind::BinOp; }

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
    NodeKind node_kind() const override { return NodeKind::UnaryOp; }

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
    NodeKind node_kind() const override { return NodeKind::Ternary; }

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
    NodeKind node_kind() const override { return NodeKind::IntLit; }

    int64_t value;
};

struct FloatLit : public Expr {
    FloatLit(
        float value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::FloatLit; }

    float value;
};

struct StrLit : public Expr {
    StrLit(
        std::string value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::StrLit; }

    std::string value;
};

struct BoolLit : public Expr {
    BoolLit(
        bool value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::BoolLit; }

    bool value;
};

struct Ref : public Expr {
    Ref(
        RefPath* path
    )
        : path(path) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Ref; }

    RefPath* path;
};

struct Call : public Expr {
    Call(
        std::string func,
        std::vector<Expr*> args
    )
        : func(func), args(std::move(args)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Call; }

    std::string func;
    std::vector<Expr*> args;
};

struct EndianLit : public Expr {
    EndianLit(
        Endianness value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::EndianLit; }

    Endianness value;
};

struct EOI : public Expr {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::EOI; }

};

struct Builtin : public Expr {
    Builtin(
        std::string name
    )
        : name(name) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
    NodeKind node_kind() const override { return NodeKind::Builtin; }

    std::string name;
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
    NodeKind node_kind() const override { return NodeKind::Simple; }

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
    NodeKind node_kind() const override { return NodeKind::FieldAcc; }

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
    NodeKind node_kind() const override { return NodeKind::IndexAcc; }

    RefPath* base;
    Expr* index;
};

// ── Product types ───────────────────────────────────────────

} // namespace BBQ

#endif // BBQ_AST_H
