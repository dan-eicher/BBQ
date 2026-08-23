#pragma once
//
// bbq_writer — the C++ ZCow writer's grammar-ENFORCEMENT context.
//
// ZCow (`bbq::emit`, CaptureCow.h) is the byte serializer — it knows bytes, not the file
// format. This context is the thin layer the generated writer uses to ENFORCE the grammar
// on the edited document (the `zcow` overlay over the `FieldCapture` baseline): walk the
// grammar, recompute the derived fields that edits invalidated — array `count`s (and, later,
// `@rest` sizes) — into the overlay, then `emit()`. It does NOT serialize bytes itself; that
// is `emit()`'s job (and it already handles add/delete by reserializing the overlay).
//
// Read → graph (the reader); mutate → overlay (the handles/zcow); writer enforces the
// grammar into the overlay; emit() serializes; re-parse verifies. The dual of the C side
// (reader → owning struct → writer), with the overlay as the edited document.
//
#include "bbq_machine.h"
#include "Capture.h"
#include "CaptureCow.h"
#include "bbq_node.h"       // bbq::node_child — content-based child lookup (names are literals here)

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bbq {

struct writer : machine {
    const uint8_t* data = nullptr;   // baseline bytes the graph spans point into
    size_t len = 0;                  // baseline length (for emit's identity/patch paths)
    zcow* zc = nullptr;              // the edited document (overlay); own_ when none supplied
    zcow own_;                       // empty overlay → emit() is identity (unmutated re-emit)

    // Graph cursor over the edited document (struct fields by name, arrays positional);
    // overlay-aware only where it matters: the EFFECTIVE child count of an edited array.
    struct frame { const FieldCapture* node; bool is_array; };
    std::vector<frame> scope;
    // count_id → the count field's node (recorded when its read is visited; the matching
    // array then sets it to the array's effective child count, so emit() writes a valid prefix).
    std::unordered_map<int, const FieldCapture*> count_field_;
    std::vector<const FieldCapture*> rest_stack_;   // open @rest windows: their size fields, LIFO
    const char* error = nullptr;

    writer(const FieldCapture* root, const uint8_t* buf, size_t length, zcow* overlay)
        : data(buf), len(length), zc(overlay ? overlay : &own_) { scope.push_back({root, false}); }

    bool fail(const char* m) { if (!error) error = m; return false; }

    // ── cursor (navigation only — no byte output) ──
    const FieldCapture* cur() const { return scope.back().node; }
    // The effective (post-edit) child count: the overlay's count when the array was mutated
    // (append/remove materialize it), else the baseline's.
    int eff_count(const FieldCapture* n) const {
        if (!n) return 0;
        int o = zc->overlay_child_count(n);
        return o >= 0 ? o : n->child_count;
    }
    const FieldCapture* field(const char* name) const {
        if (scope.empty()) return nullptr;
        const FieldCapture* s = scope.back().node;
        if (!name || !*name) {   // anonymous: array element (positional) / struct child[0]
            if (scope.back().is_array) { int i = (int)loop_index();
                return (s && i >= 0 && i < s->child_count) ? &s->children[i] : nullptr; }
            return (s && s->child_count > 0) ? &s->children[0] : nullptr;
        }
        for (auto it = scope.rbegin(); it != scope.rend(); ++it)   // outward lexical lookup
            if (const FieldCapture* c = node_child(it->node, name)) return c;
        return nullptr;
    }
    void enter_struct(const char* name) { scope.push_back({field(name), false}); }
    void leave() { scope.pop_back(); }
    bool has(const char* name) const { return field(name) != nullptr; }
    int variant_tag(const char* name) const { const FieldCapture* n = field(name); return n ? n->variant_tag : -1; }

    // Enter an array's element scope (so the body's per-element field lookups resolve against
    // children[loop_index]) and report its effective element count, so the caller loops the
    // body that many times — descending into element bodies enforces nested derived fields
    // (an inner counted array, an inner @rest) that emit() alone cannot fix.
    int64_t begin_array(const char* name) { const FieldCapture* a = field(name); scope.push_back({a, true}); return eff_count(a); }
    void end_array() { scope.pop_back(); }

    // ── grammar enforcement (the only writes — into the overlay, not the byte stream) ──
    void mark_count(int id, const char* field_name) { count_field_[id] = field(field_name); }
    void set_count(int id, int64_t v, znode::Enc enc) {
        auto it = count_field_.find(id);
        if (it != count_field_.end() && it->second) zc->set_scalar(it->second, v, enc);
    }
    int64_t eff_count_of(const char* array_field) const { return eff_count(field(array_field)); }

    // @rest: a size field whose value is the serialized byte length of the window that follows
    // it (the rest of its container). Recorded when its read is visited (the field is resolved
    // in scope, before any descent); recomputed at the window's close from the EDITED overlay
    // (window_len reserializes the post-size children) and written back, so a content edit that
    // changed the window's length yields a corrected size. SIZE_MAX = the container was never
    // touched → keep the stored size (an unmutated re-emit is byte-identity).
    void mark_rest(const char* size_field) { rest_stack_.push_back(field(size_field)); }
    void set_rest(znode::Enc enc) {
        if (rest_stack_.empty()) return;
        const FieldCapture* sz = rest_stack_.back(); rest_stack_.pop_back();
        if (!sz || !sz->parent) return;
        const FieldCapture* par = sz->parent;
        int idx = -1;
        for (int i = 0; i < par->child_count; i++) if (&par->children[i] == sz) { idx = i; break; }
        if (idx < 0) return;
        size_t wl = zc->window_len(par, idx + 1, data);
        if (wl != (size_t)-1) zc->set_scalar(sz, (int64_t)wl, enc);
    }

    // The bytes: ZCow serializes the (now grammar-consistent) overlay.
    std::vector<uint8_t> emit_bytes() { return bbq::emit(data, len, *zc); }
};

}  // namespace bbq
