// Stub runtime for ddcgc end-to-end tests.
//
// Provides only what's *external* to the generated compiler class: the
// source AST nodes and the data/ctrl destination types. Everything else
// (per-compile state, aux/pred bodies) lives in the .ddcg's MEMBERS
// block and ends up inside the generated `tiny::Compiler` class.

#pragma once

#include <cstdint>

namespace tiny {

// ── Source location (matches the asdl-generated SourceLoc shape) ──
struct SourceLoc {
    const char* file = nullptr;
    int line   = 0;
    int column = 0;
};

// ── Source AST (the tiny.asdl analog, hand-written) ──
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

// ── data_dest delta — paper's δ ─────────────────────────────────────
//   `effect`           — value not needed (sub-expression evaluated for side effects)
//   `ac`               — store the value in the accumulator
//   `stack(slot: int)` — push it on the stack at slot offset
enum class DeltaTag : uint8_t { Effect, Ac, Stack };
struct delta_t {
    DeltaTag tag;
    int slot;
};
inline delta_t effect()       { return {DeltaTag::Effect, 0}; }
inline delta_t ac()           { return {DeltaTag::Ac, 0}; }
inline delta_t stack(int s)   { return {DeltaTag::Stack, s}; }

// ── ctrl_dest gamma — paper's γ ─────────────────────────────────────
enum class GammaTag : uint8_t { Fail, Restore };
struct gamma_t {
    GammaTag tag;
    int save;
};
inline gamma_t fail()           { return {GammaTag::Fail, 0}; }
inline gamma_t restore(int s)   { return {GammaTag::Restore, s}; }

} // namespace tiny
