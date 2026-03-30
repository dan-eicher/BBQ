#pragma once

#include "CalcAST.h"
#include "CalcIR.h"
#include "calc_stencil_table.h"
#include "../runtime/jitterator.h"
#include <vector>
#include <unordered_map>
#include <cassert>
#include <cstring>

// ── Stencil emit/backpatch helpers ───────────────────────────

static size_t emit_stencil(jitterator::CodeBuffer &buf,
                           const StencilDef &stencil,
                           const uint64_t *values) {
    size_t code_base = buf.size();
    buf.emit_bytes(stencil.code, stencil.code_size);

    // Allocate constant pool for data holes
    size_t pool_base = buf.size();
    uint8_t zeros[8] = {};
    int16_t pool_slot[64] = {};
    memset(pool_slot, -1, sizeof(pool_slot));
    uint16_t next_slot = 0;

    for (uint32_t i = 0; i < stencil.patch_count; i++) {
        auto &p = stencil.patches[i];
        if (p.type == PATCH_REL_DATA && pool_slot[p.hole_index] < 0) {
            pool_slot[p.hole_index] = next_slot++;
            buf.emit_bytes(zeros, 8);
        }
    }

    for (uint16_t hi = 0; hi < stencil.hole_count; hi++) {
        if (pool_slot[hi] >= 0) {
            size_t entry_off = pool_base + pool_slot[hi] * 8;
            uint64_t val = values ? values[hi] : 0;
            buf.patch_64(entry_off, val);
        }
    }

    for (uint32_t i = 0; i < stencil.patch_count; i++) {
        auto &p = stencil.patches[i];
        size_t patch_addr = code_base + p.offset;
        switch (p.type) {
        case PATCH_REL_BRANCH: {
            uint64_t target = values ? values[p.hole_index] : 0;
            if (target != 0) {
                uintptr_t patch_abs = (uintptr_t)buf.data() + patch_addr;
                int32_t disp = (int32_t)(target - (patch_abs + 4));
                buf.patch_32(patch_addr, disp);
            }
            break;
        }
        case PATCH_REL_DATA: {
            size_t entry_off = pool_base + pool_slot[p.hole_index] * 8;
            int32_t disp = (int32_t)(entry_off - (patch_addr + 4));
            buf.patch_32(patch_addr, disp);
            break;
        }
        case PATCH_ABS64:
            buf.patch_64(patch_addr, values ? values[p.hole_index] : 0);
            break;
        case PATCH_ABS32S:
            buf.patch_32(patch_addr, (int32_t)(values ? values[p.hole_index] : 0));
            break;
        }
    }
    return code_base;
}

static void backpatch(jitterator::CodeBuffer &buf, size_t stencil_base,
                      const StencilDef &stencil, uint16_t hole_index, uint64_t target) {
    for (uint32_t i = 0; i < stencil.patch_count; i++) {
        auto &p = stencil.patches[i];
        if (p.hole_index != hole_index || p.type != PATCH_REL_BRANCH) continue;
        size_t patch_addr = stencil_base + p.offset;
        uintptr_t patch_abs = (uintptr_t)buf.data() + patch_addr;
        int32_t disp = (int32_t)(target - (patch_abs + 4));
        buf.patch_32(patch_addr, disp);
    }
}

// ── IR node tag enum (BURG terminal IDs) ────────────────────

enum CalcIRTag {
    IR_SLOT_REF   = 0,  // not a BURG terminal — passthrough
    IR_LOAD_CONST = 1,
    IR_ADD        = 2,
    IR_SUB        = 3,
    IR_MUL        = 4,
    IR_NEG        = 5,
    IR_HALT       = 6,
};

static int ir_tag(const calc_ir::Node* n) {
    if (dynamic_cast<const calc_ir::SlotRef*>(n))   return IR_SLOT_REF;
    if (dynamic_cast<const calc_ir::LoadConst*>(n)) return IR_LOAD_CONST;
    if (dynamic_cast<const calc_ir::Add*>(n))       return IR_ADD;
    if (dynamic_cast<const calc_ir::Sub*>(n))       return IR_SUB;
    if (dynamic_cast<const calc_ir::Mul*>(n))       return IR_MUL;
    if (dynamic_cast<const calc_ir::Neg*>(n))       return IR_NEG;
    if (dynamic_cast<const calc_ir::Halt*>(n))      return IR_HALT;
    return 0;
}

// ── BURG node access helpers ────────────────────────────────

// Def-use tracking: which node wrote to each slot?
struct SlotTracker {
    calc_ir::Node* defs[64] = {};

    void record(calc_ir::Node* n, int64_t slot) {
        assert(slot >= 0 && slot < 64);
        defs[slot] = n;
    }

    calc_ir::Node* def_of(int64_t slot) const {
        assert(slot >= 0 && slot < 64);
        return defs[slot];
    }
};

static SlotTracker slot_tracker;

// Operand count: how many slot-read operands does this node have?
static int operand_count(const calc_ir::Node* n) {
    int tag = ir_tag(n);
    switch (tag) {
    case IR_ADD: case IR_SUB: case IR_MUL: return 2;
    case IR_NEG: return 1;
    case IR_LOAD_CONST: case IR_HALT: return 0;
    default: return 0;
    }
}

// Operand producer: which node defined the slot that operand i reads?
static calc_ir::Node* operand_producer(const calc_ir::Node* n, int i) {
    switch (ir_tag(n)) {
    case IR_ADD: {
        auto* a = static_cast<const calc_ir::Add*>(n);
        return slot_tracker.def_of(i == 0 ? a->lhs : a->rhs);
    }
    case IR_SUB: {
        auto* s = static_cast<const calc_ir::Sub*>(n);
        return slot_tracker.def_of(i == 0 ? s->lhs : s->rhs);
    }
    case IR_MUL: {
        auto* m = static_cast<const calc_ir::Mul*>(n);
        return slot_tracker.def_of(i == 0 ? m->lhs : m->rhs);
    }
    case IR_NEG: {
        auto* neg = static_cast<const calc_ir::Neg*>(n);
        return slot_tracker.def_of(neg->src);
    }
    default: return nullptr;
    }
}

// Successor count/access: follow continuation edges
static int successor_count(const calc_ir::Node* n) {
    if (dynamic_cast<const calc_ir::Halt*>(n)) return 0;
    return 1; // all other nodes have a single 'next' continuation
}

static calc_ir::Node* successor(const calc_ir::Node* n, int /*i*/) {
    if (auto* sr = dynamic_cast<const calc_ir::SlotRef*>(n))   return sr->next;
    if (auto* lc = dynamic_cast<const calc_ir::LoadConst*>(n)) return lc->next;
    if (auto* a  = dynamic_cast<const calc_ir::Add*>(n))       return a->next;
    if (auto* s  = dynamic_cast<const calc_ir::Sub*>(n))       return s->next;
    if (auto* m  = dynamic_cast<const calc_ir::Mul*>(n))       return m->next;
    if (auto* ne = dynamic_cast<const calc_ir::Neg*>(n))       return ne->next;
    return nullptr;
}

// Get the destination slot for a node (where it writes)
static int64_t node_dest(const calc_ir::Node* n) {
    if (auto* sr = dynamic_cast<const calc_ir::SlotRef*>(n))   return sr->slot;
    if (auto* lc = dynamic_cast<const calc_ir::LoadConst*>(n)) return lc->dest;
    if (auto* a  = dynamic_cast<const calc_ir::Add*>(n))       return a->dest;
    if (auto* s  = dynamic_cast<const calc_ir::Sub*>(n))       return s->dest;
    if (auto* m  = dynamic_cast<const calc_ir::Mul*>(n))       return m->dest;
    if (auto* ne = dynamic_cast<const calc_ir::Neg*>(n))       return ne->dest;
    return -1;
}

// Get constant value for a LoadConst node
static int64_t const_value(const calc_ir::Node* n) {
    auto* lc = dynamic_cast<const calc_ir::LoadConst*>(n);
    assert(lc);
    return lc->value;
}

// Helper for where-guards: get const value of operand i
static int64_t const_val(calc_ir::Node* node, int i) {
    calc_ir::Node* child = operand_producer(node, i);
    return const_value(child);
}

// ── JIT context ─────────────────────────────────────────────

struct JitContext {
    jitterator::CodeBuffer buf;

    struct Emitted {
        size_t offset;
        int stencil_id;
        calc_ir::Node* node;
    };
    std::vector<Emitted> emitted;
    size_t entry_offset = 0;

    static int find_hole(const StencilDef& def, const char* name) {
        for (int i = 0; i < def.hole_count; i++) {
            if (strcmp(def.hole_names[i], name) == 0) return i;
        }
        return -1;
    }

    void emit_entry() {
        uint64_t vals[1] = {0}; // cont = 0, backpatch later
        entry_offset = emit_stencil(buf, stencil_table[STENCIL_ENTRY], vals);
    }

    size_t emit(int stencil_id, uint64_t* vals, calc_ir::Node* node) {
        size_t off = emit_stencil(buf, stencil_table[stencil_id], vals);
        emitted.push_back({off, stencil_id, node});
        return off;
    }

    void backpatch_all() {
        uint8_t* base = buf.data();

        // Chain stencils in emission order: entry → [0] → [1] → ... → halt
        if (!emitted.empty()) {
            auto& entry_def = stencil_table[STENCIL_ENTRY];
            int cont_idx = find_hole(entry_def, "_HOLE_cont");
            backpatch(buf, entry_offset, entry_def, cont_idx,
                      (uint64_t)(base + emitted[0].offset));
        }

        for (size_t i = 0; i < emitted.size(); i++) {
            auto& e = emitted[i];
            auto& def = stencil_table[e.stencil_id];
            int cont_idx = find_hole(def, "_HOLE_cont");
            if (cont_idx < 0) continue; // halt has no cont

            if (i + 1 < emitted.size()) {
                backpatch(buf, e.offset, def, cont_idx,
                          (uint64_t)(base + emitted[i + 1].offset));
            }
        }
    }
};

static JitContext* jit_ctx = nullptr;

// ── BURG semantic action helpers ────────────────────────────

static void jit_emit_load_const(calc_ir::Node* node) {
    auto* lc = static_cast<calc_ir::LoadConst*>(node);
    auto& def = stencil_table[STENCIL_LOAD_CONST];
    uint64_t vals[3];
    vals[JitContext::find_hole(def, "_HOLE_value")] = (uint64_t)lc->value;
    vals[JitContext::find_hole(def, "_HOLE_dst")]   = (uint64_t)lc->dest;
    vals[JitContext::find_hole(def, "_HOLE_cont")]  = 0;
    jit_ctx->emit(STENCIL_LOAD_CONST, vals, node);
}

static void jit_emit_binop(calc_ir::Node* node, int stencil_id) {
    int64_t lhs = -1, rhs = -1, dest = -1;
    if (auto* a = dynamic_cast<calc_ir::Add*>(node)) { lhs = a->lhs; rhs = a->rhs; dest = a->dest; }
    if (auto* s = dynamic_cast<calc_ir::Sub*>(node)) { lhs = s->lhs; rhs = s->rhs; dest = s->dest; }
    if (auto* m = dynamic_cast<calc_ir::Mul*>(node)) { lhs = m->lhs; rhs = m->rhs; dest = m->dest; }

    auto& def = stencil_table[stencil_id];
    uint64_t vals[4];
    vals[JitContext::find_hole(def, "_HOLE_lhs")]  = (uint64_t)lhs;
    vals[JitContext::find_hole(def, "_HOLE_rhs")]  = (uint64_t)rhs;
    vals[JitContext::find_hole(def, "_HOLE_dst")]  = (uint64_t)dest;
    vals[JitContext::find_hole(def, "_HOLE_cont")] = 0;
    jit_ctx->emit(stencil_id, vals, node);
}

static void jit_emit_neg(calc_ir::Node* node) {
    auto* n = static_cast<calc_ir::Neg*>(node);
    auto& def = stencil_table[STENCIL_NEG];
    uint64_t vals[3];
    vals[JitContext::find_hole(def, "_HOLE_src")]  = (uint64_t)n->src;
    vals[JitContext::find_hole(def, "_HOLE_dst")]  = (uint64_t)n->dest;
    vals[JitContext::find_hole(def, "_HOLE_cont")] = 0;
    jit_ctx->emit(STENCIL_NEG, vals, node);
}

static void jit_emit_halt(calc_ir::Node* node) {
    jit_ctx->emit(STENCIL_HALT, nullptr, node);
}

// Constant folding: replace a binop node's stencil with a load_const
static void jit_fold_binop(calc_ir::Node* node, char op) {
    int64_t a = const_val(node, 0);
    int64_t b = const_val(node, 1);
    int64_t result = 0;
    switch (op) {
    case '+': result = a + b; break;
    case '-': result = a - b; break;
    case '*': result = a * b; break;
    }
    // Emit a load_const with the folded value into this node's dest slot
    int64_t dest = node_dest(node);
    auto& def = stencil_table[STENCIL_LOAD_CONST];
    uint64_t vals[3];
    vals[JitContext::find_hole(def, "_HOLE_value")] = (uint64_t)result;
    vals[JitContext::find_hole(def, "_HOLE_dst")]   = (uint64_t)dest;
    vals[JitContext::find_hole(def, "_HOLE_cont")]  = 0;
    jit_ctx->emit(STENCIL_LOAD_CONST, vals, node);
}

// Mul by 0: result is always 0
static void jit_fold_zero(calc_ir::Node* node) {
    int64_t dest = node_dest(node);
    auto& def = stencil_table[STENCIL_LOAD_CONST];
    uint64_t vals[3];
    vals[JitContext::find_hole(def, "_HOLE_value")] = 0;
    vals[JitContext::find_hole(def, "_HOLE_dst")]   = (uint64_t)dest;
    vals[JitContext::find_hole(def, "_HOLE_cont")]  = 0;
    jit_ctx->emit(STENCIL_LOAD_CONST, vals, node);
}

// ── DDCG Compiler (AST → IR continuation graph) ─────────────

class Compiler {
public:
    calc_ir::Node* compile(calc::Expr* expr) {
        auto* halt = new calc_ir::Halt(0); // placeholder result slot
        calc_ir::Node* entry = compile_expr(expr, halt);

        // Fix halt's result_slot to the final expression's dest
        static_cast<calc_ir::Halt*>(halt)->result_slot = result_slot_;

        // Register def-use for all nodes (walk the chain)
        for (auto* n = entry; n; ) {
            int64_t dest = node_dest(n);
            if (dest >= 0) slot_tracker.record(n, dest);
            if (successor_count(n) > 0) n = successor(n, 0);
            else break;
        }

        return entry;
    }

    int64_t result_slot() const { return result_slot_; }

private:
    int64_t next_slot_ = 0;
    int64_t result_slot_ = 0;

    int64_t alloc_slot() { return next_slot_++; }

    // Compile an expression into a chain of IR nodes ending at 'next'.
    // Returns the entry node of the chain. Result goes to the returned slot.
    struct CompileResult {
        calc_ir::Node* entry;
        int64_t dest;
    };

    CompileResult compile_expr_inner(calc::Expr* expr, calc_ir::Node* next) {
        if (auto* lit = dynamic_cast<calc::IntLit*>(expr)) {
            int64_t dest = alloc_slot();
            auto* node = new calc_ir::LoadConst(lit->value, dest, next);
            return {node, dest};
        }

        if (auto* ref = dynamic_cast<calc::SlotRef*>(expr)) {
            // SlotRef: the value is already in the named slot.
            // No IR node needed — just record the slot and pass through.
            // Reserve slots up to that index so the compiler doesn't reuse them.
            while (next_slot_ <= ref->slot) alloc_slot();
            // Create a SlotRef node for BURG to see (for def-use tracing),
            // but it's a passthrough — no stencil will be emitted.
            auto* node = new calc_ir::SlotRef(ref->slot, next);
            return {node, ref->slot};
        }

        if (auto* bin = dynamic_cast<calc::BinOp*>(expr)) {
            // Pre-scan children to reserve any SlotRef slots first
            reserve_slots(bin->left);
            reserve_slots(bin->right);

            int64_t dest = alloc_slot();
            calc_ir::Node* op_node = nullptr;

            switch (bin->op) {
            case calc::Op::Add:
                op_node = new calc_ir::Add(0, 0, dest, next);
                break;
            case calc::Op::Sub:
                op_node = new calc_ir::Sub(0, 0, dest, next);
                break;
            case calc::Op::Mul:
                op_node = new calc_ir::Mul(0, 0, dest, next);
                break;
            }

            // Compile right operand, then left (bottom-up threading)
            auto [right_entry, right_dest] = compile_expr_inner(bin->right, op_node);
            auto [left_entry, left_dest] = compile_expr_inner(bin->left, right_entry);

            // Patch the slot references
            patch_binop_slots(op_node, left_dest, right_dest);

            return {left_entry, dest};
        }

        if (auto* neg = dynamic_cast<calc::NegOp*>(expr)) {
            reserve_slots(neg->operand);
            int64_t dest = alloc_slot();
            auto* op_node = new calc_ir::Neg(0, dest, next);
            auto [inner_entry, inner_dest] = compile_expr_inner(neg->operand, op_node);
            static_cast<calc_ir::Neg*>(op_node)->src = inner_dest;
            return {inner_entry, dest};
        }

        assert(false && "unknown expression type");
        return {nullptr, -1};
    }

    calc_ir::Node* compile_expr(calc::Expr* expr, calc_ir::Node* next) {
        auto [entry, dest] = compile_expr_inner(expr, next);
        result_slot_ = dest;
        return entry;
    }

    // Pre-scan to reserve SlotRef slots before allocating dest slots
    void reserve_slots(calc::Expr* expr) {
        if (auto* ref = dynamic_cast<calc::SlotRef*>(expr)) {
            while (next_slot_ <= ref->slot) alloc_slot();
        } else if (auto* bin = dynamic_cast<calc::BinOp*>(expr)) {
            reserve_slots(bin->left);
            reserve_slots(bin->right);
        } else if (auto* neg = dynamic_cast<calc::NegOp*>(expr)) {
            reserve_slots(neg->operand);
        }
    }

    static void patch_binop_slots(calc_ir::Node* n, int64_t lhs, int64_t rhs) {
        if (auto* a = dynamic_cast<calc_ir::Add*>(n)) { a->lhs = lhs; a->rhs = rhs; }
        if (auto* s = dynamic_cast<calc_ir::Sub*>(n)) { s->lhs = lhs; s->rhs = rhs; }
        if (auto* m = dynamic_cast<calc_ir::Mul*>(n)) { m->lhs = lhs; m->rhs = rhs; }
    }
};
