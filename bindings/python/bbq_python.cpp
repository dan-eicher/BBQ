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

using namespace bbq::cek;


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

static PyObject* decode_int(const FieldCapture* cap, const uint8_t* data) {
    size_t off = cap->start_offset;
    switch (cap->type) {
        case CaptureType::UInt8:
            return PyLong_FromUnsignedLong(data[off]);
        case CaptureType::UInt16LE:
            return PyLong_FromUnsignedLong(
                (uint32_t)data[off] | ((uint32_t)data[off+1] << 8));
        case CaptureType::UInt16BE:
            return PyLong_FromUnsignedLong(
                ((uint32_t)data[off] << 8) | (uint32_t)data[off+1]);
        case CaptureType::UInt32LE:
            return PyLong_FromUnsignedLong(
                (uint32_t)data[off]       | ((uint32_t)data[off+1] << 8) |
                ((uint32_t)data[off+2] << 16) | ((uint32_t)data[off+3] << 24));
        case CaptureType::UInt32BE:
            return PyLong_FromUnsignedLong(
                ((uint32_t)data[off] << 24) | ((uint32_t)data[off+1] << 16) |
                ((uint32_t)data[off+2] << 8) | (uint32_t)data[off+3]);
        case CaptureType::UInt64LE: {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= (uint64_t)data[off+i] << (i * 8);
            return PyLong_FromUnsignedLongLong(v);
        }
        case CaptureType::UInt64BE: {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= (uint64_t)data[off+i] << ((7-i) * 8);
            return PyLong_FromUnsignedLongLong(v);
        }
        case CaptureType::Int8:
            return PyLong_FromLong((int8_t)data[off]);
        case CaptureType::Int16LE: {
            uint16_t raw = (uint16_t)data[off] | ((uint16_t)data[off+1] << 8);
            return PyLong_FromLong((int16_t)raw);
        }
        case CaptureType::Int16BE: {
            uint16_t raw = ((uint16_t)data[off] << 8) | (uint16_t)data[off+1];
            return PyLong_FromLong((int16_t)raw);
        }
        case CaptureType::Int32LE: {
            uint32_t raw = (uint32_t)data[off]       | ((uint32_t)data[off+1] << 8) |
                          ((uint32_t)data[off+2] << 16) | ((uint32_t)data[off+3] << 24);
            return PyLong_FromLong((int32_t)raw);
        }
        case CaptureType::Int32BE: {
            uint32_t raw = ((uint32_t)data[off] << 24) | ((uint32_t)data[off+1] << 16) |
                          ((uint32_t)data[off+2] << 8) | (uint32_t)data[off+3];
            return PyLong_FromLong((int32_t)raw);
        }
        case CaptureType::Int64LE: {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= (uint64_t)data[off+i] << (i * 8);
            return PyLong_FromLongLong((int64_t)v);
        }
        case CaptureType::Int64BE: {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= (uint64_t)data[off+i] << ((7-i) * 8);
            return PyLong_FromLongLong((int64_t)v);
        }
        case CaptureType::Bool:
            return PyLong_FromLong(data[off] ? 1 : 0);
        case CaptureType::Computed: {
            // computed_value is a typed Value* in the new IR. Dispatch
            // on the Value tag — IntValue/BoolValue → int, FloatValue
            // → float (truncated to int for back-compat with this
            // function's int-typed return path), other types fail.
            if (!cap->computed_value) return PyLong_FromLong(0);
            switch (cap->computed_value->tag) {
                case bbq::cek::ValueTag::IntValue:
                    return PyLong_FromLongLong(
                        static_cast<bbq::cek::IntValue*>(cap->computed_value)->v);
                case bbq::cek::ValueTag::BoolValue:
                    return PyLong_FromLong(
                        static_cast<bbq::cek::BoolValue*>(cap->computed_value)->v ? 1 : 0);
                case bbq::cek::ValueTag::FloatValue:
                    return PyLong_FromLongLong(static_cast<int64_t>(
                        static_cast<bbq::cek::FloatValue*>(cap->computed_value)->v));
                default:
                    PyErr_SetString(PyExc_TypeError,
                                    "computed value is not numeric");
                    return NULL;
            }
        }
        default:
            PyErr_Format(PyExc_TypeError, "cannot convert %s to int",
                        capture_type_name(cap->type));
            return NULL;
    }
}

static PyObject* decode_float(const FieldCapture* cap, const uint8_t* data) {
    size_t off = cap->start_offset;
    switch (cap->type) {
        case CaptureType::Float32LE: {
            uint32_t raw = (uint32_t)data[off]       | ((uint32_t)data[off+1] << 8) |
                          ((uint32_t)data[off+2] << 16) | ((uint32_t)data[off+3] << 24);
            float f; memcpy(&f, &raw, sizeof(f));
            return PyFloat_FromDouble(f);
        }
        case CaptureType::Float32BE: {
            uint32_t raw = ((uint32_t)data[off] << 24) | ((uint32_t)data[off+1] << 16) |
                          ((uint32_t)data[off+2] << 8) | (uint32_t)data[off+3];
            float f; memcpy(&f, &raw, sizeof(f));
            return PyFloat_FromDouble(f);
        }
        case CaptureType::Float64LE: {
            uint64_t raw = 0;
            for (int i = 0; i < 8; i++) raw |= (uint64_t)data[off+i] << (i * 8);
            double d; memcpy(&d, &raw, sizeof(d));
            return PyFloat_FromDouble(d);
        }
        case CaptureType::Float64BE: {
            uint64_t raw = 0;
            for (int i = 0; i < 8; i++) raw |= (uint64_t)data[off+i] << ((7-i) * 8);
            double d; memcpy(&d, &raw, sizeof(d));
            return PyFloat_FromDouble(d);
        }
        default:
            PyErr_Format(PyExc_TypeError, "cannot convert %s to float",
                        capture_type_name(cap->type));
            return NULL;
    }
}

static PyObject* decode_auto_value(const FieldCapture* cap, PyBBQResult* result) {
    const uint8_t* data = (const uint8_t*)result->view.buf;

    switch (cap->type) {
        case CaptureType::UInt8:    case CaptureType::UInt16LE: case CaptureType::UInt16BE:
        case CaptureType::UInt32LE: case CaptureType::UInt32BE:
        case CaptureType::UInt64LE: case CaptureType::UInt64BE:
        case CaptureType::Int8:     case CaptureType::Int16LE:  case CaptureType::Int16BE:
        case CaptureType::Int32LE:  case CaptureType::Int32BE:
        case CaptureType::Int64LE:  case CaptureType::Int64BE:
        case CaptureType::Computed:
            return decode_int(cap, data);

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

// ── Number protocol ──

static PyObject* PyBBQNode_nb_int(PyBBQNode* self) {
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
            if (v->tag == bbq::cek::ValueTag::IntValue) {
                return static_cast<bbq::cek::IntValue*>(v)->v != 0 ? 1 : 0;
            }
            if (v->tag == bbq::cek::ValueTag::BoolValue) {
                return static_cast<bbq::cek::BoolValue*>(v)->v ? 1 : 0;
            }
            return 1;  // any other typed value present → truthy
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
    if (type == CaptureType::Struct || type == CaptureType::Array)
        return self->capture->child_count;
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

static PyMappingMethods PyBBQNode_as_mapping = {
    (lenfunc)      PyBBQNode_mp_length,
    (binaryfunc)   PyBBQNode_mp_subscript,
    (objobjargproc)NULL,
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

static PyMethodDef PyBBQNode_methods[] = {
    {"__format__", (PyCFunction)PyBBQNode_format, METH_VARARGS,
     "Format node value with format spec."},
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
    NULL,                                   // tp_setattro
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

static PyMethodDef PyBBQResult_methods[] = {
    {"__dir__", (PyCFunction)PyBBQResult_dir, METH_NOARGS,
     "List attributes including parsed field names."},
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
    NULL,                                  // tp_setattro
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
    delete self->grammar;
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
    if (rule_name) {
        entry = self->grammar->lookup(std::string(rule_name));
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
    Parser parser;
    parser.init(source, (int)length);
    if (!parser.parse()) {
        PyErr_Format(PyBBQParseError, "parse error at line %d, col %d",
                     parser.line(), parser.col());
        return NULL;
    }

    bbqgen::ErrorReporter errors;
    bbqgen::Sema sema(errors);
    if (!sema.analyze(parser.ast)) {
        std::ostringstream oss;
        errors.print_all(oss);
        PyErr_SetString(PyBBQParseError, oss.str().c_str());
        return NULL;
    }

    ::bbq::Compiler compiler;
    CompiledGrammar* grammar = compiler.compile_grammar(parser.ast);
    if (!grammar) {
        PyErr_SetString(PyBBQParseError, "compilation failed");
        return NULL;
    }

    PyBBQSpec* spec = PyObject_New(PyBBQSpec, &PyBBQSpec_Type);
    if (!spec) { delete grammar; return NULL; }
    spec->grammar = grammar;
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

    return m;
}
