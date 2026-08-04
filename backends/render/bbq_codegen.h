/*
 * bbq_codegen.h — the codegen surface bbq.burg drives. The render reader is the
 * compiled CEK (see memory render_reader_is_compiled_cek): each spine kont becomes
 * a C stencil that does its work and `[[clang::musttail]] return`s to its successor.
 *
 * burg's two axes, straight off the asdl:
 *   - kont_expr (BURG_NODE_CHILD): operand sub-trees, tiled bottom-up into C
 *     expression strings via the semantic stack `sstk` (leaf pushes, operator
 *     pops its operands and pushes the combined string). This is burg's real job.
 *   - kont_node (BURG_NODE_SUCC): the spine. Each spine rule emits one node record
 *     {id, op, fields, succ:[ids]}; cg_jump records the successor ids → the
 *     stencil's tail-call target(s). No goto/label/RPO/join analysis: the edge IS
 *     the tail call, so emission order is irrelevant.
 *
 * The MatchPrim/Compute/Bytes read collapses its following Capture/StoreEnv pair
 * (the owning struct field is the binding), mirroring RenderEmit's peephole.
 */
#ifndef BBQ_CODEGEN_H
#define BBQ_CODEGEN_H

#include "KontNode.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace bbq::render {

using json = nlohmann::json;
using namespace bbq::cek;

// ── BURG node access over the kont graph ────────────────────────────
static inline int  k_op(const KontNode* n)            { return (int)n->kind(); }
// A null operand means that operand is absent (e.g. BeginArrayNode.count_expr is
// null for eof/until arrays), so the node's real arity is the non-null operand
// count. burg then matches the right rule by arity: BeginArrayNode(expr) for a
// counted array, BeginArrayNode (leaf) for eof/until. Operands are contiguous —
// only a trailing operand is ever null in this IR.
static inline int  k_arity(const KontNode* n) {
    int c = n->child_count();
    while (c > 0 && n->child(c - 1) == nullptr) c--;
    return c;
}
static inline KontNode* k_child(const KontNode* n, int i) { return n->child(i); }
static inline int  k_succ_count(const KontNode* n)    { return n->succ_count(); }
static inline KontNode* k_succ(const KontNode* n, int i)  { return n->succ(i); }

// A neutral primitive descriptor (kind/width/sign/endian/LEB); the per-language
// mapper (read_fn/ctype callbacks in RenderEmit.cpp) turns it into concrete names.
inline json prim_json(const PrimitiveInfo& p, bool default_le) {
    bool be = (p.endian == PrimEndian::Big) ||
              (p.endian == PrimEndian::Native && !default_le);
    const char* kind = p.kind == PrimKind::Bool ? "bool"
                     : p.kind == PrimKind::Float ? "float" : "int";
    int w = (p.width == PrimWidth::W8) ? 8 : (p.width == PrimWidth::W16) ? 16
          : (p.width == PrimWidth::W32) ? 32 : 64;
    const char* enc = p.encoding == PrimEncoding::Uleb ? "uleb"
                    : p.encoding == PrimEncoding::Sleb ? "sleb" : "fixed";
    return {{"kind", kind}, {"w", w}, {"signed", p.sign == PrimSign::Signed},
            {"be", be}, {"native", p.endian == PrimEndian::Native}, {"enc", enc}};
}

// Per-backend output spelling (a field reference, an @-builtin, path-nav, scope
// nesting) lives on the `Emitter` (backends/render/RenderEmit.cpp) as overridable
// methods — the Emitter IS the speller, there is no swappable profile object. burg's
// `Emit` is fully backend-blind: it carries no Emitter scalars and emits ONE neutral
// op-list (boundaries threaded + id'd, bare leaf names); each backend specializes it
// via post_lower + spelling (the owning fold folds boundaries + derives the struct
// path; the view keeps the boundaries as runtime stencils).

// ── Emit — the stencil-record accumulator + expr semantic stack ─────
struct Emit {
    std::vector<json> nodes;                 // one record per spine stencil
    std::vector<json> sstk;                  // neutral expr-tree semantic stack
    bool default_le = true;                  // grammar default endian, for prim_json
    bool need_struct_start = false;          // frame carries _struct_start
    std::unordered_map<const void*, int> ids;
    // A switch registers EVERY arm's target (struct or scalar or default) → (wrapper
    // field, union member) before the variants reduce, naming all arms the one canonical
    // way `<field>.u.<member>` (`member` = `case_<i>`/`default_val`, matching the types
    // model). A struct arm's begin_struct folds it into the path; a scalar arm's
    // collapse_store redirects its leaf to it. burg's RPO keeps each subtree contiguous.
    std::unordered_map<const void*, std::pair<std::string, std::string>> sw_targets;
    // A bitfield is a run of ExtractBits konts (sharing one container) + a trailing
    // AdvancePos; the first ExtractBits stencil reads the container once and does all
    // the shift/masks (the temp can't survive a tail call), absorbing the rest.
    std::unordered_set<const void*> absorbed;
    // Optional present-element konts (BeginStruct / read) whose capture must target
    // `<field>.value` instead of `<field>` — the optional<Rule|bytes|...> retarget.
    std::unordered_set<const void*> opt_value;
    // The array container field (`out->items`) per open array, pushed at BeginArray
    // and popped at EndArray. An uncounted (eof/until) ArrayNext grows this field;
    // it can't recompute the name itself (ArrayNext carries no array name).
    std::vector<std::string> array_fields;

    int nid(const KontNode* n) {
        auto it = ids.find(n);
        if (it != ids.end()) return it->second;
        int v = (int)ids.size(); ids[n] = v; return v;
    }

    // Transparent konts emit no stencil: capture/store are folded into the read,
    // halt is the terminal past a return. The struct/variant scope markers are
    // transparent ONLY when the backend resolves nesting at compile time (owning,
    // where the owning fold bakes the path into target strings); the index backends
    // emit them as begin_struct/end_struct stencils. Control in the stencil chain
    // skips straight through transparents — they do no runtime work.
    bool transparent(KontKind k) const {
        // The grammar-neutral always-transparent tier: capture/store fold into the
        // preceding read, halt is past a return, and BeginVariant is choice-internal
        // (its stencil targets u.<variant> directly, emitting no record). Scope
        // boundaries (begin/end_struct, end_array) are NOT transparent — burg threads
        // them as neutral records; the owning post_lower folds + drops them (remapping
        // successors), the view keeps them as runtime stencils.
        return k == KontKind::CaptureKont || k == KontKind::StoreEnvKont ||
               k == KontKind::HaltNode || k == KontKind::BeginVariantNode;
    }
    // The id of the next stencil-emitting node along succ(0), skipping transparents.
    int next_id(const KontNode* n) {
        while (n && transparent(n->kind())) n = n->succ(0);
        return n ? nid(n) : -1;
    }

    // ── expr semantic stack (neutral trees; the Emitter spells the leaves) ──
    void epush(json e) { sstk.push_back(std::move(e)); }
    json epop() { json e = std::move(sstk.back()); sstk.pop_back(); return e; }
    void ebin(const char* op) {
        json r = epop(), l = epop();
        epush({{"e", "bin"}, {"op", op}, {"l", std::move(l)}, {"r", std::move(r)}});
    }
    void eun(const char* op) { epush({{"e", "un"}, {"op", op}, {"x", epop()}}); }
    // A function call: the n args were tiled (pushed) before this; pop them in reverse.
    void ecall(const CallKont* c, int n) {
        // peek() is a lookahead BUILTIN (the next byte, unconsumed) — not a user
        // function; the runtime has no `peek()` of this shape, so it spells per profile.
        if (n == 0 && std::string(c->func_name) == "peek") {
            epush({{"e", "builtin"}, {"name", "peek"}}); return;
        }
        json args = json::array();
        args.get_ref<json::array_t&>().resize(n);
        for (int i = n - 1; i >= 0; i--) args[i] = epop();
        epush({{"e", "call"}, {"func", std::string(c->func_name)}, {"args", std::move(args)}});
    }

    // A RefKont resolves to a same-scope field or, when Sema resolved it cross-rule,
    // to a scope-stack cast — emitted as a neutral leaf the Emitter spells.
    void eref(const RefKont* r) {
        if (r->resolved_parent && r->resolved_parent[0] && r->resolved_path && r->resolved_path[0])
            epush({{"e", "cross_ref"}, {"parent", cname(r->resolved_parent)},
                   {"depth", r->resolved_depth}, {"path", std::string(r->resolved_path)}});
        else
            epush({{"e", "field_ref"}, {"name", std::string(r->name)}});
    }

    // ── spine records ──
    // Record one stencil node. `succ` are the positional control successors
    // (KontNode::succ order); the template knows each kind's positional meaning.
    void node(const KontNode* n, const char* op, json fields = json::object()) {
        fields["op"] = op;
        fields["id"] = nid(n);
        json s = json::array();
        for (int i = 0; i < n->succ_count(); i++)
            s.push_back(n->succ(i) ? nid(n->succ(i)) : -1);
        fields["succ"] = s;
        nodes.push_back(std::move(fields));
    }

    // The CANONICAL rule name — BBQ's one naming convention (CamelCase), which
    // grammars are held to (not raw author pass-through; that would let inconsistent
    // naming propagate). The lowering is consumer-agnostic: it never snake_cases or
    // prefixes (C conventions). Each backend's template/mapper converts (C →
    // snake+prefix, C++ → keep), so a new backend reads one convention, no guessing.
    std::string cname(const std::string& rule) const { return rule; }
};

// The wrapper field that names a switch's arms — `body` for `body: switch(...)`, empty
// for a top-level switch RULE (the union is the root). Every arm belongs to the one
// switch field, so it is taken from whichever arm exposes it: a struct arm's BeginStruct
// name or a scalar arm's capture name (they are the same field). burg names all arms
// `<field>.u.<member>` from this, consistently.
inline std::string switch_field_name(const SwitchDispatchNode* sw) {
    auto from = [](const KontNode* t) -> const char* {
        if (!t) return nullptr;
        if (t->kind() == KontKind::BeginStructNode)
            return static_cast<const BeginStructNode*>(t)->struct_name;
        const KontNode* c = t->succ(0);
        if (c && c->kind() == KontKind::CaptureKont)  return static_cast<const CaptureKont*>(c)->name;
        if (c && c->kind() == KontKind::StoreEnvKont) return static_cast<const StoreEnvKont*>(c)->name;
        return nullptr;
    };
    for (int i = 0; i < sw->cases_count; i++)
        if (const char* n = from(sw->cases[i].target)) return n;
    if (sw->default_target) if (const char* n = from(sw->default_target)) return n;
    return "";
}

// ── Capture/Store collapse ──────────────────────────────────────────
// A read (MatchPrim/Compute/Bytes) is followed by Capture(name)→StoreEnv (a real
// field) or StoreEnv(name) alone (`_`: verify-only). Returns the target path the
// read writes into, and the spine node to continue at (past the store).
struct StoreInfo { std::string target; const KontNode* after; int after_id; };

inline StoreInfo collapse_store(Emit& e, const KontNode* read_node) {
    const KontNode* c = read_node->succ(0);
    const char* nm = (c->kind() == KontKind::CaptureKont)
        ? static_cast<const CaptureKont*>(c)->name
        : static_cast<const StoreEnvKont*>(c)->name;
    // Array elements have a null (positional) capture name — the prefix already
    // points at the indexed slot, so an empty leaf targets the slot itself.
    std::string name = nm ? nm : "";
    // Emit the BARE leaf name only — burg is backend-neutral. The owning struct layout
    // (an optional present-element's `.value`, a scalar switch arm's `.u.<member>`) is
    // the owning backend's concern: its fold-pass derives those from the neutral
    // structure (the optional/begin_optional/optional_prim present pointers and the
    // switch op's cases/ordinals) and prepends the compile-time path. The view records
    // the bare leaf by name + variant_tag, so it needs no transform.
    // next_id skips the Capture/StoreEnv pair.
    return { name, nullptr, e.next_id(c) };
}

// PrimitiveInfo → the C scalar type for a field/element of that primitive.
inline std::string prim_ctype(const PrimitiveInfo& p) {
    if (p.kind == PrimKind::Bool) return "bool";
    if (p.kind == PrimKind::Float) return (p.width == PrimWidth::W64) ? "double" : "float";
    int w = (p.width == PrimWidth::W8) ? 8 : (p.width == PrimWidth::W16) ? 16
          : (p.width == PrimWidth::W32) ? 32 : 64;
    return (p.sign == PrimSign::Unsigned ? "uint" : "int") + std::to_string(w) + "_t";
}

// The array is a tail-call loop (mirrors the CEK): BeginArray sets up the loop and
// the element body is emitted as ordinary stencils (so any element shape — prim,
// bytes, rule, interval-wrapped, nested — just works), with ArrayNext looping back.
// The element C type isn't spelled at all: the alloc uses `sizeof(*field.items)` and
// calloc's void* converts to the field's own type, which works for every element
// shape (named or anonymous-struct) without the C type-identity mismatch.

// ── Bitfield collapse ───────────────────────────────────────────────
// Walk the ExtractBits run from `first`, reading the shared container once and
// shift/masking each member out (MSB-first for a big-endian container). Marks the
// trailing ExtractBits + AdvancePos absorbed. Returns the members, the container
// prim descriptor, and the next stencil id (past the AdvancePos).
struct BitfieldInfo { json members; json prim; int next; };

inline BitfieldInfo collapse_bitfield(Emit& e, const KontNode* first) {
    const PrimitiveInfo& cont = static_cast<const ExtractBitsNode*>(first)->container;
    int cbits = (cont.width == PrimWidth::W8) ? 8 : (cont.width == PrimWidth::W16) ? 16
              : (cont.width == PrimWidth::W32) ? 32 : 64;
    bool be = (cont.endian == PrimEndian::Big) ||
              (cont.endian == PrimEndian::Native && !e.default_le);
    json members = json::array();
    const KontNode* m = first;
    while (m && m->kind() == KontKind::ExtractBitsNode) {
        auto* eb = static_cast<const ExtractBitsNode*>(m);
        const KontNode* c = m->succ(0);
        std::string name = (c->kind() == KontKind::CaptureKont)
            ? static_cast<const CaptureKont*>(c)->name
            : static_cast<const StoreEnvKont*>(c)->name;
        const KontNode* after = (c->kind() == KontKind::CaptureKont) ? c->succ(0)->succ(0) : c->succ(0);
        int shift = be ? (cbits - eb->offset_before - eb->width_bits) : eb->offset_before;
        int64_t mask = (int64_t)((1ull << eb->width_bits) - 1);
        members.push_back({{"target", name}, {"shift", shift}, {"mask", mask}});  // leaf; fold prepends
        if (m != first) e.absorbed.insert(m);
        // An entry's `where` sits between its extract and the next one. The run is one
        // container read, so the check belongs INSIDE it, against the entry just
        // extracted — stopping here would restart the run and re-read the container.
        // The constraint kont still emits its own op so its expr renders; post_lower
        // folds that op into this member and drops it.
        if (after && after->kind() == KontKind::EvalConstraintNode) {
            const KontNode* on_false = after->succ(0);
            bool hard = on_false->kind() == KontKind::AbortRuleKont ||
                        on_false->kind() == KontKind::OnFailKont;
            if (hard) {
                members.back()["where_id"] = e.nid(after);
                after = after->succ(1);
            }
        }
        m = after;
    }
    if (m && m->kind() == KontKind::AdvancePosNode) { e.absorbed.insert(m); m = m->succ(0); }
    return { members, prim_json(cont, e.default_le), e.next_id(m) };
}

// ── cg_jump ─────────────────────────────────────────────────────────
// In the stencil model the successor edge IS the tail-call target, already
// recorded in node().succ. cg_jump is the seam where a node overrides which
// successor id its stencil tail-calls (e.g. a read collapses past its store).
static inline void cg_jump(Emit&, const KontNode*) {}

} // namespace bbq::render

#endif
