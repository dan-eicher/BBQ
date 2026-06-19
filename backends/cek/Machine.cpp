// Machine.cpp — Layer 1 stubs.
//
// The real CEK trampoline + 94 invoke bodies arrive in Layer 2. During
// Layer 1, every kont's invoke() is a stub that calls m->fail() so the
// project compiles and the type system catches signature drift.
//
// Helpers (bswap, read_primitive, primitive_capture_type) remain in
// place — Layer 2 uses them from the new structural invokes.

#include "Machine.h"
#include "CompiledGrammar.h"
#include "CaptureDecode.h"   // decode_int/decode_float — a path leaf re-decodes its span

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bbq::cek {

// --- Computed-value boundary (machine IR <-> neutral index runtime) ---
// The index runtime stores a neutral ComputedValue (no dependency on the machine
// Value hierarchy). The machine lowers its scalar Value into that when building a
// capture, and lifts it back when an eval reads a computed/bound field.

static ComputedValue* lower_computed(ParseArena& arena, Value* v) {
    auto* cv = arena.alloc<ComputedValue>();
    if (!v) { cv->kind = ComputedValue::Kind::Int; cv->i = 0; return cv; }
    switch (v->tag) {
        case ValueTag::IntValue:    cv->kind = ComputedValue::Kind::Int;    cv->i = static_cast<IntValue*>(v)->v; break;
        case ValueTag::FloatValue:  cv->kind = ComputedValue::Kind::Float;  cv->f = static_cast<FloatValue*>(v)->v; break;
        case ValueTag::BoolValue:   cv->kind = ComputedValue::Kind::Bool;   cv->b = static_cast<BoolValue*>(v)->v; break;
        case ValueTag::StringValue: cv->kind = ComputedValue::Kind::String; cv->s = static_cast<StringValue*>(v)->v; break;
        default:                    cv->kind = ComputedValue::Kind::Int;    cv->i = 0; break;
    }
    return cv;
}

static Value* lift_computed(ParseArena& arena, const ComputedValue* cv) {
    if (!cv) return nullptr;
    switch (cv->kind) {
        case ComputedValue::Kind::Int:    { auto* v = arena.alloc<IntValue>();    v->v = cv->i; return v; }
        case ComputedValue::Kind::Float:  { auto* v = arena.alloc<FloatValue>();  v->v = cv->f; return v; }
        case ComputedValue::Kind::Bool:   { auto* v = arena.alloc<BoolValue>();   v->v = cv->b; return v; }
        case ComputedValue::Kind::String: { auto* v = arena.alloc<StringValue>(); v->v = cv->s; return v; }
    }
    return nullptr;
}

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

    // Walk frame_top for the innermost Savepoint that still has an
    // on_fail_target. Non-Savepoint frames (ReturnFrame, LoopFrame)
    // and exhausted Savepoints are popped — failure unwinds through
    // them. The savepoint's on_fail_target is a static OnFailKont
    // chain link built at compile time; pushing it dispatches to the
    // correct next-arm or skip target.
    while (frame_top != nullptr) {
        if (frame_top->type == FrameType::Savepoint) {
            auto* sp = static_cast<Savepoint*>(frame_top);
            if (sp->on_fail_target) {
                kont_stack.push_back(sp->on_fail_target);
                return;
            }
        }
        frame_top = frame_top->prev;
    }

    // No savepoint with a fail target remains — parse fails.
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
    interval_depth = 0;
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
    iv->v = static_cast<int64_t>(m->effective_end() - m->pos);
    m->result = iv;
}

void EOIKont::invoke(CEKMachine* m) const {
    auto* iv = m->arena->alloc<IntValue>();
    iv->v = static_cast<int64_t>(m->input_length);
    m->result = iv;
}

void BufferKont::invoke(CEKMachine* m) const {
    // `@buffer`: the enclosing construct's consumed byte interval [start, pos), as a
    // FieldCaptureValue a verification function reads over (m->input[start..end)).
    // start = the innermost open capture scope, or 0 for an inlined top-level struct.
    auto* cap = m->arena->alloc<FieldCapture>();
    cap->start_offset = m->builder.current_scope_start();
    cap->end_offset = m->pos;
    cap->type = CaptureType::Bytes;
    auto* fcv = m->arena->alloc<FieldCaptureValue>();
    fcv->capture = cap;
    m->result = fcv;
}

void StartKont::invoke(CEKMachine* m) const {
    // `@start`: entry offset of the enclosing construct (the open capture scope).
    auto* iv = m->arena->alloc<IntValue>();
    iv->v = static_cast<int64_t>(m->builder.current_scope_start());
    m->result = iv;
}

void EndKont::invoke(CEKMachine* m) const {
    // `@end`: the active end bound — the innermost interval, or the input length.
    auto* iv = m->arena->alloc<IntValue>();
    iv->v = static_cast<int64_t>(m->effective_end());
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

// Usual arithmetic conversions: an operand is numeric (int or float), and its value
// widened to double. Mixed int/float promotes to float, like every compiler. Expression
// evaluation is the int64/double model (the defined semantics) — it matches C for in-range
// values; it does NOT replicate C's narrower-width integer wraparound or float32 precision
// (those are the owning struct's typed-member behaviour, not the expression's). Pinned by
// cek_test's ArithConformsToC against constant-folded C++.
static inline bool is_num(const Value* v) {
    return v && (v->tag == ValueTag::IntValue || v->tag == ValueTag::FloatValue);
}
static inline double as_double(const Value* v) {
    return v->tag == ValueTag::FloatValue ? static_cast<const FloatValue*>(v)->v
                                          : static_cast<double>(static_cast<const IntValue*>(v)->v);
}

// Numeric arithmetic apply: Int+Int stays int; any float operand promotes to float.
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
        } else if (is_num(lhs) && is_num(rhs)) {                              \
            double a = as_double(lhs), b = as_double(rhs);                    \
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
    } else if (is_num(lhs) && is_num(rhs)) {
        auto* r = m->arena->alloc<FloatValue>();
        r->v = as_double(lhs) / as_double(rhs);   // mixed int/float promotes to float
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
    if (lhs->tag != rhs->tag) {
        if (is_num(lhs) && is_num(rhs)) { m->result = alloc_bool(m, as_double(lhs) == as_double(rhs)); return; }
        m->result = alloc_bool(m, false); return;
    }
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
    if (lhs->tag != rhs->tag) {
        if (is_num(lhs) && is_num(rhs)) { m->result = alloc_bool(m, as_double(lhs) != as_double(rhs)); return; }
        m->result = alloc_bool(m, true); return;
    }
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

// Ordered comparison: int/float/string. Mixed int/float promotes to float;
// any other cross-type fails.
#define DEFINE_ORDERED_APPLY(ApplyKont, op_str, op_token)                     \
    void ApplyKont::invoke(CEKMachine* m) const {                             \
        Value* lhs = m->result;                                               \
        Value* rhs = this->saved_right;                                       \
        if (!lhs || !rhs) { m->fail("type error: " op_str); return; }         \
        if (lhs->tag != rhs->tag) {                                           \
            if (is_num(lhs) && is_num(rhs)) {                                 \
                m->result = alloc_bool(m, as_double(lhs) op_token as_double(rhs)); return; \
            }                                                                 \
            m->fail("type error: " op_str); return;                          \
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

// ── Built-in expression functions ──────────────────────────
//
// Each built-in is a named static handler; the dispatch table below
// maps (name, arity) to the handler. Each handler validates argument
// types, allocates its result via the per-parse arena, and writes
// m->result. On error it calls m->fail(...) and returns.

namespace {

void builtin_abs(CEKMachine* m, Value** args) {
    Value* v = args[0];
    if (v && v->tag == ValueTag::IntValue) {
        auto* r = m->arena->alloc<IntValue>();
        int64_t x = static_cast<IntValue*>(v)->v;
        r->v = x < 0 ? -x : x;
        m->result = r;
        return;
    }
    if (v && v->tag == ValueTag::FloatValue) {
        auto* r = m->arena->alloc<FloatValue>();
        double x = static_cast<FloatValue*>(v)->v;
        r->v = x < 0 ? -x : x;
        m->result = r;
        return;
    }
    m->fail("abs: type mismatch");
}

void builtin_minmax(CEKMachine* m, Value** args, bool is_max) {
    Value* a = args[0]; Value* b = args[1];
    if (!a || !b || a->tag != b->tag) { m->fail("min/max: type mismatch"); return; }
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
    m->fail("min/max: type mismatch");
}

void builtin_min(CEKMachine* m, Value** args) { builtin_minmax(m, args, false); }
void builtin_max(CEKMachine* m, Value** args) { builtin_minmax(m, args, true);  }

void builtin_clamp(CEKMachine* m, Value** args) {
    Value* x = args[0]; Value* lo = args[1]; Value* hi = args[2];
    if (!x || !lo || !hi || x->tag != lo->tag || x->tag != hi->tag) {
        m->fail("clamp: type mismatch"); return;
    }
    if (x->tag == ValueTag::IntValue) {
        int64_t xv = static_cast<IntValue*>(x)->v;
        int64_t lov = static_cast<IntValue*>(lo)->v;
        int64_t hiv = static_cast<IntValue*>(hi)->v;
        auto* rv = m->arena->alloc<IntValue>();
        rv->v = xv < lov ? lov : (xv > hiv ? hiv : xv);
        m->result = rv;
        return;
    }
    if (x->tag == ValueTag::FloatValue) {
        double xv = static_cast<FloatValue*>(x)->v;
        double lov = static_cast<FloatValue*>(lo)->v;
        double hiv = static_cast<FloatValue*>(hi)->v;
        auto* rv = m->arena->alloc<FloatValue>();
        rv->v = xv < lov ? lov : (xv > hiv ? hiv : xv);
        m->result = rv;
        return;
    }
    m->fail("clamp: type mismatch");
}

// peek() — read the next byte without consuming. Mirrors the C
// backend's bbq_peek built-in. Used as a switch discriminator for
// tag-prefixed unions where the discriminating byte is also the
// first byte of each case body (see e.g. cap.bbq's CpInfo).
void builtin_peek(CEKMachine* m, Value** /*args*/) {
    if (m->pos >= m->effective_end()) { m->fail("peek: end of input"); return; }
    auto* r = m->arena->alloc<IntValue>();
    r->v = static_cast<int64_t>(m->input[m->pos]);
    m->result = r;
}

// Master registration list — literal name + arity + handler. Pre-
// populated into each CompiledGrammar's BuiltinFnTable at compile
// time, with the names interned in that grammar's StringPool so the
// CallApplyKont dispatch can use pointer-equality.
struct BuiltinRegistration {
    const char* name;
    int arity;
    BuiltinFnTable::Handler fn;
};

constexpr BuiltinRegistration BUILTIN_REGISTRATIONS[] = {
    { "abs",   1, builtin_abs   },
    { "min",   2, builtin_min   },
    { "max",   2, builtin_max   },
    { "clamp", 3, builtin_clamp },
    { "peek",  0, builtin_peek  },
};

constexpr size_t BUILTIN_COUNT =
    sizeof(BUILTIN_REGISTRATIONS) / sizeof(BUILTIN_REGISTRATIONS[0]);

} // namespace

void populate_builtins_into(BuiltinFnTable& out, ParseArena& arena,
                            StringPool& pool) {
    auto* entries = static_cast<BuiltinFnTable::Entry*>(
        arena.allocate(BUILTIN_COUNT * sizeof(BuiltinFnTable::Entry),
                       alignof(BuiltinFnTable::Entry)));
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        entries[i].name = pool.intern(BUILTIN_REGISTRATIONS[i].name);
        entries[i].arity = BUILTIN_REGISTRATIONS[i].arity;
        entries[i].fn = BUILTIN_REGISTRATIONS[i].fn;
    }
    out.entries = entries;
    out.count = static_cast<int>(BUILTIN_COUNT);
}

void populate_builtins(CompiledGrammar& g) {
    populate_builtins_into(g.builtins, g.arena, g.strings);
}

void CallApplyKont::invoke(CEKMachine* m) const {
    if (m->builtins) {
        if (auto* e = m->builtins->lookup(this->func_name, this->arg_count)) {
            e->fn(m, this->args_buf);
            return;
        }
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
    if (cap->computed_value != nullptr) {
        m->result = lift_computed(*m->arena, cap->computed_value);
        return;
    }
    // A path to a plain (non-computed) scalar leaf — `ehdr.e_shoff`, `h.n` — re-decodes
    // its recorded span from the input, matching the compiled readers (node_int). The
    // baked CaptureType carries the endian, so this follows mid-struct @endian switches.
    if (is_float_type(cap->type)) {
        double d = 0;
        if (!decode_float(cap, m->input, &d)) { m->fail("path: cannot decode float leaf"); return; }
        auto* fv = m->arena->alloc<FloatValue>(); fv->v = d; m->result = fv;
    } else {
        int64_t bits = 0; bool sgn = false;
        if (!decode_int(cap, m->input, &bits, &sgn)) { m->fail("path: cannot decode scalar leaf"); return; }
        auto* iv = m->arena->alloc<IntValue>(); iv->v = bits; m->result = iv;
    }
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
        // Compile-time-baked failure handler. compile_primitive
        // resolves on_false via find_backtrack_target(rho); the
        // result is the same OnFailKont link the surrounding
        // Savepoint carries as on_fail_target, so static dispatch
        // here reaches the same place fail() would after walking.
        m->push_kont(this->on_false);
    } else {
        // No on_false plumbed (defensive — compile_primitive should
        // always wire one, falling back to AbortRuleKont at root).
        m->fail("constraint failed");
    }
}

void BindComputeNode::invoke(CEKMachine* m) const {
    auto* store = m->arena->alloc<BindComputeStoreKont>();
    store->next = this->next;
    m->push_kont(store);
    m->push_kont(this->compute_expr);
}

void BindComputeStoreKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v) { m->fail("bind-compute: null value"); return; }
    // Producer: wrap the computed value as a Computed FieldCaptureValue at the
    // current position (no bytes consumed); the store konts that follow record it
    // (Computed → value in the tree) and bind it.
    auto* cap = m->arena->alloc<FieldCapture>();
    cap->start_offset = m->pos;
    cap->end_offset = m->pos;
    cap->type = CaptureType::Computed;
    cap->computed_value = lower_computed(*m->arena, v);
    auto* fcv = m->arena->alloc<FieldCaptureValue>();
    fcv->capture = cap;
    m->result = fcv;
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
    apply->is_string = this->is_string;
    apply->next = this->next;
    m->push_kont(apply);
    m->push_kont(this->length_expr);
}

void MatchBytesApplyKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v || v->tag != ValueTag::IntValue) { m->fail("bytes length not integer"); return; }
    int64_t len = static_cast<IntValue*>(v)->v;
    if (len < 0 || m->pos + static_cast<size_t>(len) > m->effective_end()) {
        m->fail("bytes out of range");
        return;
    }
    size_t start = m->pos;
    m->pos += static_cast<size_t>(len);
    // Producer: yield the byte span as a FieldCaptureValue (no scalar value, so
    // computed_value stays null); the store konts that follow record/bind it.
    auto* cap = m->arena->alloc<FieldCapture>();
    cap->start_offset = start;
    cap->end_offset = m->pos;
    cap->type = this->is_string ? CaptureType::String : CaptureType::Bytes;
    auto* fcv = m->arena->alloc<FieldCaptureValue>();
    fcv->capture = cap;
    m->result = fcv;
    m->push_kont(this->next);
}

void SwitchDispatchNode::invoke(CEKMachine* m) const {
    auto* select = m->arena->alloc<SwitchSelectKont>();
    select->cases = this->cases;
    select->cases_count = this->cases_count;
    select->default_target = this->default_target;
    select->default_reject = this->default_reject;
    m->push_kont(select);
    m->push_kont(this->discriminator_expr);
}

void SwitchSelectKont::invoke(CEKMachine* m) const {
    Value* v = m->result;
    if (!v) { m->fail("switch: null discriminator"); return; }
    // The default arm's ordinal follows the last case (matches the read-side model,
    // which appends default_val after the cases).
    int max_ordinal = -1;
    for (int i = 0; i < this->cases_count; ++i)
        if (this->cases[i].ordinal > max_ordinal) max_ordinal = this->cases[i].ordinal;
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
            // Record which case the parser took; the arm's first capture consumes it.
            m->builder.set_pending_variant_tag(c.ordinal);
            m->push_kont(c.target);
            return;
        }
    }
    if (this->default_target) {
        m->builder.set_pending_variant_tag(max_ordinal + 1);
        m->push_kont(this->default_target);
    } else if (this->default_reject) {
        m->fail("switch: discriminant rejected by `default: reject`");
    } else {
        m->fail("switch: no matching case and no default");
    }
}

// ── Array spine (begin / next / end + count/until checks) ──

// Push a Savepoint anchored to the array's resync target. ArrayNext
// refreshes its saved state per iteration; ResyncKont restores from
// it on element-fail. EndArray pops it. Caller passes nullptr in
// non-resync mode and this is a no-op.
static void push_resync_savepoint(CEKMachine* m, KontNode* resync_target) {
    if (!resync_target) return;
    auto* sp = m->arena->alloc<Savepoint>();
    sp->type = FrameType::Savepoint;
    sp->saved_env = m->env;
    sp->saved_pos = m->pos;
    sp->saved_interval_depth = m->interval_depth;
    sp->saved_endian = m->little_endian;
    sp->saved_kont_size = m->kont_stack_size();
    sp->capture_mark = m->builder.mark();
    sp->on_fail_target = resync_target;
    sp->prev = m->frame_top;
    m->frame_top = sp;
}

void BeginArrayNode::invoke(CEKMachine* m) const {
    if (this->mode == ArrayMode::FixedCount || this->mode == ArrayMode::Count) {
        // count_expr is non-null; evaluate it then site-consumer sets up frame.
        auto* check = m->arena->alloc<BeginArrayWithLimitKont>();
        check->array_name = this->array_name;
        check->mode = this->mode;
        check->end_node = this->end_node;
        check->resync_target = this->resync_target;
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

    push_resync_savepoint(m, this->resync_target);

    if (this->mode == ArrayMode::EOF_ && m->pos >= m->effective_end()) {
        m->push_kont(this->end_node);
    } else if (this->mode == ArrayMode::Until) {
        // Pre-test the until predicate before the first element, so an empty array
        // (terminator true at the start) reads zero elements — the dual of the EOF
        // pre-test above and the Count limit<=0 check. The element body loops back
        // through ArrayNext, which re-tests for every subsequent element.
        auto* check = m->arena->alloc<ArrayUntilCheckKont>();
        check->end_node = this->end_node;
        check->body_entry = this->next;
        m->push_kont(check);
        m->push_kont(this->until_expr);
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

    if (limit > 0) {
        push_resync_savepoint(m, this->resync_target);
    }
    m->push_kont(limit <= 0 ? this->end_node : this->next);
}

void ArrayNextNode::invoke(CEKMachine* m) const {
    // In resync mode, the topmost frame is the per-iteration Savepoint;
    // refresh its captured state for the next iteration before falling
    // through to the LoopFrame check.
    if (this->resync) {
        if (m->frame_top == nullptr ||
            m->frame_top->type != FrameType::Savepoint) {
            m->fail("ArrayNext (resync) missing Savepoint");
            return;
        }
        auto* sp = static_cast<Savepoint*>(m->frame_top);
        sp->saved_env = m->env;
        sp->saved_pos = m->pos;
        sp->saved_interval_depth = m->interval_depth;
        sp->saved_endian = m->little_endian;
        sp->saved_kont_size = m->kont_stack_size();
        sp->capture_mark = m->builder.mark();
    }
    Frame* loop_frame = this->resync ? m->frame_top->prev : m->frame_top;
    if (loop_frame == nullptr || loop_frame->type != FrameType::Loop) {
        m->fail("ArrayNext without LoopFrame");
        return;
    }
    auto* lf = static_cast<LoopFrame*>(loop_frame);
    lf->counter++;

    switch (this->mode) {
        case ArrayMode::FixedCount:
            m->push_kont(lf->counter >= lf->limit ? this->end_node : this->body_entry);
            break;
        case ArrayMode::EOF_:
            m->push_kont(m->pos >= m->effective_end() ? this->end_node : this->body_entry);
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
    // In resync mode, the topmost frame is the per-iteration
    // Savepoint sitting on top of the LoopFrame. Pop it first.
    if (this->resync) {
        if (m->frame_top != nullptr &&
            m->frame_top->type == FrameType::Savepoint) {
            m->frame_top = m->frame_top->prev;
        }
        // ResyncKont's EOF path skips the savepoint pop and jumps
        // straight here, so the savepoint may already be gone.
    }
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

// Per-element-failure recovery for `array<T> ... resync`. Reached via
// the per-iteration Savepoint's on_fail_target. Restores from the
// Savepoint, advances pos by one byte, and retries the element body.
// On EOF, terminates the array gracefully via end_node.
void ResyncKont::invoke(CEKMachine* m) const {
    auto* frame = m->frame_top;
    while (frame && frame->type != FrameType::Savepoint) {
        frame = frame->prev;
    }
    if (!frame) { m->fail("ResyncKont: no Savepoint in scope"); return; }
    auto* sp = static_cast<Savepoint*>(frame);
    m->pos = sp->saved_pos;
    m->interval_depth = sp->saved_interval_depth;
    m->env = sp->saved_env;
    m->little_endian = sp->saved_endian;
    m->builder.restore(sp->capture_mark);
    m->truncate_kont_stack(sp->saved_kont_size);

    // Advance one byte past the failed element start.
    if (m->pos >= m->effective_end()) {
        // No bytes left to scan. A counted array (`[n] resync`) declares exactly n
        // elements — exiting with fewer silently violates the declared structure
        // (the IPG correct-by-construction guarantee), so it is a parse failure.
        // An eof/until array has no count contract and exits normally.
        auto* lf = static_cast<LoopFrame*>(sp->prev);
        if (lf && lf->type == FrameType::Loop && lf->limit >= 0 && lf->counter < lf->limit) {
            // Drop the resync savepoint first: otherwise fail() walks back to it,
            // re-pushes this ResyncKont, and loops. With it gone, the failure unwinds
            // past the array to an enclosing backtrack scope (or fails the parse).
            m->frame_top = sp->prev;
            m->fail("resync: fewer elements than the declared count");
            return;
        }
        // No bytes left to scan — pop the Savepoint and exit the
        // array. EndArray then pops the LoopFrame.
        m->frame_top = sp->prev;
        m->push_kont(this->end_node);
        return;
    }
    m->pos += 1;

    // Refresh savepoint state for the new attempt position so a
    // subsequent failure re-enters at the next byte.
    sp->saved_pos = m->pos;
    sp->saved_interval_depth = m->interval_depth;
    sp->saved_env = m->env;
    sp->saved_endian = m->little_endian;
    sp->saved_kont_size = m->kont_stack_size();
    sp->capture_mark = m->builder.mark();

    m->push_kont(this->body_entry);
}

// ── Bitfield ──────────────────────────────────────────────

void ExtractBitsNode::invoke(CEKMachine* m) const {
    size_t nbytes = container.byte_count();
    if (m->pos + nbytes > m->effective_end()) {
        m->fail("bitfield: unexpected end of input");
        return;
    }
    // Read the container in place — pos is NOT advanced; every member reads the
    // same bytes, and AdvancePos moves past it after the last member. Shared
    // interval [start, end) across all members; runtime msb_first for Native.
    int64_t raw = read_primitive(m->input, m->pos, container, m->little_endian);
    size_t start = m->pos;
    size_t end = m->pos + nbytes;

    bool msb_first = (container.endian == PrimEndian::Big) ||
                     (container.endian == PrimEndian::Native && !m->little_endian);
    int total_bits = static_cast<int>(nbytes * 8);
    int shift = msb_first ? (total_bits - this->offset_before - this->width_bits)
                          : this->offset_before;
    int64_t mask = (this->width_bits >= 64) ? ~int64_t(0)
                                            : ((int64_t(1) << this->width_bits) - 1);

    auto* iv = m->arena->alloc<IntValue>();
    iv->v = (raw >> shift) & mask;
    // Producer: yield the extracted bits as a Computed value over the container's
    // (shared) interval; the store konts that follow record/bind it.
    auto* cap = m->arena->alloc<FieldCapture>();
    cap->start_offset = start;
    cap->end_offset = end;
    cap->type = CaptureType::Computed;
    cap->computed_value = lower_computed(*m->arena, iv);
    auto* fcv = m->arena->alloc<FieldCaptureValue>();
    fcv->capture = cap;
    m->result = fcv;
    m->push_kont(this->next);
}

void AdvancePosNode::invoke(CEKMachine* m) const {
    // Move past the bitfield container the members just read in place. They
    // already verified it fits, so no bounds check is needed.
    m->pos += static_cast<size_t>(this->by);
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
    size_t remaining = m->effective_end() - m->pos;
    size_t consumed = 0;
    bool ok = entry->fn(m->input + m->pos, remaining, &consumed,
                        m->arena, entry->user_data);
    if (!ok) {
        m->fail("external parser failed");
        return;
    }
    size_t start = m->pos;
    m->pos += consumed;
    // Producer: yield the external span as a FieldCaptureValue (no scalar value);
    // the store konts that follow record/bind it.
    auto* cap = m->arena->alloc<FieldCapture>();
    cap->start_offset = start;
    cap->end_offset = m->pos;
    cap->type = CaptureType::External;
    auto* fcv = m->arena->alloc<FieldCaptureValue>();
    fcv->capture = cap;
    m->result = fcv;
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
    frame->rule_fail_target = this->caller_fail_target;
    frame->prev = m->frame_top;
    m->frame_top = frame;

    m->push_kont(this->entry);
}

// AbortRuleKont — cross-rule failure handler. Emitted as the on_false
// target of where-checks at a rule's top level when no in-rule scope
// can recover. Walks ReturnFrames until one with a non-null
// rule_fail_target is found, pops it (and everything above it in the
// frame chain), and dispatches to the caller's failure handler.
//
// If no ReturnFrame in the chain carries a rule_fail_target, the
// failure has nowhere to go — hard parse-fail.
void AbortRuleKont::invoke(CEKMachine* m) const {
    auto* frame = m->frame_top;
    while (frame) {
        if (frame->type == FrameType::Return) {
            auto* rf = static_cast<ReturnFrame*>(frame);
            if (rf->rule_fail_target) {
                m->frame_top = rf->prev;
                m->push_kont(rf->rule_fail_target);
                return;
            }
        }
        frame = frame->prev;
    }
    m->fail("rule abort with no caller backtrack scope");
}

// Failure-handler link in a static OnFailKont chain.
// 1. Find the topmost Savepoint and restore machine state.
// 2. Mutate the savepoint's on_fail_target to next_on_fail (so the
//    next failure in this scope walks to the following arm).
// 3. If pop_frame is set or next_on_fail is null, pop the savepoint
//    (no further retries possible in this scope).
// 4. Dispatch to target — either the next arm's entry, or, when
//    target is null (= scope exhausted), re-fail outward.
void OnFailKont::invoke(CEKMachine* m) const {
    auto* frame = m->frame_top;
    while (frame && frame->type != FrameType::Savepoint) {
        frame = frame->prev;
    }
    if (!frame) {
        m->fail("OnFailKont with no Savepoint in scope");
        return;
    }
    auto* sp = static_cast<Savepoint*>(frame);
    m->pos = sp->saved_pos;
    m->interval_depth = sp->saved_interval_depth;
    m->env = sp->saved_env;
    m->little_endian = sp->saved_endian;
    m->builder.restore(sp->capture_mark);
    m->truncate_kont_stack(sp->saved_kont_size);

    if (this->target == nullptr) {
        // Scope exhausted: drop the savepoint and propagate failure
        // outward via fail()'s walk to the next enclosing scope.
        m->frame_top = sp->prev;
        m->fail("alt exhausted");
        return;
    }

    if (this->pop_frame) {
        m->frame_top = sp->prev;
    } else {
        sp->on_fail_target = this->next_on_fail;
    }
    m->push_kont(this->target);
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

// --- LEB128 variable-length decoders ---
//
// LSB-first base-128, 7 value bits/byte, bit 7 = continuation. Strict: bounded to
// ceil(bits/7) bytes, the final byte's unused high bits must be zero (unsigned) or a
// clean sign-extension (signed). Mirrors the C/C++ runtimes' bbq_read_uleb128/sleb128.
// On success sets *out and *consumed; returns false on truncation / overlong / over-wide.
static bool decode_uleb128(const uint8_t* data, size_t pos, size_t len, int bits,
                           uint64_t* out, size_t* consumed) {
    uint64_t result = 0;
    int shift = 0;
    int maxbytes = (bits + 6) / 7;
    for (int i = 0; i < maxbytes; i++) {
        if (pos + (size_t)i >= len) return false;       // truncated
        uint8_t byte = data[pos + i];
        if (i == maxbytes - 1) {
            if (byte & 0x80) return false;              // too long
            int avail = bits - shift;
            if (avail < 7 && (byte >> avail)) return false;  // value too large
        }
        result |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) { *out = result; *consumed = (size_t)i + 1; return true; }
        shift += 7;
    }
    return false;                                       // unterminated
}

static bool decode_sleb128(const uint8_t* data, size_t pos, size_t len, int bits,
                           int64_t* out, size_t* consumed) {
    uint64_t result = 0;
    int shift = 0;
    int maxbytes = (bits + 6) / 7;
    for (int i = 0; i < maxbytes; i++) {
        if (pos + (size_t)i >= len) return false;       // truncated
        uint8_t byte = data[pos + i];
        if (i == maxbytes - 1) {
            if (byte & 0x80) return false;              // too long
            int avail = bits - shift;
            uint8_t low_mask = (uint8_t)(0x7f >> (7 - avail));
            uint8_t hi = (uint8_t)((byte & 0x7f) & ~low_mask);
            uint8_t expect = (byte & (1u << (avail - 1)))
                           ? (uint8_t)(0x7f & ~low_mask) : 0;
            if (hi != expect) return false;             // value out of range
        }
        result |= (uint64_t)(byte & 0x7f) << shift;
        shift += 7;
        if (!(byte & 0x80)) {
            if (shift < 64 && (byte & 0x40)) result |= ~(uint64_t)0 << shift;
            *out = (int64_t)result;
            *consumed = (size_t)i + 1;
            return true;
        }
    }
    return false;                                       // unterminated
}

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

void BeginVariantNode::invoke(CEKMachine* m) const {
    // A union/alternatives variant opens a named capture scope (like BeginStruct)
    // that ALSO records which arm matched — the discriminator decision, so readers
    // read it instead of re-deriving it.
    m->builder.begin_variant(this->variant_name, m->pos, this->variant_tag);
    m->push_kont(this->next);
}

void EndStructNode::invoke(CEKMachine* m) const {
    m->builder.end_struct(m->pos);
    m->push_kont(this->next);
}

// ── Interval region konts ───────────────────────────────────
// Each region kont carries its offset expression and triggers it (push
// [apply, expr]); the apply consumer reads the evaluated offset from ac.
// Same eval-and-apply discipline as MatchBytes.

void SeekKont::invoke(CEKMachine* m) const {
    auto* apply = m->arena->alloc<SeekApplyKont>();
    apply->next = this->next;
    m->push_kont(apply);
    m->push_kont(this->expr);
}

void SeekApplyKont::invoke(CEKMachine* m) const {
    // `@[start, end]` random access: jump the cursor to the evaluated start.
    if (!m->result || m->result->tag != ValueTag::IntValue) {
        m->fail("interval start is not an integer");
        return;
    }
    int64_t target = static_cast<IntValue*>(m->result)->v;
    if (target < 0 || static_cast<size_t>(target) > m->input_length) {
        m->fail("interval start out of range");
        return;
    }
    m->pos = static_cast<size_t>(target);
    m->push_kont(this->next);
}

void PushIntervalNode::invoke(CEKMachine* m) const {
    auto* apply = m->arena->alloc<PushIntervalApplyKont>();
    apply->relative = this->relative;
    apply->next = this->next;
    m->push_kont(apply);
    m->push_kont(this->expr);
}

void PushIntervalApplyKont::invoke(CEKMachine* m) const {
    if (!m->result || m->result->tag != ValueTag::IntValue) {
        m->fail("interval end is not an integer");
        return;
    }
    int64_t v = static_cast<IntValue*>(m->result)->v;
    // The new window is [start, end). start = the cursor (already Seek'd to the
    // interval's left for @[start,end]; the current pos for @[length]); end is the
    // absolute end, or pos+length when relative.
    size_t start = m->pos;
    size_t end = this->relative ? m->pos + static_cast<size_t>(v < 0 ? 0 : v)
                                : static_cast<size_t>(v < 0 ? 0 : v);
    // IPG confinement: a nested interval may only NARROW the parent's window — it
    // must fall entirely within [effective_start, effective_end). Escaping it is a
    // parse failure (the paper's "checks if the interval falls in the range").
    if (start > end || start < m->effective_start() || end > m->effective_end()) {
        m->fail("interval out of range");
        return;
    }
    // Grow the backing store to hold this depth (no fixed cap); depth <= size always, so
    // a slot at index == size is a fresh push, below it is a reused (post-backtrack) slot.
    if ((size_t)m->interval_depth == m->interval_starts.size()) {
        m->interval_starts.push_back(start);
        m->interval_ends.push_back(end);
    } else {
        m->interval_starts[m->interval_depth] = start;
        m->interval_ends[m->interval_depth] = end;
    }
    m->interval_depth++;
    m->push_kont(this->next);
}

void PopIntervalNode::invoke(CEKMachine* m) const {
    if (m->interval_depth > 0) {
        // A checked pop (an @rest window) demands exact consumption: a size
        // prefix means the rest IS that many bytes, not at most. Reads can't
        // overrun effective_end(), so pos <= end always — pos < end is leftover.
        if (this->checked && m->pos != m->interval_ends[m->interval_depth - 1]) {
            m->fail("interval: size mismatch");
            return;
        }
        m->interval_depth--;
    }
    m->push_kont(this->next);
}

// --- Backtracking spine konts ---

void BeginChoiceNode::invoke(CEKMachine* m) const {
    // Push a Savepoint with on_fail_target = head of the OnFailKont
    // chain. Failure inside any arm walks frame_top to this Savepoint,
    // pushes its on_fail_target, which restores state and dispatches
    // to the next arm (mutating on_fail_target to the next link).
    auto* sp = m->arena->alloc<Savepoint>();
    sp->type = FrameType::Savepoint;
    sp->saved_env = m->env;
    sp->saved_pos = m->pos;
    sp->saved_interval_depth = m->interval_depth;
    sp->saved_endian = m->little_endian;
    sp->saved_kont_size = m->kont_stack_size();
    sp->capture_mark = m->builder.mark();
    sp->on_fail_target = this->on_fail_target;
    sp->prev = m->frame_top;
    m->frame_top = sp;

    m->push_kont(this->next);
}

void CommitChoiceNode::invoke(CEKMachine* m) const {
    // The chosen arm succeeded — pop the Savepoint so subsequent
    // failures don't backtrack into the already-committed choice.
    if (m->frame_top != nullptr && m->frame_top->type == FrameType::Savepoint) {
        m->frame_top = m->frame_top->prev;
    }
    m->push_kont(this->next);
}

void BeginOptionalNode::invoke(CEKMachine* m) const {
    auto* sp = m->arena->alloc<Savepoint>();
    sp->type = FrameType::Savepoint;
    sp->saved_env = m->env;
    sp->saved_pos = m->pos;
    sp->saved_interval_depth = m->interval_depth;
    sp->saved_endian = m->little_endian;
    sp->saved_kont_size = m->kont_stack_size();
    sp->capture_mark = m->builder.mark();
    sp->on_fail_target = this->on_fail_target;
    sp->prev = m->frame_top;
    m->frame_top = sp;

    m->push_kont(this->inner);
}

void EndOptionalNode::invoke(CEKMachine* m) const {
    if (m->frame_top != nullptr && m->frame_top->type == FrameType::Savepoint) {
        m->frame_top = m->frame_top->prev;
    }
    m->push_kont(this->next);
}

void MatchPrimitiveNode::invoke(CEKMachine* m) const {
    size_t start = m->pos;
    Value* val;
    CaptureType ct;

    if (prim.encoding != PrimEncoding::Fixed) {
        // Variable-length LEB128: decode now (the byte span is data-dependent), advance
        // pos by the bytes actually consumed, and record the decoded integer as a
        // Computed capture (a fixed-width byte span can't be re-decoded as a varint).
        int bits = (prim.width == PrimWidth::W64) ? 64 : 32;
        size_t consumed = 0;
        auto* iv = m->arena->alloc<IntValue>();
        if (prim.encoding == PrimEncoding::Uleb) {
            uint64_t u;
            if (!decode_uleb128(m->input, m->pos, m->effective_end(), bits, &u, &consumed)) {
                m->fail("invalid uleb128");
                return;
            }
            iv->v = (int64_t)u;
        } else {
            int64_t s;
            if (!decode_sleb128(m->input, m->pos, m->effective_end(), bits, &s, &consumed)) {
                m->fail("invalid sleb128");
                return;
            }
            iv->v = s;
        }
        m->pos += consumed;
        // Producer: yield a placeable FieldCaptureValue to ac; place() records/
        // binds/discards it. No capture or bind here, and no spine push — the
        // place node's PlaceApplyKont is already queued beneath this producer.
        auto* cap = m->arena->alloc<FieldCapture>();
        cap->start_offset = start;
        cap->end_offset = m->pos;
        cap->type = CaptureType::Computed;
        cap->computed_value = lower_computed(*m->arena, iv);
        auto* fcv = m->arena->alloc<FieldCaptureValue>();
        fcv->capture = cap;
        m->result = fcv;
        m->push_kont(this->next);
        return;
    }

    size_t nbytes = prim.byte_count();

    // Bounds check
    if (m->pos + nbytes > m->effective_end()) {
        m->fail("unexpected end of input");
        return;
    }

    val = read_primitive_as_value(m, prim);
    m->pos += nbytes;
    size_t end = m->pos;

    ct = primitive_capture_type(prim, m->little_endian);
    // Producer: yield a placeable FieldCaptureValue to ac. The value is carried
    // for the env binding; place() applies the "value in the tree only for
    // Computed" rule, so a fixed primitive's tree capture stays valueless.
    auto* cap = m->arena->alloc<FieldCapture>();
    cap->start_offset = start;
    cap->end_offset = end;
    cap->type = ct;
    cap->computed_value = lower_computed(*m->arena, val);
    auto* fcv = m->arena->alloc<FieldCaptureValue>();
    fcv->capture = cap;
    m->result = fcv;
    m->push_kont(this->next);
}

// ── Concrete store konts ──
// Each reads the producer's value from ac (a FieldCaptureValue carrying the
// interval + type + value) and does exactly one thing with it.

static FieldCapture* place_value(CEKMachine* m, const char* who) {
    Value* v = m->result;
    if (!v || v->tag != ValueTag::FieldCaptureValue) {
        m->fail(who);
        return nullptr;
    }
    return static_cast<FieldCaptureValue*>(v)->capture;
}

void CaptureKont::invoke(CEKMachine* m) const {
    FieldCapture* cap = place_value(m, "capture: no value");
    if (!cap) return;
    // The output node carries a value only for Computed types; a fixed
    // primitive's span is re-decoded, so its node stays valueless (parity).
    ComputedValue* tree_val = (cap->type == CaptureType::Computed) ? cap->computed_value
                                                                   : nullptr;
    m->builder.add_field(this->name, cap->start_offset, cap->end_offset,
                         cap->type, tree_val);
    m->push_kont(this->next);
}

void StoreEnvKont::invoke(CEKMachine* m) const {
    FieldCapture* cap = place_value(m, "store_env: no value");
    if (!cap) return;
    auto* new_env = m->arena->alloc<Environment>();
    new_env->binding.name = this->name;
    new_env->binding.value = lift_computed(*m->arena, cap->computed_value);  // carried by the producer
    new_env->binding.start_offset = cap->start_offset;
    new_env->binding.end_offset = cap->end_offset;
    new_env->binding.type = cap->type;
    new_env->parent = m->env;
    m->env = new_env;
    m->push_kont(this->next);
}

// (All dynamic konts implemented above.)

#undef STUB_INVOKE

} // namespace bbq::cek
