// calc_eqsat.h — the rewrite pass, slotted at calc's IR→tile seam.
//
// Build an e-graph from a value tree, saturate it with the axioms in
// calc.burg's REWRITE section, extract the cheapest equivalent tree, and hand
// THAT to the existing burg tiler, which is untouched. The tiler still
// consumes a tree; it never learns an e-graph exists.
//
// What distinguishes a leaf travels in the e-node's `data`, because that is
// part of what the node IS: two LoadLocals differ by slot and two LoadConsts
// by value, and neither has children to tell them apart. Putting it anywhere
// else would let the graph decide local 0 equals local 1. It also means the
// value is still there at extraction — nothing has to reconstruct it from a
// representation that dropped it.
#pragma once

#include "frontend/CalcIR.h"
#include "calc_codegen.h"
extern "C" {
#include "egraph.h"
}
#include <cstdint>
#include <vector>

// ── Guards and auxiliaries the generated rewriter calls ────────────
// Free functions: the emitted matcher is generated C over an opaque graph and
// knows nothing of calc's namespaces. A guard reads a class's node data; an
// auxiliary returns the class of its result, which is the rewriter's only
// currency.

// The value of `c`, when it is a known constant.
inline bool calc_const_of(egraph* g, eg_id c, int64_t* out) {
    for (int i = 0; i < eg_class_nodes(g, c); i++) {
        if (eg_class_node_op(g, c, i) != calc_ir::LoadConst::kind_value) continue;
        *out = eg_class_node_data(g, c, i);
        return true;
    }
    return false;
}

inline bool calc_is_const(egraph* g, eg_id c, int64_t want) {
    int64_t v;
    return calc_const_of(g, c, &v) && v == want;
}

inline bool calc_both_const(egraph* g, eg_id a, eg_id b) {
    int64_t x, y;
    return calc_const_of(g, a, &x) && calc_const_of(g, b, &y);
}

inline eg_id calc_fold_add(egraph* g, eg_id a, eg_id b) {
    int64_t x = 0, y = 0;
    calc_const_of(g, a, &x);
    calc_const_of(g, b, &y);
    return eg_add(g, calc_ir::LoadConst::kind_value, x + y, nullptr, 0);
}

inline eg_id calc_fold_mul(egraph* g, eg_id a, eg_id b) {
    int64_t x = 0, y = 0;
    calc_const_of(g, a, &x);
    calc_const_of(g, b, &y);
    return eg_add(g, calc_ir::LoadConst::kind_value, x * y, nullptr, 0);
}

// ── The e-class analysis calc.burg declares ────────────────────────
//
// egg §4.1's own worked example: "is this class a known constant" as a
// FACT on the class rather than a scan of its nodes. The difference is
// that a fact flows along equalities — proving one form of a value
// constant makes every form of it constant — where the scan above only
// sees a LoadConst somebody already put in the class.
//
// The domain is flat: unknown, or a value. `join` is the meet of two
// facts about ONE value, so two knowns that disagree cannot both hold
// and the result is unknown; it is idempotent, commutative and
// associative, and the domain has height two, so it needs no widening.
struct calc_fact {
    bool    known;
    int64_t value;
};

extern "C" {

inline void calc_an_make(egraph* g, int op, int64_t data,
                         const eg_id* kids, int nkids, void* out, void* user) {
    (void)user;
    calc_fact* d = (calc_fact*)out;
    d->known = false;
    d->value = 0;
    if (op == calc_ir::LoadConst::kind_value) {
        d->known = true;
        d->value = data;
        return;
    }
    // An operator over two known operands is known. The children's facts
    // are read through eg_class_data, which is what makes this an
    // analysis rather than a rule: no pattern had to match.
    if (nkids != 2) return;
    const calc_fact* a = (const calc_fact*)eg_class_data(g, kids[0]);
    const calc_fact* b = (const calc_fact*)eg_class_data(g, kids[1]);
    if (!a || !b || !a->known || !b->known) return;
    if (op == calc_ir::Add::kind_value)      { d->known = true; d->value = a->value + b->value; }
    else if (op == calc_ir::Sub::kind_value) { d->known = true; d->value = a->value - b->value; }
    else if (op == calc_ir::Mul::kind_value) { d->known = true; d->value = a->value * b->value; }
}

inline void calc_an_join(const void* pa, const void* pb, void* out, void* user) {
    (void)user;
    const calc_fact* a = (const calc_fact*)pa;
    const calc_fact* b = (const calc_fact*)pb;
    calc_fact* d = (calc_fact*)out;
    d->known = false;
    d->value = 0;
    if (a->known && b->known && a->value == b->value) { *d = *a; return; }
    if (a->known && !b->known) { *d = *a; return; }
    if (b->known && !a->known) { *d = *b; return; }
}

// Put the constant IN the class once the fact proves one, so extraction
// can choose it. Idempotent for free: interning a term that already
// exists returns its class.
inline void calc_an_modify(egraph* g, eg_id c, const void* pd, void* user) {
    (void)user;
    const calc_fact* d = (const calc_fact*)pd;
    if (!d->known) return;
    eg_merge(g, c, eg_add(g, calc_ir::LoadConst::kind_value, d->value,
                          nullptr, 0));
}

}  // extern "C"

// The generated rewriter, included AFTER the guards and auxiliaries above:
// it names calc's terminal ids and calls those functions, so it compiles
// inside a translation unit that has them. Its own only #include is
// egraph.h — it knows the graph and nothing else.
#include "calc_rewrite.h"

namespace calc {

// ── IR → e-graph → IR ──────────────────────────────────────────────

// Only pure value nodes enter the graph. Control and effects are the region's
// boundary, not its content: a Branch or a StoreLocal is where a region ends,
// and rewriting across one would be rewriting control flow. An impure node
// enters as an opaque leaf keyed by its own identity, so it keeps its place
// and no rule can look inside it.
inline bool eqsat_is_pure(const calc_ir::Node* n) {
    using T = calc_ir::NodeTag;
    switch (n->tag) {
    case T::LoadConst: case T::LoadLocal:
    case T::Add: case T::Sub: case T::Mul: case T::Div: case T::Neg:
    case T::CmpEq: case T::CmpNeq:
    case T::CmpLt: case T::CmpLe: case T::CmpGt: case T::CmpGe:
        return true;
    default:
        return false;   // Call, StoreLocal, Branch, Goto, Nop, …
    }
}

inline eg_id eqsat_build(egraph* g, calc_ir::Node* n) {
    // An opaque node is its own class, told apart by its identity — the same
    // treatment Click gives one (`cp_new_opaque`: "an opaque source value —
    // its own congruence class, no inputs").
    if (!eqsat_is_pure(n))
        return eg_add(g, ir_tag(n), (int64_t)(intptr_t)n, nullptr, 0);

    if (n->tag == calc_ir::NodeTag::LoadConst)
        return eg_add(g, ir_tag(n),
                      static_cast<calc_ir::LoadConst*>(n)->value, nullptr, 0);
    if (n->tag == calc_ir::NodeTag::LoadLocal)
        return eg_add(g, ir_tag(n),
                      static_cast<calc_ir::LoadLocal*>(n)->src, nullptr, 0);

    int nk = operand_count(n);
    std::vector<eg_id> kids;
    kids.reserve((size_t)nk);
    for (int i = 0; i < nk; i++)
        kids.push_back(eqsat_build(g, operand_producer(n, i)));
    return eg_add(g, ir_tag(n), 0, kids.empty() ? nullptr : kids.data(), nk);
}

// Rebuild IR from an extracted term. The runtime hands back (op, data, kids);
// what those mean is this file's business, which is why the e-graph never had
// to know.
inline calc_ir::Node* eqsat_rebuild(const eg_extract_result& r, int i,
                                    std::vector<calc_ir::Node*>& memo) {
    if (memo[(size_t)i]) return memo[(size_t)i];
    using T = calc_ir::NodeTag;
    int op = r.ops[i];
    int64_t d = r.data[i];
    calc_ir::Node* out = nullptr;

    if (op == calc_ir::LoadConst::kind_value) {
        out = new calc_ir::LoadConst(d);
    } else if (op == calc_ir::LoadLocal::kind_value) {
        out = new calc_ir::LoadLocal(d);
    } else if (r.nkids[i] == 0) {
        // An opaque leaf: `data` is the node it came from, returned as-is.
        out = (calc_ir::Node*)(intptr_t)d;
    } else {
        calc_ir::Node* a = eqsat_rebuild(r, r.kids[r.kid_off[i]], memo);
        calc_ir::Node* b = r.nkids[i] > 1
            ? eqsat_rebuild(r, r.kids[r.kid_off[i] + 1], memo) : nullptr;
        switch ((T)op) {
        case T::Add:    out = new calc_ir::Add(a, b);    break;
        case T::Sub:    out = new calc_ir::Sub(a, b);    break;
        case T::Mul:    out = new calc_ir::Mul(a, b);    break;
        case T::Div:    out = new calc_ir::Div(a, b);    break;
        case T::Neg:    out = new calc_ir::Neg(a);       break;
        case T::CmpEq:  out = new calc_ir::CmpEq(a, b);  break;
        case T::CmpNeq: out = new calc_ir::CmpNeq(a, b); break;
        case T::CmpLt:  out = new calc_ir::CmpLt(a, b);  break;
        case T::CmpLe:  out = new calc_ir::CmpLe(a, b);  break;
        case T::CmpGt:  out = new calc_ir::CmpGt(a, b);  break;
        case T::CmpGe:  out = new calc_ir::CmpGe(a, b);  break;
        default:        out = nullptr;                   break;
        }
    }
    memo[(size_t)i] = out;
    return out;
}

// What a node costs to emit, in bytes. Extraction is size-first, so this is
// the emitter's own encoding read back: one opcode byte for an operation,
// and a constant additionally carries its sleb128 immediate — whose width
// depends on the VALUE, which is why the cost is asked per node rather than
// per operator.
inline int eqsat_node_cost(int op, int64_t data, void* /*user*/) {
    if (op != calc_ir::LoadConst::kind_value) return 1;
    int bytes = 1;                       // OP_CONST
    int64_t v = data;
    for (;;) {                           // sleb128, as Emit::sleb writes it
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        bytes++;
        bool last = (v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40));
        if (last) break;
    }
    return bytes;
}

// The pass: one pure value tree in, its cheapest equivalent out.
inline calc_ir::Node* eqsat_optimize(calc_ir::Node* root, eg_caps caps) {
    if (!root || !eqsat_is_pure(root)) return root;

    egraph g;
    eg_init(&g);
    // Before the first eg_add, as egraph.h requires: a fact is computed
    // as each node is added, so a class created earlier would have none.
    calc_set_analysis(&g, nullptr);
    eg_id c = eqsat_build(&g, root);
    calc_rewrite_region(&g, caps);

    eg_extract_result r;
    calc_ir::Node* out = root;
    if (eg_extract(&g, c, eqsat_node_cost, nullptr, &r)) {
        std::vector<calc_ir::Node*> memo((size_t)r.count, nullptr);
        out = eqsat_rebuild(r, r.root, memo);
        eg_extract_free(&r);
    }
    eg_free(&g);
    return out ? out : root;
}

// Walk the spine and rewrite each value operand hanging off it. The spine —
// StoreLocal, Branch, Goto, Nop, the landing pads — is never touched: those
// nodes are the region boundaries, and a Nop in particular is the label the
// assembler resolves branches through, so moving one would break jump
// resolution rather than optimise anything.
inline void eqsat_run(calc_ir::Node* root, eg_caps caps) {
    // Rewrite each pure value subtree hanging off the spine, and touch nothing
    // else. The spine's control nodes are region boundaries, and a Nop in
    // particular is the label the assembler resolves branches through.
    //
    // What makes the axioms able to fire at all is upstream of here: the DDCG
    // implements Dybvig §3.2 Figure 8's four-case binop fanout, so a binop
    // whose operands are simple keeps them INLINE. `2 * 3` arrives as
    // `Mul(LoadConst 2, LoadConst 3)` and folds. Under a lowering that spilled
    // every operand it would arrive as `Mul(LoadLocal, LoadLocal)` and no rule
    // could see a constant — the answer to which is the missing Figure 8 case,
    // never a pass here that walks back to find what a load was bound to.
    std::vector<calc_ir::Node*> stack{root};
    std::vector<calc_ir::Node*> seen;
    while (!stack.empty()) {
        calc_ir::Node* n = stack.back(); stack.pop_back();
        if (!n) continue;
        bool dup = false;
        for (auto* s : seen) if (s == n) { dup = true; break; }
        if (dup) continue;
        seen.push_back(n);

        for (int i = 0; i < operand_count(n); i++) {
            calc_ir::Node* v = operand_producer(n, i);
            if (!v) continue;
            if (eqsat_is_pure(v)) {
                calc_ir::Node* better = eqsat_optimize(v, caps);
                if (better && better != v) set_operand_producer(n, i, better);
            } else {
                stack.push_back(v);
            }
        }
        for (int i = 0; i < successor_count(n); i++)
            stack.push_back(successor(n, i));
    }
}

// The caps calc runs with. Small on purpose: a calculator's regions are
// tiny, and a bound that is never reached would not demonstrate that the
// bound exists.
inline eg_caps eqsat_default_caps() {
    eg_caps c;
    c.rounds = 8;
    c.node_budget = 4096;
    return c;
}

}  // namespace calc
