// bbq_python.cpp — CPython C API extension module for the BBQ CEK VM
//
// Single-file module, no headers. All types are file-scope statics.
// Follows the patterns established in ptree-python.

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstring>
#include <cstdio>
#include <string>
#include <sstream>

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
#include "CaptureDecode.h"
#include "CaptureCow.h"

// The WRITE half: emit() is the lens's `put`, so it runs the grammar-enforcement walk
// (dependent fields recomputed into the overlay) before ZCow serializes. The op-list is
// the shared writer lowering; WriterInterp executes it, as the generated C++ writer's
// stencils render it.
#include "CompilerCtx.h"
#include "RenderEmit.h"
#include "WriterInterp.h"

// bbq.build — the grammar-free byte-construction submodule (its own translation unit)
#include "bbq_build.h"

using namespace bbq;        // index runtime: FieldCapture, CaptureType, decoders, zcow
using namespace bbq::cek;   // machine IR: Value, FieldCaptureValue, ...


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

static PyBBQNode* PyBBQNode_New(const FieldCapture* cap, PyBBQResult* result);


// ── Structs ─────────────────────────────────────────────────────────────────

struct PyBBQSpec {
    PyObject_HEAD
    CompiledGrammar* grammar;
    // The frontend artifacts, kept alive for the WRITE side: the writer lowering reads the
    // AST (which the Parser owns) alongside the kont IR. Reads need none of this.
    Parser* parser;
    bbqgen::ErrorReporter* errors;
    bbqgen::Sema* sema;
    // The writer op-list, lowered on the first emit() and cached (see spec_wops). Lowering
    // it costs ~7x what compiling the grammar does — an 87-rule spec is 15 ms to compile and
    // 100 ms to lower — and a session that only reads never needs it. The module never
    // releases the GIL, so this cache cannot be raced; `parse` still touches none of it.
    nlohmann::json* wops;

    // Extern parser support
    PyObject** ext_callables;               // Array of strong refs (INCREFed)
    ExternalParserTable::Entry* ext_entries; // Parallel array
    int ext_count;
    int ext_capacity;
    ExternalParserTable ext_table;
};

struct PyBBQResult {
    PyObject_HEAD
    ParseArena* arena;
    CaptureMetadata meta;
    Py_buffer view;
    PyBBQSpec* spec;
    bool view_valid;
    bbq::zcow* zcow;   // copy-on-write overlay, lazily created on first mutation
    const char* rule;  // the rule this was parsed with — emit() enforces THAT rule's grammar
};

struct PyBBQNode {
    PyObject_HEAD
    const FieldCapture* capture;
    PyBBQResult* result;
};

struct PyBBQNodeIter {
    PyObject_HEAD
    PyBBQResult* result;
    const FieldCapture* children;
    int count;
    int index;
    bool yield_tuples;  // true for struct → (name, node), false for array → node
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

// Thin Python wrappers over the shared Borrowed-leaf decoders
// (backends/cpp/runtime/CaptureDecode.h) — the offset→value logic lives there, shared
// with the compiled C++ face; here we only widen to a Python object / raise.
static PyObject* decode_int(const FieldCapture* cap, const uint8_t* data) {
    int64_t bits; bool is_signed;
    if (!bbq::decode_int(cap, data, &bits, &is_signed)) {
        if (cap->type == CaptureType::Computed)
            PyErr_SetString(PyExc_TypeError, "computed value is not numeric");
        else
            PyErr_Format(PyExc_TypeError, "cannot convert %s to int",
                         capture_type_name(cap->type));
        return NULL;
    }
    return is_signed ? PyLong_FromLongLong(bits)
                     : PyLong_FromUnsignedLongLong((uint64_t)bits);
}

static PyObject* decode_float(const FieldCapture* cap, const uint8_t* data) {
    double d;
    if (!bbq::decode_float(cap, data, &d)) {
        PyErr_Format(PyExc_TypeError, "cannot convert %s to float",
                     capture_type_name(cap->type));
        return NULL;
    }
    return PyFloat_FromDouble(d);
}

// A ZCow integer override for `cap` as a PyLong, or null if the field is
// unmutated. The dynamic face of the same overlay the C++ handles use.
static PyObject* zcow_int_override(const FieldCapture* cap, PyBBQResult* result) {
    if (result->zcow)
        if (auto* o = result->zcow->get_int(cap))
            return PyLong_FromLongLong(*o);
    return nullptr;
}

// Write `value` into leaf node `cap` via the ZCow overlay, dispatched on the
// field's grammar type so every leaf kind round-trips (not just integers).
static int zcow_set_capture(const FieldCapture* cap, PyBBQResult* result, PyObject* value) {
    if (!result->zcow) result->zcow = new bbq::zcow();
    CaptureType t = cap->type;
    if (is_float_type(t)) {
        double d = PyFloat_AsDouble(value);
        if (d == -1.0 && PyErr_Occurred()) return -1;
        result->zcow->set_float(cap, d);
        return 0;
    }
    if (t == CaptureType::Bytes || t == CaptureType::External) {
        if (!PyBytes_Check(value)) {
            PyErr_SetString(PyExc_TypeError, "expected bytes for a bytes field"); return -1;
        }
        const char* p = PyBytes_AS_STRING(value);
        result->zcow->set_bytes(cap, std::vector<uint8_t>(p, p + PyBytes_GET_SIZE(value)));
        return 0;
    }
    if (t == CaptureType::String) {
        if (PyBytes_Check(value)) {
            const char* p = PyBytes_AS_STRING(value);
            result->zcow->set_bytes(cap, std::vector<uint8_t>(p, p + PyBytes_GET_SIZE(value)));
        } else {
            Py_ssize_t n = 0;
            const char* s = PyUnicode_AsUTF8AndSize(value, &n);
            if (!s) return -1;
            result->zcow->set_bytes(cap, std::vector<uint8_t>(s, s + n));
        }
        return 0;
    }
    if (t == CaptureType::Bool) {
        int b = PyObject_IsTrue(value);
        if (b < 0) return -1;
        result->zcow->set_int(cap, b ? 1 : 0);
        return 0;
    }
    if (t == CaptureType::Struct || t == CaptureType::Array || t == CaptureType::Computed) {
        PyErr_Format(PyExc_TypeError, "cannot assign directly to a %s field", capture_type_name(t));
        return -1;
    }
    int64_t v = PyLong_AsLongLong(value);   // integer leaf types
    if (v == -1 && PyErr_Occurred()) return -1;
    result->zcow->set_int(cap, v);
    return 0;
}

// Set struct field `key` of `cap` via the ZCow overlay (any leaf type).
static int zcow_set_field(const FieldCapture* cap, PyBBQResult* result,
                          const char* key, PyObject* value) {
    for (int i = 0; i < cap->child_count; i++) {
        if (cap->children[i].name && strcmp(cap->children[i].name, key) == 0)
            return zcow_set_capture(&cap->children[i], result, value);
    }
    PyErr_Format(PyExc_AttributeError, "no field '%s'", key);
    return -1;
}

// Build a raw znode from a Python value for append/splice — ZCow is byte-level, so
// there is no grammar typing here. bytes/str become a Bytes node (verbatim); a number
// becomes a fixed-width Scalar at `type_src`'s recorded type (an existing sibling
// element, pure data). Variable-width (LEB/computed), spans, and composite shapes have
// no by-value form — supply bytes for those. Returns null + sets an error otherwise.
static std::unique_ptr<bbq::znode> znode_from_pyvalue(PyObject* value, const FieldCapture* type_src) {
    auto z = std::make_unique<bbq::znode>();
    if (bbq_build_is_value(value)) {   // a bbq.build object enters as its serialized bytes
        std::vector<uint8_t> out;
        if (!bbq_build_serialize(value, out)) return nullptr;
        z->kind = bbq::znode::Kind::Bytes;
        z->bval = std::move(out);
        return z;
    }
    if (PyBytes_Check(value)) {
        const char* p = PyBytes_AS_STRING(value);
        z->kind = bbq::znode::Kind::Bytes;
        z->bval.assign(p, p + PyBytes_GET_SIZE(value));
        return z;
    }
    if (PyUnicode_Check(value)) {
        Py_ssize_t n = 0; const char* s = PyUnicode_AsUTF8AndSize(value, &n);
        if (!s) return nullptr;
        z->kind = bbq::znode::Kind::Bytes;
        z->bval.assign(s, s + n);
        return z;
    }
    CaptureType t = type_src ? type_src->type : CaptureType::Struct;
    if (!is_float_type(t) && t != CaptureType::Bool && leaf_width(t) == 0) {
        PyErr_SetString(PyExc_TypeError,
            "append/splice: a non-bytes value needs an existing fixed-width element to "
            "take its type from; supply bytes for a composite or variable-width element");
        return nullptr;
    }
    z->kind = bbq::znode::Kind::Scalar;
    z->type = t;
    if (is_float_type(t)) {
        double d = PyFloat_AsDouble(value);
        if (d == -1.0 && PyErr_Occurred()) return nullptr;
        z->fval = d;
    } else if (t == CaptureType::Bool) {
        int b = PyObject_IsTrue(value); if (b < 0) return nullptr;
        z->ival = b ? 1 : 0;
    } else {
        int64_t v = PyLong_AsLongLong(value);
        if (v == -1 && PyErr_Occurred()) return nullptr;
        z->ival = v;
    }
    return z;
}

static PyObject* decode_auto_value(const FieldCapture* cap, PyBBQResult* result) {
    if (PyObject* ov = zcow_int_override(cap, result)) return ov;
    const uint8_t* data = (const uint8_t*)result->view.buf;

    switch (cap->type) {
        case CaptureType::UInt8:    case CaptureType::UInt16LE: case CaptureType::UInt16BE:
        case CaptureType::UInt32LE: case CaptureType::UInt32BE:
        case CaptureType::UInt64LE: case CaptureType::UInt64BE:
        case CaptureType::Int8:     case CaptureType::Int16LE:  case CaptureType::Int16BE:
        case CaptureType::Int32LE:  case CaptureType::Int32BE:
        case CaptureType::Int64LE:  case CaptureType::Int64BE:
            return decode_int(cap, data);

        case CaptureType::Computed: {
            // A Computed leaf carries a typed value (compute(...)/leb/bitfield). Project
            // it to the matching Python type — not always int.
            auto* cv = cap->computed_value;
            if (!cv) return PyLong_FromLongLong(0);
            switch (cv->kind) {
                case bbq::ComputedValue::Kind::Int:    return PyLong_FromLongLong(cv->i);
                case bbq::ComputedValue::Kind::Bool:   return PyBool_FromLong(cv->b ? 1 : 0);
                case bbq::ComputedValue::Kind::Float:  return PyFloat_FromDouble(cv->f);
                case bbq::ComputedValue::Kind::String: return PyUnicode_FromString(cv->s ? cv->s : "");
            }
            return PyLong_FromLongLong(0);
        }

        case CaptureType::Float32LE: case CaptureType::Float32BE:
        case CaptureType::Float64LE: case CaptureType::Float64BE:
            return decode_float(cap, data);

        case CaptureType::Bool:
            return PyBool_FromLong(data[cap->start_offset] ? 1 : 0);

        case CaptureType::String: {
            size_t len = cap->end_offset - cap->start_offset;
            return PyUnicode_DecodeUTF8(
                (const char*)(data + cap->start_offset), (Py_ssize_t)len, NULL);
        }

        case CaptureType::Bytes:
        case CaptureType::External: {
            size_t len = cap->end_offset - cap->start_offset;
            return PyBytes_FromStringAndSize(
                (const char*)(data + cap->start_offset), (Py_ssize_t)len);
        }

        case CaptureType::Struct:
        case CaptureType::Array:
            return (PyObject*)PyBBQNode_New(cap, result);
    }
    Py_RETURN_NONE;
}


// ── PyBBQNode ───────────────────────────────────────────────────────────────

static PyBBQNode* PyBBQNode_New(const FieldCapture* cap, PyBBQResult* result) {
    PyBBQNode* node = PyObject_GC_New(PyBBQNode, &PyBBQNode_Type);
    if (!node) return NULL;
    node->capture = cap;
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

static PyObject* PyBBQNode_getattro(PyBBQNode* self, PyObject* name) {
    // Try standard attributes first (methods, properties)
    PyObject* attr = PyObject_GenericGetAttr((PyObject*)self, name);
    if (attr) return attr;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return NULL;

    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return NULL;

    // Linear scan — Python strings aren't interned in the same pool, use strcmp
    for (int i = 0; i < self->capture->child_count; i++) {
        if (self->capture->children[i].name &&
            strcmp(self->capture->children[i].name, key) == 0) {
            PyErr_Clear();
            return (PyObject*)PyBBQNode_New(&self->capture->children[i], self->result);
        }
    }
    return NULL;  // keep AttributeError
}

// `node.field = v` → detach that field to Owned in the ZCow overlay.
static int PyBBQNode_setattro(PyBBQNode* self, PyObject* name, PyObject* value) {
    if (!value) { PyErr_SetString(PyExc_TypeError, "cannot delete a BBQ field"); return -1; }
    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return -1;
    return zcow_set_field(self->capture, self->result, key, value);
}

// ── Number protocol ──

static PyObject* PyBBQNode_nb_int(PyBBQNode* self) {
    if (PyObject* ov = zcow_int_override(self->capture, self->result)) return ov;
    return decode_int(self->capture, (const uint8_t*)self->result->view.buf);
}

static PyObject* PyBBQNode_nb_float(PyBBQNode* self) {
    const uint8_t* data = (const uint8_t*)self->result->view.buf;
    CaptureType type = self->capture->type;

    // Native float types
    if (type == CaptureType::Float32LE || type == CaptureType::Float32BE ||
        type == CaptureType::Float64LE || type == CaptureType::Float64BE)
        return decode_float(self->capture, data);

    // Integer promotion
    PyObject* ival = decode_int(self->capture, data);
    if (!ival) return NULL;
    double d = PyLong_AsDouble(ival);
    Py_DECREF(ival);
    if (d == -1.0 && PyErr_Occurred()) return NULL;
    return PyFloat_FromDouble(d);
}

static int PyBBQNode_nb_bool(PyBBQNode* self) {
    switch (self->capture->type) {
        case CaptureType::Bool:
            return ((const uint8_t*)self->result->view.buf)[self->capture->start_offset] ? 1 : 0;
        case CaptureType::Struct:
        case CaptureType::Array:
            return 1;
        case CaptureType::String:
        case CaptureType::Bytes:
        case CaptureType::External:
            return (self->capture->end_offset > self->capture->start_offset) ? 1 : 0;
        case CaptureType::Computed: {
            // Truthy iff the Value pointer exists AND its underlying
            // value is non-zero / non-empty. Match the runtime's
            // bool-coercion semantics: IntValue nonzero=true, BoolValue
            // by-value, others → consider present-ness.
            auto* v = self->capture->computed_value;
            if (!v) return 0;
            if (v->kind == bbq::ComputedValue::Kind::Int)  return v->i != 0 ? 1 : 0;
            if (v->kind == bbq::ComputedValue::Kind::Bool) return v->b ? 1 : 0;
            return 1;  // any other present scalar → truthy
        }
        default: {
            PyObject* val = decode_auto_value(self->capture, self->result);
            if (!val) return -1;
            int r = PyObject_IsTrue(val);
            Py_DECREF(val);
            return r;
        }
    }
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
    (void*)       NULL,                     // nb_reserved
    (unaryfunc)   PyBBQNode_nb_float,       // nb_float
    (binaryfunc)  NULL,                     // nb_inplace_add
    (binaryfunc)  NULL,                     // nb_inplace_subtract
    (binaryfunc)  NULL,                     // nb_inplace_multiply
    (binaryfunc)  NULL,                     // nb_inplace_remainder
    (ternaryfunc) NULL,                     // nb_inplace_power
    (binaryfunc)  NULL,                     // nb_inplace_lshift
    (binaryfunc)  NULL,                     // nb_inplace_rshift
    (binaryfunc)  NULL,                     // nb_inplace_and
    (binaryfunc)  NULL,                     // nb_inplace_xor
    (binaryfunc)  NULL,                     // nb_inplace_or
    (binaryfunc)  NULL,                     // nb_floor_divide
    (binaryfunc)  NULL,                     // nb_true_divide
    (binaryfunc)  NULL,                     // nb_inplace_floor_divide
    (binaryfunc)  NULL,                     // nb_inplace_true_divide
    (unaryfunc)   NULL,                     // nb_index
    (binaryfunc)  NULL,                     // nb_matrix_multiply
    (binaryfunc)  NULL,                     // nb_inplace_matrix_multiply
};

// ── Mapping protocol ──

static Py_ssize_t PyBBQNode_mp_length(PyBBQNode* self) {
    CaptureType type = self->capture->type;
    if (type == CaptureType::Struct || type == CaptureType::Array) {
        // Live like a Python list: reflect overlay appends/deletes when the container
        // was structurally edited, else the baseline child count.
        int o = self->result->zcow ? self->result->zcow->overlay_child_count(self->capture) : -1;
        return o >= 0 ? o : self->capture->child_count;
    }
    PyErr_Format(PyExc_TypeError,
                 "object of type 'bbq.Node' (%s) has no len()",
                 capture_type_name(type));
    return -1;
}

static PyObject* PyBBQNode_mp_subscript(PyBBQNode* self, PyObject* key) {
    CaptureType ct = self->capture->type;
    bool is_container = (ct == CaptureType::Struct || ct == CaptureType::Array);

    // Slice
    if (PySlice_Check(key)) {
        if (!is_container) {
            PyErr_Format(PyExc_TypeError,
                         "'bbq.Node' (%s) is not subscriptable",
                         capture_type_name(ct));
            return NULL;
        }
        Py_ssize_t start, stop, step, length;
        if (PySlice_GetIndicesEx(key, self->capture->child_count,
                                 &start, &stop, &step, &length) < 0)
            return NULL;
        PyObject* list = PyList_New(length);
        if (!list) return NULL;
        for (Py_ssize_t i = 0, idx = start; i < length; i++, idx += step) {
            PyObject* node = (PyObject*)PyBBQNode_New(
                &self->capture->children[idx], self->result);
            if (!node) { Py_DECREF(list); return NULL; }
            PyList_SET_ITEM(list, i, node);
        }
        return list;
    }

    // Integer index
    if (PyIndex_Check(key)) {
        if (!is_container) {
            PyErr_Format(PyExc_TypeError,
                         "'bbq.Node' (%s) is not subscriptable",
                         capture_type_name(ct));
            return NULL;
        }
        Py_ssize_t index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (index == -1 && PyErr_Occurred()) return NULL;

        if (index < 0) index += self->capture->child_count;

        if (index < 0 || index >= self->capture->child_count) {
            PyErr_SetString(PyExc_IndexError, "index out of range");
            return NULL;
        }
        return (PyObject*)PyBBQNode_New(&self->capture->children[index], self->result);
    }

    // String key — only valid on containers with named children
    if (PyUnicode_Check(key)) {
        if (!is_container) {
            PyErr_Format(PyExc_TypeError,
                         "'bbq.Node' (%s) is not subscriptable",
                         capture_type_name(ct));
            return NULL;
        }
        const char* name = PyUnicode_AsUTF8(key);
        if (!name) return NULL;

        for (int i = 0; i < self->capture->child_count; i++) {
            if (self->capture->children[i].name &&
                strcmp(self->capture->children[i].name, name) == 0)
                return (PyObject*)PyBBQNode_New(&self->capture->children[i], self->result);
        }
        PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }

    PyErr_Format(PyExc_TypeError,
                 "indices must be integers or strings, not %.200s",
                 Py_TYPE(key)->tp_name);
    return NULL;
}

// node[i] = v / node[name] = v (CoW-detach the child), or del node[i] (remove).
static int PyBBQNode_mp_ass_subscript(PyBBQNode* self, PyObject* key, PyObject* value) {
    const FieldCapture* cap = self->capture;
    if (cap->type != CaptureType::Struct && cap->type != CaptureType::Array) {
        PyErr_Format(PyExc_TypeError, "'bbq.Node' (%s) does not support item assignment",
                     capture_type_name(cap->type));
        return -1;
    }
    const FieldCapture* child = nullptr;
    Py_ssize_t index = -1;
    if (PyIndex_Check(key)) {
        index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (index == -1 && PyErr_Occurred()) return -1;
        if (index < 0) index += cap->child_count;
        if (index < 0 || index >= cap->child_count) {
            PyErr_SetString(PyExc_IndexError, "index out of range"); return -1;
        }
        child = &cap->children[index];
    } else if (PyUnicode_Check(key)) {
        const char* name = PyUnicode_AsUTF8(key);
        if (!name) return -1;
        for (int i = 0; i < cap->child_count; i++)
            if (cap->children[i].name && strcmp(cap->children[i].name, name) == 0) {
                child = &cap->children[i]; index = i; break;
            }
        if (!child) { PyErr_SetObject(PyExc_KeyError, key); return -1; }
    } else {
        PyErr_Format(PyExc_TypeError, "indices must be integers or strings, not %.200s",
                     Py_TYPE(key)->tp_name);
        return -1;
    }
    if (value == NULL) {   // del node[index]
        if (!self->result->zcow) self->result->zcow = new bbq::zcow();
        if (!self->result->zcow->remove_index(cap, (size_t)index)) {
            PyErr_SetString(PyExc_IndexError, "cannot delete"); return -1;
        }
        return 0;
    }
    // Splice: replacing a composite array element (`arr[i] = b"..."`) sets a raw byte
    // subtree via the ZCow splice (CoW path-copy + re-serialize on emit) — ZCow is
    // byte-level, so the replacement is supplied as bytes. Leaf elements fall through
    // to the per-type leaf setter.
    if (cap->type == CaptureType::Array &&
        (child->type == CaptureType::Struct || child->type == CaptureType::Array)) {
        PyBBQResult* res = self->result;
        std::unique_ptr<bbq::znode> elem = znode_from_pyvalue(value, nullptr);
        if (!elem) return -1;
        if (!res->zcow) res->zcow = new bbq::zcow();
        res->zcow->set_node(child, std::move(elem));
        return 0;
    }
    return zcow_set_capture(child, self->result, value);
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

    for (int i = 0; i < self->capture->child_count; i++) {
        if (self->capture->children[i].name &&
            strcmp(self->capture->children[i].name, key) == 0)
            return 1;
    }
    return 0;
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

static int PyBBQNode_getbuffer(PyBBQNode* self, Py_buffer* view, int flags) {
    const uint8_t* data = (const uint8_t*)self->result->view.buf;
    size_t start = self->capture->start_offset;
    Py_ssize_t len = (Py_ssize_t)(self->capture->end_offset - start);

    return PyBuffer_FillInfo(view, (PyObject*)self,
                             (void*)(data + start), len,
                             1 /* readonly */, flags);
}

static PyBufferProcs PyBBQNode_as_buffer = {
    (getbufferproc) PyBBQNode_getbuffer,
    (releasebufferproc) NULL,
};

// ── str / repr ──

static PyObject* PyBBQNode_tp_repr(PyBBQNode* self) {
    const FieldCapture* cap = self->capture;
    if (cap->name) {
        return PyUnicode_FromFormat("<bbq.Node '%s' type=%s [0x%zx:0x%zx]>",
            cap->name, capture_type_name(cap->type),
            cap->start_offset, cap->end_offset);
    }
    return PyUnicode_FromFormat("<bbq.Node type=%s [0x%zx:0x%zx]>",
        capture_type_name(cap->type),
        cap->start_offset, cap->end_offset);
}

static PyObject* PyBBQNode_tp_str(PyBBQNode* self) {
    const FieldCapture* cap = self->capture;
    const uint8_t* data = (const uint8_t*)self->result->view.buf;

    switch (cap->type) {
        case CaptureType::String: {
            size_t len = cap->end_offset - cap->start_offset;
            return PyUnicode_DecodeUTF8(
                (const char*)(data + cap->start_offset), (Py_ssize_t)len, NULL);
        }
        case CaptureType::Bool:
            return PyUnicode_FromString(data[cap->start_offset] ? "True" : "False");
        case CaptureType::Struct:
        case CaptureType::Array:
            return PyBBQNode_tp_repr(self);
        default: {
            PyObject* val = decode_auto_value(cap, self->result);
            if (!val) return NULL;
            PyObject* str = PyObject_Str(val);
            Py_DECREF(val);
            return str;
        }
    }
}

// ── Rich comparison ──

static PyObject* PyBBQNode_richcompare(PyObject* self_obj, PyObject* other, int op) {
    PyBBQNode* self = (PyBBQNode*)self_obj;
    CaptureType type = self->capture->type;

    // Containers: identity comparison with other nodes
    if (type == CaptureType::Struct || type == CaptureType::Array) {
        if (Py_TYPE(other) == &PyBBQNode_Type) {
            bool eq = (self->capture == ((PyBBQNode*)other)->capture);
            switch (op) {
                case Py_EQ: return PyBool_FromLong(eq);
                case Py_NE: return PyBool_FromLong(!eq);
                default: Py_RETURN_NOTIMPLEMENTED;
            }
        }
        Py_RETURN_NOTIMPLEMENTED;
    }

    // Leaf nodes: materialize and compare
    PyObject* val = decode_auto_value(self->capture, self->result);
    if (!val) return NULL;
    PyObject* result = PyObject_RichCompare(val, other, op);
    Py_DECREF(val);
    return result;
}

// ── Iterator ──

static PyObject* PyBBQNode_tp_iter(PyBBQNode* self) {
    CaptureType type = self->capture->type;
    if (type != CaptureType::Struct && type != CaptureType::Array) {
        PyErr_Format(PyExc_TypeError,
                     "'bbq.Node' (%s) is not iterable",
                     capture_type_name(type));
        return NULL;
    }

    PyBBQNodeIter* iter = PyObject_GC_New(PyBBQNodeIter, &PyBBQNodeIter_Type);
    if (!iter) return NULL;

    Py_INCREF(self->result);
    iter->result = self->result;
    iter->children = self->capture->children;
    iter->count = self->capture->child_count;
    iter->index = 0;
    iter->yield_tuples = (self->capture->type == CaptureType::Struct);

    PyObject_GC_Track((PyObject*)iter);
    return (PyObject*)iter;
}

// ── Node methods ──

static PyObject* PyBBQNode_format(PyBBQNode* self, PyObject* args) {
    PyObject* format_spec;
    if (!PyArg_ParseTuple(args, "U", &format_spec)) return NULL;

    PyObject* val = decode_auto_value(self->capture, self->result);
    if (!val) return NULL;
    PyObject* result = PyObject_Format(val, format_spec);
    Py_DECREF(val);
    return result;
}

// Helper: append names from getset and methods tables to a list
static int append_type_attrs(PyObject* list, PyTypeObject* type) {
    // Properties from tp_getset
    if (type->tp_getset) {
        for (PyGetSetDef* gs = type->tp_getset; gs->name; gs++) {
            PyObject* s = PyUnicode_FromString(gs->name);
            if (!s || PyList_Append(list, s) < 0) {
                Py_XDECREF(s);
                return -1;
            }
            Py_DECREF(s);
        }
    }
    // Methods from tp_methods (skip dunder)
    if (type->tp_methods) {
        for (PyMethodDef* m = type->tp_methods; m->ml_name; m++) {
            if (m->ml_name[0] == '_' && m->ml_name[1] == '_') continue;
            PyObject* s = PyUnicode_FromString(m->ml_name);
            if (!s || PyList_Append(list, s) < 0) {
                Py_XDECREF(s);
                return -1;
            }
            Py_DECREF(s);
        }
    }
    return 0;
}

static PyObject* PyBBQNode_dir(PyBBQNode* self, PyObject*) {
    PyObject* list = PyList_New(0);
    if (!list) return NULL;

    if (append_type_attrs(list, &PyBBQNode_Type) < 0) {
        Py_DECREF(list);
        return NULL;
    }

    // Append child field names
    for (int i = 0; i < self->capture->child_count; i++) {
        if (self->capture->children[i].name) {
            PyObject* s = PyUnicode_FromString(self->capture->children[i].name);
            if (!s || PyList_Append(list, s) < 0) {
                Py_XDECREF(s);
                Py_DECREF(list);
                return NULL;
            }
            Py_DECREF(s);
        }
    }
    return list;
}

static PyObject* PyBBQNode_keys(PyBBQNode* self, PyObject*) {
    PyObject* list = PyList_New(0);
    if (!list) return NULL;

    for (int i = 0; i < self->capture->child_count; i++) {
        if (self->capture->children[i].name) {
            PyObject* s = PyUnicode_FromString(self->capture->children[i].name);
            if (!s || PyList_Append(list, s) < 0) {
                Py_XDECREF(s);
                Py_DECREF(list);
                return NULL;
            }
            Py_DECREF(s);
        }
    }
    return list;
}

static PyObject* PyBBQNode_values(PyBBQNode* self, PyObject*) {
    PyObject* list = PyList_New(0);
    if (!list) return NULL;

    for (int i = 0; i < self->capture->child_count; i++) {
        if (!self->capture->children[i].name) continue;
        PyObject* node = (PyObject*)PyBBQNode_New(&self->capture->children[i], self->result);
        if (!node || PyList_Append(list, node) < 0) {
            Py_XDECREF(node);
            Py_DECREF(list);
            return NULL;
        }
        Py_DECREF(node);
    }
    return list;
}

static PyObject* PyBBQNode_items(PyBBQNode* self, PyObject*) {
    PyObject* list = PyList_New(0);
    if (!list) return NULL;

    for (int i = 0; i < self->capture->child_count; i++) {
        const FieldCapture* child = &self->capture->children[i];
        if (!child->name) continue;

        PyObject* name = PyUnicode_FromString(child->name);
        if (!name) { Py_DECREF(list); return NULL; }

        PyObject* node = (PyObject*)PyBBQNode_New(child, self->result);
        if (!node) { Py_DECREF(name); Py_DECREF(list); return NULL; }

        PyObject* tuple = PyTuple_Pack(2, name, node);
        Py_DECREF(name);
        Py_DECREF(node);
        if (!tuple) { Py_DECREF(list); return NULL; }
        if (PyList_Append(list, tuple) < 0) {
            Py_DECREF(tuple);
            Py_DECREF(list);
            return NULL;
        }
        Py_DECREF(tuple);
    }
    return list;
}

// node.append(value): add an element to an array. ZCow is byte-level — a number takes
// the array's existing element type (a sibling, pure data); bytes/str append verbatim
// (the form for a composite or variable-width element). The array's child count updates
// live (like a Python list); any format count field is a separate byte the caller owns.
static PyObject* PyBBQNode_append(PyBBQNode* self, PyObject* value) {
    const FieldCapture* cap = self->capture;
    if (cap->type != CaptureType::Array) {
        PyErr_SetString(PyExc_TypeError, "append() requires an array node");
        return NULL;
    }
    PyBBQResult* res = self->result;
    const FieldCapture* sib = (cap->child_count > 0) ? &cap->children[0] : nullptr;
    std::unique_ptr<bbq::znode> elem = znode_from_pyvalue(value, sib);
    if (!elem) return NULL;
    if (!res->zcow) res->zcow = new bbq::zcow();
    res->zcow->append(cap, std::move(elem));
    Py_RETURN_NONE;
}

static PyMethodDef PyBBQNode_methods[] = {
    {"__format__", (PyCFunction)PyBBQNode_format, METH_VARARGS,
     "Format node value with format spec."},
    {"append",     (PyCFunction)PyBBQNode_append, METH_O,
     "append(value): add an element to an array node (typed from the grammar)."},
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
    return Py_BuildValue("(nn)",
        (Py_ssize_t)self->capture->start_offset,
        (Py_ssize_t)self->capture->end_offset);
}

static PyObject* PyBBQNode_get_raw(PyBBQNode* self, void*) {
    const uint8_t* data = (const uint8_t*)self->result->view.buf;
    size_t start = self->capture->start_offset;
    size_t len = self->capture->end_offset - start;
    return PyBytes_FromStringAndSize((const char*)(data + start), (Py_ssize_t)len);
}

static PyObject* PyBBQNode_get_capture_type(PyBBQNode* self, void*) {
    return PyUnicode_FromString(capture_type_name(self->capture->type));
}

static PyObject* PyBBQNode_get_name(PyBBQNode* self, void*) {
    if (self->capture->name)
        return PyUnicode_FromString(self->capture->name);
    Py_RETURN_NONE;
}

static PyObject* PyBBQNode_get_value(PyBBQNode* self, void*) {
    return decode_auto_value(self->capture, self->result);
}

// The arm/case ordinal the parser recorded for a union/alternatives/switch node
// (0 = first arm, 1 = second, …), or None when this node is not a variant. Pure
// data off the parsed capture — like offset/capture_type, not a grammar query.
static PyObject* PyBBQNode_get_variant_tag(PyBBQNode* self, void*) {
    if (self->capture->variant_tag < 0) Py_RETURN_NONE;
    return PyLong_FromLong(self->capture->variant_tag);
}

static PyGetSetDef PyBBQNode_getset[] = {
    {(char*)"offset",       (getter)PyBBQNode_get_offset,       NULL,
     (char*)"(start, end) byte offset tuple", NULL},
    {(char*)"raw",          (getter)PyBBQNode_get_raw,          NULL,
     (char*)"raw bytes from buffer", NULL},
    {(char*)"capture_type", (getter)PyBBQNode_get_capture_type, NULL,
     (char*)"capture type name string", NULL},
    {(char*)"name",         (getter)PyBBQNode_get_name,         NULL,
     (char*)"field name or None", NULL},
    {(char*)"value",        (getter)PyBBQNode_get_value,        NULL,
     (char*)"auto-materialized Python value", NULL},
    {(char*)"variant_tag",  (getter)PyBBQNode_get_variant_tag,  NULL,
     (char*)"union/switch arm ordinal, or None if not a variant", NULL},
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
    PyObject_HashNotImplemented,            // tp_hash
    NULL,                                   // tp_call
    (reprfunc)PyBBQNode_tp_str,             // tp_str
    (getattrofunc)PyBBQNode_getattro,       // tp_getattro
    (setattrofunc)PyBBQNode_setattro,       // tp_setattro
    &PyBBQNode_as_buffer,                   // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    NULL,                                   // tp_doc
    (traverseproc)PyBBQNode_traverse,       // tp_traverse
    (inquiry)PyBBQNode_clear,               // tp_clear
    PyBBQNode_richcompare,                  // tp_richcompare
    0,                                      // tp_weaklistoffset
    (getiterfunc)PyBBQNode_tp_iter,         // tp_iter
    NULL,                                   // tp_iternext
    PyBBQNode_methods,                      // tp_methods
    NULL,                                   // tp_members
    PyBBQNode_getset,                       // tp_getset
    NULL,                                   // tp_base
    NULL,                                   // tp_dict
    NULL,                                   // tp_descr_get
    NULL,                                   // tp_descr_set
    0,                                      // tp_dictoffset
    NULL,                                   // tp_init
    NULL,                                   // tp_alloc
    NULL,                                   // tp_new
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
    if (self->index >= self->count)
        return NULL;  // StopIteration — tp_iternext convention

    const FieldCapture* child = &self->children[self->index++];

    if (self->yield_tuples) {
        // Struct: yield (name, node)
        PyObject* name = child->name
            ? PyUnicode_FromString(child->name)
            : Py_NewRef(Py_None);
        if (!name) return NULL;

        PyBBQNode* node = PyBBQNode_New(child, self->result);
        if (!node) { Py_DECREF(name); return NULL; }

        PyObject* tuple = PyTuple_Pack(2, name, node);
        Py_DECREF(name);
        Py_DECREF(node);
        return tuple;
    }

    // Array: yield node
    return (PyObject*)PyBBQNode_New(child, self->result);
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

static void PyBBQResult_dealloc(PyBBQResult* self) {
    PyObject_GC_UnTrack((PyObject*)self);
    delete self->zcow;
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

static PyObject* PyBBQResult_getattro(PyBBQResult* self, PyObject* name) {
    PyObject* attr = PyObject_GenericGetAttr((PyObject*)self, name);
    if (attr) return attr;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return NULL;

    if (!self->meta.root) return NULL;  // keep AttributeError

    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return NULL;

    for (int i = 0; i < self->meta.root->child_count; i++) {
        if (self->meta.root->children[i].name &&
            strcmp(self->meta.root->children[i].name, key) == 0) {
            PyErr_Clear();
            return (PyObject*)PyBBQNode_New(&self->meta.root->children[i], self);
        }
    }
    return NULL;  // keep AttributeError
}

// `result.field = v` → detach that top-level field to Owned in the overlay.
static int PyBBQResult_setattro(PyBBQResult* self, PyObject* name, PyObject* value) {
    if (!value) { PyErr_SetString(PyExc_TypeError, "cannot delete a BBQ field"); return -1; }
    if (!self->meta.root) { PyErr_SetString(PyExc_AttributeError, "no parse tree"); return -1; }
    const char* key = PyUnicode_AsUTF8(name);
    if (!key) return -1;
    return zcow_set_field(self->meta.root, self, key, value);
}

// ── Result repr ──

static PyObject* PyBBQResult_tp_repr(PyBBQResult* self) {
    if (!self->meta.success) {
        return PyUnicode_FromFormat(
            "<bbq.ParseResult failed at offset 0x%zx>",
            self->meta.error_offset);
    }

    // Build field names string
    std::string fields;
    if (self->meta.root) {
        for (int i = 0; i < self->meta.root->child_count; i++) {
            if (self->meta.root->children[i].name) {
                if (!fields.empty()) fields += ", ";
                fields += self->meta.root->children[i].name;
            }
        }
    }

    return PyUnicode_FromFormat(
        "<bbq.ParseResult ok %zd bytes [%s]>",
        self->meta.bytes_consumed, fields.c_str());
}

// ── Result properties ──

static PyObject* PyBBQResult_get_success(PyBBQResult* self, void*) {
    return PyBool_FromLong(self->meta.success);
}

static PyObject* PyBBQResult_get_bytes_consumed(PyBBQResult* self, void*) {
    return PyLong_FromSize_t(self->meta.bytes_consumed);
}

static PyObject* PyBBQResult_get_error_message(PyBBQResult* self, void*) {
    if (self->meta.error_message)
        return PyUnicode_FromString(self->meta.error_message);
    Py_RETURN_NONE;
}

static PyObject* PyBBQResult_get_error_offset(PyBBQResult* self, void*) {
    return PyLong_FromSize_t(self->meta.error_offset);
}

static PyObject* PyBBQResult_get_root(PyBBQResult* self, void*) {
    if (!self->meta.root) Py_RETURN_NONE;
    return (PyObject*)PyBBQNode_New(self->meta.root, self);
}

static PyGetSetDef PyBBQResult_getset[] = {
    {(char*)"success",        (getter)PyBBQResult_get_success,        NULL,
     (char*)"True if parse succeeded", NULL},
    {(char*)"bytes_consumed", (getter)PyBBQResult_get_bytes_consumed, NULL,
     (char*)"number of bytes consumed", NULL},
    {(char*)"error_message",  (getter)PyBBQResult_get_error_message,  NULL,
     (char*)"error message or None", NULL},
    {(char*)"error_offset",   (getter)PyBBQResult_get_error_offset,   NULL,
     (char*)"byte offset of error", NULL},
    {(char*)"root",           (getter)PyBBQResult_get_root,           NULL,
     (char*)"root Node of the parse tree", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

// ── Result methods ──

static PyObject* PyBBQResult_dir(PyBBQResult* self, PyObject*) {
    PyObject* list = PyList_New(0);
    if (!list) return NULL;

    if (append_type_attrs(list, &PyBBQResult_Type) < 0) {
        Py_DECREF(list);
        return NULL;
    }

    if (self->meta.root) {
        for (int i = 0; i < self->meta.root->child_count; i++) {
            if (self->meta.root->children[i].name) {
                PyObject* s = PyUnicode_FromString(self->meta.root->children[i].name);
                if (!s || PyList_Append(list, s) < 0) {
                    Py_XDECREF(s);
                    Py_DECREF(list);
                    return NULL;
                }
                Py_DECREF(s);
            }
        }
    }
    return list;
}

// The spec's writer op-list, lowered on first use. Null + a Python error on failure.
static const nlohmann::json* spec_wops(PyBBQSpec* spec) {
    if (spec->wops) return spec->wops;
    bbq::render::CompilerCtx ctx{spec->parser->ast, spec->sema, spec->grammar, ""};
    try {
        spec->wops = new nlohmann::json(bbq::render::lower_writer_ops(ctx));
    } catch (const std::exception& e) {
        PyErr_Format(PyBBQParseError, "writer lowering failed: %s", e.what());
        return nullptr;
    }
    return spec->wops;
}

// `put` — the write half of the lens. ZCow (bbq::emit) is the byte serializer; it knows
// bytes, not the format. So the grammar walk runs first over the edited overlay and
// recomputes the DEPENDENT fields the edits invalidated (array counts, @rest window
// sizes), then ZCow serializes the now-consistent document. Unedited, that is the
// identity — GetPut, byte for byte; edited, the result re-parses to the edit — PutGet.
static PyObject* PyBBQResult_emit(PyBBQResult* self, PyObject*) {
    const uint8_t* b = self->view_valid ? (const uint8_t*)self->view.buf : nullptr;
    size_t l = self->view_valid ? (size_t)self->view.len : 0;

    if (!self->meta.success || !self->meta.root) {
        // A failed parse has no document to enforce a grammar over; the overlay is
        // whatever it was, so this is the raw serialization by definition.
        bbq::zcow empty;
        std::vector<uint8_t> raw = bbq::emit(b, l, self->zcow ? *self->zcow : empty);
        return PyBytes_FromStringAndSize((const char*)raw.data(), (Py_ssize_t)raw.size());
    }

    const nlohmann::json* wops = spec_wops(self->spec);
    if (!wops) return NULL;

    if (!self->zcow) self->zcow = new bbq::zcow();
    std::string err;
    std::vector<uint8_t> out = bbq::render::run_writer(
        *wops, self->rule, self->meta.root, b, l, self->zcow, &err);
    if (out.empty() && !err.empty()) {
        PyErr_Format(PyBBQParseError, "emit: %s", err.c_str());
        return NULL;
    }
    return PyBytes_FromStringAndSize((const char*)out.data(), (Py_ssize_t)out.size());
}

// The structured delta set since parse: a list of dicts
// {path, offset:(start,end), old, new} — a typed, path-addressed diff.
static PyObject* PyBBQResult_deltas(PyBBQResult* self, PyObject*) {
    PyObject* list = PyList_New(0);
    if (!list || !self->zcow) return list;
    auto ds = bbq::deltas((const uint8_t*)self->view.buf, *self->zcow);
    for (const auto& d : ds) {
        PyObject* old_o = d.is_bytes
            ? PyBytes_FromStringAndSize((const char*)d.old_bytes.data(), (Py_ssize_t)d.old_bytes.size())
            : PyLong_FromLongLong(d.old_int);
        PyObject* new_o = d.is_bytes
            ? PyBytes_FromStringAndSize((const char*)d.new_bytes.data(), (Py_ssize_t)d.new_bytes.size())
            : PyLong_FromLongLong(d.new_int);
        PyObject* item = Py_BuildValue("{s:s,s:(nn),s:O,s:O}",
            "path", d.path.c_str(),
            "offset", (Py_ssize_t)d.start, (Py_ssize_t)d.end,
            "old", old_o, "new", new_o);
        Py_XDECREF(old_o); Py_XDECREF(new_o);
        if (item) { PyList_Append(list, item); Py_DECREF(item); }
    }
    return list;
}

static PyMethodDef PyBBQResult_methods[] = {
    {"__dir__", (PyCFunction)PyBBQResult_dir, METH_NOARGS,
     "List attributes including parsed field names."},
    {"emit", (PyCFunction)PyBBQResult_emit, METH_NOARGS,
     "Serialize back to bytes: enforce the grammar over the edited document "
     "(dependent fields — array counts, @rest sizes — recomputed), then blit the "
     "input and patch what changed. Byte-identical to the input if nothing was "
     "changed; re-parses to the edit if something was."},
    {"deltas", (PyCFunction)PyBBQResult_deltas, METH_NOARGS,
     "The structured diff since parse: list of {path, offset, old, new}."},
    {NULL, NULL, 0, NULL}
};

// ── Result iteration and contains ──

static PyObject* PyBBQResult_tp_iter(PyBBQResult* self) {
    if (!self->meta.root) {
        PyErr_SetString(PyExc_RuntimeError, "no parse result");
        return NULL;
    }

    PyBBQNodeIter* iter = PyObject_GC_New(PyBBQNodeIter, &PyBBQNodeIter_Type);
    if (!iter) return NULL;

    Py_INCREF(self);
    iter->result = self;
    iter->children = self->meta.root->children;
    iter->count = self->meta.root->child_count;
    iter->index = 0;
    iter->yield_tuples = true;  // ParseResult root is always struct-like

    PyObject_GC_Track((PyObject*)iter);
    return (PyObject*)iter;
}

static int PyBBQResult_sq_contains(PyBBQResult* self, PyObject* value) {
    if (!PyUnicode_Check(value)) return 0;
    if (!self->meta.root) return 0;

    const char* key = PyUnicode_AsUTF8(value);
    if (!key) { PyErr_Clear(); return 0; }

    for (int i = 0; i < self->meta.root->child_count; i++) {
        if (self->meta.root->children[i].name &&
            strcmp(self->meta.root->children[i].name, key) == 0)
            return 1;
    }
    return 0;
}

static PySequenceMethods PyBBQResult_as_sequence = {
    (lenfunc)        NULL,                       // sq_length
    (binaryfunc)     NULL,                       // sq_concat
    (ssizeargfunc)   NULL,                       // sq_repeat
    (ssizeargfunc)   NULL,                       // sq_item
    NULL,                                        // was sq_slice
    (ssizeobjargproc)NULL,                       // sq_ass_item
    NULL,                                        // was sq_ass_slice
    (objobjproc)     PyBBQResult_sq_contains,    // sq_contains
    (binaryfunc)     NULL,                       // sq_inplace_concat
    (ssizeargfunc)   NULL,                       // sq_inplace_repeat
};

// ── Result mapping protocol ──

static Py_ssize_t PyBBQResult_mp_length(PyBBQResult* self) {
    if (!self->meta.root) return 0;
    return self->meta.root->child_count;
}

static PyObject* PyBBQResult_mp_subscript(PyBBQResult* self, PyObject* key) {
    if (!self->meta.root) {
        PyErr_SetString(PyExc_RuntimeError, "no parse result");
        return NULL;
    }

    // Slice
    if (PySlice_Check(key)) {
        Py_ssize_t start, stop, step, length;
        if (PySlice_GetIndicesEx(key, self->meta.root->child_count,
                                 &start, &stop, &step, &length) < 0)
            return NULL;
        PyObject* list = PyList_New(length);
        if (!list) return NULL;
        for (Py_ssize_t i = 0, idx = start; i < length; i++, idx += step) {
            PyObject* node = (PyObject*)PyBBQNode_New(
                &self->meta.root->children[idx], self);
            if (!node) { Py_DECREF(list); return NULL; }
            PyList_SET_ITEM(list, i, node);
        }
        return list;
    }

    // Integer index
    if (PyIndex_Check(key)) {
        Py_ssize_t index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (index == -1 && PyErr_Occurred()) return NULL;

        if (index < 0) index += self->meta.root->child_count;

        if (index < 0 || index >= self->meta.root->child_count) {
            PyErr_SetString(PyExc_IndexError, "index out of range");
            return NULL;
        }
        return (PyObject*)PyBBQNode_New(&self->meta.root->children[index], self);
    }

    // String key
    if (PyUnicode_Check(key)) {
        const char* name = PyUnicode_AsUTF8(key);
        if (!name) return NULL;

        for (int i = 0; i < self->meta.root->child_count; i++) {
            if (self->meta.root->children[i].name &&
                strcmp(self->meta.root->children[i].name, name) == 0)
                return (PyObject*)PyBBQNode_New(&self->meta.root->children[i], self);
        }
        PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }

    PyErr_Format(PyExc_TypeError,
                 "indices must be integers or strings, not %.200s",
                 Py_TYPE(key)->tp_name);
    return NULL;
}

static PyMappingMethods PyBBQResult_as_mapping = {
    (lenfunc)      PyBBQResult_mp_length,
    (binaryfunc)   PyBBQResult_mp_subscript,
    (objobjargproc)NULL,
};

PyTypeObject PyBBQResult_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bbq.ParseResult",                     // tp_name
    sizeof(PyBBQResult),                   // tp_basicsize
    0,                                     // tp_itemsize
    (destructor)PyBBQResult_dealloc,       // tp_dealloc
    0,                                     // tp_vectorcall_offset
    NULL,                                  // tp_getattr
    NULL,                                  // tp_setattr
    NULL,                                  // tp_as_async
    (reprfunc)PyBBQResult_tp_repr,         // tp_repr
    NULL,                                  // tp_as_number
    &PyBBQResult_as_sequence,              // tp_as_sequence
    &PyBBQResult_as_mapping,               // tp_as_mapping
    NULL,                                  // tp_hash
    NULL,                                  // tp_call
    NULL,                                  // tp_str
    (getattrofunc)PyBBQResult_getattro,    // tp_getattro
    (setattrofunc)PyBBQResult_setattro,    // tp_setattro
    NULL,                                  // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    NULL,                                  // tp_doc
    (traverseproc)PyBBQResult_traverse,    // tp_traverse
    (inquiry)PyBBQResult_clear,            // tp_clear
    NULL,                                  // tp_richcompare
    0,                                     // tp_weaklistoffset
    (getiterfunc)PyBBQResult_tp_iter,      // tp_iter
    NULL,                                  // tp_iternext
    PyBBQResult_methods,                   // tp_methods
    NULL,                                  // tp_members
    PyBBQResult_getset,                    // tp_getset
};


// ── PyBBQSpec ───────────────────────────────────────────────────────────────

static void PyBBQSpec_dealloc(PyBBQSpec* self) {
    for (int i = 0; i < self->ext_count; i++)
        Py_XDECREF(self->ext_callables[i]);
    PyMem_Free(self->ext_callables);
    PyMem_Free(self->ext_entries);
    delete self->wops;
    delete self->grammar;
    delete self->sema;      // holds a reference to *errors, so it goes first
    delete self->errors;
    delete self->parser;    // owns the AST the lowering read
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

    // Check if already registered — if so, replace
    for (int i = 0; i < self->ext_count; i++) {
        if (self->ext_entries[i].name == interned) {
            Py_INCREF(callable);
            Py_DECREF(self->ext_callables[i]);
            self->ext_callables[i] = callable;
            self->ext_entries[i].user_data = (void*)callable;
            Py_RETURN_NONE;
        }
    }

    // Grow arrays if needed
    if (self->ext_count >= self->ext_capacity) {
        int new_cap = self->ext_capacity ? self->ext_capacity * 2 : 4;
        auto* new_callables = (PyObject**)PyMem_Realloc(
            self->ext_callables, sizeof(PyObject*) * new_cap);
        if (!new_callables) return PyErr_NoMemory();
        self->ext_callables = new_callables;

        auto* new_entries = (ExternalParserTable::Entry*)PyMem_Realloc(
            self->ext_entries, sizeof(ExternalParserTable::Entry) * new_cap);
        if (!new_entries) return PyErr_NoMemory();
        self->ext_entries = new_entries;

        self->ext_capacity = new_cap;
    }

    Py_INCREF(callable);
    int idx = self->ext_count++;
    self->ext_callables[idx] = callable;
    self->ext_entries[idx] = {interned, py_extern_trampoline, (void*)callable};

    // Update table pointer
    self->ext_table.entries = self->ext_entries;
    self->ext_table.count = self->ext_count;

    Py_RETURN_NONE;
}

// Core parse logic — takes ownership of view on success
static PyObject* do_parse(PyBBQSpec* self, Py_buffer* view, const char* rule_name) {
    KontNode* entry = nullptr;
    // The RESOLVED rule name (the grammar's own interned copy, so it outlives the call):
    // emit() enforces this rule's grammar, so the result has to remember which one it is.
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

    ParseArena* arena = new ParseArena();
    CEKMachine machine;
    machine.arena = arena;
    machine.builtins = &self->grammar->builtins;
    if (self->ext_count > 0)
        machine.ext_parsers = &self->ext_table;

    CaptureMetadata meta = machine.execute_from(
        entry,
        (const uint8_t*)view->buf,
        (size_t)view->len,
        self->grammar->default_little_endian);

    PyBBQResult* result = PyObject_GC_New(PyBBQResult, &PyBBQResult_Type);
    if (!result) {
        delete arena;
        return NULL;
    }
    result->arena = arena;
    result->meta = meta;
    result->view = *view;       // transfer buffer ownership
    result->view_valid = true;
    result->zcow = nullptr;     // created on first mutation
    result->rule = resolved;
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
        // mmap via Python's mmap module
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
    PyObject* list = PyList_New(self->grammar->rule_count);
    if (!list) return NULL;
    for (int i = 0; i < self->grammar->rule_count; i++) {
        PyObject* name = PyUnicode_FromString(self->grammar->rules[i].name);
        if (!name) { Py_DECREF(list); return NULL; }
        PyList_SET_ITEM(list, i, name);
    }
    return list;
}

static PyObject* PyBBQSpec_get_default_endian(PyBBQSpec* self, void*) {
    return PyUnicode_FromString(
        self->grammar->default_little_endian ? "little" : "big");
}

static PyGetSetDef PyBBQSpec_getset[] = {
    {(char*)"rules",          (getter)PyBBQSpec_get_rules,          NULL,
     (char*)"list of rule names", NULL},
    {(char*)"default_endian", (getter)PyBBQSpec_get_default_endian, NULL,
     (char*)"\"little\" or \"big\"", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMethodDef PyBBQSpec_methods[] = {
    {"parse",      (PyCFunction)PyBBQSpec_parse,
     METH_VARARGS | METH_KEYWORDS,
     "parse(data, *, rule=None) -> ParseResult\n\n"
     "Parse binary data. data must support the buffer protocol."},
    {"parse_file", (PyCFunction)PyBBQSpec_parse_file,
     METH_VARARGS | METH_KEYWORDS,
     "parse_file(path, *, rule=None) -> ParseResult\n\n"
     "Parse a binary file (mmap'd for zero-copy)."},
    {"register_extern", (PyCFunction)PyBBQSpec_register_extern,
     METH_VARARGS,
     "register_extern(name, callable) -> None\n\n"
     "Register an external parser function. callable(memoryview) -> int bytes consumed, or None on failure."},
    {NULL, NULL, 0, NULL}
};

PyTypeObject PyBBQSpec_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bbq.Spec",                            // tp_name
    sizeof(PyBBQSpec),                     // tp_basicsize
    0,                                     // tp_itemsize
    (destructor)PyBBQSpec_dealloc,         // tp_dealloc
    0,                                     // tp_vectorcall_offset
    NULL,                                  // tp_getattr
    NULL,                                  // tp_setattr
    NULL,                                  // tp_as_async
    NULL,                                  // tp_repr
    NULL,                                  // tp_as_number
    NULL,                                  // tp_as_sequence
    NULL,                                  // tp_as_mapping
    NULL,                                  // tp_hash
    NULL,                                  // tp_call
    NULL,                                  // tp_str
    NULL,                                  // tp_getattro
    NULL,                                  // tp_setattro
    NULL,                                  // tp_as_buffer
    Py_TPFLAGS_DEFAULT,                    // tp_flags
    "Compiled BBQ grammar",                // tp_doc
    NULL,                                  // tp_traverse
    NULL,                                  // tp_clear
    NULL,                                  // tp_richcompare
    0,                                     // tp_weaklistoffset
    NULL,                                  // tp_iter
    NULL,                                  // tp_iternext
    PyBBQSpec_methods,                     // tp_methods
    NULL,                                  // tp_members
    PyBBQSpec_getset,                      // tp_getset
};


// ── Module functions ────────────────────────────────────────────────────────

static PyObject* compile_source(const char* source, Py_ssize_t length) {
    // The Parser owns the AST and the Sema its resolved facts; both outlive this call
    // because the writer lowering consumes them (the Spec frees them in dealloc).
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
    spec->wops = nullptr;
    spec->ext_callables = nullptr;
    spec->ext_entries = nullptr;
    spec->ext_count = 0;
    spec->ext_capacity = 0;
    spec->ext_table = {};
    return (PyObject*)spec;
}

static PyObject* bbq_compile(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
    const char* path;
    static const char* kwlist[] = {"path", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s",
                                     (char**)kwlist, &path))
        return NULL;

    FILE* f = fopen(path, "r");
    if (!f) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string source((size_t)size, '\0');
    size_t nread = fread(&source[0], 1, (size_t)size, f);
    fclose(f);
    source.resize(nread);

    return compile_source(source.c_str(), (Py_ssize_t)nread);
}

static PyObject* bbq_compile_string(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
    const char* source;
    Py_ssize_t source_len;
    static const char* kwlist[] = {"source", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#",
                                     (char**)kwlist, &source, &source_len))
        return NULL;

    return compile_source(source, source_len);
}

static PyMethodDef bbq_module_methods[] = {
    {"compile",        (PyCFunction)bbq_compile,
     METH_VARARGS | METH_KEYWORDS,
     "compile(path) -> Spec\n\nCompile a BBQ spec file."},
    {"compile_string", (PyCFunction)bbq_compile_string,
     METH_VARARGS | METH_KEYWORDS,
     "compile_string(source) -> Spec\n\nCompile a BBQ spec from a string."},
    {NULL, NULL, 0, NULL}
};


// ── Module init ─────────────────────────────────────────────────────────────

static struct PyModuleDef bbq_moduledef = {
    PyModuleDef_HEAD_INIT,
    "bbq",                                 // m_name
    "BBQ binary format parser",            // m_doc
    -1,                                    // m_size
    bbq_module_methods,                    // m_methods
};

#if defined(__cplusplus)
extern "C"
#endif
#if defined(__GNUC__) && __GNUC__ >= 4
__attribute__ ((visibility("default")))
#endif

PyObject*
PyInit_bbq(void)
{
    PyObject* m;

    m = PyModule_Create(&bbq_moduledef);
    if (m == NULL) {
        return NULL;
    }

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
