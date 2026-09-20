// bbq_python.cpp — CPython C API extension module for the BBQ CEK VM
//
// Single-file module, no headers. All types are file-scope statics.
// Follows the patterns established in ptree-python.
//
// This is a WRAPPER over the ZCow document and nothing else. The CEK machine parses a
// buffer into a zcow::document — one tree, whose nodes either name a span of the input
// (zero-copy) or carry a value of their own — and every Python operation here is one
// call into that API. The copy-on-write is entirely ZCow's: a consumer parses a file and
// walks the tree, and `doc.foo.bar[4] = 42` works without knowing any of it exists.
//
// It is the same wrapper the generated C++ handles are, resolved at runtime rather than
// at codegen: a handle there holds (node, source, transient) and turns a field NAME into
// one ZCow call, which is exactly what tp_getattro does here — the grammar is a runtime
// thing on this side, so the name arrives as a string instead of being baked in.
//
// Following that model down to the detail that matters: a CONTAINER is owned when it is
// navigated to (the generated `sub_`), which makes its pointer stable and writable; a
// LEAF is re-resolved from its container on every access (the generated `r_`) and is
// never owned by a read. Owning a leaf on read would be a corruption, not an
// inefficiency — an owned leaf carries its own value, and a read has no value to put
// there.

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstring>
#include <cstdio>
#include <string>
#include <sstream>
#include <vector>

// POSIX (for parse_file)
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// BBQ frontend
#include "Parser.h"
#include "Sema.h"
#include "Errors.h"

// BBQ CEK backend
#include "bbq_compile.h"
#include "Machine.h"
#include "Capture.h"
#include "CaptureCow.h"     // bbq::zcow — the document, and the whole write side

// bbq.build — the grammar-free byte-construction submodule (its own translation unit)
#include "bbq_build.h"

using namespace bbq;        // index runtime: CaptureType, ComputedValue
using namespace bbq::cek;   // machine IR: Value, ...
namespace zc = bbq::zcow;


// ── Forward declarations ────────────────────────────────────────────────────

struct PyBBQSpec;
struct PyBBQResult;
struct PyBBQNode;
struct PyBBQNodeIter;

extern PyTypeObject PyBBQSpec_Type;
extern PyTypeObject PyBBQResult_Type;
extern PyTypeObject PyBBQNode_Type;
extern PyTypeObject PyBBQNodeIter_Type;

static PyObject* PyBBQParseError;

static PyBBQNode* PyBBQNode_New(PyBBQResult* result, zc::node* parent, Py_ssize_t slot,
                                PyBBQNode* up);

// The multi-valued half of the selector vocabulary — a tuple of selectors, an
// Ellipsis for the descendant segment, a slice, a filter. Defined beside the
// NodeList it answers with; declared here because Node's subscript reaches it.
static bool key_is_multi(PyObject* key);
static PyObject* node_select(PyBBQNode* node, PyObject* key);


// ── Locking ─────────────────────────────────────────────────────────────────
//
// The module declares Py_MOD_GIL_NOT_USED (see PyInit_bbq), so it must serialize its own
// shared state. Everything guarded here is plain C++ — a node tree, two parallel arrays —
// which the interpreter knows nothing about and cannot protect.
//
// PyMutex, not Py_BEGIN_CRITICAL_SECTION: a critical section is *suspended* when the
// holding thread blocks, which is right for locking a Python object and wrong for a C++
// container mid-resize. The rule that makes a plain mutex safe here is that a lock is only
// ever held over C++ — every PyObject conversion happens before it is taken, so nothing can
// re-enter the interpreter (and thus this module) while one is held.
//
// Below 3.13 there is no PyMutex and no free-threaded build, so the GIL already serializes
// all of this and the shim compiles to nothing.
#if PY_VERSION_HEX >= 0x030D0000
using BBQMutex = PyMutex;
#  define BBQ_MUTEX_INIT PyMutex()
static inline void bbq_mutex_lock(BBQMutex* m)   { PyMutex_Lock(m); }
static inline void bbq_mutex_unlock(BBQMutex* m) { PyMutex_Unlock(m); }
#else
struct BBQMutex { char unused; };
#  define BBQ_MUTEX_INIT BBQMutex{0}
static inline void bbq_mutex_lock(BBQMutex*)   {}
static inline void bbq_mutex_unlock(BBQMutex*) {}
#endif

struct BBQLock {
    BBQMutex* m;
    explicit BBQLock(BBQMutex* mu) : m(mu) { bbq_mutex_lock(m); }
    ~BBQLock() { bbq_mutex_unlock(m); }
    BBQLock(const BBQLock&) = delete;
    BBQLock& operator=(const BBQLock&) = delete;
};


// ── Structs ─────────────────────────────────────────────────────────────────

struct PyBBQSpec {
    PyObject_HEAD
    CompiledGrammar* grammar;
    // The frontend artifacts, kept alive because the compiled grammar points into what
    // they own — rule names, interned field names, the builtin table.
    Parser* parser;
    bbqgen::ErrorReporter* errors;
    bbqgen::Sema* sema;

    // Extern parser support
    PyObject** ext_callables;               // Array of strong refs (INCREFed)
    ExternalParserTable::Entry* ext_entries; // Parallel array
    int ext_count;
    int ext_capacity;
    ExternalParserTable ext_table;

    // Guards the extern registry. register_extern REALLOCS both arrays, so a parse
    // cannot read them directly — it takes a snapshot under this lock (do_parse).
    BBQMutex lock;
};

struct PyBBQResult {
    PyObject_HEAD
    ParseArena* arena;
    // The document, held as the one transient every node in it comes from. There is no
    // second structure and nothing overlays anything: this IS the parsed document, and
    // an edit is a write into it. It is never committed — a caller that goes on editing
    // after asking for bytes is the normal case here, and transient::serialize settles
    // the dependent fields exactly as commit does.
    zc::transient* edit;

    bool success;
    size_t bytes_consumed;
    const char* error_message;   // grammar-owned or static; not freed here
    size_t error_offset;

    Py_buffer view;
    bool view_valid;
    PyBBQSpec* spec;
    const char* rule;            // the rule this was parsed with

    // Guards `edit` — every read of the tree and every write into it. A transient is
    // single-owner by design (L'orange §4.2), and this is what makes one Python object
    // usable from more than one thread at a time without corrupting it.
    BBQMutex lock;
};

// A position in the document, addressed the way the generated handles address one: by
// its CONTAINER and its slot in it. Resolving through the container on each access is
// what `r_(f)` does in the generated C++ — it is why a write through one handle is
// visible through another, and why nothing here goes stale when copy-on-write replaces
// a node. `parent == nullptr` is the root.
struct PyBBQNode {
    PyObject_HEAD
    zc::node* parent;
    Py_ssize_t slot;
    PyBBQResult* result;
    // The node this one was reached THROUGH, or null at the root. `parent`
    // above is the ZCow node — enough to read this node's value, and nothing
    // more; a document is a tree of spans with no back edges, so without this a
    // node found by a descendant search cannot say which of forty identically
    // named siblings it is. That is what makes a query result actionable rather
    // than merely correct, and it is what _path, _root and _parent are built on.
    PyBBQNode* up;
};

struct PyBBQNodeIter {
    PyObject_HEAD
    PyBBQResult* result;
    zc::node* container;   // owned, so it stays put while the iteration runs
    PyBBQNode* owner;      // the container as a Node, so what it yields knows its parent
    Py_ssize_t index;
    bool yield_tuples;     // true for struct → (name, node), false for array → node
};


// Taking a document's lock also takes OWNERSHIP of its transient for this thread.
//
// ZCow checks the owning thread on every operation because that is what C++ can observe
// of single use (L'orange §7.2, §8.1.2) — the thread standing still is a proxy for
// "nobody else is inside this". A Python ParseResult is one object that any thread may
// reach, and this module gives it the real property with the mutex: every operation on
// the document happens under this lock, and a lock is only ever held over C++, so nothing
// can re-enter. So the proxy is replaced by the guarantee it was approximating, in the
// single place where the guarantee is actually established — a caller cannot touch the
// document without coming through here.
struct DocLock {
    BBQMutex* m;
    explicit DocLock(PyBBQResult* r);
    ~DocLock() { bbq_mutex_unlock(m); }
    DocLock(const DocLock&) = delete;
    DocLock& operator=(const DocLock&) = delete;
};


// ── Helpers ─────────────────────────────────────────────────────────────────

static const char* capture_type_name(CaptureType type) {
    switch (type) {
        case CaptureType::UInt8:     return "uint8";
        case CaptureType::UInt16LE:  return "uint16le";
        case CaptureType::UInt16BE:  return "uint16be";
        case CaptureType::UInt32LE:  return "uint32le";
        case CaptureType::UInt32BE:  return "uint32be";
        case CaptureType::UInt64LE:  return "uint64le";
        case CaptureType::UInt64BE:  return "uint64be";
        case CaptureType::Int8:      return "int8";
        case CaptureType::Int16LE:   return "int16le";
        case CaptureType::Int16BE:   return "int16be";
        case CaptureType::Int32LE:   return "int32le";
        case CaptureType::Int32BE:   return "int32be";
        case CaptureType::Int64LE:   return "int64le";
        case CaptureType::Int64BE:   return "int64be";
        case CaptureType::Float32LE: return "float32le";
        case CaptureType::Float32BE: return "float32be";
        case CaptureType::Float64LE: return "float64le";
        case CaptureType::Float64BE: return "float64be";
        case CaptureType::Bool:      return "bool";
        case CaptureType::Bytes:     return "bytes";
        case CaptureType::String:    return "string";
        case CaptureType::Struct:    return "struct";
        case CaptureType::Array:     return "array";
        case CaptureType::Computed:  return "computed";
        case CaptureType::External:  return "external";
    }
    return "unknown";
}

DocLock::DocLock(PyBBQResult* r) : m(&r->lock) {
    bbq_mutex_lock(m);
    r->edit->adopt();
}

static inline bool is_container(CaptureType t) { return zc::is_container_type(t); }

// ── Addressing ──
//
// Resolve a node from the (container, slot) pair it is addressed by. Caller holds the
// result's lock.
static const zc::node* node_of(PyBBQNode* self) {
    if (!self->parent) return self->result->edit->root();
    if (self->slot < 0 || (size_t)self->slot >= self->parent->kids.size()) return nullptr;
    return self->parent->kids[(size_t)self->slot].get();
}

// The same, owned — so it can be written through, or hold children whose addresses stay
// valid. This is the generated `sub_`: navigating INTO a container claims it. Caller
// holds the lock.
static zc::node* owned_node_of(PyBBQNode* self) {
    zc::transient* t = self->result->edit;
    if (!self->parent) return t->root_mut();
    return t->own_child(self->parent, (size_t)self->slot);
}

static Py_ssize_t child_slot(const zc::node* c, const char* name) {
    if (!c) return -1;
    for (size_t i = 0; i < c->kids.size(); i++)
        if (c->kids[i]->name && std::strcmp(c->kids[i]->name, name) == 0)
            return (Py_ssize_t)i;
    return -1;
}

// ── Reading a leaf ──
//
// One ZCow call per kind, dispatched on what the PARSER recorded on the node. Nothing
// here consults the grammar: the type, the bitfield layout and the computed kind are all
// on the node, which is why a runtime wrapper can be as thin as a generated one.
static PyObject* node_value(PyBBQResult* result, const zc::node* n) {
    if (!n) Py_RETURN_NONE;
    const zc::source& src = result->edit->src();

    switch (n->type) {
        // A 64-bit UNSIGNED leaf holds values the int64_t carrier cannot express.
        // read_int hands back the bit pattern, which is right for a caller that
        // casts it to the field's declared C++ type — the generated reader does
        // exactly that. Nothing here has a declared type to cast to, so the
        // signedness has to come from the capture type, or 2**64-1 reports as -1
        // through every conversion at once: they all come through this function.
        case CaptureType::UInt64LE: case CaptureType::UInt64BE:
            return PyLong_FromUnsignedLongLong(zc::read_uint(n, src));

        case CaptureType::UInt8:    case CaptureType::UInt16LE: case CaptureType::UInt16BE:
        case CaptureType::UInt32LE: case CaptureType::UInt32BE:
        case CaptureType::Int8:     case CaptureType::Int16LE:  case CaptureType::Int16BE:
        case CaptureType::Int32LE:  case CaptureType::Int32BE:
        case CaptureType::Int64LE:  case CaptureType::Int64BE:
            return PyLong_FromLongLong(zc::read_int(n, src));

        case CaptureType::Computed: {
            // A Computed leaf carries a typed value (compute(...)/leb/bitfield entry).
            // Project it to the matching Python type — not always int. An EDITED one
            // carries its value directly, which read_int is what knows.
            if (!n->parsed) return PyLong_FromLongLong(zc::read_int(n, src));
            auto* cv = n->computed_value;
            if (!cv) return PyLong_FromLongLong(0);
            switch (cv->kind) {
                case ComputedValue::Kind::Int:    return PyLong_FromLongLong(cv->i);
                case ComputedValue::Kind::Bool:   return PyBool_FromLong(cv->b ? 1 : 0);
                case ComputedValue::Kind::Float:  return PyFloat_FromDouble(cv->f);
                case ComputedValue::Kind::String: return PyUnicode_FromString(cv->s ? cv->s : "");
            }
            return PyLong_FromLongLong(0);
        }

        case CaptureType::Float32LE: case CaptureType::Float32BE:
        case CaptureType::Float64LE: case CaptureType::Float64BE:
            return PyFloat_FromDouble(zc::read_float(n, src));

        case CaptureType::Bool:
            return PyBool_FromLong(zc::read_int(n, src) ? 1 : 0);

        case CaptureType::String: {
            std::string_view s = zc::read_str(n, src);
            return PyUnicode_DecodeUTF8(s.data(), (Py_ssize_t)s.size(), NULL);
        }

        case CaptureType::Bytes:
        case CaptureType::External: {
            auto b = zc::read_bytes(n, src);
            return PyBytes_FromStringAndSize((const char*)b.first, (Py_ssize_t)b.second);
        }

        case CaptureType::Struct:
        case CaptureType::Array:
            break;   // containers are not values; the caller hands back a node
    }
    Py_RETURN_NONE;
}

// ── Writing a leaf ──
//
// `n` is already owned (the caller got it from own_child). Every PyObject→C conversion
// happens BEFORE the lock is taken: PyFloat_AsDouble, PyObject_IsTrue and
// PyLong_AsLongLong can all run arbitrary Python (__float__ / __bool__ / __index__), and
// re-entering this module while holding the lock would deadlock.
struct PendingWrite {
    enum class Kind { Int, Uint, Float, Bytes } kind;
    int64_t i = 0;
    uint64_t u = 0;
    double f = 0;
    std::vector<uint8_t> b;
    bool as_str = false;
};

// A leaf whose top half is out of an int64_t's reach. Its values have to be
// carried, converted and range-checked as unsigned or the upper half of the
// field is simply unreachable from Python.
static bool is_wide_unsigned(CaptureType t) {
    return t == CaptureType::UInt64LE || t == CaptureType::UInt64BE;
}

// `e` is the leaf's encoding, which decides the range as much as the type does:
// a varint's width follows its value. The range is checked HERE, before the
// caller owns the node it is about to write — own_child clones a parsed node
// into an edited one, and a refusal after that point has already changed the
// document it was refusing to change.
static bool convert_for(CaptureType t, zc::Enc e, PyObject* value, PendingWrite* out) {
    if (is_float_type(t)) {
        double d = PyFloat_AsDouble(value);
        if (d == -1.0 && PyErr_Occurred()) return false;
        out->kind = PendingWrite::Kind::Float; out->f = d;
        return true;
    }
    if (t == CaptureType::Bytes || t == CaptureType::External) {
        if (!PyBytes_Check(value)) {
            PyErr_SetString(PyExc_TypeError, "expected bytes for a bytes field");
            return false;
        }
        const char* p = PyBytes_AS_STRING(value);
        out->kind = PendingWrite::Kind::Bytes;
        out->b.assign(p, p + PyBytes_GET_SIZE(value));
        return true;
    }
    if (t == CaptureType::String) {
        out->kind = PendingWrite::Kind::Bytes;
        out->as_str = true;
        if (PyBytes_Check(value)) {
            const char* p = PyBytes_AS_STRING(value);
            out->b.assign(p, p + PyBytes_GET_SIZE(value));
        } else {
            Py_ssize_t n = 0;
            const char* s = PyUnicode_AsUTF8AndSize(value, &n);
            if (!s) return false;
            out->b.assign(s, s + n);
        }
        return true;
    }
    if (t == CaptureType::Bool) {
        int b = PyObject_IsTrue(value);
        if (b < 0) return false;
        out->kind = PendingWrite::Kind::Int; out->i = b ? 1 : 0;
        return true;
    }
    if (is_container(t)) {
        PyErr_Format(PyExc_TypeError, "cannot assign directly to a %s field",
                     capture_type_name(t));
        return false;
    }
    // Integer leaves, and Computed (leb/compute). A leaf whose range an int64_t
    // cannot carry is converted as unsigned, so its top half is reachable at
    // all; a NEGATIVE handed to one falls through to the signed path below,
    // where the range check names what it actually failed.
    if (is_wide_unsigned(t) || e == zc::Enc::Uleb) {
        unsigned long long uv = PyLong_AsUnsignedLongLong(value);
        if (uv != (unsigned long long)-1 || !PyErr_Occurred()) {
            if (!zc::leaf_holds_unsigned(t, e, (uint64_t)uv)) {
                PyErr_Format(PyExc_OverflowError, "%llu does not fit a %s field (%s)",
                             uv, zc::leaf_kind_text(t, e).c_str(),
                             zc::leaf_range_text(t, e).c_str());
                return false;
            }
            out->kind = PendingWrite::Kind::Uint; out->u = (uint64_t)uv;
            return true;
        }
        if (!PyErr_ExceptionMatches(PyExc_OverflowError)) return false;
        PyErr_Clear();   /* negative, or wider than 64 bits — say which, below */
    }
    int64_t v = PyLong_AsLongLong(value);
    if (v == -1 && PyErr_Occurred()) return false;
    if (!zc::leaf_holds_signed(t, e, v)) {
        PyErr_Format(PyExc_OverflowError, "%lld does not fit a %s field (%s)",
                     (long long)v, zc::leaf_kind_text(t, e).c_str(),
                     zc::leaf_range_text(t, e).c_str());
        return false;
    }
    out->kind = PendingWrite::Kind::Int; out->i = v;
    return true;
}

// Content supplied as a SUBTREE rather than a field value: a bbq.build value, or raw
// bytes. This is the form a composite element takes — the grammar is not describing the
// shape, the caller is. A build value is already a ZCow node, so it grafts in as
// structure and stays navigable; bytes become a node that holds them, which is as much
// structure as bytes carry. Returns the node, or null with 0 in `*is_kind` when the value
// is simply not one of these. Runs Python, so no lock may be held.
static zc::node_ptr as_subtree(PyObject* value, int* is_kind) {
    *is_kind = 1;
    if (bbq_build_is_value(value) || PyBytes_Check(value) || PyByteArray_Check(value))
        return bbq_build_node(value);
    if (PyUnicode_Check(value)) {
        Py_ssize_t n = 0;
        const char* s = PyUnicode_AsUTF8AndSize(value, &n);
        if (!s) return nullptr;
        auto node = std::make_shared<zc::node>();
        node->type = CaptureType::String;
        zc::set_str(node.get(), std::string_view(s, (size_t)n));
        return node;
    }
    *is_kind = 0;
    return nullptr;
}

// The ZCow setters refuse a value the leaf cannot hold by throwing. The frame
// above this one is CPython, not C++, so the exception has to become a Python
// error here — letting it cross that boundary terminates the process, which is
// a worse answer to a bad value than the silent truncation this replaced.
static bool apply_write(zc::node* n, const PendingWrite& w) {
    try {
        switch (w.kind) {
            case PendingWrite::Kind::Int:   zc::set_int(n, w.i, n->enc); break;
            case PendingWrite::Kind::Uint:  zc::set_uint(n, w.u, n->enc); break;
            case PendingWrite::Kind::Float: zc::set_float(n, w.f); break;
            case PendingWrite::Kind::Bytes:
                if (w.as_str) zc::set_str(n, std::string_view((const char*)w.b.data(), w.b.size()));
                else          zc::set_bytes(n, w.b.data(), w.b.size());
                break;
        }
    } catch (const zc::range_error& e) {
        PyErr_SetString(PyExc_OverflowError, e.what());
        return false;
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_TypeError, e.what());
        return false;
    }
    return true;
}

// Write `value` into the child of `container` named `key` (or at `index`). This is the
// generated `set_x`: own the child, then one ZCow setter.
static int write_child(PyBBQResult* result, PyBBQNode* holder,
                       const char* key, Py_ssize_t index, PyObject* value) {
    CaptureType t;
    zc::Enc enc;
    Py_ssize_t slot;
    {
        DocLock g(result);
        const zc::node* c = node_of(holder);
        if (!c || !is_container(c->type)) {
            PyErr_SetString(PyExc_TypeError, "not a container");
            return -1;
        }
        slot = key ? child_slot(c, key) : index;
        if (slot < 0 || (size_t)slot >= c->kids.size()) {
            if (key) PyErr_Format(PyExc_AttributeError, "no field '%s'", key);
            else     PyErr_SetString(PyExc_IndexError, "index out of range");
            return -1;
        }
        t   = c->kids[(size_t)slot]->type;
        enc = c->kids[(size_t)slot]->enc;
    }

    PendingWrite w;
    if (!convert_for(t, enc, value, &w)) return -1;   // may run Python — no lock held

    DocLock g(result);
    zc::node* c = owned_node_of(holder);
    zc::node* child = result->edit->own_child(c, (size_t)slot);
    if (!child) { PyErr_SetString(PyExc_RuntimeError, "field vanished"); return -1; }
    return apply_write(child, w) ? 0 : -1;
}


// ── PyBBQNode ───────────────────────────────────────────────────────────────

static PyBBQNode* PyBBQNode_New(PyBBQResult* result, zc::node* parent, Py_ssize_t slot,
                                PyBBQNode* up) {
    PyBBQNode* node = PyObject_GC_New(PyBBQNode, &PyBBQNode_Type);
    if (!node) return NULL;
    node->parent = parent;
    node->slot = slot;
    Py_INCREF(result);
    node->result = result;
    node->up = up;
    Py_XINCREF(up);          // a node keeps the chain it was reached through alive
    PyObject_GC_Track((PyObject*)node);
    return node;
}

static void PyBBQNode_dealloc(PyBBQNode* self) {
    PyObject_GC_UnTrack((PyObject*)self);
    Py_XDECREF(self->result);
    Py_XDECREF(self->up);
    PyObject_GC_Del(self);
}

static int PyBBQNode_traverse(PyBBQNode* self, visitproc visit, void* arg) {
    Py_VISIT(self->result);
    Py_VISIT(self->up);
    return 0;
}

static int PyBBQNode_clear(PyBBQNode* self) {
    Py_CLEAR(self->result);
    return 0;
}

// Navigate to a child of `holder`. Navigating INTO a container claims it — the generated
// `sub_` — because it becomes the address of everything below it, and an owned node is
// one copy-on-write will not move again. The CHILD is not claimed: a leaf that has been
// owned carries its own value, and a read has no value to put there, so owning one on a
// read would not be an inefficiency but a corruption.
static PyObject* child_node(PyBBQResult* result, PyBBQNode* holder, Py_ssize_t slot) {
    DocLock g(result);
    const zc::node* c = node_of(holder);
    if (!c || slot < 0 || (size_t)slot >= c->kids.size()) {
        PyErr_SetString(PyExc_IndexError, "index out of range");
        return NULL;
    }
    zc::node* owned = owned_node_of(holder);
    if (!owned) { PyErr_SetString(PyExc_RuntimeError, "node vanished"); return NULL; }
    return (PyObject*)PyBBQNode_New(result, owned, slot, holder);
}

// THE FIELD WINS.
//
// A format names its own fields, and real ones are called `name`, `value`,
// `offset`, `length`, `parent`, `type`. A library that claims those names makes
// those formats unreachable by the spelling their specification uses — you would
// have to rename the field in the grammar to read it, which is the format
// describing the library instead of the other way round.
//
// So: `node.x` is the field `x` when there is one. Every library member also
// answers to `node._x`, which a field cannot take away because the underscored
// forms are the canonical ones. And `node["x"]` is always the field and never a
// library member, which is the escape hatch that works even for a field named
// `_value`.
//
// (Before this, `.name`, `.value`, `.offset` and `.raw` were silently
// unreachable as fields — a format with a `name` field read the node's own name
// instead and said nothing about it.)
static PyObject* PyBBQNode_getattro(PyBBQNode* self, PyObject* name) {
    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return NULL;

    if (key[0] != '_') {                       // dunders and the canon skip this
        Py_ssize_t slot;
        { DocLock g(self->result); slot = child_slot(node_of(self), key); }
        if (slot >= 0) return child_node(self->result, self, slot);
    }
    return PyObject_GenericGetAttr((PyObject*)self, name);
}

// `node.field = v` — own the field and write it.
static int PyBBQNode_setattro(PyBBQNode* self, PyObject* name, PyObject* value) {
    if (!value) { PyErr_SetString(PyExc_TypeError, "cannot delete a BBQ field"); return -1; }
    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return -1;
    return write_child(self->result, self, key, -1, value);
}

// ── Number protocol ──

// int()/float() coerce a NUMERIC leaf. Anything else is a TypeError naming the type it
// was asked to convert — not whatever int() would say about the value it got handed.
static bool is_numeric_leaf(CaptureType t) {
    return !is_container(t) && t != CaptureType::String &&
           t != CaptureType::Bytes && t != CaptureType::External;
}

static PyObject* coerce(PyBBQNode* self, const char* to) {
    CaptureType t;
    PyObject* v;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n) { PyErr_SetString(PyExc_RuntimeError, "node vanished"); return NULL; }
        t = n->type;
        if (!is_numeric_leaf(t)) {
            PyErr_Format(PyExc_TypeError, "cannot convert %s to %s",
                         capture_type_name(t), to);
            return NULL;
        }
        v = node_value(self->result, n);
    }
    if (!v) return NULL;
    if (PyUnicode_Check(v)) {   // a Computed carrying a string is not numeric either
        Py_DECREF(v);
        PyErr_Format(PyExc_TypeError, "cannot convert %s to %s", capture_type_name(t), to);
        return NULL;
    }
    PyObject* r = (to[0] == 'i') ? PyNumber_Long(v) : PyNumber_Float(v);
    Py_DECREF(v);
    return r;
}

static PyObject* PyBBQNode_nb_int(PyBBQNode* self)   { return coerce(self, "int"); }
static PyObject* PyBBQNode_nb_float(PyBBQNode* self) { return coerce(self, "float"); }

static int PyBBQNode_nb_bool(PyBBQNode* self) {
    CaptureType t;
    PyObject* v;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n) return 0;
        t = n->type;
        if (is_container(t)) return n->kids.empty() ? 0 : 1;
        v = node_value(self->result, n);
    }
    if (!v) return -1;
    int r = PyObject_IsTrue(v);
    Py_DECREF(v);
    return r;
}

static PyNumberMethods PyBBQNode_as_number = {
    (binaryfunc)  NULL,                     // nb_add
    (binaryfunc)  NULL,                     // nb_subtract
    (binaryfunc)  NULL,                     // nb_multiply
    (binaryfunc)  NULL,                     // nb_remainder
    (binaryfunc)  NULL,                     // nb_divmod
    (ternaryfunc) NULL,                     // nb_power
    (unaryfunc)   NULL,                     // nb_negative
    (unaryfunc)   NULL,                     // nb_positive
    (unaryfunc)   NULL,                     // nb_absolute
    (inquiry)     PyBBQNode_nb_bool,        // nb_bool
    (unaryfunc)   NULL,                     // nb_invert
    (binaryfunc)  NULL,                     // nb_lshift
    (binaryfunc)  NULL,                     // nb_rshift
    (binaryfunc)  NULL,                     // nb_and
    (binaryfunc)  NULL,                     // nb_xor
    (binaryfunc)  NULL,                     // nb_or
    (unaryfunc)   PyBBQNode_nb_int,         // nb_int
    NULL,                                   // nb_reserved
    (unaryfunc)   PyBBQNode_nb_float,       // nb_float
};

// ── Mapping protocol ──

static Py_ssize_t PyBBQNode_mp_length(PyBBQNode* self) {
    DocLock g(self->result);
    const zc::node* n = node_of(self);
    if (n && is_container(n->type)) return (Py_ssize_t)zc::size_of(n);
    PyErr_Format(PyExc_TypeError,
                 "object of type 'bbq.Node' (%s) has no len()",
                 capture_type_name(n ? n->type : CaptureType::UInt8));
    return -1;
}

static PyObject* PyBBQNode_mp_subscript(PyBBQNode* self, PyObject* key) {
    CaptureType ct;
    Py_ssize_t count;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n) { PyErr_SetString(PyExc_RuntimeError, "node vanished"); return NULL; }
        ct = n->type;
        count = (Py_ssize_t)zc::size_of(n);
    }
    // A selector that can match more than one thing answers with a NodeList, so
    // segments compose — and it answers with an EMPTY one over a leaf rather
    // than raising, because "selects nothing" is what a selector does when there
    // is nothing to select (§2.3.1.2). Asking a leaf for a named field or an
    // index is a different act: that is a caller who expects to get one, and it
    // still raises.
    if (key_is_multi(key)) return node_select(self, key);

    if (!is_container(ct)) {
        PyErr_Format(PyExc_TypeError, "'bbq.Node' (%s) is not subscriptable",
                     capture_type_name(ct));
        return NULL;
    }

    if (PyIndex_Check(key)) {
        Py_ssize_t index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (index == -1 && PyErr_Occurred()) return NULL;
        if (index < 0) index += count;
        if (index < 0 || index >= count) {
            PyErr_SetString(PyExc_IndexError, "index out of range");
            return NULL;
        }
        return child_node(self->result, self, index);
    }

    if (PyUnicode_Check(key)) {
        const char* name = PyUnicode_AsUTF8(key);
        if (!name) return NULL;
        Py_ssize_t slot;
        { DocLock g(self->result); slot = child_slot(node_of(self), name); }
        if (slot < 0) { PyErr_SetObject(PyExc_KeyError, key); return NULL; }
        return child_node(self->result, self, slot);
    }

    PyErr_Format(PyExc_TypeError,
                 "indices must be integers or strings, not %.200s",
                 Py_TYPE(key)->tp_name);
    return NULL;
}

// node[i] = v / node[name] = v, or del node[i].
static int PyBBQNode_mp_ass_subscript(PyBBQNode* self, PyObject* key, PyObject* value) {
    CaptureType ct;
    Py_ssize_t count;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n) { PyErr_SetString(PyExc_RuntimeError, "node vanished"); return -1; }
        ct = n->type;
        count = (Py_ssize_t)zc::size_of(n);
    }
    if (!is_container(ct)) {
        PyErr_Format(PyExc_TypeError, "'bbq.Node' (%s) does not support item assignment",
                     capture_type_name(ct));
        return -1;
    }

    Py_ssize_t index = -1;
    const char* name = nullptr;
    if (PyIndex_Check(key)) {
        index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (index == -1 && PyErr_Occurred()) return -1;
        if (index < 0) index += count;
        if (index < 0 || index >= count) {
            PyErr_SetString(PyExc_IndexError, "index out of range"); return -1;
        }
    } else if (PyUnicode_Check(key)) {
        name = PyUnicode_AsUTF8(key);
        if (!name) return -1;
        DocLock g(self->result);
        index = child_slot(node_of(self), name);
        if (index < 0) { PyErr_SetObject(PyExc_KeyError, key); return -1; }
        name = nullptr;   // resolved to a slot; write by index from here
    } else {
        PyErr_Format(PyExc_TypeError, "indices must be integers or strings, not %.200s",
                     Py_TYPE(key)->tp_name);
        return -1;
    }

    if (value == NULL) {   // del node[index]
        DocLock g(self->result);
        zc::node* c = owned_node_of(self);
        if (!c || !self->result->edit->remove(c, (size_t)index)) {
            PyErr_SetString(PyExc_IndexError, "cannot delete");
            return -1;
        }
        return 0;
    }

    // Replacing a COMPOSITE element: the shape being put there is not one the grammar is
    // describing, so it goes in as bytes and the element becomes those bytes (the splice).
    bool composite;
    {
        DocLock g(self->result);
        const zc::node* c = node_of(self);
        composite = c && (size_t)index < c->kids.size() &&
                    is_container(c->kids[(size_t)index]->type);
    }
    if (composite) {
        int kind = 0;
        zc::node_ptr sub = as_subtree(value, &kind);   // may run Python — no lock held
        if (kind && !sub) return -1;
        if (!kind) {
            PyErr_SetString(PyExc_TypeError,
                "a composite element is replaced by a bbq.build value or bytes");
            return -1;
        }
        DocLock g(self->result);
        zc::node* c = owned_node_of(self);
        if (!c || !self->result->edit->replace(c, (size_t)index, std::move(sub))) {
            PyErr_SetString(PyExc_RuntimeError, "replace failed");
            return -1;
        }
        return 0;
    }
    return write_child(self->result, self, name, index, value);
}

static PyMappingMethods PyBBQNode_as_mapping = {
    (lenfunc)      PyBBQNode_mp_length,
    (binaryfunc)   PyBBQNode_mp_subscript,
    (objobjargproc)PyBBQNode_mp_ass_subscript,
};

// ── Sequence protocol ──

static int PyBBQNode_sq_contains(PyBBQNode* self, PyObject* value) {
    if (!PyUnicode_Check(value)) return 0;
    const char* key = PyUnicode_AsUTF8(value);
    if (!key) { PyErr_Clear(); return 0; }
    DocLock g(self->result);
    return child_slot(node_of(self), key) >= 0 ? 1 : 0;
}

static PySequenceMethods PyBBQNode_as_sequence = {
    (lenfunc)        NULL,                  // sq_length
    (binaryfunc)     NULL,                  // sq_concat
    (ssizeargfunc)   NULL,                  // sq_repeat
    (ssizeargfunc)   NULL,                  // sq_item
    NULL,                                   // was sq_slice
    (ssizeobjargproc)NULL,                  // sq_ass_item
    NULL,                                   // was sq_ass_slice
    (objobjproc)     PyBBQNode_sq_contains, // sq_contains
    (binaryfunc)     NULL,                  // sq_inplace_concat
    (ssizeargfunc)   NULL,                  // sq_inplace_repeat
};

// ── Buffer protocol ──
//
// The zero-copy read, when there is one to give: a node still described by its span
// points straight into the mapped input. Once something has been written to it there is
// no span to point at, and `raw` (which serializes) is the way to get its bytes.
static int PyBBQNode_getbuffer(PyBBQNode* self, Py_buffer* view, int flags) {
    DocLock g(self->result);
    const zc::node* n = node_of(self);
    const zc::source& src = self->result->edit->src();
    if (!n || !n->parsed || !src.buf) {
        PyErr_SetString(PyExc_BufferError,
                        "node has been edited: it no longer names bytes of the input "
                        "(use .raw)");
        return -1;
    }
    return PyBuffer_FillInfo(view, (PyObject*)self,
                             (void*)(src.buf + n->start_offset),
                             (Py_ssize_t)(n->end_offset - n->start_offset),
                             1 /* readonly */, flags);
}

static PyBufferProcs PyBBQNode_as_buffer = {
    (getbufferproc) PyBBQNode_getbuffer,
    (releasebufferproc) NULL,
};

// ── str / repr ──

static PyObject* PyBBQNode_tp_repr(PyBBQNode* self) {
    DocLock g(self->result);
    const zc::node* n = node_of(self);
    if (!n) return PyUnicode_FromString("<bbq.Node (gone)>");
    if (n->name)
        return PyUnicode_FromFormat("<bbq.Node '%s' type=%s [0x%zx:0x%zx]%s>",
            n->name, capture_type_name(n->type),
            n->start_offset, n->end_offset, n->parsed ? "" : " edited");
    return PyUnicode_FromFormat("<bbq.Node type=%s [0x%zx:0x%zx]%s>",
        capture_type_name(n->type), n->start_offset, n->end_offset,
        n->parsed ? "" : " edited");
}

static PyObject* PyBBQNode_tp_str(PyBBQNode* self) {
    CaptureType t;
    PyObject* val;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n) return PyUnicode_FromString("");
        t = n->type;
        if (is_container(t)) { /* fall through to repr below */ }
        val = is_container(t) ? nullptr : node_value(self->result, n);
    }
    if (is_container(t)) return PyBBQNode_tp_repr(self);
    if (!val) return NULL;
    if (PyUnicode_Check(val)) return val;
    PyObject* str = PyObject_Str(val);
    Py_DECREF(val);
    return str;
}

// ── Rich comparison ──

static PyObject* PyBBQNode_richcompare(PyObject* self_obj, PyObject* other, int op) {
    PyBBQNode* self = (PyBBQNode*)self_obj;
    CaptureType t;
    PyObject* val;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n) Py_RETURN_NOTIMPLEMENTED;
        t = n->type;
        // Containers: identity comparison with other nodes.
        if (is_container(t)) {
            if (Py_TYPE(other) == &PyBBQNode_Type) {
                PyBBQNode* o = (PyBBQNode*)other;
                bool eq = (o->result == self->result && node_of(o) == n);
                switch (op) {
                    case Py_EQ: return PyBool_FromLong(eq);
                    case Py_NE: return PyBool_FromLong(!eq);
                    default: Py_RETURN_NOTIMPLEMENTED;
                }
            }
            Py_RETURN_NOTIMPLEMENTED;
        }
        val = node_value(self->result, n);
    }
    if (!val) return NULL;
    PyObject* result = PyObject_RichCompare(val, other, op);
    Py_DECREF(val);
    return result;
}

// ── Iterator ──

static PyObject* make_iter(PyBBQResult* result, PyBBQNode* holder) {
    zc::node* c;
    bool tuples;
    {
        DocLock g(result);
        const zc::node* n = node_of(holder);
        if (!n || !is_container(n->type)) {
            PyErr_Format(PyExc_TypeError, "'bbq.Node' (%s) is not iterable",
                         capture_type_name(n ? n->type : CaptureType::UInt8));
            return NULL;
        }
        tuples = (n->type == CaptureType::Struct);
        c = owned_node_of(holder);   // pinned for the walk
    }
    PyBBQNodeIter* iter = PyObject_GC_New(PyBBQNodeIter, &PyBBQNodeIter_Type);
    if (!iter) return NULL;
    Py_INCREF(result);
    iter->result = result;
    iter->container = c;
    Py_INCREF(holder);
    iter->owner = holder;
    iter->index = 0;
    iter->yield_tuples = tuples;
    PyObject_GC_Track((PyObject*)iter);
    return (PyObject*)iter;
}

static PyObject* PyBBQNode_tp_iter(PyBBQNode* self) {
    return make_iter(self->result, self);
}

// ── Node methods ──

static PyObject* PyBBQNode_format(PyBBQNode* self, PyObject* args) {
    PyObject* format_spec;
    if (!PyArg_ParseTuple(args, "U", &format_spec)) return NULL;
    PyObject* val;
    { DocLock g(self->result); val = node_value(self->result, node_of(self)); }
    if (!val) return NULL;
    PyObject* result = PyObject_Format(val, format_spec);
    Py_DECREF(val);
    return result;
}

// Helper: append names from getset and methods tables to a list
static int append_type_attrs(PyObject* list, PyTypeObject* type) {
    if (type->tp_getset) {
        for (PyGetSetDef* gs = type->tp_getset; gs->name; gs++) {
            PyObject* s = PyUnicode_FromString(gs->name);
            if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); return -1; }
            Py_DECREF(s);
        }
    }
    if (type->tp_methods) {
        for (PyMethodDef* m = type->tp_methods; m->ml_name; m++) {
            if (m->ml_name[0] == '_' && m->ml_name[1] == '_') continue;
            PyObject* s = PyUnicode_FromString(m->ml_name);
            if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); return -1; }
            Py_DECREF(s);
        }
    }
    return 0;
}

// The child field names of `c`, appended to `list`. Caller holds the lock.
static int append_child_names(PyObject* list, const zc::node* c) {
    if (!c) return 0;
    for (const auto& k : c->kids) {
        // An unnamed child — an array element — is not an attribute name.
        if (!k->name || !*k->name) continue;
        PyObject* s = PyUnicode_FromString(k->name);
        if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); return -1; }
        Py_DECREF(s);
    }
    return 0;
}

// The children a mapping view exposes. A struct's are its named fields; an ARRAY's
// are positional, so keying them by name drops every one of them — which is what
// `keys()`/`values()`/`items()` used to do, silently, leaving any generic walk of a
// document to skip whatever an array held. Worse, it depended on the element type:
// a primitive element has no name and vanished, a struct element has an empty one
// and came back under the key "". An array keys by INDEX, which is what `node[0]`
// already accepts. Slots are collected under the lock and turned into Python
// objects after it — child_node takes the lock itself.
static bool child_slots(PyBBQResult* result, PyBBQNode* holder, bool* is_array,
                        std::vector<std::pair<Py_ssize_t, std::string>>* out) {
    DocLock g(result);
    const zc::node* c = node_of(holder);
    if (!c) return false;
    *is_array = (c->type == CaptureType::Array);
    for (size_t i = 0; i < c->kids.size(); i++) {
        if (*is_array)                          out->emplace_back((Py_ssize_t)i, std::string());
        else if (c->kids[i]->name && *c->kids[i]->name)
            out->emplace_back((Py_ssize_t)i, c->kids[i]->name);
    }
    return true;
}

// The key a slot is reached by: its index in an array, its name in a struct.
static PyObject* slot_key(bool is_array,
                          const std::pair<Py_ssize_t, std::string>& s) {
    return is_array ? PyLong_FromSsize_t(s.first)
                    : PyUnicode_FromString(s.second.c_str());
}

static PyObject* PyBBQNode_dir(PyBBQNode* self, PyObject*) {
    PyObject* list = PyList_New(0);
    if (!list) return NULL;
    if (append_type_attrs(list, &PyBBQNode_Type) < 0) { Py_DECREF(list); return NULL; }
    DocLock g(self->result);
    if (append_child_names(list, node_of(self)) < 0) { Py_DECREF(list); return NULL; }
    return list;
}

static PyObject* PyBBQNode_keys(PyBBQNode* self, PyObject*) {
    bool is_array = false;
    std::vector<std::pair<Py_ssize_t, std::string>> slots;
    if (!child_slots(self->result, self, &is_array, &slots)) return PyList_New(0);
    PyObject* list = PyList_New(0);
    if (!list) return NULL;
    for (const auto& s : slots) {
        PyObject* k = slot_key(is_array, s);
        if (!k || PyList_Append(list, k) < 0) { Py_XDECREF(k); Py_DECREF(list); return NULL; }
        Py_DECREF(k);
    }
    return list;
}

static PyObject* PyBBQNode_values(PyBBQNode* self, PyObject*) {
    bool is_array = false;
    std::vector<std::pair<Py_ssize_t, std::string>> slots;
    if (!child_slots(self->result, self, &is_array, &slots)) return PyList_New(0);
    PyObject* list = PyList_New(0);
    if (!list) return NULL;
    for (const auto& s : slots) {
        PyObject* node = child_node(self->result, self, s.first);
        if (!node || PyList_Append(list, node) < 0) {
            Py_XDECREF(node); Py_DECREF(list); return NULL;
        }
        Py_DECREF(node);
    }
    return list;
}

static PyObject* PyBBQNode_items(PyBBQNode* self, PyObject*) {
    bool is_array = false;
    std::vector<std::pair<Py_ssize_t, std::string>> slots;
    if (!child_slots(self->result, self, &is_array, &slots)) return PyList_New(0);
    PyObject* list = PyList_New(0);
    if (!list) return NULL;
    for (const auto& s : slots) {
        PyObject* name = slot_key(is_array, s);
        if (!name) { Py_DECREF(list); return NULL; }
        PyObject* node = child_node(self->result, self, s.first);
        if (!node) { Py_DECREF(name); Py_DECREF(list); return NULL; }
        PyObject* tuple = PyTuple_Pack(2, name, node);
        Py_DECREF(name); Py_DECREF(node);
        if (!tuple) { Py_DECREF(list); return NULL; }
        if (PyList_Append(list, tuple) < 0) { Py_DECREF(tuple); Py_DECREF(list); return NULL; }
        Py_DECREF(tuple);
    }
    return list;
}

// node.append(value): add an element to an array. The element takes the array's existing
// element type (a sibling — pure data, no grammar lookup); bytes and str append as a
// bytes element, which is the form a composite or variable-width element takes. The
// array's length updates immediately; the format's count field is a DEPENDENT field and
// ZCow derives it when the document is serialized.
static PyObject* PyBBQNode_append(PyBBQNode* self, PyObject* value) {
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n || n->type != CaptureType::Array) {
            PyErr_SetString(PyExc_TypeError, "append() requires an array node");
            return NULL;
        }
    }

    int kind = 0;
    zc::node_ptr sub = as_subtree(value, &kind);   // may run Python — no lock held
    if (kind && !sub) return NULL;
    if (kind) {
        sub->name = nullptr;                      // an array element is anonymous
        DocLock g(self->result);
        zc::node* c = owned_node_of(self);
        if (!c || !self->result->edit->append(c, std::move(sub))) {
            PyErr_SetString(PyExc_RuntimeError, "append failed");
            return NULL;
        }
        Py_RETURN_NONE;
    }

    CaptureType elem;
    zc::Enc elem_enc = zc::Enc::Fixed;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n || n->kids.empty()) {
            PyErr_SetString(PyExc_TypeError,
                "append: an empty array has no element type to take — append bytes "
                "(or a bbq.build value), or give the array an element first");
            return NULL;
        }
        elem     = n->kids[0]->type;
        elem_enc = n->kids[0]->enc;
    }

    PendingWrite w;
    if (!convert_for(elem, elem_enc, value, &w)) return NULL;   // may run Python — no lock held

    DocLock g(self->result);
    zc::node* c = owned_node_of(self);
    zc::node* e = self->result->edit->append(c, elem);
    if (!e) { PyErr_SetString(PyExc_RuntimeError, "append failed"); return NULL; }
    if (!apply_write(e, w)) return NULL;
    Py_RETURN_NONE;
}

static PyObject* PyBBQNode_dump(PyBBQNode* self, PyObject* args, PyObject* kwargs);

static PyMethodDef PyBBQNode_methods[] = {
    {"__format__", (PyCFunction)PyBBQNode_format, METH_VARARGS,
     "Format node value with format spec."},
    {"append",     (PyCFunction)PyBBQNode_append, METH_O,
     "append(value): add an element to an array node (typed from its siblings)."},
    {"__dir__",    (PyCFunction)PyBBQNode_dir,    METH_NOARGS,
     "List attributes including child field names."},
    {"keys",       (PyCFunction)PyBBQNode_keys,   METH_NOARGS,
     "Child field names (like dict.keys)."},
    {"values",     (PyCFunction)PyBBQNode_values, METH_NOARGS,
     "Child nodes (like dict.values)."},
    {"items",      (PyCFunction)PyBBQNode_items,  METH_NOARGS,
     "Child (name, node) pairs (like dict.items)."},
    {"dump",       (PyCFunction)(void(*)(void))PyBBQNode_dump, METH_VARARGS | METH_KEYWORDS,
     "dump(depth=None) -> str: this node and what is under it, as a tree."},
    // The underscored twins — see the note on PyBBQNode_getset.
    {"_keys",      (PyCFunction)PyBBQNode_keys,   METH_NOARGS,  "Child field names."},
    {"_values",    (PyCFunction)PyBBQNode_values, METH_NOARGS,  "Child nodes."},
    {"_items",     (PyCFunction)PyBBQNode_items,  METH_NOARGS,  "Child (name, node) pairs."},
    {"_append",    (PyCFunction)PyBBQNode_append, METH_O,       "Add an element to an array node."},
    {"_dump",      (PyCFunction)(void(*)(void))PyBBQNode_dump, METH_VARARGS | METH_KEYWORDS,
     "dump(depth=None) -> str: this node and what is under it, as a tree."},
    {NULL, NULL, 0, NULL}
};

// ── Properties ──

static PyObject* PyBBQNode_get_offset(PyBBQNode* self, void*) {
    DocLock g(self->result);
    const zc::node* n = node_of(self);
    if (!n) Py_RETURN_NONE;
    return Py_BuildValue("(nn)", (Py_ssize_t)n->start_offset, (Py_ssize_t)n->end_offset);
}

// This node's bytes as they stand — the span it was parsed from, or what it would
// serialize to once something has been written to it.
static PyObject* PyBBQNode_get_raw(PyBBQNode* self, void*) {
    std::vector<uint8_t> out;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n) Py_RETURN_NONE;
        zc::detail::emit_node(*n, self->result->edit->src(), std::string(), out);
    }
    return PyBytes_FromStringAndSize((const char*)out.data(), (Py_ssize_t)out.size());
}

static PyObject* PyBBQNode_get_capture_type(PyBBQNode* self, void*) {
    DocLock g(self->result);
    const zc::node* n = node_of(self);
    return PyUnicode_FromString(capture_type_name(n ? n->type : CaptureType::UInt8));
}

static PyObject* PyBBQNode_get_name(PyBBQNode* self, void*) {
    DocLock g(self->result);
    const zc::node* n = node_of(self);
    if (n && n->name) return PyUnicode_FromString(n->name);
    Py_RETURN_NONE;
}

// A leaf's value; a container is its own value — it stays a node, which is what makes
// `.value` uniform to walk without materializing a subtree nobody asked for.
static PyObject* PyBBQNode_get_value(PyBBQNode* self, void*) {
    PyObject* v;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (n && is_container(n->type)) return Py_NewRef((PyObject*)self);
        v = node_value(self->result, n);
    }
    return v;
}

// The arm/case ordinal the parser recorded for a union/alternatives/switch node
// (0 = first arm, 1 = second, …), or None when this node is not a variant. Pure data off
// the parsed node — like offset/capture_type, not a grammar query.
static PyObject* PyBBQNode_get_variant_tag(PyBBQNode* self, void*) {
    DocLock g(self->result);
    const zc::node* n = node_of(self);
    if (!n || n->variant_tag < 0) Py_RETURN_NONE;
    return PyLong_FromLong(n->variant_tag);
}

// Whether this node still names bytes of the input — false once something has been
// written to it. The zero-copy half of ZCow, made visible rather than guessed at.
static PyObject* PyBBQNode_get_parsed(PyBBQNode* self, void*) {
    DocLock g(self->result);
    const zc::node* n = node_of(self);
    return PyBool_FromLong(n && n->parsed);
}

// ── Where a node is ──────────────────────────────────────────────────────────
//
// A descendant search over a 500-chunk file hands back forty nodes all called
// `kind`. Without these, telling them apart means having tracked the walk
// yourself, which is the work the search was supposed to do. RFC 9535 defines
// normalized paths (§2.7) for the same reason: a query result that cannot say
// where it came from is not actionable.

static PyObject* PyBBQNode_get_parent(PyBBQNode* self, void*) {
    if (!self->up) Py_RETURN_NONE;
    return Py_NewRef((PyObject*)self->up);
}

static PyObject* PyBBQNode_get_root(PyBBQNode* self, void*) {
    PyBBQNode* n = self;
    while (n->up) n = n->up;
    return Py_NewRef((PyObject*)n);
}

// The steps from the root to here: field names as str, array positions as int.
// Unambiguous and directly usable — reduce(getitem, steps, root) is this node.
// ONE spelling of a path, for everything here that names a node.
//
// Two walks produce paths and they share the STEP, not the walk: _path ascends a
// chain of Nodes, the delta scan descends through zc::nodes. They each used to
// format their own and disagreed — deltas said `pts[1].y` where a node said
// `$.pts[1].y` — and the delta side decided positional-vs-named by whether the
// CHILD carried a name, which is the same bug _steps had.
static void path_append(std::string* path, bool positional, const char* name, size_t index) {
    if (positional || !name || !*name) {
        *path += "[";
        *path += std::to_string(index);
        *path += "]";
    } else {
        *path += ".";
        *path += name;
    }
}

// Which step identifies this node inside its parent: a field NAME under a
// struct, a POSITION under an array. Decided by the PARENT's type rather than by
// whether the child carries a name, because an array element carries an empty
// one — which is why the first cut of this rendered every element as "." and
// made two paths identical.
static PyObject* step_of(PyBBQNode* n) {
    bool positional;
    const char* nm = nullptr;
    {
        DocLock g(n->result);
        const zc::node* p = node_of(n->up);
        const zc::node* c = node_of(n);
        positional = !p || p->type == CaptureType::Array;
        if (c) nm = c->name;
    }
    if (positional || !nm || !*nm) return PyLong_FromSsize_t(n->slot);
    return PyUnicode_FromString(nm);
}

static PyObject* PyBBQNode_get_steps(PyBBQNode* self, void*) {
    PyObject* rev = PyList_New(0);
    if (!rev) return NULL;
    for (PyBBQNode* n = self; n->up; n = n->up) {
        PyObject* step = step_of(n);
        if (!step || PyList_Append(rev, step) < 0) {
            Py_XDECREF(step); Py_DECREF(rev); return NULL;
        }
        Py_DECREF(step);
    }
    if (PyList_Reverse(rev) < 0) { Py_DECREF(rev); return NULL; }
    PyObject* out = PyList_AsTuple(rev);
    Py_DECREF(rev);
    return out;
}

// The same, as something to put in an error message: $.chunks[7].kind
static PyObject* PyBBQNode_get_path(PyBBQNode* self, void*) {
    std::vector<PyBBQNode*> up;
    for (PyBBQNode* n = self; n->up; n = n->up) up.push_back(n);
    std::string path = "$";
    for (size_t i = up.size(); i-- > 0; ) {
        PyBBQNode* n = up[i];
        DocLock g(n->result);
        const zc::node* p = node_of(n->up);
        const zc::node* c = node_of(n);
        path_append(&path, !p || p->type == CaptureType::Array,
                    c ? c->name : nullptr, (size_t)n->slot);
    }
    return PyUnicode_FromStringAndSize(path.data(), (Py_ssize_t)path.size());
}

// ── Seeing the document ──────────────────────────────────────────────────────
//
// A library whose job is "what is in this binary file" has to be able to show
// you. Every walker in this repo's own test suite — dump_node in
// cross_backend_test.cpp, the walk in TestContainerProtocol — is this function
// written again by hand, which is the signal that it belongs here.
// An array whose elements are single bytes — which is how a grammar says "I do
// not know what this is yet". Listing them one per line is 32 lines that say
// nothing; the shape, the size and where it starts are the whole content.
static bool is_opaque_bytes(PyBBQNode* node, CaptureType ct, Py_ssize_t count) {
    if (ct != CaptureType::Array || count == 0) return false;
    DocLock g(node->result);
    const zc::node* n = node_of(node);
    if (!n) return false;
    for (const auto& k : n->kids)
        if (k->type != CaptureType::UInt8 && k->type != CaptureType::Int8) return false;
    return true;
}

static int dump_into(PyBBQNode* node, PyObject* out, int indent, int depth_left,
                     int limit) {
    CaptureType ct;
    Py_ssize_t count, start, end;
    const char* nm;
    {
        DocLock g(node->result);
        const zc::node* n = node_of(node);
        if (!n) return 0;
        ct = n->type; nm = n->name;
        start = (Py_ssize_t)n->start_offset; end = (Py_ssize_t)n->end_offset;
        count = is_container(ct) ? (Py_ssize_t)zc::size_of(n) : 0;
    }

    // The root is "$"; everything else is named or numbered by its parent.
    PyObject* label;
    if (!node->up) {
        label = PyUnicode_FromString("$");
    } else {
        PyObject* step = step_of(node);
        if (!step) return -1;
        label = PyUnicode_Check(step) ? Py_NewRef(step)
                                      : PyUnicode_FromFormat("[%S]", step);
        Py_DECREF(step);
    }
    if (!label) return -1;
    (void)nm;

    bool opaque = is_opaque_bytes(node, ct, count);

    PyObject* line;
    if (opaque) {
        // Summarised, not listed: how many bytes, and the first few so the shape
        // is visible. Anything more is a hex viewer, which is the caller's.
        PyObject* raw = PyBBQNode_get_raw(node, NULL);
        if (!raw) { Py_DECREF(label); return -1; }
        Py_ssize_t n_show = PyBytes_GET_SIZE(raw) < 8 ? PyBytes_GET_SIZE(raw) : 8;
        PyObject* head = PyObject_CallMethod(raw, "hex", NULL);
        Py_DECREF(raw);
        if (!head) { Py_DECREF(label); return -1; }
        PyObject* cut = PyUnicode_Substring(head, 0, n_show * 2);
        Py_DECREF(head);
        if (!cut) { Py_DECREF(label); return -1; }
        line = PyUnicode_FromFormat("%*s%U: bytes[%zd] = %U%s  @%zd..%zd\n", indent, "",
                                    label, count, cut, n_show < count ? "..." : "",
                                    start, end);
        Py_DECREF(cut);
    } else if (is_container(ct)) {
        line = PyUnicode_FromFormat("%*s%U: %s(%zd)  @%zd..%zd\n", indent, "",
                                    label, capture_type_name(ct), count, start, end);
    } else {
        PyObject* v = node_value(node->result, node_of(node));
        if (!v) { Py_DECREF(label); return -1; }
        PyObject* vs = PyObject_Repr(v);
        Py_DECREF(v);
        if (!vs) { Py_DECREF(label); return -1; }
        // A 4 MB bytes field must not become 4 MB of dump.
        if (PyUnicode_GET_LENGTH(vs) > 60) {
            PyObject* cut = PyUnicode_Substring(vs, 0, 57);
            PyObject* ell = cut ? PyUnicode_FromFormat("%U...", cut) : NULL;
            Py_XDECREF(cut); Py_DECREF(vs); vs = ell;
            if (!vs) { Py_DECREF(label); return -1; }
        }
        line = PyUnicode_FromFormat("%*s%U: %s = %U  @%zd..%zd\n", indent, "",
                                    label, capture_type_name(ct), vs, start, end);
        Py_DECREF(vs);
    }
    Py_DECREF(label);
    if (!line) return -1;
    int rc = PyList_Append(out, line);
    Py_DECREF(line);
    if (rc < 0) return -1;

    if (!is_container(ct) || opaque || depth_left == 0) return 0;
    if (Py_EnterRecursiveCall(" while dumping")) return -1;
    rc = 0;
    // A container with ten thousand elements is ten thousand lines nobody reads.
    Py_ssize_t shown = (limit > 0 && count > limit) ? limit : count;
    for (Py_ssize_t i = 0; i < shown && rc == 0; i++) {
        PyObject* kid = child_node(node->result, node, i);
        if (!kid) { rc = -1; break; }
        rc = dump_into((PyBBQNode*)kid, out, indent + 2,
                       depth_left < 0 ? -1 : depth_left - 1, limit);
        Py_DECREF(kid);
    }
    if (rc == 0 && shown < count) {
        PyObject* more = PyUnicode_FromFormat("%*s... %zd more\n", indent + 2, "",
                                              count - shown);
        if (!more) rc = -1;
        else { rc = PyList_Append(out, more); Py_DECREF(more); }
    }
    Py_LeaveRecursiveCall();
    return rc;
}

static PyObject* PyBBQNode_dump(PyBBQNode* self, PyObject* args, PyObject* kwargs) {
    int depth = -1, limit = 16;
    static const char* kwlist[] = {"depth", "limit", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ii", (char**)kwlist, &depth, &limit))
        return NULL;
    PyObject* lines = PyList_New(0);
    if (!lines) return NULL;
    if (dump_into(self, lines, 0, depth, limit) < 0) { Py_DECREF(lines); return NULL; }
    PyObject* empty = PyUnicode_FromString("");
    PyObject* out = empty ? PyUnicode_Join(empty, lines) : NULL;
    Py_XDECREF(empty);
    Py_DECREF(lines);
    return out;
}

// Every member is registered TWICE: once plain, once underscored.
//
// A binary format names its own fields, and some of them are called `name`,
// `value`, `offset` or `parent` — a library that claims those names makes those
// formats unreachable by the spelling their specification uses. So the
// underscored form is the canonical one and can always be relied on, the plain
// form is a convenience that YIELDS to a field of the same name (see
// PyBBQNode_getattro), and node["whatever"] is always the field and never a
// library member. Kaitai Struct answers the same problem the same way with
// _parent/_root/_io, and namedtuple with _fields/_replace/_asdict.
#define NODE_MEMBER(plain, fn, doc) \
    {(char*)plain,       (getter)fn, NULL, (char*)doc, NULL}, \
    {(char*)"_" plain,   (getter)fn, NULL, (char*)doc, NULL}

static PyGetSetDef PyBBQNode_getset[] = {
    NODE_MEMBER("offset",       PyBBQNode_get_offset,       "(start, end) byte offset tuple"),
    NODE_MEMBER("raw",          PyBBQNode_get_raw,          "this node's bytes as they stand"),
    NODE_MEMBER("capture_type", PyBBQNode_get_capture_type, "capture type name string"),
    NODE_MEMBER("name",         PyBBQNode_get_name,         "field name or None"),
    NODE_MEMBER("value",        PyBBQNode_get_value,        "auto-materialized Python value"),
    NODE_MEMBER("variant_tag",  PyBBQNode_get_variant_tag,  "union/switch arm ordinal, or None"),
    NODE_MEMBER("parsed",       PyBBQNode_get_parsed,       "True while this node still names input bytes"),
    NODE_MEMBER("parent",       PyBBQNode_get_parent,       "the node this one was reached through, or None"),
    NODE_MEMBER("root",         PyBBQNode_get_root,         "the document root this node came from"),
    NODE_MEMBER("path",         PyBBQNode_get_path,         "where this node is: $.chunks[7].kind"),
    NODE_MEMBER("steps",        PyBBQNode_get_steps,        "the path as a tuple of names and indices"),
    {NULL, NULL, NULL, NULL, NULL}
};

// ── Type object ──

PyTypeObject PyBBQNode_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bbq.Node",                             // tp_name
    sizeof(PyBBQNode),                      // tp_basicsize
    0,                                      // tp_itemsize
    (destructor)PyBBQNode_dealloc,          // tp_dealloc
    0,                                      // tp_vectorcall_offset
    NULL,                                   // tp_getattr
    NULL,                                   // tp_setattr
    NULL,                                   // tp_as_async
    (reprfunc)PyBBQNode_tp_repr,            // tp_repr
    &PyBBQNode_as_number,                   // tp_as_number
    &PyBBQNode_as_sequence,                 // tp_as_sequence
    &PyBBQNode_as_mapping,                  // tp_as_mapping
    NULL,                                   // tp_hash
    NULL,                                   // tp_call
    (reprfunc)PyBBQNode_tp_str,             // tp_str
    (getattrofunc)PyBBQNode_getattro,       // tp_getattro
    (setattrofunc)PyBBQNode_setattro,       // tp_setattro
    &PyBBQNode_as_buffer,                   // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    PyDoc_STR("A position in a parsed BBQ document."), // tp_doc
    (traverseproc)PyBBQNode_traverse,       // tp_traverse
    (inquiry)PyBBQNode_clear,               // tp_clear
    (richcmpfunc)PyBBQNode_richcompare,     // tp_richcompare
    0,                                      // tp_weaklistoffset
    (getiterfunc)PyBBQNode_tp_iter,         // tp_iter
    NULL,                                   // tp_iternext
    PyBBQNode_methods,                      // tp_methods
    NULL,                                   // tp_members
    PyBBQNode_getset,                       // tp_getset
};


// ── PyBBQNodeIter ───────────────────────────────────────────────────────────

static void PyBBQNodeIter_dealloc(PyBBQNodeIter* self) {
    PyObject_GC_UnTrack((PyObject*)self);
    Py_XDECREF(self->result);
    Py_XDECREF(self->owner);
    PyObject_GC_Del(self);
}

static int PyBBQNodeIter_traverse(PyBBQNodeIter* self, visitproc visit, void* arg) {
    Py_VISIT(self->result);
    Py_VISIT(self->owner);
    return 0;
}

static int PyBBQNodeIter_clear(PyBBQNodeIter* self) {
    Py_CLEAR(self->result);
    Py_CLEAR(self->owner);
    return 0;
}

static PyObject* PyBBQNodeIter_iternext(PyBBQNodeIter* self) {
    Py_ssize_t i = self->index;
    const char* name = nullptr;
    bool container;
    {
        DocLock g(self->result);
        if (i < 0 || (size_t)i >= self->container->kids.size())
            return NULL;  // StopIteration — tp_iternext convention
        const zc::node* k = self->container->kids[(size_t)i].get();
        name = k->name;
        container = is_container(k->type);
        if (container) self->result->edit->own_child(self->container, (size_t)i);
    }
    self->index++;

    PyBBQNode* node = PyBBQNode_New(self->result, self->container, i, self->owner);
    if (!node) return NULL;

    if (!self->yield_tuples) return (PyObject*)node;

    PyObject* key = name ? PyUnicode_FromString(name) : Py_NewRef(Py_None);
    if (!key) { Py_DECREF(node); return NULL; }
    PyObject* tuple = PyTuple_Pack(2, key, node);
    Py_DECREF(key);
    Py_DECREF(node);
    return tuple;
}

PyTypeObject PyBBQNodeIter_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bbq.NodeIter",                         // tp_name
    sizeof(PyBBQNodeIter),                  // tp_basicsize
    0,                                      // tp_itemsize
    (destructor)PyBBQNodeIter_dealloc,      // tp_dealloc
    0,                                      // tp_vectorcall_offset
    NULL,                                   // tp_getattr
    NULL,                                   // tp_setattr
    NULL,                                   // tp_as_async
    NULL,                                   // tp_repr
    NULL,                                   // tp_as_number
    NULL,                                   // tp_as_sequence
    NULL,                                   // tp_as_mapping
    NULL,                                   // tp_hash
    NULL,                                   // tp_call
    NULL,                                   // tp_str
    NULL,                                   // tp_getattro
    NULL,                                   // tp_setattro
    NULL,                                   // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    NULL,                                   // tp_doc
    (traverseproc)PyBBQNodeIter_traverse,   // tp_traverse
    (inquiry)PyBBQNodeIter_clear,           // tp_clear
    NULL,                                   // tp_richcompare
    0,                                      // tp_weaklistoffset
    PyObject_SelfIter,                      // tp_iter
    (iternextfunc)PyBBQNodeIter_iternext,   // tp_iternext
};


// ── PyBBQNodeList ───────────────────────────────────────────────────────────
//
// What a selector that can match more than one thing answers with.
//
// RFC 9535 §2.1.2: a query is a sequence of SEGMENTS, each applied to the
// nodelist the previous one produced, and the result of applying a segment to a
// nodelist is the concatenation of applying it to every node in it. That
// composition is the whole reason this type exists rather than a plain list:
// `root["xs"][...]["v"]` is three segments, and each one has to be able to take
// the one before it.
//
// A selector that can only ever match once — a name, an index — keeps returning
// a Node, so nothing that already worked changes shape. The rule is: a selector
// that can match more than one thing hands back a NodeList.
struct PyBBQNodeList {
    PyObject_HEAD
    PyObject* items;        // a real list of PyBBQNode, in document order
};

extern PyTypeObject PyBBQNodeList_Type;

static PyObject* nodelist_steal(PyObject* items) {
    if (!items) return NULL;
    PyBBQNodeList* nl = PyObject_GC_New(PyBBQNodeList, &PyBBQNodeList_Type);
    if (!nl) { Py_DECREF(items); return NULL; }
    nl->items = items;      // the reference is taken, not borrowed
    PyObject_GC_Track(nl);
    return (PyObject*)nl;
}

static void PyBBQNodeList_dealloc(PyBBQNodeList* self) {
    PyObject_GC_UnTrack((PyObject*)self);
    Py_XDECREF(self->items);
    PyObject_GC_Del(self);
}
static int PyBBQNodeList_traverse(PyBBQNodeList* self, visitproc visit, void* arg) {
    Py_VISIT(self->items);
    return 0;
}
static int PyBBQNodeList_clear(PyBBQNodeList* self) { Py_CLEAR(self->items); return 0; }

static Py_ssize_t PyBBQNodeList_length(PyBBQNodeList* self) {
    return PyList_GET_SIZE(self->items);
}
static PyObject* PyBBQNodeList_iter(PyBBQNodeList* self) {
    return PyObject_GetIter(self->items);
}
static PyObject* PyBBQNodeList_repr(PyBBQNodeList* self) {
    return PyUnicode_FromFormat("<bbq.NodeList %zd node%s>",
                                PyList_GET_SIZE(self->items),
                                PyList_GET_SIZE(self->items) == 1 ? "" : "s");
}

// `nodes` is the way to the list ITSELF, because subscripting a NodeList means
// applying a segment to every node in it — `nl[0]` is each node's first child,
// not the list's first node. The two readings cannot both have the brackets, and
// the composing one is the reason the type is here.
static PyObject* PyBBQNodeList_get_nodes(PyBBQNodeList* self, void*) {
    return PyList_GetSlice(self->items, 0, PyList_GET_SIZE(self->items));
}

// The values of every node in the list — the common last step of a query.
static PyObject* PyBBQNodeList_get_values(PyBBQNodeList* self, void*) {
    Py_ssize_t n = PyList_GET_SIZE(self->items);
    PyObject* out = PyList_New(n);
    if (!out) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* v = PyObject_GetAttrString(PyList_GET_ITEM(self->items, i), "value");
        if (!v) { Py_DECREF(out); return NULL; }
        PyList_SET_ITEM(out, i, v);
    }
    return out;
}

// ── Selectors ────────────────────────────────────────────────────────────────
//
// The vocabulary, and its RFC 9535 spelling:
//
//   node["name"]        $.name          name selector      → Node (one, strict)
//   node[3] / node[-1]  $[3]            index selector     → Node (one, strict)
//   node[1:3:2]         $[1:3:2]        array slice        → NodeList
//   node[0, 2]          $[0,2]          several selectors  → NodeList
//   node[:]             $[*]            wildcard           → NodeList
//   node[...]           the node set `..` iterates over    → NodeList
//   node[..., "name"]   $..name         descendant segment → NodeList
//
// A miss inside a multi-selector contributes nothing, per §2.3.1.2 ("selects
// nothing if there is no such member"). The single accessors keep raising,
// because they are how a caller asks for one field and expects to get it.

extern PyTypeObject PyBBQQuery_Type;
static int query_matches(PyBBQNode* node, PyObject* q);

static bool key_is_multi(PyObject* key) {
    return key == Py_Ellipsis || PyTuple_Check(key) || PySlice_Check(key)
        || Py_TYPE(key) == &PyBBQQuery_Type;
}

static int select_into(PyBBQNode* node, PyObject* key, PyObject* out);

// The node itself and every node below it, in document order — what a descendant
// segment applies its selectors to (§2.5.2.2).
static int collect_descendants(PyBBQNode* node, PyObject* out) {
    if (PyList_Append(out, (PyObject*)node) < 0) return -1;
    Py_ssize_t count = 0;
    {
        DocLock g(node->result);
        const zc::node* c = node_of(node);
        if (c && is_container(c->type)) count = (Py_ssize_t)zc::size_of(c);
    }
    if (count == 0) return 0;
    // A recursive grammar over input nobody here wrote can nest as deep as the
    // input likes. Python's own recursion limit is what turns that into a
    // RecursionError instead of a blown C stack.
    if (Py_EnterRecursiveCall(" while walking descendants")) return -1;
    int rc = 0;
    for (Py_ssize_t i = 0; i < count && rc == 0; i++) {
        PyObject* kid = child_node(node->result, node, i);
        if (!kid) { rc = -1; break; }
        rc = collect_descendants((PyBBQNode*)kid, out);
        Py_DECREF(kid);
    }
    Py_LeaveRecursiveCall();
    return rc;
}

// One selector, one node, every match appended in order.
static int select_into(PyBBQNode* node, PyObject* key, PyObject* out) {
    CaptureType ct;
    Py_ssize_t count;
    {
        DocLock g(node->result);
        const zc::node* n = node_of(node);
        if (!n) { PyErr_SetString(PyExc_RuntimeError, "node vanished"); return -1; }
        ct = n->type;
        count = is_container(ct) ? (Py_ssize_t)zc::size_of(n) : 0;
    }

    if (key == Py_Ellipsis) return collect_descendants(node, out);

    // The filter selector: every CHILD of this node that satisfies the
    // expression (§2.3.5.2 — a filter applies to the children of the node it is
    // applied to, not to the node itself).
    if (Py_TYPE(key) == &PyBBQQuery_Type) {
        if (!is_container(ct)) return 0;
        for (Py_ssize_t i = 0; i < count; i++) {
            PyObject* kid = child_node(node->result, node, i);
            if (!kid) return -1;
            int hit = query_matches((PyBBQNode*)kid, key);
            if (hit < 0) { Py_DECREF(kid); return -1; }
            int rc = hit ? PyList_Append(out, kid) : 0;
            Py_DECREF(kid);
            if (rc < 0) return -1;
        }
        return 0;
    }

    if (PyTuple_Check(key)) {
        Py_ssize_t k = PyTuple_GET_SIZE(key);
        // A leading Ellipsis is the descendant segment: the rest of the tuple is
        // applied to this node AND every node below it.
        if (k > 0 && PyTuple_GET_ITEM(key, 0) == Py_Ellipsis) {
            PyObject* scope = PyList_New(0);
            if (!scope) return -1;
            if (collect_descendants(node, scope) < 0) { Py_DECREF(scope); return -1; }
            int rc = 0;
            for (Py_ssize_t s = 0; s < PyList_GET_SIZE(scope) && rc == 0; s++) {
                PyBBQNode* d = (PyBBQNode*)PyList_GET_ITEM(scope, s);
                for (Py_ssize_t j = 1; j < k && rc == 0; j++)
                    rc = select_into(d, PyTuple_GET_ITEM(key, j), out);
            }
            Py_DECREF(scope);
            return rc;
        }
        for (Py_ssize_t j = 0; j < k; j++)
            if (select_into(node, PyTuple_GET_ITEM(key, j), out) < 0) return -1;
        return 0;
    }

    if (PySlice_Check(key)) {
        if (!is_container(ct)) return 0;        /* nothing to slice: no matches */
        Py_ssize_t start, stop, step, length;
        if (PySlice_GetIndicesEx(key, count, &start, &stop, &step, &length) < 0) return -1;
        for (Py_ssize_t i = 0, idx = start; i < length; i++, idx += step) {
            PyObject* kid = child_node(node->result, node, idx);
            if (!kid) return -1;
            int rc = PyList_Append(out, kid);
            Py_DECREF(kid);
            if (rc < 0) return -1;
        }
        return 0;
    }

    if (PyUnicode_Check(key)) {
        if (!is_container(ct)) return 0;
        const char* name = PyUnicode_AsUTF8(key);
        if (!name) return -1;
        Py_ssize_t slot;
        { DocLock g(node->result); slot = child_slot(node_of(node), name); }
        if (slot < 0) return 0;                 /* selects nothing, not an error */
        PyObject* kid = child_node(node->result, node, slot);
        if (!kid) return -1;
        int rc = PyList_Append(out, kid);
        Py_DECREF(kid);
        return rc;
    }

    if (PyIndex_Check(key)) {
        if (!is_container(ct)) return 0;
        Py_ssize_t idx = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (idx == -1 && PyErr_Occurred()) return -1;
        if (idx < 0) idx += count;
        if (idx < 0 || idx >= count) return 0;  /* out of range selects nothing */
        PyObject* kid = child_node(node->result, node, idx);
        if (!kid) return -1;
        int rc = PyList_Append(out, kid);
        Py_DECREF(kid);
        return rc;
    }

    PyErr_Format(PyExc_TypeError, "not a selector: %.200s", Py_TYPE(key)->tp_name);
    return -1;
}

static PyObject* node_select(PyBBQNode* node, PyObject* key) {
    PyObject* items = PyList_New(0);
    if (!items) return NULL;
    if (select_into(node, key, items) < 0) { Py_DECREF(items); return NULL; }
    return nodelist_steal(items);
}

// Subscripting a NodeList reads one of two ways, and which one is decided by
// the key, because each kind has exactly one sensible meaning:
//
//   nl[0], nl[1:3]      POSITIONAL — the list is a Python sequence, and
//                       `xs[0:2][0]` has to be the first of the two.
//   nl["x"], nl[...],   COMPOSITIONAL — a segment applied to every node and
//   nl[(..., "x")],     concatenated (§2.1.2), which is what makes
//   nl[bbq.this.x > 1]  root["pts"][:]["x"] mean anything. There is no
//                       competing positional reading for any of these.
//
// The wildcard is the one selector that would have been ambiguous, so it is
// named instead of punned: `nl.children`.
static PyObject* PyBBQNodeList_subscript(PyBBQNodeList* self, PyObject* key) {
    Py_ssize_t n = PyList_GET_SIZE(self->items);

    if (PyIndex_Check(key)) {
        Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred()) return NULL;
        if (i < 0) i += n;
        if (i < 0 || i >= n) {
            PyErr_SetString(PyExc_IndexError, "nodelist index out of range");
            return NULL;
        }
        return Py_NewRef(PyList_GET_ITEM(self->items, i));
    }
    if (PySlice_Check(key)) {
        Py_ssize_t start, stop, step, length;
        if (PySlice_GetIndicesEx(key, n, &start, &stop, &step, &length) < 0) return NULL;
        PyObject* items = PyList_New(length);
        if (!items) return NULL;
        for (Py_ssize_t i = 0, idx = start; i < length; i++, idx += step)
            PyList_SET_ITEM(items, i, Py_NewRef(PyList_GET_ITEM(self->items, idx)));
        return nodelist_steal(items);
    }

    PyObject* items = PyList_New(0);
    if (!items) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyBBQNode* node = (PyBBQNode*)PyList_GET_ITEM(self->items, i);
        if (select_into(node, key, items) < 0) { Py_DECREF(items); return NULL; }
    }
    return nodelist_steal(items);
}

// The wildcard applied to every node in the list — `$[*]` one level on.
static PyObject* PyBBQNodeList_get_children(PyBBQNodeList* self, void*) {
    PyObject* items = PyList_New(0);
    if (!items) return NULL;
    PyObject* all = PySlice_New(NULL, NULL, NULL);
    if (!all) { Py_DECREF(items); return NULL; }
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(self->items); i++) {
        PyBBQNode* node = (PyBBQNode*)PyList_GET_ITEM(self->items, i);
        if (select_into(node, all, items) < 0) {
            Py_DECREF(all); Py_DECREF(items); return NULL;
        }
    }
    Py_DECREF(all);
    return nodelist_steal(items);
}

static PyMappingMethods PyBBQNodeList_as_mapping = {
    (lenfunc)      PyBBQNodeList_length,
    (binaryfunc)   PyBBQNodeList_subscript,
    (objobjargproc)NULL,
};

static PyGetSetDef PyBBQNodeList_getset[] = {
    {(char*)"nodes",  (getter)PyBBQNodeList_get_nodes,  NULL,
     (char*)"the nodes themselves, as a plain list", NULL},
    {(char*)"values", (getter)PyBBQNodeList_get_values, NULL,
     (char*)"each node's value, as a plain list", NULL},
    {(char*)"children", (getter)PyBBQNodeList_get_children, NULL,
     (char*)"every child of every node — the wildcard, one level on", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

// ── The filter expression ────────────────────────────────────────────────────
//
// RFC 9535 §2.3.5's filter selector, spelled the way pandas and SQLAlchemy spell
// one: operators on a PLACEHOLDER build a tree instead of computing an answer.
//
// It has to be a placeholder and cannot be Node itself. Node's __eq__ already
// materialises the value and compares it, so `node.kind == 3` is a real bool —
// which is exactly the property that makes the rest of the API pleasant and
// exactly why it cannot double as the expression builder. `bbq.this` is `@`.
//
//     pts[bbq.this.x > 1]                     $[?@.x > 1]
//     pts[(bbq.this.x > 1) & (bbq.this.y < 9)]   $[?@.x > 1 && @.y < 9]
//     pts[~(bbq.this.x == 1)]                 $[?!(@.x == 1)]
//     pts[bbq.this.x]                         $[?@.x]        (existence)
//     pts[bbq.count(bbq.this[:]) == 2]        $[?count(@[*]) == 2]
//
// & | ~ rather than and/or/not, because Python cannot overload those three.

enum QKind { Q_PATH, Q_CMP, Q_AND, Q_OR, Q_NOT, Q_COUNT, Q_LENGTH };

struct PyBBQQuery {
    PyObject_HEAD
    int kind;
    PyObject* steps;     // Q_PATH: a list of selector keys, applied in order
    PyObject* left;      // operand, or the operand of count()/length()
    PyObject* right;     // Q_CMP: the other side — a Query or a plain Python value
    int op;              // Q_CMP: Py_EQ … Py_GE
};

static PyObject* query_new(int kind) {
    PyBBQQuery* q = PyObject_GC_New(PyBBQQuery, &PyBBQQuery_Type);
    if (!q) return NULL;
    q->kind = kind; q->steps = NULL; q->left = NULL; q->right = NULL; q->op = 0;
    PyObject_GC_Track(q);
    return (PyObject*)q;
}

static void PyBBQQuery_dealloc(PyBBQQuery* self) {
    PyObject_GC_UnTrack((PyObject*)self);
    Py_XDECREF(self->steps); Py_XDECREF(self->left); Py_XDECREF(self->right);
    PyObject_GC_Del(self);
}
static int PyBBQQuery_traverse(PyBBQQuery* self, visitproc visit, void* arg) {
    Py_VISIT(self->steps); Py_VISIT(self->left); Py_VISIT(self->right);
    return 0;
}
static int PyBBQQuery_clear(PyBBQQuery* self) {
    Py_CLEAR(self->steps); Py_CLEAR(self->left); Py_CLEAR(self->right);
    return 0;
}

// `this.a["b"][0]` — each step is a selector, appended to a fresh path so a
// placeholder can be reused and branched from without being mutated.
static PyObject* query_extend(PyBBQQuery* base, PyObject* step) {
    if (base->kind != Q_PATH) {
        PyErr_SetString(PyExc_TypeError,
                        "only a path expression can be navigated further");
        return NULL;
    }
    PyObject* q = query_new(Q_PATH);
    if (!q) return NULL;
    PyObject* steps = base->steps ? PySequence_List(base->steps) : PyList_New(0);
    if (!steps || PyList_Append(steps, step) < 0) {
        Py_XDECREF(steps); Py_DECREF(q); return NULL;
    }
    ((PyBBQQuery*)q)->steps = steps;
    return q;
}

static PyObject* PyBBQQuery_getattro(PyBBQQuery* self, PyObject* name) {
    const char* s = PyUnicode_AsUTF8(name);
    if (!s) return NULL;
    // Dunders and privates stay attributes, so repr() and isinstance() work; a
    // field actually called `_x` is reachable as this["_x"].
    if (s[0] == '_') return PyObject_GenericGetAttr((PyObject*)self, name);
    return query_extend(self, name);
}

static PyObject* PyBBQQuery_subscript(PyBBQQuery* self, PyObject* key) {
    return query_extend(self, key);
}

static PyObject* PyBBQQuery_richcompare(PyObject* self, PyObject* other, int op) {
    PyObject* q = query_new(Q_CMP);
    if (!q) return NULL;
    ((PyBBQQuery*)q)->op = op;
    Py_INCREF(self);  ((PyBBQQuery*)q)->left  = self;
    Py_INCREF(other); ((PyBBQQuery*)q)->right = other;
    return q;
}

static PyObject* query_binop(int kind, PyObject* a, PyObject* b) {
    if (Py_TYPE(a) != &PyBBQQuery_Type || Py_TYPE(b) != &PyBBQQuery_Type)
        Py_RETURN_NOTIMPLEMENTED;
    PyObject* q = query_new(kind);
    if (!q) return NULL;
    Py_INCREF(a); ((PyBBQQuery*)q)->left  = a;
    Py_INCREF(b); ((PyBBQQuery*)q)->right = b;
    return q;
}
static PyObject* PyBBQQuery_and(PyObject* a, PyObject* b) { return query_binop(Q_AND, a, b); }
static PyObject* PyBBQQuery_or (PyObject* a, PyObject* b) { return query_binop(Q_OR,  a, b); }
static PyObject* PyBBQQuery_not(PyObject* a) {
    PyObject* q = query_new(Q_NOT);
    if (!q) return NULL;
    Py_INCREF(a); ((PyBBQQuery*)q)->left = a;
    return q;
}

// An expression is never a bool by accident: `if this.x == 1` or a stray
// `and`/`or` would silently take this branch and throw the query away.
static int PyBBQQuery_bool(PyObject*) {
    PyErr_SetString(PyExc_TypeError,
        "a bbq query is not a truth value — combine with & | ~, not and/or/not");
    return -1;
}

static PyObject* PyBBQQuery_repr(PyBBQQuery* self) {
    static const char* names[] = {"path", "cmp", "and", "or", "not", "count", "length"};
    return PyUnicode_FromFormat("<bbq.query %s>", names[self->kind]);
}

// ── Evaluating one against a node ────────────────────────────────────────────

// The nodes a path expression reaches from `node` (with `@` bound to it).
static PyObject* query_path_nodes(PyBBQNode* node, PyBBQQuery* q) {
    PyObject* items = PyList_New(0);
    if (!items) return NULL;
    if (PyList_Append(items, (PyObject*)node) < 0) { Py_DECREF(items); return NULL; }
    Py_ssize_t nsteps = q->steps ? PyList_GET_SIZE(q->steps) : 0;
    for (Py_ssize_t s = 0; s < nsteps; s++) {
        PyObject* next = PyList_New(0);
        if (!next) { Py_DECREF(items); return NULL; }
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(items); i++) {
            PyBBQNode* cur = (PyBBQNode*)PyList_GET_ITEM(items, i);
            if (select_into(cur, PyList_GET_ITEM(q->steps, s), next) < 0) {
                Py_DECREF(next); Py_DECREF(items); return NULL;
            }
        }
        Py_DECREF(items);
        items = next;
    }
    return items;
}

// One side of a comparison. A query that did not match exactly one node is
// "Nothing" (§2.3.5.2), which is reported as NULL with no error set.
static PyObject* query_operand(PyBBQNode* node, PyObject* side, bool* nothing) {
    *nothing = false;
    if (Py_TYPE(side) != &PyBBQQuery_Type) { Py_INCREF(side); return side; }
    PyBBQQuery* q = (PyBBQQuery*)side;
    if (q->kind == Q_COUNT) {
        PyObject* nodes = query_path_nodes(node, (PyBBQQuery*)q->left);
        if (!nodes) return NULL;
        PyObject* n = PyLong_FromSsize_t(PyList_GET_SIZE(nodes));
        Py_DECREF(nodes);
        return n;
    }
    if (q->kind == Q_LENGTH || q->kind == Q_PATH) {
        PyBBQQuery* path = (PyBBQQuery*)(q->kind == Q_LENGTH ? q->left : (PyObject*)q);
        PyObject* nodes = query_path_nodes(node, path);
        if (!nodes) return NULL;
        if (PyList_GET_SIZE(nodes) != 1) { Py_DECREF(nodes); *nothing = true; return NULL; }
        PyObject* only = PyList_GET_ITEM(nodes, 0);
        PyObject* out;
        if (q->kind == Q_LENGTH) {
            Py_ssize_t len = PyObject_Length(only);
            if (len < 0) { PyErr_Clear(); Py_DECREF(nodes); *nothing = true; return NULL; }
            out = PyLong_FromSsize_t(len);
        } else {
            out = PyObject_GetAttrString(only, "value");
        }
        Py_DECREF(nodes);
        return out;
    }
    PyErr_SetString(PyExc_TypeError, "a logical expression is not a value");
    return NULL;
}

static int query_matches(PyBBQNode* node, PyObject* expr) {
    PyBBQQuery* q = (PyBBQQuery*)expr;
    switch (q->kind) {
    case Q_PATH: {                      // existence test
        PyObject* nodes = query_path_nodes(node, q);
        if (!nodes) return -1;
        int hit = PyList_GET_SIZE(nodes) > 0;
        Py_DECREF(nodes);
        return hit;
    }
    case Q_NOT: {
        int v = query_matches(node, q->left);
        return v < 0 ? -1 : !v;
    }
    case Q_AND: case Q_OR: {
        int a = query_matches(node, q->left);
        if (a < 0) return -1;
        if (q->kind == Q_AND && !a) return 0;
        if (q->kind == Q_OR  &&  a) return 1;
        return query_matches(node, q->right);
    }
    case Q_CMP: {
        bool ln = false, rn = false;
        PyObject* lv = query_operand(node, q->left, &ln);
        if (!lv && !ln) return -1;
        PyObject* rv = query_operand(node, q->right, &rn);
        if (!rv && !rn) { Py_XDECREF(lv); return -1; }
        int result;
        if (ln || rn) {
            // Nothing equals only Nothing; every ordering against it is false.
            result = (q->op == Py_EQ) ? (ln && rn)
                   : (q->op == Py_NE) ? !(ln && rn)
                   : 0;
        } else {
            result = PyObject_RichCompareBool(lv, rv, q->op);
        }
        Py_XDECREF(lv); Py_XDECREF(rv);
        return result;
    }
    default:
        PyErr_SetString(PyExc_TypeError, "count()/length() is a value, not a test");
        return -1;
    }
}

static PyNumberMethods PyBBQQuery_as_number = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    (inquiry)PyBBQQuery_bool,               // nb_bool
    (unaryfunc)PyBBQQuery_not,              // nb_invert  (~)
    NULL, NULL,
    (binaryfunc)PyBBQQuery_and,             // nb_and     (&)
    NULL,
    (binaryfunc)PyBBQQuery_or,              // nb_or      (|)
};

static PyMappingMethods PyBBQQuery_as_mapping = {
    NULL, (binaryfunc)PyBBQQuery_subscript, NULL,
};

PyTypeObject PyBBQQuery_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bbq.Query",                             // tp_name
    sizeof(PyBBQQuery),                      // tp_basicsize
    0,                                       // tp_itemsize
    (destructor)PyBBQQuery_dealloc,          // tp_dealloc
    0,                                       // tp_vectorcall_offset
    NULL,                                    // tp_getattr
    NULL,                                    // tp_setattr
    NULL,                                    // tp_as_async
    (reprfunc)PyBBQQuery_repr,               // tp_repr
    &PyBBQQuery_as_number,                   // tp_as_number
    NULL,                                    // tp_as_sequence
    &PyBBQQuery_as_mapping,                  // tp_as_mapping
    NULL,                                    // tp_hash
    NULL,                                    // tp_call
    NULL,                                    // tp_str
    (getattrofunc)PyBBQQuery_getattro,       // tp_getattro
    NULL,                                    // tp_setattro
    NULL,                                    // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    PyDoc_STR("A filter expression over the node being tested."), // tp_doc
    (traverseproc)PyBBQQuery_traverse,       // tp_traverse
    (inquiry)PyBBQQuery_clear,               // tp_clear
    (richcmpfunc)PyBBQQuery_richcompare,     // tp_richcompare
};

static PyObject* bbq_count(PyObject*, PyObject* arg) {
    if (Py_TYPE(arg) != &PyBBQQuery_Type || ((PyBBQQuery*)arg)->kind != Q_PATH) {
        PyErr_SetString(PyExc_TypeError, "count() takes a path expression");
        return NULL;
    }
    PyObject* q = query_new(Q_COUNT);
    if (!q) return NULL;
    Py_INCREF(arg); ((PyBBQQuery*)q)->left = arg;
    return q;
}
static PyObject* bbq_length(PyObject*, PyObject* arg) {
    if (Py_TYPE(arg) != &PyBBQQuery_Type || ((PyBBQQuery*)arg)->kind != Q_PATH) {
        PyErr_SetString(PyExc_TypeError, "length() takes a path expression");
        return NULL;
    }
    PyObject* q = query_new(Q_LENGTH);
    if (!q) return NULL;
    Py_INCREF(arg); ((PyBBQQuery*)q)->left = arg;
    return q;
}

PyTypeObject PyBBQNodeList_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bbq.NodeList",                          // tp_name
    sizeof(PyBBQNodeList),                   // tp_basicsize
    0,                                       // tp_itemsize
    (destructor)PyBBQNodeList_dealloc,       // tp_dealloc
    0,                                       // tp_vectorcall_offset
    NULL,                                    // tp_getattr
    NULL,                                    // tp_setattr
    NULL,                                    // tp_as_async
    (reprfunc)PyBBQNodeList_repr,            // tp_repr
    NULL,                                    // tp_as_number
    NULL,                                    // tp_as_sequence
    &PyBBQNodeList_as_mapping,               // tp_as_mapping
    NULL,                                    // tp_hash
    NULL,                                    // tp_call
    NULL,                                    // tp_str
    PyObject_GenericGetAttr,                 // tp_getattro
    NULL,                                    // tp_setattro
    NULL,                                    // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    PyDoc_STR("The nodes a selector matched."), // tp_doc
    (traverseproc)PyBBQNodeList_traverse,    // tp_traverse
    (inquiry)PyBBQNodeList_clear,            // tp_clear
    NULL,                                    // tp_richcompare
    0,                                       // tp_weaklistoffset
    (getiterfunc)PyBBQNodeList_iter,         // tp_iter
    NULL,                                    // tp_iternext
    NULL,                                    // tp_methods
    NULL,                                    // tp_members
    PyBBQNodeList_getset,                    // tp_getset
};


// ── PyBBQResult ─────────────────────────────────────────────────────────────
//
// The document. Everything about its CONTENT is the root node's, and is forwarded there
// rather than reimplemented — what belongs here is what is about the parse: whether it
// succeeded, how far it got, and the bytes it turns back into.

static void PyBBQResult_dealloc(PyBBQResult* self) {
    PyObject_GC_UnTrack((PyObject*)self);
    delete self->edit;
    delete self->arena;
    if (self->view_valid)
        PyBuffer_Release(&self->view);
    Py_XDECREF(self->spec);
    PyObject_GC_Del(self);
}

static int PyBBQResult_traverse(PyBBQResult* self, visitproc visit, void* arg) {
    if (self->view_valid && self->view.obj)
        Py_VISIT(self->view.obj);
    Py_VISIT(self->spec);
    return 0;
}

static int PyBBQResult_clear(PyBBQResult* self) {
    if (self->view_valid) {
        PyBuffer_Release(&self->view);
        self->view_valid = false;
    }
    Py_CLEAR(self->spec);
    return 0;
}

// The root, as a node. Everything the document forwards goes through this.
static PyBBQNode* result_root(PyBBQResult* self) {
    { DocLock g(self);
      if (!self->edit->root()) {
          PyErr_SetString(PyExc_AttributeError, "no parse tree");
          return NULL;
      } }
    return PyBBQNode_New(self, nullptr, -1, nullptr);
}

// The same rule as Node's: a root field named `success` or `root` has to be
// reachable as `result.success`, and the library's own is `result._success`.
static PyObject* PyBBQResult_getattro(PyBBQResult* self, PyObject* name) {
    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return NULL;

    if (key[0] != '_') {
        Py_ssize_t slot;
        { DocLock g(self); slot = child_slot(self->edit->root(), key); }
        if (slot >= 0) {
            PyBBQNode* root = PyBBQNode_New(self, nullptr, -1, nullptr);
            if (!root) return NULL;
            PyObject* child = child_node(self, root, slot);
            Py_DECREF(root);
            return child;
        }
    }
    return PyObject_GenericGetAttr((PyObject*)self, name);
}

static int PyBBQResult_setattro(PyBBQResult* self, PyObject* name, PyObject* value) {
    if (!value) { PyErr_SetString(PyExc_TypeError, "cannot delete a BBQ field"); return -1; }
    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return -1;
    { DocLock g(self);
      if (!self->edit->root()) {
          PyErr_SetString(PyExc_AttributeError, "no parse tree"); return -1;
      } }
    PyBBQNode* root = PyBBQNode_New(self, nullptr, -1, nullptr);
    if (!root) return -1;
    int rc = write_child(self, root, key, -1, value);
    Py_DECREF(root);
    return rc;
}

// ── Result repr ──

static PyObject* PyBBQResult_tp_repr(PyBBQResult* self) {
    if (!self->success)
        return PyUnicode_FromFormat("<bbq.ParseResult failed at offset 0x%zx>",
                                    self->error_offset);
    std::string fields;
    {
        DocLock g(self);
        if (const zc::node* r = self->edit->root())
            for (const auto& k : r->kids) {
                if (!k->name) continue;
                if (!fields.empty()) fields += ", ";
                fields += k->name;
            }
    }
    return PyUnicode_FromFormat("<bbq.ParseResult ok %zd bytes [%s]>",
                                self->bytes_consumed, fields.c_str());
}

// ── Result properties ──

static PyObject* PyBBQResult_get_success(PyBBQResult* self, void*) {
    return PyBool_FromLong(self->success);
}

static PyObject* PyBBQResult_get_bytes_consumed(PyBBQResult* self, void*) {
    return PyLong_FromSize_t(self->bytes_consumed);
}

static PyObject* PyBBQResult_get_error_message(PyBBQResult* self, void*) {
    if (!self->error_message) Py_RETURN_NONE;
    return PyUnicode_FromString(self->error_message);
}

static PyObject* PyBBQResult_get_error_offset(PyBBQResult* self, void*) {
    return PyLong_FromSize_t(self->error_offset);
}

static PyObject* PyBBQResult_get_root(PyBBQResult* self, void*) {
    { DocLock g(self); if (!self->edit->root()) Py_RETURN_NONE; }
    return (PyObject*)PyBBQNode_New(self, nullptr, -1, nullptr);
}

// ── The bytes that went in ───────────────────────────────────────────────────
//
// A ParseResult holds the input for its whole life — for parse_file that is a
// live mmap, and the spans in the document name it — so the bytes are right
// there and there was simply no way to ask for them. That matters most when a
// grammar covers only part of a file, which is the normal state of affairs while
// a format is still being worked out: the parse SUCCEEDS with bytes_consumed
// short of the end, and the interesting part is the remainder.
//
// A memoryview over the source object rather than over the raw pointer, so the
// mapping cannot go away underneath it. Zero-copy, and read-only because the
// document's spans point into it.
//
// `bytes(result)` is the other direction — what emit() would write.
static PyObject* PyBBQResult_get_input(PyBBQResult* self, void*) {
    if (!self->view_valid || !self->view.obj) Py_RETURN_NONE;
    PyObject* mv = PyMemoryView_FromObject(self->view.obj);
    if (!mv) return NULL;
    PyObject* ro = PyObject_CallMethod(mv, "toreadonly", NULL);
    Py_DECREF(mv);
    return ro;
}

// What the grammar did not account for: input[bytes_consumed:].
static PyObject* PyBBQResult_get_tail(PyBBQResult* self, void*) {
    PyObject* mv = PyBBQResult_get_input(self, NULL);
    if (!mv || mv == Py_None) return mv;
    size_t used;
    { DocLock g(self); used = self->bytes_consumed; }
    PyObject* sl = PySequence_GetSlice(mv, (Py_ssize_t)used, PY_SSIZE_T_MAX);
    Py_DECREF(mv);
    return sl;
}

#define RESULT_MEMBER(plain, fn, doc) \
    {(char*)plain,     (getter)fn, NULL, (char*)doc, NULL}, \
    {(char*)"_" plain, (getter)fn, NULL, (char*)doc, NULL}

static PyGetSetDef PyBBQResult_getset[] = {
    RESULT_MEMBER("success",        PyBBQResult_get_success,        "whether the parse succeeded"),
    RESULT_MEMBER("bytes_consumed", PyBBQResult_get_bytes_consumed, "how many bytes the parse consumed"),
    RESULT_MEMBER("error_message",  PyBBQResult_get_error_message,  "failure message, or None"),
    RESULT_MEMBER("error_offset",   PyBBQResult_get_error_offset,   "offset the parse gave up at"),
    RESULT_MEMBER("root",           PyBBQResult_get_root,           "the root node"),
    RESULT_MEMBER("input",          PyBBQResult_get_input,          "the bytes that were parsed, as a read-only memoryview"),
    RESULT_MEMBER("tail",           PyBBQResult_get_tail,           "input[bytes_consumed:] — what the grammar did not account for"),
    {NULL, NULL, NULL, NULL, NULL}
};

// ── Result methods ──

static PyObject* PyBBQResult_dir(PyBBQResult* self, PyObject*) {
    PyObject* list = PyList_New(0);
    if (!list) return NULL;
    if (append_type_attrs(list, &PyBBQResult_Type) < 0) { Py_DECREF(list); return NULL; }
    DocLock g(self);
    if (append_child_names(list, self->edit->root()) < 0) { Py_DECREF(list); return NULL; }
    return list;
}

// `put` — the write half of the lens, and one ZCow call. Serializing settles the
// DEPENDENT fields the edits invalidated (array counts, @rest window sizes) and then
// emits: unedited that is the identity, byte for byte — GetPut — and edited, the result
// re-parses to the edit — PutGet. An edit that resizes nothing patches the input in
// place, so bytes no field covers survive it.
static PyObject* PyBBQResult_emit(PyBBQResult* self, PyObject*) {
    // A failed parse produced no document, so there is nothing to serialize and nothing
    // to enforce a grammar over: what comes back is the input, unchanged.
    { DocLock g(self);
      if (!self->edit->root())
          return PyBytes_FromStringAndSize(
              self->view_valid ? (const char*)self->view.buf : "",
              self->view_valid ? self->view.len : 0); }

    std::vector<uint8_t> out;
    { DocLock g(self); out = self->edit->serialize(); }
    return PyBytes_FromStringAndSize((const char*)out.data(), (Py_ssize_t)out.size());
}

// What is no longer the input: every leaf that stopped being described by its span,
// with the value the file held and the value it holds now. A node still span-backed
// cannot have changed, which is what keeps this proportional to the edit.
static void collect_deltas(PyBBQResult* self, const zc::node* n, const std::string& path,
                           std::vector<std::pair<std::string, const zc::node*>>* out) {
    if (!n || n->parsed) return;
    if (is_container(n->type)) {
        bool positional = n->type == CaptureType::Array;
        for (size_t i = 0; i < n->kids.size(); i++) {
            std::string kid = path;
            path_append(&kid, positional, n->kids[i]->name, i);
            collect_deltas(self, n->kids[i].get(), kid, out);
        }
        return;
    }
    out->emplace_back(path, n);
}

static PyObject* PyBBQResult_deltas(PyBBQResult* self, PyObject*) {
    PyObject* list = PyList_New(0);
    if (!list) return NULL;

    // Snapshot under the lock, build the Python objects after it — PyBytes/PyLong
    // construction must not run while the lock is held.
    struct Row { std::string path; size_t start, end; PyObject* old_v; PyObject* new_v; };
    std::vector<Row> rows;
    {
        DocLock g(self);
        std::vector<std::pair<std::string, const zc::node*>> hits;
        collect_deltas(self, self->edit->root(), std::string("$"), &hits);
        const zc::source& src = self->edit->src();
        for (const auto& h : hits) {
            const zc::node* n = h.second;
            // What the file said: the same node read as if it were still its span.
            zc::node was = *n;
            was.parsed = true;
            PyObject* old_v = (n->end_offset > n->start_offset && src.buf)
                ? node_value(self, &was) : Py_NewRef(Py_None);
            PyObject* new_v = node_value(self, n);
            rows.push_back({h.first, n->start_offset, n->end_offset, old_v, new_v});
        }
    }
    for (auto& r : rows) {
        PyObject* item = Py_BuildValue("{s:s,s:(nn),s:O,s:O}",
            "path", r.path.c_str(),
            "offset", (Py_ssize_t)r.start, (Py_ssize_t)r.end,
            "old", r.old_v ? r.old_v : Py_None,
            "new", r.new_v ? r.new_v : Py_None);
        Py_XDECREF(r.old_v); Py_XDECREF(r.new_v);
        if (item) { PyList_Append(list, item); Py_DECREF(item); }
    }
    return list;
}

static PyObject* PyBBQResult_bytes(PyBBQResult* self, PyObject*) {
    return PyBBQResult_emit(self, NULL);
}

static PyObject* PyBBQResult_dump(PyBBQResult* self, PyObject* args, PyObject* kwargs) {
    PyBBQNode* root = result_root(self);
    if (!root) return NULL;
    PyObject* out = PyBBQNode_dump(root, args, kwargs);
    Py_DECREF(root);
    return out;
}

static PyMethodDef PyBBQResult_methods[] = {
    {"__dir__", (PyCFunction)PyBBQResult_dir, METH_NOARGS,
     "List attributes including parsed field names."},
    {"emit", (PyCFunction)PyBBQResult_emit, METH_NOARGS,
     "Serialize back to bytes: the dependent fields (array counts, @rest sizes) are "
     "recomputed from what the edits produced, then the input is blitted and what "
     "changed is patched into it. Byte-identical to the input if nothing was changed; "
     "re-parses to the edit if something was."},
    {"__bytes__", (PyCFunction)PyBBQResult_bytes, METH_NOARGS,
     "bytes(result) — the same as emit()."},
    {"deltas", (PyCFunction)PyBBQResult_deltas, METH_NOARGS,
     "What is no longer the input: list of {path, offset, old, new}."},
    {"dump", (PyCFunction)(void(*)(void))PyBBQResult_dump, METH_VARARGS | METH_KEYWORDS,
     "dump(depth=None) -> str: the whole document as a tree."},
    {"_emit",   (PyCFunction)PyBBQResult_emit,   METH_NOARGS, "Serialize back to bytes."},
    {"_deltas", (PyCFunction)PyBBQResult_deltas, METH_NOARGS, "What is no longer the input."},
    {"_dump",   (PyCFunction)(void(*)(void))PyBBQResult_dump, METH_VARARGS | METH_KEYWORDS,
     "dump(depth=None) -> str: the whole document as a tree."},
    {NULL, NULL, 0, NULL}
};

// ── Result iteration, contains, and mapping — all the root node's ──

static PyObject* PyBBQResult_tp_iter(PyBBQResult* self) {
    PyBBQNode* root = result_root(self);
    if (!root) return NULL;
    PyObject* it = make_iter(self, root);
    Py_DECREF(root);
    return it;
}

static int PyBBQResult_sq_contains(PyBBQResult* self, PyObject* value) {
    if (!PyUnicode_Check(value)) return 0;
    const char* key = PyUnicode_AsUTF8(value);
    if (!key) { PyErr_Clear(); return 0; }
    DocLock g(self);
    return child_slot(self->edit->root(), key) >= 0 ? 1 : 0;
}

static PySequenceMethods PyBBQResult_as_sequence = {
    (lenfunc)        NULL,                    // sq_length
    (binaryfunc)     NULL,                    // sq_concat
    (ssizeargfunc)   NULL,                    // sq_repeat
    (ssizeargfunc)   NULL,                    // sq_item
    NULL,                                     // was sq_slice
    (ssizeobjargproc)NULL,                    // sq_ass_item
    NULL,                                     // was sq_ass_slice
    (objobjproc)     PyBBQResult_sq_contains, // sq_contains
    (binaryfunc)     NULL,                    // sq_inplace_concat
    (ssizeargfunc)   NULL,                    // sq_inplace_repeat
};

static Py_ssize_t PyBBQResult_mp_length(PyBBQResult* self) {
    DocLock g(self);
    return (Py_ssize_t)zc::size_of(self->edit->root());
}

static PyObject* PyBBQResult_mp_subscript(PyBBQResult* self, PyObject* key) {
    PyBBQNode* root = result_root(self);
    if (!root) return NULL;
    PyObject* v = PyBBQNode_mp_subscript(root, key);
    Py_DECREF(root);
    return v;
}

static int PyBBQResult_mp_ass_subscript(PyBBQResult* self, PyObject* key, PyObject* value) {
    PyBBQNode* root = result_root(self);
    if (!root) return -1;
    int rc = PyBBQNode_mp_ass_subscript(root, key, value);
    Py_DECREF(root);
    return rc;
}

static PyMappingMethods PyBBQResult_as_mapping = {
    (lenfunc)      PyBBQResult_mp_length,
    (binaryfunc)   PyBBQResult_mp_subscript,
    (objobjargproc)PyBBQResult_mp_ass_subscript,
};

PyTypeObject PyBBQResult_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bbq.ParseResult",                      // tp_name
    sizeof(PyBBQResult),                    // tp_basicsize
    0,                                      // tp_itemsize
    (destructor)PyBBQResult_dealloc,        // tp_dealloc
    0,                                      // tp_vectorcall_offset
    NULL,                                   // tp_getattr
    NULL,                                   // tp_setattr
    NULL,                                   // tp_as_async
    (reprfunc)PyBBQResult_tp_repr,          // tp_repr
    NULL,                                   // tp_as_number
    &PyBBQResult_as_sequence,               // tp_as_sequence
    &PyBBQResult_as_mapping,                // tp_as_mapping
    NULL,                                   // tp_hash
    NULL,                                   // tp_call
    NULL,                                   // tp_str
    (getattrofunc)PyBBQResult_getattro,     // tp_getattro
    (setattrofunc)PyBBQResult_setattro,     // tp_setattro
    NULL,                                   // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    PyDoc_STR("A parsed BBQ document."),    // tp_doc
    (traverseproc)PyBBQResult_traverse,     // tp_traverse
    (inquiry)PyBBQResult_clear,             // tp_clear
    NULL,                                   // tp_richcompare
    0,                                      // tp_weaklistoffset
    (getiterfunc)PyBBQResult_tp_iter,       // tp_iter
    NULL,                                   // tp_iternext
    PyBBQResult_methods,                    // tp_methods
    NULL,                                   // tp_members
    PyBBQResult_getset,                     // tp_getset
};


// ── PyBBQSpec ───────────────────────────────────────────────────────────────

// A Spec holds STRONG references to arbitrary user callables — the externs —
// so it is the one type here that can sit in a cycle the user built: the
// natural way to write a stateful extern is a closure or a bound method that
// reaches the Spec back. Untracked, that cycle is not merely uncollected, it is
// invisible: it never reaches gc.garbage either, and every Spec so registered
// leaks its grammar, its AST and its sema for the life of the process.
static int PyBBQSpec_traverse(PyBBQSpec* self, visitproc visit, void* arg) {
    for (int i = 0; i < self->ext_count; i++) Py_VISIT(self->ext_callables[i]);
    return 0;
}

static int PyBBQSpec_clear(PyBBQSpec* self) {
    for (int i = 0; i < self->ext_count; i++) Py_CLEAR(self->ext_callables[i]);
    // The entries point at the callables that just went; nothing may parse
    // through a spec the collector has decided is unreachable, but the table is
    // emptied rather than left naming freed objects.
    self->ext_count = 0;
    self->ext_table = {};
    return 0;
}

static void PyBBQSpec_dealloc(PyBBQSpec* self) {
    PyObject_GC_UnTrack((PyObject*)self);
    for (int i = 0; i < self->ext_count; i++)
        Py_XDECREF(self->ext_callables[i]);
    PyMem_Free(self->ext_callables);
    PyMem_Free(self->ext_entries);
    delete self->grammar;
    delete self->sema;      // holds a reference to *errors, so it goes first
    delete self->errors;
    delete self->parser;    // owns the AST
    PyObject_GC_Del(self);
}

// ── Extern parser trampoline ──

static bool py_extern_trampoline(
    const uint8_t* data, size_t length,
    size_t* bytes_consumed, ParseArena*, void* user_data)
{
    PyObject* callable = (PyObject*)user_data;

    PyObject* mv = PyMemoryView_FromMemory(
        (char*)data, (Py_ssize_t)length, PyBUF_READ);
    if (!mv) return false;

    PyObject* ret = PyObject_CallOneArg(callable, mv);
    Py_DECREF(mv);

    if (!ret) { PyErr_Clear(); return false; }  // Python exception → fail

    if (ret == Py_None) {
        Py_DECREF(ret);
        return false;                   // None → fail
    }

    Py_ssize_t n = PyLong_AsSsize_t(ret);
    Py_DECREF(ret);

    if (n < 0 && PyErr_Occurred()) { PyErr_Clear(); return false; }
    if (n < 0 || (size_t)n > length) return false;

    *bytes_consumed = (size_t)n;
    return true;
}

static PyObject* PyBBQSpec_register_extern(PyBBQSpec* self, PyObject* args) {
    const char* name;
    PyObject* callable;

    if (!PyArg_ParseTuple(args, "sO", &name, &callable))
        return NULL;

    if (!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "second argument must be callable");
        return NULL;
    }

    // Intern name so pointer-equality lookup works in the VM
    const char* interned = self->grammar->strings.intern(name);

    Py_INCREF(callable);            // the registry's reference, taken before the lock
    PyObject* displaced = nullptr;  // the old callable, released after it
    bool oom = false;

    {
        BBQLock g(&self->lock);

        int slot = -1;                                    // existing entry for this name
        for (int i = 0; i < self->ext_count; i++)
            if (self->ext_entries[i].name == interned) { slot = i; break; }

        if (slot >= 0) {
            displaced = self->ext_callables[slot];
            self->ext_callables[slot] = callable;
            self->ext_entries[slot].user_data = (void*)callable;
        } else {
            if (self->ext_count >= self->ext_capacity) {   // grow
                int new_cap = self->ext_capacity ? self->ext_capacity * 2 : 4;
                auto* new_callables = (PyObject**)PyMem_Realloc(
                    self->ext_callables, sizeof(PyObject*) * new_cap);
                auto* new_entries = new_callables ? (ExternalParserTable::Entry*)PyMem_Realloc(
                    self->ext_entries, sizeof(ExternalParserTable::Entry) * new_cap) : nullptr;
                if (new_callables) self->ext_callables = new_callables;
                if (new_entries)   self->ext_entries = new_entries;
                if (!new_callables || !new_entries) oom = true;
                else self->ext_capacity = new_cap;
            }
            if (!oom) {
                int idx = self->ext_count++;
                self->ext_callables[idx] = callable;
                self->ext_entries[idx] = {interned, py_extern_trampoline, (void*)callable};
                self->ext_table.entries = self->ext_entries;
                self->ext_table.count = self->ext_count;
            }
        }
    }

    // Outside the lock: a DECREF can run a __del__, which could re-enter this module.
    if (oom) { Py_DECREF(callable); return PyErr_NoMemory(); }
    Py_XDECREF(displaced);
    Py_RETURN_NONE;
}

static PyObject* do_parse(PyBBQSpec* self, Py_buffer* view, const char* rule_name) {
    KontNode* entry = nullptr;
    // The RESOLVED rule name (the grammar's own interned copy, so it outlives the call).
    const char* resolved = nullptr;
    if (rule_name) {
        for (int i = 0; i < self->grammar->rule_count; i++)
            if (std::strcmp(self->grammar->rules[i].name, rule_name) == 0) {
                entry = self->grammar->rules[i].entry;
                resolved = self->grammar->rules[i].name;
                break;
            }
        if (!entry) {
            PyErr_Format(PyBBQParseError, "unknown rule: '%s'", rule_name);
            return NULL;
        }
    } else {
        if (self->grammar->rule_count == 0) {
            PyErr_SetString(PyBBQParseError, "grammar has no rules");
            return NULL;
        }
        entry = self->grammar->rules[0].entry;
        resolved = self->grammar->rules[0].name;
    }

    // The extern table, SNAPSHOT under the spec lock rather than pointed at: a concurrent
    // register_extern PyMem_Reallocs both arrays (so `&self->ext_table` would dangle) and
    // can DECREF a callable this parse is about to invoke. The snapshot holds a strong
    // reference to each callable for the parse's duration, so a registration during a
    // running parse does not affect that parse — the only coherent rule under concurrency.
    std::vector<ExternalParserTable::Entry> ext_snapshot;
    std::vector<PyObject*> ext_held;
    {
        BBQLock g(&self->lock);
        ext_snapshot.assign(self->ext_entries, self->ext_entries + self->ext_count);
        ext_held.assign(self->ext_callables, self->ext_callables + self->ext_count);
        for (PyObject* c : ext_held) Py_INCREF(c);
    }
    ExternalParserTable ext_table{ext_snapshot.data(), (int)ext_snapshot.size()};

    ParseArena* arena = new ParseArena();
    CEKMachine machine;
    machine.arena = arena;
    machine.builtins = &self->grammar->builtins;
    if (!ext_snapshot.empty())
        machine.ext_parsers = &ext_table;

    zc::parse_result pr = machine.execute_from(
        entry,
        (const uint8_t*)view->buf,
        (size_t)view->len,
        self->grammar->default_little_endian);

    for (PyObject* c : ext_held) Py_DECREF(c);

    PyBBQResult* result = PyObject_GC_New(PyBBQResult, &PyBBQResult_Type);
    if (!result) {
        delete arena;
        return NULL;
    }
    result->arena = arena;
    // The document, taken as the one transient every node comes from. It is never
    // committed: a caller here edits, asks for bytes, and goes on editing.
    result->edit = new zc::transient(pr.doc.begin_edit());
    result->success = pr.success;
    result->bytes_consumed = pr.bytes_consumed;
    result->error_message = pr.error_message;
    result->error_offset = pr.error_offset;
    result->view = *view;       // transfer buffer ownership
    result->view_valid = true;
    result->rule = resolved;
    result->lock = BBQ_MUTEX_INIT;   // PyObject_GC_New does not zero
    Py_INCREF(self);
    result->spec = self;
    PyObject_GC_Track((PyObject*)result);
    return (PyObject*)result;
}

static PyObject* PyBBQSpec_parse(PyBBQSpec* self, PyObject* args, PyObject* kwargs) {
    PyObject* data_obj;
    const char* rule_name = NULL;
    static const char* kwlist[] = {"data", "rule", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|s",
                                     (char**)kwlist, &data_obj, &rule_name))
        return NULL;

    Py_buffer view;
    if (PyObject_GetBuffer(data_obj, &view, PyBUF_SIMPLE) < 0)
        return NULL;

    PyObject* result = do_parse(self, &view, rule_name);
    if (!result) PyBuffer_Release(&view);
    return result;
}

static PyObject* PyBBQSpec_parse_file(PyBBQSpec* self, PyObject* args, PyObject* kwargs) {
    const char* path;
    const char* rule_name = NULL;
    static const char* kwlist[] = {"path", "rule", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|s",
                                     (char**)kwlist, &path, &rule_name))
        return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        return NULL;
    }

    Py_buffer view;
    memset(&view, 0, sizeof(view));

    if (st.st_size == 0) {
        close(fd);
        PyObject* empty = PyBytes_FromStringAndSize(NULL, 0);
        if (!empty) return NULL;
        if (PyObject_GetBuffer(empty, &view, PyBUF_SIMPLE) < 0) {
            Py_DECREF(empty);
            return NULL;
        }
        Py_DECREF(empty);
    } else {
        // mmap via Python's mmap module — the zero-copy half is the point: the document
        // names spans of THIS mapping, and an unedited emit() is one blit out of it.
        PyObject* mmap_mod = PyImport_ImportModule("mmap");
        if (!mmap_mod) { close(fd); return NULL; }

        PyObject* mmap_cls = PyObject_GetAttrString(mmap_mod, "mmap");
        Py_DECREF(mmap_mod);
        if (!mmap_cls) { close(fd); return NULL; }

        PyObject* mm_args = Py_BuildValue("(in)", fd, (Py_ssize_t)0);
        PyObject* mm_kwargs = Py_BuildValue("{s:i}", "access", 1); // ACCESS_READ
        PyObject* mm = PyObject_Call(mmap_cls, mm_args, mm_kwargs);
        Py_DECREF(mmap_cls);
        Py_DECREF(mm_args);
        Py_DECREF(mm_kwargs);
        close(fd);

        if (!mm) return NULL;

        if (PyObject_GetBuffer(mm, &view, PyBUF_SIMPLE) < 0) {
            Py_DECREF(mm);
            return NULL;
        }
        Py_DECREF(mm);  // view.obj holds the mmap ref
    }

    PyObject* result = do_parse(self, &view, rule_name);
    if (!result) PyBuffer_Release(&view);
    return result;
}

// ── Spec properties ──

static PyObject* PyBBQSpec_get_rules(PyBBQSpec* self, void*) {
    PyObject* list = PyList_New(0);
    if (!list) return NULL;
    for (int i = 0; i < self->grammar->rule_count; i++) {
        PyObject* s = PyUnicode_FromString(self->grammar->rules[i].name);
        if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); Py_DECREF(list); return NULL; }
        Py_DECREF(s);
    }
    return list;
}

static PyObject* PyBBQSpec_get_default_endian(PyBBQSpec* self, void*) {
    return PyUnicode_FromString(self->grammar->default_little_endian ? "little" : "big");
}

static PyGetSetDef PyBBQSpec_getset[] = {
    {(char*)"rules",          (getter)PyBBQSpec_get_rules,          NULL,
     (char*)"list of rule names", NULL},
    {(char*)"default_endian", (getter)PyBBQSpec_get_default_endian, NULL,
     (char*)"'little' or 'big'", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMethodDef PyBBQSpec_methods[] = {
    {"parse", (PyCFunction)(void(*)(void))PyBBQSpec_parse, METH_VARARGS | METH_KEYWORDS,
     "parse(data, rule=None) -> ParseResult"},
    {"parse_file", (PyCFunction)(void(*)(void))PyBBQSpec_parse_file,
     METH_VARARGS | METH_KEYWORDS,
     "parse_file(path, rule=None) -> ParseResult"},
    {"register_extern", (PyCFunction)PyBBQSpec_register_extern, METH_VARARGS,
     "register_extern(name, callable): supply an external parser for `name`."},
    {NULL, NULL, 0, NULL}
};

PyTypeObject PyBBQSpec_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bbq.Spec",                             // tp_name
    sizeof(PyBBQSpec),                      // tp_basicsize
    0,                                      // tp_itemsize
    (destructor)PyBBQSpec_dealloc,          // tp_dealloc
    0,                                      // tp_vectorcall_offset
    NULL,                                   // tp_getattr
    NULL,                                   // tp_setattr
    NULL,                                   // tp_as_async
    NULL,                                   // tp_repr
    NULL,                                   // tp_as_number
    NULL,                                   // tp_as_sequence
    NULL,                                   // tp_as_mapping
    NULL,                                   // tp_hash
    NULL,                                   // tp_call
    NULL,                                   // tp_str
    NULL,                                   // tp_getattro
    NULL,                                   // tp_setattro
    NULL,                                   // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    PyDoc_STR("A compiled BBQ grammar."),   // tp_doc
    (traverseproc)PyBBQSpec_traverse,       // tp_traverse
    (inquiry)PyBBQSpec_clear,               // tp_clear
    NULL,                                   // tp_richcompare
    0,                                      // tp_weaklistoffset
    NULL,                                   // tp_iter
    NULL,                                   // tp_iternext
    PyBBQSpec_methods,                      // tp_methods
    NULL,                                   // tp_members
    PyBBQSpec_getset,                       // tp_getset
};


// ── Module functions ────────────────────────────────────────────────────────

static PyObject* compile_source(const char* source, Py_ssize_t length) {
    // The Parser owns the AST and the Sema its resolved facts; the compiled grammar
    // points into what they own, so both outlive this call (the Spec frees them).
    Parser* parser = new Parser();
    parser->init(source, (int)length);
    if (!parser->parse()) {
        PyErr_Format(PyBBQParseError, "parse error at line %d, col %d",
                     parser->line(), parser->col());
        delete parser;
        return NULL;
    }

    bbqgen::ErrorReporter* errors = new bbqgen::ErrorReporter();
    bbqgen::Sema* sema = new bbqgen::Sema(*errors);
    if (!sema->analyze(parser->ast)) {
        std::ostringstream oss;
        errors->print_all(oss);
        PyErr_SetString(PyBBQParseError, oss.str().c_str());
        delete sema; delete errors; delete parser;
        return NULL;
    }

    ::bbq::Compiler compiler;
    CompiledGrammar* grammar = compiler.compile_grammar(parser->ast);
    if (!grammar) {
        PyErr_SetString(PyBBQParseError, "compilation failed");
        delete sema; delete errors; delete parser;
        return NULL;
    }

    PyBBQSpec* spec = PyObject_GC_New(PyBBQSpec, &PyBBQSpec_Type);
    if (!spec) { delete grammar; delete sema; delete errors; delete parser; return NULL; }
    spec->grammar = grammar;
    spec->parser = parser;
    spec->errors = errors;
    spec->sema = sema;
    spec->ext_callables = nullptr;
    spec->ext_entries = nullptr;
    spec->ext_count = 0;
    spec->ext_capacity = 0;
    spec->ext_table = {};
    spec->lock = BBQ_MUTEX_INIT;    // PyObject_GC_New does not zero
    // Tracked only now: the collector may walk this object the moment it is
    // tracked, and tp_traverse reads the fields above.
    PyObject_GC_Track(spec);
    return (PyObject*)spec;
}

static PyObject* bbq_compile(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
    const char* path;
    static const char* kwlist[] = {"path", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s", (char**)kwlist, &path))
        return NULL;

    FILE* f = fopen(path, "rb");
    if (!f) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        return NULL;
    }
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);

    return compile_source(text.data(), (Py_ssize_t)text.size());
}

static PyObject* bbq_compile_string(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
    const char* source;
    Py_ssize_t length;
    static const char* kwlist[] = {"source", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#", (char**)kwlist, &source, &length))
        return NULL;
    return compile_source(source, length);
}

static PyMethodDef bbq_module_methods[] = {
    {"compile", (PyCFunction)(void(*)(void))bbq_compile, METH_VARARGS | METH_KEYWORDS,
     "compile(path) -> Spec: compile a .bbq grammar file."},
    {"compile_string", (PyCFunction)(void(*)(void))bbq_compile_string,
     METH_VARARGS | METH_KEYWORDS,
     "compile_string(source) -> Spec: compile a grammar from a string."},
    {"count",  (PyCFunction)bbq_count,  METH_O,
     "count(path): how many nodes the path matched — a value for a comparison."},
    {"length", (PyCFunction)bbq_length, METH_O,
     "length(path): len() of the one node the path matched."},
    {NULL, NULL, 0, NULL}
};


// ── Module init ─────────────────────────────────────────────────────────────

static struct PyModuleDef bbq_moduledef = {
    PyModuleDef_HEAD_INIT,
    "bbq",
    PyDoc_STR("Binary format parsing with the BBQ CEK machine."),
    -1,
    bbq_module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC
PyInit_bbq(void)
{
    PyObject* m;

    m = PyModule_Create(&bbq_moduledef);
    if (m == NULL) {
        return NULL;
    }

    /* Free-threaded builds: this module serializes its own shared state (the Spec's
     * extern registry, each Result's document — see the locking note near the top), so
     * it does not need the interpreter to do it. Without this declaration importing bbq
     * would re-enable the GIL for the whole process. PyUnstable_Module_SetGIL is the
     * single-phase-init spelling and exists only in the free-threaded build. */
#ifdef Py_GIL_DISABLED
    PyUnstable_Module_SetGIL(m, Py_MOD_GIL_NOT_USED);
#endif

    /* Register the 'bbq.Spec' class */

    if (PyType_Ready(&PyBBQSpec_Type)) {
        return NULL;
    }

    PyModule_AddObject(m, (char*) "Spec", (PyObject*) &PyBBQSpec_Type);

    /* Register the 'bbq.ParseResult' class */

    if (PyType_Ready(&PyBBQResult_Type)) {
        return NULL;
    }

    PyModule_AddObject(m, (char*) "ParseResult", (PyObject*) &PyBBQResult_Type);

    /* Register the 'bbq.Node' class */

    if (PyType_Ready(&PyBBQNode_Type)) {
        return NULL;
    }

    PyModule_AddObject(m, (char*) "Node", (PyObject*) &PyBBQNode_Type);

    /* Register the 'bbq.NodeIter' class */

    if (PyType_Ready(&PyBBQNodeIter_Type)) {
        return NULL;
    }

    PyModule_AddObject(m, (char*) "NodeIter", (PyObject*) &PyBBQNodeIter_Type);

    if (PyType_Ready(&PyBBQNodeList_Type)) {
        return NULL;
    }
    Py_INCREF(&PyBBQNodeList_Type);
    PyModule_AddObject(m, (char*) "NodeList", (PyObject*) &PyBBQNodeList_Type);

    if (PyType_Ready(&PyBBQQuery_Type)) {
        return NULL;
    }
    Py_INCREF(&PyBBQQuery_Type);
    PyModule_AddObject(m, (char*) "Query", (PyObject*) &PyBBQQuery_Type);
    // `this` is `@`: the node a filter is being asked about. One shared empty
    // path — every navigation from it builds a new one, so it is never mutated.
    {
        PyObject* self_q = query_new(Q_PATH);
        if (!self_q) return NULL;
        PyModule_AddObject(m, (char*) "this", self_q);
    }

    /* Register the 'bbq.ParseError' exception */

    if (!(PyBBQParseError = (PyObject*) PyErr_NewException((char*) "bbq.ParseError", NULL, NULL))) {
        return NULL;
    }

    Py_INCREF(PyBBQParseError);
    PyModule_AddObject(m, (char*) "ParseError", PyBBQParseError);

    /* Attach the 'bbq.build' construction submodule */

    PyObject* build_mod = bbq_build_create_module();
    if (!build_mod) {
        return NULL;
    }
    if (PyModule_AddObject(m, (char*) "build", build_mod) < 0) {
        Py_DECREF(build_mod);
        return NULL;
    }

    return m;
}
