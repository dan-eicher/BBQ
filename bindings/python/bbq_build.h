#pragma once
//
// bbq.build — construction from nothing.
//
// Typed leaf factories (u8/…/leb/sleb/text/raw) and ordered containers (Struct/Array).
// A constructed value is a `bbq::zcow` NODE — the same node a parse makes, minus the
// span, because a span is what a node was read from and this one was not read from
// anything. So writing bytes is `document::serialize()`, the one writer, and a value
// added to a parsed document is grafted into it as structure rather than flattened to
// the bytes it would have serialized to.
//
// It knows nothing about grammars: the caller says `u32` and means it. Which is why it
// is separate from the parse side rather than part of it.
//
#include <Python.h>

#include "CaptureCow.h"

// Build and return the fully-initialized `bbq.build` submodule (types ready, factories
// + Struct/Array registered). The caller drops it into the parent module + sys.modules.
PyObject* bbq_build_create_module(void);

// Is `o` one of the bbq.build value types (Leaf/Struct/Array)?
bool bbq_build_is_value(PyObject* o);

// The node behind a bbq.build value — or a fresh Bytes node for plain bytes/bytearray,
// so the two ways of supplying content meet at the same place. The parse side attaches
// what comes back. Sets a Python error and returns null if `o` is neither.
bbq::zcow::node_ptr bbq_build_node(PyObject* o);
