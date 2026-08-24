// bbq.build — construction from nothing (see bbq_build.h).
//
// A constructed value IS a bbq::zcow node: the same node a parse makes, minus the span,
// because a span is what a node was read from and this one was not read from anything.
// So there is no tree here and no serializer here — `bytes(x)` is document::serialize(),
// the one writer, and handing a value to a parsed document grafts it in as structure.
//
// What is here is the spelling: typed leaf factories, and two containers that say
// whether their children are named. Nothing in this file knows a grammar.

#define PY_SSIZE_T_CLEAN
#include "bbq_build.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

using bbq::CaptureType;
namespace zc = bbq::zcow;

// A Struct's children live in the node's `kids`, and every accessor is a check-then-act
// over that vector: find the index, then index into it. Critical sections (rather than
// the plain mutex bbq_python.cpp uses) are right here — the invariant belongs to the
// Python object, and the guarded code can call back into the interpreter (rich
// comparison), which a critical section tolerates. Below 3.13 the macros do not exist
// and the GIL already provides this.
#if PY_VERSION_HEX < 0x030D0000
#  define Py_BEGIN_CRITICAL_SECTION(op) {
#  define Py_END_CRITICAL_SECTION() }
#endif

namespace {

// A node's name is a `const char*` it does not own — the parse side points at the
// grammar's interned copy. Constructed names come from Python strings that will not
// outlive the call, so they are interned here, which is the same answer for the same
// reason. Field names repeat heavily across constructed values, so the pool is small.
const char* intern(const char* s) {
    static std::mutex m;
    static std::unordered_set<std::string> pool;
    std::lock_guard<std::mutex> g(m);
    return pool.insert(s).first->c_str();
}

struct PyBuildValue {
    PyObject_HEAD
    zc::node_ptr* np;
};

PyTypeObject PyBuildLeaf_Type   = { PyVarObject_HEAD_INIT(nullptr, 0) };
PyTypeObject PyBuildStruct_Type = { PyVarObject_HEAD_INIT(nullptr, 0) };
PyTypeObject PyBuildArray_Type  = { PyVarObject_HEAD_INIT(nullptr, 0) };

zc::node& nd(PyObject* o) { return **reinterpret_cast<PyBuildValue*>(o)->np; }
const zc::node_ptr& ndp(PyObject* o) { return *reinterpret_cast<PyBuildValue*>(o)->np; }

zc::node_ptr new_node(CaptureType t) {
    auto n = std::make_shared<zc::node>();
    n->type = t;
    n->parsed = false;      // it names no bytes of anything: there is no input
    return n;
}

// Wrap an existing node as the build type that matches its shape.
PyObject* wrap(zc::node_ptr n) {
    PyTypeObject* t = n->type == CaptureType::Struct ? &PyBuildStruct_Type
                    : n->type == CaptureType::Array  ? &PyBuildArray_Type
                                                     : &PyBuildLeaf_Type;
    auto* v = reinterpret_cast<PyBuildValue*>(t->tp_alloc(t, 0));
    if (!v) return nullptr;
    v->np = new zc::node_ptr(std::move(n));
    return reinterpret_cast<PyObject*>(v);
}

PyObject* make_leaf(zc::node_ptr n) { return wrap(std::move(n)); }

void Value_dealloc(PyObject* self) {
    delete reinterpret_cast<PyBuildValue*>(self)->np;
    Py_TYPE(self)->tp_free(self);
}

const char* leaf_type_name(const zc::node& n) {
    if (n.enc == zc::Enc::Uleb) return "leb";
    if (n.enc == zc::Enc::Sleb) return "sleb";
    switch (n.type) {
    case CaptureType::String:    return "string"; case CaptureType::Bytes:    return "bytes";
    case CaptureType::UInt8:     return "u8";     case CaptureType::Int8:     return "i8";
    case CaptureType::UInt16LE:  return "u16le";  case CaptureType::UInt16BE: return "u16be";
    case CaptureType::Int16LE:   return "i16le";  case CaptureType::Int16BE:  return "i16be";
    case CaptureType::UInt32LE:  return "u32le";  case CaptureType::UInt32BE: return "u32be";
    case CaptureType::Int32LE:   return "i32le";  case CaptureType::Int32BE:  return "i32be";
    case CaptureType::UInt64LE:  return "u64le";  case CaptureType::UInt64BE: return "u64be";
    case CaptureType::Int64LE:   return "i64le";  case CaptureType::Int64BE:  return "i64be";
    case CaptureType::Float32LE: return "f32le";  case CaptureType::Float32BE:return "f32be";
    case CaptureType::Float64LE: return "f64le";  case CaptureType::Float64BE:return "f64be";
    case CaptureType::Struct:    return "struct"; case CaptureType::Array:    return "array";
    default: return "?";
    }
}

bool is_bytes_leaf(const zc::node& n) {
    return n.type == CaptureType::Bytes || n.type == CaptureType::String;
}

PyObject* obj_bytes(PyObject* self) {
    // The one writer. A source-less document: every node here owns what it holds, so
    // there are no spans to blit and nothing to patch into.
    std::vector<uint8_t> out;
    try {
        out = zc::document(ndp(self), nullptr).serialize();
    } catch (const std::exception& e) {
        PyErr_Format(PyExc_ValueError, "bbq.build: %s", e.what());
        return nullptr;
    }
    return PyBytes_FromStringAndSize(reinterpret_cast<const char*>(out.data()),
                                     static_cast<Py_ssize_t>(out.size()));
}

// A value being taken INTO a container takes what it is now: the node is copied so the
// parent can name it without renaming it everywhere else it was used. The copy is
// shallow — the subtree below is shared, which is what sharing means here, and a write
// down into it clones on the way (owner id).
zc::node_ptr adopt_child(PyObject* v, const char* name) {
    zc::node_ptr n = bbq_build_node(v);
    if (!n) return nullptr;
    auto copy = std::make_shared<zc::node>(*n);
    copy->name = name ? intern(name) : nullptr;
    return copy;
}

bool check_child(PyObject* v) {
    if (bbq_build_is_value(v) || PyBytes_Check(v) || PyByteArray_Check(v)) return true;
    PyErr_Format(PyExc_TypeError,
                 "bbq.build: a child must be a build value or bytes, not %.100s",
                 Py_TYPE(v)->tp_name);
    return false;
}

// ── Leaf ─────────────────────────────────────────────────────────────────────

PyObject* Leaf_new_blocked(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError, "use the bbq.build factories (u8/u16/…/leb/raw/text)");
    return nullptr;
}
PyObject* Leaf_bytes(PyObject* self, PyObject*) { return obj_bytes(self); }

PyObject* Leaf_nb_int(PyObject* self) {
    const zc::node& n = nd(self);
    if (is_bytes_leaf(n)) { PyErr_SetString(PyExc_TypeError, "bytes leaf is not an integer"); return nullptr; }
    if (bbq::is_float_type(n.type)) return PyLong_FromDouble(n.fval);
    return PyLong_FromLongLong(n.ival);
}
PyObject* Leaf_nb_float(PyObject* self) {
    const zc::node& n = nd(self);
    if (is_bytes_leaf(n)) { PyErr_SetString(PyExc_TypeError, "bytes leaf is not a float"); return nullptr; }
    if (bbq::is_float_type(n.type)) return PyFloat_FromDouble(n.fval);
    return PyFloat_FromDouble(static_cast<double>(n.ival));
}
PyObject* Leaf_get_value(PyObject* self, void*) {
    const zc::node& n = nd(self);
    if (is_bytes_leaf(n))
        return PyBytes_FromStringAndSize(reinterpret_cast<const char*>(n.bval.data()),
                                         static_cast<Py_ssize_t>(n.bval.size()));
    if (bbq::is_float_type(n.type)) return PyFloat_FromDouble(n.fval);
    return PyLong_FromLongLong(n.ival);
}
PyObject* Leaf_get_type(PyObject* self, void*) {
    return PyUnicode_FromString(leaf_type_name(nd(self)));
}
PyObject* Leaf_repr(PyObject* self) {
    PyObject* v = Leaf_get_value(self, nullptr);
    if (!v) return nullptr;
    PyObject* r = PyUnicode_FromFormat("<bbq.build.%s %R>", leaf_type_name(nd(self)), v);
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

PyObject* make_int(int64_t v, CaptureType t) {
    zc::node_ptr n = new_node(t);
    zc::set_int(n.get(), v);
    return make_leaf(std::move(n));
}
PyObject* make_float(double v, CaptureType t) {
    zc::node_ptr n = new_node(t);
    zc::set_float(n.get(), v);
    return make_leaf(std::move(n));
}
PyObject* make_varint(int64_t v, zc::Enc e) {
    zc::node_ptr n = new_node(CaptureType::Computed);
    zc::set_int(n.get(), v, e);
    return make_leaf(std::move(n));
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
    return make_varint((int64_t)v, zc::Enc::Uleb);
}
PyObject* f_sleb(PyObject*, PyObject* a) {
    long long v; if (!PyArg_ParseTuple(a, "L", &v)) return nullptr;
    return make_varint((int64_t)v, zc::Enc::Sleb);
}
PyObject* f_raw(PyObject*, PyObject* a) {
    PyObject* b; if (!PyArg_ParseTuple(a, "S", &b)) return nullptr;
    zc::node_ptr n = new_node(CaptureType::Bytes);
    zc::set_bytes(n.get(), reinterpret_cast<const uint8_t*>(PyBytes_AS_STRING(b)),
                  (size_t)PyBytes_GET_SIZE(b));
    return make_leaf(std::move(n));
}
PyObject* f_text(PyObject*, PyObject* a) {
    const char* s; Py_ssize_t len; if (!PyArg_ParseTuple(a, "s#", &s, &len)) return nullptr;
    zc::node_ptr n = new_node(CaptureType::String);
    zc::set_str(n.get(), std::string_view(s, (size_t)len));
    return make_leaf(std::move(n));
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

// ── Containers ───────────────────────────────────────────────────────────────

// index of `name` among the children: >=0 found, -1 absent, -2 error.
Py_ssize_t field_index(PyObject* self, PyObject* name) {
    const char* want = PyUnicode_AsUTF8(name);
    if (!want) return -2;
    const zc::node& n = nd(self);
    for (size_t i = 0; i < n.kids.size(); i++)
        if (n.kids[i]->name && std::strcmp(n.kids[i]->name, want) == 0) return (Py_ssize_t)i;
    return -1;
}

Py_ssize_t Value_length(PyObject* self) { return (Py_ssize_t)nd(self).kids.size(); }

PyObject* child_at(PyObject* self, Py_ssize_t i) {
    zc::node_ptr k;
    Py_BEGIN_CRITICAL_SECTION(self);
    const zc::node& n = nd(self);
    Py_ssize_t j = i < 0 ? i + (Py_ssize_t)n.kids.size() : i;
    if (j >= 0 && j < (Py_ssize_t)n.kids.size()) k = n.kids[(size_t)j];
    Py_END_CRITICAL_SECTION();
    if (!k) { PyErr_SetString(PyExc_IndexError, "index out of range"); return nullptr; }
    return wrap(std::move(k));
}

// Put `value` at slot `i`, or append it when `i` is past the end.
int put_child(PyObject* self, Py_ssize_t i, PyObject* value, const char* name) {
    zc::node_ptr child = adopt_child(value, name);   // may run Python — before the section
    if (!child) return -1;
    int rc = 0;
    Py_BEGIN_CRITICAL_SECTION(self);
    zc::node& n = nd(self);
    if (i < 0 || i >= (Py_ssize_t)n.kids.size()) n.kids.push_back(std::move(child));
    else                                         n.kids[(size_t)i] = std::move(child);
    Py_END_CRITICAL_SECTION();
    return rc;
}

// Assign a NAMED field: look the name up and place the value in one step. Splitting the
// two is check-then-act — two threads both find the name absent and both append it, and
// the struct ends up with the field twice. So the conversion (which can run Python, and
// so cannot be inside a critical section) happens first, and what is left is one
// indivisible lookup-and-place.
int put_named(PyObject* self, PyObject* key, const char* name, PyObject* value) {
    zc::node_ptr child = adopt_child(value, name);   // may run Python — before the section
    if (!child) return -1;
    int rc = 0;
    Py_BEGIN_CRITICAL_SECTION(self);
    zc::node& n = nd(self);
    Py_ssize_t i = field_index(self, key);
    if (i == -2)     rc = -1;
    else if (i < 0)  n.kids.push_back(std::move(child));
    else             n.kids[(size_t)i] = std::move(child);
    Py_END_CRITICAL_SECTION();
    return rc;
}

// ── Struct ───────────────────────────────────────────────────────────────────

PyObject* Struct_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    if (args && PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "bbq.build.Struct takes only keyword fields");
        return nullptr;
    }
    PyObject* self = wrap(new_node(CaptureType::Struct));
    if (!self) return nullptr;
    if (kwds) {
        PyObject *k, *v; Py_ssize_t pos = 0;
        while (PyDict_Next(kwds, &pos, &k, &v)) {
            const char* name = PyUnicode_AsUTF8(k);
            if (!name || !check_child(v) || put_child(self, -1, v, name) < 0) {
                Py_DECREF(self); return nullptr;
            }
        }
    }
    return self;
}

PyObject* Struct_subscript(PyObject* self, PyObject* key) {
    if (PyUnicode_Check(key)) {
        Py_ssize_t i;
        Py_BEGIN_CRITICAL_SECTION(self);
        i = field_index(self, key);
        Py_END_CRITICAL_SECTION();
        if (i == -2) return nullptr;
        if (i < 0) { PyErr_SetObject(PyExc_KeyError, key); return nullptr; }
        return child_at(self, i);
    }
    if (PyIndex_Check(key)) {
        Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred()) return nullptr;
        return child_at(self, i);
    }
    PyErr_SetString(PyExc_TypeError, "struct index must be str or int");
    return nullptr;
}

int Struct_ass_subscript(PyObject* self, PyObject* key, PyObject* value) {
    if (!value) { PyErr_SetString(PyExc_TypeError, "cannot delete a struct field"); return -1; }
    if (!check_child(value)) return -1;
    if (PyUnicode_Check(key)) {
        const char* name = PyUnicode_AsUTF8(key);
        if (!name) return -1;
        return put_named(self, key, name, value);
    }
    if (PyIndex_Check(key)) {
        Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred()) return -1;
        const char* name = nullptr;
        Py_BEGIN_CRITICAL_SECTION(self);
        const zc::node& n = nd(self);
        Py_ssize_t j = i < 0 ? i + (Py_ssize_t)n.kids.size() : i;
        if (j >= 0 && j < (Py_ssize_t)n.kids.size()) { name = n.kids[(size_t)j]->name; i = j; }
        else i = -1;
        Py_END_CRITICAL_SECTION();
        if (i < 0) { PyErr_SetString(PyExc_IndexError, "struct index out of range"); return -1; }
        return put_child(self, i, value, name);          // keeps the field's name
    }
    PyErr_SetString(PyExc_TypeError, "struct index must be str or int");
    return -1;
}

PyObject* Struct_getattro(PyObject* self, PyObject* name) {
    PyObject* attr = PyObject_GenericGetAttr(self, name);
    if (attr) return attr;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return nullptr;
    Py_ssize_t i;
    Py_BEGIN_CRITICAL_SECTION(self);
    i = field_index(self, name);
    Py_END_CRITICAL_SECTION();
    if (i == -2) return nullptr;
    if (i < 0) return nullptr;   // keep the AttributeError
    PyErr_Clear();
    return child_at(self, i);
}

int Struct_setattro(PyObject* self, PyObject* name, PyObject* value) {
    return Struct_ass_subscript(self, name, value);
}

int Struct_contains(PyObject* self, PyObject* value) {
    if (!PyUnicode_Check(value)) return 0;
    Py_ssize_t i;
    Py_BEGIN_CRITICAL_SECTION(self);
    i = field_index(self, value);
    Py_END_CRITICAL_SECTION();
    if (i == -2) return -1;
    return i >= 0 ? 1 : 0;
}

// The names, and the slots they sit in, read at one instant.
bool field_slots(PyObject* self, std::vector<std::pair<Py_ssize_t, std::string>>* out) {
    Py_BEGIN_CRITICAL_SECTION(self);
    const zc::node& n = nd(self);
    for (size_t i = 0; i < n.kids.size(); i++)
        if (n.kids[i]->name) out->emplace_back((Py_ssize_t)i, n.kids[i]->name);
    Py_END_CRITICAL_SECTION();
    return true;
}

PyObject* Struct_keys(PyObject* self, PyObject*) {
    std::vector<std::pair<Py_ssize_t, std::string>> slots;
    field_slots(self, &slots);
    PyObject* out = PyList_New(0);
    if (!out) return nullptr;
    for (const auto& s : slots) {
        PyObject* k = PyUnicode_FromString(s.second.c_str());
        if (!k || PyList_Append(out, k) < 0) { Py_XDECREF(k); Py_DECREF(out); return nullptr; }
        Py_DECREF(k);
    }
    return out;
}

PyObject* Struct_values(PyObject* self, PyObject*) {
    std::vector<std::pair<Py_ssize_t, std::string>> slots;
    field_slots(self, &slots);
    PyObject* out = PyList_New(0);
    if (!out) return nullptr;
    for (const auto& s : slots) {
        PyObject* v = child_at(self, s.first);
        if (!v || PyList_Append(out, v) < 0) { Py_XDECREF(v); Py_DECREF(out); return nullptr; }
        Py_DECREF(v);
    }
    return out;
}

PyObject* Struct_items(PyObject* self, PyObject*) {
    std::vector<std::pair<Py_ssize_t, std::string>> slots;
    field_slots(self, &slots);
    PyObject* out = PyList_New(0);
    if (!out) return nullptr;
    for (const auto& s : slots) {
        PyObject* k = PyUnicode_FromString(s.second.c_str());
        if (!k) { Py_DECREF(out); return nullptr; }
        PyObject* v = child_at(self, s.first);
        if (!v) { Py_DECREF(k); Py_DECREF(out); return nullptr; }
        PyObject* t = PyTuple_Pack(2, k, v);
        Py_DECREF(k); Py_DECREF(v);
        if (!t || PyList_Append(out, t) < 0) { Py_XDECREF(t); Py_DECREF(out); return nullptr; }
        Py_DECREF(t);
    }
    return out;
}

PyObject* Struct_iter(PyObject* self) {
    PyObject* items = Struct_items(self, nullptr);
    if (!items) return nullptr;
    PyObject* it = PyObject_GetIter(items);
    Py_DECREF(items);
    return it;
}

PyObject* Struct_bytes(PyObject* self, PyObject*) { return obj_bytes(self); }
PyObject* Struct_repr(PyObject* self) {
    return PyUnicode_FromFormat("<bbq.build.Struct %zd fields>", Value_length(self));
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
    PyObject* self = wrap(new_node(CaptureType::Array));
    if (!self) { if (owned) Py_DECREF(src); return nullptr; }
    for (Py_ssize_t i = 0, n = PySequence_Fast_GET_SIZE(src); i < n; i++) {
        PyObject* v = PySequence_Fast_GET_ITEM(src, i);
        if (!check_child(v) || put_child(self, -1, v, nullptr) < 0) {
            Py_DECREF(self); if (owned) Py_DECREF(src); return nullptr;
        }
    }
    if (owned) Py_DECREF(src);
    return self;
}

PyObject* Array_subscript(PyObject* self, PyObject* key) {
    if (PySlice_Check(key)) {
        Py_ssize_t start, stop, step, len;
        if (PySlice_GetIndicesEx(key, Value_length(self), &start, &stop, &step, &len) < 0)
            return nullptr;
        PyObject* out = PyList_New(len);
        if (!out) return nullptr;
        for (Py_ssize_t i = 0, j = start; i < len; i++, j += step) {
            PyObject* v = child_at(self, j);
            if (!v) { Py_DECREF(out); return nullptr; }
            PyList_SET_ITEM(out, i, v);
        }
        return out;
    }
    if (PyIndex_Check(key)) {
        Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred()) return nullptr;
        return child_at(self, i);
    }
    PyErr_SetString(PyExc_TypeError, "array index must be an integer or a slice");
    return nullptr;
}

int Array_ass_subscript(PyObject* self, PyObject* key, PyObject* value) {
    if (!PyIndex_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "array index must be an integer");
        return -1;
    }
    Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) return -1;

    Py_ssize_t n = Value_length(self);
    Py_ssize_t j = i < 0 ? i + n : i;
    if (j < 0 || j >= n) { PyErr_SetString(PyExc_IndexError, "index out of range"); return -1; }

    if (!value) {   // del arr[i]
        Py_BEGIN_CRITICAL_SECTION(self);
        zc::node& nn = nd(self);
        if (j < (Py_ssize_t)nn.kids.size()) nn.kids.erase(nn.kids.begin() + j);
        Py_END_CRITICAL_SECTION();
        return 0;
    }
    if (!check_child(value)) return -1;
    return put_child(self, j, value, nullptr);
}

PyObject* Array_append(PyObject* self, PyObject* value) {
    if (!check_child(value)) return nullptr;
    if (put_child(self, -1, value, nullptr) < 0) return nullptr;
    Py_RETURN_NONE;
}

PyObject* Array_iter(PyObject* self) {
    PyObject* all = PyList_New(0);
    if (!all) return nullptr;
    for (Py_ssize_t i = 0, n = Value_length(self); i < n; i++) {
        PyObject* v = child_at(self, i);
        if (!v || PyList_Append(all, v) < 0) { Py_XDECREF(v); Py_DECREF(all); return nullptr; }
        Py_DECREF(v);
    }
    PyObject* it = PyObject_GetIter(all);
    Py_DECREF(all);
    return it;
}

PyObject* Array_bytes(PyObject* self, PyObject*) { return obj_bytes(self); }
PyObject* Array_repr(PyObject* self) {
    return PyUnicode_FromFormat("<bbq.build.Array %zd elems>", Value_length(self));
}

PyMappingMethods Array_as_mapping;
PyMethodDef Array_methods[] = {
    {"append", (PyCFunction)Array_append, METH_O, "Append an element."},
    {"__bytes__", Array_bytes, METH_NOARGS, "Serialize to bytes."},
    {nullptr, nullptr, 0, nullptr}
};

}  // namespace


bool bbq_build_is_value(PyObject* o) {
    return PyObject_TypeCheck(o, &PyBuildLeaf_Type)
        || PyObject_TypeCheck(o, &PyBuildStruct_Type)
        || PyObject_TypeCheck(o, &PyBuildArray_Type);
}

zc::node_ptr bbq_build_node(PyObject* o) {
    if (bbq_build_is_value(o)) return ndp(o);
    if (PyBytes_Check(o) || PyByteArray_Check(o)) {
        const uint8_t* p; size_t len;
        if (PyBytes_Check(o)) {
            p = reinterpret_cast<const uint8_t*>(PyBytes_AS_STRING(o));
            len = (size_t)PyBytes_GET_SIZE(o);
        } else {
            p = reinterpret_cast<const uint8_t*>(PyByteArray_AS_STRING(o));
            len = (size_t)PyByteArray_GET_SIZE(o);
        }
        zc::node_ptr n = new_node(CaptureType::Bytes);
        zc::set_bytes(n.get(), p, len);
        return n;
    }
    PyErr_Format(PyExc_TypeError, "bbq.build: cannot take a %.100s", Py_TYPE(o)->tp_name);
    return nullptr;
}

PyObject* bbq_build_create_module(void) {
    static PyModuleDef def = {
        PyModuleDef_HEAD_INIT, "bbq.build",
        PyDoc_STR("Construct binary content from nothing: typed leaves, Struct, Array."),
        -1, build_factories, nullptr, nullptr, nullptr, nullptr
    };

    Leaf_as_number.nb_int = Leaf_nb_int;
    Leaf_as_number.nb_float = Leaf_nb_float;

    PyBuildLeaf_Type.tp_name = "bbq.build.Leaf";
    PyBuildLeaf_Type.tp_basicsize = sizeof(PyBuildValue);
    PyBuildLeaf_Type.tp_dealloc = Value_dealloc;
    PyBuildLeaf_Type.tp_repr = Leaf_repr;
    PyBuildLeaf_Type.tp_as_number = &Leaf_as_number;
    PyBuildLeaf_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    PyBuildLeaf_Type.tp_doc = PyDoc_STR("A typed scalar or byte string.");
    PyBuildLeaf_Type.tp_methods = Leaf_methods;
    PyBuildLeaf_Type.tp_getset = Leaf_getset;
    PyBuildLeaf_Type.tp_new = Leaf_new_blocked;

    Struct_as_mapping.mp_length = Value_length;
    Struct_as_mapping.mp_subscript = Struct_subscript;
    Struct_as_mapping.mp_ass_subscript = Struct_ass_subscript;
    Struct_as_sequence.sq_contains = Struct_contains;

    PyBuildStruct_Type.tp_name = "bbq.build.Struct";
    PyBuildStruct_Type.tp_basicsize = sizeof(PyBuildValue);
    PyBuildStruct_Type.tp_dealloc = Value_dealloc;
    PyBuildStruct_Type.tp_repr = Struct_repr;
    PyBuildStruct_Type.tp_as_mapping = &Struct_as_mapping;
    PyBuildStruct_Type.tp_as_sequence = &Struct_as_sequence;
    PyBuildStruct_Type.tp_getattro = Struct_getattro;
    PyBuildStruct_Type.tp_setattro = Struct_setattro;
    PyBuildStruct_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    PyBuildStruct_Type.tp_doc = PyDoc_STR("An ordered set of named fields.");
    PyBuildStruct_Type.tp_iter = Struct_iter;
    PyBuildStruct_Type.tp_methods = Struct_methods;
    PyBuildStruct_Type.tp_new = Struct_new;

    Array_as_mapping.mp_length = Value_length;
    Array_as_mapping.mp_subscript = Array_subscript;
    Array_as_mapping.mp_ass_subscript = Array_ass_subscript;

    PyBuildArray_Type.tp_name = "bbq.build.Array";
    PyBuildArray_Type.tp_basicsize = sizeof(PyBuildValue);
    PyBuildArray_Type.tp_dealloc = Value_dealloc;
    PyBuildArray_Type.tp_repr = Array_repr;
    PyBuildArray_Type.tp_as_mapping = &Array_as_mapping;
    PyBuildArray_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    PyBuildArray_Type.tp_doc = PyDoc_STR("An ordered sequence of elements.");
    PyBuildArray_Type.tp_iter = Array_iter;
    PyBuildArray_Type.tp_methods = Array_methods;
    PyBuildArray_Type.tp_new = Array_new;

    if (PyType_Ready(&PyBuildLeaf_Type) < 0) return nullptr;
    if (PyType_Ready(&PyBuildStruct_Type) < 0) return nullptr;
    if (PyType_Ready(&PyBuildArray_Type) < 0) return nullptr;

    PyObject* m = PyModule_Create(&def);
    if (!m) return nullptr;
#ifdef Py_GIL_DISABLED
    PyUnstable_Module_SetGIL(m, Py_MOD_GIL_NOT_USED);
#endif
    Py_INCREF(&PyBuildStruct_Type);
    if (PyModule_AddObject(m, "Struct", (PyObject*)&PyBuildStruct_Type) < 0) {
        Py_DECREF(&PyBuildStruct_Type); Py_DECREF(m); return nullptr;
    }
    Py_INCREF(&PyBuildArray_Type);
    if (PyModule_AddObject(m, "Array", (PyObject*)&PyBuildArray_Type) < 0) {
        Py_DECREF(&PyBuildArray_Type); Py_DECREF(m); return nullptr;
    }
    Py_INCREF(&PyBuildLeaf_Type);
    if (PyModule_AddObject(m, "Leaf", (PyObject*)&PyBuildLeaf_Type) < 0) {
        Py_DECREF(&PyBuildLeaf_Type); Py_DECREF(m); return nullptr;
    }
    return m;
}
