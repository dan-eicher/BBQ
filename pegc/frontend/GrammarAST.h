// ============================================================
// Grammar AST — Auto-generated from grammar.asdl
//
// Do not edit by hand. Re-generate with:
//   ./asdl -i grammar.asdl -t templates/grammar_ast.inja -o GrammarAST.h
// ============================================================
#ifndef PEGGRAMMAR_AST_H
#define PEGGRAMMAR_AST_H

#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace PegGrammar {

// ── Enum types ──────────────────────────────────────────────

// ── Forward declarations ────────────────────────────────────

struct Grammar;
struct Header;
struct CharsetDef;
struct CharsetExpr;
struct TokenDef;
struct TokenExpr;
struct CommentDef;
struct Production;
struct Param;
struct PegExpr;
struct Arg;
struct Grammar;
struct Header;
struct CharsetDef;
struct Range;
struct Single;
struct Union;
struct Diff;
struct NameRef;
struct Any;
struct TokenDef;
struct TLiteral;
struct TCharset;
struct TSequence;
struct TAlternation;
struct TStar;
struct TPlus;
struct TOptional;
struct TRange;
struct CommentDef;
struct Production;
struct Param;
struct Literal;
struct CharsetRef;
struct TokenRef;
struct RuleCall;
struct Sequence;
struct Choice;
struct Star;
struct Plus;
struct Optional;
struct And;
struct Not;
struct Group;
struct Action;
struct Resolver;
struct AnyExpr;
struct Arg;

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
    virtual void visit(Header* node) {}
    virtual void visit(CharsetDef* node) {}
    virtual void visit(Range* node) {}
    virtual void visit(Single* node) {}
    virtual void visit(Union* node) {}
    virtual void visit(Diff* node) {}
    virtual void visit(NameRef* node) {}
    virtual void visit(Any* node) {}
    virtual void visit(TokenDef* node) {}
    virtual void visit(TLiteral* node) {}
    virtual void visit(TCharset* node) {}
    virtual void visit(TSequence* node) {}
    virtual void visit(TAlternation* node) {}
    virtual void visit(TStar* node) {}
    virtual void visit(TPlus* node) {}
    virtual void visit(TOptional* node) {}
    virtual void visit(TRange* node) {}
    virtual void visit(CommentDef* node) {}
    virtual void visit(Production* node) {}
    virtual void visit(Param* node) {}
    virtual void visit(Literal* node) {}
    virtual void visit(CharsetRef* node) {}
    virtual void visit(TokenRef* node) {}
    virtual void visit(RuleCall* node) {}
    virtual void visit(Sequence* node) {}
    virtual void visit(Choice* node) {}
    virtual void visit(Star* node) {}
    virtual void visit(Plus* node) {}
    virtual void visit(Optional* node) {}
    virtual void visit(And* node) {}
    virtual void visit(Not* node) {}
    virtual void visit(Group* node) {}
    virtual void visit(Action* node) {}
    virtual void visit(Resolver* node) {}
    virtual void visit(AnyExpr* node) {}
    virtual void visit(Arg* node) {}
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
        std::string name,
        std::vector<Header*> headers,
        std::vector<CharsetDef*> charsets,
        std::vector<TokenDef*> tokens,
        std::vector<CommentDef*> comments,
        bool has_ignore,
        std::optional<CharsetExpr*> ignore,
        std::vector<Production*> productions
    )
        : name(name), headers(std::move(headers)), charsets(std::move(charsets)), tokens(std::move(tokens)), comments(std::move(comments)), has_ignore(has_ignore), ignore(std::move(ignore)), productions(std::move(productions)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<Header*> headers;
    std::vector<CharsetDef*> charsets;
    std::vector<TokenDef*> tokens;
    std::vector<CommentDef*> comments;
    bool has_ignore;
    std::optional<CharsetExpr*> ignore;
    std::vector<Production*> productions;
};

struct Header : public ASTNode {
    Header(
        std::string code
    )
        : code(code) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string code;
};

struct CharsetDef : public ASTNode {
    CharsetDef(
        std::string name,
        CharsetExpr* body
    )
        : name(name), body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    CharsetExpr* body;
};

// charset_expr
struct CharsetExpr : public ASTNode {
    virtual ~CharsetExpr() = default;
};

struct Range : public CharsetExpr {
    Range(
        int from,
        int to
    )
        : from(from), to(to) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int from;
    int to;
};

struct Single : public CharsetExpr {
    Single(
        int ch
    )
        : ch(ch) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int ch;
};

struct Union : public CharsetExpr {
    Union(
        CharsetExpr* left,
        CharsetExpr* right
    )
        : left(left), right(right) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    CharsetExpr* left;
    CharsetExpr* right;
};

struct Diff : public CharsetExpr {
    Diff(
        CharsetExpr* base,
        CharsetExpr* minus
    )
        : base(base), minus(minus) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    CharsetExpr* base;
    CharsetExpr* minus;
};

struct NameRef : public CharsetExpr {
    NameRef(
        std::string name
    )
        : name(name) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct Any : public CharsetExpr {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct TokenDef : public ASTNode {
    TokenDef(
        std::string name,
        TokenExpr* pattern
    )
        : name(name), pattern(pattern) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    TokenExpr* pattern;
};

// token_expr
struct TokenExpr : public ASTNode {
    virtual ~TokenExpr() = default;
};

struct TLiteral : public TokenExpr {
    TLiteral(
        std::string value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string value;
};

struct TCharset : public TokenExpr {
    TCharset(
        std::string name
    )
        : name(name) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct TSequence : public TokenExpr {
    TSequence(
        std::vector<TokenExpr*> elements
    )
        : elements(std::move(elements)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<TokenExpr*> elements;
};

struct TAlternation : public TokenExpr {
    TAlternation(
        std::vector<TokenExpr*> alternatives
    )
        : alternatives(std::move(alternatives)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<TokenExpr*> alternatives;
};

struct TStar : public TokenExpr {
    TStar(
        TokenExpr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    TokenExpr* body;
};

struct TPlus : public TokenExpr {
    TPlus(
        TokenExpr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    TokenExpr* body;
};

struct TOptional : public TokenExpr {
    TOptional(
        TokenExpr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    TokenExpr* body;
};

struct TRange : public TokenExpr {
    TRange(
        int from,
        int to
    )
        : from(from), to(to) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int from;
    int to;
};

struct CommentDef : public ASTNode {
    CommentDef(
        std::string open,
        std::string close,
        bool nested
    )
        : open(open), close(close), nested(nested) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string open;
    std::string close;
    bool nested;
};

struct Production : public ASTNode {
    Production(
        std::string name,
        std::vector<Param*> params,
        std::optional<std::string> locals_code,
        PegExpr* body
    )
        : name(name), params(std::move(params)), locals_code(std::move(locals_code)), body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<Param*> params;
    std::optional<std::string> locals_code;
    PegExpr* body;
};

struct Param : public ASTNode {
    Param(
        std::string type_code,
        std::string name,
        bool is_ref
    )
        : type_code(type_code), name(name), is_ref(is_ref) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string type_code;
    std::string name;
    bool is_ref;
};

// peg_expr
struct PegExpr : public ASTNode {
    virtual ~PegExpr() = default;
};

struct Literal : public PegExpr {
    Literal(
        std::string value
    )
        : value(value) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string value;
};

struct CharsetRef : public PegExpr {
    CharsetRef(
        std::string name
    )
        : name(name) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct TokenRef : public PegExpr {
    TokenRef(
        std::string name
    )
        : name(name) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct RuleCall : public PegExpr {
    RuleCall(
        std::string name,
        std::vector<Arg*> args
    )
        : name(name), args(std::move(args)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::vector<Arg*> args;
};

struct Sequence : public PegExpr {
    Sequence(
        std::vector<PegExpr*> elements
    )
        : elements(std::move(elements)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<PegExpr*> elements;
};

struct Choice : public PegExpr {
    Choice(
        std::vector<PegExpr*> alternatives
    )
        : alternatives(std::move(alternatives)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<PegExpr*> alternatives;
};

struct Star : public PegExpr {
    Star(
        PegExpr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    PegExpr* body;
};

struct Plus : public PegExpr {
    Plus(
        PegExpr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    PegExpr* body;
};

struct Optional : public PegExpr {
    Optional(
        PegExpr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    PegExpr* body;
};

struct And : public PegExpr {
    And(
        PegExpr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    PegExpr* body;
};

struct Not : public PegExpr {
    Not(
        PegExpr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    PegExpr* body;
};

struct Group : public PegExpr {
    Group(
        PegExpr* body
    )
        : body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    PegExpr* body;
};

struct Action : public PegExpr {
    Action(
        std::string code
    )
        : code(code) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string code;
};

struct Resolver : public PegExpr {
    Resolver(
        std::string code,
        PegExpr* body
    )
        : code(code), body(body) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string code;
    PegExpr* body;
};

struct AnyExpr : public PegExpr {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct Arg : public ASTNode {
    Arg(
        std::string expr
    )
        : expr(expr) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string expr;
};

// ── Product types ───────────────────────────────────────────

} // namespace PegGrammar

#endif // PEGGRAMMAR_AST_H
