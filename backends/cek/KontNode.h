// ============================================================
// BBQ CEK IR — auto-generated from bbq_ir.asdl
// Do not edit by hand. Regenerate via the asdl tool.
// ============================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace bbq::cek {

// Forward declaration for the runtime that consumes nodes.
struct CEKMachine;

// FieldCapture lives in Capture.h. Forward-declare so FieldCaptureValue
// can reference it without a header dependency.
struct FieldCapture;

// ── Primitive type descriptor ───────────────────────────────

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
struct bitfield_entry;
struct IntValue;
struct FloatValue;
struct StringValue;
struct BoolValue;
struct FieldCaptureValue;
struct MatchPrimitiveNode;
struct BeginStructNode;
struct EndStructNode;
struct EvalConstraintNode;
struct BindComputeNode;
struct InvokeRuleNode;
struct ReturnRuleNode;
struct BeginChoiceNode;
struct CommitChoiceNode;
struct BeginOptionalNode;
struct EndOptionalNode;
struct BeginArrayNode;
struct ArrayNextNode;
struct EndArrayNode;
struct SwitchDispatchNode;
struct BitfieldReadNode;
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
struct AtEndKont;
struct EOIKont;
struct LoopVarKont;
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

private:
    friend struct CEKMachine;
    virtual void invoke(CEKMachine* m) const = 0;
};

// ── Product types (inline structs nested in node fields) ────

struct switch_case {
    Value* match_value{};
    KontNode* target{};
};

struct bitfield_entry {
    const char* name{};
    int width_bits{};
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

    FieldCapture* capture{};
};

// StaticKont — sum base inheriting from the shared `KontNode` root.
struct StaticKont : public KontNode {
};

struct MatchPrimitiveNode : public StaticKont {
    const char* field_name{};
    PrimitiveInfo prim{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginStructNode : public StaticKont {
    const char* struct_name{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct EndStructNode : public StaticKont {
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct EvalConstraintNode : public StaticKont {
    KontNode* constraint_expr{};
    KontNode* on_false{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BindComputeNode : public StaticKont {
    const char* field_name{};
    KontNode* compute_expr{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct InvokeRuleNode : public StaticKont {
    const char* rule_name{};
    KontNode* entry{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ReturnRuleNode : public StaticKont {
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginChoiceNode : public StaticKont {
    KontNode** alternatives = nullptr;
    int alternatives_count = 0;
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct CommitChoiceNode : public StaticKont {
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginOptionalNode : public StaticKont {
    KontNode* inner{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct EndOptionalNode : public StaticKont {
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginArrayNode : public StaticKont {
    const char* array_name{};
    ArrayMode mode{};
    KontNode* count_expr{};
    KontNode* end_node{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ArrayNextNode : public StaticKont {
    ArrayMode mode{};
    KontNode* count_expr{};
    KontNode* until_expr{};
    KontNode* end_node{};
    KontNode* body_entry{};

private:
    void invoke(CEKMachine* m) const override;
};

struct EndArrayNode : public StaticKont {
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct SwitchDispatchNode : public StaticKont {
    KontNode* discriminator_expr{};
    switch_case* cases = nullptr;
    int cases_count = 0;
    KontNode* default_target{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BitfieldReadNode : public StaticKont {
    PrimitiveInfo container{};
    bitfield_entry* entries = nullptr;
    int entries_count = 0;
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct SetEndianNode : public StaticKont {
    KontNode* endian_expr{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ExternalCallNode : public StaticKont {
    const char* func_name{};
    const char* field_name{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct MatchBytesNode : public StaticKont {
    const char* field_name{};
    KontNode* length_expr{};
    bool is_string{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct HaltNode : public StaticKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct IntLitKont : public StaticKont {
    int64_t v{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BoolLitKont : public StaticKont {
    bool v{};

private:
    void invoke(CEKMachine* m) const override;
};

struct FloatLitKont : public StaticKont {
    float v{};

private:
    void invoke(CEKMachine* m) const override;
};

struct StrLitKont : public StaticKont {
    const char* v{};

private:
    void invoke(CEKMachine* m) const override;
};

struct RefKont : public StaticKont {
    const char* name{};

private:
    void invoke(CEKMachine* m) const override;
};

struct PosKont : public StaticKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct RemainingKont : public StaticKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct AtEndKont : public StaticKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct EOIKont : public StaticKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct LoopVarKont : public StaticKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct NegKont : public StaticKont {
    KontNode* operand{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BitNotKont : public StaticKont {
    KontNode* operand{};

private:
    void invoke(CEKMachine* m) const override;
};

struct NotKont : public StaticKont {
    KontNode* operand{};

private:
    void invoke(CEKMachine* m) const override;
};

struct AddKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct SubKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct MulKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct DivKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ModKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BitAndKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BitOrKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BitXorKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ShlKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ShrKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct EqKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct NeKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct LtKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct LeKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct GtKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct GeKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct LogicalAndKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct LogicalOrKont : public StaticKont {
    KontNode* left{};
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct TernaryKont : public StaticKont {
    KontNode* cond{};
    KontNode* then_{};
    KontNode* else_{};

private:
    void invoke(CEKMachine* m) const override;
};

struct CallKont : public StaticKont {
    const char* func_name{};
    KontNode** args = nullptr;
    int args_count = 0;

private:
    void invoke(CEKMachine* m) const override;
};

struct PathStartKont : public StaticKont {
    const char* name{};

private:
    void invoke(CEKMachine* m) const override;
};

struct FieldAccessKont : public StaticKont {
    const char* field{};
    KontNode* base{};

private:
    void invoke(CEKMachine* m) const override;
};

struct IndexAccessKont : public StaticKont {
    KontNode* index_expr{};
    KontNode* base{};

private:
    void invoke(CEKMachine* m) const override;
};

struct CaptureValueKont : public StaticKont {
    KontNode* path{};

private:
    void invoke(CEKMachine* m) const override;
};

// DynamicKont — sum base inheriting from the shared `KontNode` root.
struct DynamicKont : public KontNode {
};

struct ApplyAddKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplySubKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyMulKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyDivKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyModKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyBitAndKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyBitOrKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyBitXorKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyShlKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyShrKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyEqKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyNeKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyLtKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyLeKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyGtKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyGeKont : public DynamicKont {
    Value* saved_right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyNegKont : public DynamicKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyBitNotKont : public DynamicKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct ApplyNotKont : public DynamicKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct EvalLeftKont : public DynamicKont {
    Value** saved_right_slot{};
    KontNode* left_kont{};

private:
    void invoke(CEKMachine* m) const override;
};

struct LogicalAndCheckKont : public DynamicKont {
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct LogicalOrCheckKont : public DynamicKont {
    KontNode* right{};

private:
    void invoke(CEKMachine* m) const override;
};

struct NormalizeKont : public DynamicKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct TernaryDispatchKont : public DynamicKont {
    KontNode* then_{};
    KontNode* else_{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ArgStoreKont : public DynamicKont {
    Value** args_buf{};
    int slot{};

private:
    void invoke(CEKMachine* m) const override;
};

struct CallApplyKont : public DynamicKont {
    const char* func_name{};
    Value** args_buf{};
    int arg_count{};

private:
    void invoke(CEKMachine* m) const override;
};

struct FieldAccessApplyKont : public DynamicKont {
    const char* field{};

private:
    void invoke(CEKMachine* m) const override;
};

struct IndexAccessApplyKont : public DynamicKont {
    Value* saved_index{};

private:
    void invoke(CEKMachine* m) const override;
};

struct CaptureValueApplyKont : public DynamicKont {

private:
    void invoke(CEKMachine* m) const override;
};

struct EvalConstraintCheckKont : public DynamicKont {
    KontNode* on_false{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct SwitchSelectKont : public DynamicKont {
    switch_case* cases = nullptr;
    int cases_count = 0;
    KontNode* default_target{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BindComputeStoreKont : public DynamicKont {
    const char* field_name{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct BeginArrayWithLimitKont : public DynamicKont {
    const char* array_name{};
    ArrayMode mode{};
    KontNode* end_node{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ArrayCountCheckKont : public DynamicKont {
    KontNode* end_node{};
    KontNode* body_entry{};

private:
    void invoke(CEKMachine* m) const override;
};

struct ArrayUntilCheckKont : public DynamicKont {
    KontNode* end_node{};
    KontNode* body_entry{};

private:
    void invoke(CEKMachine* m) const override;
};

struct MatchBytesApplyKont : public DynamicKont {
    const char* field_name{};
    bool is_string{};
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

struct SetEndianApplyKont : public DynamicKont {
    KontNode* next{};

private:
    void invoke(CEKMachine* m) const override;
};

} // namespace bbq::cek
