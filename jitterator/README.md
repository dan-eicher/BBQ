# jitterator — copy-and-patch stencil extractor

Reads one relocatable object file of hand-written stencils and writes a C header
describing them: the code bytes, where the holes are, and how each hole is
patched. A consumer stamps those bytes into an executable buffer at run time and
fills the holes — compilation as `memcpy` plus fixups, no code generator at run
time. The technique is Xu & Kjolstad's *Copy-and-Patch Compilation* (OOPSLA 2021).

**No dependency, deliberately.** The whole tool is an ELF reader — generated from
`extract/elf64.bbq` by BBQ's own C++ backend — plus a symbol table, a relocation
table, and a template. Other copy-and-patch implementations reach for LLVM to
build their stencils; this one reads the `.o` a plain `clang -c` already produced.
That is the design, not a stage on the way to something bigger: see *Where the
field has moved*.

```sh
jitterator stencils.o -o stencil_table.h     # or to stdout with no -o
```

`Design.md` is the long form: calling convention, hole kinds, the runtime, the
CMake wiring. This is the short one.

## The stencil contract

A stencil is a global function in the object. It ends in a `musttail` call so
stamped stencils chain without growing a stack, and it uses `preserve_none` so
there are no callee-saved registers to fight over.

```c
#define STENCIL __attribute__((preserve_none))
#define TAIL    __attribute__((musttail))

extern uint64_t _HOLE_v;                    /* a value the driver plugs   */
extern void STENCIL _HOLE_cont(int64_t*);   /* the next stencil           */

void STENCIL op_add(int64_t* s) {
    s[0] += (int64_t)(uintptr_t)&_HOLE_v;
    TAIL return _HOLE_cont(s);
}
```

**Build stencils with `-fno-pic -mcmodel=small`.** This is not a preference. PIC
turns `&_HOLE_v` into a GOT-relative load and a stencil copied into a fresh buffer
has no GOT; the large code model turns it into a 64-bit PC-relative field that
does not fit the 32-bit patch. jitterator refuses both rather than emitting a
stencil that is quietly wrong — many distributions default to PIE, so say it.

**A native is called through its address, never branched to.** Declare it as
`extern uint64_t _HOLE_fn` and call through the pointer. That makes it a data hole
resolved out of the constant pool, which holds a full 64-bit address. A direct
call would be a 32-bit PC-relative branch, and a JIT buffer typically lands tens
of thousands of GB from the host's text — far outside rel32's ±2 GB. (JITs that
do branch to native code solve this with trampolines; jitterator sidesteps it, and
`jcb_patch_rel32` checks that the sidestep holds.)

**A constant the compiler invents cannot be patched.** A float literal becomes a
`.rodata` entry reached RIP-relatively, and that reference dangles the moment the
code is copied. jitterator refuses it and says which op. Either move the op into a
native, or hoist the constant into a hole of its own — which is what the calc
stencils do with `_HOLE_k41e0000000000000` and friends.

## What it refuses

Each of these is a stencil that would be silently wrong, so it is an error with
the offending symbol named:

| | |
|---|---|
| a relocation that is not against a `_HOLE_` symbol | a compiler constant; cannot be copy-and-patched |
| a relocation type other than `PC32`, `PLT32`, `64`, `32S` | GOT-relative, 64-bit PC-relative, … — not this patcher's shapes |
| a symbol with size 0, or one running past `.text` | a stencil with no body stamps nothing and falls off the buffer |

## Patch kinds

| | |
|---|---|
| `PATCH_REL_BRANCH` | 32-bit PC-relative branch — a tail call to another stencil in the same buffer |
| `PATCH_REL_DATA` | 32-bit PC-relative data reference — resolved through a constant-pool slot |
| `PATCH_ABS64` / `PATCH_ABS32S` | absolute, written directly |

## The code buffer

`runtime/jit_codebuf.h` (C) and `runtime/jitterator.h` (C++) mmap, stamp, patch
and mprotect. Two things worth knowing:

- **The buffer never moves.** It reserves `JCB_RESERVE` (64 MB of address space,
  pages on demand) up front. Stamped code holds displacements computed from
  `base`, so a grow-by-copy would leave every one of them pointing at where the
  buffer used to be.
- **Failure is sticky.** An emit that does not fit, or a displacement out of
  rel32 range, marks the buffer; `jcb_finalize` then returns `NULL` instead of
  handing back code that is not what was stamped. Check it.

## Tests

`ctest -R jitterator_tests` — the extractor's own, which build real objects with
clang and run the real binary, because how clang's output is read is the thing
under test. They cover the shape it is built for, each refusal above, and
truncated and byte-corrupted objects (every 1/16th prefix; bytes across the
header and section table), which must be diagnosed rather than crashed.

`ctest -R calc_vm` — the calculator end to end, tier 0 (threaded interpreter) and
tier 1 (this JIT), plus a falsifier build with the rewrite pass compiled out.

## Where the field has moved

The 2021 paper is the floor, not the ceiling. Worth knowing before extending this:

- **[Deegen](https://arxiv.org/abs/2411.11469)** (Xu & Kjolstad, OOPSLA 2026)
  extracts stencils from **LLVM IR** instead of from a linked object, which lets
  it see a compiler-invented constant and hoist it rather than discovering it as a
  relocation and refusing it — and buys hot/cold splitting and inline-cache
  lowering besides.

  **jitterator does not want that, and the reason is the point of the tool.**
  Reading the object file is what keeps this an afternoon's worth of code with no
  dependency: an ELF reader generated from `extract/elf64.bbq` by BBQ's own
  backend, a symbol table, a relocation table, a header. Taking the IR route means
  linking LLVM and tracking its API — a whole other system to extract stencils
  with. Within the down-and-dirty design the constant problem already has an
  answer: hoist it into a hole yourself, which is what the calc stencils do with
  `_HOLE_k41e0000000000000` and friends, and what jitterator's error message tells
  you to do. Deegen is listed because it is the state of the art, not because this
  should become it.
- **CPython's JIT** (3.13+) is copy-and-patch in production. Its engineering is
  the reference for the parts jitterator does not do: **trampolines** for targets
  out of branch range (which is what makes AArch64 viable, where the range is
  ±128 MB, not ±2 GB), and reusing one trampoline per symbol. Note that CPython
  needs them because it branches to native code; jitterator's constant-pool
  indirection means it does not — a cheaper answer to the same problem.
- **Superinstructions** — one stencil for a fused pair, so the pair's dispatch and
  its round trip through the stack both disappear. opgen has no fusion pass;
  this is the variant axis it does not have.
- **AArch64** is unimplemented. `reloc_to_patch_type` is x86-64 relocation types
  and the patch kinds are x86-64 field widths.

Two of the paper's ideas are **already here**, in opgen rather than in jitterator,
and it is worth knowing where before reaching for them again:

- **Stencil variants and register allocation** are opgen's `-tier2 N`: Ertl-style
  stack caching, where the top *N* values live in registers. They are the
  `cache_slot_t _r0.._rN` arguments in `CACHE_ARGS`, which `preserve_none` puts in
  registers, and opgen emits a variant family per opcode — `gen_st_<op>__s<K>` for
  each entry state *K*, and `__s<K>m` for the same state spilling its result
  instead of caching it. The driver tiles the bytecode by state and stamps the
  matching variant. javelina ships `TIER2_N = 8`.
- The **constant-pool indirection** for natives, described above, is what lets a
  stencil reach any address without the trampolines CPython needs.

**The calc does not exercise any of that.** It invokes opgen without `-tier2`, so
it generates 48 plain stencils and no variants at all — the variant family, the
most intricate part of the JIT, is covered only by javelina, in another repository.
Closing it means giving the calc's JIT driver the state tracking javelina's has:
choosing a variant per point and emitting the transitions between states.
