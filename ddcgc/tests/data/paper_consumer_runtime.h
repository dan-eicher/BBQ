// Runtime for the paper_consumer end-to-end test.
//
// Provides everything external to the generated Compiler class:
//   1. The source AST (Lit / Add) — matches paper_consumer_ast.asdl.
//   2. The reference IR — CEK-tree shape per Phase 9R: producers
//      (LoadConst, LoadLocal, Add, Call) carry value sub-tree
//      operand fields, no `next`. Consumers (StoreLocal, ExprEffect,
//      Branch, Goto, Leave, Halt, Nop) have `next` and take a value
//      sub-tree where they need one.
//   3. The δ / γ / ρ destination structs — match the dybvig.ddcg
//      DESTINATIONS block (spliced into paper_consumer.ddcg on import).
//   4. The make_X aux implementations — paper-direct: each one
//      allocates the corresponding ir:: node and returns it.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace ir {

// Pointer hierarchy. ddcgc's match-arm field access compiles to
// `_m.Lt` etc., and the L==Lnext peephole is plain pointer equality.
struct Expr {
    virtual ~Expr() = default;
    virtual int kind() const = 0;
};

// ── Producers (value sub-trees, no `next`) ─────────────────────────
struct LoadConst : Expr {
    int value;
    explicit LoadConst(int v) : value(v) {}
    int kind() const override { return 2; }
};

struct LoadLocal : Expr {
    int src;
    explicit LoadLocal(int s) : src(s) {}
    int kind() const override { return 3; }
};

struct Add : Expr {
    Expr* left;
    Expr* right;
    Add(Expr* l, Expr* r) : left(l), right(r) {}
    int kind() const override { return 5; }
};

struct Call : Expr {
    Expr* target;
    std::vector<Expr*> args;
    int frame_size;
    Call(Expr* t, std::vector<Expr*> as, int fs)
        : target(t), args(std::move(as)), frame_size(fs) {}
    int kind() const override { return 7; }
};

// ── Consumers (have `next`; take a value sub-tree where needed) ────
struct StoreLocal : Expr {
    int dest;
    Expr* value;
    Expr* next;
    StoreLocal(int d, Expr* v, Expr* n) : dest(d), value(v), next(n) {}
    int kind() const override { return 4; }
};

struct ExprEffect : Expr {
    Expr* value;
    Expr* next;
    ExprEffect(Expr* v, Expr* n) : value(v), next(n) {}
    int kind() const override { return 8; }
};

struct Branch : Expr {
    Expr* test;
    Expr* then_;
    Expr* else_;
    Branch(Expr* t, Expr* th, Expr* el) : test(t), then_(th), else_(el) {}
    int kind() const override { return 6; }
};

struct Goto : Expr {
    Expr* target;
    explicit Goto(Expr* t) : target(t) {}
    int kind() const override { return 1; }
};

struct Leave : Expr {
    Expr* value;
    explicit Leave(Expr* v) : value(v) {}
    int kind() const override { return 9; }
};

struct Halt : Expr {
    Expr* value;
    explicit Halt(Expr* v) : value(v) {}
    int kind() const override { return 0; }
};

} // namespace ir

namespace ast {

struct SourceLoc {
    const char* file = nullptr;
    int line   = 0;
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

} // namespace ast

// Destination types live in the consumer's namespace (the .ddcg's
// COMPILER name) — ddcgc reads `<name>_t` directly without a
// `using namespace`.
namespace paper_consumer {

using SourceLoc = ast::SourceLoc;

// ── data_dest delta ────────────────────────────────────────────────
// Paper §3.1 Location class: ac, stack, local_i. paper_consumer only
// uses effect/ac; stack/local(i) are inhabited solely to satisfy the
// lib's δ enumeration — the corresponding make_X stubs in
// paper_consumer.ddcg abort if reached.
enum class DeltaTag : uint8_t { Effect, Ac, Stack, Local };
struct delta_t {
    DeltaTag tag;
    int i;
};
inline delta_t effect()       { return {DeltaTag::Effect, 0}; }
inline delta_t ac()           { return {DeltaTag::Ac, 0}; }
inline delta_t stack()        { return {DeltaTag::Stack, 0}; }
inline delta_t local(int i)   { return {DeltaTag::Local, i}; }

// ── ctrl_dest gamma ────────────────────────────────────────────────
//   `jump(L: ir.expr)` → tag=Jump, .L  populated
//   `pair(Lt, Lf)`     → tag=Pair, .Lt and .Lf populated
//   `ret`              → tag=Ret, nothing populated
//
// Three fields named exactly per the destination variant fields so
// match arms `jump(L)` / `pair(Lt, Lf)` lower to `_m.L` / `_m.Lt`
// / `_m.Lf` field reads.
enum class GammaTag : uint8_t { Jump, Pair, Ret };
struct gamma_t {
    GammaTag tag;
    ir::Expr* L;
    ir::Expr* Lt;
    ir::Expr* Lf;
};
inline gamma_t jump(ir::Expr* L)                    { return {GammaTag::Jump, L, nullptr, nullptr}; }
inline gamma_t pair(ir::Expr* Lt, ir::Expr* Lf)     { return {GammaTag::Pair, nullptr, Lt, Lf}; }
inline gamma_t ret()                                { return {GammaTag::Ret, nullptr, nullptr, nullptr}; }

// ── env_dest rho ───────────────────────────────────────────────────
enum class RhoTag : unsigned char { Frame };
struct rho_t {
    RhoTag tag;
    ir::Expr* L_break;
};
inline rho_t frame(ir::Expr* L)  { return {RhoTag::Frame, L}; }

} // namespace paper_consumer
