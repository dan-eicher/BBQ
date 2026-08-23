# BBQ Python Extension Module

CPython C API extension wrapping the CEK VM. The workflow is **parse · explore ·
edit · emit** for prototyping grammars and exploring binary formats: parse a real
binary, get a zero-copy Python container, poke and edit it, and serialize back to
bytes. `parse`/`emit` are the `get`/`put` of a well-behaved lens: an unedited
`emit()` returns the input byte for byte, and an edited one re-parses to the edit
that was made — so **dependent fields** (array counts, `@rest` window sizes) are
recomputed for you, never maintained by hand. See [Bidirectional.md](Bidirectional.md)
for the laws and what they do and do not claim.

A separate `bbq.build` submodule constructs binary content from scratch (a typed
byte-emitter — the struct-tier analogue of Python's `struct`).

**Source:** `bindings/python/bbq_python.cpp` (parse/edit) + `bindings/python/bbq_build.cpp` (construction)
**Tests:** `test/test_bbq_python.py` (215 tests) + `test/test_bbq_freethreaded.py` (free-threading stress)

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Python                                                      │
│  bbq.Spec  ──parse()──▶  bbq.ParseResult ──▶ bbq.Node        │
│             ──parse_file()──▶ (mmap-backed)   (lazy, 0-copy) │
│                                                              │
│  bbq.build.{u8…u64,i8…i64,f32,f64,leb,sleb,text,raw}         │
│            .Struct(**fields)  .Array(*elems)  → bytes(x)     │
├──────────────────────────────────────────────────────────────┤
│  C++                                                         │
│  bbq_python.cpp : Spec / ParseResult / Node over the CEK    │
│                   parse + the bbq::zcow overlay (edit/emit)  │
│  bbq_build.cpp  : grammar-free byte construction (knows only │
│                   bytes; meets the parse side at bytes only) │
├──────────────────────────────────────────────────────────────┤
│  CEK VM (parse)  ·  WriterInterp (grammar enforcement)       │
│                  ·  bbq::zcow / bbq::emit (overlay + bytes)  │
└──────────────────────────────────────────────────────────────┘
```

The parse produces a `FieldCapture` index over the input buffer (zero-copy reads).
Edits go through the `bbq::zcow` copy-on-write overlay. ZCow itself knows only bytes —
it has no concept of grammar, counts, or validity — so `emit()` first walks the
grammar over the edited overlay and recomputes the dependent fields the edits
invalidated, then hands off to ZCow, which blits unchanged (Borrowed) ranges and
re-serializes changed (Owned) ones. The walk is the shared writer lowering
(`lower_writer_ops`), executed rather than rendered: the same op-list `bbqc` turns
into the generated C++ ZCow writer.

## Types

### `bbq.Spec`

A compiled grammar. Thread-safe for concurrent reads.

| Method | Description |
|--------|-------------|
| `parse(data, *, rule=None)` | Parse `bytes`/buffer → `ParseResult`. |
| `parse_file(path, *, rule=None)` | Parse a file (mmap'd, zero-copy) → `ParseResult`. |
| `register_extern(name, callable)` | Register an external parser. See [External Parsers](#external-parsers). |

Properties: `rules` (`list[str]`), `default_endian` (`"little"`/`"big"`).

Module functions `bbq.compile(path)` / `bbq.compile_string(src)` build a `Spec`.

### `bbq.ParseResult`

Owns the parse lifetime; attribute access falls through to the root's children.

| Property | Type | Description |
|----------|------|-------------|
| `success` | `bool` | Whether the parse succeeded |
| `bytes_consumed` | `int` | Bytes consumed |
| `error_message` | `str \| None` | Error on failure |
| `error_offset` | `int` | Byte offset of the error |
| `root` | `Node \| None` | Root capture node |

Container protocols: `result.field`, `result["field"]`, `result[i]`, slices, `len()`,
`in`, iteration as `(name, node)`, `dir()`. Plus:

| Method | Description |
|--------|-------------|
| `emit() -> bytes` | Enforce the grammar over the edited overlay (dependent fields recomputed), then serialize. Byte-identical to the input if unedited; re-parses to the edit if not. |
| `deltas() -> list[dict]` | The structured diff since parse: `{path, offset, old, new}` per changed field. |

### `bbq.Node`

The core lazy, zero-copy type over a single `FieldCapture`.

**Read / navigate:** `.field`, `["field"]`, `[i]` (negative ok), `[a:b]` slices,
`len()`, `in`, iteration (struct → `(name, node)`, array → nodes), `keys()`/`values()`/`items()`, `dict(node)`, `dir()`.

**Materialize:** `int()`, `float()`, `bool()`, `str()`, `bytes()`, `memoryview()`,
`.value` (auto), rich comparison (`== 42`, `< 100`), `f"{node:08x}"`, `repr`.
`hash()` raises (nodes have `__eq__`).

**Properties:** `offset` `(start,end)`, `raw` (bytes), `capture_type` (str), `name`
(`str|None`), `value`, and **`variant_tag`** — the union/switch arm ordinal, or
`None` if the node is not a variant.

**Edit (copy-on-write, byte-level):**

| Operation | Behavior |
|-----------|----------|
| `node.field = v` / `node[i] = v` | Set a leaf to an `int`/`float`/`bytes`/`str`/`bool`, keyed on the field's own recorded type (no grammar). |
| `node.append(v)` | Append an array element: a scalar (typed from the array's existing elements), `bytes`, or a `bbq.build` object. |
| `node[i] = bytes` / `bbq.build` obj | Splice an element wholesale (raw bytes in). |
| `del node[i]` | Remove an array element. |

`len(array_node)` is **live** — it reflects appends/deletes immediately, like a
Python list. Structural *contents* (the new elements) are read back after
`emit()` → re-parse. The array's count field is a **dependent field**: `emit()`
derives it from the array's effective length, so do not set it yourself — the
array wins. The same holds for a count reached by a path (`array<uint8>[h.n]`),
a count nested inside an array element, and an `@rest` window's size.

## `bbq.build` — construction from scratch

A grammar-free factory of typed byte-emitters. Build mirrors read: leaves are typed
scalars (a value, no children); containers are dict-/list-like; `bytes(x)` is the one
shared verb.

**Leaf factories** (little-endian default; `endian="big"` to flip):
`u8 u16 u32 u64  i8 i16 i32 i64  f32 f64`, `leb(v)` / `sleb(v)`, `text(s)` (UTF-8) /
`raw(b)` (verbatim). A leaf has `.value`, `int()`/`float()`, `bytes()`, `.type`
(`"u32le"`), `repr`. It has no `len`/index — it is not a container.

**Containers:**
- `Struct(**fields)` — named, dict-like: `len`, `s["k"]`, `s.k`, `in`, iterate
  `(name, child)`, `keys/values/items`, mutate (`s.k = ...`).
- `Array(*elems)` (or `Array([elems])`) — positional, list-like: `len`, `a[i]`,
  `append`, `del a[i]`, iterate.

`bytes(x)` serializes (leaves encode; containers concatenate children in order). A
scalar requires an explicit type (`b.u32(5)`, never bare `5`); a plain `bytes`/`bytearray`
is accepted anywhere a child is. Names are labels only — never in the bytes.

```python
import bbq, bbq.build as b

rec = b.Struct(
    magic = b.u32(0xCAFEBABE, endian="big"),
    n     = b.u8(3),
    xs    = b.Array(b.u16(10), b.u16(20), b.u16(30)),
)
data = bytes(rec)                 # a whole binary from scratch
```

### Construction ↔ ZCow boundary

The two layers meet only at **bytes**:

- **New file from nothing:** `bytes(bbq.build.Struct(...))` — no parse, no overlay.
- **Add to a parsed file:** the build object's bytes cross into the ZCow overlay as
  an Owned node; `emit()` stitches them with the untouched (zero-copy) original.

```python
r = spec.parse(data)
r.records.append(b.Struct(v=b.u8(3), w=b.u8(4)))   # enters as bytes
out = r.emit()
```

A built object added to a parse is opaque bytes in the tree afterward — re-parse to
navigate it as structure.

## CaptureType → Python value

All conversions read directly from the buffer (zero copy until a value is requested).

| CaptureType | `.value` |
|---|---|
| UInt8..UInt64*, Int8..Int64* | `int` |
| Float32/64* | `float` |
| Bool | `bool` |
| String | `str` (UTF-8) |
| Bytes / External | `bytes` |
| Computed | `int`/`bool`/`float`/`str` per its stored kind |
| Struct / Array | `Node` (self) |

Endianness is baked into the `CaptureType` (e.g. `UInt32LE` vs `UInt32BE`).

## External Parsers

A spec can delegate to a Python callable via `extern("name", "type")`:

```python
spec = bbq.compile_string('Msg = struct { hdr: uint32, blob: extern("decompress", "void") }')
spec.register_extern("decompress", lambda mv: len(mv))   # (memoryview) -> int consumed | None
result = spec.parse(raw_bytes)
```

The callable gets a read-only `memoryview` of the remaining input and returns the
number of bytes consumed (or `None`/raise to fail). The `Spec` holds a strong
reference to each registered callable.

A parse runs against the registry **as it stood when the parse began**: it takes a
snapshot holding a strong reference to each callable. So `register_extern` during a
running parse (from the extern callback itself, or from another thread) affects the
next parse, not that one.

## Error Handling

| Condition | Exception |
|-----------|-----------|
| Compile failure | `bbq.ParseError` |
| Unknown rule name | `bbq.ParseError` |
| Parse failure | `result.success == False` (not an exception) |
| `int()`/`float()` on a wrong type | `TypeError` |
| Missing field attribute / subscript | `AttributeError` / `KeyError` |
| Index out of range | `IndexError` |
| `bbq.build` child that isn't a build value or bytes | `TypeError` |

## Free-threaded Python

The module declares `Py_MOD_GIL_NOT_USED`, so importing it into a free-threaded
build (PEP 703; 3.13 experimental, 3.14 supported) leaves the GIL disabled — it
does not silently re-enable it for the whole process.

That means the module serializes its own shared state rather than leaning on the
interpreter:

| State | Guard |
|---|---|
| `Spec`'s writer op-list cache, extern registry | a `PyMutex` on the `Spec` |
| each `ParseResult`'s ZCow overlay (creation, every edit, `emit`, `deltas`) | a `PyMutex` on the result |
| `bbq.build` `Struct`'s parallel `names`/`children` lists | a critical section on the object |

Locks are only ever held over C++ — every `PyObject` conversion happens before one
is taken, so nothing can re-enter the interpreter while a lock is held.

What this does **not** promise is that concurrent edits to *one* `ParseResult` are
meaningful; they are merely safe. Which of two racing writes wins is up to the
schedule, exactly as for a shared `list`. Separate results are fully parallel.

Below 3.13 the locking compiles to nothing, since the GIL already provides it.

## Build

Built only if `Python3::Development` is found.

```sh
cmake -B build && cmake --build build -j4
ctest --test-dir build -R bbq_python --output-on-failure
PYTHONPATH=build python -m pytest test/test_bbq_python.py -v
```

For the free-threading stress suite (`test/test_bbq_freethreaded.py`), configure a
second build dir against a free-threaded interpreter — `ctest` picks the test up
automatically when the interpreter reports `Py_GIL_DISABLED`:

```sh
cmake -B build-ft -DPython3_FIND_ABI='ANY;ANY;ANY;ON' \
                  -DPython3_EXECUTABLE=/usr/bin/python3.14t
ctest --test-dir build-ft -R bbq_python_freethreaded --output-on-failure
```
