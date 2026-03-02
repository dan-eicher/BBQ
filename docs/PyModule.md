# BBQ Python Extension Module

CPython C API extension module wrapping the CEK VM backend. Provides
zero-copy parsing of binary data with lazy materialization — values are
decoded from the underlying buffer only when accessed from Python.

**Source:** `bindings/python/bbq_python.cpp` (single file, no headers)
**Tests:** `test/test_bbq_python.py` (146 tests)

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Python                                                      │
│  bbq.Spec  ──parse()──▶  bbq.ParseResult                    │
│             ──parse_file()──▶  (mmap-backed)                 │
│                              │                               │
│                              ▼                               │
│                          bbq.Node  (lazy, zero-copy)         │
│                           ├── .field → Node                  │
│                           ├── ["field"] → Node               │
│                           ├── [i] → Node                     │
│                           ├── [::2] → [Node, ...]            │
│                           ├── int(n) / float(n) / str(n)     │
│                           ├── bytes(n) / memoryview(n)       │
│                           ├── n == 42 / n < 100              │
│                           ├── .keys() / .values() / .items() │
│                           ├── dict(n)                        │
│                           ├── f"{n:08x}"                     │
│                           └── dir(n)  (tab-completion)       │
├──────────────────────────────────────────────────────────────┤
│  C++ (bindings/python/bbq_python.cpp)                        │
│                                                              │
│  PyBBQSpec     → owns CompiledGrammar*                       │
│  PyBBQResult   → owns ParseArena* + Py_buffer + root         │
│  PyBBQNode     → borrows FieldCapture* + back-ref Result     │
│  PyBBQNodeIter → borrows FieldCapture children array         │
├──────────────────────────────────────────────────────────────┤
│  CEK VM (unchanged)                                          │
│  CEKCompiler::compile() → CompiledGrammar*                   │
│  CEKMachine::execute_from() → CaptureMetadata                │
└──────────────────────────────────────────────────────────────┘
```

## Ownership Model

All types are GC-tracked where needed. No dangling pointers are possible.

```
PyBBQSpec (ref-counted)
  ├─ owns CompiledGrammar* (deleted in tp_dealloc)
  └─ owns ext_callables[] (Py_INCREF'd refs, Py_DECREF'd in tp_dealloc)

PyBBQResult (ref-counted, GC-tracked)
  ├─ owns ParseArena* (heap-allocated, deleted in tp_dealloc)
  ├─ owns Py_buffer view (released in tp_dealloc; .obj holds the buffer/mmap ref)
  ├─ holds Py_INCREF'd ref to PyBBQSpec (prevents grammar GC)
  └─ CaptureMetadata.root → points into arena

PyBBQNode (ref-counted, GC-tracked)
  ├─ borrows const FieldCapture* (points into Result's arena)
  └─ holds Py_INCREF'd ref to PyBBQResult (prevents arena GC)

PyBBQNodeIter (ref-counted, GC-tracked)
  ├─ borrows const FieldCapture* children array
  └─ holds Py_INCREF'd ref to PyBBQResult (prevents arena GC)
```

## Types

### `bbq.Spec`

Wraps a compiled BBQ grammar. Thread-safe for concurrent reads (the
continuation graph is immutable after compilation).

**Methods:**

| Method | Description |
|--------|-------------|
| `parse(data, *, rule=None)` | Parse `bytes`/`bytearray`/buffer. Returns `ParseResult`. |
| `parse_file(path, *, rule=None)` | Parse a file (mmap'd for zero-copy). Returns `ParseResult`. |
| `register_extern(name, callable)` | Register an external parser. See [External Parsers](#external-parsers). |

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `rules` | `list[str]` | Rule names defined in the spec |
| `default_endian` | `str` | `"little"` or `"big"` |

### `bbq.ParseResult`

Owns the parse lifetime. Attribute access falls through to root children,
so `result.header` is equivalent to `result.root.header`.

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `success` | `bool` | Whether the parse succeeded |
| `bytes_consumed` | `int` | Number of bytes consumed |
| `error_message` | `str \| None` | Error message on failure, `None` on success |
| `error_offset` | `int` | Byte offset where the error occurred |
| `root` | `Node \| None` | Root node of the capture tree |

**Protocols:**

| Operation | Behavior |
|-----------|----------|
| `result.field` | Attribute access to root children |
| `result["field"]` | Subscript access (useful for Python-keyword field names) |
| `result[0]` | Integer index into root children |
| `result[0:2]` | Slice of root children → `list[Node]` |
| `len(result)` | Number of root children |
| `"field" in result` | Check if field name exists |
| `for name, node in result:` | Iterate root children as `(name, node)` tuples |
| `dir(result)` | Properties + root child names (tab-completion) |
| `repr(result)` | `<bbq.ParseResult ok 7 bytes [tag, length, value]>` |

### `bbq.Node`

The core type. Wraps a single `FieldCapture*` with lazy materialization.
Never copies data from the buffer until a Python value is requested.

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `offset` | `tuple[int, int]` | `(start, end)` byte offsets in the buffer |
| `raw` | `bytes` | Raw bytes from buffer `[start:end)` |
| `capture_type` | `str` | Type name: `"uint32le"`, `"string"`, `"struct"`, etc. |
| `name` | `str \| None` | Field name, or `None` for unnamed nodes (array elements) |
| `value` | `int\|float\|bool\|str\|bytes\|Node` | Auto-materialized Python value |

**Number protocol:**

| Operation | Behavior |
|-----------|----------|
| `int(node)` | Decode integer value. TypeError on non-numeric types. |
| `float(node)` | Decode float value. Int types promote to float. |
| `bool(node)` | Truthy test: bool value, nonzero int, non-empty string, containers always true. |

**Mapping protocol:**

| Operation | Behavior |
|-----------|----------|
| `node["field"]` | Child by name → `Node`. Raises `KeyError` if missing. |
| `node[i]` | Child by index → `Node`. Supports negative indices. |
| `node[0:2]` | Slice of children → `list[Node]` |
| `len(node)` | Child count for struct/array. `TypeError` on leaf nodes. |

**Sequence protocol:**

| Operation | Behavior |
|-----------|----------|
| `"field" in node` | Check if a child with the given name exists |

**Buffer protocol:**

| Operation | Behavior |
|-----------|----------|
| `bytes(node)` | Raw bytes of this node's span |
| `memoryview(node)` | Read-only memoryview into the underlying buffer |

**Iterator protocol:**

| Operation | Behavior |
|-----------|----------|
| `for node in array_node:` | Yields child `Node` objects |
| `for name, node in struct_node:` | Yields `(name, node)` tuples |
| Leaf nodes | `TypeError: not iterable` |

**Rich comparison:**

| Operation | Behavior |
|-----------|----------|
| `node == 42` | Materialize value, compare with Python semantics |
| `node < 100` | All comparison operators supported on leaf nodes |
| Struct/array nodes | Identity comparison (`==`/`!=` only); ordering raises `TypeError` |

**Methods:**

| Method | Description |
|--------|-------------|
| `keys()` | Child field names → `list[str]` (empty for leaf nodes) |
| `values()` | Child nodes → `list[Node]` (empty for leaf nodes) |
| `items()` | `list[tuple[str, Node]]` (empty for leaf nodes) |

**Format protocol:**

| Operation | Example |
|-----------|---------|
| `f"{node:08x}"` | Format as hex: `"0000002a"` |
| `f"{node:.2f}"` | Format as float: `"3.14"` |
| `f"{node:>10s}"` | Format as string: `"     hello"` |
| `format(node, "")` | Default string representation |

**Other:**

| Operation | Behavior |
|-----------|----------|
| `str(node)` | String value for string nodes; `str(int)` for int nodes; `"True"`/`"False"` for bool |
| `repr(node)` | `<bbq.Node 'magic' type=uint32le [0x0:0x4]>` |
| `dir(node)` | Properties + methods + child names (tab-completion) |
| `dict(node)` | Works on struct nodes via `items()` iteration |
| `hash(node)` | Raises `TypeError: unhashable` (nodes have `__eq__`) |

### `bbq.NodeIter`

Iterator returned by `iter(node)` and `iter(result)`. Yields `(name, node)`
tuples for struct nodes and bare `Node` objects for array nodes.

### `bbq.ParseError`

Exception raised by `compile()` and `compile_string()` on parse or semantic
errors. Also raised by `parse()` for unknown rule names.

## CaptureType → Python Value Mapping

All conversions read directly from the buffer. Zero copy until a Python
object is created.

| CaptureType | `int()` | `float()` | `str()` | `bytes()` | `.value` |
|---|---|---|---|---|---|
| UInt8..UInt64* | decode | int→float | `str(int)` | raw | `int` |
| Int8..Int64* | decode | int→float | `str(int)` | raw | `int` |
| Float32/64* | TypeError | decode | `str(float)` | raw | `float` |
| Bool | 0/1 | TypeError | `"True"/"False"` | raw | `bool` |
| String | TypeError | TypeError | decode UTF-8 | raw | `str` |
| Bytes | TypeError | TypeError | repr | raw | `bytes` |
| Computed | value | float(value) | `str(value)` | raw | `int` |
| Struct | TypeError | TypeError | repr | span bytes | `Node` (self) |
| Array | TypeError | TypeError | repr | span bytes | `Node` (self) |
| External | TypeError | TypeError | repr | raw | `bytes` |

Endianness is baked into the `CaptureType` enum (e.g., `UInt32LE` vs
`UInt32BE`), so decode is a simple switch — no runtime endian parameter.

## Error Handling

| Condition | Exception |
|-----------|-----------|
| Compile failure (parse/sema) | `bbq.ParseError` |
| Unknown rule name | `bbq.ParseError` |
| Parse failure | `result.success == False` (not an exception) |
| `int()` on non-integer type | `TypeError` |
| `float()` on non-numeric type | `TypeError` |
| Missing field attribute | `AttributeError` |
| Missing field subscript | `KeyError` |
| Index out of range | `IndexError` |
| `len()`/`iter()`/subscript on leaf | `TypeError` |
| Bad subscript key type | `TypeError` |
| `hash(node)` | `TypeError: unhashable` |
| `register_extern(name, non_callable)` | `TypeError` |

## External Parsers

BBQ specs can delegate to user-provided functions via `extern("name", "type")`.
In Python, register a callable before parsing:

```python
spec = bbq.compile_string('''
Msg = struct {
    header: uint32,
    payload: extern("decompress", "void")
}
''')

def decompress(data: memoryview) -> int:
    """Return number of bytes consumed, or None on failure."""
    # data is a read-only memoryview of remaining input
    return len(data)

spec.register_extern("decompress", decompress)
result = spec.parse(raw_bytes)
```

**Callable signature:** `(memoryview) -> int | None`

- The `memoryview` covers remaining input from the current parse position.
- Return an `int` (number of bytes consumed) on success.
- Return `None` to signal failure (the VM will backtrack or fail).
- Raising an exception is treated as failure (exception is cleared internally).
- Returning a value larger than `len(data)` is treated as failure.
- Returning `0` is valid (zero-length capture).

**Multiple externs:** Call `register_extern()` once per name. Registering the
same name twice replaces the previous callable.

**Lifecycle:** The Spec holds strong references (`Py_INCREF`) to all
registered callables. They are released when the Spec is deallocated.

**Capture type:** External fields appear as `CaptureType::External`. The
`.value` property returns raw `bytes` of the consumed span.

## Build

The Python extension is optional — built only if `Python3::Development` is
found by CMake.

```cmake
find_package(Python3 COMPONENTS Development)
if(Python3_FOUND)
    add_library(bbq_python MODULE bindings/python/bbq_python.cpp)
    target_link_libraries(bbq_python PRIVATE bbq_frontend bbq_cek_backend Python3::Python)
    set_target_properties(bbq_python PROPERTIES PREFIX "" OUTPUT_NAME "bbq")
endif()
```

```sh
# Build and test
cmake -B build && cmake --build build -j4
ctest --test-dir build --output-on-failure
PYTHONPATH=build python -m pytest test/test_bbq_python.py -v
```
