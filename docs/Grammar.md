# BBQ (Binary Block Query) Language Reference

## 1. Overview

BBQ is a domain-specific language for describing the structure and constraints of
binary data formats. It combines parsing expression grammar concepts with a
declarative type system for validation and random-access parsing.

A BBQ specification can be compiled to a standalone parser (static codegen) or
interpreted directly — the same grammar yields the same parse either way.

## 2. Top-Level Structure

A `.bbq` file consists of zero or more directives and code blocks followed by
one or more rules.

```
grammar    = ( directive | code_block )* rule+
directive  = "@endian" ( "big" | "little" )
code_block = ( "@header" | "@source" | "@writer" ) "(." inline_code ".)"
rule       = identifier "=" type_body
```

### 2.1 Endian Directive

The `@endian` directive sets the default byte order for unsuffixed integer and float
types. Without a directive, the default is little-endian.

```bbq
@endian big

Header = struct {
    magic: uint32,       # reads as big-endian (from directive)
    flags: uint16le      # reads as little-endian (explicit suffix wins)
}
```

### 2.2 Rules

Every rule binds a name to a type expression. Rules are the unit of code generation —
each rule produces a C++ type and a parse function.

```bbq
Header = struct { magic: uint32be, version: uint16le }
Entries = array<Entry>(none, eof)
Flags = uint8
```

### 2.3 Code Blocks

Code blocks carry verbatim host-language code into the generated output, so a
spec can ship its own helpers (most commonly `where` predicates — see §7.4)
without a separate hand-maintained file. `@header` lands in the generated types
header (visible to every translation unit), `@source` in the reader, `@writer`
in the writer. The convention is declarations in `@header`, definitions in
`@source`/`@writer`.

```bbq
@header (.
bool name_utf8_ok(bbq_bytes_t b);
.)
@source (.
bool name_utf8_ok(bbq_bytes_t b) { /* … */ }
.)

Name = struct { count: uleb128, bytes: bytes[count] } where name_utf8_ok(bytes)
```

## 3. Type Expressions

### 3.1 Alternatives (Biased Choice)

```
type_body = type_term ( "|" type_term )*
```

The `|` operator creates biased choice — alternatives are tried left-to-right with
backtracking, and the first match wins (PEG semantics).

```bbq
Number = uint8 | uint16le | uint32le
```

### 3.2 Constraints and Intervals

Any type term can have an optional `where` constraint and/or interval:

```
type_term = type_primary [ "where" expr ] [ interval ]
```

### 3.3 Type Primary

```
type_primary = "struct"   struct_body
             | "union"    union_body
             | "array"    array_body
             | "optional" optional_body
             | "switch"   switch_body
             | "compute"  compute_body
             | "extern"   extern_body
             | "bitfield" bitfield_body
             | primitive_type
             | "(" type_body ")"
             | rule_ref
```

## 4. Compound Types

### 4.1 Struct

Ordered collection of named fields, parsed sequentially.

```
struct_body = "{" [ field ( "," field )* ","? ] "}" [ "where" expr ]
field       = identifier ":" type_body [ "where" expr ] [ interval ]
```

```bbq
Header = struct {
    magic:   uint32be where magic == 0x89504E47,
    version: uint16le,
    flags:   uint8,
    length:  uint32le where length > 0
}
```

A `where` after the closing brace constrains the **whole struct** — it runs after
all fields have parsed, with every field plus the context builtins (§7.3) in
scope. This is the place for whole-construct properties like checksums or
encoding validation, usually via a user predicate (§7.4):

```bbq
IPv4Header = struct {
    …
    checksum: uint16,
    …
} where ipv4_checksum_ok(@buffer, checksum)
```

### 4.2 Union

Named alternatives with backtracking. Each variant is tried in order; the first
successful parse wins.

```
union_body  = "{" [ variant ( "," variant )* ","? ] "}"
variant     = identifier ":" type_body [ "where" expr ] [ interval ]
```

```bbq
Message = union {
    text:   TextPayload,
    binary: BinaryPayload,
    error:  ErrorPayload
}
```

### 4.3 Array

Repeated elements with either a fixed count or a separator/terminator specification.

```
array_body   = "<" type_body ">" array_spec [ "resync" ]
               [ "where" expr ] [ interval ]
               [ "@" element_interval ]

array_spec   = "[" expr "]"                           # fixed count
             | "(" sep_spec "," term_spec ")"         # sep/term

sep_spec     = "none" | type_body
term_spec    = "eof"
             | "count" "(" expr ")"
             | "until" "(" expr ")"
             | type_body                              # type terminator
```

**Fixed count:**
```bbq
Colors = array<uint8>[3]
```

**Separator/terminator patterns:**
```bbq
Items   = array<Entry>(none, eof)                # read until end of input
Records = array<Record>(Comma, count(num_recs))  # with separator, counted
Packets = array<Packet>(none, until(remaining < 4))
Chunks  = array<Chunk>(none, PNGEnd)             # stop on type match
```

**Per-element intervals** (only on counted arrays) position each element at a
computed byte range using the loop variable `i`:

```bbq
entries: array<Entry>[num] @ [off + i * sz, off + (i + 1) * sz]
```

**Resync mode** lets the array recover from a per-element parse failure by
advancing one byte and retrying the same element type. Useful for forensic
parsing of corrupted streams or scanning for known record shapes in mixed
data.

```bbq
Records = array<Record> (none, eof) resync
```

When an element fails to parse at the current position (bytes-out-of-range,
where-check false, etc.), the parser restores to the start of the failed
attempt, advances `pos` by one, and tries again from the new position. On
EOF an `eof`-terminated array ends with the records gathered so far. A counted
`[count]` array that has not yet collected `count` records when EOF is reached
**fails** — a declared count left unmet is a malformed structure, never silently
returned short (the IPG correct-by-construction guarantee).

`resync` is rejected by sema when combined with `until` — the two have
contradictory semantics around per-element termination. Compatible with
`[count]`, `count(N)`, and `eof` terminators.

### 4.4 Optional

A value that may or may not be present. Without a constraint, uses try/restore
backtracking. With a constraint, the constraint controls presence.

```
optional_body = "<" type_body ">" [ "where" expr ] [ interval ]
```

```bbq
MaybeExt = optional<ExtHeader>
OptData  = optional<ExtraData> where flags & 0x01
```

### 4.5 Switch

Discriminated dispatch — selects a parse target based on an expression value.
No backtracking; the discriminator is evaluated once.

```
switch_body    = "(" expr ")" "{" switch_case* [ default_case ] "}"
switch_case    = case_value ":" type_body [ interval ] ";"
default_case   = "default" ":" ( type_body [ interval ] | "reject" ) ";"
case_value     = integer [ ".." integer ] | string | identifier | dotted_ref
```

Cases can be integer literals, inclusive integer **ranges** (`lo .. hi`), string
literals, bare identifiers (enum constants), or dotted references. The default
case can be `reject` (a contextual keyword) to make the fallthrough an explicit,
audit-visible parse failure — the idiom for fail-closed grammars where an
unlisted discriminant is malformed input, not data to skip.

```bbq
# Integer cases
Payload = switch(header.type) {
    1: TextPayload;
    2: BinaryPayload;
    default: RawPayload;
}

# Range cases + fail-closed default
Instr = switch(op) {
    0x00 .. 0x01: Empty;
    0x02 .. 0x03: Block;
    0x0e:         BrTable;
    default:      reject;
}

# String cases
Format = switch(content_type) {
    "json": JsonBody;
    "xml":  XmlBody;
}

# Enum/identifier cases
Action = switch(cmd) {
    CMD_READ:  ReadPayload;
    CMD_WRITE: WritePayload;
}
```

### 4.6 Compute

A derived field — evaluates an expression without consuming input. The result type
is specified after a colon.

```
compute_body = "(" expr ":" primitive_keyword ")"
```

```bbq
IPv4Header = struct {
    ver_ihl: uint8,
    version: compute((ver_ihl >> 4) & 0x0F : uint8),
    ihl:     compute(ver_ihl & 0x0F : uint8)
}
```

### 4.7 Extern

Delegates parsing to a user-provided C++ function. The function name and C++ return
type are specified as string literals.

```
extern_body = "(" string "," string ")"
              [ "where" expr ] [ interval ]
```

```bbq
Payload = extern("decompress_zlib", "std::vector<uint8_t>")
```

The extern is supplied to the runtime that executes the spec. With the Python
module, register a callable:
```python
spec.register_extern("decompress_zlib", fn)   # fn(memoryview) -> int | None
```
`fn` receives the remaining input and returns the number of bytes consumed, or
`None` to fail the parse.

### 4.8 Bitfield

Extracts sub-byte fields from an integer container using shift-mask operations.
The container type provides byte count, signedness, and endianness. Entry widths
must sum to the container's total bit count.

Big-endian containers use MSB-first ordering (first entry at highest bits).
Little-endian containers use LSB-first ordering (first entry at lowest bits).

```
bitfield_body = "<" primitive_keyword ">"
                "{" [ bitfield_entry ( "," bitfield_entry )* ","? ] "}"

bitfield_entry = identifier ":" integer
```

```bbq
# IP header flags (big-endian, MSB-first)
Header = struct {
    flags: bitfield<uint16be> {
        version: 4,
        ihl: 4,
        dscp: 6,
        ecn: 2
    },
    total_length: uint16be
}

# TCP flags (standalone rule)
TCPFlags = bitfield<uint8> {
    reserved: 2,
    urgent: 1,
    ack: 1,
    push: 1,
    reset: 1,
    syn: 1,
    fin: 1
}
```

## 5. Primitive Types

```
primitive_type = primitive_keyword [ "where" expr ] [ interval ]
```

### 5.1 Integer Types

| Type | Size | Signedness |
|------|------|------------|
| `uint8` | 1 byte | unsigned |
| `uint16`, `uint16le`, `uint16be` | 2 bytes | unsigned |
| `uint32`, `uint32le`, `uint32be` | 4 bytes | unsigned |
| `uint64`, `uint64le`, `uint64be` | 8 bytes | unsigned |
| `int8` | 1 byte | signed |
| `int16`, `int16le`, `int16be` | 2 bytes | signed |
| `int32`, `int32le`, `int32be` | 4 bytes | signed |
| `int64`, `int64le`, `int64be` | 8 bytes | signed |
| `uleb128` | 1–5 bytes | unsigned LEB128, u32 value range |
| `uleb64` | 1–10 bytes | unsigned LEB128, u64 value range |
| `sleb128` | 1–5 bytes | signed LEB128, s32 value range |
| `sleb64` | 1–10 bytes | signed LEB128, s64 value range |

Unsuffixed multi-byte types use the `@endian` directive's default (little if unset).

The LEB128 varint types are strict on read — an encoding longer than the value
range allows (or with set bits beyond it) is a parse error, not a truncation —
and canonical (minimal-length) on write. Like any integer type they can carry
`where` constraints, drive switches, size arrays, and scope `@rest` intervals
(§6).

### 5.2 Float Types

| Type | Size |
|------|------|
| `float32`, `float32le`, `float32be` | 4 bytes |
| `float64`, `float64le`, `float64be` | 8 bytes |

### 5.3 Other Types

| Type | C++ Type | Notes |
|------|----------|-------|
| `bool` | `bool` | Reads 1 byte, nonzero = true |
| `bytes` | `std::vector<uint8_t>` | Requires length interval: `bytes[N]` |
| `string` | `std::string` | Requires length interval: `string[N]` |

## 6. Intervals

Intervals specify byte ranges for parsing.

```
interval = "[" expr [ "," expr ] "]"
rest     = integer_field "@rest"
```

- `[start, end]` — seek to `start`, parse until `end` (absolute positions)
- `[length]` — read `length` bytes from current position (for `bytes` and `string`)

```bbq
File = struct {
    header:  Header[0, 64],
    payload: Data[header.offset, header.offset + header.size],
    footer:  Footer[EOI - 16, EOI]
}

Record = struct {
    len:  uint16le,
    data: bytes[len],
    tag:  string[4]
}
```

### 6.1 `@rest` — size-prefixed windows

An integer field marked `@rest` declares that its value is the byte length of
the **rest of the enclosing struct** — the size-prefix idiom (TLV payloads,
WASM sections, archive members). All subsequent fields parse inside that
window; `@end`/`@remaining` reflect it.

```bbq
Section = struct {
    id:   uint8,
    size: uleb128 @rest,       # everything after `size` is exactly `size` bytes
    body: switch(id) { … }
}
```

`@rest` windows are **confined**: a window that extends past its parent (or the
input) fails the parse at the size field — it is never silently clamped — and
the struct must consume the window **exactly**; leftover bytes at the close are
a parse error ("size mismatch"), since a size prefix means *is*, not *at most*.
Nested `@rest` scopes nest (a child window must lie inside the parent's).

On the write side, a `uleb` `@rest` size is **recomputed from the serialized
rest**, not echoed from the struct — so programmatically-built or edited trees
serialize with correct sizes. Fixed-width `@rest` fields write their stored
value. The seek-based `[start, end]` windows are random-access views and carry
no exact-consumption requirement.

## 7. Expressions

BBQ has a unified expression language for constraints, intervals, array counts,
switch discriminators, and compute values.

### 7.1 Operator Precedence (high to low)

| Precedence | Operators | Description |
|------------|-----------|-------------|
| 1 | `()` | Grouping, function calls |
| 2 | `-` `~` `!` `abs` | Unary negation, bitwise not, logical not, absolute value |
| 3 | `*` `/` `%` | Multiplicative |
| 4 | `+` `-` | Additive |
| 5 | `<<` `>>` | Bit shift |
| 6 | `<` `<=` `>` `>=` | Relational |
| 7 | `==` `!=` | Equality |
| 8 | `&` | Bitwise AND |
| 9 | `^` | Bitwise XOR |
| 10 | `|` | Bitwise OR |
| 11 | `&&` | Logical AND |
| 12 | `||` | Logical OR |
| 13 | `? :` | Ternary conditional |

### 7.2 Primary Expressions

```
primary = integer | float | string | "true" | "false" | "EOI"
        | identifier "(" [ expr ( "," expr )* ] ")"    # function call
        | "@" identifier                                # context builtin
        | identifier                                    # field reference
        | "(" expr ")"
```

### 7.3 References and Context Builtins

Field references access previously parsed fields in the same struct. Dot notation
accesses nested fields; bracket notation indexes arrays. A **bare identifier is
always a field reference** — the parse-state environment lives behind the
reserved `@` sigil, so fields named `pos`, `buffer`, or `i` never collide with it.

```bbq
header.length           # nested field
entries[i].size         # array element field
peek()                  # byte value at current position (without consuming)
EOI                     # end-of-input position
```

The context builtins are a closed set (an unknown `@name` is a compile error):

| Builtin | Meaning |
|---------|---------|
| `@start` | the enclosing construct's entry offset |
| `@end` | the active end bound (innermost interval, or end of input) |
| `@pos` | the parse cursor |
| `@index` | the current array/loop index |
| `@buffer` | the construct's consumed bytes, `input[@start..@pos)` |
| `@remaining` | `@end - @pos` |

Together with the in-scope field values these form the complete environment of
any expression: a field's *name* is its decoded value, `@buffer` is the raw
on-wire bytes (e.g. a `uleb128` field's name is the integer; `@buffer` over it
is the raw varint bytes).

### 7.4 Function Calls

The built-in math functions (`abs`, `min`, `max`, `clamp`) map to the runtime
(`bbq_…`/`bbq::…` in generated code). **Any other name is a user function**,
called verbatim — supply it via code blocks (§2.3): declare in `@header`, define
in `@source` (and/or `@writer`). The dominant use is bool-returning `where`
predicates: a field passes as its decoded value (a `bytes` field as the
generated bytes type), and `@buffer` passes the raw consumed bytes, so a
predicate can validate either view.

```bbq
crc32(@buffer, length)
abs(offset)

# struct-level where calling a user predicate (declared in @header)
Name = struct { count: uleb128, bytes: bytes[count] } where name_utf8_ok(bytes)
```

## 8. Lexical Elements

### 8.1 Identifiers

```
identifier = [a-zA-Z_][a-zA-Z0-9_]*
```

Identifiers start with a letter or underscore, followed by letters, digits, or
underscores.

### 8.2 Numbers

```
integer = [0-9]+                    # decimal
        | "0x" [0-9a-fA-F]+        # hexadecimal
        | "0b" [01]+               # binary
float   = [0-9]+ "." [0-9]*
```

### 8.3 Strings

```
string = '"' [^"\r\n]* '"'
```

### 8.4 Comments

```
comment = "#" [^\n]* newline
        | "//" [^\n]* newline
```

Both `#` and `//` introduce line comments.

### 8.5 Whitespace

Spaces, tabs, carriage returns, and newlines are ignored outside of tokens.

## 9. Examples

### 9.1 File Header with Validation

```bbq
Header = struct {
    magic:   uint32be where magic == 0x89504E47,
    version: uint16le,
    flags:   uint8,
    length:  uint32le where length > 0
}
```

### 9.2 IPv4 Packet Header

```bbq
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

### 9.3 Type-Length-Value Protocol

```bbq
@endian little

TextData = struct { data: uint8 }
BinData  = struct { data: uint16 }

TLVEntry = struct {
    type:   uint8,
    length: uint16,
    value:  switch(type) {
        1: TextData;
        2: BinData;
        default: TextData;
    }
}

TLVStream = struct {
    entries: array<TLVEntry>(none, eof)
}
```

### 9.4 Random-Access Table with Per-Element Intervals

```bbq
Entry = struct { x: uint8 }

FileWithTable = struct {
    num_entries: uint32le,
    table_off:  uint32le,
    entry_sz:   uint32le,
    entries:    array<Entry>[num_entries]
                @ [table_off + i * entry_sz, table_off + (i + 1) * entry_sz]
}
```

### 9.5 Bitfield Parsing

```bbq
@endian big

IPHeader = struct {
    flags: bitfield<uint16be> {
        version: 4,
        ihl: 4,
        dscp: 6,
        ecn: 2
    },
    total_length: uint16be,
    rest: bytes[total_length - 4]
}

TCPFlags = bitfield<uint8> {
    reserved: 2,
    urgent: 1,
    ack: 1,
    push: 1,
    reset: 1,
    syn: 1,
    fin: 1
}
```

### 9.6 External Parser Call

```bbq
Chunk = struct {
    length:     uint32le,
    compressed: extern("decompress_zlib", "std::vector<uint8_t>")
}
```
