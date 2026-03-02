#pragma once

#include <cstddef>
#include <cstdint>

namespace bbq::cek {

// Forward declaration
struct CEKMachine;

// --- Primitive type enums (own enums, no dependency on BBQ_AST.h) ---

enum class PrimWidth : uint8_t { W8, W16, W32, W64 };
enum class PrimSign  : uint8_t { Unsigned, Signed };
enum class PrimEndian : uint8_t { Little, Big, Native };

enum class PrimKind : uint8_t { Integer, Float, Bool };

struct PrimitiveInfo {
    PrimKind kind = PrimKind::Integer;
    PrimWidth width = PrimWidth::W8;
    PrimSign sign = PrimSign::Unsigned;
    PrimEndian endian = PrimEndian::Native;

    size_t byte_count() const {
        if (kind == PrimKind::Bool) return 1;
        if (kind == PrimKind::Float) {
            return (width == PrimWidth::W64) ? 8 : 4;
        }
        switch (width) {
            case PrimWidth::W8:  return 1;
            case PrimWidth::W16: return 2;
            case PrimWidth::W32: return 4;
            case PrimWidth::W64: return 8;
        }
        return 0;
    }
};

// --- Continuation node types (for debugging/visitor use) ---

// Forward declaration for expression nodes
struct CompiledExpr;

enum class KontType : uint8_t {
    MatchPrimitive,
    BeginStruct,
    EndStruct,
    EvalConstraint,
    BindCompute,
    InvokeRule,
    ReturnRule,
    BeginChoice,
    CommitChoice,
    BeginOptional,
    EndOptional,
    BeginArray,
    ArrayNext,
    EndArray,
    SwitchDispatch,
    SetEndian,
    ExternalCall,
    MatchBytes,
    Halt,
};

// --- Base continuation node ---

struct KontNode {
    KontNode* next = nullptr;
    virtual void invoke(CEKMachine* m) = 0;

protected:
    ~KontNode() = default; // Arena-allocated, never delete'd
};

// --- MatchPrimitiveNode ---

struct MatchPrimitiveNode : KontNode {
    const char* field_name = nullptr;   // Interned
    PrimitiveInfo prim;

    void invoke(CEKMachine* m) override;
};

// --- BeginStructNode ---

struct BeginStructNode : KontNode {
    const char* struct_name = nullptr;  // Interned (may be nullptr for anonymous)
    void invoke(CEKMachine* m) override;
};

// --- EndStructNode ---

struct EndStructNode : KontNode {
    void invoke(CEKMachine* m) override;
};

// --- EvalConstraintNode ---

struct EvalConstraintNode : KontNode {
    CompiledExpr* constraint = nullptr;
    KontNode* on_false = nullptr;   // If non-null, branch here instead of failing
    void invoke(CEKMachine* m) override;
};

// --- BindComputeNode ---

struct BindComputeNode : KontNode {
    const char* field_name = nullptr;   // Interned
    CompiledExpr* expr = nullptr;
    void invoke(CEKMachine* m) override;
};

// --- InvokeRuleNode ---

struct InvokeRuleNode : KontNode {
    const char* rule_name = nullptr;    // Interned (for diagnostics + env binding)
    KontNode* entry = nullptr;          // Target rule's entry point
    void invoke(CEKMachine* m) override;
};

// --- ReturnRuleNode ---

struct ReturnRuleNode : KontNode {
    void invoke(CEKMachine* m) override;
};

// --- BeginChoiceNode ---

struct BeginChoiceNode : KontNode {
    KontNode** alternatives = nullptr;  // Remaining alternatives (after the first)
    int alt_count = 0;                  // Count of remaining alternatives
    void invoke(CEKMachine* m) override;
    // The first alternative is wired as whatever follows `next`.
    // `alternatives` holds entries for alt 2, 3, ..., N.
    // On backtrack, the machine tries alternatives[0] and advances.
};

// --- CommitChoiceNode ---

struct CommitChoiceNode : KontNode {
    void invoke(CEKMachine* m) override;
    // Pops the AlternativeFrame after a successful choice alternative.
    // Codegen pattern for (a | b | c):
    //   BeginChoice [alt2, alt3] → alt1 → Commit → join
    //                               alt2 → Commit → join
    //                               alt3 → join    (last alt needs no commit)
};

// --- BeginOptionalNode ---

struct BeginOptionalNode : KontNode {
    KontNode* inner = nullptr;          // The optional body entry
    void invoke(CEKMachine* m) override;
    // `next` is the skip point (where to go if optional absent).
    // `inner` is the body entry (where to go if optional present).
};

// --- EndOptionalNode ---

struct EndOptionalNode : KontNode {
    void invoke(CEKMachine* m) override;
    // Pops the AlternativeFrame (optional succeeded).
};

// --- Array mode ---

enum class ArrayMode : uint8_t { FixedCount, EOF_, Count, Until };

// --- BeginArrayNode ---

struct BeginArrayNode : KontNode {
    const char* array_name = nullptr;   // Interned
    ArrayMode mode = ArrayMode::FixedCount;
    CompiledExpr* count_expr = nullptr; // FixedCount/Count: evaluated for limit
    KontNode* end_node = nullptr;       // EndArray node (for empty-array shortcut)
    // next = first element body entry
    void invoke(CEKMachine* m) override;
};

// --- ArrayNextNode ---

struct ArrayNextNode : KontNode {
    KontNode* end_node = nullptr;       // EndArray node (jump when done)
    ArrayMode mode = ArrayMode::FixedCount;
    CompiledExpr* count_expr = nullptr; // Count mode: re-evaluated each iteration
    CompiledExpr* until_expr = nullptr; // Until mode: condition expression
    // next = continue target (body_entry for no-sep, sep_chain for sep)
    void invoke(CEKMachine* m) override;
};

// --- EndArrayNode ---

struct EndArrayNode : KontNode {
    // next = continuation after array
    void invoke(CEKMachine* m) override;
};

// --- SwitchDispatchNode ---

struct SwitchDispatchNode : KontNode {
    struct Case {
        int64_t int_val = 0;        // Case value (int, or interned ptr cast to int64)
        const char* str_val = nullptr; // For diagnostics (interned); not used in dispatch
        KontNode* target = nullptr;
    };
    CompiledExpr* discriminator = nullptr;
    Case* cases = nullptr;          // Arena-allocated array
    int case_count = 0;
    KontNode* default_target = nullptr; // nullptr = no default (fatal on miss)
    void invoke(CEKMachine* m) override;
};

// --- BitfieldReadNode ---

struct BitfieldReadNode : KontNode {
    PrimitiveInfo container;            // Container type (e.g., u16be)
    struct Entry {
        const char* name = nullptr;     // Interned
        int width_bits = 0;
    };
    Entry* entries = nullptr;           // Arena-allocated array
    int entry_count = 0;
    void invoke(CEKMachine* m) override;
};

// --- SetEndianNode ---

struct SetEndianNode : KontNode {
    CompiledExpr* expr = nullptr;       // 0 = big endian, non-zero = little endian
    void invoke(CEKMachine* m) override;
};

// --- ExternalCallNode ---

struct ExternalCallNode : KontNode {
    const char* func_name = nullptr;    // Interned, looked up in ext_parsers
    const char* field_name = nullptr;   // Interned, capture/env binding name
    void invoke(CEKMachine* m) override;
};

// --- MatchBytesNode ---

struct MatchBytesNode : KontNode {
    const char* field_name = nullptr;   // Interned
    CompiledExpr* length_expr = nullptr;
    bool is_string = false;             // CaptureType::String vs Bytes
    void invoke(CEKMachine* m) override;
};

// --- HaltNode ---

struct HaltNode : KontNode {
    void invoke(CEKMachine* m) override;
};

} // namespace bbq::cek
