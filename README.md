# BBQ — Binary Block Query

A declarative DSL for specifying binary format parsers. Write a `.bbq` spec
describing your format, then parse binary data from Python or generate
standalone C++ parsers.

## Quick Example

```
Header = struct {
    magic:   uint32be where magic == 0x89504E47,
    version: uint16le,
    flags:   uint8,
    length:  uint32le where length > 0
}
```

```python
>>> import bbq
>>> spec = bbq.compile("examples/header.bbq")
>>> result = spec.parse(b"\x89PNG\x03\x00\x01\x0a\x00\x00\x00")
>>> result.magic == 0x89504E47
True
>>> int(result.version)
3
>>> f"{result.length:#x}"
'0xa'
>>> {k: v.value for k, v in result.root.items()}
{'magic': 2303741511, 'version': 3, 'flags': 1, 'length': 10}
```

## Features

- **Types**: struct, union, array, optional, switch, compute, extern, bitfield
- **Integers**: fixed-width (`uint32be`, …) and LEB128 varints (`uleb128`/`sleb128`/`uleb64`/`sleb64`), strict on read (max width enforced), canonical on write
- **Constraints**: `where` clauses validated during parsing — on a field, or on a whole struct (`} where check(…)`), including calls to user predicate functions returning bool
- **Switch**: integer, range (`0x00 .. 0x0B:`), string and enum cases; `reject` arms for fail-closed grammars
- **Intervals**: `[length]` and `[start, end]` windows; `@rest` fields that scope the remainder (confined: a window must fit its parent and be consumed exactly; the writer recomputes `uleb` `@rest` sizes on serialize)
- **Context builtins**: `@start` `@end` `@pos` `@index` `@buffer` `@remaining` — the parse-state environment available to any expression (see below)
- **Code blocks**: `@header` / `@source` / `@writer` inline code emitted into the generated parser, for helper declarations and definitions (e.g. `where` predicates)
- **Arrays**: fixed count, separator/terminator, eof-terminated, `resync` mode for per-element-skip recovery
- **Endianness**: `@endian big|little` directive, per-type overrides (`uint32be`)
- **Zero-copy**: mmap-backed file parsing, lazy value materialization (Python/CEK path); the generated C parsers build fully OWNING trees (bytes/strings copied, released by the generated `_free`)
- **Python protocols**: comparison, format, dict, buffer, iteration, tab-completion

## The interval environment

Every semantic position in a parse has the same small, orthogonal environment:
the decoded fields that are in scope (referenced by their bare names), plus five
offsets reachable through the `@` sigil:

| Builtin | Meaning |
|---------|---------|
| `@start` | the enclosing construct's entry offset |
| `@end` | the active end bound (innermost interval, or the input end) |
| `@pos` | the parse cursor |
| `@index` | the current array/loop index |
| `@buffer` | the construct's consumed bytes, `input[@start..@pos)` |
| `@remaining` | `@end - @pos` |

Everything else is derivable from these. A decoded field's *name* is its decoded
value; `@buffer` is the raw on-wire bytes — the two views are distinct on purpose
(a `uleb128` field's name is the integer, `@buffer` over it is the raw varint
bytes). Bare identifiers are *always* field references — the `@` sigil is a
reserved namespace, so a spec may freely name fields `pos`, `buffer`, or `i`.

A struct-level `where` combines these with user predicates to express whole-
construct checks, e.g. a checksum over the header bytes:

```
@header (.
static inline bool ipv4_checksum_ok(bbq_bytes_t buf, uint16_t stored) { … }
.)
IPv4Header = struct {
    …
    checksum: uint16,
    …
} where ipv4_checksum_ok(@buffer, checksum)
```

## Building

```sh
cmake -B build
cmake --build build -j$(nproc)
```

The Python extension module is built automatically if Python 3 development
headers are found. On Fedora: `sudo dnf install python3-devel`. On Ubuntu:
`sudo apt install python3-dev`.

To use the module, add the build directory to your Python path:

```sh
export PYTHONPATH=build
python -c "import bbq; print('ok')"
```

## Two Paths

### Python module

`import bbq` — compile specs and parse binary data interactively. Zero-copy
mmap for large files, lazy materialization, full Python protocols for
navigating parsed structures.

See [Getting Started](docs/GettingStarted.md) for a walkthrough, and
[PyModule.md](docs/PyModule.md) for the full API reference.

### Code generation (C, C++)

`bbqc` compiles `.bbq` specs into standalone parsers with no runtime dependency
on BBQ, selected by `-backend`. **`-backend cpp`** (default) emits header-only C++
zero-copy view types (`<prefix>Types.h`, `<prefix>Reader.h`, `<prefix>Writer.h`) —
a parse records offsets and copies nothing, and `emit()` writes mutations back
(copy-on-write). **`-backend c`** emits an owning C reader **and a writer** plus
generated free functions (`<prefix>_types.h`, `<prefix>_reader.h/.c`,
`<prefix>_writer.h/.c`) — read∘write round-trips, and the writer recomputes `uleb`
`@rest` sizes so programmatically-built trees serialize correctly. **`-backend
c-lite`** emits a read-only, zero-copy C view reader (a span index, decode-on-access)
for embedded / pure-C consumers with no C++ runtime.

```sh
./build/bbqc -backend cpp examples/header.bbq -prefix hdr -o gen/ -templates backends/render/templates
./build/bbqc -backend c   examples/header.bbq -prefix hdr -o gen/ -templates backends/render/templates -frames backends/c/frames/
```

See [Getting Started](docs/GettingStarted.md) for the C++ workflow.

## Project Layout

```
frontend/           Parser, AST, semantic analysis (shared by all backends)
backends/c/         C runtime (bbq_read.h shared cursor/decoders, bbq_runtime.h owning+writer, bbq_lite.{h,c} c-lite index) + C name mapper
backends/cpp/       C++ zero-copy view runtime (bbq_node.h); types/reader/writer generated via the render backend
backends/render/    Lower-to-JSON + inja templates: the render C/C++ codegen backend
backends/cek/       CEK VM interpreter (Compiler, Machine, continuation nodes)
bindings/python/    CPython C API extension module wrapping the CEK VM
grammar/            BBQ.peg, BBQ.asdl, frame templates
tools/              bbqc CLI entry point
test/               C++ tests (GTest) and Python tests (pytest)
docs/               Language reference, design docs, guides
examples/           Sample .bbq specs (header, ipv4, tlv, wasm — the WebAssembly module format)
crt/                Tiny C runtime helpers shared by generated code (bbq_arena, bbq_vec, bbq_htree)
pegc/               PEG parser generator (in-tree, no external dependency)
asdl/               ASDL code generator (in-tree)
burgc/              BURS instruction-selector generator
ddcgc/              Destination-driven code generator generator
jitterator/         Copy-and-patch JIT stencil extractor and runtime
```

## Documentation

| Document | Description |
|----------|-------------|
| [Getting Started](docs/GettingStarted.md) | End-to-end tutorial: writing specs, Python workflow, C++ codegen |
| [Grammar Reference](docs/Grammar.md) | Complete BBQ language syntax and semantics |
| [Python Module](docs/PyModule.md) | Python API reference (Spec, ParseResult, Node) |
| [CEK Machine Design](docs/CEK%20Machine%20Design.md) | Internals of the interpretive VM backend |

## Running Tests

```sh
# Full suite (C++ via ctest, Python via pytest hook): 642 + 146 tests
ctest --test-dir build --output-on-failure
```
