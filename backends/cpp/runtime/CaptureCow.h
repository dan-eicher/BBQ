#pragma once
//
// CaptureCow — ZCow: the parsed document.
//
// ONE tree, ONE node type. A parser builds it; edits produce new versions of it;
// serializing walks it. There is no second structure and nothing overlays anything.
//
// A node's STORAGE is either the span it was parsed from, or a value the node owns:
//
//   parsed == true   the bytes at [start,end) in the input ARE this node's content.
//                    Reading decodes them in place; serializing blits them. Nothing
//                    is copied, which is the zero-copy half — a document nobody has
//                    written to is entirely this case, and serializing it is one
//                    memcpy of the root's span.
//
//   parsed == false  this node carries its own value (ival/fval/bval) or, for a
//                    container, is rebuilt from its children. Set by a write, and
//                    by every ancestor on the path to that write.
//
// Children are POINTERS, which is what makes copy-on-write work: a new version's
// node points at the *same* child nodes as the old one except along the path it
// changed (L'orange 2014, §2.4 / Listing 2.3 / Figure 2.2a). Copying a node copies
// its vector of child pointers, so untouched subtrees are shared, never duplicated.
// Editing one field of a 40 MB document allocates depth-many nodes.
//
// Transients (ch. 4) add an owner id per node: on the way down a write, a node this
// transient already made is mutated in place instead of cloned again (Listing 4.1),
// so repeated writes down one path copy it once. `persistent()` invalidates the
// transient, and every transient operation checks both that it is still live and
// that it is being used from the thread that created it (§7.2, §8.1.2).
//
#include "Capture.h"          // CaptureType — the grammar's primitive kinds
#include "CaptureDecode.h"    // bbq_rd_* / encode_int / encode_float

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bbq::zcow {

// Integer encoding of an owned scalar: a fixed-width field, or a variable-length
// LEB128 varint whose encoded width tracks its value.
enum class Enc : uint8_t { Fixed, Uleb, Sleb };

struct node;
using node_ptr = std::shared_ptr<node>;

struct node {
    // ── identity, from the grammar ──
    const char* name = nullptr;              // interned; null for an array element
    CaptureType type = CaptureType::UInt8;
    int variant_tag = -1;                    // which union/switch arm; -1 if none

    // ── storage ──
    size_t start_offset = 0, end_offset = 0; // the span this node was parsed from
    bool parsed = true;                      // the span still describes this node
    Enc enc = Enc::Fixed;                    // encoding of an owned integer
    int64_t ival = 0;
    double  fval = 0;
    std::vector<uint8_t> bval;

    // A value the grammar computes rather than reads. Neutral and arena-owned — the
    // same representation the C runtime mirrors as bbq_computed_value, so an index
    // built by either backend carries computed values identically.
    ComputedValue* computed_value = nullptr;

    // What this node's value is DERIVED from. The parser records it at the moment it
    // knows — when it reads a count and uses it to bound an array — for the same
    // reason variant_tag is recorded: so nothing has to re-derive the grammar
    // afterwards. Named rather than pointed at, because a name survives the path
    // copying that would leave a pointer aimed at the previous version.
    //
    // Nail's dependent field: "not exposed in the data model, but instead
    // transparently computed". Here it is computed when the document becomes a
    // document — see transient::commit().
    // The annotation lives on the node that CHANGES — the array, or the container
    // holding an @rest window — not on the dependent field itself. Only changed
    // nodes are walked when a document is committed, and the field a change
    // invalidates is often in a subtree that did not change: in
    // `struct { h: struct { n: uint8 }, xs: array<uint8>[h.n] }` appending to `xs`
    // leaves `h` untouched, so an annotation on `n` would never be reached.
    //
    // `determines` is a dotted path resolved from this node's scope outward, the
    // same lexical rule the parser uses when an expression names a field — `n` for a
    // sibling, `h.n` for one a struct deeper.
    enum class Derives : uint8_t {
        Nothing,
        HasCount,      // `determines` names the field holding my element count
        HasRestSize,   // `determines` names a field holding the byte length of
                       // everything that follows it inside me (@rest)
    };
    Derives derives = Derives::Nothing;
    const char* determines = nullptr;

    // Set when a child was added or dropped. A container that only had values
    // rewritten still occupies exactly the bytes it was parsed from, which is what
    // lets serializing patch the input in place and keep everything no leaf covers.
    bool resized = false;

    // ── Bitfields (Ftypes §3.4) ──
    //
    // A bits CONTAINER owns the run's bytes; `bits_msb_first` is Ftypes' `swap?`
    // already resolved against the host, so it says outright which way the run is
    // laid out. An ENTRY records "a flag indicating if the field is signed, the
    // starting bit of the field, and the ending bit of the field" — and owns no
    // storage: its value is DERIVED from the container's current bytes.
    //
    // That read model is IPG's (§4.2 computes GIF's flag bits as `num.val ≫ 8` and
    // `flag & 7` — arithmetic over a parsed byte, no field of its own), and it is
    // what makes a neighbour's write visible. IPG says nothing about writing them;
    // Ftypes §3.10 does, and it is read-modify-write on the container.
    struct bits_at {
        uint8_t start = 0, end = 0;   // [start, end) bits within the run
        bool is_signed = false;
        bool is_entry = false;
    };
    bool bits_container = false;
    bool bits_msb_first = false;
    bits_at bits;

    std::vector<node_ptr> kids;

    // Which transient owns this node, or 0 for a node reachable from a persistent
    // document. Listing 4.1: a path copy clones only when `owner != id`.
    uint64_t owner = 0;

    bool container() const {
        return type == CaptureType::Struct || type == CaptureType::Array;
    }
};

inline bool is_container_type(CaptureType t) {
    return t == CaptureType::Struct || t == CaptureType::Array;
}

// A fresh owner token per transient. Listing 4.1 uses a heap object compared by
// pointer; a counter is the same thing without the allocation.
inline uint64_t next_owner_id() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// ── Errors ───────────────────────────────────────────────────────────────────

struct type_error : std::runtime_error {
    explicit type_error(const std::string& w) : std::runtime_error(w) {}
};
// Def 4.1: an invalidated transient cannot be used as an argument to any function.
struct invalidated : std::runtime_error {
    explicit invalidated(const std::string& w) : std::runtime_error(w) {}
};

// ── Reading a node's value ───────────────────────────────────────────────────

namespace detail {

inline bool decode_int_at(const uint8_t* b, size_t off, CaptureType t, int64_t* out, bool* sgn) {
    *sgn = false;
    switch (t) {
    case CaptureType::UInt8:    *out = bbq_rd_u8(b, off); return true;
    case CaptureType::Bool:     *out = bbq_rd_u8(b, off) != 0; return true;
    case CaptureType::UInt16LE: *out = bbq_rd_u16le(b, off); return true;
    case CaptureType::UInt16BE: *out = bbq_rd_u16be(b, off); return true;
    case CaptureType::UInt32LE: *out = bbq_rd_u32le(b, off); return true;
    case CaptureType::UInt32BE: *out = bbq_rd_u32be(b, off); return true;
    case CaptureType::UInt64LE: *out = (int64_t)bbq_rd_u64le(b, off); return true;
    case CaptureType::UInt64BE: *out = (int64_t)bbq_rd_u64be(b, off); return true;
    case CaptureType::Int8:     *out = (int8_t)bbq_rd_u8(b, off); *sgn = true; return true;
    case CaptureType::Int16LE:  *out = (int16_t)bbq_rd_u16le(b, off); *sgn = true; return true;
    case CaptureType::Int16BE:  *out = (int16_t)bbq_rd_u16be(b, off); *sgn = true; return true;
    case CaptureType::Int32LE:  *out = (int32_t)bbq_rd_u32le(b, off); *sgn = true; return true;
    case CaptureType::Int32BE:  *out = (int32_t)bbq_rd_u32be(b, off); *sgn = true; return true;
    case CaptureType::Int64LE:  *out = (int64_t)bbq_rd_u64le(b, off); *sgn = true; return true;
    case CaptureType::Int64BE:  *out = (int64_t)bbq_rd_u64be(b, off); *sgn = true; return true;
    default: return false;
    }
}

inline bool decode_float_at(const uint8_t* b, size_t off, CaptureType t, double* out) {
    switch (t) {
    case CaptureType::Float32LE: *out = bbq_rd_f32le(b, off); return true;
    case CaptureType::Float32BE: *out = bbq_rd_f32be(b, off); return true;
    case CaptureType::Float64LE: *out = bbq_rd_f64le(b, off); return true;
    case CaptureType::Float64BE: *out = bbq_rd_f64be(b, off); return true;
    default: return false;
    }
}

}  // namespace detail

// Byte width of a fixed-width leaf type — for a node that owns its value and has no
// parsed span to take a width from.
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

// LEB128 encoders — the inverse of the varint decoders the readers use.
// Templated on where the bytes go, because a varint's WIDTH is a property of its value:
// the only honest way to ask how wide one is, is to encode it. A caller that wants the
// width and not the bytes hands in something that counts (see `sink`), and gets the
// answer from this encoder rather than from a second one that could disagree with it.
template <class Out>
inline void encode_uleb128(Out& out, uint64_t v) {
    do { uint8_t b = (uint8_t)(v & 0x7F); v >>= 7; if (v) b |= 0x80; out.push_back(b); } while (v);
}
template <class Out>
inline void encode_sleb128(Out& out, int64_t v) {
    for (;;) {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;                                  // arithmetic (sign-propagating)
        bool done = (v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40));
        if (!done) b |= 0x80;
        out.push_back(b);
        if (done) break;
    }
}

// ── The bytes a document was parsed from ─────────────────────────────────────
//
// Shared by every version: spans point into it, so it outlives them all. `keepalive`
// holds whatever owns the memory (a mapping, an arena, a Python buffer) so no
// version can outlive its own bytes.
struct source {
    const uint8_t* buf = nullptr;
    size_t len = 0;
    std::shared_ptr<void> keepalive;
};
using source_ptr = std::shared_ptr<const source>;

// Decode the value in a node's SPAN. This is the parser's own read rather than a
// consumer's: it works on a carrier that is not in the tree yet, and it reports
// failure instead of throwing, because a parse fails in its own way.
inline bool decode_int(const node* n, const uint8_t* buf, int64_t* out, bool* is_signed) {
    return n && detail::decode_int_at(buf, n->start_offset, n->type, out, is_signed);
}
inline bool decode_float(const node* n, const uint8_t* buf, double* out) {
    return n && detail::decode_float_at(buf, n->start_offset, n->type, out);
}

// A computed value the parse produced: a compute(), a varint, a bitfield run. Its
// bytes — if it has any — cannot be re-decoded at a fixed width, so the value is
// carried rather than read back out of the span.
inline bool computed_int(const node* n, int64_t* out) {
    if (!n->parsed || !n->computed_value) { *out = n->ival; return true; }
    switch (n->computed_value->kind) {
    case ComputedValue::Kind::Int:   *out = n->computed_value->i; return true;
    case ComputedValue::Kind::Float: *out = (int64_t)n->computed_value->f; return true;
    case ComputedValue::Kind::Bool:  *out = n->computed_value->b ? 1 : 0; return true;
    default: return false;
    }
}

// ── Serializing ──────────────────────────────────────────────────────────────

namespace detail {

// Where emitted bytes go. With a buffer it collects them; without one it only counts
// them. That is what lets an @rest window be MEASURED by the emitter itself: a separate
// "how many bytes would this be" function would be a second case analysis over the same
// nodes, and the day it disagreed with this one the size field would be quietly wrong.
struct sink {
    std::vector<uint8_t>* out;   // null: count only
    size_t n = 0;

    explicit sink(std::vector<uint8_t>* o) : out(o) { if (o) n = o->size(); }

    void raw(const uint8_t* p, size_t len) {
        if (out) out->insert(out->end(), p, p + len);
        n += len;
    }
    // A fixed-width leaf, encoded where it lands. Counting needs no encoding at all,
    // because a fixed width does not depend on the value.
    void fixed(size_t w, const node& nd) {
        if (!out) { n += w; return; }
        size_t p = out->size();
        out->resize(p + w);
        if (is_float_type(nd.type)) encode_float(out->data(), p, nd.type, nd.fval);
        else                        encode_int(out->data(), p, nd.type, nd.ival);
        n += w;
    }
    // A varint, whose width IS its value — so both faces go through the one encoder.
    void push_back(uint8_t) { n++; }             // the counting face of the encoders
    void uleb(uint64_t v) { if (out) { size_t b = out->size(); encode_uleb128(*out, v);
                                       n += out->size() - b; }
                            else encode_uleb128(*this, v); }
    void sleb(int64_t v)  { if (out) { size_t b = out->size(); encode_sleb128(*out, v);
                                       n += out->size() - b; }
                            else encode_sleb128(*this, v); }
};

inline void emit_node(const node& n, const source& src, const std::string& at, sink& out) {
    // Untouched: its bytes are still exactly what it was parsed from. One blit,
    // however large the subtree — this is the zero-copy payoff.
    if (n.parsed) {
        if (src.buf && n.end_offset > n.start_offset)
            out.raw(src.buf + n.start_offset, n.end_offset - n.start_offset);
        return;
    }
    if (n.type == CaptureType::Computed) {
        // A compute() occupies no bytes. A varint does — its value is decoded rather
        // than read at a fixed width, which is why it is recorded as computed at all
        // — so writing one back has to re-encode it. Which of the two this is, is the
        // ENCODING and not the span: a varint built from nothing never had a span, and
        // reading that as "occupies no bytes" would emit it as nothing.
        if (n.enc == Enc::Uleb) out.uleb((uint64_t)n.ival);
        else if (n.enc == Enc::Sleb) out.sleb(n.ival);
        return;
    }
    if (is_container_type(n.type)) {
        // A bitfield run owns the bytes its entries are read out of; the entries
        // themselves contribute none, so concatenating them would emit nothing.
        if (!n.bval.empty()) { out.raw(n.bval.data(), n.bval.size()); return; }
        for (size_t i = 0; i < n.kids.size(); i++) {
            const node& k = *n.kids[i];
            const char* nm = k.name;
            emit_node(k, src,
                      (nm && *nm) ? (at.empty() ? std::string(nm) : at + "." + nm)
                                  : at + "[" + std::to_string(i) + "]",
                      out);
        }
        return;
    }
    if (n.type == CaptureType::Bytes || n.type == CaptureType::String) {
        out.raw(n.bval.data(), n.bval.size());
        return;
    }
    if (n.enc == Enc::Uleb) { out.uleb((uint64_t)n.ival); return; }
    if (n.enc == Enc::Sleb) { out.sleb(n.ival); return; }
    size_t w = (n.end_offset > n.start_offset) ? (n.end_offset - n.start_offset)
                                               : leaf_width(n.type);
    if (w == 0)
        throw type_error("zcow emit: '" + (at.empty() ? std::string("<root>") : at) +
                         "' has no width for its type");
    out.fixed(w, n);
}

// Collecting the bytes is the common case; measuring is the other one.
inline void emit_node(const node& n, const source& src, const std::string& at,
                      std::vector<uint8_t>& out) {
    sink s(&out);
    emit_node(n, src, at, s);
}

// How many bytes this subtree occupies — by the emitter, so it cannot disagree with it.
inline size_t emitted_size(const node& n, const source& src) {
    sink s(nullptr);
    emit_node(n, src, std::string(), s);
    return s.n;
}

// Does anything under here occupy a different number of bytes than it was parsed
// from? If not, the input can be used as the canvas and only the changed leaves
// written over it — which is the only way to keep bytes NO LEAF COVERS: separators,
// padding, alignment, regions a seek jumped over. Concatenating children would drop
// every one of them.
inline bool changes_length(const node& n) {
    if (n.parsed) return false;
    if (n.resized) return true;
    if (!n.kids.empty()) {
        for (const auto& k : n.kids) if (changes_length(*k)) return true;
        return false;
    }
    // A computed value with no bytes cannot change any length; a varint's encoded
    // width tracks its value, so writing one can — and which it is, is the encoding,
    // for the same reason emitting one goes by the encoding.
    if (n.type == CaptureType::Computed) return n.enc != Enc::Fixed;
    if (n.end_offset <= n.start_offset) return true;    // no span: content with no origin
    if (n.enc != Enc::Fixed) return true;               // a varint's width tracks its value
    if (n.type == CaptureType::Bytes || n.type == CaptureType::String)
        return n.bval.size() != (n.end_offset - n.start_offset);
    return false;
}

// Write each changed leaf at its own offset on a copy of the input. Everything not
// written keeps the bytes it was parsed from, in place — gaps and scattered layout
// included.
inline void patch_node(const node& n, size_t base, std::vector<uint8_t>& out) {
    if (n.parsed) return;
    // A node that owns bytes writes them at its own offset, whatever its shape: a
    // bitfield run is a container for reading and a run of bytes for writing, and
    // its entries contribute none of their own.
    if (!n.bval.empty() && n.end_offset > n.start_offset && is_container_type(n.type)) {
        std::memcpy(out.data() + (n.start_offset - base), n.bval.data(), n.bval.size());
        return;
    }
    if (!n.kids.empty()) { for (const auto& k : n.kids) patch_node(*k, base, out); return; }
    if (n.type == CaptureType::Computed) return;
    size_t at = n.start_offset - base;
    if (n.type == CaptureType::Bytes || n.type == CaptureType::String)
        std::memcpy(out.data() + at, n.bval.data(), n.bval.size());
    else if (is_float_type(n.type)) encode_float(out.data(), at, n.type, n.fval);
    else                            encode_int(out.data(), at, n.type, n.ival);
}

// The bytes of one version: identity when untouched, patched in place when nothing
// resized, rebuilt from the tree otherwise.
inline std::vector<uint8_t> emit_document(const node* root, const source& s) {
    std::vector<uint8_t> out;
    if (!root) return out;
    if (!root->parsed && s.buf && !changes_length(*root)) {
        out.assign(s.buf + root->start_offset, s.buf + root->end_offset);
        patch_node(*root, root->start_offset, out);
        return out;
    }
    emit_node(*root, s, std::string(), out);
    return out;
}

}  // namespace detail

// ── Versions ─────────────────────────────────────────────────────────────────

class transient;

// A persistent document: one version of the tree, plus the bytes its spans name.
// Copying one is O(1) and shares everything — that is what persistence means here.
class document {
public:
    document() = default;
    document(node_ptr root, source_ptr src) : root_(std::move(root)), src_(std::move(src)) {}

    bool valid() const { return root_ != nullptr; }
    const node* root() const { return root_.get(); }
    const source& src() const { static const source none; return src_ ? *src_ : none; }

    // An unmodified parse round-trips byte-identical: the root is still `parsed`, so
    // this is a memcpy of the input.
    std::vector<uint8_t> serialize() const {
        return detail::emit_document(root_.get(), src());
    }

    // transient : α → α!  (Def 4.5). Structurally shared, O(1); this document is
    // never disturbed by anything done to the transient.
    transient begin_edit() const;

private:
    friend class transient;
    node_ptr   root_;
    source_ptr src_;
};

// What a parse produced: the document, or why there is not one. The document is a
// document; whether a parse succeeded, and where it gave up, is about the parse.
struct parse_result {
    document doc;
    bool success = false;
    size_t bytes_consumed = 0;
    const char* error_message = nullptr;
    size_t error_offset = 0;

    explicit operator bool() const { return success; }
};

// A transient: move-only and single-owner (§4.2). Every write invalidates the
// previous state, which C++ ownership makes unobservable — there is only one.
class transient {
public:
    transient(transient&&) noexcept = default;
    transient& operator=(transient&&) noexcept = default;
    transient(const transient&) = delete;
    transient& operator=(const transient&) = delete;

    bool live() const { return id_ != 0; }

    const node* root() const { check(); return root_.get(); }
    // The root, ready to be written through. Owning it is what a write down any path
    // would do first anyway.
    node* root_mut() { check(); return root_ ? own(root_) : nullptr; }
    const source& src() const { static const source none; return src_ ? *src_ : none; }

    // persistent! : α! → α  (Def 4.5). Consumes; the transient is dead afterwards,
    // and its id is cleared so any later use is caught (§4.2, p.54).
    document commit() && {
        check();
        // A dependent field is the grammar's to compute, not the caller's: make the
        // counts and @rest sizes agree with what the edits actually produced, so the
        // document that comes out is one the grammar accepts.
        std::vector<node*> scope;
        reconcile(root_, scope);
        id_ = 0;
        return document(std::move(root_), std::move(src_));
    }

    // Serializing a transient settles the dependent fields first, for exactly the reason
    // commit() does: what comes out has to be bytes the grammar accepts, and a count
    // nobody recomputed is a lie. Which is why this is not const — deriving a field
    // writes it. commit() is this same walk plus the end of the transient's life, so a
    // caller that wants bytes without giving up the ability to keep editing (the Python
    // binding, which never commits) gets the same answer here.
    std::vector<uint8_t> serialize() {
        check();
        std::vector<node*> scope;
        reconcile(root_, scope);
        return detail::emit_document(root_.get(), src());
    }

    // Take ownership on the calling thread. The thread check is a PROXY for single use —
    // that is the property ch.4 needs, and a thread that does not move is what C++ can
    // actually observe. A caller that enforces single use directly, by holding a mutex
    // across every operation on this transient, has the real property and states the
    // transfer here rather than tripping over the approximation of it. Calling this while
    // another thread is inside the transient is an error nothing can detect — which is
    // why the proxy stands for everyone who cannot make the guarantee.
    void adopt() {
        if (id_ == 0) throw invalidated("transient used after commit()");
        tid_ = std::this_thread::get_id();
    }

    // ── Writes. Each names a position by walking down from the root. ──

    // UPDATE (Listing 2.3) + TRANSIENT-CLONE (Listing 4.1): clone each node on the
    // path that this transient does not already own, mark it no longer described by
    // its parsed span, and hand back the node the path names. Off-path siblings keep
    // their pointers, so their subtrees are shared, not copied.
    node* own_path(const char* const* names, const size_t* idx, size_t n) {
        check();
        if (!root_) return nullptr;
        node* cur = own(root_);
        cur->parsed = false;
        for (size_t s = 0; s < n; s++) {
            size_t slot;
            if (!slot_of(*cur, names ? names[s] : nullptr, idx ? idx[s] : 0, &slot)) return nullptr;
            cur = own(cur->kids[slot]);
            cur->parsed = false;
        }
        return cur;
    }

    node* own_child(node* parent, size_t i) {
        check();
        if (!parent || i >= parent->kids.size()) return nullptr;
        // The parent's span stops describing it too: owning a child in order to write
        // it is one step of the path copy, and own_path marks every step for the same
        // reason. Missing this leaves a document whose root still claims to be exactly
        // its input, so nothing below it is ever reconciled.
        parent->parsed = false;
        node* k = own(parent->kids[i]);
        k->parsed = false;
        return k;
    }

    // The same by name, because a struct field is something the grammar names —
    // the write-side counterpart of child_named. Without it every caller writes the
    // name→index scan itself.
    node* own_child(node* parent, std::string_view name) {
        check();
        if (!parent) return nullptr;
        for (size_t i = 0; i < parent->kids.size(); i++)
            if (parent->kids[i]->name && name == parent->kids[i]->name)
                return own_child(parent, i);
        return nullptr;
    }

    // Replace what a node holds with raw bytes — the byte-level sibling of append and
    // remove. A substructure whose shape the caller is not describing (an element
    // spliced in from elsewhere, a re-encoded blob) becomes exactly these bytes, and its
    // children go with it, and so does its type: what it holds afterwards is bytes, and
    // saying so is what makes every reader of the node — emitting, patching, measuring
    // whether the length moved — take the one branch that is now true of it. `resized`
    // still has to be set for a node with no span, whose length nothing can be compared
    // against.
    node* splice(node* n, const uint8_t* b, size_t len) {
        check();
        if (!n) return nullptr;
        n->kids.clear();
        n->bval.assign(b, b + len);
        n->type = CaptureType::Bytes;
        n->enc = Enc::Fixed;
        n->parsed = false;
        if (len != n->end_offset - n->start_offset) n->resized = true;
        return n;
    }

    // conj! — append an element and return it. It has no parsed span; nothing else
    // could name it, which is why the append hands it back.
    node* append(node* container, CaptureType t) {
        check();
        if (!container) return nullptr;
        auto e = std::make_shared<node>();
        e->type = t; e->parsed = false; e->owner = id_;
        container->parsed = false;
        container->resized = true;
        container->kids.push_back(std::move(e));
        return container->kids.back().get();
    }

    // The same, for a subtree that already exists — one built from nothing, or lifted
    // out of another document. It arrives owned by nobody, so the first write down into
    // it clones exactly as it would for anything else shared; nothing here is special
    // about where it came from. This is what lets a constructed value BE structure in
    // the document instead of collapsing to the bytes it would have serialized to.
    node* append(node* container, node_ptr child) {
        check();
        if (!container || !child) return nullptr;
        container->parsed = false;
        container->resized = true;
        container->kids.push_back(std::move(child));
        return container->kids.back().get();
    }

    bool replace(node* container, size_t i, node_ptr child) {
        check();
        if (!container || !child || i >= container->kids.size()) return false;
        container->parsed = false;
        container->resized = true;
        container->kids[i] = std::move(child);
        return true;
    }

    // pop! — drop an element. Nothing dangles: a caller holding the removed node's
    // pointer holds a live shared_ptr to a node that is simply no longer in the tree.
    bool remove(node* container, size_t i) {
        check();
        if (!container || i >= container->kids.size()) return false;
        container->parsed = false;
        container->resized = true;
        container->kids.erase(container->kids.begin() + (std::ptrdiff_t)i);
        return true;
    }

private:
    friend class document;
    transient(node_ptr root, source_ptr src)
        : root_(std::move(root)), src_(std::move(src)),
          id_(next_owner_id()), tid_(std::this_thread::get_id()) {}

    // §4.2 p.54: the id is checked on every transient operation and cleared by
    // persistent!. §7.2/§8.1.2: and the thread is checked, because C++ cannot
    // enforce single-use at compile time any more than C can.
    void check() const {
        if (id_ == 0) throw invalidated("transient used after commit()");
        if (tid_ != std::this_thread::get_id())
            throw invalidated("transient used from a thread other than the one that created it");
    }

    node* own(node_ptr& slot) {
        if (slot->owner == id_) return slot.get();
        auto c = std::make_shared<node>(*slot);   // copies the child-pointer vector
        c->owner = id_;
        slot = std::move(c);
        return slot.get();
    }

    // Recompute the dependent fields a container holds. A subtree that is still
    // span-backed cannot have gone stale — nothing in it changed — so the walk stops
    // there, which is what keeps this proportional to the edit rather than the file.
    //
    // Order matters: descend first, so an inner array's count is right before an
    // outer window measures the bytes it produces; then counts at this level, since
    // a LEB count's own width feeds the window length; then the windows.
    void reconcile(node_ptr& slot, std::vector<node*>& scope) {
        if (!slot || slot->parsed) return;
        node* n = own(slot);
        scope.push_back(n);

        for (auto& k : n->kids) {
            if (k->parsed) continue;
            // By TYPE, not by emptiness: an array every element was removed from
            // still determines its count, and still has to be walked to say so.
            if (is_container_type(k->type)) reconcile(k, scope);
            else                            restore_if_unchanged(k);
        }

        // Inner first: an inner array's count is settled before an outer window
        // measures the bytes it produces.
        if (n->derives == node::Derives::HasCount) {
            std::vector<node*> chain;
            if (node_ptr* c = resolve(scope, n->determines, chain))
                // Only if it actually wrote: a container marked dirty for nothing
                // would be rebuilt from its children and lose any bytes they do not
                // cover.
                if (set_derived(*c, (int64_t)n->kids.size()))
                    for (node* a : chain) a->parsed = false;
        } else if (n->derives == node::Derives::HasRestSize) {
            for (size_t i = 0; i < n->kids.size(); i++) {
                if (!n->kids[i]->name || !n->determines ||
                    std::strcmp(n->kids[i]->name, n->determines) != 0) continue;
                // Measured by the emitter rather than by a second walk that knows how
                // wide things are: nothing is collected, only counted.
                size_t window = 0;
                for (size_t j = i + 1; j < n->kids.size(); j++)
                    window += detail::emitted_size(*n->kids[j], src());
                set_derived(n->kids[i], (int64_t)window);
                break;
            }
        }

        scope.pop_back();
    }

    // Resolve a dotted path from the innermost scope outward, owning each node on
    // the way so the result can be written. Innermost-first is the parser's own
    // lexical rule, so a name means here what it meant when the grammar was read.
    // `chain` collects the containers stepped through, so the caller can mark them
    // no longer described by their spans — but only if it goes on to write.
    node_ptr* resolve(const std::vector<node*>& scope, const char* path,
                      std::vector<node*>& chain) {
        if (!path || !*path) return nullptr;
        const char* dot = std::strchr(path, '.');
        std::string head(path, dot ? (size_t)(dot - path) : std::strlen(path));

        for (size_t d = scope.size(); d > 0; --d) {
            node* holder = scope[d - 1];
            for (auto& k : holder->kids) {
                if (!k->name || head != k->name) continue;
                if (!dot) return &k;
                node* inner_node = own(k);    // descending means we may write below
                chain.push_back(inner_node);
                std::vector<node*> inner{inner_node};
                return resolve(inner, dot + 1, chain);
            }
        }
        return nullptr;
    }

    // A write that changed nothing should leave nothing behind: if a leaf now holds
    // exactly what its span already said, put it back to span-backed so the original
    // bytes are blitted verbatim.
    //
    // This is what makes GetPut hold — read a field, write it back unchanged, and
    // the document is byte-identical. Without it, a read that is not injective
    // rewrites the field's representation: a Bool byte of 0x11 reads as true and
    // would be written back as 0x01, silently changing a file nobody edited.
    void restore_if_unchanged(node_ptr& slot) {
        const node* c = slot.get();
        if (c->parsed || !c->kids.empty()) return;
        if (c->end_offset <= c->start_offset || !src().buf) return;

        if (c->type == CaptureType::Computed) {
            int64_t was = 0;
            node was_parsed = *c; was_parsed.parsed = true;
            if (computed_int(&was_parsed, &was) && was == c->ival) own(slot)->parsed = true;
            return;
        }

        if (c->type == CaptureType::Bytes || c->type == CaptureType::String) {
            size_t w = c->end_offset - c->start_offset;
            if (c->bval.size() != w ||
                std::memcmp(c->bval.data(), src().buf + c->start_offset, w) != 0) return;
            node* m = own(slot); m->parsed = true; m->bval.clear();
            return;
        }
        if (c->enc != Enc::Fixed) return;   // a varint's encoded width tracks its value
        if (is_float_type(c->type)) {
            double d = 0;
            if (decode_float(c, src().buf, &d) && d == c->fval) own(slot)->parsed = true;
            return;
        }
        int64_t v = 0; bool sgn = false;
        if (decode_int(c, src().buf, &v, &sgn) && v == c->ival) own(slot)->parsed = true;
    }

    // Write a dependent field only if it does not already say the right thing. A
    // field whose value is unchanged stays span-backed and shared, so an edit that
    // does not move a count leaves no trace on it — an array element rewritten in
    // place must not dirty the array's length.
    bool set_derived(node_ptr& slot, int64_t want) {
        int64_t have = 0;
        if (slot->type == CaptureType::Computed) {
            if (!computed_int(slot.get(), &have)) have = want + 1;
        } else if (slot->parsed) {
            bool sgn = false;
            if (!decode_int(slot.get(), src().buf, &have, &sgn)) have = want + 1;
        } else {
            have = slot->ival;
        }
        if (have == want) return false;
        node* c = own(slot);
        c->parsed = false;
        c->ival = want;
        c->bval.clear();
        return true;
    }

    static bool slot_of(const node& cur, const char* name, size_t i, size_t* out) {
        if (!name) { if (i >= cur.kids.size()) return false; *out = i; return true; }
        for (size_t k = 0; k < cur.kids.size(); k++)
            if (cur.kids[k]->name && std::strcmp(cur.kids[k]->name, name) == 0) { *out = k; return true; }
        return false;
    }

    node_ptr   root_;
    source_ptr src_;
    uint64_t   id_ = 0;
    std::thread::id tid_;
};

inline transient document::begin_edit() const { return transient(root_, src_); }

// ── Reading ──────────────────────────────────────────────────────────────────
//
// Free functions over (node, source), so the same code serves a document and a
// transient — Def 4.5's f!_r is the read on the transient, not a different read.

inline size_t size_of(const node* n) { return n ? n->kids.size() : 0; }

inline const node* child_at(const node* n, size_t i) {
    return (n && i < n->kids.size()) ? n->kids[i].get() : nullptr;
}

inline const node* child_named(const node* n, std::string_view nm) {
    if (!n) return nullptr;
    for (const auto& k : n->kids)
        if (k->name && nm == k->name) return k.get();
    return nullptr;
}

inline int64_t read_int(const node* n, const source& src) {
    if (!n) throw type_error("read on a node that does not exist");
    if (n->type == CaptureType::Computed) {
        int64_t v = 0;
        if (!computed_int(n, &v)) throw type_error("computed field is not integer-convertible");
        return v;
    }
    if (!n->parsed) return is_float_type(n->type) ? (int64_t)n->fval : n->ival;
    int64_t v = 0; bool sgn = false;
    if (!detail::decode_int_at(src.buf, n->start_offset, n->type, &v, &sgn))
        throw type_error("field is not integer-convertible");
    return v;
}

inline double read_float(const node* n, const source& src) {
    if (!n) throw type_error("read on a node that does not exist");
    if (n->type == CaptureType::Computed) {
        if (!n->parsed) return is_float_type(n->type) ? n->fval : (double)n->ival;
        if (n->computed_value && n->computed_value->kind == ComputedValue::Kind::Float)
            return n->computed_value->f;
        int64_t v = 0;
        if (!computed_int(n, &v)) throw type_error("computed field is not numeric");
        return (double)v;
    }
    if (!n->parsed) return is_float_type(n->type) ? n->fval : (double)n->ival;
    double d = 0;
    if (detail::decode_float_at(src.buf, n->start_offset, n->type, &d)) return d;
    int64_t v = 0; bool sgn = false;
    if (!detail::decode_int_at(src.buf, n->start_offset, n->type, &v, &sgn))
        throw type_error("field is not numeric");
    return (double)v;
}

// The bytes of a leaf: straight out of the mapping when untouched, out of the node
// when written. Valid while the document that owns them is alive.
inline std::pair<const uint8_t*, size_t> read_bytes(const node* n, const source& src) {
    if (!n) throw type_error("read on a node that does not exist");
    if (n->parsed) return { src.buf + n->start_offset, n->end_offset - n->start_offset };
    return { n->bval.data(), n->bval.size() };
}

inline std::string_view read_str(const node* n, const source& src) {
    auto b = read_bytes(n, src);
    return std::string_view((const char*)b.first, b.second);
}

// ── Writing a leaf ───────────────────────────────────────────────────────────
//
// The node's grammar type moderates the value: serializing encodes it at that type.

inline void set_int(node* n, int64_t v, Enc e = Enc::Fixed) {
    if (!n) throw type_error("write to a node that does not exist");
    if (is_float_type(n->type)) throw type_error("integer assigned to a float field");
    n->parsed = false; n->ival = v; n->enc = e; n->bval.clear();
}

inline void set_float(node* n, double v) {
    if (!n) throw type_error("write to a node that does not exist");
    if (!is_float_type(n->type)) throw type_error("float assigned to a non-float field");
    n->parsed = false; n->fval = v; n->bval.clear();
}

inline void set_bytes(node* n, const uint8_t* b, size_t len) {
    if (!n) throw type_error("write to a node that does not exist");
    n->parsed = false; n->bval.assign(b, b + len);
}

inline void set_str(node* n, std::string_view s) {
    set_bytes(n, (const uint8_t*)s.data(), s.size());
}

// ── Bitfields ────────────────────────────────────────────────────────────────
//
// Both directions name the CONTAINER and the entry, the way Ftypes' accessor path
// does (`(ftype-ref x-type (b x) my-x)`), because an entry has no storage of its
// own — its value lives in the run's bytes and only the container has those.

namespace detail {

inline const node* bits_entry(const node* container, std::string_view name) {
    if (!container) throw type_error("bitfield access on a node that does not exist");
    for (const auto& k : container->kids)
        if (k->name && name == k->name && k->bits.is_entry) return k.get();
    throw type_error("no bitfield entry '" + std::string(name) + "' in this run");
}

// The run's CURRENT bytes: what has been written into it, or failing that what it
// was parsed from. Not `read_bytes`, because a container on the path to some other
// edit is marked no longer described by its span while its own bytes are still
// exactly that — `parsed` says "something under me changed", not "my bytes are gone".
inline std::pair<const uint8_t*, size_t> run_bytes(const node* c, const source& src) {
    if (!c->bval.empty()) return { c->bval.data(), c->bval.size() };
    if (c->end_offset > c->start_offset && src.buf)
        return { src.buf + c->start_offset, c->end_offset - c->start_offset };
    return { nullptr, 0 };
}

// The run's bytes as one integer, in the order the run is laid out.
inline uint64_t bits_word(const node* c, const source& src) {
    auto b = run_bytes(c, src);
    if (b.second == 0 || b.second > 8) throw type_error("bitfield run has no usable width");
    uint64_t w = 0;
    if (c->bits_msb_first) for (size_t i = 0; i < b.second; i++) w = (w << 8) | b.first[i];
    else                   for (size_t i = b.second; i-- > 0; )  w = (w << 8) | b.first[i];
    return w;
}

}  // namespace detail

inline int64_t read_bits(const node* container, std::string_view entry, const source& src) {
    const node* e = detail::bits_entry(container, entry);
    const unsigned width = (unsigned)(e->bits.end - e->bits.start);
    const uint64_t mask = (width >= 64) ? ~uint64_t(0) : ((uint64_t(1) << width) - 1);
    uint64_t v = (detail::bits_word(container, src) >> e->bits.start) & mask;
    // Ftypes records signedness per entry; its own example reads `[x signed 4]`
    // holding 0xD back as -3.
    if (e->bits.is_signed && width && (v & (uint64_t(1) << (width - 1))))
        return (int64_t)v - (int64_t)(uint64_t(1) << width);
    return (int64_t)v;
}

inline void write_bits(node* container, std::string_view entry, const source& src, int64_t v) {
    const node* e = detail::bits_entry(container, entry);
    const size_t nbytes = detail::run_bytes(container, src).second;
    const unsigned width = (unsigned)(e->bits.end - e->bits.start);
    const uint64_t mask = (width >= 64) ? ~uint64_t(0) : ((uint64_t(1) << width) - 1);

    uint64_t w = detail::bits_word(container, src);
    w = (w & ~(mask << e->bits.start)) | (((uint64_t)v & mask) << e->bits.start);

    std::vector<uint8_t> out(nbytes);
    for (size_t i = 0; i < nbytes; i++)
        out[container->bits_msb_first ? nbytes - 1 - i : i] = (uint8_t)(w >> (i * 8));
    // The run keeps its entries — they are still read through it — and takes on the
    // bytes it now stands for. Same width, so serializing still patches in place.
    container->parsed = false;
    container->bval = std::move(out);
}

// ── Bitfields ────────────────────────────────────────────────────────────────
//
// An ENTRY has no storage of its own: it is a run of bits inside a containing
// scalar, and only the grammar knows where. Both directions therefore go through
// the CONTAINER, with the layout supplied by whoever has it from the grammar.
//
// Reading through the container rather than through the entry's own recorded value
// is what makes a write visible: that value is what the parse decoded, and it does
// not move when a neighbouring entry is written.

// The container's bytes as one integer, in the container's own byte order.
inline uint64_t packed_bits(const node* n, const source& src, bool big_endian) {
    auto b = read_bytes(n, src);
    uint64_t w = 0;
    if (big_endian) for (size_t i = 0; i < b.second; i++) w = (w << 8) | b.first[i];
    else            for (size_t i = b.second; i-- > 0; )  w = (w << 8) | b.first[i];
    return w;
}

inline uint64_t read_bits(const node* n, const source& src,
                          unsigned shift, uint64_t mask, bool big_endian) {
    return (packed_bits(n, src, big_endian) >> shift) & mask;
}

inline void write_bits(node* n, const source& src,
                       unsigned shift, uint64_t mask, bool big_endian, uint64_t v) {
    if (!n) throw type_error("write to a node that does not exist");
    const size_t w = read_bytes(n, src).second;
    if (w == 0 || w > 8) throw type_error("bitfield container has no usable width");
    uint64_t packed = packed_bits(n, src, big_endian);
    packed = (packed & ~(mask << shift)) | ((v & mask) << shift);
    std::vector<uint8_t> out(w);
    for (size_t i = 0; i < w; i++)
        out[big_endian ? w - 1 - i : i] = (uint8_t)(packed >> (i * 8));
    // The container keeps its children — the entries are still read through it — and
    // takes on the bytes it now stands for. Same width, so this still patches in place.
    n->parsed = false;
    n->bval = std::move(out);
}


// ── Building: what a parser produces ─────────────────────────────────────────
//
// The parser builds THIS tree — there is nothing else to build. Nodes come out
// `parsed`, so a document nobody edits owns no values at all and serializes as one
// memcpy.
class builder {
public:
    // Where the build had got to. A parse that speculates (an alternative, a
    // lookahead) marks here and restores if the attempt fails, so the index rolls
    // back with the cursor. Truncating a subtree is a resize: the scopes above it
    // are still open, and everything a failed attempt attached hangs off the node
    // that was current when the mark was taken.
    struct Mark { size_t depth = 0; size_t kids = 0; };

    Mark mark() const {
        return { stack_.size(), stack_.empty() ? roots_.size() : stack_.back()->kids.size() };
    }
    void restore(Mark m) {
        if (m.depth < stack_.size()) stack_.resize(m.depth);
        auto& kids = stack_.empty() ? roots_ : stack_.back()->kids;
        if (m.kids < kids.size()) kids.resize(m.kids);
    }

    // Start offset of the innermost open scope — backs the `buffer` builtin (the
    // current construct's [start, pos)). Backtrack-safe: restore() rewinds the stack.
    size_t current_scope_start() const { return stack_.empty() ? 0 : stack_.back()->start_offset; }

    // ── Scopes ──
    void begin_struct(const char* name, size_t pos) { open(name, CaptureType::Struct, pos, take_pending_tag()); }
    void begin_array(const char* name, size_t pos)  {
        open(name, CaptureType::Array, pos, take_pending_tag());
        if (pending_count_field_) {
            stack_.back()->derives = node::Derives::HasCount;
            stack_.back()->determines = pending_count_field_;
            pending_count_field_ = nullptr;
        }
    }

    // Armed when the parser knows an array's bound is a field — that field IS its
    // element count — and consumed by the begin_array that follows. Armed early
    // because a counted array does not open its scope until the bound has been
    // evaluated. Same shape as the pending variant tag.
    void set_pending_count_field(const char* path) { pending_count_field_ = path; }
    void begin_variant(const char* name, size_t pos, int tag) { open(name, CaptureType::Struct, pos, tag); }
    // Closing hands the container back, so the parser can record what it determines
    // at the moment it knows — the count field it was bounded by, the @rest size
    // field it fills.
    node* end_struct(size_t pos) { return close(pos); }
    node* end_array(size_t pos)  { return close(pos); }

    // A grammar's top level is a bare sequence of fields wrapped by finish(), so
    // there is no scope to close for it. This records what that wrapper determines.
    void root_determines(node::Derives d, const char* name) {
        root_derives_ = d; root_determines_ = name;
    }

    // The scope currently open, so a parser can annotate it as it opens it rather
    // than having to hold the node until it closes.
    node* open_scope() { return stack_.empty() ? nullptr : stack_.back().get(); }

    // The innermost scope IS a bitfield run: it owns the bytes its entries are read
    // out of, and `msb_first` says which way they are laid out (Ftypes' `swap?`,
    // already resolved against the host). A top-level bitfield rule is inlined flat,
    // so its run is the wrapper finish() builds — whose span is exactly the run.
    void scope_is_bits_run(bool msb_first) {
        if (node* s = open_scope()) { s->bits_container = true; s->bits_msb_first = msb_first; }
        else { root_bits_ = true; root_bits_msb_first_ = msb_first; }
    }

    // The innermost scope holds a field measuring the bytes that follow it — a
    // window opened over the remainder of a struct. At the top level there is no
    // scope to annotate, so it goes on the wrapper finish() builds.
    void scope_determines_rest(const char* size_field) {
        if (node* s = open_scope()) {
            s->derives = node::Derives::HasRestSize;
            s->determines = size_field;
        } else {
            root_determines(node::Derives::HasRestSize, size_field);
        }
    }

    // Armed at a switch dispatch, consumed by the arm it selects.
    void set_pending_variant_tag(int tag) { pending_variant_tag_ = tag; }

    // ── Leaves. Return the node so the caller can finish filling it in — a computed
    // field's value is produced after the capture exists. ──
    node* add_field(const char* name, size_t start, size_t end, CaptureType t,
                    ComputedValue* v = nullptr) {
        auto n = std::make_shared<node>();
        n->name = name; n->type = t;
        n->start_offset = start; n->end_offset = end; n->computed_value = v;
        // A leaf takes the pending tag too: a switch arm that is a plain scalar opens
        // no scope, so this is the only place its decision can be recorded.
        n->variant_tag = take_pending_tag();
        return attach(std::move(n));
    }
    node* add_computed(const char* name, ComputedValue* v, size_t pos) {
        return add_field(name, pos, pos, CaptureType::Computed, v);
    }
    node* add_computed(const char* name, ComputedValue* v, size_t start, size_t end) {
        return add_field(name, start, end, CaptureType::Computed, v);
    }

    // A node that is NOT in the tree: a parser carries one as a value — the span,
    // type and computed value a construct produced — before a later step records it
    // with add_field or binds it into the environment. It is kept alive for the
    // parse, the way an arena-allocated capture was.
    node* make_free() {
        free_.push_back(std::make_shared<node>());
        return free_.back().get();
    }

    // ── Mid-parse lookup ──
    //
    // An expression may name a field in any ENCLOSING scope, not just the innermost
    // (a per-element interval referencing a header field parsed before the array
    // opened). Walking the open scopes outward, each scanned backward, gives correct
    // lexical shadowing — innermost, most recent, first.
    node* find_field(const char* name) const { return lookup(name, false); }
    node* find_field_str(const char* name) const { return lookup(name, true); }

    node* find_child(const node* parent, const char* name) const { return kid(parent, name, false); }
    node* find_child_str(const node* parent, const char* name) const { return kid(parent, name, true); }
    node* find_child_at(const node* parent, int index) const {
        if (!parent || index < 0 || (size_t)index >= parent->kids.size()) return nullptr;
        return parent->kids[(size_t)index].get();
    }

    // ── Result ──
    parse_result finish(bool success, size_t bytes_consumed, const uint8_t* buf, size_t len,
                        const char* error_message = nullptr, size_t error_offset = 0,
                        std::shared_ptr<void> keepalive = nullptr) {
        parse_result r;
        r.success = success;
        r.bytes_consumed = bytes_consumed;
        r.error_message = error_message;
        r.error_offset = error_offset;

        auto s = std::make_shared<source>();
        s->buf = buf; s->len = len; s->keepalive = std::move(keepalive);
        if (!success || roots_.empty()) { r.doc = document(nullptr, std::move(s)); return r; }
        // A grammar's top level is a sequence of fields, and the document root wraps
        // them — ALWAYS, even when there is exactly one. Returning a lone anonymous
        // field as the root instead would make a rule's shape depend on how many
        // top-level fields it happened to produce, and the two producers do not always
        // produce the same number (a top-level switch arm is a scope in one and flat in
        // the other).
        auto root = std::make_shared<node>();
        root->type = CaptureType::Struct;
        root->start_offset = 0; root->end_offset = bytes_consumed;
        root->derives = root_derives_;
        root->determines = root_determines_;
        root->bits_container = root_bits_;
        root->bits_msb_first = root_bits_msb_first_;
        root->kids = std::move(roots_);
        r.doc = document(std::move(root), std::move(s));
        return r;
    }

    void clear() {
        stack_.clear(); roots_.clear(); free_.clear();
        pending_variant_tag_ = -1;
        pending_count_field_ = nullptr;
        root_derives_ = node::Derives::Nothing; root_determines_ = nullptr;
        root_bits_ = false; root_bits_msb_first_ = false;
    }

private:
    void open(const char* name, CaptureType t, size_t pos, int tag) {
        auto n = std::make_shared<node>();
        n->name = name; n->type = t;
        n->start_offset = pos; n->end_offset = pos; n->variant_tag = tag;
        stack_.push_back(std::move(n));
    }
    node* close(size_t pos) {
        if (stack_.empty()) return nullptr;
        auto n = std::move(stack_.back());
        stack_.pop_back();
        n->end_offset = pos;
        return attach(std::move(n));
    }
    node* attach(node_ptr n) {
        node* raw = n.get();
        (stack_.empty() ? roots_ : stack_.back()->kids).push_back(std::move(n));
        return raw;
    }
    int take_pending_tag() { int t = pending_variant_tag_; pending_variant_tag_ = -1; return t; }

    static bool same(const char* a, const char* b, bool by_content) {
        if (!a || !b) return false;
        return by_content ? std::strcmp(a, b) == 0 : a == b;
    }
    node* lookup(const char* name, bool by_content) const {
        for (size_t d = stack_.size(); d > 0; --d) {
            const auto& kids = stack_[d - 1]->kids;
            for (size_t i = kids.size(); i > 0; --i)
                if (same(kids[i - 1]->name, name, by_content)) return kids[i - 1].get();
        }
        for (size_t i = roots_.size(); i > 0; --i)
            if (same(roots_[i - 1]->name, name, by_content)) return roots_[i - 1].get();
        return nullptr;
    }
    node* kid(const node* parent, const char* name, bool by_content) const {
        if (!parent) return nullptr;
        for (const auto& k : parent->kids)
            if (same(k->name, name, by_content)) return k.get();
        return nullptr;
    }

    std::vector<node_ptr> stack_;    // open scopes, outermost first
    std::vector<node_ptr> roots_;    // top-level fields
    std::vector<node_ptr> free_;     // carried but not (yet) in the tree
    int pending_variant_tag_ = -1;
    const char* pending_count_field_ = nullptr;
    node::Derives root_derives_ = node::Derives::Nothing;
    const char* root_determines_ = nullptr;
    bool root_bits_ = false;
    bool root_bits_msb_first_ = false;
};

}  // namespace bbq::zcow
