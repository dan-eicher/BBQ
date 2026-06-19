#pragma once
//
// bbq.build — the grammar-free byte-construction submodule.
//
// Typed leaf factories (u8/…/leb/sleb/text/raw) and ordered containers (Struct/Array)
// that serialize straight to bytes via the shared encode atoms. It knows nothing about
// grammars, parses, or the ZCow overlay — bytes are its only currency. The parse side
// ingests its output as plain bytes (append/splice).
//
#include <Python.h>
#include <cstdint>
#include <vector>

// Build and return the fully-initialized `bbq.build` submodule (types ready, factories
// + Struct/Array registered). The caller drops it into the parent module + sys.modules.
PyObject* bbq_build_create_module(void);

// Is `o` one of the bbq.build value types (Leaf/Struct/Array)?
bool bbq_build_is_value(PyObject* o);

// Serialize a bbq.build value (or plain bytes/bytearray) into `out`. The parse side
// calls this to take constructed content in as bytes. Sets a Python error + returns
// false on an unserializable object.
bool bbq_build_serialize(PyObject* o, std::vector<uint8_t>& out);
