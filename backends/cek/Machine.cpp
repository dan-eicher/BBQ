#include "Machine.h"
#include "Expr.h"

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
            // Truncate to int64 for environment binding
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
        case PrimWidth::W8: {
            uint8_t raw = p[0];
            if (prim.sign == PrimSign::Signed)
                return static_cast<int64_t>(static_cast<int8_t>(raw));
            return static_cast<int64_t>(raw);
        }
        case PrimWidth::W16: {
            uint16_t raw;
            std::memcpy(&raw, p, 2);
            if (want_le != host_little_endian()) raw = bswap16(raw);
            if (prim.sign == PrimSign::Signed)
                return static_cast<int64_t>(static_cast<int16_t>(raw));
            return static_cast<int64_t>(raw);
        }
        case PrimWidth::W32: {
            uint32_t raw;
            std::memcpy(&raw, p, 4);
            if (want_le != host_little_endian()) raw = bswap32(raw);
            if (prim.sign == PrimSign::Signed)
                return static_cast<int64_t>(static_cast<int32_t>(raw));
            return static_cast<int64_t>(raw);
        }
        case PrimWidth::W64: {
            uint64_t raw;
            std::memcpy(&raw, p, 8);
            if (want_le != host_little_endian()) raw = bswap64(raw);
            if (prim.sign == PrimSign::Signed)
                return static_cast<int64_t>(raw);
            return static_cast<int64_t>(raw);
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

    // Integer
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

// --- MatchPrimitiveNode::invoke ---

void MatchPrimitiveNode::invoke(CEKMachine* m) {
    size_t nbytes = prim.byte_count();

    // Bounds check
    if (m->pos + nbytes > m->input_length) {
        m->fail("unexpected end of input");
        return;
    }

    size_t start = m->pos;
    int64_t value = read_primitive(m->input, m->pos, prim, m->little_endian);
    m->pos += nbytes;
    size_t end = m->pos;

    // Record capture
    CaptureType ct = primitive_capture_type(prim, m->little_endian);
    m->builder.add_field(field_name, start, end, ct, value);

    // Bind in environment
    if (field_name && m->arena) {
        auto* new_env = m->arena->alloc<Environment>();
        new_env->binding = {field_name, value, start, end, ct};
        new_env->parent = m->env;
        m->env = new_env;
    }

    // Advance to next continuation
    m->control = next;
}

// --- BeginStructNode::invoke ---

void BeginStructNode::invoke(CEKMachine* m) {
    m->builder.begin_struct(struct_name, m->pos);
    m->control = next;
}

// --- EndStructNode::invoke ---

void EndStructNode::invoke(CEKMachine* m) {
    m->builder.end_struct(m->pos);
    m->control = next;
}

// --- Built-in expression functions ---

static int64_t builtin_abs(const int64_t* args, int, const CEKMachine*) {
    return std::abs(args[0]);
}
static int64_t builtin_min(const int64_t* args, int, const CEKMachine*) {
    return std::min(args[0], args[1]);
}
static int64_t builtin_max(const int64_t* args, int, const CEKMachine*) {
    return std::max(args[0], args[1]);
}
static int64_t builtin_clamp(const int64_t* args, int, const CEKMachine*) {
    return std::clamp(args[0], args[1], args[2]);
}
static int64_t builtin_peek(const int64_t*, int, const CEKMachine* m) {
    return (m->pos < m->input_length)
        ? static_cast<int64_t>(m->input[m->pos]) : 0;
}
static FunctionTable::Entry builtin_func_entries[] = {
    {"abs", builtin_abs},
    {"min", builtin_min},
    {"max", builtin_max},
    {"clamp", builtin_clamp},
    {"peek", builtin_peek},
};

static FunctionTable builtin_func_table = {
    builtin_func_entries, 5
};

// --- eval (expression evaluator) ---

static int64_t eval_call(const CompiledExpr* expr, CEKMachine* m) {
    const char* fn = expr->call.func_name;
    int argc = expr->call.arg_count;

    // Look up in user table first (allows overriding built-ins)
    ExprFunction found = nullptr;
    if (m->func_table)
        found = m->func_table->lookup(fn);
    if (!found)
        found = builtin_func_table.lookup(fn);

    if (found) {
        int64_t args[8];
        int n = argc > 8 ? 8 : argc;
        for (int i = 0; i < n; i++) {
            args[i] = eval(expr->call.args[i], m);
            if (m->failed) return 0;
        }
        return found(args, n, m);
    }

    m->fail("unknown function in expression");
    return 0;
}

// --- resolve_capture (navigate capture tree for FieldAccess/IndexAccess) ---

static const FieldCapture* resolve_capture(const CompiledExpr* expr, CEKMachine* m) {
    if (expr->op == ExprOp::Ref)
        return m->builder.find_field(expr->ref_name);
    if (expr->op == ExprOp::FieldAccess) {
        auto* base = resolve_capture(expr->field_access.base, m);
        return base ? m->builder.find_child(base, expr->field_access.field) : nullptr;
    }
    if (expr->op == ExprOp::IndexAccess) {
        auto* base = resolve_capture(expr->index_access.base, m);
        if (!base) return nullptr;
        int64_t idx = eval(expr->index_access.index, m);
        return m->failed ? nullptr : m->builder.find_child_at(base, static_cast<int>(idx));
    }
    return nullptr;
}

int64_t eval(const CompiledExpr* expr, CEKMachine* m) {
    switch (expr->op) {
    case ExprOp::IntLit:
        return expr->int_val;
    case ExprOp::BoolLit:
        return expr->bool_val ? 1 : 0;
    case ExprOp::FloatLit:
        return static_cast<int64_t>(expr->float_val);
    case ExprOp::StrLit:
        return 0;
    case ExprOp::Ref: {
        if (!m->env) {
            m->fail("undefined reference in expression");
            return 0;
        }
        const auto* b = m->env->lookup(expr->ref_name);
        if (!b) {
            m->fail("undefined reference in expression");
            return 0;
        }
        return b->value;
    }

    // Binary arithmetic
    case ExprOp::Add:
        return eval(expr->binary.left, m) + eval(expr->binary.right, m);
    case ExprOp::Sub:
        return eval(expr->binary.left, m) - eval(expr->binary.right, m);
    case ExprOp::Mul:
        return eval(expr->binary.left, m) * eval(expr->binary.right, m);
    case ExprOp::Div: {
        int64_t rhs = eval(expr->binary.right, m);
        if (rhs == 0) { m->fail("division by zero"); return 0; }
        return eval(expr->binary.left, m) / rhs;
    }
    case ExprOp::Mod: {
        int64_t rhs = eval(expr->binary.right, m);
        if (rhs == 0) { m->fail("modulo by zero"); return 0; }
        return eval(expr->binary.left, m) % rhs;
    }

    // Bitwise
    case ExprOp::BitAnd:
        return eval(expr->binary.left, m) & eval(expr->binary.right, m);
    case ExprOp::BitOr:
        return eval(expr->binary.left, m) | eval(expr->binary.right, m);
    case ExprOp::BitXor:
        return eval(expr->binary.left, m) ^ eval(expr->binary.right, m);
    case ExprOp::Shl:
        return eval(expr->binary.left, m) << eval(expr->binary.right, m);
    case ExprOp::Shr:
        return eval(expr->binary.left, m) >> eval(expr->binary.right, m);

    // Logical
    case ExprOp::And:
        return (eval(expr->binary.left, m) && eval(expr->binary.right, m)) ? 1 : 0;
    case ExprOp::Or:
        return (eval(expr->binary.left, m) || eval(expr->binary.right, m)) ? 1 : 0;

    // Comparison
    case ExprOp::Eq:
        return (eval(expr->binary.left, m) == eval(expr->binary.right, m)) ? 1 : 0;
    case ExprOp::Ne:
        return (eval(expr->binary.left, m) != eval(expr->binary.right, m)) ? 1 : 0;
    case ExprOp::Lt:
        return (eval(expr->binary.left, m) < eval(expr->binary.right, m)) ? 1 : 0;
    case ExprOp::Le:
        return (eval(expr->binary.left, m) <= eval(expr->binary.right, m)) ? 1 : 0;
    case ExprOp::Gt:
        return (eval(expr->binary.left, m) > eval(expr->binary.right, m)) ? 1 : 0;
    case ExprOp::Ge:
        return (eval(expr->binary.left, m) >= eval(expr->binary.right, m)) ? 1 : 0;

    // Unary
    case ExprOp::Neg:
        return -eval(expr->unary.operand, m);
    case ExprOp::BitNot:
        return ~eval(expr->unary.operand, m);
    case ExprOp::Not:
        return eval(expr->unary.operand, m) ? 0 : 1;

    // Ternary
    case ExprOp::Ternary:
        return eval(expr->ternary.cond, m)
            ? eval(expr->ternary.then_, m)
            : eval(expr->ternary.else_, m);

    // Function call
    case ExprOp::Call:
        return eval_call(expr, m);

    // Special references
    case ExprOp::Pos:
        return static_cast<int64_t>(m->pos);
    case ExprOp::Remaining:
        return static_cast<int64_t>(m->input_length - m->pos);
    case ExprOp::AtEnd:
        return (m->pos >= m->input_length) ? 1 : 0;
    case ExprOp::EOI:
        return static_cast<int64_t>(m->input_length);

    case ExprOp::LoopVar: {
        for (Frame* f = m->frame_top; f; f = f->prev) {
            if (f->type == FrameType::Loop) {
                return static_cast<LoopFrame*>(f)->counter;
            }
        }
        m->fail("loop variable 'i' used outside array context");
        return 0;
    }

    case ExprOp::FieldAccess:
    case ExprOp::IndexAccess: {
        auto* cap = resolve_capture(expr, m);
        if (!cap) { m->fail("field/index not found"); return 0; }
        return cap->computed_value;
    }
    }
    return 0;
}

// --- EvalConstraintNode::invoke ---

void EvalConstraintNode::invoke(CEKMachine* m) {
    int64_t result = eval(constraint, m);
    if (m->failed) return;
    if (!result) {
        if (on_false) {
            m->control = on_false;  // Conditional branch (optional with constraint)
        } else {
            m->fail("constraint violated");
        }
        return;
    }
    m->control = next;
}

// --- BindComputeNode::invoke ---

void BindComputeNode::invoke(CEKMachine* m) {
    int64_t value = eval(expr, m);
    if (m->failed) return;

    // Extend environment
    if (field_name && m->arena) {
        auto* new_env = m->arena->alloc<Environment>();
        new_env->binding = {field_name, value, m->pos, m->pos, CaptureType::Computed};
        new_env->parent = m->env;
        m->env = new_env;
    }

    // Record computed capture
    m->builder.add_computed(field_name, value, m->pos);

    m->control = next;
}

// --- InvokeRuleNode::invoke ---

void InvokeRuleNode::invoke(CEKMachine* m) {
    // Push return frame
    auto* frame = m->arena->alloc<ReturnFrame>();
    frame->return_to = next;         // Resume here after rule completes
    frame->saved_env = m->env;
    frame->start_pos = m->pos;
    frame->capture_mark = m->builder.mark();
    frame->prev = m->frame_top;
    m->frame_top = frame;

    // Begin struct capture for the rule
    m->builder.begin_struct(rule_name, m->pos);

    // Jump to rule entry
    if (!entry) { m->fail("null rule entry"); return; }
    m->control = entry;
}

// --- ReturnRuleNode::invoke ---

void ReturnRuleNode::invoke(CEKMachine* m) {
    // Top-level rule (no InvokeRuleNode): just continue to next (Halt)
    if (!m->frame_top || m->frame_top->type != FrameType::Return) {
        m->control = next;
        return;
    }

    // Pop return frame
    auto* frame = static_cast<ReturnFrame*>(m->frame_top);
    m->frame_top = frame->prev;

    // End struct capture for the rule
    m->builder.end_struct(m->pos);

    // Restore caller's env, then extend with a binding for the rule result
    m->env = frame->saved_env;
    // Bind rule as a single Struct entry in the caller's env
    // (rule's internal fields are NOT visible to caller)
    // Only bind if rule_name is available — look at the corresponding InvokeRuleNode.
    // The rule_name is stored on the struct capture we just ended; we use
    // the builder's last field (which is the struct capture).
    const auto& fields = m->builder.fields();
    if (!fields.empty()) {
        const auto& last = fields.back();
        if (last.name && m->arena) {
            auto* new_env = m->arena->alloc<Environment>();
            new_env->binding = {last.name, 0, frame->start_pos, m->pos,
                                CaptureType::Struct};
            new_env->parent = m->env;
            m->env = new_env;
        }
    }

    // Resume at return point
    m->control = frame->return_to;
}

// --- BeginChoiceNode::invoke ---

void BeginChoiceNode::invoke(CEKMachine* m) {
    // Push alternative frame with remaining options
    auto* frame = m->arena->alloc<AlternativeFrame>();
    frame->remaining = alternatives;
    frame->remaining_count = alt_count;
    frame->join = nullptr;            // Choice: no skip point (all alts fail = real failure)
    frame->saved_env = m->env;
    frame->saved_pos = m->pos;
    frame->saved_endian = m->little_endian;
    frame->capture_mark = m->builder.mark();
    frame->prev = m->frame_top;
    m->frame_top = frame;

    // First alternative is wired as next in the chain
    m->control = next;
}

// --- CommitChoiceNode::invoke ---

void CommitChoiceNode::invoke(CEKMachine* m) {
    // Pop the AlternativeFrame — this alternative succeeded, discard backtrack point
    m->frame_top = m->frame_top->prev;
    m->control = next;
}

// --- BeginOptionalNode::invoke ---

void BeginOptionalNode::invoke(CEKMachine* m) {
    // Push alternative frame with no remaining alternatives.
    // On failure, restore and jump to `next` (skip point).
    auto* frame = m->arena->alloc<AlternativeFrame>();
    frame->remaining = nullptr;
    frame->remaining_count = 0;
    frame->join = next;               // Skip point on failure
    frame->saved_env = m->env;
    frame->saved_pos = m->pos;
    frame->saved_endian = m->little_endian;
    frame->capture_mark = m->builder.mark();
    frame->prev = m->frame_top;
    m->frame_top = frame;

    // Try optional body
    m->control = inner;
}

// --- EndOptionalNode::invoke ---

void EndOptionalNode::invoke(CEKMachine* m) {
    // Optional succeeded — pop the alternative frame
    m->frame_top = m->frame_top->prev;
    m->control = next;
}

// --- BeginArrayNode::invoke ---

void BeginArrayNode::invoke(CEKMachine* m) {
    int64_t lim = -1;
    if (mode == ArrayMode::FixedCount || mode == ArrayMode::Count) {
        lim = eval(count_expr, m);
        if (m->failed) return;
    }

    auto* frame = m->arena->alloc<LoopFrame>();
    frame->counter = 0;
    frame->limit = lim;
    frame->end_node = end_node;
    frame->soft_fail = (mode == ArrayMode::EOF_ || mode == ArrayMode::Until);
    frame->outer_env = m->env;
    frame->outer_endian = m->little_endian;
    frame->saved_env = m->env;
    frame->saved_pos = m->pos;
    frame->saved_endian = m->little_endian;
    frame->prev = m->frame_top;
    m->frame_top = frame;

    m->builder.begin_array(array_name, m->pos);

    // Take capture mark AFTER begin_array so the array scope survives
    // restore on soft-fail (EOF/Until element failure → end-of-array).
    frame->capture_mark = m->builder.mark();

    // Check for immediate termination (empty array)
    bool skip = false;
    if (mode == ArrayMode::FixedCount && lim <= 0) {
        skip = true;
    } else if (mode == ArrayMode::Count && lim <= 0) {
        skip = true;
    } else if (mode == ArrayMode::EOF_ && m->pos >= m->input_length) {
        skip = true;
    }
    // Note: Until condition is not checked before first element (consistent with do-style)

    if (skip) {
        m->control = end_node;
    } else {
        m->control = next;
    }
}

// --- ArrayNextNode::invoke ---

void ArrayNextNode::invoke(CEKMachine* m) {
    auto* frame = static_cast<LoopFrame*>(m->frame_top);
    frame->counter++;

    bool done = false;
    switch (mode) {
    case ArrayMode::FixedCount:
        done = (frame->counter >= frame->limit);
        break;
    case ArrayMode::EOF_:
        done = (m->pos >= m->input_length);
        break;
    case ArrayMode::Count:
        done = (frame->counter >= eval(count_expr, m));
        if (m->failed) return;
        break;
    case ArrayMode::Until:
        done = (eval(until_expr, m) != 0);
        if (m->failed) return;
        break;
    }

    if (done) {
        m->control = end_node;
    } else {
        // PartialCommit — update saved state for backtracking
        frame->saved_pos = m->pos;
        frame->saved_env = m->env;
        frame->saved_endian = m->little_endian;
        frame->capture_mark = m->builder.mark();
        m->control = next;
    }
}

// --- EndArrayNode::invoke ---

void EndArrayNode::invoke(CEKMachine* m) {
    auto* frame = static_cast<LoopFrame*>(m->frame_top);
    m->env = frame->outer_env;
    m->little_endian = frame->outer_endian;
    m->frame_top = frame->prev;
    m->builder.end_array(m->pos);
    m->control = next;
}

// --- SwitchDispatchNode::invoke ---

void SwitchDispatchNode::invoke(CEKMachine* m) {
    int64_t disc = eval(discriminator, m);
    if (m->failed) return;

    for (int i = 0; i < case_count; i++) {
        if (disc == cases[i].int_val) {
            m->control = cases[i].target;
            return;
        }
    }

    if (default_target) {
        m->control = default_target;
    } else {
        m->fail("switch: no matching case and no default");
    }
}

// --- BitfieldReadNode::invoke ---

void BitfieldReadNode::invoke(CEKMachine* m) {
    size_t nbytes = container.byte_count();
    if (m->pos + nbytes > m->input_length) {
        m->fail("bitfield: unexpected end of input");
        return;
    }

    size_t start = m->pos;
    int64_t raw = read_primitive(m->input, m->pos, container, m->little_endian);
    m->pos += nbytes;
    size_t end = m->pos;

    // Determine bit ordering from container endianness
    bool want_le = (container.endian == PrimEndian::Native)
                    ? m->little_endian
                    : (container.endian == PrimEndian::Little);
    bool msb_first = !want_le;

    int container_bits = static_cast<int>(nbytes * 8);
    int bit_offset = msb_first ? container_bits : 0;

    for (int i = 0; i < entry_count; i++) {
        int w = entries[i].width_bits;
        if (msb_first) bit_offset -= w;
        uint64_t mask = (w >= 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
        int64_t val = (static_cast<uint64_t>(raw) >> bit_offset) & mask;
        if (!msb_first) bit_offset += w;

        // Record entry capture (flat, into enclosing scope)
        m->builder.add_field(entries[i].name, start, end,
                             CaptureType::Computed, val);

        // Bind in environment
        if (entries[i].name && m->arena) {
            auto* new_env = m->arena->alloc<Environment>();
            new_env->binding = {entries[i].name, val, start, end,
                                CaptureType::Computed};
            new_env->parent = m->env;
            m->env = new_env;
        }
    }

    m->control = next;
}

// --- SetEndianNode::invoke ---

void SetEndianNode::invoke(CEKMachine* m) {
    int64_t val = eval(expr, m);
    if (m->failed) return;
    m->little_endian = (val != 0);
    m->control = next;
}

// --- ExternalCallNode::invoke ---

void ExternalCallNode::invoke(CEKMachine* m) {
    if (!m->ext_parsers) {
        m->fail("external parser table not configured");
        return;
    }

    auto* entry = m->ext_parsers->lookup(func_name);
    if (!entry) {
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

    // Record capture
    m->builder.add_field(field_name, start, m->pos, CaptureType::External);

    // Bind in environment
    if (field_name && m->arena) {
        auto* new_env = m->arena->alloc<Environment>();
        new_env->binding = {field_name, 0, start, m->pos, CaptureType::External};
        new_env->parent = m->env;
        m->env = new_env;
    }

    m->control = next;
}

// --- MatchBytesNode::invoke ---

void MatchBytesNode::invoke(CEKMachine* m) {
    int64_t len = eval(length_expr, m);
    if (m->failed) return;

    if (len < 0) {
        m->fail("bytes/string: negative length");
        return;
    }

    size_t n = static_cast<size_t>(len);
    if (m->pos + n > m->input_length) {
        m->fail("bytes/string: unexpected end of input");
        return;
    }

    size_t start = m->pos;
    m->pos += n;

    CaptureType ct = is_string ? CaptureType::String : CaptureType::Bytes;
    m->builder.add_field(field_name, start, m->pos, ct, len);

    // Bind length in environment
    if (field_name && m->arena) {
        auto* new_env = m->arena->alloc<Environment>();
        new_env->binding = {field_name, len, start, m->pos, ct};
        new_env->parent = m->env;
        m->env = new_env;
    }

    m->control = next;
}

// --- HaltNode::invoke ---

void HaltNode::invoke(CEKMachine* m) {
    m->control = nullptr;
}

// --- CEKMachine ---

CaptureMetadata CEKMachine::execute_from(KontNode* entry, const uint8_t* data,
                                          size_t length,
                                          bool default_little_endian) {
    if (!arena) {
        CaptureMetadata meta{};
        meta.error_message = "arena not set";
        return meta;
    }

    // Initialize state
    control = entry;
    env = nullptr;
    frame_top = nullptr;
    input = data;
    input_length = length;
    pos = 0;
    little_endian = default_little_endian;
    failed = false;
    best_error_pos = 0;
    best_error_msg = nullptr;
    builder.clear();

    // Trampoline
    while (control) {
        control->invoke(this);
        if (failed) break;
    }

    return builder.finish(*arena, !failed, pos,
                          best_error_msg, best_error_pos);
}

void CEKMachine::fail(const char* message) {
    // Track best error position for diagnostics
    if (pos >= best_error_pos) {
        best_error_pos = pos;
        best_error_msg = message;
    }

    // Search for a backtrack point (AlternativeFrame or soft-fail LoopFrame)
    while (frame_top) {
        if (frame_top->type == FrameType::Loop) {
            auto* loop = static_cast<LoopFrame*>(frame_top);
            if (loop->soft_fail) {
                // Element parse failure in EOF/Until array → end the array gracefully.
                // Restore to the state before the failed element started.
                pos = loop->saved_pos;
                env = loop->saved_env;
                little_endian = loop->saved_endian;
                builder.restore(loop->capture_mark);
                control = loop->end_node;
                return;
            }
        }
        if (frame_top->type == FrameType::Alternative) {
            auto* alt = static_cast<AlternativeFrame*>(frame_top);
            if (alt->remaining_count > 0) {
                // Restore state
                pos = alt->saved_pos;
                env = alt->saved_env;
                little_endian = alt->saved_endian;
                builder.restore(alt->capture_mark);
                // Try next alternative
                control = alt->remaining[0];
                alt->remaining++;
                alt->remaining_count--;
                return;
            }
            // AlternativeFrame with 0 remaining — optional absent
            // Restore state and continue to join point
            if (alt->join) {
                pos = alt->saved_pos;
                env = alt->saved_env;
                little_endian = alt->saved_endian;
                builder.restore(alt->capture_mark);
                frame_top = alt->prev;
                control = alt->join;
                return;
            }
        }
        frame_top = frame_top->prev;
    }

    // No alternatives — parse fails
    failed = true;
    control = nullptr;
}

} // namespace bbq::cek
