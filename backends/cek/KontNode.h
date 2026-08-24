// ============================================================
// BBQ CEK IR — auto-generated from bbq_ir.asdl
// Do not edit by hand. Regenerate via the asdl tool.
// ============================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "CaptureCow.h"  // bbq::zcow::node — the parsed document the machine
                         // builds and that FieldCaptureValue references (resolved
                         // by outer-namespace lookup from bbq::cek). The runtime
                         // does NOT depend back.

namespace bbq::cek {

// Forward declaration for the runtime that consumes nodes.
struct CEKMachine;

// ── Primitive type descriptor ───────────────────────────────

enum class PrimWidth : uint8_t { W8, W16, W32, W64 };
enum class PrimSign  : uint8_t { Unsigned, Signed };
enum class PrimEndian : uint8_t { Little, Big, Native };
enum class PrimKind : uint8_t { Integer, Float, Bool };
// Integer byte encoding: a fixed-width field, or a variable-length LEB128 varint
// (unsigned ULEB / signed SLEB). LEB128 is decoded at read time into the carrier
// width (W32/W64) and recorded as a Computed capture (it is not re-decodable from a
// fixed-width byte span). Mirrors BBQ::Encoding in the AST.
enum class PrimEncoding : uint8_t { Fixed, Uleb, Sleb };

struct PrimitiveInfo {
    PrimKind kind = PrimKind::Integer;
    PrimWidth width = PrimWidth::W8;
    PrimSign sign = PrimSign::Unsigned;
    PrimEndian endian = PrimEndian::Native;
    PrimEncoding encoding = PrimEncoding::Fixed;

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

// ── Generated enums ─────────────────────────────────────────

enum class ArrayMode : uint8_t {
    FixedCount,
    EOF_,
    Count,
    Until
};

// ── Forward declarations ────────────────────────────────────

struct KontNode;
struct Value;
struct StaticKont;
struct DynamicKont;
struct switch_case;
struct IntValue;
struct FloatValue;
struct StringValue;
struct BoolValue;
struct FieldCaptureValue;
struct MatchPrimitiveNode;
struct CaptureKont;
struct StoreEnvKont;
struct BeginStructNode;
struct BeginVariantNode;
struct EndStructNode;
struct SeekKont;
struct PushIntervalNode;
struct PopIntervalNode;
struct EvalConstraintNode;
struct BindComputeNode;
struct InvokeRuleNode;
struct ReturnRuleNode;
struct OnFailKont;
struct AbortRuleKont;
struct BeginChoiceNode;
struct CommitChoiceNode;
struct BeginOptionalNode;
struct EndOptionalNode;
struct BeginArrayNode;
struct ArrayNextNode;
struct EndArrayNode;
struct ResyncKont;
struct SwitchDispatchNode;
struct ExtractBitsNode;
struct AdvancePosNode;
struct SetEndianNode;
struct ExternalCallNode;
struct MatchBytesNode;
struct HaltNode;
struct IntLitKont;
struct BoolLitKont;
struct FloatLitKont;
struct StrLitKont;
struct RefKont;
struct PosKont;
struct RemainingKont;
struct EOIKont;
struct LoopVarKont;
struct BufferKont;
struct StartKont;
struct EndKont;
struct NegKont;
struct BitNotKont;
struct NotKont;
struct AddKont;
struct SubKont;
struct MulKont;
struct DivKont;
struct ModKont;
struct BitAndKont;
struct BitOrKont;
struct BitXorKont;
struct ShlKont;
struct ShrKont;
struct EqKont;
struct NeKont;
struct LtKont;
struct LeKont;
struct GtKont;
struct GeKont;
struct LogicalAndKont;
struct LogicalOrKont;
struct TernaryKont;
struct CallKont;
struct PathStartKont;
struct FieldAccessKont;
struct IndexAccessKont;
struct CaptureValueKont;
struct ApplyAddKont;
struct ApplySubKont;
struct ApplyMulKont;
struct ApplyDivKont;
struct ApplyModKont;
struct ApplyBitAndKont;
struct ApplyBitOrKont;
struct ApplyBitXorKont;
struct ApplyShlKont;
struct ApplyShrKont;
struct ApplyEqKont;
struct ApplyNeKont;
struct ApplyLtKont;
struct ApplyLeKont;
struct ApplyGtKont;
struct ApplyGeKont;
struct ApplyNegKont;
struct ApplyBitNotKont;
struct ApplyNotKont;
struct EvalLeftKont;
struct LogicalAndCheckKont;
struct LogicalOrCheckKont;
struct NormalizeKont;
struct TernaryDispatchKont;
struct ArgStoreKont;
struct CallApplyKont;
struct FieldAccessApplyKont;
struct IndexAccessApplyKont;
struct CaptureValueApplyKont;
struct EvalConstraintCheckKont;
struct SwitchSelectKont;
struct BindComputeStoreKont;
struct BeginArrayWithLimitKont;
struct ArrayCountCheckKont;
struct ArrayUntilCheckKont;
struct MatchBytesApplyKont;
struct SetEndianApplyKont;
struct SeekApplyKont;
struct PushIntervalApplyKont;

// ── Kont kind tag ───────────────────────────────────────────
//
// One value per kont variant. This is what burg matches on
// (BURG_NODE_OP) and what the source lowering / generated accessors
// switch over. Emitted straight from the asdl variant list so it can
// never drift from the IR.

enum class KontKind : int {
    MatchPrimitiveNode,
    CaptureKont,
    StoreEnvKont,
    BeginStructNode,
    BeginVariantNode,
    EndStructNode,
    SeekKont,
    PushIntervalNode,
    PopIntervalNode,
    EvalConstraintNode,
    BindComputeNode,
    InvokeRuleNode,
    ReturnRuleNode,
    OnFailKont,
    AbortRuleKont,
    BeginChoiceNode,
    CommitChoiceNode,
    BeginOptionalNode,
    EndOptionalNode,
    BeginArrayNode,
    ArrayNextNode,
    EndArrayNode,
    ResyncKont,
    SwitchDispatchNode,
    ExtractBitsNode,
    AdvancePosNode,
    SetEndianNode,
    ExternalCallNode,
    MatchBytesNode,
    HaltNode,
    IntLitKont,
    BoolLitKont,
    FloatLitKont,
    StrLitKont,
    RefKont,
    PosKont,
    RemainingKont,
    EOIKont,
    LoopVarKont,
    BufferKont,
    StartKont,
    EndKont,
    NegKont,
    BitNotKont,
    NotKont,
    AddKont,
    SubKont,
    MulKont,
    DivKont,
    ModKont,
    BitAndKont,
    BitOrKont,
    BitXorKont,
    ShlKont,
    ShrKont,
    EqKont,
    NeKont,
    LtKont,
    LeKont,
    GtKont,
    GeKont,
    LogicalAndKont,
    LogicalOrKont,
    TernaryKont,
    CallKont,
    PathStartKont,
    FieldAccessKont,
    IndexAccessKont,
    CaptureValueKont,
    ApplyAddKont,
    ApplySubKont,
    ApplyMulKont,
    ApplyDivKont,
    ApplyModKont,
    ApplyBitAndKont,
    ApplyBitOrKont,
    ApplyBitXorKont,
    ApplyShlKont,
    ApplyShrKont,
    ApplyEqKont,
    ApplyNeKont,
    ApplyLtKont,
    ApplyLeKont,
    ApplyGtKont,
    ApplyGeKont,
    ApplyNegKont,
    ApplyBitNotKont,
    ApplyNotKont,
    EvalLeftKont,
    LogicalAndCheckKont,
    LogicalOrCheckKont,
    NormalizeKont,
    TernaryDispatchKont,
    ArgStoreKont,
    CallApplyKont,
    FieldAccessApplyKont,
    IndexAccessApplyKont,
    CaptureValueApplyKont,
    EvalConstraintCheckKont,
    SwitchSelectKont,
    BindComputeStoreKont,
    BeginArrayWithLimitKont,
    ArrayCountCheckKont,
    ArrayUntilCheckKont,
    MatchBytesApplyKont,
    SetEndianApplyKont,
    SeekApplyKont,
    PushIntervalApplyKont,
};

inline const char* kont_kind_name(KontKind k) {
    switch (k) {
    case KontKind::MatchPrimitiveNode: return "MatchPrimitiveNode";
    case KontKind::CaptureKont: return "CaptureKont";
    case KontKind::StoreEnvKont: return "StoreEnvKont";
    case KontKind::BeginStructNode: return "BeginStructNode";
    case KontKind::BeginVariantNode: return "BeginVariantNode";
    case KontKind::EndStructNode: return "EndStructNode";
    case KontKind::SeekKont: return "SeekKont";
    case KontKind::PushIntervalNode: return "PushIntervalNode";
    case KontKind::PopIntervalNode: return "PopIntervalNode";
    case KontKind::EvalConstraintNode: return "EvalConstraintNode";
    case KontKind::BindComputeNode: return "BindComputeNode";
    case KontKind::InvokeRuleNode: return "InvokeRuleNode";
    case KontKind::ReturnRuleNode: return "ReturnRuleNode";
    case KontKind::OnFailKont: return "OnFailKont";
    case KontKind::AbortRuleKont: return "AbortRuleKont";
    case KontKind::BeginChoiceNode: return "BeginChoiceNode";
    case KontKind::CommitChoiceNode: return "CommitChoiceNode";
    case KontKind::BeginOptionalNode: return "BeginOptionalNode";
    case KontKind::EndOptionalNode: return "EndOptionalNode";
    case KontKind::BeginArrayNode: return "BeginArrayNode";
    case KontKind::ArrayNextNode: return "ArrayNextNode";
    case KontKind::EndArrayNode: return "EndArrayNode";
    case KontKind::ResyncKont: return "ResyncKont";
    case KontKind::SwitchDispatchNode: return "SwitchDispatchNode";
    case KontKind::ExtractBitsNode: return "ExtractBitsNode";
    case KontKind::AdvancePosNode: return "AdvancePosNode";
    case KontKind::SetEndianNode: return "SetEndianNode";
    case KontKind::ExternalCallNode: return "ExternalCallNode";
    case KontKind::MatchBytesNode: return "MatchBytesNode";
    case KontKind::HaltNode: return "HaltNode";
    case KontKind::IntLitKont: return "IntLitKont";
    case KontKind::BoolLitKont: return "BoolLitKont";
    case KontKind::FloatLitKont: return "FloatLitKont";
    case KontKind::StrLitKont: return "StrLitKont";
    case KontKind::RefKont: return "RefKont";
    case KontKind::PosKont: return "PosKont";
    case KontKind::RemainingKont: return "RemainingKont";
    case KontKind::EOIKont: return "EOIKont";
    case KontKind::LoopVarKont: return "LoopVarKont";
    case KontKind::BufferKont: return "BufferKont";
    case KontKind::StartKont: return "StartKont";
    case KontKind::EndKont: return "EndKont";
    case KontKind::NegKont: return "NegKont";
    case KontKind::BitNotKont: return "BitNotKont";
    case KontKind::NotKont: return "NotKont";
    case KontKind::AddKont: return "AddKont";
    case KontKind::SubKont: return "SubKont";
    case KontKind::MulKont: return "MulKont";
    case KontKind::DivKont: return "DivKont";
    case KontKind::ModKont: return "ModKont";
    case KontKind::BitAndKont: return "BitAndKont";
    case KontKind::BitOrKont: return "BitOrKont";
    case KontKind::BitXorKont: return "BitXorKont";
    case KontKind::ShlKont: return "ShlKont";
    case KontKind::ShrKont: return "ShrKont";
    case KontKind::EqKont: return "EqKont";
    case KontKind::NeKont: return "NeKont";
    case KontKind::LtKont: return "LtKont";
    case KontKind::LeKont: return "LeKont";
    case KontKind::GtKont: return "GtKont";
    case KontKind::GeKont: return "GeKont";
    case KontKind::LogicalAndKont: return "LogicalAndKont";
    case KontKind::LogicalOrKont: return "LogicalOrKont";
    case KontKind::TernaryKont: return "TernaryKont";
    case KontKind::CallKont: return "CallKont";
    case KontKind::PathStartKont: return "PathStartKont";
    case KontKind::FieldAccessKont: return "FieldAccessKont";
    case KontKind::IndexAccessKont: return "IndexAccessKont";
    case KontKind::CaptureValueKont: return "CaptureValueKont";
    case KontKind::ApplyAddKont: return "ApplyAddKont";
    case KontKind::ApplySubKont: return "ApplySubKont";
    case KontKind::ApplyMulKont: return "ApplyMulKont";
    case KontKind::ApplyDivKont: return "ApplyDivKont";
    case KontKind::ApplyModKont: return "ApplyModKont";
    case KontKind::ApplyBitAndKont: return "ApplyBitAndKont";
    case KontKind::ApplyBitOrKont: return "ApplyBitOrKont";
    case KontKind::ApplyBitXorKont: return "ApplyBitXorKont";
    case KontKind::ApplyShlKont: return "ApplyShlKont";
    case KontKind::ApplyShrKont: return "ApplyShrKont";
    case KontKind::ApplyEqKont: return "ApplyEqKont";
    case KontKind::ApplyNeKont: return "ApplyNeKont";
    case KontKind::ApplyLtKont: return "ApplyLtKont";
    case KontKind::ApplyLeKont: return "ApplyLeKont";
    case KontKind::ApplyGtKont: return "ApplyGtKont";
    case KontKind::ApplyGeKont: return "ApplyGeKont";
    case KontKind::ApplyNegKont: return "ApplyNegKont";
    case KontKind::ApplyBitNotKont: return "ApplyBitNotKont";
    case KontKind::ApplyNotKont: return "ApplyNotKont";
    case KontKind::EvalLeftKont: return "EvalLeftKont";
    case KontKind::LogicalAndCheckKont: return "LogicalAndCheckKont";
    case KontKind::LogicalOrCheckKont: return "LogicalOrCheckKont";
    case KontKind::NormalizeKont: return "NormalizeKont";
    case KontKind::TernaryDispatchKont: return "TernaryDispatchKont";
    case KontKind::ArgStoreKont: return "ArgStoreKont";
    case KontKind::CallApplyKont: return "CallApplyKont";
    case KontKind::FieldAccessApplyKont: return "FieldAccessApplyKont";
    case KontKind::IndexAccessApplyKont: return "IndexAccessApplyKont";
    case KontKind::CaptureValueApplyKont: return "CaptureValueApplyKont";
    case KontKind::EvalConstraintCheckKont: return "EvalConstraintCheckKont";
    case KontKind::SwitchSelectKont: return "SwitchSelectKont";
    case KontKind::BindComputeStoreKont: return "BindComputeStoreKont";
    case KontKind::BeginArrayWithLimitKont: return "BeginArrayWithLimitKont";
    case KontKind::ArrayCountCheckKont: return "ArrayCountCheckKont";
    case KontKind::ArrayUntilCheckKont: return "ArrayUntilCheckKont";
    case KontKind::MatchBytesApplyKont: return "MatchBytesApplyKont";
    case KontKind::SetEndianApplyKont: return "SetEndianApplyKont";
    case KontKind::SeekApplyKont: return "SeekApplyKont";
    case KontKind::PushIntervalApplyKont: return "PushIntervalApplyKont";
    }
    return "?";
}

// ── KontNode root ───────────────────────────────────────────
//
// The shared root for static_kont and dynamic_kont variants.
// `invoke()` is private with `friend class CEKMachine` so the
// trampoline (`CEKMachine::execute_from()`) is the only legal
// call site. Any kont's invoke body that tries to call another
// kont's invoke directly is a compile error — schedule via
// `m->push_kont(other)` instead.

struct KontNode {
    virtual ~KontNode() = default;

    // The variant tag — burg's BURG_NODE_OP, and what the source
    // lowering / generated accessors switch on. Every kont variant
    // overrides it (generated below).
    virtual KontKind kind() const = 0;

    // Graph-walk axes for burg / the source lowering. Operands are the
    // tiled value sub-trees (BURG_NODE_CHILD, the `kont_expr` fields);
    // successors are the spine/control edges (BURG_NODE_SUCC, the
    // `kont_node` fields + switch-case targets). Static konts override
    // these (generated); dynamic konts keep the no-edge defaults.
    virtual int child_count() const { return 0; }
    virtual KontNode* child(int) const { return nullptr; }
    virtual int succ_count() const { return 0; }
    virtual KontNode* succ(int) const { return nullptr; }

private:
    friend struct CEKMachine;
    virtual void invoke(CEKMachine* m) const = 0;
};

// ── Product types (inline structs nested in node fields) ────

struct switch_case {
    Value* match_value{};
    KontNode* target{};
    int ordinal{};
};

// ── Sum types ───────────────────────────────────────────────
//
// `value` is a sum of plain data records — no virtual methods,
// tag-based dispatch via `kind` constants (consumed by `expect<T>`
// in Machine.cpp).
//
// `static_kont` and `dynamic_kont` are sums of kont variants —
// each inherits from its sum base, which inherits from `KontNode`.
// Variants override the private `invoke()` declared on KontNode.

// Value sum — typed accumulator values + binding values + computed_value.
enum class ValueTag : int {
    IntValue,
    FloatValue,
    StringValue,
    BoolValue,
    FieldCaptureValue
};

struct Value {
    ValueTag tag;
    explicit Value(ValueTag t) : tag(t) {}

protected:
    ~Value() = default;  // arena-allocated, never delete'd
};

struct IntValue : public Value {
    static constexpr ValueTag kind = ValueTag::IntValue;

    IntValue() : Value(kind) {}

    int64_t v{};
};

struct FloatValue : public Value {
    static constexpr ValueTag kind = ValueTag::FloatValue;

    FloatValue() : Value(kind) {}

    double v{};
};

struct StringValue : public Value {
    static constexpr ValueTag kind = ValueTag::StringValue;

    StringValue() : Value(kind) {}

    const char* v{};
};

struct BoolValue : public Value {
    static constexpr ValueTag kind = ValueTag::BoolValue;

    BoolValue() : Value(kind) {}

    bool v{};
};

struct FieldCaptureValue : public Value {
    static constexpr ValueTag kind = ValueTag::FieldCaptureValue;

    FieldCaptureValue() : Value(kind) {}

    zcow::node* capture{};
};

// StaticKont — sum base inheriting from the shared `KontNode` root.
struct StaticKont : public KontNode {
};

struct MatchPrimitiveNode : public StaticKont {
    PrimitiveInfo prim{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::MatchPrimitiveNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::MatchPrimitiveNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct CaptureKont : public StaticKont {
    const char* name{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::CaptureKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::CaptureKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct StoreEnvKont : public StaticKont {
    const char* name{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::StoreEnvKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::StoreEnvKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginStructNode : public StaticKont {
    const char* struct_name{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::BeginStructNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BeginStructNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginVariantNode : public StaticKont {
    const char* variant_name{};
    int variant_tag{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::BeginVariantNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BeginVariantNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct EndStructNode : public StaticKont {
    KontNode* next{};

    KontKind kind() const override { return KontKind::EndStructNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::EndStructNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct SeekKont : public StaticKont {
    KontNode* expr{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::SeekKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::SeekKont);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->expr, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct PushIntervalNode : public StaticKont {
    bool relative{};
    KontNode* expr{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::PushIntervalNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::PushIntervalNode);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->expr, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct PopIntervalNode : public StaticKont {
    bool checked{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::PopIntervalNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::PopIntervalNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct EvalConstraintNode : public StaticKont {
    KontNode* constraint_expr{};
    KontNode* on_false{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::EvalConstraintNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::EvalConstraintNode);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->constraint_expr, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->on_false, this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BindComputeNode : public StaticKont {
    KontNode* compute_expr{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::BindComputeNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BindComputeNode);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->compute_expr, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct InvokeRuleNode : public StaticKont {
    const char* rule_name{};
    KontNode* entry{};
    KontNode* caller_fail_target{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::InvokeRuleNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::InvokeRuleNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct ReturnRuleNode : public StaticKont {
    KontNode* next{};

    KontKind kind() const override { return KontKind::ReturnRuleNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ReturnRuleNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct OnFailKont : public StaticKont {
    KontNode* target{};
    KontNode* next_on_fail{};
    bool pop_frame{};

    KontKind kind() const override { return KontKind::OnFailKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::OnFailKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->target, this->next_on_fail, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct AbortRuleKont : public StaticKont {

    KontKind kind() const override { return KontKind::AbortRuleKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::AbortRuleKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginChoiceNode : public StaticKont {
    KontNode* on_fail_target{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::BeginChoiceNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BeginChoiceNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->on_fail_target, this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct CommitChoiceNode : public StaticKont {
    KontNode* next{};

    KontKind kind() const override { return KontKind::CommitChoiceNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::CommitChoiceNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginOptionalNode : public StaticKont {
    KontNode* on_fail_target{};
    KontNode* inner{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::BeginOptionalNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BeginOptionalNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1+ 1+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->on_fail_target, this->inner, this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct EndOptionalNode : public StaticKont {
    KontNode* next{};

    KontKind kind() const override { return KontKind::EndOptionalNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::EndOptionalNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginArrayNode : public StaticKont {
    const char* array_name{};
    ArrayMode mode{};
    KontNode* count_expr{};
    KontNode* until_expr{};
    KontNode* end_node{};
    KontNode* resync_target{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::BeginArrayNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BeginArrayNode);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->count_expr, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1+ 1+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->end_node, this->resync_target, this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct ArrayNextNode : public StaticKont {
    ArrayMode mode{};
    KontNode* count_expr{};
    KontNode* until_expr{};
    KontNode* end_node{};
    KontNode* body_entry{};
    bool resync{};

    KontKind kind() const override { return KontKind::ArrayNextNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ArrayNextNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->end_node, this->body_entry, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct EndArrayNode : public StaticKont {
    bool resync{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::EndArrayNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::EndArrayNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct ResyncKont : public StaticKont {
    KontNode* body_entry{};
    KontNode* end_node{};

    KontKind kind() const override { return KontKind::ResyncKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ResyncKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->body_entry, this->end_node, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct SwitchDispatchNode : public StaticKont {
    KontNode* discriminator_expr{};
    switch_case* cases = nullptr;
    int cases_count = 0;
    KontNode* default_target{};
    bool default_reject{};

    KontKind kind() const override { return KontKind::SwitchDispatchNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::SwitchDispatchNode);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->discriminator_expr, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ cases_count+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->default_target, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        if (i >= nf && i < nf + this->cases_count) return this->cases[i - nf].target;
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct ExtractBitsNode : public StaticKont {
    PrimitiveInfo container{};
    int width_bits{};
    int offset_before{};
    bool is_signed{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::ExtractBitsNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ExtractBitsNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct AdvancePosNode : public StaticKont {
    int by{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::AdvancePosNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::AdvancePosNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct SetEndianNode : public StaticKont {
    KontNode* endian_expr{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::SetEndianNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::SetEndianNode);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->endian_expr, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct ExternalCallNode : public StaticKont {
    const char* func_name{};
    const char* write_func{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::ExternalCallNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ExternalCallNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct MatchBytesNode : public StaticKont {
    KontNode* length_expr{};
    bool is_string{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::MatchBytesNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::MatchBytesNode);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->length_expr, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0+ 1;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { this->next, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct HaltNode : public StaticKont {

    KontKind kind() const override { return KontKind::HaltNode; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::HaltNode);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct IntLitKont : public StaticKont {
    int64_t v{};

    KontKind kind() const override { return KontKind::IntLitKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::IntLitKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BoolLitKont : public StaticKont {
    bool v{};

    KontKind kind() const override { return KontKind::BoolLitKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BoolLitKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct FloatLitKont : public StaticKont {
    float v{};

    KontKind kind() const override { return KontKind::FloatLitKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::FloatLitKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct StrLitKont : public StaticKont {
    const char* v{};

    KontKind kind() const override { return KontKind::StrLitKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::StrLitKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct RefKont : public StaticKont {
    const char* name{};
    const char* resolved_path{};
    const char* resolved_parent{};
    int resolved_depth{};

    KontKind kind() const override { return KontKind::RefKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::RefKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct PosKont : public StaticKont {

    KontKind kind() const override { return KontKind::PosKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::PosKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct RemainingKont : public StaticKont {

    KontKind kind() const override { return KontKind::RemainingKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::RemainingKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct EOIKont : public StaticKont {

    KontKind kind() const override { return KontKind::EOIKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::EOIKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct LoopVarKont : public StaticKont {

    KontKind kind() const override { return KontKind::LoopVarKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::LoopVarKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BufferKont : public StaticKont {

    KontKind kind() const override { return KontKind::BufferKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BufferKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct StartKont : public StaticKont {

    KontKind kind() const override { return KontKind::StartKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::StartKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct EndKont : public StaticKont {

    KontKind kind() const override { return KontKind::EndKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::EndKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct NegKont : public StaticKont {
    KontNode* operand{};

    KontKind kind() const override { return KontKind::NegKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::NegKont);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->operand, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BitNotKont : public StaticKont {
    KontNode* operand{};

    KontKind kind() const override { return KontKind::BitNotKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BitNotKont);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->operand, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct NotKont : public StaticKont {
    KontNode* operand{};

    KontKind kind() const override { return KontKind::NotKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::NotKont);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->operand, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct AddKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::AddKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::AddKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct SubKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::SubKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::SubKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct MulKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::MulKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::MulKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct DivKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::DivKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::DivKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct ModKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::ModKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ModKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BitAndKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::BitAndKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BitAndKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BitOrKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::BitOrKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BitOrKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct BitXorKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::BitXorKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BitXorKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct ShlKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::ShlKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ShlKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct ShrKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::ShrKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ShrKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct EqKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::EqKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::EqKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct NeKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::NeKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::NeKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct LtKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::LtKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::LtKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct LeKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::LeKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::LeKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct GtKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::GtKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::GtKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct GeKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::GeKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::GeKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct LogicalAndKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::LogicalAndKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::LogicalAndKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct LogicalOrKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

    KontKind kind() const override { return KontKind::LogicalOrKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::LogicalOrKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->left, this->right, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct TernaryKont : public StaticKont {
    KontNode* cond{};
    KontNode* then_{};
    KontNode* else_{};

    KontKind kind() const override { return KontKind::TernaryKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::TernaryKont);

    int child_count() const override {
        return 0+ 1+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->cond, this->then_, this->else_, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct CallKont : public StaticKont {
    const char* func_name{};
    KontNode** args = nullptr;
    int args_count = 0;

    KontKind kind() const override { return KontKind::CallKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::CallKont);

    int child_count() const override {
        return 0+ args_count;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        if (i >= nf && i < nf + this->args_count) return this->args[i - nf];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct PathStartKont : public StaticKont {
    const char* name{};

    KontKind kind() const override { return KontKind::PathStartKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::PathStartKont);

    int child_count() const override {
        return 0;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct FieldAccessKont : public StaticKont {
    const char* field{};
    KontNode* base{};

    KontKind kind() const override { return KontKind::FieldAccessKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::FieldAccessKont);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->base, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct IndexAccessKont : public StaticKont {
    KontNode* index_expr{};
    KontNode* base{};

    KontKind kind() const override { return KontKind::IndexAccessKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::IndexAccessKont);

    int child_count() const override {
        return 0+ 1+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->index_expr, this->base, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

struct CaptureValueKont : public StaticKont {
    KontNode* path{};

    KontKind kind() const override { return KontKind::CaptureValueKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::CaptureValueKont);

    int child_count() const override {
        return 0+ 1;
    }
    KontNode* child(int i) const override {
        KontNode* fixed[] = { this->path, nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }
    int succ_count() const override {
        return 0;
    }
    KontNode* succ(int i) const override {
        KontNode* fixed[] = { nullptr };
        int nf = (int)(sizeof(fixed) / sizeof(fixed[0])) - 1;
        if (i >= 0 && i < nf) return fixed[i];
        return nullptr;
    }

private:
    void invoke(CEKMachine* m) const override;
};

// DynamicKont — sum base inheriting from the shared `KontNode` root.
struct DynamicKont : public KontNode {
};

struct ApplyAddKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyAddKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyAddKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplySubKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplySubKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplySubKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyMulKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyMulKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyMulKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyDivKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyDivKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyDivKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyModKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyModKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyModKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyBitAndKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyBitAndKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyBitAndKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyBitOrKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyBitOrKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyBitOrKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyBitXorKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyBitXorKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyBitXorKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyShlKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyShlKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyShlKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyShrKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyShrKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyShrKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyEqKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyEqKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyEqKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyNeKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyNeKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyNeKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyLtKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyLtKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyLtKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyLeKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyLeKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyLeKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyGtKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyGtKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyGtKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyGeKont : public DynamicKont {
    Value* saved_right{};

    KontKind kind() const override { return KontKind::ApplyGeKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyGeKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyNegKont : public DynamicKont {

    KontKind kind() const override { return KontKind::ApplyNegKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyNegKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyBitNotKont : public DynamicKont {

    KontKind kind() const override { return KontKind::ApplyBitNotKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyBitNotKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyNotKont : public DynamicKont {

    KontKind kind() const override { return KontKind::ApplyNotKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ApplyNotKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct EvalLeftKont : public DynamicKont {
    Value** saved_right_slot{};
    KontNode* left_kont{};

    KontKind kind() const override { return KontKind::EvalLeftKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::EvalLeftKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct LogicalAndCheckKont : public DynamicKont {
    KontNode* right{};

    KontKind kind() const override { return KontKind::LogicalAndCheckKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::LogicalAndCheckKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct LogicalOrCheckKont : public DynamicKont {
    KontNode* right{};

    KontKind kind() const override { return KontKind::LogicalOrCheckKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::LogicalOrCheckKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct NormalizeKont : public DynamicKont {

    KontKind kind() const override { return KontKind::NormalizeKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::NormalizeKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct TernaryDispatchKont : public DynamicKont {
    KontNode* then_{};
    KontNode* else_{};

    KontKind kind() const override { return KontKind::TernaryDispatchKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::TernaryDispatchKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ArgStoreKont : public DynamicKont {
    Value** args_buf{};
    int slot{};

    KontKind kind() const override { return KontKind::ArgStoreKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ArgStoreKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct CallApplyKont : public DynamicKont {
    const char* func_name{};
    Value** args_buf{};
    int arg_count{};

    KontKind kind() const override { return KontKind::CallApplyKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::CallApplyKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct FieldAccessApplyKont : public DynamicKont {
    const char* field{};

    KontKind kind() const override { return KontKind::FieldAccessApplyKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::FieldAccessApplyKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct IndexAccessApplyKont : public DynamicKont {
    Value* saved_index{};

    KontKind kind() const override { return KontKind::IndexAccessApplyKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::IndexAccessApplyKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct CaptureValueApplyKont : public DynamicKont {

    KontKind kind() const override { return KontKind::CaptureValueApplyKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::CaptureValueApplyKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct EvalConstraintCheckKont : public DynamicKont {
    KontNode* on_false{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::EvalConstraintCheckKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::EvalConstraintCheckKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct SwitchSelectKont : public DynamicKont {
    switch_case* cases = nullptr;
    int cases_count = 0;
    KontNode* default_target{};
    bool default_reject{};

    KontKind kind() const override { return KontKind::SwitchSelectKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::SwitchSelectKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct BindComputeStoreKont : public DynamicKont {
    KontNode* next{};

    KontKind kind() const override { return KontKind::BindComputeStoreKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BindComputeStoreKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginArrayWithLimitKont : public DynamicKont {
    const char* array_name{};
    ArrayMode mode{};
    KontNode* end_node{};
    KontNode* resync_target{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::BeginArrayWithLimitKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::BeginArrayWithLimitKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ArrayCountCheckKont : public DynamicKont {
    KontNode* end_node{};
    KontNode* body_entry{};

    KontKind kind() const override { return KontKind::ArrayCountCheckKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ArrayCountCheckKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct ArrayUntilCheckKont : public DynamicKont {
    KontNode* end_node{};
    KontNode* body_entry{};

    KontKind kind() const override { return KontKind::ArrayUntilCheckKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::ArrayUntilCheckKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct MatchBytesApplyKont : public DynamicKont {
    bool is_string{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::MatchBytesApplyKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::MatchBytesApplyKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct SetEndianApplyKont : public DynamicKont {
    KontNode* next{};

    KontKind kind() const override { return KontKind::SetEndianApplyKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::SetEndianApplyKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct SeekApplyKont : public DynamicKont {
    KontNode* next{};

    KontKind kind() const override { return KontKind::SeekApplyKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::SeekApplyKont);

private:
    void invoke(CEKMachine* m) const override;
};

struct PushIntervalApplyKont : public DynamicKont {
    bool relative{};
    KontNode* next{};

    KontKind kind() const override { return KontKind::PushIntervalApplyKont; }
    // Plain-int form of the kind tag — burg TERM ids (BURG_NODE_OP), since
    // KontKind is an enum class and won't implicitly convert to int.
    static constexpr int kind_value = static_cast<int>(KontKind::PushIntervalApplyKont);

private:
    void invoke(CEKMachine* m) const override;
};

} // namespace bbq::cek
