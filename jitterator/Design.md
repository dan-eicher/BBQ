# Jitterator — Copy-and-Patch JIT Generator

A tool and runtime library that lets you write JIT stencils in C, compiled
by Clang with the `preserve_none` calling convention, and stitched together
at runtime using Copy-and-Patch compilation. You hand-write the stencils.
Jitterator handles extraction from ELF object files and provides the
runtime patching machinery.

Based on Xu & Kjolstad's Copy-and-Patch technique (OOPSLA '21), adapted
for continuation-graph compilation.

## Key Ideas

1. **Stencils are C functions.** Each stencil is a small
   `__attribute__((preserve_none))` function compiled by Clang. No code
   generation from a DSL — you write the C yourself.

2. **Holes are extern symbols.** Stencils reference undefined extern
   symbols for values that vary at JIT compile time (continuation
   addresses, constants, slot offsets). These produce ELF relocations
   that `jitterator` discovers automatically.

3. **One tool.** `jitterator` reads a compiled `.o` file, takes every
   global function as a stencil, reads their relocations, and outputs
   a `stencil_table.h` with code bytes and hole descriptors.

4. **CPS via tail calls.** Stencils pass control to the next stencil
   by tail-calling a continuation hole. With `preserve_none`, this
   compiles to a single `jmp` instruction — no prologue, no epilogue,
   no stack frame.

5. **Continuation graphs map directly.** Any CEK machine's continuation
   graph (DDCG) can be JIT-compiled by walking the graph and emitting
   stencils. The continuation pointers in the graph become patched
   jump targets in the generated code.

## Architecture

```
Hand-written          Clang              jitterator
stencils.c ─────────► stencils.o ──────► stencil_table.h
(preserve_none        (ELF64 with         (code bytes +
 functions with        relocations)        hole descriptors)
 extern holes)

                    Runtime library
                    ├── CodeBuffer      (mmap RW → mprotect RX)
                    └── emit_stencil    (memcpy + constant pool + patch holes)
```

No code generator. No .def file. No template engine. No interpreter.
The stencil C source is the single source of truth.

## Stencil Authoring

### Calling Convention

All stencils use `__attribute__((preserve_none))`, which:
- Passes up to 12 arguments in registers (rdi, rsi, rdx, rcx, r8, r9,
  r11, r12, r13, r14, r15, rax)
- Makes ALL registers caller-saved — no callee-saved registers
- Eliminates function prologues/epilogues (nothing to save/restore)
- Tail calls compile to plain `jmp` instructions

This is the same calling convention used by CPython's JIT and the
original Copy-and-Patch paper (which used GHC calling convention,
the predecessor of `preserve_none`).

Requires Clang 19+. GCC does not support `preserve_none`.

### Macros

Stencil source files use two convenience macros:

```c
#define STENCIL __attribute__((preserve_none))
#define TAIL    __attribute__((musttail))
```

`STENCIL` marks the calling convention. `TAIL` guarantees the compiler
emits a tail call — if it can't, compilation fails rather than silently
generating a non-tail call that would break the stencil chain.

### Stencil Structure

A stencil is a `preserve_none` function that:
1. Receives the execution context in registers
2. Does its computation
3. Tail-calls its continuation (another stencil) — must use `TAIL`

```c
// stencils.c

#define STENCIL __attribute__((preserve_none))
#define TAIL    __attribute__((musttail))

// Continuation holes — tail-called, become jmp instructions
extern void STENCIL _HOLE_cont(int64_t *slots);

// Data holes — loaded via RIP-relative addressing, need constant pool
extern uint64_t _HOLE_dst;
extern uint64_t _HOLE_lhs;
extern uint64_t _HOLE_rhs;

void STENCIL add(int64_t *slots) {
    slots[_HOLE_dst] = slots[_HOLE_lhs] + slots[_HOLE_rhs];
    TAIL return _HOLE_cont(slots);
}
```

Clang compiles this to something like:
```asm
add:
    mov  rax, [rip+0]          ; load _HOLE_lhs from constant pool
    mov  rcx, [rip+0]          ; load _HOLE_rhs from constant pool
    mov  rcx, [r12+rcx*8]      ; slots[rhs]
    add  rcx, [r12+rax*8]      ; + slots[lhs]
    mov  rax, [rip+0]          ; load _HOLE_dst from constant pool
    mov  [r12+rax*8], rcx      ; slots[dst] = result
    jmp  0                     ; _HOLE_cont → relocation
```

No prologue. No epilogue. No stack frame. The `TAIL return _HOLE_cont(slots)`
becomes a `jmp` because `preserve_none` + `musttail` = guaranteed tail call.

### Two Kinds of Holes

`extern uint64_t` data holes and `extern void` continuation holes produce
different ELF relocations and are patched differently:

| Declaration | Compiler output | ELF relocation | Patch type |
|---|---|---|---|
| `extern void __attribute__((preserve_none)) _HOLE_cont(...)` | `jmp 0` / `call 0` | `R_X86_64_PLT32` | `PATCH_REL_BRANCH` — 32-bit displacement patched directly |
| `extern uint64_t _HOLE_value` | `mov rax, [rip+0]` | `R_X86_64_PC32` | `PATCH_REL_DATA` — value stored in constant pool, displacement points to pool entry |

Data holes generate RIP-relative memory loads — the compiler emits
`mov rax, [rip+disp32]` to load the value of the extern. At JIT time,
the actual value must be stored near the code in a **constant pool**
so the RIP-relative offset can reach it. Branch holes are simpler:
the displacement is patched to point directly at the target code.

### Entry and Exit

The first stencil emitted should be an **entry** stencil that transitions
from the standard C calling convention to `preserve_none`:

```c
// Called from regular C code — note: NOT preserve_none, NOT TAIL.
// Crossing calling conventions requires a call (not a tail call) so the
// compiler generates a prologue/epilogue that saves callee-saved regs.
void entry(int64_t *slots) {
    return ((void STENCIL (*)(int64_t*))_HOLE_cont)(slots);
}
```

The compiler generates:
```asm
entry:
    push r15              ; save callee-saved regs (compiler-generated)
    push r14
    push r13
    push r12
    push rbx
    mov  r12, rdi         ; slots ptr → r12 (preserve_none arg register)
    call _HOLE_cont       ; call into preserve_none world
    pop  rbx              ; ← halt's ret lands here
    pop  r12
    pop  r13
    pop  r14
    pop  r15
    ret                   ; return to C caller
```

The last stencil is a **halt** stencil — a `preserve_none` function that
returns to the caller:

```c
void STENCIL halt(int64_t *slots) {
    return;  // just ret — returns to entry's epilogue
}
```

The entry's `call` pushes a return address. The preserve_none stencils
chain via `jmp` (no stack effects). Halt's `ret` pops that return address
and lands back in entry's cleanup code. The C caller sees a normal
function call and return.

### External Function Calls

When a stencil needs to call a regular C function (not a stencil), the
function pointer is passed as a hole:

```c
extern uint64_t _HOLE_extern_fn;
extern uint64_t _HOLE_extern_arg;

void STENCIL call_extern(int64_t *slots) {
    typedef int64_t (*ext_fn_t)(int64_t*);
    ext_fn_t fn = (ext_fn_t)_HOLE_extern_fn;
    int64_t result = fn(slots);
    slots[_HOLE_extern_arg] = result;
    TAIL return _HOLE_cont(slots);
}
```

Because `preserve_none` has no callee-saved registers, the compiler
freely uses whatever registers it needs for the call and restores
the stencil's state from the stack afterward. No register conflicts.

## jitterator

One tool. Reads a `.o` file, outputs a header.

### Input

An ELF64 relocatable object file compiled from the stencils source.
`jitterator` uses the existing BBQ-generated ELF64 parser to read:

- **Symbol table** — every global function symbol (STT_FUNC + STB_GLOBAL)
  becomes a stencil. Static helpers (STB_LOCAL) are ignored.
- **`.text` section** — the machine code bytes
- **Relocation entries** (`.rela.text`) — each relocation targeting a
  `_HOLE_*` symbol is a hole to be patched

### Relocation Classification

Jitterator classifies relocations by ELF relocation type:

| ELF Relocation     | Patch type         | Meaning |
|--------------------|--------------------|----|
| `R_X86_64_PLT32`   | `PATCH_REL_BRANCH` | Branch/call — 32-bit PC-relative displacement |
| `R_X86_64_PC32`    | `PATCH_REL_DATA`   | Data load — RIP-relative, needs constant pool entry |
| `R_X86_64_64`      | `PATCH_ABS64`      | 64-bit absolute |
| `R_X86_64_32S`     | `PATCH_ABS32S`     | 32-bit signed absolute |

The distinction between `PATCH_REL_BRANCH` and `PATCH_REL_DATA` drives
the constant pool: each data hole gets an 8-byte slot in the pool,
and the RIP-relative displacement is patched to point at that slot.

### Output

```c
// stencil_table.h — generated by jitterator. DO NOT EDIT.
#pragma once
#include <stdint.h>

typedef enum {
    STENCIL_ADD,
    STENCIL_SUB,
    STENCIL_HALT,
    STENCIL_ENTRY,
    // ...
    STENCIL_COUNT
} StencilId;

typedef enum {
    PATCH_ABS64,        // 64-bit absolute
    PATCH_ABS32S,       // 32-bit signed absolute
    PATCH_REL_BRANCH,   // 32-bit PC-relative branch (patched directly)
    PATCH_REL_DATA,     // 32-bit PC-relative data (needs constant pool)
} PatchType;

typedef struct {
    uint32_t offset;        // byte offset within stencil code
    PatchType type;         // how to patch
    uint16_t hole_index;    // index into the stencil's hole array
    const char *hole_name;  // original symbol name (e.g., "_HOLE_cont")
} PatchEntry;

typedef struct {
    const uint8_t *code;
    uint32_t code_size;
    const PatchEntry *patches;
    uint32_t patch_count;
    uint16_t hole_count;          // number of unique holes
    uint16_t data_hole_count;     // holes needing constant pool (8 bytes each)
    const char **hole_names;      // hole names indexed by hole_index
} StencilDef;

static const StencilDef stencil_table[STENCIL_COUNT] = { ... };
```

Each unique `_HOLE_*` symbol referenced by a stencil gets a hole index
(assigned in order of first appearance). Multiple relocations can
reference the same hole (e.g., the same constant used twice).

### Discovery Rules

- **Stencils**: every global function symbol (STT_FUNC + STB_GLOBAL)
  in the `.o` file becomes a stencil. The symbol name becomes the enum
  name (uppercased). Static helpers (STB_LOCAL) are ignored.
- **Holes**: any relocation in a stencil's code range whose target symbol
  starts with `_HOLE_`. Other relocations (to libc, to static helpers)
  are left as-is — they're internal to the stencil.
- **Stencil boundaries**: determined by symbol addresses and sizes from
  the ELF symbol table.

## Runtime Library

### CodeBuffer

Manages an mmap'd region. Write code as RW, finalize as RX:

```cpp
namespace jitterator {

class CodeBuffer {
public:
    explicit CodeBuffer(size_t initial_capacity = 4096);
    void emit_bytes(const uint8_t *data, size_t len);
    void patch_32(size_t offset, int32_t value);
    void patch_64(size_t offset, uint64_t value);
    size_t size() const;
    uint8_t *data();
    void *finalize();  // mprotect RX, icache flush, return base
    void reset();      // mprotect RW, reset size to 0
};

} // namespace jitterator
```

Header-only: `jitterator/runtime/jitterator.h`.

### Emitting Stencils

The consumer emits stencils with a function like `emit_stencil` that:
1. Copies the stencil's code bytes into the buffer
2. Allocates a constant pool after the code (8 bytes per data hole)
3. Writes data hole values into the constant pool
4. Patches `PATCH_REL_DATA` displacements to point at pool entries
5. Patches `PATCH_REL_BRANCH` displacements (or leaves them as 0
   for backpatching)

```cpp
static size_t emit_stencil(jitterator::CodeBuffer &buf,
                           const StencilDef &stencil,
                           const uint64_t *values)
{
    size_t code_base = buf.size();
    buf.emit_bytes(stencil.code, stencil.code_size);

    // Allocate constant pool for data holes (8 bytes each, after code)
    size_t pool_base = buf.size();
    int16_t pool_slot[64] = {};
    memset(pool_slot, -1, sizeof(pool_slot));
    uint16_t next_slot = 0;
    uint8_t zeros[8] = {};

    for (uint32_t i = 0; i < stencil.patch_count; i++) {
        auto &p = stencil.patches[i];
        if (p.type == PATCH_REL_DATA && pool_slot[p.hole_index] < 0) {
            pool_slot[p.hole_index] = next_slot++;
            buf.emit_bytes(zeros, 8);
        }
    }

    // Write values into constant pool
    for (uint16_t hi = 0; hi < stencil.hole_count; hi++) {
        if (pool_slot[hi] >= 0) {
            size_t entry_off = pool_base + pool_slot[hi] * 8;
            uint64_t val = values ? values[hi] : 0;
            buf.patch_64(entry_off, val);
        }
    }

    // Patch relocations
    for (uint32_t i = 0; i < stencil.patch_count; i++) {
        auto &p = stencil.patches[i];
        size_t patch_addr = code_base + p.offset;

        switch (p.type) {
        case PATCH_REL_BRANCH: {
            uint64_t target = values ? values[p.hole_index] : 0;
            if (target != 0) {
                uintptr_t patch_abs = (uintptr_t)buf.data() + patch_addr;
                int32_t disp = (int32_t)(target - (patch_abs + 4));
                buf.patch_32(patch_addr, disp);
            }
            break;
        }
        case PATCH_REL_DATA: {
            size_t entry_off = pool_base + pool_slot[p.hole_index] * 8;
            int32_t disp = (int32_t)(entry_off - (patch_addr + 4));
            buf.patch_32(patch_addr, disp);
            break;
        }
        case PATCH_ABS64:
            buf.patch_64(patch_addr, values ? values[p.hole_index] : 0);
            break;
        case PATCH_ABS32S:
            buf.patch_32(patch_addr, (int32_t)(values ? values[p.hole_index] : 0));
            break;
        }
    }

    return code_base;
}
```

Note: for `PATCH_REL_DATA`, the displacement is relative within the
buffer (both code and pool are in the same buffer, so the base address
cancels). For `PATCH_REL_BRANCH`, the target is an absolute address,
so the displacement must account for the buffer's base address.

### Backpatching

Branch holes left as 0 during emission are patched after all stencils
are emitted and their addresses are known:

```cpp
static void backpatch(jitterator::CodeBuffer &buf,
                      size_t stencil_base,
                      const StencilDef &stencil,
                      uint16_t hole_index,
                      uint64_t target)
{
    for (uint32_t i = 0; i < stencil.patch_count; i++) {
        auto &p = stencil.patches[i];
        if (p.hole_index != hole_index) continue;
        if (p.type != PATCH_REL_BRANCH) continue;

        size_t patch_addr = stencil_base + p.offset;
        uintptr_t patch_abs = (uintptr_t)buf.data() + patch_addr;
        int32_t disp = (int32_t)(target - (patch_abs + 4));
        buf.patch_32(patch_addr, disp);
    }
}
```

## CMake Integration

```cmake
# Jitterator.cmake

function(jitterator_stencils)
    cmake_parse_arguments(JT "" "NAME;SOURCES" "CC_FLAGS" ${ARGN})

    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/${JT_NAME}_gen")
    file(MAKE_DIRECTORY ${GEN_DIR})

    # Compile stencils with Clang + preserve_none
    find_program(CLANG_CC clang REQUIRED)
    if(NOT JT_CC_FLAGS)
        set(JT_CC_FLAGS -O2)
    endif()

    add_custom_command(
        OUTPUT ${GEN_DIR}/stencils.o
        COMMAND ${CLANG_CC} ${JT_CC_FLAGS}
                -c ${JT_SOURCES}
                -o ${GEN_DIR}/stencils.o
        DEPENDS ${JT_SOURCES}
        COMMENT "Compiling ${JT_NAME} stencils"
    )

    # Extract stencil table
    add_custom_command(
        OUTPUT ${GEN_DIR}/stencil_table.h
        COMMAND $<TARGET_FILE:jitterator>
                ${GEN_DIR}/stencils.o
                -o ${GEN_DIR}/stencil_table.h
        DEPENDS ${GEN_DIR}/stencils.o jitterator
        COMMENT "jitterator: extracting ${JT_NAME} stencil table"
    )

    # Interface library for include path + runtime
    add_library(${JT_NAME} INTERFACE)
    target_include_directories(${JT_NAME} INTERFACE ${GEN_DIR} ${RUNTIME_DIR})

    add_custom_target(${JT_NAME}_gen DEPENDS ${GEN_DIR}/stencil_table.h)
    add_dependencies(${JT_NAME} ${JT_NAME}_gen)
endfunction()
```

## Consumer Usage

### Walking a Continuation Graph

The primary use case. A CEK machine compiles an AST to a continuation
graph (DDCG). The JIT compiler walks the graph and emits stencils:

```cpp
#include "stencil_table.h"
#include "jitterator.h"

// Two-pass compilation:
// Pass 1: emit stencils, record locations for backpatching
// Pass 2: patch continuation holes with resolved addresses

jitterator::CodeBuffer buf;

// Map from graph node → code offset
std::unordered_map<Node*, size_t> offsets;

// Pass 1: emit all stencils (data holes filled, branch holes left as 0)
for (auto *node : topo_order(graph)) {
    size_t off = emit_stencil(buf, stencil_for(node), values_for(node));
    offsets[node] = off;
}

// Pass 2: backpatch branch continuations
uintptr_t base = (uintptr_t)buf.data();
for (auto *node : topo_order(graph)) {
    for (auto &[hole_idx, target_node] : continuations_of(node)) {
        backpatch(buf, offsets[node], stencil_for(node),
                  hole_idx, base + offsets[target_node]);
    }
}

auto fn = (exec_fn)buf.finalize();
fn(slots);
```

### Proof-of-Life — Calculator

Stencils (`example/calc_stencils.c`):
```c
#define STENCIL __attribute__((preserve_none))
#define TAIL    __attribute__((musttail))

extern void STENCIL _HOLE_cont(int64_t *slots);
extern uint64_t _HOLE_dst;
extern uint64_t _HOLE_lhs;
extern uint64_t _HOLE_rhs;
extern uint64_t _HOLE_value;

void STENCIL add(int64_t *slots) {
    slots[_HOLE_dst] = slots[_HOLE_lhs] + slots[_HOLE_rhs];
    TAIL return _HOLE_cont(slots);
}

void STENCIL halt(int64_t *slots) {
    return;
}

// No TAIL — crossing calling conventions requires call, not tail call
void entry(int64_t *slots) {
    return ((void STENCIL (*)(int64_t*))_HOLE_cont)(slots);
}
```

Consumer (`example/calc.cpp`) compiles `slots[2] = slots[0] + slots[1]`:
```cpp
jitterator::CodeBuffer buf;

size_t entry_off = emit_stencil(buf, stencil_table[STENCIL_ENTRY], ...);
size_t add_off   = emit_stencil(buf, stencil_table[STENCIL_ADD], ...);
size_t halt_off  = emit_stencil(buf, stencil_table[STENCIL_HALT], nullptr);

// Backpatch: entry → add → halt
uintptr_t base = (uintptr_t)buf.data();
backpatch(buf, entry_off, ..., base + add_off);
backpatch(buf, add_off,   ..., base + halt_off);

auto fn = (void(*)(int64_t*))buf.finalize();
int64_t slots[3] = {10, 20, 0};
fn(slots);
assert(slots[2] == 30);  // passes
```

## Project Structure

```
jitterator/
├── src/
│   └── jitterator.cpp             # the tool (ELF reader + header generator)
├── extract/
│   ├── elf64.bbq                  # BBQ spec for ELF64 object files
│   ├── elf64Types.h               # generated by bbqc
│   ├── elf64Parser.h              # generated by bbqc
│   └── elf64Parser.cpp            # generated by bbqc
├── runtime/
│   └── jitterator.h               # CodeBuffer (header-only)
├── example/
│   ├── calc_stencils.c            # calculator stencils (proof of life)
│   ├── calc_stencil_table.h       # generated by jitterator
│   └── calc.cpp                   # calculator consumer
└── CMakeLists.txt
```

### Dependencies

| Dependency | When | Purpose |
|---|---|---|
| Clang 19+ | Stencil compilation | `preserve_none` support |
| C++ stdlib | Runtime | CodeBuffer internals |
| BBQ (bbqc) | Jitterator build | ELF64 parser for jitterator |

The consumer's project can use any C++ compiler. Only the stencil `.c`
file needs Clang.

## BBQ Integration

The BBQ JIT backend would reuse the existing DDCG compiler. The pipeline:

```
BBQ source
    │
    ▼
Parser + Sema (shared frontend)
    │
    ▼
DDCG Compiler (shared with CEK interpreter)
    │
    ▼
Continuation Graph
    │
    ├──► CEK trampoline VM (interpreter path)
    │
    └──► Copy-and-Patch JIT (compiled path)
         Walk graph, emit stencils, patch continuations
         → callable function pointer
```

Stencils would correspond to DDCG node types:

| DDCG Node          | Stencil          | Holes                    |
|--------------------|------------------|--------------------------|
| ReadNode           | read_*           | cont, field_offset       |
| SequenceNode       | (fall-through)   | (none — just emit next)  |
| ComputeNode        | compute_*        | cont, operands, dest     |
| AlternativeNode    | alt_save         | cont_try, cont_fail      |
| StructBeginNode    | struct_begin     | cont, name               |
| StructEndNode      | struct_end       | cont                     |
| HaltNode           | halt             | (none)                   |

## Comparison with Previous Design (jitgen)

| Aspect | Old (Ertl & Gregg) | New (Copy-and-Patch) |
|--------|--------------------|-----------------------|
| Fragment source | Generated from .def | Hand-written C |
| Calling convention | System V + `-ffixed` hacks | `preserve_none` |
| Hole discovery | Two-variant byte diff | ELF relocations |
| Object files | Two (variant A + B) | One |
| Tools | jitgen-emit + jitgen-extract | jitterator only |
| Interpreter | Generated (threaded-code) | None (CEK VM is the interpreter) |
| Prologue/epilogue | Special opcode or trampoline | Entry stencil (compiler-generated) |
| Register safety | `-ffixed-rbx -ffixed-r12 ...` | Zero callee-saved regs by design |
| External calls | `ctx->fn[]` table | Function pointer holes |
| Templates | 5 inja templates | None |
| .def parser | pegc-generated PEG parser | None |
| Consumer model | Emit low-level opcodes | Walk continuation graph |

## References

- Haoran Xu, Fredrik Kjolstad. "Copy-and-Patch Compilation." OOPSLA '21.
- M. Anton Ertl, David Gregg. "Retargeting JIT compilers by using
  C-compiler generated executable code." PACT '04.
- Philip Zucker. "Copy and Micropatch: Writing Binary Patches in C with
  Clang preserve_none."
- CPython JIT (PEP 744) — production Copy-and-Patch implementation.
- BBQ CEK Machine: `docs/CEK Machine Design.md`
