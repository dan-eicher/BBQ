// ============================================================
// Grammar AST — Auto-generated from grammar.asdl
//
// Do not edit by hand. Re-generate with:
//   ./asdl -i grammar.asdl -t templates/grammar_ast.inja -o GrammarAST.h
// ============================================================
#ifndef PARSERKONT_AST_H
#define PARSERKONT_AST_H

#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace ParserKont {

// ── Enum types ──────────────────────────────────────────────

// ── Forward declarations ────────────────────────────────────

struct Kont;
struct KSeq;
struct KChoice;
struct KStar;
struct KPlus;
struct KOptional;
struct KAnd;
struct KNot;
struct KMatchString;
struct KMatchAny;
struct KMatchCharRange;
struct KMatchCharset;
struct KCallProduction;
struct KCallToken;
struct KCallExtern;
struct KAction;
struct KResolver;

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

    virtual void visit(KSeq* node) {}
    virtual void visit(KChoice* node) {}
    virtual void visit(KStar* node) {}
    virtual void visit(KPlus* node) {}
    virtual void visit(KOptional* node) {}
    virtual void visit(KAnd* node) {}
    virtual void visit(KNot* node) {}
    virtual void visit(KMatchString* node) {}
    virtual void visit(KMatchAny* node) {}
    virtual void visit(KMatchCharRange* node) {}
    virtual void visit(KMatchCharset* node) {}
    virtual void visit(KCallProduction* node) {}
    virtual void visit(KCallToken* node) {}
    virtual void visit(KCallExtern* node) {}
    virtual void visit(KAction* node) {}
    virtual void visit(KResolver* node) {}
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

// kont
struct Kont : public ASTNode {
    virtual ~Kont() = default;
};

struct KSeq : public Kont {
    KSeq(
        std::vector<Kont*> elems
    )
        : elems(std::move(elems)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<Kont*> elems;
};

struct KChoice : public Kont {
    KChoice(
        std::vector<Kont*> alts
    )
        : alts(std::move(alts)){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::vector<Kont*> alts;
};

struct KStar : public Kont {
    KStar(
        Kont* body
    )
        : body(body){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Kont* body;
};

struct KPlus : public Kont {
    KPlus(
        Kont* body
    )
        : body(body){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Kont* body;
};

struct KOptional : public Kont {
    KOptional(
        Kont* body
    )
        : body(body){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Kont* body;
};

struct KAnd : public Kont {
    KAnd(
        Kont* body
    )
        : body(body){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Kont* body;
};

struct KNot : public Kont {
    KNot(
        Kont* body
    )
        : body(body){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    Kont* body;
};

struct KMatchString : public Kont {
    KMatchString(
        std::string value
    )
        : value(value){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string value;
};

struct KMatchAny : public Kont {

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

};

struct KMatchCharRange : public Kont {
    KMatchCharRange(
        int from,
        int to
    )
        : from(from), to(to){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    int from;
    int to;
};

struct KMatchCharset : public Kont {
    KMatchCharset(
        std::string name
    )
        : name(name){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
};

struct KCallProduction : public Kont {
    KCallProduction(
        std::string name,
        std::string args
    )
        : name(name), args(args){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::string args;
};

struct KCallToken : public Kont {
    KCallToken(
        std::string name,
        std::string args
    )
        : name(name), args(args){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::string args;
};

struct KCallExtern : public Kont {
    KCallExtern(
        std::string name,
        std::string args
    )
        : name(name), args(args){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string name;
    std::string args;
};

struct KAction : public Kont {
    KAction(
        std::string code
    )
        : code(code){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string code;
};

struct KResolver : public Kont {
    KResolver(
        std::string cond,
        Kont* body
    )
        : cond(cond), body(body){}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }

    std::string cond;
    Kont* body;
};

// ── Product types ───────────────────────────────────────────

} // namespace ParserKont

#endif // PARSERKONT_AST_H
