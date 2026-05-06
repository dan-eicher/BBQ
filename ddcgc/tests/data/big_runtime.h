// Runtime for the big end-to-end test fixture. Provides everything
// *external* to the generated Compiler class — the AST nodes and the
// δ / γ tagged unions plus their constructors. Per-compile state, aux
// implementations, and predicate bodies all live in big.ddcg's MEMBERS
// block and end up inside `big::Compiler`.

#pragma once

#include <optional>
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
    // asdl `expr? sub` lowers to std::optional<Expr*>; ddcgc's pattern
    // emitter unwraps with .has_value() / *sub at use sites.
    std::optional<Expr*> sub;
    explicit Maybe(std::optional<Expr*> s) : sub(s) {}
};

// asdl lowers all-zero-arg sums to `enum class`; the generated code
// references these values as `big::Op::OpAdd` etc.
enum class Op { OpAdd, OpSub, OpMul };

struct BinOp : Expr {
    Op op;
    Expr* left;
    Expr* right;
    BinOp(Op o, Expr* l, Expr* r) : op(o), left(l), right(r) {}
};

struct Sel : Expr {
    Op which;
    explicit Sel(Op w) : which(w) {}
};

struct Multi : Expr {
    std::vector<int> nums;
    explicit Multi(std::vector<int> ns) : nums(std::move(ns)) {}
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

// ── env_dest rho — paper's ρ ──
enum class RhoTag : unsigned char { Frame, Scoped };
struct rho_t { RhoTag tag; int depth; int label; };
inline rho_t frame(int d)              { return {RhoTag::Frame, d, 0}; }
inline rho_t scoped(int d, int l)      { return {RhoTag::Scoped, d, l}; }

} // namespace big

namespace target {

struct Value {
    virtual ~Value() = default;
    virtual int kind_tag() const = 0;
    virtual int payload() const = 0;
};

struct Imm : Value {
    int v;
    explicit Imm(int v_) : v(v_) {}
    int kind_tag() const override { return 0; }
    int payload() const override  { return v; }
};

struct Slot : Value {
    int s;
    explicit Slot(int s_) : s(s_) {}
    int kind_tag() const override { return 1; }
    int payload() const override  { return s; }
};

} // namespace target
