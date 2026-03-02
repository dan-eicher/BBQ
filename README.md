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
- **Constraints**: `where` clauses validated during parsing
- **Intervals**: `[length]` and `[start, end]` for variable-size fields
- **Arrays**: fixed count, separator/terminator, eof-terminated
- **Endianness**: `@endian big|little` directive, per-type overrides (`uint32be`)
- **Zero-copy**: mmap-backed file parsing, lazy value materialization
- **Python protocols**: comparison, format, dict, buffer, iteration, tab-completion

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

### C++ code generation

`bbqc` compiles `.bbq` specs into standalone C++ parsers — three files
(`Types.h`, `Parser.h`, `Parser.cpp`) that you compile and link into your
project. No runtime dependency on BBQ.

```sh
./build/bbqc examples/header.bbq -o gen/ -frames backends/cpp/frames/
```

See [StaticCodegen.md](docs/StaticCodegen.md) for the full design and usage.

## Project Layout

```
frontend/           Parser, AST, semantic analysis (shared by both backends)
backends/cpp/       Static C++ code generator (TypeEmitter, ParserEmitter)
backends/cek/       CEK VM interpreter (Compiler, Machine, continuation nodes)
bindings/python/    CPython C API extension module wrapping the CEK VM
grammar/            BBQ.peg, BBQ.asdl, frame templates
tools/              bbqc CLI entry point
test/               C++ tests (GTest) and Python tests (pytest)
docs/               Language reference, design docs, guides
examples/           Sample .bbq specs (header, ipv4, tlv)
pegc/               PEG parser generator (in-tree, no external dependency)
asdl/               ASDL code generator (in-tree)
```

## Documentation

| Document | Description |
|----------|-------------|
| [Getting Started](docs/GettingStarted.md) | End-to-end tutorial: writing specs, Python workflow, C++ codegen |
| [Grammar Reference](docs/Grammar.md) | Complete BBQ language syntax and semantics |
| [Python Module](docs/PyModule.md) | Python API reference (Spec, ParseResult, Node) |
| [Static Codegen](docs/StaticCodegen.md) | C++ code generator architecture and output format |
| [CEK Machine Design](docs/CEK%20Machine%20Design.md) | Internals of the interpretive VM backend |

## Running Tests

```sh
# C++ tests (512 tests)
ctest --test-dir build --output-on-failure

# Python tests (136 tests)
PYTHONPATH=build python -m pytest test/test_bbq_python.py -v
```
