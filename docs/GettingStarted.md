# Getting Started with BBQ

This guide walks through writing BBQ specs and using them to parse binary
data. It's a practical tutorial — for the full language reference see
[Grammar.md](Grammar.md), and for the complete Python API see
[PyModule.md](PyModule.md).

## Writing a Spec

A BBQ spec is one or more rules. Each rule names a type:

```
Header = struct {
    magic:   uint32be,
    version: uint16le
}
```

Fields have a name and a primitive type. Endianness is part of the type name
(`uint32be`, `uint16le`) or set globally with a directive:

```
@endian big

Header = struct {
    magic:   uint32,
    version: uint16
}
```

### Constraints

`where` clauses validate fields during parsing. If a constraint fails, the
parse fails:

```
Header = struct {
    magic:   uint32be where magic == 0x89504E47,
    version: uint16le,
    flags:   uint8,
    length:  uint32le where length > 0
}
```

### Variable-length fields

A field's size can depend on another field:

```
TLVEntry = struct {
    type:   uint8,
    length: uint16,
    value:  bytes[length]
}
```

`bytes[length]` reads `length` bytes. `string[length]` works the same way for
UTF-8 text.

### Arrays

Arrays repeat an element type. Fixed count:

```
Block = struct {
    count: uint8,
    items: array<uint16>[count]
}
```

EOF-terminated (reads elements until input is exhausted):

```
TLVStream = struct {
    entries: array<TLVEntry>(none, eof)
}
```

The `(none, eof)` means: no separator, terminate at end-of-input.

### Computed fields

Computed fields derive values from other fields without consuming input:

```
IPv4Header = struct {
    ver_ihl: uint8,
    version: compute((ver_ihl >> 4) & 0x0F : uint8),
    ihl:     compute(ver_ihl & 0x0F : uint8)
}
```

The expression is followed by `: type` to specify the result type.

### Multiple rules

Rules can reference each other. The first rule is the default entry point:

```
Inner = struct { x: uint8, y: uint8 }
Outer = struct { hdr: Inner, z: uint16 }
```

For the full set of constructs — union, optional, switch, bitfield, extern,
intervals, alternatives — see [Grammar.md](Grammar.md).

## Python: Parsing Binary Data

### Compiling a spec

```python
import bbq

# From a file
spec = bbq.compile("examples/header.bbq")

# From a string
spec = bbq.compile_string("""
Header = struct {
    magic:   uint32be where magic == 0x89504E47,
    version: uint16le,
    flags:   uint8,
    length:  uint32le where length > 0
}
""")

spec.rules          # ['Header']
spec.default_endian # 'little'
```

Compile errors raise `bbq.ParseError`.

### Parsing

```python
data = b"\x89PNG\x03\x00\x01\x0a\x00\x00\x00"
result = spec.parse(data)

result.success        # True
result.bytes_consumed # 11
result.error_message  # None (only set on failure)
```

`parse()` accepts `bytes`, `bytearray`, or `memoryview`. To parse a specific
rule when the spec defines multiple:

```python
spec = bbq.compile_string("""
Inner = struct { x: uint8, y: uint8 }
Outer = struct { hdr: Inner, z: uint16 }
""")
result = spec.parse(b"\x01\x02\x03\x00", rule="Outer")
```

### Navigating results

Access fields by name, as attributes or subscripts:

```python
result.magic            # bbq.Node
result["magic"]         # same node, subscript syntax
int(result.magic)       # 2303741511
```

Nesting works naturally:

```python
int(result.hdr.x)      # 1
int(result.hdr["y"])    # 2
```

### Getting values

Every node supports `.value` for automatic materialization — it returns the
appropriate Python type (int, float, str, bool):

```python
result.magic.value      # 2303741511  (int)
result.name.value       # "hello"     (str)
result.flag.value       # True        (bool)
```

Explicit conversions work too:

```python
int(result.magic)       # 2303741511
float(result.val)       # 3.14
str(result.name)        # "hello"
bool(result.flag)       # True
```

### Comparison

Leaf nodes compare directly with Python values:

```python
result.magic == 0x89504E47  # True
result.version < 3          # True
result.name == "hello"      # True
result.flag == True          # True
```

### Inspection

```python
result.magic.offset       # (0, 4)  — byte range [start, end)
result.magic.raw          # b'\x89PNG'
result.magic.capture_type # 'uint32be'
result.magic.name         # 'magic'
repr(result.magic)        # "<bbq.Node 'magic' type=uint32be [0x0:0x4]>"
```

### Containers: arrays and structs

```python
# Array length and indexing
len(result.items)         # 3
int(result.items[0])      # 1
int(result.items[-1])     # 3
result.items[0:2]         # [Node, Node]

# Iterate arrays
[int(n) for n in result.items]  # [1, 2, 3]

# Iterate structs (yields name, node tuples)
for name, node in result.hdr:
    print(f"{name} = {int(node)}")

# Membership
"x" in result.hdr        # True
"z" not in result.hdr    # True
```

### Dict interface

Struct nodes act like dicts:

```python
result.hdr.keys()         # ['x', 'y']
result.hdr.values()       # [Node, Node]
result.hdr.items()        # [('x', Node), ('y', Node)]
dict(result.hdr)          # {'x': Node, 'y': Node}
```

### Formatting

Nodes support Python format specs:

```python
f"{result.magic:08x}"     # '0000002a'
f"{result.magic:#x}"      # '0x2a'
f"{result.magic:#b}"      # '0b101010'
f"{result.val:.1f}"       # '3.1'
f"{result.name:>10s}"     # '     hello'
```

### Buffer protocol

Nodes expose their raw bytes through the buffer protocol:

```python
bytes(result.magic)       # b'\x2a\x00\x00\x00'
mv = memoryview(result.magic)
struct.unpack("<I", mv)   # (42,)
```

### Tab completion

`dir()` lists available field names alongside standard properties, so
IPython and Jupyter tab-completion works:

```python
dir(result.hdr)  # ['capture_type', 'items', 'keys', 'name', 'offset',
                  #  'raw', 'value', 'values', 'x', 'y', ...]
```

### Error handling

Compile errors raise an exception; parse failures set a flag:

```python
# Compile error
try:
    bbq.compile_string("not valid !!!")
except bbq.ParseError as e:
    print(e)

# Parse failure (not an exception)
result = spec.parse(b"\x01")  # too short
if not result.success:
    print(f"failed at offset {result.error_offset}")
    print(result.error_message)
```

### Large files

`parse_file()` uses mmap under the hood. Combined with lazy materialization
(values are decoded from the buffer only when you access them), this lets you
browse multi-gigabyte files without loading them into memory:

```python
result = spec.parse_file("/path/to/large.bin")
int(result.header.magic)  # only this field's bytes are read
```

## C++ Code Generation

`bbqc -backend cpp` compiles a `.bbq` spec into header-only C++ files of
**zero-copy view** types — a parse records offsets into the input and copies
nothing; field accessors decode lazily, and `emit()` writes back:

```sh
./build/bbqc -backend cpp examples/header.bbq -o gen/ \
    -templates backends/render/templates -prefix header -namespace hdr
# Produces: gen/headerTypes.h, gen/headerReader.h, gen/headerWriter.h
```

Options:

| Flag | Default | Description |
|------|---------|-------------|
| `-backend <c\|cpp\|c-lite>` | `cpp` | Backend: `c` = owning C (read+write), `cpp` = C++ ZCow view (read+write+edit), `c-lite` = read-only zero-copy C view reader |
| `-o <dir>` | `.` | Output directory |
| `-templates <dir>` | — | inja template directory (`backends/render/templates`) |
| `-prefix <name>` | from input filename | Generated file prefix |
| `-namespace <ns>` | none | C++ namespace for generated code |

The generated headers depend only on the C++ view runtime in
`backends/cpp/runtime/` (`bbq_node.h`, `bbq_reader.h`, `bbq_writer.h` and the index/
decode headers they include: `Capture.h`, `CaptureDecode.h`, `CaptureCow.h`,
`ParseArena.h`, `bbq_machine.h`). Compile against that one include dir and link
`backends/cpp/runtime/CaptureDecode.cpp`; nothing else is needed (no `bbq::cek`).

Minimal usage — `<Rule>_read` builds the index, a handle is a typed view over it,
and `<Rule>_write` serializes the (optionally edited) overlay back to bytes:

```cpp
#include "headerTypes.h"     // typed handles (hdr::Header)
#include "headerReader.h"    // hdr::Header_read
#include "headerWriter.h"    // hdr::Header_write
#include <cstdio>
#include <vector>

int main() {
    const uint8_t data[] = {0x89, 'P', 'N', 'G', 0x03, 0x00,
                            0x01, 0x0a, 0x00, 0x00, 0x00};
    bbq::ParseArena arena;
    auto vm = hdr::Header_read(data, sizeof data, arena);   // zero-copy parse → index
    if (!vm.success) return 1;

    hdr::Header h(vm.root, data, nullptr);   // typed view over `data` (no edits)
    printf("magic: 0x%08x\n", h.magic());
    printf("version: %d\n", h.version());

    // Mutate through a copy-on-write overlay, then write the edited bytes back.
    bbq::zcow zc;
    hdr::Header edit(vm.root, data, &zc);
    edit.set_version(2);
    std::vector<uint8_t> out = hdr::Header_write(vm.root, data, sizeof data, &zc);
    if (out.empty()) return 1;   // the edit does not yield a document this grammar accepts
}
```

`<Rule>_write` recomputes the fields the grammar derives (array counts, `@rest`
sizes), then **re-reads its own output** and returns an empty vector rather than
bytes that would not parse — see [The Write Side](Bidirectional.md). That is why
`headerWriter.h` needs `headerReader.h`'s definitions linked.

```sh
g++ -std=c++17 -I gen -I backends/cpp/runtime \
    main.cpp backends/cpp/runtime/CaptureDecode.cpp -o demo
```

The view borrows `data`, so the input buffer must outlive the handles and any
`*_write` call.

## Real-World Examples

### header.bbq — Struct with constraints

```
Header = struct {
    magic:   uint32be where magic == 0x89504E47,
    version: uint16le,
    flags:   uint8,
    length:  uint32le where length > 0
}
```

```python
>>> import bbq, struct
>>> spec = bbq.compile("examples/header.bbq")
>>> data = struct.pack(">I", 0x89504E47) + struct.pack("<HBI", 3, 1, 10)
>>> result = spec.parse(data)
>>> result.success
True
>>> result.magic == 0x89504E47
True
>>> int(result.version)
3
>>> result.magic.offset
(0, 4)
>>> dict(result)
{'magic': 2303741511, 'version': 3, 'flags': 1, 'length': 10}
```

The `where` clause on `magic` rejects data that doesn't start with the PNG
signature. The `where` on `length` rejects zero-length headers.

### ipv4.bbq — Big-endian with computed fields

```
@endian big

IPv4Header = struct {
    ver_ihl:        uint8,
    version:        compute((ver_ihl >> 4) & 0x0F : uint8),
    ihl:            compute(ver_ihl & 0x0F : uint8),
    dscp_ecn:       uint8,
    total_length:   uint16,
    identification: uint16,
    flags_offset:   uint16,
    flags:          compute((flags_offset >> 13) & 0x07 : uint8),
    frag_offset:    compute(flags_offset & 0x1FFF : uint16),
    ttl:            uint8,
    protocol:       uint8,
    checksum:       uint16,
    src_addr:       uint32,
    dst_addr:       uint32
}
```

```python
>>> import bbq, struct
>>> spec = bbq.compile("examples/ipv4.bbq")
>>> # Minimal IPv4 header: version 4, IHL 5, total_length 20, TTL 64, TCP (6)
>>> data = struct.pack(">BBHHHBBH4s4s",
...     0x45,           # ver_ihl: version=4, ihl=5
...     0x00,           # dscp_ecn
...     20,             # total_length
...     0x1234,         # identification
...     0x4000,         # flags=2 (DF), frag_offset=0
...     64, 6, 0x0000,  # ttl, protocol (TCP), checksum
...     b"\xC0\xA8\x01\x01",  # src: 192.168.1.1
...     b"\xC0\xA8\x01\x02")  # dst: 192.168.1.2
>>> result = spec.parse(data)
>>> result.success
True
>>> int(result.version)
4
>>> int(result.ihl)
5
>>> int(result.ttl)
64
>>> int(result.protocol)
6
>>> f"{result.src_addr:#010x}"
'0xc0a80101'
```

Computed fields extract sub-byte values from packed fields using shift and
mask, without consuming additional input bytes.

### tlv.bbq — Variable-length arrays

```
@endian little

TLVEntry = struct {
    type:   uint8,
    length: uint16,
    value:  bytes[length]
}

TLVStream = struct {
    entries: array<TLVEntry>(none, eof)
}
```

```python
>>> import bbq, struct
>>> spec = bbq.compile("examples/tlv.bbq")
>>> # Two TLV entries: type=1 "hello", type=2 "world"
>>> entry1 = struct.pack("<BH", 1, 5) + b"hello"
>>> entry2 = struct.pack("<BH", 2, 5) + b"world"
>>> result = spec.parse(entry1 + entry2, rule="TLVStream")
>>> result.success
True
>>> len(result.entries)
2
>>> int(result.entries[0].type)
1
>>> int(result.entries[0].length)
5
>>> bytes(result.entries[0]["value"])
b'hello'
>>> int(result.entries[1].type)
2
>>> bytes(result.entries[1]["value"])
b'world'
>>> for entry in result.entries:
...     print(f"type={int(entry.type)} len={int(entry.length)}")
type=1 len=5
type=2 len=5
```

`array<TLVEntry>(none, eof)` reads `TLVEntry` structs until the input is
consumed. Each entry's `value` field reads exactly `length` bytes, as
determined by the preceding field.
