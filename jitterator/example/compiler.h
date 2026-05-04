#pragma once

#include "CalcAST.h"
#include "CalcIR.h"
#include "calc_stencil_table.h"
#include "../runtime/jitterator.h"
#include <vector>
#include <unordered_map>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>

// Runtime context — must match the layout in calc_stencils.c exactly.
// Paper's accumulator (r0) is `ac`. Frame-relative locals (paper's
// local_i Location class) live in `frame[fp..fp+frame_size)`. Stack
// temps + outgoing args (paper's stack Location class) push to
// `frame[sp]`. Return continuations + saved fp ride on the cpu's
// hardware call stack via the call stencil's regular C call (not
// musttail) — see calc_stencils.c.
struct Ctx {
    int64_t ac;
    int64_t frame[1024];
    int32_t fp;
    int32_t sp;
};

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

// BURG terminals = the asdl-emitted NodeTag values. calc.burg's TERM
// declarations reference `<Type>::kind_value` directly, so the numbers
// stay in sync without a hand-maintained mirror enum.
static int ir_tag(const calc_ir::Node* n) {
    return static_cast<int>(n->tag);
}

// ── BURG node access helpers ────────────────────────────────

// CEK-tree IR (Phase 9R): operands are real sub-tree fields. BURG
// walks them via these accessors during pattern matching. No more
// `per_node_writers` side-channel — every operand reference is a
// concrete IR pointer.
//
// Arity table (BURG_NODE_ARITY): how many sub-tree children does
// the node have? Producers expose their value sub-tree fields;
// consumers expose their value-input sub-tree(s). Node-graph
// successors (spine `next` etc.) are NOT counted here — those go
// through successor()/successor_count() for backpatch_all's RPO.
static int operand_count(const calc_ir::Node* n) {
    using T = calc_ir::NodeTag;
    switch (n->tag) {
    // Binops / cmps: two value sub-trees (left, right)
    case T::Add: case T::Sub: case T::Mul:
    case T::CmpEq: case T::CmpNeq:
    case T::CmpLt: case T::CmpLe: case T::CmpGt: case T::CmpGe:
        return 2;
    // Unary producer: one value sub-tree
    case T::Neg:
        return 1;
    // Consumers that take a value sub-tree
    case T::StoreLocal: case T::ExprEffect:
    case T::Branch: case T::Leave: case T::Halt:
        return 1;
    // Call: variable arity = args.size(). BURG auto-recurses on
    // each arg as stack_reg (chain rule pushes them).
    case T::Call:
        return (int)static_cast<const calc_ir::Call*>(n)->args.size();
    // Leaves
    case T::LoadConst: case T::LoadLocal:
    case T::Goto: case T::Nop:
        return 0;
    }
    return 0;
}

static calc_ir::Node* operand_producer(const calc_ir::Node* n, int i) {
    using T = calc_ir::NodeTag;
    switch (n->tag) {
    case T::Add:       return i == 0 ? static_cast<const calc_ir::Add*      >(n)->left
                                     : static_cast<const calc_ir::Add*      >(n)->right;
    case T::Sub:       return i == 0 ? static_cast<const calc_ir::Sub*      >(n)->left
                                     : static_cast<const calc_ir::Sub*      >(n)->right;
    case T::Mul:       return i == 0 ? static_cast<const calc_ir::Mul*      >(n)->left
                                     : static_cast<const calc_ir::Mul*      >(n)->right;
    case T::CmpEq:     return i == 0 ? static_cast<const calc_ir::CmpEq*    >(n)->left
                                     : static_cast<const calc_ir::CmpEq*    >(n)->right;
    case T::CmpNeq:    return i == 0 ? static_cast<const calc_ir::CmpNeq*   >(n)->left
                                     : static_cast<const calc_ir::CmpNeq*   >(n)->right;
    case T::CmpLt:     return i == 0 ? static_cast<const calc_ir::CmpLt*    >(n)->left
                                     : static_cast<const calc_ir::CmpLt*    >(n)->right;
    case T::CmpLe:     return i == 0 ? static_cast<const calc_ir::CmpLe*    >(n)->left
                                     : static_cast<const calc_ir::CmpLe*    >(n)->right;
    case T::CmpGt:     return i == 0 ? static_cast<const calc_ir::CmpGt*    >(n)->left
                                     : static_cast<const calc_ir::CmpGt*    >(n)->right;
    case T::CmpGe:     return i == 0 ? static_cast<const calc_ir::CmpGe*    >(n)->left
                                     : static_cast<const calc_ir::CmpGe*    >(n)->right;
    case T::Neg:       return static_cast<const calc_ir::Neg*       >(n)->operand;
    case T::StoreLocal:return static_cast<const calc_ir::StoreLocal*>(n)->value;
    case T::ExprEffect:return static_cast<const calc_ir::ExprEffect*>(n)->value;
    case T::Branch:    return static_cast<const calc_ir::Branch*    >(n)->test;
    case T::Leave:     return static_cast<const calc_ir::Leave*     >(n)->value;
    case T::Halt:      return static_cast<const calc_ir::Halt*      >(n)->value;
    case T::Call:      return static_cast<const calc_ir::Call*      >(n)->args[i];
    default: return nullptr;
    }
}

// Successor count/access: follow spine continuation edges (NOT
// value sub-trees — those go through operand_producer). Used by
// the JIT's backpatch_all to chase emitted-stencil offsets.
//
// Spine consumers thread `next`; Branch threads then_ + else_;
// Goto threads its `target`. Call (value-producer) has only the
// `target` graph edge (function entry IR root) — used for RPO
// graph walk into the function body and for backpatch _HOLE_target.
// Producers (Add/Sub/...) are NOT on the spine — sub-tree children
// of their parent consumer.
//
// Halt and Leave are terminators — no spine successor.
static int successor_count(const calc_ir::Node* n) {
    using T = calc_ir::NodeTag;
    switch (n->tag) {
    case T::Halt:       return 0;
    case T::Leave:      return 0;
    case T::Branch:     return 2;
    case T::Call:       return 1;  // target only (no spine `next`; Call is value-producer)
    case T::StoreLocal: return 1;
    case T::ExprEffect: return 1;
    case T::Goto:       return 1;
    case T::Nop:        return 1;
    default:            return 0;  // producers (Add, Sub, etc.) — sub-tree-only
    }
}

static calc_ir::Node* successor(const calc_ir::Node* n, int i) {
    using T = calc_ir::NodeTag;
    switch (n->tag) {
    case T::Branch: {
        auto* b = static_cast<const calc_ir::Branch*>(n);
        return i == 0 ? b->then_ : b->else_;
    }
    case T::Call:       return static_cast<const calc_ir::Call*      >(n)->target;
    case T::Goto:       return static_cast<const calc_ir::Goto*      >(n)->target;
    case T::Nop:        return static_cast<const calc_ir::Nop*       >(n)->next;
    case T::StoreLocal: return static_cast<const calc_ir::StoreLocal*>(n)->next;
    case T::ExprEffect: return static_cast<const calc_ir::ExprEffect*>(n)->next;
    default:            return nullptr;
    }
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

    // Backpatching strategy for CEK-tree IR:
    //
    //   * Linear chain default: each emitted stencil's _HOLE_cont
    //     points at the NEXT emitted stencil in `emitted[]`. BURG's
    //     reduce traversal emits stencils in execution order (post-
    //     order on operand sub-trees, then parent's emit; spine
    //     successors via RPO), so "next emitted" == "next at
    //     runtime" for any stencil whose continuation is sequential.
    //
    //   * Branch / Goto / Call need IR-driven target resolution
    //     because the next stencil at runtime is NOT the next emitted:
    //       - Branch: _HOLE_then_target = first stencil of then_'s
    //         sub-IR; _HOLE_else_target = first stencil of else_'s.
    //         Branch has no _HOLE_cont (it always transfers).
    //       - Goto: _HOLE_cont = first stencil of target's sub-IR.
    //       - Call: _HOLE_target = first stencil of target (function
    //         entry, separately compiled); _HOLE_cont = next emitted
    //         (the post-call kont).
    //
    //   * Halt / Leave terminate (no cont; ret).
    //
    //   * Nop is cost-0 (no stencil emitted); resolve walks past it
    //     via successor().
    void backpatch_all() {
        uint8_t* base = buf.data();

        // node_to_first_offset: for each IR node, the offset of the
        // FIRST stencil emitted whose ir_node was that node. Used
        // to resolve "where does this spine node's lowering start."
        std::unordered_map<calc_ir::Node*, size_t> node_to_first_offset;
        node_to_first_offset.reserve(emitted.size());
        for (auto& e : emitted) {
            node_to_first_offset.emplace(e.node, e.offset);
        }

        // For a spine node N, find the offset of the first stencil
        // emitted within N's BURG-reduce subtree (operand sub-trees,
        // recursively, then N itself). Walks past Nop placeholders
        // via successor() — Nop is cost-0 and emits nothing, so its
        // "first" is whatever it points at.
        std::function<int64_t(calc_ir::Node*)> first_in_tree =
            [&](calc_ir::Node* n) -> int64_t {
                while (n && n->tag == calc_ir::NodeTag::Nop) {
                    n = successor(n, 0);
                }
                if (!n) return -1;
                int oc = operand_count(n);
                for (int i = 0; i < oc; i++) {
                    int64_t r = first_in_tree(operand_producer(n, i));
                    if (r >= 0) return r;
                }
                auto it = node_to_first_offset.find(n);
                if (it != node_to_first_offset.end())
                    return (int64_t)it->second;
                return -1;
            };

        // entry → first stencil overall (linear chain head).
        if (!emitted.empty()) {
            auto& entry_def = stencil_table[STENCIL_ENTRY];
            int cont_idx = find_hole(entry_def, "_HOLE_cont");
            backpatch(buf, entry_offset, entry_def, cont_idx,
                      (uint64_t)(base + emitted[0].offset));
        }

        for (size_t i = 0; i < emitted.size(); i++) {
            auto& e = emitted[i];
            auto& def = stencil_table[e.stencil_id];
            calc_ir::NodeTag tag = e.node->tag;

            // Branch: two IR-driven targets, no _HOLE_cont.
            if (tag == calc_ir::NodeTag::Branch) {
                int then_idx = find_hole(def, "_HOLE_then_target");
                int else_idx = find_hole(def, "_HOLE_else_target");
                if (then_idx >= 0) {
                    int64_t off = first_in_tree(successor(e.node, 0));
                    if (off >= 0) backpatch(buf, e.offset, def, then_idx,
                                             (uint64_t)(base + off));
                }
                if (else_idx >= 0) {
                    int64_t off = first_in_tree(successor(e.node, 1));
                    if (off >= 0) backpatch(buf, e.offset, def, else_idx,
                                             (uint64_t)(base + off));
                }
                continue;
            }

            // Goto: IR-driven target into _HOLE_cont (no other holes).
            if (tag == calc_ir::NodeTag::Goto) {
                int cont_idx = find_hole(def, "_HOLE_cont");
                if (cont_idx >= 0) {
                    int64_t off = first_in_tree(successor(e.node, 0));
                    if (off >= 0) backpatch(buf, e.offset, def, cont_idx,
                                             (uint64_t)(base + off));
                }
                continue;
            }

            // Call: _HOLE_target = function entry (IR-driven via
            // successor 0); _HOLE_cont = post-call kont (next-emitted
            // in the buffer — Call is a value-producer, so its spine
            // continuation is whatever stencil BURG emitted next).
            if (tag == calc_ir::NodeTag::Call) {
                int target_idx = find_hole(def, "_HOLE_target");
                int cont_idx   = find_hole(def, "_HOLE_cont");
                if (target_idx >= 0) {
                    int64_t off = first_in_tree(successor(e.node, 0));
                    if (off >= 0) backpatch(buf, e.offset, def, target_idx,
                                             (uint64_t)(base + off));
                }
                if (cont_idx >= 0 && i + 1 < emitted.size()) {
                    backpatch(buf, e.offset, def, cont_idx,
                              (uint64_t)(base + emitted[i + 1].offset));
                }
                continue;
            }

            // Halt / Leave: terminators, no _HOLE_cont to patch.
            if (tag == calc_ir::NodeTag::Halt ||
                tag == calc_ir::NodeTag::Leave) {
                continue;
            }

            int cont_idx = find_hole(def, "_HOLE_cont");
            if (cont_idx < 0) continue;

            // Spine consumers (successor_count > 0): IR-driven —
            // _HOLE_cont = first stencil of spine successor's tree.
            // Required for non-sequential flow (loop back-edges,
            // sequence rejoins, etc.) where the next emitted stencil
            // in buffer order is NOT the runtime continuation.
            //
            // Value-producers (successor_count == 0): linear chain —
            // _HOLE_cont = next emitted stencil. BURG's tree-walk
            // emits in execution order, so adjacency in `emitted[]`
            // is adjacency at runtime.
            if (successor_count(e.node) > 0) {
                int64_t off = first_in_tree(successor(e.node, 0));
                if (off >= 0) backpatch(buf, e.offset, def, cont_idx,
                                         (uint64_t)(base + off));
            } else if (i + 1 < emitted.size()) {
                backpatch(buf, e.offset, def, cont_idx,
                          (uint64_t)(base + emitted[i + 1].offset));
            }
        }
    }
};

static JitContext* jit_ctx = nullptr;

// ── BURG semantic action helpers ────────────────────────────
//
// CEK-tree IR + paper figure 8 cases via specialized BURG patterns.
// Per binop, three rule shapes:
//   (LoadLocal, reg)  — F8 case 2, left-slot stencil with right in ac
//   (reg, LoadLocal)  — F8 case 3, commutative ops use right-slot
//                       stencil; non-commutative use rev_sub or
//                       swapped-cmp
//   (stack_reg, reg)  — F8 case 4, *_pop stencil reading frame[--sp]+ac
//
// LoadConst and LoadLocal are leaves; LoadAc is a no-op leaf
// (value already in ctx.ac after a Call's cpu RET).

static void jit_emit_load_const(calc_ir::Node* node) {
    auto* lc = static_cast<calc_ir::LoadConst*>(node);
    auto& def = stencil_table[STENCIL_LOAD_CONST];
    uint64_t vals[2];
    vals[JitContext::find_hole(def, "_HOLE_value")] = (uint64_t)lc->value;
    vals[JitContext::find_hole(def, "_HOLE_cont")]  = 0;
    jit_ctx->emit(STENCIL_LOAD_CONST, vals, node);
}

static void jit_emit_load_local(calc_ir::Node* node) {
    auto* n = static_cast<calc_ir::LoadLocal*>(node);
    auto& def = stencil_table[STENCIL_LOAD_LOCAL];
    uint64_t vals[2];
    vals[JitContext::find_hole(def, "_HOLE_local_offset")] = (uint64_t)n->src;
    vals[JitContext::find_hole(def, "_HOLE_cont")]         = 0;
    jit_ctx->emit(STENCIL_LOAD_LOCAL, vals, node);
}

// Helper: emit a 1-hole-slot binop stencil (add/sub/mul/cmp_*) with
// its _HOLE_lhs filled from a slot index. BURG has already lowered
// the right operand into ctx.ac before calling this.
static void jit_emit_lhs_local_stencil(int stencil_id, int64_t slot,
                                        calc_ir::Node* node) {
    auto& def = stencil_table[stencil_id];
    uint64_t vals[2];
    vals[JitContext::find_hole(def, "_HOLE_lhs")]  = (uint64_t)slot;
    vals[JitContext::find_hole(def, "_HOLE_cont")] = 0;
    jit_ctx->emit(stencil_id, vals, node);
}

// F8 case 2 — `reg: Op(LoadLocal, reg)`. Right operand auto-recursed
// to ac; emit Op stencil reading frame[fp+left.src] + ac → ac.
template <class OpT>
static void jit_emit_op_lhs_local(calc_ir::Node* node, int stencil_id) {
    auto* op = static_cast<OpT*>(node);
    auto* ll = static_cast<calc_ir::LoadLocal*>(op->left);
    jit_emit_lhs_local_stencil(stencil_id, ll->src, node);
}
static void jit_emit_add_lhs_local(calc_ir::Node* n)     { jit_emit_op_lhs_local<calc_ir::Add  >(n, STENCIL_ADD); }
static void jit_emit_sub_lhs_local(calc_ir::Node* n)     { jit_emit_op_lhs_local<calc_ir::Sub  >(n, STENCIL_SUB); }
static void jit_emit_mul_lhs_local(calc_ir::Node* n)     { jit_emit_op_lhs_local<calc_ir::Mul  >(n, STENCIL_MUL); }
static void jit_emit_cmp_eq_lhs_local(calc_ir::Node* n)  { jit_emit_op_lhs_local<calc_ir::CmpEq >(n, STENCIL_CMP_EQ); }
static void jit_emit_cmp_neq_lhs_local(calc_ir::Node* n) { jit_emit_op_lhs_local<calc_ir::CmpNeq>(n, STENCIL_CMP_NEQ); }
static void jit_emit_cmp_lt_lhs_local(calc_ir::Node* n)  { jit_emit_op_lhs_local<calc_ir::CmpLt >(n, STENCIL_CMP_LT); }
static void jit_emit_cmp_le_lhs_local(calc_ir::Node* n)  { jit_emit_op_lhs_local<calc_ir::CmpLe >(n, STENCIL_CMP_LE); }
static void jit_emit_cmp_gt_lhs_local(calc_ir::Node* n)  { jit_emit_op_lhs_local<calc_ir::CmpGt >(n, STENCIL_CMP_GT); }
static void jit_emit_cmp_ge_lhs_local(calc_ir::Node* n)  { jit_emit_op_lhs_local<calc_ir::CmpGe >(n, STENCIL_CMP_GE); }

// F8 case 3 commutative — `reg: Op(reg, LoadLocal)`. Left operand
// auto-recursed to ac; emit Op stencil reading frame[fp+right.src]
// + ac → ac (commutativity makes this equivalent to case 2).
template <class OpT>
static void jit_emit_op_rhs_local(calc_ir::Node* node, int stencil_id) {
    auto* op = static_cast<OpT*>(node);
    auto* ll = static_cast<calc_ir::LoadLocal*>(op->right);
    jit_emit_lhs_local_stencil(stencil_id, ll->src, node);
}
static void jit_emit_add_rhs_local(calc_ir::Node* n)    { jit_emit_op_rhs_local<calc_ir::Add  >(n, STENCIL_ADD); }
static void jit_emit_mul_rhs_local(calc_ir::Node* n)    { jit_emit_op_rhs_local<calc_ir::Mul  >(n, STENCIL_MUL); }
static void jit_emit_cmp_eq_rhs_local(calc_ir::Node* n) { jit_emit_op_rhs_local<calc_ir::CmpEq >(n, STENCIL_CMP_EQ); }
static void jit_emit_cmp_neq_rhs_local(calc_ir::Node* n){ jit_emit_op_rhs_local<calc_ir::CmpNeq>(n, STENCIL_CMP_NEQ); }

// F8 case 3 non-commutative cmp swap — `reg: CmpLt(reg, LoadLocal)`
// uses cmp_gt stencil (CmpLt(E, S) ≡ CmpGt(S, E)); Le↔Ge, Gt↔Lt,
// Ge↔Le. Same right-slot extraction.
static void jit_emit_cmp_lt_rhs_local(calc_ir::Node* n) { jit_emit_op_rhs_local<calc_ir::CmpLt>(n, STENCIL_CMP_GT); }
static void jit_emit_cmp_le_rhs_local(calc_ir::Node* n) { jit_emit_op_rhs_local<calc_ir::CmpLe>(n, STENCIL_CMP_GE); }
static void jit_emit_cmp_gt_rhs_local(calc_ir::Node* n) { jit_emit_op_rhs_local<calc_ir::CmpGt>(n, STENCIL_CMP_LT); }
static void jit_emit_cmp_ge_rhs_local(calc_ir::Node* n) { jit_emit_op_rhs_local<calc_ir::CmpGe>(n, STENCIL_CMP_LE); }

// F8 case 3 Sub non-commutative — `reg: Sub(reg, LoadLocal)` uses
// rev_sub stencil: ctx.ac = ctx.ac - frame[fp+rhs] = E - S.
static void jit_emit_rev_sub_rhs_local(calc_ir::Node* node) {
    auto* op = static_cast<calc_ir::Sub*>(node);
    auto* ll = static_cast<calc_ir::LoadLocal*>(op->right);
    auto& def = stencil_table[STENCIL_REV_SUB];
    uint64_t vals[2];
    vals[JitContext::find_hole(def, "_HOLE_rhs")]  = (uint64_t)ll->src;
    vals[JitContext::find_hole(def, "_HOLE_cont")] = 0;
    jit_ctx->emit(STENCIL_REV_SUB, vals, node);
}

// F8 case 4 — `reg: Op(stack_reg, reg)`. Left lifted to eval-stack
// via the chain rule (push_stack stencil); right auto-recursed to
// ac. Emit *_pop stencil: ctx.ac = ctx.frame[--sp] OP ctx.ac.
static void jit_emit_pop_stencil(int stencil_id, calc_ir::Node* node) {
    auto& def = stencil_table[stencil_id];
    uint64_t vals[1];
    vals[JitContext::find_hole(def, "_HOLE_cont")] = 0;
    jit_ctx->emit(stencil_id, vals, node);
}
static void jit_emit_add_pop(calc_ir::Node* n)     { jit_emit_pop_stencil(STENCIL_ADD_POP, n); }
static void jit_emit_sub_pop(calc_ir::Node* n)     { jit_emit_pop_stencil(STENCIL_SUB_POP, n); }
static void jit_emit_mul_pop(calc_ir::Node* n)     { jit_emit_pop_stencil(STENCIL_MUL_POP, n); }
static void jit_emit_cmp_eq_pop(calc_ir::Node* n)  { jit_emit_pop_stencil(STENCIL_CMP_EQ_POP, n); }
static void jit_emit_cmp_neq_pop(calc_ir::Node* n) { jit_emit_pop_stencil(STENCIL_CMP_NEQ_POP, n); }
static void jit_emit_cmp_lt_pop(calc_ir::Node* n)  { jit_emit_pop_stencil(STENCIL_CMP_LT_POP, n); }
static void jit_emit_cmp_le_pop(calc_ir::Node* n)  { jit_emit_pop_stencil(STENCIL_CMP_LE_POP, n); }
static void jit_emit_cmp_gt_pop(calc_ir::Node* n)  { jit_emit_pop_stencil(STENCIL_CMP_GT_POP, n); }
static void jit_emit_cmp_ge_pop(calc_ir::Node* n)  { jit_emit_pop_stencil(STENCIL_CMP_GE_POP, n); }

// Neg — unary; operand auto-recursed to ac, emit neg stencil.
static void jit_emit_neg(calc_ir::Node* node) { jit_emit_pop_stencil(STENCIL_NEG, node); }

// Push-stack — fires from the chain rule `stack_reg: reg = 1` for
// any value-producer that needs to land on the eval stack: binop
// case-4 spills AND call args. Same stencil for both.
static void jit_emit_push_stack(calc_ir::Node* node) { jit_emit_pop_stencil(STENCIL_PUSH_STACK, node); }

// ── Spine consumers ─────────────────────────────────────────

static void jit_emit_store_local(calc_ir::Node* node) {
    auto* n = static_cast<calc_ir::StoreLocal*>(node);
    auto& def = stencil_table[STENCIL_STORE_LOCAL];
    uint64_t vals[2];
    vals[JitContext::find_hole(def, "_HOLE_local_offset")] = (uint64_t)n->dest;
    vals[JitContext::find_hole(def, "_HOLE_cont")]         = 0;
    jit_ctx->emit(STENCIL_STORE_LOCAL, vals, node);
}

// ExprEffect — value sub-tree was lowered to ac for side effect;
// then jbr to next (cont gets patched). Reuses the jbr stencil
// since "evaluate then unconditional jump" is exactly that.
static void jit_emit_expr_effect(calc_ir::Node* node) { jit_emit_pop_stencil(STENCIL_JBR, node); }

static void jit_emit_branch(calc_ir::Node* node) {
    auto& def = stencil_table[STENCIL_BRANCH];
    uint64_t vals[2];
    vals[JitContext::find_hole(def, "_HOLE_then_target")] = 0;
    vals[JitContext::find_hole(def, "_HOLE_else_target")] = 0;
    jit_ctx->emit(STENCIL_BRANCH, vals, node);
}

static void jit_emit_goto(calc_ir::Node* node) { jit_emit_pop_stencil(STENCIL_JBR, node); }

// Halt and Leave terminate; value sub-tree was lowered to ac before.
// Stencils have no holes other than implicit return.
static void jit_emit_halt(calc_ir::Node* node)  { jit_ctx->emit(STENCIL_HALT,  nullptr, node); }
static void jit_emit_leave(calc_ir::Node* node) { jit_ctx->emit(STENCIL_LEAVE, nullptr, node); }

// Paper page 14 CG[call]. Args were lowered by BURG via the
// per-arity Call rules (each arg auto-recursed as stack_reg → chain
// rule emits push_stack stencil). This stencil sets up the new
// frame, cpu CALLs target, and on RET the function's return value
// materializes in ctx.ac. Two patch sites: _HOLE_target (function
// entry, via successor(0)=target) and _HOLE_cont (post-call kont,
// via next-emitted in the buffer since Call is a value-producer).
static void jit_emit_call(calc_ir::Node* node) {
    auto* c = static_cast<calc_ir::Call*>(node);
    auto& def = stencil_table[STENCIL_CALL];
    uint64_t vals[4];
    vals[JitContext::find_hole(def, "_HOLE_n_args")]     = (uint64_t)c->args.size();
    vals[JitContext::find_hole(def, "_HOLE_frame_size")] = (uint64_t)c->frame_size;
    vals[JitContext::find_hole(def, "_HOLE_target")]     = 0;
    vals[JitContext::find_hole(def, "_HOLE_cont")]       = 0;
    jit_ctx->emit(STENCIL_CALL, vals, node);
}

// ── DDCG Compiler ───────────────────────────────────────────
// The hand-written `Compiler` class has been replaced by the
// ddcgc-generated `calc::Compiler` in calc_compile.h. The .ddcg
// source lives at jitterator/example/grammar/calc.ddcg; rule
// bodies mirror the previous compile_expr_inner cases. Aux
// implementations + slot allocator are in the .ddcg's MEMBERS
// block.
