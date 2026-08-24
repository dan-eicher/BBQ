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

static PyBBQNode* PyBBQNode_New(PyBBQResult* result, zc::node* parent, Py_ssize_t slot);


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
};

struct PyBBQNodeIter {
    PyObject_HEAD
    PyBBQResult* result;
    zc::node* container;   // owned, so it stays put while the iteration runs
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
        case CaptureType::UInt8:    case CaptureType::UInt16LE: case CaptureType::UInt16BE:
        case CaptureType::UInt32LE: case CaptureType::UInt32BE:
        case CaptureType::UInt64LE: case CaptureType::UInt64BE:
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
    enum class Kind { Int, Float, Bytes } kind;
    int64_t i = 0;
    double f = 0;
    std::vector<uint8_t> b;
    bool as_str = false;
};

static bool convert_for(CaptureType t, PyObject* value, PendingWrite* out) {
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
    int64_t v = PyLong_AsLongLong(value);   // integer leaves, and Computed (leb/compute)
    if (v == -1 && PyErr_Occurred()) return false;
    out->kind = PendingWrite::Kind::Int; out->i = v;
    return true;
}

// Content supplied as BYTES: bytes, str, or a bbq.build value (which is a byte
// constructor and enters as what it serializes to). This is the form a composite or
// variable-width element takes — ZCow is byte-level, so a shape the grammar is not
// describing arrives as its bytes. Returns 1 = took it, 0 = not that kind of value,
// -1 = error. Runs Python, so no lock may be held.
static int as_byte_content(PyObject* value, std::vector<uint8_t>* out) {
    if (bbq_build_is_value(value))
        return bbq_build_serialize(value, *out) ? 1 : -1;
    if (PyBytes_Check(value)) {
        const char* p = PyBytes_AS_STRING(value);
        out->assign(p, p + PyBytes_GET_SIZE(value));
        return 1;
    }
    if (PyUnicode_Check(value)) {
        Py_ssize_t n = 0;
        const char* s = PyUnicode_AsUTF8AndSize(value, &n);
        if (!s) return -1;
        out->assign(s, s + n);
        return 1;
    }
    return 0;
}

static void apply_write(zc::node* n, const PendingWrite& w) {
    switch (w.kind) {
        case PendingWrite::Kind::Int:   zc::set_int(n, w.i, n->enc); break;
        case PendingWrite::Kind::Float: zc::set_float(n, w.f); break;
        case PendingWrite::Kind::Bytes:
            if (w.as_str) zc::set_str(n, std::string_view((const char*)w.b.data(), w.b.size()));
            else          zc::set_bytes(n, w.b.data(), w.b.size());
            break;
    }
}

// Write `value` into the child of `container` named `key` (or at `index`). This is the
// generated `set_x`: own the child, then one ZCow setter.
static int write_child(PyBBQResult* result, PyBBQNode* holder,
                       const char* key, Py_ssize_t index, PyObject* value) {
    CaptureType t;
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
        t = c->kids[(size_t)slot]->type;
    }

    PendingWrite w;
    if (!convert_for(t, value, &w)) return -1;   // may run Python — no lock held

    DocLock g(result);
    zc::node* c = owned_node_of(holder);
    zc::node* child = result->edit->own_child(c, (size_t)slot);
    if (!child) { PyErr_SetString(PyExc_RuntimeError, "field vanished"); return -1; }
    apply_write(child, w);
    return 0;
}


// ── PyBBQNode ───────────────────────────────────────────────────────────────

static PyBBQNode* PyBBQNode_New(PyBBQResult* result, zc::node* parent, Py_ssize_t slot) {
    PyBBQNode* node = PyObject_GC_New(PyBBQNode, &PyBBQNode_Type);
    if (!node) return NULL;
    node->parent = parent;
    node->slot = slot;
    Py_INCREF(result);
    node->result = result;
    PyObject_GC_Track((PyObject*)node);
    return node;
}

static void PyBBQNode_dealloc(PyBBQNode* self) {
    PyObject_GC_UnTrack((PyObject*)self);
    Py_XDECREF(self->result);
    PyObject_GC_Del(self);
}

static int PyBBQNode_traverse(PyBBQNode* self, visitproc visit, void* arg) {
    Py_VISIT(self->result);
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
    return (PyObject*)PyBBQNode_New(result, owned, slot);
}

static PyObject* PyBBQNode_getattro(PyBBQNode* self, PyObject* name) {
    // Try standard attributes first (methods, properties)
    PyObject* attr = PyObject_GenericGetAttr((PyObject*)self, name);
    if (attr) return attr;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return NULL;

    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return NULL;

    Py_ssize_t slot;
    { DocLock g(self->result); slot = child_slot(node_of(self), key); }
    if (slot < 0) return NULL;   // keep AttributeError

    PyErr_Clear();
    return child_node(self->result, self, slot);
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
    if (!is_container(ct)) {
        PyErr_Format(PyExc_TypeError, "'bbq.Node' (%s) is not subscriptable",
                     capture_type_name(ct));
        return NULL;
    }

    if (PySlice_Check(key)) {
        Py_ssize_t start, stop, step, length;
        if (PySlice_GetIndicesEx(key, count, &start, &stop, &step, &length) < 0)
            return NULL;
        PyObject* list = PyList_New(length);
        if (!list) return NULL;
        for (Py_ssize_t i = 0, idx = start; i < length; i++, idx += step) {
            PyObject* node = child_node(self->result, self, idx);
            if (!node) { Py_DECREF(list); return NULL; }
            PyList_SET_ITEM(list, i, node);
        }
        return list;
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
        std::vector<uint8_t> raw;
        int took = as_byte_content(value, &raw);   // may run Python — no lock held
        if (took < 0) return -1;
        if (!took) {
            PyErr_SetString(PyExc_TypeError,
                "a composite element is replaced by bytes (or a bbq.build value)");
            return -1;
        }
        DocLock g(self->result);
        zc::node* c = owned_node_of(self);
        zc::node* e = self->result->edit->own_child(c, (size_t)index);
        if (!e || !self->result->edit->splice(e, raw.data(), raw.size())) {
            PyErr_SetString(PyExc_RuntimeError, "splice failed");
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
        if (!k->name) continue;
        PyObject* s = PyUnicode_FromString(k->name);
        if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); return -1; }
        Py_DECREF(s);
    }
    return 0;
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
    PyObject* list = PyList_New(0);
    if (!list) return NULL;
    DocLock g(self->result);
    if (append_child_names(list, node_of(self)) < 0) { Py_DECREF(list); return NULL; }
    return list;
}

// The named children, as nodes. Slots are collected under the lock and turned into
// Python objects after it — child_node takes the lock itself.
static bool named_slots(PyBBQResult* result, PyBBQNode* holder,
                        std::vector<std::pair<Py_ssize_t, std::string>>* out) {
    DocLock g(result);
    const zc::node* c = node_of(holder);
    if (!c) return false;
    for (size_t i = 0; i < c->kids.size(); i++)
        if (c->kids[i]->name) out->emplace_back((Py_ssize_t)i, c->kids[i]->name);
    return true;
}

static PyObject* PyBBQNode_values(PyBBQNode* self, PyObject*) {
    std::vector<std::pair<Py_ssize_t, std::string>> slots;
    if (!named_slots(self->result, self, &slots)) return PyList_New(0);
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
    std::vector<std::pair<Py_ssize_t, std::string>> slots;
    if (!named_slots(self->result, self, &slots)) return PyList_New(0);
    PyObject* list = PyList_New(0);
    if (!list) return NULL;
    for (const auto& s : slots) {
        PyObject* name = PyUnicode_FromString(s.second.c_str());
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

    std::vector<uint8_t> raw;
    int took = as_byte_content(value, &raw);   // may run Python — no lock held
    if (took < 0) return NULL;
    if (took) {
        DocLock g(self->result);
        zc::node* c = owned_node_of(self);
        zc::node* e = self->result->edit->append(c, CaptureType::Bytes);
        if (!e) { PyErr_SetString(PyExc_RuntimeError, "append failed"); return NULL; }
        zc::set_bytes(e, raw.data(), raw.size());
        Py_RETURN_NONE;
    }

    CaptureType elem;
    {
        DocLock g(self->result);
        const zc::node* n = node_of(self);
        if (!n || n->kids.empty()) {
            PyErr_SetString(PyExc_TypeError,
                "append: an empty array has no element type to take — append bytes "
                "(or a bbq.build value), or give the array an element first");
            return NULL;
        }
        elem = n->kids[0]->type;
    }

    PendingWrite w;
    if (!convert_for(elem, value, &w)) return NULL;   // may run Python — no lock held

    DocLock g(self->result);
    zc::node* c = owned_node_of(self);
    zc::node* e = self->result->edit->append(c, elem);
    if (!e) { PyErr_SetString(PyExc_RuntimeError, "append failed"); return NULL; }
    apply_write(e, w);
    Py_RETURN_NONE;
}

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

static PyGetSetDef PyBBQNode_getset[] = {
    {(char*)"offset",       (getter)PyBBQNode_get_offset,       NULL,
     (char*)"(start, end) byte offset tuple", NULL},
    {(char*)"raw",          (getter)PyBBQNode_get_raw,          NULL,
     (char*)"this node's bytes as they stand", NULL},
    {(char*)"capture_type", (getter)PyBBQNode_get_capture_type, NULL,
     (char*)"capture type name string", NULL},
    {(char*)"name",         (getter)PyBBQNode_get_name,         NULL,
     (char*)"field name or None", NULL},
    {(char*)"value",        (getter)PyBBQNode_get_value,        NULL,
     (char*)"auto-materialized Python value", NULL},
    {(char*)"variant_tag",  (getter)PyBBQNode_get_variant_tag,  NULL,
     (char*)"union/switch arm ordinal, or None if not a variant", NULL},
    {(char*)"parsed",       (getter)PyBBQNode_get_parsed,       NULL,
     (char*)"True while this node still names bytes of the input", NULL},
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
    PyObject_GC_Del(self);
}

static int PyBBQNodeIter_traverse(PyBBQNodeIter* self, visitproc visit, void* arg) {
    Py_VISIT(self->result);
    return 0;
}

static int PyBBQNodeIter_clear(PyBBQNodeIter* self) {
    Py_CLEAR(self->result);
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

    PyBBQNode* node = PyBBQNode_New(self->result, self->container, i);
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
    return PyBBQNode_New(self, nullptr, -1);
}

static PyObject* PyBBQResult_getattro(PyBBQResult* self, PyObject* name) {
    PyObject* attr = PyObject_GenericGetAttr((PyObject*)self, name);
    if (attr) return attr;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return NULL;

    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return NULL;

    Py_ssize_t slot;
    { DocLock g(self); slot = child_slot(self->edit->root(), key); }
    if (slot < 0) return NULL;   // keep AttributeError

    PyErr_Clear();
    PyBBQNode* root = PyBBQNode_New(self, nullptr, -1);
    if (!root) return NULL;
    PyObject* child = child_node(self, root, slot);
    Py_DECREF(root);
    return child;
}

static int PyBBQResult_setattro(PyBBQResult* self, PyObject* name, PyObject* value) {
    if (!value) { PyErr_SetString(PyExc_TypeError, "cannot delete a BBQ field"); return -1; }
    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return -1;
    { DocLock g(self);
      if (!self->edit->root()) {
          PyErr_SetString(PyExc_AttributeError, "no parse tree"); return -1;
      } }
    PyBBQNode* root = PyBBQNode_New(self, nullptr, -1);
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
    return (PyObject*)PyBBQNode_New(self, nullptr, -1);
}

static PyGetSetDef PyBBQResult_getset[] = {
    {(char*)"success",        (getter)PyBBQResult_get_success,        NULL,
     (char*)"whether the parse succeeded", NULL},
    {(char*)"bytes_consumed", (getter)PyBBQResult_get_bytes_consumed, NULL,
     (char*)"how many bytes the parse consumed", NULL},
    {(char*)"error_message",  (getter)PyBBQResult_get_error_message,  NULL,
     (char*)"failure message, or None", NULL},
    {(char*)"error_offset",   (getter)PyBBQResult_get_error_offset,   NULL,
     (char*)"offset the parse gave up at", NULL},
    {(char*)"root",           (getter)PyBBQResult_get_root,           NULL,
     (char*)"the root node", NULL},
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
        for (size_t i = 0; i < n->kids.size(); i++) {
            const zc::node& k = *n->kids[i];
            const char* nm = k.name;
            collect_deltas(self, &k,
                (nm && *nm) ? (path.empty() ? std::string(nm) : path + "." + nm)
                            : path + "[" + std::to_string(i) + "]", out);
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
        collect_deltas(self, self->edit->root(), std::string(), &hits);
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

static void PyBBQSpec_dealloc(PyBBQSpec* self) {
    for (int i = 0; i < self->ext_count; i++)
        Py_XDECREF(self->ext_callables[i]);
    PyMem_Free(self->ext_callables);
    PyMem_Free(self->ext_entries);
    delete self->grammar;
    delete self->sema;      // holds a reference to *errors, so it goes first
    delete self->errors;
    delete self->parser;    // owns the AST
    Py_TYPE(self)->tp_free((PyObject*)self);
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
    Py_TPFLAGS_DEFAULT,                     // tp_flags
    PyDoc_STR("A compiled BBQ grammar."),   // tp_doc
    NULL,                                   // tp_traverse
    NULL,                                   // tp_clear
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

    PyBBQSpec* spec = PyObject_New(PyBBQSpec, &PyBBQSpec_Type);
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
    spec->lock = BBQ_MUTEX_INIT;    // PyObject_New does not zero
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
