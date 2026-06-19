#pragma once
//
// CaptureCow — ZCow: the zero-copy copy-on-write overlay over a parsed index.
//
// A parsed value is a node that is either Borrowed (offsets into the input — the
// FieldCapture index the parse built, copying nothing) or Owned (a value this
// overlay holds). ZCow is the persistent, path-copying tree over the Borrowed
// baseline:
//
//   * Every BBQ value is a typed container — a struct is a named-field container,
//     an array an element container, a leaf a value cell. Reads see Borrowed.
//   * Writing detaches the touched node Borrowed -> Owned (copy this node; its
//     children stay Borrowed views = structural sharing) and path-copies up to the
//     root, so each ancestor on the path becomes an Owned container pointing at the
//     new child while clean siblings stay Borrowed.
//   * A write goes *through* the container, which moderates it (the grammar type of
//     the slot governs how the value is encoded) and owns what is added.
//
// The overlay is keyed off the index node (FieldCapture*) so every face — the C++
// handles and the Python binding — mutates one shared tree and converges on one
// model. The writer (emit) then blits Borrowed ranges and re-serializes Owned ones.
//
#include "Capture.h"
#include "CaptureDecode.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bbq {

// Byte width of a fixed-width leaf type (for growing the output when an Owned
// leaf has no baseline span to measure — i.e. a from-scratch node).
inline size_t leaf_width(CaptureType t) {
    switch (t) {
    case CaptureType::UInt8: case CaptureType::Int8: case CaptureType::Bool: return 1;
    case CaptureType::UInt16LE: case CaptureType::UInt16BE:
    case CaptureType::Int16LE:  case CaptureType::Int16BE:  return 2;
    case CaptureType::UInt32LE: case CaptureType::UInt32BE:
    case CaptureType::Int32LE:  case CaptureType::Int32BE:
    case CaptureType::Float32LE: case CaptureType::Float32BE: return 4;
    case CaptureType::UInt64LE: case CaptureType::UInt64BE:
    case CaptureType::Int64LE:  case CaptureType::Int64BE:
    case CaptureType::Float64LE: case CaptureType::Float64BE: return 8;
    default: return 0;
    }
}

// An overlay node. Borrowed nodes carry only their baseline `src`; Owned nodes
// carry the value (Scalar/Bytes) or the owned children (Container). `src` is the
// baseline identity (type, offsets, child structure) and is null only for a
// from-scratch node with no parsed origin.
struct znode {
    enum class Kind : uint8_t { Borrowed, Scalar, Bytes, Container };
    // Integer encoding of a Scalar: a fixed-width field, or a variable-length
    // LEB128 varint (unsigned ULEB / signed SLEB). LEB leaves serialize through
    // encode_uleb128/sleb128, not the fixed-width encode.
    enum class Enc : uint8_t { Fixed, Uleb, Sleb };
    Kind kind = Kind::Borrowed;
    Enc enc = Enc::Fixed;
    const FieldCapture* src = nullptr;
    CaptureType type = CaptureType::UInt8;            // leaf type (Scalar/Bytes)
    int64_t ival = 0;                                 // Scalar (integer)
    double fval = 0;                                  // Scalar (float types)
    std::vector<uint8_t> bval;                         // Bytes
    std::vector<std::unique_ptr<znode>> kids;          // Container children, in order
};

// LEB128 encoders (the inverse of Machine.cpp's decode_uleb128/sleb128) — append
// the varint bytes to `out`. The writer's variable-length integer emit.
inline void encode_uleb128(std::vector<uint8_t>& out, uint64_t v) {
    do {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}
inline void encode_sleb128(std::vector<uint8_t>& out, int64_t v) {
    for (;;) {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;   // arithmetic shift (sign-propagating)
        bool done = (v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40));
        if (!done) b |= 0x80;
        out.push_back(b);
        if (done) break;
    }
}

namespace detail { inline void reserialize_node(const znode* z, const uint8_t* buf, std::vector<uint8_t>& out); }

struct zcow {
    std::unique_ptr<znode> root_;                              // overlay root; null until first write
    std::unordered_map<const FieldCapture*, znode*> by_src_;   // baseline node -> its overlay node

    // ── Path materialization (detach + path-copy) ───────────────────────────
    // The overlay node for baseline `n`, creating the whole path from the root and
    // upgrading each ancestor to an Owned container, so `n` is reachable from
    // root_->kids. `n` itself is returned in whatever state it is (a Borrowed leaf
    // ready to be set, or a container the caller will ensure_container()).
    znode* node_for(const FieldCapture* n) {
        auto it = by_src_.find(n);
        if (it != by_src_.end()) return it->second;
        if (!n->parent) {                       // the index root
            if (!root_) root_ = borrowed(n);    // don't clobber an existing (edited) root
            by_src_[n] = root_.get();           // register it like any node, so lookups find it
            return root_.get();
        }
        znode* par = node_for(n->parent);
        ensure_container(par);                  // creates n's Borrowed slot + registers it
        return by_src_.at(n);
    }

    // Upgrade a Borrowed overlay over a struct/array baseline into an Owned
    // container: materialize a Borrowed child for each baseline child (structural
    // sharing — only what is later written detaches further).
    void ensure_container(znode* z) {
        if (z->kind == znode::Kind::Container) return;
        z->kind = znode::Kind::Container;
        z->kids.clear();
        if (!z->src) return;
        for (int i = 0; i < z->src->child_count; i++) {
            auto k = borrowed(&z->src->children[i]);
            by_src_[k->src] = k.get();
            z->kids.push_back(std::move(k));
        }
    }

    // ── Moderated writes ─────────────────────────────────────────────────────
    // The slot's grammar type (n->type) moderates the value: emit encodes it at
    // that type's width. set detaches n to Owned and path-copies to the root.
    void set_int(const FieldCapture* n, int64_t v) {
        znode* z = node_for(n);
        z->kind = znode::Kind::Scalar;
        z->type = n->type;
        z->ival = v;
        z->bval.clear();
        z->kids.clear();
    }
    void set_bytes(const FieldCapture* n, std::vector<uint8_t> b) {
        znode* z = node_for(n);
        z->kind = znode::Kind::Bytes;
        z->type = n->type;
        z->bval = std::move(b);
        z->kids.clear();
    }
    // Set an integer scalar with an explicit encoding (Fixed/Uleb/Sleb) — used by
    // derived-field recompute, where a count/length field may itself be LEB.
    void set_scalar(const FieldCapture* n, int64_t v, znode::Enc e) {
        znode* z = node_for(n);
        z->kind = znode::Kind::Scalar;
        z->type = n->type;
        z->ival = v;
        z->enc = e;
        z->bval.clear();
        z->kids.clear();
    }
    void set_float(const FieldCapture* n, double d) {
        znode* z = node_for(n);
        z->kind = znode::Kind::Scalar;
        z->type = n->type;
        z->fval = d;
        z->bval.clear();
        z->kids.clear();
    }

    // Append an element into an array container (the container takes ownership). A
    // freshly-built element is Owned (no baseline src); an element spliced from
    // elsewhere is Borrowed, so register its subtree to keep the path-copy invariant.
    void append(const FieldCapture* array_n, std::unique_ptr<znode> elem) {
        znode* z = node_for(array_n);
        ensure_container(z);
        register_subtree(elem.get());
        z->kids.push_back(std::move(elem));
    }

    // Splice: replace node `n`'s content with a new subtree `sub` (an array-element
    // assignment `arr[i] = v`, or replacing a field's value wholesale). `n` keeps its
    // own identity (src=n, by_src_[n]→z); its outgoing children leave by_src_ (no
    // dangling) and the incoming subtree's baseline-derived descendants are registered.
    void set_node(const FieldCapture* n, std::unique_ptr<znode> sub) {
        znode* z = node_for(n);
        for (auto& k : z->kids) unregister_subtree(k.get());
        z->kind = sub->kind;
        z->type = sub->type;
        z->enc  = sub->enc;   // a varint leaf carries its LEB encoding
        z->ival = sub->ival;
        z->fval = sub->fval;
        z->bval = std::move(sub->bval);
        z->kids = std::move(sub->kids);
        for (auto& k : z->kids) register_subtree(k.get());
    }

    // Remove the i-th child of a container (array element / optional). Path-copies
    // and drops the kid; emit re-serializes the shorter container. The dropped kid's
    // WHOLE subtree leaves by_src_ — the child unique_ptr frees every descendant
    // znode, so leaving their baseline→znode entries behind would dangle.
    bool remove_index(const FieldCapture* container_n, size_t i) {
        znode* z = node_for(container_n);
        ensure_container(z);
        if (i >= z->kids.size()) return false;
        unregister_subtree(z->kids[i].get());
        z->kids.erase(z->kids.begin() + (std::ptrdiff_t)i);
        return true;
    }
    // Number of children a container currently has in the overlay (materializing
    // it from the baseline if untouched). Reflects appends/removes.
    size_t child_count(const FieldCapture* container_n) {
        znode* z = node_for(container_n);
        ensure_container(z);
        return z->kids.size();
    }
    // Read-only current element count: the overlay's if the container was touched,
    // else the baseline child count — never materializes.
    size_t cur_count(const FieldCapture* n) const {
        auto it = by_src_.find(n);
        if (it != by_src_.end() && it->second->kind == znode::Kind::Container)
            return it->second->kids.size();
        return n ? (size_t)n->child_count : 0;
    }
    // Read-only current byte length of a bytes leaf: the override's if set, else
    // the baseline span.
    size_t cur_bytes_len(const FieldCapture* n) const {
        auto it = by_src_.find(n);
        if (it != by_src_.end() && it->second->kind == znode::Kind::Bytes)
            return it->second->bval.size();
        return n ? (n->end_offset - n->start_offset) : 0;
    }
    // Serialized byte length of container `n`'s children from index `from` onward
    // in the current overlay — for recomputing an @rest window's size on edit.
    // SIZE_MAX if `n` is unmaterialized (no edit within → keep the baseline size).
    size_t window_len(const FieldCapture* n, int from, const uint8_t* buf) const {
        auto it = by_src_.find(n);
        if (it == by_src_.end() || it->second->kind != znode::Kind::Container) return (size_t)-1;
        const znode* z = it->second;
        std::vector<uint8_t> tmp;
        for (size_t i = (size_t)from; i < z->kids.size(); i++)
            detail::reserialize_node(z->kids[i].get(), buf, tmp);
        return tmp.size();
    }

    // ── Reads ──────────────────────────────────────────────────────────────
    const int64_t* get_int(const FieldCapture* n) const {
        auto it = by_src_.find(n);
        return (it != by_src_.end() && it->second->kind == znode::Kind::Scalar)
                   ? &it->second->ival : nullptr;
    }
    const std::vector<uint8_t>* get_bytes(const FieldCapture* n) const {
        auto it = by_src_.find(n);
        return (it != by_src_.end() && it->second->kind == znode::Kind::Bytes)
                   ? &it->second->bval : nullptr;
    }
    const double* get_float(const FieldCapture* n) const {
        auto it = by_src_.find(n);
        return (it != by_src_.end() && it->second->kind == znode::Kind::Scalar)
                   ? &it->second->fval : nullptr;
    }
    bool owned(const FieldCapture* n) const {
        auto it = by_src_.find(n);
        return it != by_src_.end() && (it->second->kind == znode::Kind::Scalar ||
                                       it->second->kind == znode::Kind::Bytes);
    }
    // A materialized container is on a write path — it has an Owned descendant.
    bool is_dirty(const FieldCapture* n) const {
        auto it = by_src_.find(n);
        return it != by_src_.end() && it->second->kind == znode::Kind::Container;
    }
    // The EFFECTIVE child count of a container after edits: the overlay znode's kid count
    // when it was mutated (append/remove materialize it), else -1 → use the baseline
    // FieldCapture's child_count. The generated writer reads this to recompute count fields.
    int overlay_child_count(const FieldCapture* n) const {
        auto it = by_src_.find(n);
        return (it != by_src_.end() && it->second->kind == znode::Kind::Container)
                   ? (int)it->second->kids.size() : -1;
    }
    bool empty() const { return root_ == nullptr; }

private:
    static std::unique_ptr<znode> borrowed(const FieldCapture* n) {
        auto z = std::make_unique<znode>();
        z->kind = znode::Kind::Borrowed;
        z->src = n;
        return z;
    }

    // Recursively drop a subtree's baseline→overlay entries from by_src_, before the
    // subtree's znodes are freed (remove) or replaced (set_node) — keeps the map free
    // of dangling pointers, the path-copying invariant (one overlay node per baseline).
    void unregister_subtree(const znode* z) {
        if (z->src) by_src_.erase(z->src);
        for (const auto& k : z->kids) unregister_subtree(k.get());
    }

    // The inverse: register every baseline-derived node a freshly-attached subtree
    // carries (a Borrowed element spliced/appended from elsewhere), so a later
    // node_for() on those baselines finds the same overlay node, not a divergent copy.
    void register_subtree(znode* z) {
        if (z->src) by_src_[z->src] = z;
        for (auto& k : z->kids) register_subtree(k.get());
    }
};

// The dotted/indexed path of an index node from the root: `ehdr.e_machine`,
// `shdrs[2].sh_type`. Anonymous (array-element) nodes contribute their index.
inline std::string node_path(const FieldCapture* n) {
    if (!n || !n->parent) return n && n->name ? n->name : std::string();
    std::string base = node_path(n->parent);
    if (n->name) return base.empty() ? n->name : base + "." + n->name;
    int idx = 0;
    for (int i = 0; i < n->parent->child_count; i++)
        if (&n->parent->children[i] == n) { idx = i; break; }
    return base + "[" + std::to_string(idx) + "]";
}

// One field's change: where it is, and old (from the baseline) vs new (the
// override). A structured, typed, path-addressed diff — not a byte diff.
struct delta_info {
    std::string path;
    size_t start = 0, end = 0;
    CaptureType type{};
    bool is_bytes = false;
    int64_t old_int = 0, new_int = 0;
    std::vector<uint8_t> old_bytes, new_bytes;
};

namespace detail {
inline void collect_deltas(const znode* z, const uint8_t* buf, std::vector<delta_info>& out) {
    if (!z) return;
    switch (z->kind) {
    case znode::Kind::Scalar: {
        int64_t ov = 0; bool sgn = false;
        decode_int(z->src, buf, &ov, &sgn);
        out.push_back({node_path(z->src), z->src->start_offset, z->src->end_offset,
                       z->src->type, false, ov, z->ival, {}, {}});
        break;
    }
    case znode::Kind::Bytes:
        out.push_back({node_path(z->src), z->src->start_offset, z->src->end_offset,
                       z->src->type, true, 0, 0,
                       std::vector<uint8_t>(buf + z->src->start_offset, buf + z->src->end_offset),
                       z->bval});
        break;
    case znode::Kind::Container:
        for (const auto& k : z->kids) collect_deltas(k.get(), buf, out);
        break;
    case znode::Kind::Borrowed:
        break;
    }
}

// Re-serialize one overlay node by appending its bytes to `out`. Borrowed blits
// its baseline span; Owned writes its value; a Container concatenates its kids in
// order (resize-safe for a contiguous layout). A Container whose baseline children
// are NOT laid out contiguously (a scattered / random-access region) cannot be
// rebuilt by concatenation without losing the scatter — refuse loudly rather than
// emit a corrupt buffer.
inline void reserialize_node(const znode* z, const uint8_t* buf, std::vector<uint8_t>& out) {
    switch (z->kind) {
    case znode::Kind::Borrowed:
        out.insert(out.end(), buf + z->src->start_offset, buf + z->src->end_offset);
        break;
    case znode::Kind::Scalar: {
        if (z->enc == znode::Enc::Uleb) { encode_uleb128(out, (uint64_t)z->ival); break; }
        if (z->enc == znode::Enc::Sleb) { encode_sleb128(out, z->ival); break; }
        size_t w = z->src ? (z->src->end_offset - z->src->start_offset) : leaf_width(z->type);
        size_t p = out.size();
        out.resize(p + w);
        if (is_float_type(z->type)) encode_float(out.data(), p, z->type, z->fval);
        else encode_int(out.data(), p, z->type, z->ival);
        break;
    }
    case znode::Kind::Bytes:
        out.insert(out.end(), z->bval.begin(), z->bval.end());
        break;
    case znode::Kind::Container: {
        if (z->src && z->src->child_count > 0) {
            size_t expect = z->src->start_offset;
            for (int i = 0; i < z->src->child_count; i++) {
                if (z->src->children[i].start_offset != expect)
                    throw std::runtime_error(
                        "zcow reserialize: '" + node_path(z->src) +
                        "' has a scattered layout (child gap/overlap) — re-serialize would"
                        " drop the scatter; mutate in place instead");
                expect = z->src->children[i].end_offset;
            }
        }
        for (const auto& k : z->kids) reserialize_node(k.get(), buf, out);
        break;
    }
    }
}

// Does the overlay change the byte length anywhere (a resized Bytes leaf, an
// appended/from-scratch node)? If so, in-place patching can't represent it and we
// must re-serialize.
inline bool changes_length(const znode* z) {
    switch (z->kind) {
    case znode::Kind::Borrowed:
        return false;
    case znode::Kind::Scalar:
        // Re-serialize (not in-place patch) when the leaf can't be patched at a fixed
        // baseline offset: a from-scratch / spliced-in value has no baseline span
        // (z->src == null), and a LEB varint's encoded width tracks its value. Only a
        // fixed-width scalar over a baseline span keeps its width and stays patchable.
        return z->src == nullptr || z->enc != znode::Enc::Fixed;
    case znode::Kind::Bytes:
        return !z->src || z->bval.size() != (z->src->end_offset - z->src->start_offset);
    case znode::Kind::Container:
        if (z->src && (int)z->kids.size() != z->src->child_count) return true;
        for (const auto& k : z->kids) if (changes_length(k.get())) return true;
        return false;
    }
    return false;
}

// In-place patch: write each Owned leaf at its baseline offset on a copy of the
// input (the canvas), preserving scattered layout and inter-field gaps verbatim.
inline void patch_node(const znode* z, std::vector<uint8_t>& out) {
    switch (z->kind) {
    case znode::Kind::Borrowed:
        break;
    case znode::Kind::Scalar:
        if (is_float_type(z->type)) encode_float(out.data(), z->src->start_offset, z->type, z->fval);
        else encode_int(out.data(), z->src->start_offset, z->type, z->ival);
        break;
    case znode::Kind::Bytes:
        std::memcpy(out.data() + z->src->start_offset, z->bval.data(), z->bval.size());
        break;
    case znode::Kind::Container:
        for (const auto& k : z->kids) patch_node(k.get(), out);
        break;
    }
}
}  // namespace detail

// The full delta set of an overlay against its baseline buffer.
inline std::vector<delta_info> deltas(const uint8_t* buf, const zcow& zc) {
    std::vector<delta_info> out;
    detail::collect_deltas(zc.root_.get(), buf, out);
    return out;
}

// emit() — the writer. An unmutated parse round-trips byte-identical (a memcpy of
// the input). With only same-width edits the input is the canvas and each Owned
// leaf is patched in place at its baseline offset (scattered/random-access layout
// preserved). Any length change (resized bytes, appended/from-scratch nodes)
// switches to a full re-serialize of the overlay tree, which is resize-safe for
// contiguous layouts and refuses (loudly) on a scattered container.
inline std::vector<uint8_t> emit(const uint8_t* buf, size_t len, const zcow& zc) {
    if (zc.empty()) return buf ? std::vector<uint8_t>(buf, buf + len) : std::vector<uint8_t>();
    if (!buf) {   // from-scratch construction: no baseline canvas, always re-serialize
        std::vector<uint8_t> out;
        detail::reserialize_node(zc.root_.get(), nullptr, out);
        return out;
    }
    if (detail::changes_length(zc.root_.get())) {
        std::vector<uint8_t> out;
        detail::reserialize_node(zc.root_.get(), buf, out);
        return out;
    }
    std::vector<uint8_t> out(buf, buf + len);
    detail::patch_node(zc.root_.get(), out);
    return out;
}

}  // namespace bbq
