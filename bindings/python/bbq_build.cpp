// bbq.build — grammar-free byte construction (see bbq_build.h).
//
// Typed leaf factories + Struct/Array containers. Every value serializes to bytes via
// the shared encode atoms; bytes are the only thing this layer knows. Independent of
// the parse/overlay side — bbq_python.cpp links it in and exposes it as `bbq.build`.

#define PY_SSIZE_T_CLEAN
#include "bbq_build.h"

#include <cstring>

#include "CaptureCow.h"  // bbq::leaf_width / encode_uleb128 / encode_sleb128, pulls Capture* in

using bbq::CaptureType;

// A Struct's `names` and `children` are two lists that must stay parallel, and every
// accessor is a check-then-act across them: find the index in one, index into the other.
// Each list op is individually atomic on a free-threaded build, which is exactly why the
// pair is not — two threads interleave between the two PyList_Appends and the lists
// desynchronize, after which the unchecked PyList_GET_ITEM reads out of bounds. Critical
// sections (rather than the plain mutex bbq_python.cpp uses for its C++ state) are right
// here: the invariant belongs to the Python object, and the guarded code calls back into
// the interpreter (rich comparison, list resize), which a critical section tolerates.
// Below 3.13 the macros do not exist and the GIL already provides this.
#if PY_VERSION_HEX < 0x030D0000
#  define Py_BEGIN_CRITICAL_SECTION(op) {
#  define Py_END_CRITICAL_SECTION() }
#endif

namespace {

enum LeafKind : uint8_t { BL_INT, BL_FLOAT, BL_ULEB, BL_SLEB, BL_BYTES };

struct PyBuildLeaf {
    PyObject_HEAD
    uint8_t kind;
    CaptureType type;   // BL_INT/BL_FLOAT: width+endian; BL_BYTES: String (text) or Bytes (raw)
    int64_t ival;
    double fval;
    PyObject* blob;     // BL_BYTES payload, else nullptr
};

struct PyBuildStruct {
    PyObject_HEAD
    PyObject* names;     // list[str]
    PyObject* children;  // list[value], parallel to names
};

struct PyBuildArray {
    PyObject_HEAD
    PyObject* items;     // list[value]
};

PyTypeObject PyBuildLeaf_Type   = { PyVarObject_HEAD_INIT(nullptr, 0) };
PyTypeObject PyBuildStruct_Type = { PyVarObject_HEAD_INIT(nullptr, 0) };
PyTypeObject PyBuildArray_Type  = { PyVarObject_HEAD_INIT(nullptr, 0) };

const char* leaf_type_name(const PyBuildLeaf* L) {
    switch (L->kind) {
    case BL_ULEB:  return "leb";
    case BL_SLEB:  return "sleb";
    case BL_BYTES: return L->type == CaptureType::String ? "string" : "bytes";
    default: break;
    }
    switch (L->type) {
    case CaptureType::UInt8:     return "u8";    case CaptureType::Int8:     return "i8";
    case CaptureType::UInt16LE:  return "u16le"; case CaptureType::UInt16BE: return "u16be";
    case CaptureType::Int16LE:   return "i16le"; case CaptureType::Int16BE:  return "i16be";
    case CaptureType::UInt32LE:  return "u32le"; case CaptureType::UInt32BE: return "u32be";
    case CaptureType::Int32LE:   return "i32le"; case CaptureType::Int32BE:  return "i32be";
    case CaptureType::UInt64LE:  return "u64le"; case CaptureType::UInt64BE: return "u64be";
    case CaptureType::Int64LE:   return "i64le"; case CaptureType::Int64BE:  return "i64be";
    case CaptureType::Float32LE: return "f32le"; case CaptureType::Float32BE:return "f32be";
    case CaptureType::Float64LE: return "f64le"; case CaptureType::Float64BE:return "f64be";
    default: return "?";
    }
}

}  // namespace

bool bbq_build_is_value(PyObject* o) {
    return PyObject_TypeCheck(o, &PyBuildLeaf_Type)
        || PyObject_TypeCheck(o, &PyBuildStruct_Type)
        || PyObject_TypeCheck(o, &PyBuildArray_Type);
}

bool bbq_build_serialize(PyObject* o, std::vector<uint8_t>& out) {
    if (PyObject_TypeCheck(o, &PyBuildLeaf_Type)) {
        auto* L = reinterpret_cast<PyBuildLeaf*>(o);
        switch (L->kind) {
        case BL_INT: {
            size_t w = bbq::leaf_width(L->type), p = out.size(); out.resize(p + w);
            bbq::encode_int(out.data(), p, L->type, L->ival); return true;
        }
        case BL_FLOAT: {
            size_t w = bbq::leaf_width(L->type), p = out.size(); out.resize(p + w);
            bbq::encode_float(out.data(), p, L->type, L->fval); return true;
        }
        case BL_ULEB: bbq::encode_uleb128(out, static_cast<uint64_t>(L->ival)); return true;
        case BL_SLEB: bbq::encode_sleb128(out, L->ival); return true;
        case BL_BYTES: {
            char* p; Py_ssize_t n;
            if (PyBytes_AsStringAndSize(L->blob, &p, &n) < 0) return false;
            out.insert(out.end(), reinterpret_cast<uint8_t*>(p), reinterpret_cast<uint8_t*>(p) + n);
            return true;
        }
        }
        return false;
    }
    // Containers snapshot their child list under the object's critical section and walk the
    // COPY: indexing the live list while another thread appends would read a stale item
    // array, and holding the section across the recursion would lock every level at once.
    if (PyObject_TypeCheck(o, &PyBuildStruct_Type) || PyObject_TypeCheck(o, &PyBuildArray_Type)) {
        PyObject* live = PyObject_TypeCheck(o, &PyBuildStruct_Type)
                       ? reinterpret_cast<PyBuildStruct*>(o)->children
                       : reinterpret_cast<PyBuildArray*>(o)->items;
        PyObject* kids = nullptr;
        Py_BEGIN_CRITICAL_SECTION(o);
        kids = PyList_GetSlice(live, 0, PyList_GET_SIZE(live));
        Py_END_CRITICAL_SECTION();
        if (!kids) return false;
        for (Py_ssize_t i = 0, n = PyList_GET_SIZE(kids); i < n; i++)
            if (!bbq_build_serialize(PyList_GET_ITEM(kids, i), out)) { Py_DECREF(kids); return false; }
        Py_DECREF(kids);
        return true;
    }
    if (PyBytes_Check(o)) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(PyBytes_AS_STRING(o));
        out.insert(out.end(), p, p + PyBytes_GET_SIZE(o)); return true;
    }
    if (PyByteArray_Check(o)) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(PyByteArray_AS_STRING(o));
        out.insert(out.end(), p, p + PyByteArray_GET_SIZE(o)); return true;
    }
    PyErr_Format(PyExc_TypeError, "bbq.build: cannot serialize a %.100s", Py_TYPE(o)->tp_name);
    return false;
}

namespace {

PyObject* obj_bytes(PyObject* self) {
    std::vector<uint8_t> out;
    if (!bbq_build_serialize(self, out)) return nullptr;
    return PyBytes_FromStringAndSize(reinterpret_cast<const char*>(out.data()),
                                     static_cast<Py_ssize_t>(out.size()));
}

bool check_child(PyObject* v) {
    if (bbq_build_is_value(v) || PyBytes_Check(v) || PyByteArray_Check(v)) return true;
    PyErr_Format(PyExc_TypeError,
                 "bbq.build: a child must be a build value or bytes, not %.100s", Py_TYPE(v)->tp_name);
    return false;
}

// ── Leaf ─────────────────────────────────────────────────────────────────────

void Leaf_dealloc(PyBuildLeaf* self) {
    Py_XDECREF(self->blob);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}
PyObject* Leaf_new_blocked(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError, "use the bbq.build factories (u8/u16/…/leb/raw/text)");
    return nullptr;
}
PyObject* Leaf_bytes(PyObject* self, PyObject*) { return obj_bytes(self); }
PyObject* Leaf_nb_int(PyObject* self) {
    auto* L = reinterpret_cast<PyBuildLeaf*>(self);
    if (L->kind == BL_BYTES) { PyErr_SetString(PyExc_TypeError, "bytes leaf is not an integer"); return nullptr; }
    if (L->kind == BL_FLOAT) return PyLong_FromDouble(L->fval);
    return PyLong_FromLongLong(L->ival);
}
PyObject* Leaf_nb_float(PyObject* self) {
    auto* L = reinterpret_cast<PyBuildLeaf*>(self);
    if (L->kind == BL_BYTES) { PyErr_SetString(PyExc_TypeError, "bytes leaf is not a float"); return nullptr; }
    if (L->kind == BL_FLOAT) return PyFloat_FromDouble(L->fval);
    return PyFloat_FromDouble(static_cast<double>(L->ival));
}
PyObject* Leaf_get_value(PyObject* self, void*) {
    auto* L = reinterpret_cast<PyBuildLeaf*>(self);
    if (L->kind == BL_BYTES) { Py_INCREF(L->blob); return L->blob; }
    if (L->kind == BL_FLOAT) return PyFloat_FromDouble(L->fval);
    return PyLong_FromLongLong(L->ival);
}
PyObject* Leaf_get_type(PyObject* self, void*) {
    return PyUnicode_FromString(leaf_type_name(reinterpret_cast<PyBuildLeaf*>(self)));
}
PyObject* Leaf_repr(PyObject* self) {
    PyObject* v = Leaf_get_value(self, nullptr);
    if (!v) return nullptr;
    PyObject* r = PyUnicode_FromFormat("<bbq.build.%s %R>",
                                       leaf_type_name(reinterpret_cast<PyBuildLeaf*>(self)), v);
    Py_DECREF(v);
    return r;
}

PyNumberMethods Leaf_as_number;
PyMethodDef Leaf_methods[] = {
    {"__bytes__", Leaf_bytes, METH_NOARGS, "Serialize this leaf to bytes."},
    {nullptr, nullptr, 0, nullptr}
};
PyGetSetDef Leaf_getset[] = {
    {(char*)"value", Leaf_get_value, nullptr, (char*)"the leaf's Python value", nullptr},
    {(char*)"type",  Leaf_get_type,  nullptr, (char*)"the leaf's type spelling (e.g. u32le)", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

PyBuildLeaf* new_leaf() {
    auto* L = PyObject_New(PyBuildLeaf, &PyBuildLeaf_Type);
    if (L) { L->kind = BL_INT; L->type = CaptureType::UInt8; L->ival = 0; L->fval = 0; L->blob = nullptr; }
    return L;
}
PyObject* make_int(int64_t v, CaptureType t) {
    auto* L = new_leaf(); if (!L) return nullptr;
    L->kind = BL_INT; L->type = t; L->ival = v; return reinterpret_cast<PyObject*>(L);
}
PyObject* make_float(double v, CaptureType t) {
    auto* L = new_leaf(); if (!L) return nullptr;
    L->kind = BL_FLOAT; L->type = t; L->fval = v; return reinterpret_cast<PyObject*>(L);
}

#define INT_FACTORY(fn, LE, BE)                                                          \
    PyObject* fn(PyObject*, PyObject* a, PyObject* kw) {                                 \
        long long v; const char* en = "little";                                         \
        static const char* kwl[] = {"value", "endian", nullptr};                        \
        if (!PyArg_ParseTupleAndKeywords(a, kw, "L|s", const_cast<char**>(kwl), &v, &en)) return nullptr; \
        return make_int((int64_t)v, std::strcmp(en, "big") == 0 ? BE : LE);             \
    }
#define INT1_FACTORY(fn, T)                                                             \
    PyObject* fn(PyObject*, PyObject* a, PyObject* kw) {                                 \
        long long v; const char* en = "little";                                         \
        static const char* kwl[] = {"value", "endian", nullptr};                        \
        if (!PyArg_ParseTupleAndKeywords(a, kw, "L|s", const_cast<char**>(kwl), &v, &en)) return nullptr; \
        return make_int((int64_t)v, T);                                                  \
    }
#define FLT_FACTORY(fn, LE, BE)                                                          \
    PyObject* fn(PyObject*, PyObject* a, PyObject* kw) {                                 \
        double v; const char* en = "little";                                            \
        static const char* kwl[] = {"value", "endian", nullptr};                        \
        if (!PyArg_ParseTupleAndKeywords(a, kw, "d|s", const_cast<char**>(kwl), &v, &en)) return nullptr; \
        return make_float(v, std::strcmp(en, "big") == 0 ? BE : LE);                    \
    }

INT1_FACTORY(f_u8, CaptureType::UInt8)
INT1_FACTORY(f_i8, CaptureType::Int8)
INT_FACTORY(f_u16, CaptureType::UInt16LE, CaptureType::UInt16BE)
INT_FACTORY(f_i16, CaptureType::Int16LE, CaptureType::Int16BE)
INT_FACTORY(f_u32, CaptureType::UInt32LE, CaptureType::UInt32BE)
INT_FACTORY(f_i32, CaptureType::Int32LE, CaptureType::Int32BE)
INT_FACTORY(f_u64, CaptureType::UInt64LE, CaptureType::UInt64BE)
INT_FACTORY(f_i64, CaptureType::Int64LE, CaptureType::Int64BE)
FLT_FACTORY(f_f32, CaptureType::Float32LE, CaptureType::Float32BE)
FLT_FACTORY(f_f64, CaptureType::Float64LE, CaptureType::Float64BE)

PyObject* f_leb(PyObject*, PyObject* a) {
    long long v; if (!PyArg_ParseTuple(a, "L", &v)) return nullptr;
    auto* L = new_leaf(); if (!L) return nullptr;
    L->kind = BL_ULEB; L->ival = (int64_t)v; return reinterpret_cast<PyObject*>(L);
}
PyObject* f_sleb(PyObject*, PyObject* a) {
    long long v; if (!PyArg_ParseTuple(a, "L", &v)) return nullptr;
    auto* L = new_leaf(); if (!L) return nullptr;
    L->kind = BL_SLEB; L->ival = (int64_t)v; return reinterpret_cast<PyObject*>(L);
}
PyObject* f_raw(PyObject*, PyObject* a) {
    PyObject* b; if (!PyArg_ParseTuple(a, "S", &b)) return nullptr;
    auto* L = new_leaf(); if (!L) return nullptr;
    L->kind = BL_BYTES; L->type = CaptureType::Bytes; Py_INCREF(b); L->blob = b;
    return reinterpret_cast<PyObject*>(L);
}
PyObject* f_text(PyObject*, PyObject* a) {
    const char* s; Py_ssize_t n; if (!PyArg_ParseTuple(a, "s#", &s, &n)) return nullptr;
    PyObject* b = PyBytes_FromStringAndSize(s, n); if (!b) return nullptr;
    auto* L = new_leaf(); if (!L) { Py_DECREF(b); return nullptr; }
    L->kind = BL_BYTES; L->type = CaptureType::String; L->blob = b;
    return reinterpret_cast<PyObject*>(L);
}

PyMethodDef build_factories[] = {
    {"u8",   (PyCFunction)f_u8,   METH_VARARGS | METH_KEYWORDS, "u8(value): unsigned 8-bit."},
    {"i8",   (PyCFunction)f_i8,   METH_VARARGS | METH_KEYWORDS, "i8(value): signed 8-bit."},
    {"u16",  (PyCFunction)f_u16,  METH_VARARGS | METH_KEYWORDS, "u16(value, endian='little')."},
    {"i16",  (PyCFunction)f_i16,  METH_VARARGS | METH_KEYWORDS, "i16(value, endian='little')."},
    {"u32",  (PyCFunction)f_u32,  METH_VARARGS | METH_KEYWORDS, "u32(value, endian='little')."},
    {"i32",  (PyCFunction)f_i32,  METH_VARARGS | METH_KEYWORDS, "i32(value, endian='little')."},
    {"u64",  (PyCFunction)f_u64,  METH_VARARGS | METH_KEYWORDS, "u64(value, endian='little')."},
    {"i64",  (PyCFunction)f_i64,  METH_VARARGS | METH_KEYWORDS, "i64(value, endian='little')."},
    {"f32",  (PyCFunction)f_f32,  METH_VARARGS | METH_KEYWORDS, "f32(value, endian='little')."},
    {"f64",  (PyCFunction)f_f64,  METH_VARARGS | METH_KEYWORDS, "f64(value, endian='little')."},
    {"leb",  (PyCFunction)f_leb,  METH_VARARGS, "leb(value): unsigned LEB128 varint."},
    {"sleb", (PyCFunction)f_sleb, METH_VARARGS, "sleb(value): signed LEB128 varint."},
    {"raw",  (PyCFunction)f_raw,  METH_VARARGS, "raw(b): raw bytes, verbatim."},
    {"text", (PyCFunction)f_text, METH_VARARGS, "text(s): UTF-8 bytes of a string."},
    {nullptr, nullptr, 0, nullptr}
};

// ── Struct ───────────────────────────────────────────────────────────────────

int Struct_traverse(PyBuildStruct* self, visitproc visit, void* arg) {
    Py_VISIT(self->names); Py_VISIT(self->children); return 0;
}
int Struct_clear(PyBuildStruct* self) { Py_CLEAR(self->names); Py_CLEAR(self->children); return 0; }
void Struct_dealloc(PyBuildStruct* self) {
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->names); Py_XDECREF(self->children);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}
PyObject* Struct_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    if (args && PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "bbq.build.Struct takes only keyword fields");
        return nullptr;
    }
    auto* s = reinterpret_cast<PyBuildStruct*>(type->tp_alloc(type, 0));
    if (!s) return nullptr;
    s->names = PyList_New(0);
    s->children = PyList_New(0);
    if (!s->names || !s->children) { Py_DECREF(s); return nullptr; }
    if (kwds) {
        PyObject *k, *v; Py_ssize_t pos = 0;
        while (PyDict_Next(kwds, &pos, &k, &v)) {
            if (!check_child(v)) { Py_DECREF(s); return nullptr; }
            if (PyList_Append(s->names, k) < 0 || PyList_Append(s->children, v) < 0) {
                Py_DECREF(s); return nullptr;
            }
        }
    }
    return reinterpret_cast<PyObject*>(s);
}
Py_ssize_t Struct_length(PyBuildStruct* self) { return PyList_GET_SIZE(self->names); }
// index of `name` in names: >=0 found, -1 absent, -2 error.
Py_ssize_t struct_index(PyBuildStruct* self, PyObject* name) {
    for (Py_ssize_t i = 0, n = PyList_GET_SIZE(self->names); i < n; i++) {
        int cmp = PyObject_RichCompareBool(PyList_GET_ITEM(self->names, i), name, Py_EQ);
        if (cmp < 0) return -2;
        if (cmp) return i;
    }
    return -1;
}
PyObject* Struct_subscript(PyBuildStruct* self, PyObject* key) {
    if (PyUnicode_Check(key)) {
        PyObject* v = nullptr;
        bool err = false, missing = false;
        Py_BEGIN_CRITICAL_SECTION(self);
        Py_ssize_t i = struct_index(self, key);
        if (i == -2) err = true;
        else if (i < 0) missing = true;
        else { v = PyList_GET_ITEM(self->children, i); Py_INCREF(v); }
        Py_END_CRITICAL_SECTION();
        if (err) return nullptr;
        if (missing) { PyErr_SetObject(PyExc_KeyError, key); return nullptr; }
        return v;
    }
    if (PyIndex_Check(key)) {
        Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred()) return nullptr;
        PyObject* v = nullptr;
        Py_BEGIN_CRITICAL_SECTION(self);
        Py_ssize_t n = PyList_GET_SIZE(self->children);
        Py_ssize_t j = i < 0 ? i + n : i;
        if (j >= 0 && j < n) { v = PyList_GET_ITEM(self->children, j); Py_INCREF(v); }
        Py_END_CRITICAL_SECTION();
        if (!v) { PyErr_SetString(PyExc_IndexError, "struct index out of range"); return nullptr; }
        return v;
    }
    PyErr_SetString(PyExc_TypeError, "struct index must be str or int");
    return nullptr;
}
int Struct_ass_subscript(PyBuildStruct* self, PyObject* key, PyObject* value) {
    if (!value) { PyErr_SetString(PyExc_TypeError, "cannot delete a struct field"); return -1; }
    if (!check_child(value)) return -1;
    if (PyUnicode_Check(key)) {
        int rc = 0;
        Py_BEGIN_CRITICAL_SECTION(self);
        Py_ssize_t i = struct_index(self, key);
        if (i == -2) rc = -1;
        else if (i < 0) {  // new field → append; BOTH lists, or the pair desynchronizes
            if (PyList_Append(self->names, key) < 0) rc = -1;
            else rc = PyList_Append(self->children, value);
        } else {
            Py_INCREF(value);
            rc = PyList_SetItem(self->children, i, value);  // steals the ref, drops the old
        }
        Py_END_CRITICAL_SECTION();
        return rc;
    }
    if (PyIndex_Check(key)) {
        Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred()) return -1;
        int rc = 0;
        bool oob = false;
        Py_BEGIN_CRITICAL_SECTION(self);
        Py_ssize_t n = PyList_GET_SIZE(self->children);
        Py_ssize_t j = i < 0 ? i + n : i;
        if (j < 0 || j >= n) oob = true;
        else { Py_INCREF(value); rc = PyList_SetItem(self->children, j, value); }
        Py_END_CRITICAL_SECTION();
        if (oob) { PyErr_SetString(PyExc_IndexError, "struct index out of range"); return -1; }
        return rc;
    }
    PyErr_SetString(PyExc_TypeError, "struct index must be str or int");
    return -1;
}
PyObject* Struct_getattro(PyBuildStruct* self, PyObject* name) {
    PyObject* attr = PyObject_GenericGetAttr(reinterpret_cast<PyObject*>(self), name);
    if (attr) return attr;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return nullptr;
    PyObject* v = nullptr;
    bool err = false;
    Py_BEGIN_CRITICAL_SECTION(self);
    Py_ssize_t i = struct_index(self, name);
    if (i == -2) err = true;
    else if (i >= 0) { v = PyList_GET_ITEM(self->children, i); Py_INCREF(v); }
    Py_END_CRITICAL_SECTION();
    if (err) return nullptr;
    if (v) { PyErr_Clear(); return v; }
    return nullptr;  // keep the AttributeError
}
int Struct_setattro(PyBuildStruct* self, PyObject* name, PyObject* value) {
    return Struct_ass_subscript(self, name, value);
}
int Struct_contains(PyBuildStruct* self, PyObject* value) {
    if (!PyUnicode_Check(value)) return 0;
    Py_ssize_t i;
    Py_BEGIN_CRITICAL_SECTION(self);
    i = struct_index(self, value);
    Py_END_CRITICAL_SECTION();
    if (i == -2) return -1;
    return i >= 0 ? 1 : 0;
}
PyObject* Struct_keys(PyBuildStruct* self, PyObject*) {
    PyObject* out;
    Py_BEGIN_CRITICAL_SECTION(self);
    out = PyList_GetSlice(self->names, 0, PyList_GET_SIZE(self->names));
    Py_END_CRITICAL_SECTION();
    return out;
}
PyObject* Struct_values(PyBuildStruct* self, PyObject*) {
    PyObject* out;
    Py_BEGIN_CRITICAL_SECTION(self);
    out = PyList_GetSlice(self->children, 0, PyList_GET_SIZE(self->children));
    Py_END_CRITICAL_SECTION();
    return out;
}
// Zips the two lists by index, so it must see them at one instant — the pair's whole
// invariant is that names[i] goes with children[i].
PyObject* Struct_items(PyBuildStruct* self, PyObject*) {
    PyObject* out = nullptr;
    Py_BEGIN_CRITICAL_SECTION(self);
    Py_ssize_t n = PyList_GET_SIZE(self->names);
    out = PyList_New(n);
    if (out) {
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* t = PyTuple_Pack(2, PyList_GET_ITEM(self->names, i),
                                          PyList_GET_ITEM(self->children, i));
            if (!t) { Py_CLEAR(out); break; }
            PyList_SET_ITEM(out, i, t);
        }
    }
    Py_END_CRITICAL_SECTION();
    return out;
}
PyObject* Struct_iter(PyBuildStruct* self) {
    PyObject* items = Struct_items(self, nullptr);
    if (!items) return nullptr;
    PyObject* it = PyObject_GetIter(items);
    Py_DECREF(items);
    return it;
}
PyObject* Struct_bytes(PyObject* self, PyObject*) { return obj_bytes(self); }
PyObject* Struct_repr(PyObject* self) {
    return PyUnicode_FromFormat("<bbq.build.Struct %zd fields>",
                                PyList_GET_SIZE(reinterpret_cast<PyBuildStruct*>(self)->names));
}
PyMappingMethods Struct_as_mapping;
PySequenceMethods Struct_as_sequence;
PyMethodDef Struct_methods[] = {
    {"keys",   (PyCFunction)Struct_keys,   METH_NOARGS, "Field names."},
    {"values", (PyCFunction)Struct_values, METH_NOARGS, "Field values."},
    {"items",  (PyCFunction)Struct_items,  METH_NOARGS, "(name, value) pairs."},
    {"__bytes__", Struct_bytes, METH_NOARGS, "Serialize to bytes."},
    {nullptr, nullptr, 0, nullptr}
};

// ── Array ────────────────────────────────────────────────────────────────────

int Array_traverse(PyBuildArray* self, visitproc visit, void* arg) { Py_VISIT(self->items); return 0; }
int Array_clear(PyBuildArray* self) { Py_CLEAR(self->items); return 0; }
void Array_dealloc(PyBuildArray* self) {
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->items);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}
PyObject* Array_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    if (kwds && PyDict_GET_SIZE(kwds) != 0) {
        PyErr_SetString(PyExc_TypeError, "bbq.build.Array takes no keywords");
        return nullptr;
    }
    // Array(*elems) or Array([elems]) — a lone list/tuple arg is the element sequence.
    PyObject* src = args; bool owned = false;
    if (PyTuple_GET_SIZE(args) == 1) {
        PyObject* a0 = PyTuple_GET_ITEM(args, 0);
        if (PyList_Check(a0) || PyTuple_Check(a0)) {
            src = PySequence_Fast(a0, "expected a sequence");
            if (!src) return nullptr;
            owned = true;
        }
    }
    auto* arr = reinterpret_cast<PyBuildArray*>(type->tp_alloc(type, 0));
    if (!arr) { if (owned) Py_DECREF(src); return nullptr; }
    arr->items = PyList_New(0);
    if (!arr->items) { Py_DECREF(arr); if (owned) Py_DECREF(src); return nullptr; }
    for (Py_ssize_t i = 0, n = PySequence_Fast_GET_SIZE(src); i < n; i++) {
        PyObject* v = PySequence_Fast_GET_ITEM(src, i);
        if (!check_child(v) || PyList_Append(arr->items, v) < 0) {
            Py_DECREF(arr); if (owned) Py_DECREF(src); return nullptr;
        }
    }
    if (owned) Py_DECREF(src);
    return reinterpret_cast<PyObject*>(arr);
}
Py_ssize_t Array_length(PyBuildArray* self) { return PyList_GET_SIZE(self->items); }
PyObject* Array_subscript(PyBuildArray* self, PyObject* key) {
    return PyObject_GetItem(self->items, key);  // int / slice → list semantics
}
int Array_ass_subscript(PyBuildArray* self, PyObject* key, PyObject* value) {
    if (!value) return PyObject_DelItem(self->items, key);
    if (!check_child(value)) return -1;
    return PyObject_SetItem(self->items, key, value);
}
PyObject* Array_append(PyBuildArray* self, PyObject* value) {
    if (!check_child(value)) return nullptr;
    if (PyList_Append(self->items, value) < 0) return nullptr;
    Py_RETURN_NONE;
}
PyObject* Array_iter(PyBuildArray* self) { return PyObject_GetIter(self->items); }
PyObject* Array_bytes(PyObject* self, PyObject*) { return obj_bytes(self); }
PyObject* Array_repr(PyObject* self) {
    return PyUnicode_FromFormat("<bbq.build.Array %zd elems>",
                                PyList_GET_SIZE(reinterpret_cast<PyBuildArray*>(self)->items));
}
PyMappingMethods Array_as_mapping;
PyMethodDef Array_methods[] = {
    {"append", (PyCFunction)Array_append, METH_O, "Append an element."},
    {"__bytes__", Array_bytes, METH_NOARGS, "Serialize to bytes."},
    {nullptr, nullptr, 0, nullptr}
};

}  // namespace

PyObject* bbq_build_create_module(void) {
    Leaf_as_number.nb_int = Leaf_nb_int;
    Leaf_as_number.nb_float = Leaf_nb_float;
    PyBuildLeaf_Type.tp_name = "bbq.build.Leaf";
    PyBuildLeaf_Type.tp_basicsize = sizeof(PyBuildLeaf);
    PyBuildLeaf_Type.tp_dealloc = (destructor)Leaf_dealloc;
    PyBuildLeaf_Type.tp_repr = Leaf_repr;
    PyBuildLeaf_Type.tp_as_number = &Leaf_as_number;
    PyBuildLeaf_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    PyBuildLeaf_Type.tp_methods = Leaf_methods;
    PyBuildLeaf_Type.tp_getset = Leaf_getset;
    PyBuildLeaf_Type.tp_new = Leaf_new_blocked;
    if (PyType_Ready(&PyBuildLeaf_Type) < 0) return nullptr;

    Struct_as_mapping.mp_length = (lenfunc)Struct_length;
    Struct_as_mapping.mp_subscript = (binaryfunc)Struct_subscript;
    Struct_as_mapping.mp_ass_subscript = (objobjargproc)Struct_ass_subscript;
    Struct_as_sequence.sq_contains = (objobjproc)Struct_contains;
    PyBuildStruct_Type.tp_name = "bbq.build.Struct";
    PyBuildStruct_Type.tp_basicsize = sizeof(PyBuildStruct);
    PyBuildStruct_Type.tp_dealloc = (destructor)Struct_dealloc;
    PyBuildStruct_Type.tp_repr = Struct_repr;
    PyBuildStruct_Type.tp_as_mapping = &Struct_as_mapping;
    PyBuildStruct_Type.tp_as_sequence = &Struct_as_sequence;
    PyBuildStruct_Type.tp_getattro = (getattrofunc)Struct_getattro;
    PyBuildStruct_Type.tp_setattro = (setattrofunc)Struct_setattro;
    PyBuildStruct_Type.tp_iter = (getiterfunc)Struct_iter;
    PyBuildStruct_Type.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
    PyBuildStruct_Type.tp_traverse = (traverseproc)Struct_traverse;
    PyBuildStruct_Type.tp_clear = (inquiry)Struct_clear;
    PyBuildStruct_Type.tp_methods = Struct_methods;
    PyBuildStruct_Type.tp_new = Struct_new;
    if (PyType_Ready(&PyBuildStruct_Type) < 0) return nullptr;

    Array_as_mapping.mp_length = (lenfunc)Array_length;
    Array_as_mapping.mp_subscript = (binaryfunc)Array_subscript;
    Array_as_mapping.mp_ass_subscript = (objobjargproc)Array_ass_subscript;
    PyBuildArray_Type.tp_name = "bbq.build.Array";
    PyBuildArray_Type.tp_basicsize = sizeof(PyBuildArray);
    PyBuildArray_Type.tp_dealloc = (destructor)Array_dealloc;
    PyBuildArray_Type.tp_repr = Array_repr;
    PyBuildArray_Type.tp_as_mapping = &Array_as_mapping;
    PyBuildArray_Type.tp_iter = (getiterfunc)Array_iter;
    PyBuildArray_Type.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
    PyBuildArray_Type.tp_traverse = (traverseproc)Array_traverse;
    PyBuildArray_Type.tp_clear = (inquiry)Array_clear;
    PyBuildArray_Type.tp_methods = Array_methods;
    PyBuildArray_Type.tp_new = Array_new;
    if (PyType_Ready(&PyBuildArray_Type) < 0) return nullptr;

    static PyModuleDef build_def = {
        PyModuleDef_HEAD_INIT, "bbq.build",
        "Grammar-free byte construction: typed leaf factories + Struct/Array.",
        -1, build_factories, nullptr, nullptr, nullptr, nullptr
    };
    PyObject* m = PyModule_Create(&build_def);
    if (!m) return nullptr;
    Py_INCREF(&PyBuildStruct_Type);
    Py_INCREF(&PyBuildArray_Type);
    if (PyModule_AddObject(m, "Struct", (PyObject*)&PyBuildStruct_Type) < 0 ||
        PyModule_AddObject(m, "Array",  (PyObject*)&PyBuildArray_Type) < 0) {
        Py_DECREF(m);
        return nullptr;
    }
    return m;
}
