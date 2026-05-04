// Machine.cpp — Layer 1 stubs.
//
// The real CEK trampoline + 94 invoke bodies arrive in Layer 2. During
// Layer 1, every kont's invoke() is a stub that calls m->fail() so the
// project compiles and the type system catches signature drift.
//
// Helpers (bswap, read_primitive, primitive_capture_type) remain in
// place — Layer 2 uses them from the new structural invokes.

#include "Machine.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bbq::cek {

// --- Byte swap helpers ---

static uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

static constexpr bool host_little_endian() {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return true;
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return false;
#else
    return true;
#endif
}

// --- read_primitive ---

int64_t read_primitive(const uint8_t* data, size_t pos, PrimitiveInfo prim,
                       bool little_endian) {
    const uint8_t* p = data + pos;

    if (prim.kind == PrimKind::Bool) {
        return p[0] != 0 ? 1 : 0;
    }

    bool want_le = (prim.endian == PrimEndian::Native) ? little_endian
                                                       : (prim.endian == PrimEndian::Little);

    if (prim.kind == PrimKind::Float) {
        if (prim.width == PrimWidth::W32) {
            uint32_t raw;
            std::memcpy(&raw, p, 4);
            if (want_le != host_little_endian()) raw = bswap32(raw);
            float f;
            std::memcpy(&f, &raw, 4);
            return static_cast<int64_t>(f);
        } else {
            uint64_t raw;
            std::memcpy(&raw, p, 8);
            if (want_le != host_little_endian()) raw = bswap64(raw);
            double d;
            std::memcpy(&d, &raw, 8);
            return static_cast<int64_t>(d);
        }
    }

    // Integer
    switch (prim.width) {
        case PrimWidth::W8: return prim.sign == PrimSign::Signed
            ? static_cast<int64_t>(static_cast<int8_t>(p[0]))
            : static_cast<int64_t>(p[0]);
        case PrimWidth::W16: {
            uint16_t v; std::memcpy(&v, p, 2);
            if (want_le != host_little_endian()) v = bswap16(v);
            return prim.sign == PrimSign::Signed
                ? static_cast<int64_t>(static_cast<int16_t>(v))
                : static_cast<int64_t>(v);
        }
        case PrimWidth::W32: {
            uint32_t v; std::memcpy(&v, p, 4);
            if (want_le != host_little_endian()) v = bswap32(v);
            return prim.sign == PrimSign::Signed
                ? static_cast<int64_t>(static_cast<int32_t>(v))
                : static_cast<int64_t>(v);
        }
        case PrimWidth::W64: {
            uint64_t v; std::memcpy(&v, p, 8);
            if (want_le != host_little_endian()) v = bswap64(v);
            return static_cast<int64_t>(v);
        }
    }
    return 0;
}

// --- primitive_capture_type ---

CaptureType primitive_capture_type(PrimitiveInfo prim, bool little_endian) {
    bool want_le = (prim.endian == PrimEndian::Native) ? little_endian
                                                       : (prim.endian == PrimEndian::Little);

    if (prim.kind == PrimKind::Bool) return CaptureType::Bool;

    if (prim.kind == PrimKind::Float) {
        if (prim.width == PrimWidth::W32)
            return want_le ? CaptureType::Float32LE : CaptureType::Float32BE;
        return want_le ? CaptureType::Float64LE : CaptureType::Float64BE;
    }

    if (prim.sign == PrimSign::Signed) {
        switch (prim.width) {
            case PrimWidth::W8:  return CaptureType::Int8;
            case PrimWidth::W16: return want_le ? CaptureType::Int16LE : CaptureType::Int16BE;
            case PrimWidth::W32: return want_le ? CaptureType::Int32LE : CaptureType::Int32BE;
            case PrimWidth::W64: return want_le ? CaptureType::Int64LE : CaptureType::Int64BE;
        }
    } else {
        switch (prim.width) {
            case PrimWidth::W8:  return CaptureType::UInt8;
            case PrimWidth::W16: return want_le ? CaptureType::UInt16LE : CaptureType::UInt16BE;
            case PrimWidth::W32: return want_le ? CaptureType::UInt32LE : CaptureType::UInt32BE;
            case PrimWidth::W64: return want_le ? CaptureType::UInt64LE : CaptureType::UInt64BE;
        }
    }
    return CaptureType::UInt8;
}

// --- CEKMachine: trampoline + fail() ---

void CEKMachine::fail(const char* message) {
    if (pos >= best_error_pos) {
        best_error_pos = pos;
        best_error_msg = message;
    }

    // Walk frame_top searching for an AlternativeFrame with state to
    // restore from. Frames that aren't AlternativeFrames (ReturnFrame,
    // LoopFrame) get popped — failure unwinds through them.
    while (frame_top != nullptr) {
        if (frame_top->type == FrameType::Alternative) {
            auto* alt = static_cast<AlternativeFrame*>(frame_top);
            if (alt->remaining_count > 0) {
                // Restore state and try next alternative
                pos = alt->saved_pos;
                env = alt->saved_env;
                little_endian = alt->saved_endian;
                builder.restore(alt->capture_mark);
                kont_stack.resize(alt->saved_kont_size);
                KontNode* next_alt = alt->remaining[0];
                alt->remaining++;
                alt->remaining_count--;
                kont_stack.push_back(next_alt);
                return;
            }
            // Frame with 0 remaining alternatives:
            //   join != nullptr → optional (restore + skip to join)
            //   join == nullptr → choice exhausted (keep searching)
            if (alt->join != nullptr) {
                pos = alt->saved_pos;
                env = alt->saved_env;
                little_endian = alt->saved_endian;
                builder.restore(alt->capture_mark);
                kont_stack.resize(alt->saved_kont_size);
                frame_top = alt->prev;
                kont_stack.push_back(alt->join);
                return;
            }
        }
        // Pop the frame (non-Alternative, or exhausted Choice)
        frame_top = frame_top->prev;
    }

    // No alternative remains — parse fails
    failed = true;
}

CaptureMetadata CEKMachine::execute_from(KontNode* entry,
                                         const uint8_t* data,
                                         size_t length,
                                         bool default_little_endian) {
    // Initialize machine state for a fresh parse. env and frame_top
    // are NOT reset — callers (production drivers, tests) are
    // responsible for setting them up before invocation. Production
    // drivers leave them null; tests may inject saved state.
    input = data;
    input_length = length;
    pos = 0;
    little_endian = default_little_endian;
    result = nullptr;
    failed = false;
    best_error_pos = 0;
    best_error_msg = nullptr;
    kont_stack.clear();
    builder.clear();

    if (entry == nullptr) {
        return builder.finish(*arena, false, 0,
                              "no entry kont", 0);
    }
    kont_stack.push_back(entry);

    // K-pop trampoline. C is loaded from K's top each iteration;
    // never advanced via control->next.
    while (!failed && !kont_stack.empty()) {
        control = kont_stack.back();
        kont_stack.pop_back();
        control->invoke(this);
    }

    if (failed) {
        return builder.finish(*arena, false, pos,
                              best_error_msg ? best_error_msg : "parse failed",
                              best_error_pos);
    }
    return builder.finish(*arena, true, pos);
}

// --- Layer 2 leaf kont invokes ---
//
// Each leaf allocates a typed Value in the per-parse arena and writes
// the pointer to ac. Pushes nothing onto K — the leaf is consumed by
// whatever parent kont is below it on K (an apply, eval-left, check,
// or site-consumer kont).

void IntLitKont::invoke(CEKMachine* m) const {
    auto* iv = m->arena->alloc<IntValue>();
    iv->v = this->v;
    m->result = iv;
}

void BoolLitKont::invoke(CEKMachine* m) const {
    auto* bv = m->arena->alloc<BoolValue>();
    bv->v = this->v;
    m->result = bv;
}

void FloatLitKont::invoke(CEKMachine* m) const {
    auto* fv = m->arena->alloc<FloatValue>();
    fv->v = this->v;
    m->result = fv;
}

void StrLitKont::invoke(CEKMachine* m) const {
    auto* sv = m->arena->alloc<StringValue>();
    sv->v = this->v;
    m->result = sv;
}

void RefKont::invoke(CEKMachine* m) const {
    if (m->env == nullptr) {
        m->fail("undefined reference (env empty)");
        return;
    }
    const Binding* b = m->env->lookup(this->name);
    if (b == nullptr) {
        m->fail("undefined reference");
        return;
    }
    // Binding's value is already a typed Value*; copy the pointer.
    m->result = b->value;
}

void PosKont::invoke(CEKMachine* m) const {
    auto* iv = m->arena->alloc<IntValue>();
    iv->v = static_cast<int64_t>(m->pos);
    m->result = iv;
}

void RemainingKont::invoke(CEKMachine* m) const {
    auto* iv = m->arena->alloc<IntValue>();
    iv->v = static_cast<int64_t>(m->input_length - m->pos);
    m->result = iv;
}

void AtEndKont::invoke(CEKMachine* m) const {
    auto* bv = m->arena->alloc<BoolValue>();
    bv->v = (m->pos >= m->input_length);
    m->result = bv;
}

void EOIKont::invoke(CEKMachine* m) const {
    auto* iv = m->arena->alloc<IntValue>();
    iv->v = static_cast<int64_t>(m->input_length);
    m->result = iv;
}

void LoopVarKont::invoke(CEKMachine* m) const {
    // Walk the frame stack for the innermost LoopFrame.
    for (Frame* f = m->frame_top; f != nullptr; f = f->prev) {
        if (f->type == FrameType::Loop) {
            auto* lf = static_cast<LoopFrame*>(f);
            auto* iv = m->arena->alloc<IntValue>();
            iv->v = lf->counter;
            m->result = iv;
            return;
        }
    }
    m->fail("loop variable used outside array");
}

// ── Expression operators: eval-and-apply discipline ────────
//
// Each binary operator-shape kont allocates a fresh apply kont (in the
// per-parse arena), allocates a shared EvalLeftKont, and pushes the
// three onto K in reverse execution order: apply (runs last), eval-left
// (runs after right), right (runs first). When right's IR finishes, ac
// holds the right Value; eval-left writes it into apply's saved_right
// slot and pushes left. After left's IR finishes, ac holds the left
// Value; apply runs, dispatches on the (left, right) type pair, and
// writes the result Value to ac.

void EvalLeftKont::invoke(CEKMachine* m) const {
    *this->saved_right_slot = m->result;
    m->push_kont(this->left_kont);
}

#define DEFINE_BINOP_KONT(OpKont, ApplyKont)                                  \
    void OpKont::invoke(CEKMachine* m) const {                                \
        auto* apply = m->arena->alloc<ApplyKont>();                           \
        auto* el = m->arena->alloc<EvalLeftKont>();                           \
        el->saved_right_slot = &apply->saved_right;                           \
        el->left_kont = this->left;                                           \
        m->push_kont(apply);                                                  \
        m->push_kont(el);                                                     \
        m->push_kont(this->right);                                            \
    }

DEFINE_BINOP_KONT(AddKont,    ApplyAddKont)
DEFINE_BINOP_KONT(SubKont,    ApplySubKont)
DEFINE_BINOP_KONT(MulKont,    ApplyMulKont)
DEFINE_BINOP_KONT(DivKont,    ApplyDivKont)
DEFINE_BINOP_KONT(ModKont,    ApplyModKont)
DEFINE_BINOP_KONT(BitAndKont, ApplyBitAndKont)
DEFINE_BINOP_KONT(BitOrKont,  ApplyBitOrKont)
DEFINE_BINOP_KONT(BitXorKont, ApplyBitXorKont)
DEFINE_BINOP_KONT(ShlKont,    ApplyShlKont)
DEFINE_BINOP_KONT(ShrKont,    ApplyShrKont)
DEFINE_BINOP_KONT(EqKont,     ApplyEqKont)
DEFINE_BINOP_KONT(NeKont,     ApplyNeKont)
DEFINE_BINOP_KONT(LtKont,     ApplyLtKont)
DEFINE_BINOP_KONT(LeKont,     ApplyLeKont)
DEFINE_BINOP_KONT(GtKont,     ApplyGtKont)
DEFINE_BINOP_KONT(GeKont,     ApplyGeKont)

#undef DEFINE_BINOP_KONT

// Numeric arithmetic apply: same-type Int+Int or Float+Float; cross-type fails.
#define DEFINE_ARITH_APPLY(ApplyKont, op_str, int_op, float_op)               \
    void ApplyKont::invoke(CEKMachine* m) const {                             \
        Value* lhs = m->result;                                               \
        Value* rhs = this->saved_right;                                       \
        if (!lhs || !rhs) { m->fail("type error: " op_str); return; }         \
        if (lhs->tag == ValueTag::IntValue && rhs->tag == ValueTag::IntValue) {\
            int64_t a = static_cast<IntValue*>(lhs)->v;                       \
            int64_t b = static_cast<IntValue*>(rhs)->v;                       \
            (void)a; (void)b;                                                 \
            auto* r = m->arena->alloc<IntValue>();                            \
            r->v = (int_op);                                                  \
            m->result = r;                                                    \
        } else if (lhs->tag == ValueTag::FloatValue && rhs->tag == ValueTag::FloatValue) { \
            double a = static_cast<FloatValue*>(lhs)->v;                      \
            double b = static_cast<FloatValue*>(rhs)->v;                      \
            (void)a; (void)b;                                                 \
            auto* r = m->arena->alloc<FloatValue>();                          \
            r->v = (float_op);                                                \
            m->result = r;                                                    \
        } else {                                                              \
            m->fail("type error: " op_str);                                   \
        }                                                                     \
    }

DEFINE_ARITH_APPLY(ApplyAddKont, "+", a + b, a + b)
DEFINE_ARITH_APPLY(ApplySubKont, "-", a - b, a - b)
DEFINE_ARITH_APPLY(ApplyMulKont, "*", a * b, a * b)

#undef DEFINE_ARITH_APPLY

// Division: int needs zero-check; float follows IEEE.
void ApplyDivKont::invoke(CEKMachine* m) const {
    Value* lhs = m->result;
    Value* rhs = this->saved_right;
    if (!lhs || !rhs) { m->fail("type error: /"); return; }
    if (lhs->tag == ValueTag::IntValue && rhs->tag == ValueTag::IntValue) {
        int64_t b = static_cast<IntValue*>(rhs)->v;
        if (b == 0) { m->fail("division by zero"); return; }
        auto* r = m->arena->alloc<IntValue>();
        r->v = static_cast<IntValue*>(lhs)->v / b;
        m->result = r;
    } else if (lhs->tag == ValueTag::FloatValue && rhs->tag == ValueTag::FloatValue) {
        auto* r = m->arena->alloc<FloatValue>();
        r->v = static_cast<FloatValue*>(lhs)->v / static_cast<FloatValue*>(rhs)->v;
        m->result = r;
    } else {
        m->fail("type error: /");
    }
}

// Modulo: int-only.
void ApplyModKont::invoke(CEKMachine* m) const {
    Value* lhs = m->result;
    Value* rhs = this->saved_right;
    if (!lhs || !rhs || lhs->tag != ValueTag::IntValue || rhs->tag != ValueTag::IntValue) {
        m->fail("type error: %");
        return;
    }
    int64_t b = static_cast<IntValue*>(rhs)->v;
    if (b == 0) { m->fail("division by zero"); return; }
    auto* r = m->arena->alloc<IntValue>();
    r->v = static_cast<IntValue*>(lhs)->v % b;
    m->result = r;
}

// Bitwise apply: integer-only.
#define DEFINE_BITWISE_APPLY(ApplyKont, op_str, int_op)                       \
    void ApplyKont::invoke(CEKMachine* m) const {                             \
        Value* lhs = m->result;                                               \
        Value* rhs = this->saved_right;                                       \
        if (!lhs || !rhs || lhs->tag != ValueTag::IntValue || rhs->tag != ValueTag::IntValue) { \
            m->fail("type error: " op_str);                                   \
            return;                                                           \
        }                                                                     \
        int64_t a = static_cast<IntValue*>(lhs)->v;                           \
        int64_t b = static_cast<IntValue*>(rhs)->v;                           \
        (void)a; (void)b;                                                     \
        auto* r = m->arena->alloc<IntValue>();                                \
        r->v = (int_op);                                                      \
        m->result = r;                                                        \
    }

DEFINE_BITWISE_APPLY(ApplyBitAndKont, "&",  a & b)
DEFINE_BITWISE_APPLY(ApplyBitOrKont,  "|",  a | b)
DEFINE_BITWISE_APPLY(ApplyBitXorKont, "^",  a ^ b)
DEFINE_BITWISE_APPLY(ApplyShlKont,    "<<", a << b)
DEFINE_BITWISE_APPLY(ApplyShrKont,    ">>", a >> b)

#undef DEFINE_BITWISE_APPLY

// Equality: same-type compares; cross-type → false (==) / true (!=).
static Value* alloc_bool(CEKMachine* m, bool b) {
    auto* bv = m->arena->alloc<BoolValue>();
    bv->v = b;
    return bv;
}

void ApplyEqKont::invoke(CEKMachine* m) const {
    Value* lhs = m->result;
    Value* rhs = this->saved_right;
    if (!lhs || !rhs) { m->fail("type error: =="); return; }
    if (lhs->tag != rhs->tag) { m->result = alloc_bool(m, false); return; }
    bool eq = false;
    switch (lhs->tag) {
        case ValueTag::IntValue:
            eq = static_cast<IntValue*>(lhs)->v == static_cast<IntValue*>(rhs)->v;
            break;
        case ValueTag::FloatValue:
            eq = static_cast<FloatValue*>(lhs)->v == static_cast<FloatValue*>(rhs)->v;
            break;
        case ValueTag::StringValue:
            // Interned strings compare by pointer.
            eq = static_cast<StringValue*>(lhs)->v == static_cast<StringValue*>(rhs)->v;
            break;
        case ValueTag::BoolValue:
            eq = static_cast<BoolValue*>(lhs)->v == static_cast<BoolValue*>(rhs)->v;
            break;
        case ValueTag::FieldCaptureValue:
            eq = static_cast<FieldCaptureValue*>(lhs)->capture
              == static_cast<FieldCaptureValue*>(rhs)->capture;
            break;
    }
    m->result = alloc_bool(m, eq);
}

void ApplyNeKont::invoke(CEKMachine* m) const {
    Value* lhs = m->result;
    Value* rhs = this->saved_right;
    if (!lhs || !rhs) { m->fail("type error: !="); return; }
    if (lhs->tag != rhs->tag) { m->result = alloc_bool(m, true); return; }
    bool eq = false;
    switch (lhs->tag) {
        case ValueTag::IntValue:
            eq = static_cast<IntValue*>(lhs)->v == static_cast<IntValue*>(rhs)->v;
            break;
        case ValueTag::FloatValue:
            eq = static_cast<FloatValue*>(lhs)->v == static_cast<FloatValue*>(rhs)->v;
            break;
        case ValueTag::StringValue:
            eq = static_cast<StringValue*>(lhs)->v == static_cast<StringValue*>(rhs)->v;
            break;
        case ValueTag::BoolValue:
            eq = static_cast<BoolValue*>(lhs)->v == static_cast<BoolValue*>(rhs)->v;
            break;
        case ValueTag::FieldCaptureValue:
            eq = static_cast<FieldCaptureValue*>(lhs)->capture
              == static_cast<FieldCaptureValue*>(rhs)->capture;
            break;
    }
    m->result = alloc_bool(m, !eq);
}

// Ordered comparison: same-type required (int, float, string); cross-type fails.
#define DEFINE_ORDERED_APPLY(ApplyKont, op_str, op_token)                     \
    void ApplyKont::invoke(CEKMachine* m) const {                             \
        Value* lhs = m->result;                                               \
        Value* rhs = this->saved_right;                                       \
        if (!lhs || !rhs || lhs->tag != rhs->tag) {                           \
            m->fail("type error: " op_str);                                   \
            return;                                                           \
        }                                                                     \
        bool b = false;                                                       \
        switch (lhs->tag) {                                                   \
            case ValueTag::IntValue:                                          \
                b = static_cast<IntValue*>(lhs)->v op_token static_cast<IntValue*>(rhs)->v; \
                break;                                                        \
            case ValueTag::FloatValue:                                        \
                b = static_cast<FloatValue*>(lhs)->v op_token static_cast<FloatValue*>(rhs)->v; \
                break;                                                        \
            case ValueTag::StringValue: {                                     \
                int cmp = std::strcmp(static_cast<StringValue*>(lhs)->v,      \
                                      static_cast<StringValue*>(rhs)->v);     \
                b = cmp op_token 0;                                           \
                break;                                                        \
            }                                                                 \
            default:                                                          \
                m->fail("type error: " op_str);                               \
                return;                                                       \
        }                                                                     \
        m->result = alloc_bool(m, b);                                         \
    }

DEFINE_ORDERED_APPLY(ApplyLtKont, "<",  <)
DEFINE_ORDERED_APPLY(ApplyLeKont, "<=", <=)
DEFINE_ORDERED_APPLY(ApplyGtKont, ">",  >)
DEFINE_ORDERED_APPLY(ApplyGeKont, ">=", >=)

#undef DEFINE_ORDERED_APPLY

// ── Unary operators ────────────────────────────────────────

#define DEFINE_UNARY_KONT(OpKont, ApplyKont)              \
    void OpKont::invoke(CEKMachine* m) const {            \
        auto* apply = m->arena->alloc<ApplyKont>();       \
        m->push_kont(apply);                              \
        m->push_kont(this->operand);                      \
    }

DEFINE_UNARY_KONT(NegKont,    ApplyNegKont)
DEFINE_UNARY_KONT(BitNotKont, ApplyBitNotKont)
DEFINE_UNARY_KONT(NotKont,    ApplyNotKont)

#undef DEFINE_UNARY_KONT

void ApplyNegKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v) { m->fail("type error: unary -"); return; }
    if (v->tag == ValueTag::IntValue) {
        auto* r = m->arena->alloc<IntValue>();
        r->v = -static_cast<IntValue*>(v)->v;
        m->result = r;
    } else if (v->tag == ValueTag::FloatValue) {
        auto* r = m->arena->alloc<FloatValue>();
        r->v = -static_cast<FloatValue*>(v)->v;
        m->result = r;
    } else {
        m->fail("type error: unary -");
    }
}

void ApplyBitNotKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v || v->tag != ValueTag::IntValue) { m->fail("type error: ~"); return; }
    auto* r = m->arena->alloc<IntValue>();
    r->v = ~static_cast<IntValue*>(v)->v;
    m->result = r;
}

// Helper: coerce a Value to bool for logical ops / consumer konts.
// Returns true on Bool, nonzero-Int → true; sets has_value=false on
// type mismatch (caller calls fail).
static bool coerce_to_bool(Value* v, bool& has_value) {
    if (!v) { has_value = false; return false; }
    if (v->tag == ValueTag::BoolValue) { has_value = true; return static_cast<BoolValue*>(v)->v; }
    if (v->tag == ValueTag::IntValue)  { has_value = true; return static_cast<IntValue*>(v)->v != 0; }
    has_value = false;
    return false;
}

void ApplyNotKont::invoke(CEKMachine* m) const {
    bool has_value;
    bool b = coerce_to_bool(m->result, has_value);
    if (!has_value) { m->fail("type error: !"); return; }
    m->result = alloc_bool(m, !b);
}

// ── Logical short-circuit ─────────────────────────────────
//
// Logical-and: evaluate left; if false, ac stays false (skip right);
// if true, evaluate right and normalize to a clean BoolValue.
// Logical-or: dual.

void LogicalAndKont::invoke(CEKMachine* m) const {
    auto* check = m->arena->alloc<LogicalAndCheckKont>();
    check->right = this->right;
    m->push_kont(check);
    m->push_kont(this->left);
}

void LogicalOrKont::invoke(CEKMachine* m) const {
    auto* check = m->arena->alloc<LogicalOrCheckKont>();
    check->right = this->right;
    m->push_kont(check);
    m->push_kont(this->left);
}

void LogicalAndCheckKont::invoke(CEKMachine* m) const {
    bool has_value;
    bool b = coerce_to_bool(m->result, has_value);
    if (!has_value) { m->fail("type error: && (left)"); return; }
    if (!b) {
        // Short-circuit — ac becomes BoolValue(false).
        m->result = alloc_bool(m, false);
        return;
    }
    // Right runs; normalize its result to a clean Bool.
    auto* norm = m->arena->alloc<NormalizeKont>();
    m->push_kont(norm);
    m->push_kont(this->right);
}

void LogicalOrCheckKont::invoke(CEKMachine* m) const {
    bool has_value;
    bool b = coerce_to_bool(m->result, has_value);
    if (!has_value) { m->fail("type error: || (left)"); return; }
    if (b) {
        m->result = alloc_bool(m, true);
        return;
    }
    auto* norm = m->arena->alloc<NormalizeKont>();
    m->push_kont(norm);
    m->push_kont(this->right);
}

void NormalizeKont::invoke(CEKMachine* m) const {
    bool has_value;
    bool b = coerce_to_bool(m->result, has_value);
    if (!has_value) { m->fail("type error: && or || (right)"); return; }
    m->result = alloc_bool(m, b);
}

// ── Ternary ────────────────────────────────────────────────

void TernaryKont::invoke(CEKMachine* m) const {
    auto* dispatch = m->arena->alloc<TernaryDispatchKont>();
    dispatch->then_ = this->then_;
    dispatch->else_ = this->else_;
    m->push_kont(dispatch);
    m->push_kont(this->cond);
}

void TernaryDispatchKont::invoke(CEKMachine* m) const {
    bool has_value;
    bool b = coerce_to_bool(m->result, has_value);
    if (!has_value) { m->fail("type error: ternary cond"); return; }
    m->push_kont(b ? this->then_ : this->else_);
}

// ── Function call ─────────────────────────────────────────
//
// CallKont allocates an args buffer, an apply, and N ArgStoreKont
// frames to populate the buffer. Args evaluate left-to-right (push
// in reverse for LIFO; ArgStoreKont's slot index handles ordering).

void CallKont::invoke(CEKMachine* m) const {
    int n = this->args_count;
    auto* buf = static_cast<Value**>(
        m->arena->allocate(sizeof(Value*) * (n > 0 ? n : 1), alignof(Value*)));
    for (int i = 0; i < n; ++i) buf[i] = nullptr;

    auto* apply = m->arena->alloc<CallApplyKont>();
    apply->func_name = this->func_name;
    apply->args_buf  = buf;
    apply->arg_count = n;
    m->push_kont(apply);

    // Push in reverse — top of K runs first. We want left-to-right
    // arg evaluation, so push (store_n, arg_n) first ... actually
    // in LIFO the last-pushed runs first, so to get arg_0 first,
    // we push apply, store_n_minus_1, arg_n_minus_1, ..., store_0,
    // arg_0. Then arg_0 runs first → store_0 → arg_1 → store_1 → ... → apply.
    for (int i = n - 1; i >= 0; --i) {
        auto* store = m->arena->alloc<ArgStoreKont>();
        store->args_buf = buf;
        store->slot = i;
        m->push_kont(store);
        m->push_kont(this->args[i]);
    }
}

void ArgStoreKont::invoke(CEKMachine* m) const {
    this->args_buf[this->slot] = m->result;
}

void CallApplyKont::invoke(CEKMachine* m) const {
    // Function table lookup is only used by user functions; built-ins
    // (abs/min/max/clamp) aren't hooked up yet at this layer. For now,
    // dispatch to a minimal built-in set inline.
    const char* name = this->func_name;
    Value** args = this->args_buf;
    int n = this->arg_count;

    auto fail_and_return = [m, name]() { m->fail(name); };

    // abs(x) — int or float
    if (name && std::strcmp(name, "abs") == 0 && n == 1) {
        Value* v = args[0];
        if (!v) { fail_and_return(); return; }
        if (v->tag == ValueTag::IntValue) {
            auto* r = m->arena->alloc<IntValue>();
            int64_t x = static_cast<IntValue*>(v)->v;
            r->v = x < 0 ? -x : x;
            m->result = r;
            return;
        }
        if (v->tag == ValueTag::FloatValue) {
            auto* r = m->arena->alloc<FloatValue>();
            double x = static_cast<FloatValue*>(v)->v;
            r->v = x < 0 ? -x : x;
            m->result = r;
            return;
        }
        fail_and_return();
        return;
    }
    // min / max — int or float, two args
    if (name && (std::strcmp(name, "min") == 0 || std::strcmp(name, "max") == 0) && n == 2) {
        bool is_max = std::strcmp(name, "max") == 0;
        Value* a = args[0]; Value* b = args[1];
        if (!a || !b || a->tag != b->tag) { fail_and_return(); return; }
        if (a->tag == ValueTag::IntValue) {
            auto* r = m->arena->alloc<IntValue>();
            int64_t x = static_cast<IntValue*>(a)->v;
            int64_t y = static_cast<IntValue*>(b)->v;
            r->v = is_max ? (x > y ? x : y) : (x < y ? x : y);
            m->result = r;
            return;
        }
        if (a->tag == ValueTag::FloatValue) {
            auto* r = m->arena->alloc<FloatValue>();
            double x = static_cast<FloatValue*>(a)->v;
            double y = static_cast<FloatValue*>(b)->v;
            r->v = is_max ? (x > y ? x : y) : (x < y ? x : y);
            m->result = r;
            return;
        }
        fail_and_return();
        return;
    }
    // clamp(x, lo, hi)
    if (name && std::strcmp(name, "clamp") == 0 && n == 3) {
        Value* x = args[0]; Value* lo = args[1]; Value* hi = args[2];
        if (!x || !lo || !hi || x->tag != lo->tag || x->tag != hi->tag) { fail_and_return(); return; }
        if (x->tag == ValueTag::IntValue) {
            int64_t xv = static_cast<IntValue*>(x)->v;
            int64_t lov = static_cast<IntValue*>(lo)->v;
            int64_t hiv = static_cast<IntValue*>(hi)->v;
            int64_t r = xv < lov ? lov : (xv > hiv ? hiv : xv);
            auto* rv = m->arena->alloc<IntValue>();
            rv->v = r;
            m->result = rv;
            return;
        }
        if (x->tag == ValueTag::FloatValue) {
            double xv = static_cast<FloatValue*>(x)->v;
            double lov = static_cast<FloatValue*>(lo)->v;
            double hiv = static_cast<FloatValue*>(hi)->v;
            double r = xv < lov ? lov : (xv > hiv ? hiv : xv);
            auto* rv = m->arena->alloc<FloatValue>();
            rv->v = r;
            m->result = rv;
            return;
        }
        fail_and_return();
        return;
    }
    m->fail("unknown function");
}

// ── Path navigation ───────────────────────────────────────

void PathStartKont::invoke(CEKMachine* m) const {
    const FieldCapture* cap = m->builder.find_field(this->name);
    if (!cap) { m->fail("path: field not found"); return; }
    auto* fcv = m->arena->alloc<FieldCaptureValue>();
    fcv->capture = const_cast<FieldCapture*>(cap);
    m->result = fcv;
}

void FieldAccessKont::invoke(CEKMachine* m) const {
    auto* apply = m->arena->alloc<FieldAccessApplyKont>();
    apply->field = this->field;
    m->push_kont(apply);
    m->push_kont(this->base);
}

void FieldAccessApplyKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v || v->tag != ValueTag::FieldCaptureValue) { m->fail("path: bad base"); return; }
    auto* base = static_cast<FieldCaptureValue*>(v)->capture;
    const FieldCapture* child = m->builder.find_child(base, this->field);
    if (!child) { m->fail("path: field not in capture"); return; }
    auto* fcv = m->arena->alloc<FieldCaptureValue>();
    fcv->capture = const_cast<FieldCapture*>(child);
    m->result = fcv;
}

void IndexAccessKont::invoke(CEKMachine* m) const {
    auto* apply = m->arena->alloc<IndexAccessApplyKont>();
    auto* el = m->arena->alloc<EvalLeftKont>();
    el->saved_right_slot = &apply->saved_index;
    el->left_kont = this->base;
    m->push_kont(apply);
    m->push_kont(el);
    m->push_kont(this->index_expr);
}

void IndexAccessApplyKont::invoke(CEKMachine* m) const {
    // ac holds base FieldCaptureValue; saved_index holds the index.
    Value* base_v = m->result;
    Value* idx_v = this->saved_index;
    if (!base_v || base_v->tag != ValueTag::FieldCaptureValue) { m->fail("path: bad base for index"); return; }
    if (!idx_v || idx_v->tag != ValueTag::IntValue) { m->fail("path: index not integer"); return; }
    auto* base = static_cast<FieldCaptureValue*>(base_v)->capture;
    int idx = static_cast<int>(static_cast<IntValue*>(idx_v)->v);
    const FieldCapture* child = m->builder.find_child_at(base, idx);
    if (!child) { m->fail("path: index out of range"); return; }
    auto* fcv = m->arena->alloc<FieldCaptureValue>();
    fcv->capture = const_cast<FieldCapture*>(child);
    m->result = fcv;
}

void CaptureValueKont::invoke(CEKMachine* m) const {
    auto* apply = m->arena->alloc<CaptureValueApplyKont>();
    m->push_kont(apply);
    m->push_kont(this->path);
}

void CaptureValueApplyKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v || v->tag != ValueTag::FieldCaptureValue) { m->fail("path: capture-value on non-path"); return; }
    auto* cap = static_cast<FieldCaptureValue*>(v)->capture;
    if (cap->computed_value == nullptr) {
        m->fail("path: no computed value");
        return;
    }
    m->result = cap->computed_value;
}

// ── Site consumers (8) ────────────────────────────────────
//
// Each site kont (EvalConstraint/Switch/BindCompute/etc.) pushes its
// site-consumer dynamic kont onto K, then pushes the expression IR.
// The expression evaluates first; the consumer reads ac and acts.

void EvalConstraintNode::invoke(CEKMachine* m) const {
    auto* check = m->arena->alloc<EvalConstraintCheckKont>();
    check->on_false = this->on_false;
    check->next = this->next;
    m->push_kont(check);
    m->push_kont(this->constraint_expr);
}

void EvalConstraintCheckKont::invoke(CEKMachine* m) const {
    bool has_value;
    bool b = coerce_to_bool(m->result, has_value);
    if (!has_value) { m->fail("constraint expects bool"); return; }
    if (b) {
        m->push_kont(this->next);
    } else if (this->on_false) {
        m->push_kont(this->on_false);
    } else {
        m->fail("constraint failed");
    }
}

void BindComputeNode::invoke(CEKMachine* m) const {
    auto* store = m->arena->alloc<BindComputeStoreKont>();
    store->field_name = this->field_name;
    store->next = this->next;
    m->push_kont(store);
    m->push_kont(this->compute_expr);
}

void BindComputeStoreKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v) { m->fail("bind-compute: null value"); return; }
    m->builder.add_computed(this->field_name, v, m->pos);

    auto* new_env = m->arena->alloc<Environment>();
    new_env->binding.name = this->field_name;
    new_env->binding.value = v;
    new_env->binding.start_offset = m->pos;
    new_env->binding.end_offset = m->pos;
    new_env->binding.type = CaptureType::Computed;
    new_env->parent = m->env;
    m->env = new_env;

    m->push_kont(this->next);
}

void SetEndianNode::invoke(CEKMachine* m) const {
    auto* apply = m->arena->alloc<SetEndianApplyKont>();
    apply->next = this->next;
    m->push_kont(apply);
    m->push_kont(this->endian_expr);
}

void SetEndianApplyKont::invoke(CEKMachine* m) const {
    bool has_value;
    bool b = coerce_to_bool(m->result, has_value);
    if (!has_value) { m->fail("@endian expects bool"); return; }
    m->little_endian = b;
    m->push_kont(this->next);
}

void MatchBytesNode::invoke(CEKMachine* m) const {
    auto* apply = m->arena->alloc<MatchBytesApplyKont>();
    apply->field_name = this->field_name;
    apply->is_string = this->is_string;
    apply->next = this->next;
    m->push_kont(apply);
    m->push_kont(this->length_expr);
}

void MatchBytesApplyKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v || v->tag != ValueTag::IntValue) { m->fail("bytes length not integer"); return; }
    int64_t len = static_cast<IntValue*>(v)->v;
    if (len < 0 || m->pos + static_cast<size_t>(len) > m->input_length) {
        m->fail("bytes out of range");
        return;
    }
    size_t start = m->pos;
    m->pos += static_cast<size_t>(len);
    CaptureType ct = this->is_string ? CaptureType::String : CaptureType::Bytes;
    m->builder.add_field(this->field_name, start, m->pos, ct);

    auto* new_env = m->arena->alloc<Environment>();
    new_env->binding.name = this->field_name;
    new_env->binding.value = nullptr;  // bytes/string don't bind a typed scalar
    new_env->binding.start_offset = start;
    new_env->binding.end_offset = m->pos;
    new_env->binding.type = ct;
    new_env->parent = m->env;
    m->env = new_env;

    m->push_kont(this->next);
}

void SwitchDispatchNode::invoke(CEKMachine* m) const {
    auto* select = m->arena->alloc<SwitchSelectKont>();
    select->cases = this->cases;
    select->cases_count = this->cases_count;
    select->default_target = this->default_target;
    m->push_kont(select);
    m->push_kont(this->discriminator_expr);
}

void SwitchSelectKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v) { m->fail("switch: null discriminator"); return; }
    for (int i = 0; i < this->cases_count; ++i) {
        const switch_case& c = this->cases[i];
        if (!c.match_value || c.match_value->tag != v->tag) continue;
        bool match = false;
        switch (v->tag) {
            case ValueTag::IntValue:
                match = static_cast<IntValue*>(v)->v == static_cast<IntValue*>(c.match_value)->v;
                break;
            case ValueTag::StringValue:
                match = static_cast<StringValue*>(v)->v == static_cast<StringValue*>(c.match_value)->v;
                break;
            default:
                m->fail("switch: discriminator must be int or string");
                return;
        }
        if (match) {
            m->push_kont(c.target);
            return;
        }
    }
    if (this->default_target) {
        m->push_kont(this->default_target);
    } else {
        m->fail("switch: no matching case");
    }
}

// ── Array spine (begin / next / end + count/until checks) ──

void BeginArrayNode::invoke(CEKMachine* m) const {
    if (this->mode == ArrayMode::FixedCount || this->mode == ArrayMode::Count) {
        // count_expr is non-null; evaluate it then site-consumer sets up frame.
        auto* check = m->arena->alloc<BeginArrayWithLimitKont>();
        check->array_name = this->array_name;
        check->mode = this->mode;
        check->end_node = this->end_node;
        check->next = this->next;
        m->push_kont(check);
        m->push_kont(this->count_expr);
        return;
    }
    // EOF / Until: no count needed; allocate LoopFrame with no limit.
    m->builder.begin_array(this->array_name, m->pos);
    auto* lf = m->arena->alloc<LoopFrame>();
    lf->type = FrameType::Loop;
    lf->counter = 0;
    lf->limit = -1;
    lf->end_node = this->end_node;
    lf->outer_env = m->env;
    lf->outer_endian = m->little_endian;
    lf->saved_env = m->env;
    lf->saved_pos = m->pos;
    lf->saved_endian = m->little_endian;
    lf->capture_mark = m->builder.mark();
    lf->prev = m->frame_top;
    m->frame_top = lf;

    if (this->mode == ArrayMode::EOF_ && m->pos >= m->input_length) {
        m->push_kont(this->end_node);
    } else {
        m->push_kont(this->next);
    }
}

void BeginArrayWithLimitKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v || v->tag != ValueTag::IntValue) {
        m->fail("array count must be integer");
        return;
    }
    int64_t limit = static_cast<IntValue*>(v)->v;

    m->builder.begin_array(this->array_name, m->pos);
    auto* lf = m->arena->alloc<LoopFrame>();
    lf->type = FrameType::Loop;
    lf->counter = 0;
    lf->limit = limit;
    lf->end_node = this->end_node;
    lf->outer_env = m->env;
    lf->outer_endian = m->little_endian;
    lf->saved_env = m->env;
    lf->saved_pos = m->pos;
    lf->saved_endian = m->little_endian;
    lf->capture_mark = m->builder.mark();
    lf->prev = m->frame_top;
    m->frame_top = lf;

    m->push_kont(limit <= 0 ? this->end_node : this->next);
}

void ArrayNextNode::invoke(CEKMachine* m) const {
    if (m->frame_top == nullptr || m->frame_top->type != FrameType::Loop) {
        m->fail("ArrayNext without LoopFrame");
        return;
    }
    auto* lf = static_cast<LoopFrame*>(m->frame_top);
    lf->counter++;

    switch (this->mode) {
        case ArrayMode::FixedCount:
            m->push_kont(lf->counter >= lf->limit ? this->end_node : this->body_entry);
            break;
        case ArrayMode::EOF_:
            m->push_kont(m->pos >= m->input_length ? this->end_node : this->body_entry);
            break;
        case ArrayMode::Count: {
            auto* check = m->arena->alloc<ArrayCountCheckKont>();
            check->end_node = this->end_node;
            check->body_entry = this->body_entry;
            m->push_kont(check);
            m->push_kont(this->count_expr);
            break;
        }
        case ArrayMode::Until: {
            auto* check = m->arena->alloc<ArrayUntilCheckKont>();
            check->end_node = this->end_node;
            check->body_entry = this->body_entry;
            m->push_kont(check);
            m->push_kont(this->until_expr);
            break;
        }
    }
}

void ArrayCountCheckKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v || v->tag != ValueTag::IntValue) {
        m->fail("array count must be integer");
        return;
    }
    int64_t limit = static_cast<IntValue*>(v)->v;
    if (m->frame_top == nullptr || m->frame_top->type != FrameType::Loop) {
        m->fail("ArrayCountCheck without LoopFrame");
        return;
    }
    auto* lf = static_cast<LoopFrame*>(m->frame_top);
    m->push_kont(lf->counter >= limit ? this->end_node : this->body_entry);
}

void ArrayUntilCheckKont::invoke(CEKMachine* m) const {
    bool has_value;
    bool b = coerce_to_bool(m->result, has_value);
    if (!has_value) { m->fail("array until expects bool"); return; }
    m->push_kont(b ? this->end_node : this->body_entry);
}

void EndArrayNode::invoke(CEKMachine* m) const {
    if (m->frame_top == nullptr || m->frame_top->type != FrameType::Loop) {
        m->fail("EndArray without LoopFrame");
        return;
    }
    auto* lf = static_cast<LoopFrame*>(m->frame_top);
    m->builder.end_array(m->pos);
    m->frame_top = lf->prev;
    // Per-element env extensions don't escape the array.
    m->env = lf->outer_env;
    m->push_kont(this->next);
}

// ── Bitfield ──────────────────────────────────────────────

void BitfieldReadNode::invoke(CEKMachine* m) const {
    size_t nbytes = container.byte_count();
    if (m->pos + nbytes > m->input_length) {
        m->fail("bitfield: unexpected end of input");
        return;
    }

    int64_t raw = read_primitive(m->input, m->pos, container, m->little_endian);
    size_t start = m->pos;
    m->pos += nbytes;
    size_t end = m->pos;

    bool msb_first = (container.endian == PrimEndian::Big) ||
                     (container.endian == PrimEndian::Native && !m->little_endian);
    int total_bits = static_cast<int>(nbytes * 8);
    int offset = msb_first ? total_bits : 0;

    for (int i = 0; i < entries_count; ++i) {
        const bitfield_entry& e = entries[i];
        int64_t mask = (e.width_bits >= 64) ? ~int64_t(0)
                                            : ((int64_t(1) << e.width_bits) - 1);
        int64_t val;
        if (msb_first) {
            offset -= e.width_bits;
            val = (raw >> offset) & mask;
        } else {
            val = (raw >> offset) & mask;
            offset += e.width_bits;
        }

        auto* iv = m->arena->alloc<IntValue>();
        iv->v = val;
        m->builder.add_field(e.name, start, end, CaptureType::Computed, iv);

        auto* new_env = m->arena->alloc<Environment>();
        new_env->binding.name = e.name;
        new_env->binding.value = iv;
        new_env->binding.start_offset = start;
        new_env->binding.end_offset = end;
        new_env->binding.type = CaptureType::Computed;
        new_env->parent = m->env;
        m->env = new_env;
    }

    m->push_kont(this->next);
}

// ── External parser dispatch ──────────────────────────────

void ExternalCallNode::invoke(CEKMachine* m) const {
    if (m->ext_parsers == nullptr) {
        m->fail("external parsers not configured");
        return;
    }
    const auto* entry = m->ext_parsers->lookup(this->func_name);
    if (entry == nullptr) {
        m->fail("external parser not registered");
        return;
    }
    size_t remaining = m->input_length - m->pos;
    size_t consumed = 0;
    bool ok = entry->fn(m->input + m->pos, remaining, &consumed,
                        m->arena, entry->user_data);
    if (!ok) {
        m->fail("external parser failed");
        return;
    }
    size_t start = m->pos;
    m->pos += consumed;

    m->builder.add_field(this->field_name, start, m->pos, CaptureType::External);

    auto* new_env = m->arena->alloc<Environment>();
    new_env->binding.name = this->field_name;
    new_env->binding.value = nullptr;
    new_env->binding.start_offset = start;
    new_env->binding.end_offset = m->pos;
    new_env->binding.type = CaptureType::External;
    new_env->parent = m->env;
    m->env = new_env;

    m->push_kont(this->next);
}

// ── Rule call frames ──────────────────────────────────────

void InvokeRuleNode::invoke(CEKMachine* m) const {
    auto* frame = m->arena->alloc<ReturnFrame>();
    frame->type = FrameType::Return;
    frame->return_to = this->next;
    frame->saved_env = m->env;
    frame->start_pos = m->pos;
    frame->capture_mark = m->builder.mark();
    frame->prev = m->frame_top;
    m->frame_top = frame;

    m->push_kont(this->entry);
}

void ReturnRuleNode::invoke(CEKMachine* m) const {
    // Two cases:
    //  - inner rule (called via InvokeRule): a ReturnFrame is on the
    //    stack — pop it, restore caller env, jump to caller's saved
    //    return_to.
    //  - top-level entry: no ReturnFrame. The driver wraps every
    //    rule's exit as ReturnRule → Halt, and execute_from pushes
    //    just the entry kont — so reaching ReturnRule with no frame
    //    means "rule body finished at top level, jump to next which
    //    is Halt." Push next; trampoline drains and exits cleanly.
    if (m->frame_top != nullptr && m->frame_top->type == FrameType::Return) {
        auto* frame = static_cast<ReturnFrame*>(m->frame_top);
        m->frame_top = frame->prev;
        m->env = frame->saved_env;
        m->push_kont(frame->return_to);
        return;
    }
    m->push_kont(this->next);
}

// --- Layer 1 stub invokes (still in place for unimplemented variants) ---
//
// Every other kont variant gets a stub body that calls m->fail() until
// the corresponding Layer 2 sub-implementation lands. invoke is private
// with friend class CEKMachine; defining the method body on the variant
// itself is allowed regardless of access modifier.

#define STUB_INVOKE(KontType) \
    void KontType::invoke(CEKMachine* m) const { \
        m->fail("not yet implemented: " #KontType); \
    }

// (All static spine konts implemented above.)

// --- Helper: read a primitive into a typed Value ---
//
// Integers → IntValue; floats → FloatValue (no truncation); bools → BoolValue.
// Caller has already bounds-checked and is responsible for advancing pos.
static Value* read_primitive_as_value(CEKMachine* m, PrimitiveInfo prim) {
    const uint8_t* p = m->input + m->pos;

    if (prim.kind == PrimKind::Bool) {
        auto* bv = m->arena->alloc<BoolValue>();
        bv->v = p[0] != 0;
        return bv;
    }

    bool want_le = (prim.endian == PrimEndian::Native) ? m->little_endian
                                                       : (prim.endian == PrimEndian::Little);

    if (prim.kind == PrimKind::Float) {
        auto* fv = m->arena->alloc<FloatValue>();
        if (prim.width == PrimWidth::W32) {
            uint32_t raw;
            std::memcpy(&raw, p, 4);
            if (want_le != host_little_endian()) raw = bswap32(raw);
            float f;
            std::memcpy(&f, &raw, 4);
            fv->v = static_cast<double>(f);
        } else {
            uint64_t raw;
            std::memcpy(&raw, p, 8);
            if (want_le != host_little_endian()) raw = bswap64(raw);
            double d;
            std::memcpy(&d, &raw, 8);
            fv->v = d;
        }
        return fv;
    }

    // Integer
    auto* iv = m->arena->alloc<IntValue>();
    iv->v = read_primitive(m->input, m->pos, prim, m->little_endian);
    return iv;
}

// --- Basic spine konts ---

// HaltNode is the rule terminator. It does nothing — the trampoline
// pops it, invokes (which is empty), and on the next iteration K is
// empty so the loop exits cleanly with failed=false. Top-level rules
// chain through ReturnRule → Halt; the driver wraps γ=jump(Halt) so
// rules never invent their own program-exit konts.
void HaltNode::invoke(CEKMachine* /*m*/) const {
    // intentionally empty
}

void BeginStructNode::invoke(CEKMachine* m) const {
    m->builder.begin_struct(this->struct_name, m->pos);
    m->push_kont(this->next);
}

void EndStructNode::invoke(CEKMachine* m) const {
    m->builder.end_struct(m->pos);
    m->push_kont(this->next);
}

// --- Backtracking spine konts ---

void BeginChoiceNode::invoke(CEKMachine* m) const {
    // Push an AlternativeFrame holding the untried alternatives. On
    // failure, fail() restores the saved state and tries the next.
    // join=nullptr means "exhaustion = real failure" (vs Optional which
    // sets join to the skip target).
    auto* frame = m->arena->alloc<AlternativeFrame>();
    frame->type = FrameType::Alternative;
    frame->remaining = this->alternatives;
    frame->remaining_count = this->alternatives_count;
    frame->join = nullptr;
    frame->saved_env = m->env;
    frame->saved_pos = m->pos;
    frame->saved_endian = m->little_endian;
    frame->saved_kont_size = m->kont_stack_size();
    frame->capture_mark = m->builder.mark();
    frame->prev = m->frame_top;
    m->frame_top = frame;

    // Try the first alternative (next = first alternative chain head).
    m->push_kont(this->next);
}

void CommitChoiceNode::invoke(CEKMachine* m) const {
    // The chosen alternative succeeded — pop the AlternativeFrame so a
    // later failure can't backtrack into the already-committed choice.
    if (m->frame_top != nullptr && m->frame_top->type == FrameType::Alternative) {
        m->frame_top = m->frame_top->prev;
    }
    m->push_kont(this->next);
}

void BeginOptionalNode::invoke(CEKMachine* m) const {
    // Push an AlternativeFrame with zero alternatives but a non-null
    // join target — failure of the optional body restores state and
    // skips to next (the optional is "absent").
    auto* frame = m->arena->alloc<AlternativeFrame>();
    frame->type = FrameType::Alternative;
    frame->remaining = nullptr;
    frame->remaining_count = 0;
    frame->join = this->next;
    frame->saved_env = m->env;
    frame->saved_pos = m->pos;
    frame->saved_endian = m->little_endian;
    frame->saved_kont_size = m->kont_stack_size();
    frame->capture_mark = m->builder.mark();
    frame->prev = m->frame_top;
    m->frame_top = frame;

    m->push_kont(this->inner);
}

void EndOptionalNode::invoke(CEKMachine* m) const {
    // Optional body succeeded — pop the frame so subsequent failures
    // don't accidentally treat this as backtrackable.
    if (m->frame_top != nullptr && m->frame_top->type == FrameType::Alternative) {
        m->frame_top = m->frame_top->prev;
    }
    m->push_kont(this->next);
}

void MatchPrimitiveNode::invoke(CEKMachine* m) const {
    size_t nbytes = prim.byte_count();

    // Bounds check
    if (m->pos + nbytes > m->input_length) {
        m->fail("unexpected end of input");
        return;
    }

    size_t start = m->pos;
    Value* val = read_primitive_as_value(m, prim);
    m->pos += nbytes;
    size_t end = m->pos;

    // Record capture
    CaptureType ct = primitive_capture_type(prim, m->little_endian);
    m->builder.add_field(field_name, start, end, ct);

    // Extend env (immutable list)
    auto* new_env = m->arena->alloc<Environment>();
    new_env->binding.name = field_name;
    new_env->binding.value = val;
    new_env->binding.start_offset = start;
    new_env->binding.end_offset = end;
    new_env->binding.type = ct;
    new_env->parent = m->env;
    m->env = new_env;

    m->push_kont(this->next);
}

// (All dynamic konts implemented above.)

#undef STUB_INVOKE

} // namespace bbq::cek
