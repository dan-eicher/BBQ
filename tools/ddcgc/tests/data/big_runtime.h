// Runtime for the big end-to-end test fixture. Provides everything
// *external* to the generated Compiler class — the AST nodes and the
// δ / γ tagged unions plus their constructors. Per-compile state, aux
// implementations, and predicate bodies all live in big.ddcg's MEMBERS
// block and end up inside `big::Compiler`.

#pragma once

#include <string>
#include <vector>

namespace big {

struct SourceLoc {
    const char* file = nullptr;
    int line = 0;
    int column = 0;
};

struct Expr {
    SourceLoc loc;
    virtual ~Expr() = default;
};

struct Lit : Expr {
    int n;
    explicit Lit(int n_) : n(n_) {}
};

struct Add : Expr {
    Expr* left;
    Expr* right;
    Add(Expr* l, Expr* r) : left(l), right(r) {}
};

struct Seq : Expr {
    std::vector<Expr*> items;
    explicit Seq(std::vector<Expr*> xs) : items(std::move(xs)) {}
};

struct Pair : Expr {
    Expr* first;
    Expr* second;
    Pair(Expr* a, Expr* b) : first(a), second(b) {}
};

struct Tag : Expr {
    std::string name;
    explicit Tag(std::string s) : name(std::move(s)) {}
};

struct Maybe : Expr {
    Expr* sub;  // nullable
    explicit Maybe(Expr* s) : sub(s) {}
};

// ── data_dest delta — paper's δ ──
enum class DeltaTag : unsigned char { Effect, Ac, Spill };
struct delta_t { DeltaTag tag; int slot; };
inline delta_t effect()      { return {DeltaTag::Effect, 0}; }
inline delta_t ac()          { return {DeltaTag::Ac, 0}; }
inline delta_t spill(int s)  { return {DeltaTag::Spill, s}; }

// ── ctrl_dest gamma — paper's γ ──
enum class GammaTag : unsigned char { Fail, Jump };
struct gamma_t { GammaTag tag; int target; };
inline gamma_t fail()         { return {GammaTag::Fail, 0}; }
inline gamma_t jump(int L)    { return {GammaTag::Jump, L}; }

} // namespace big
