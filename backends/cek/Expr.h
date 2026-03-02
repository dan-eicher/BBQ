#pragma once

#include <cstdint>

namespace bbq::cek {

// Forward declaration
struct CEKMachine;

enum class ExprOp : uint8_t {
    // Literals
    IntLit, FloatLit, StrLit, BoolLit,
    // Variable reference (interned name)
    Ref,
    // Binary operators
    Add, Sub, Mul, Div, Mod,
    BitAnd, BitOr, BitXor, Shl, Shr,
    And, Or, Eq, Ne, Lt, Le, Gt, Ge,
    // Unary operators
    Neg, BitNot, Not,
    // Ternary
    Ternary,
    // Function call
    Call,
    // Access
    FieldAccess, IndexAccess,
    // Special references
    Pos, Remaining, AtEnd, EOI, LoopVar
};

struct CompiledExpr {
    ExprOp op;
    union {
        int64_t int_val;
        double float_val;
        const char* str_val;        // Interned
        bool bool_val;
        const char* ref_name;       // Interned variable name
        struct { CompiledExpr* left; CompiledExpr* right; } binary;
        struct { CompiledExpr* operand; } unary;
        struct { CompiledExpr* cond; CompiledExpr* then_; CompiledExpr* else_; } ternary;
        struct { const char* func_name; CompiledExpr** args; int arg_count; } call;
        struct { CompiledExpr* base; const char* field; } field_access;
        struct { CompiledExpr* base; CompiledExpr* index; } index_access;
    };
};

// Evaluate a compiled expression tree against the current machine state.
// On error (undefined ref, div by zero), calls m->fail() and returns 0.
// Recursive — expression depth is bounded (3-5 levels in BBQ), no stack risk.
int64_t eval(const CompiledExpr* expr, CEKMachine* m);

} // namespace bbq::cek
