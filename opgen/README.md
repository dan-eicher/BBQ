```
OPGEN(1)                  Opcode VM Generator                 OPGEN(1)
```

## NAME

**opgen** -- opcode-spec compiler: one `.def` to a two-tier bytecode VM

## SYNOPSIS

```
opgen -i opcodes.def -o out_dir
```

## DESCRIPTION

opgen compiles a single `.def` opcode specification into a complete
bytecode VM back end: a threaded-dispatch **interpreter** and a
copy-and-patch **JIT**, plus the opcode tables, type/slot tables, and
runtime headers that tie them together. Both execution tiers are
generated from the *same* per-opcode semantic body, so the interpreter
and the JIT run identical opcode semantics by construction — there is
no second hand-written copy to drift.

A `.def` declares an opcode set the way an instruction-set manual would:
each entry gives a mnemonic, its byte encoding (opcode value, optional
prefixed sub-opcode, and immediate operands), its stack effect
(`in -- out`), and a straight-line semantic body written in a small
Java-subset action language. opgen derives everything mechanical from
that — operand decode, stack push/pop, the dispatch table, JIT data
holes — so the spec carries semantics, not boilerplate.

opgen bakes in **no target identifiers**. The control-flow vocabulary
(`status` ok/err/halt), the value/slot model (`type` rows), and the VM
substrate (`backend` header supplying `frame_t`/`vm_t`/`heap_t`) are all
spec-declared, which is what lets one generator serve a 16-bit-slot
JCVM, a 32-bit-word WASM VM, and a toy stack calculator without code
changes.

opgen sits at the bottom of BBQ's canonical pipeline
(parser → AST → ddcgc → IR → burgc → **op stream → opgen VM**): burgc
lowers a continuation graph to this op set's bytecode (`emit_op`), and
opgen is what executes that bytecode. It is the embedded-C "ship a VM +
bytecode-as-data" leaf of that pipeline.

opgen is itself a BBQ-citizen tool: its `.def` front end is a
pegc-generated C++ parser (`grammar/opgen.peg` → `frontend/`) over an
ASDL-defined AST (`grammar/opgen.asdl`), and its emitters are C++.

## OPTIONS

**-i** *opcodes.def*
: Input opcode specification. Required.

**-o** *out_dir*
: Output directory for the generated VM. Required. opgen writes the
  full set of files listed under GENERATED OUTPUTS into it.

## THE `.def` LANGUAGE

A spec is an optional leading C-header block, a set of directives, the
opcode entries, and an optional verbatim trailer.

### Header block

```
(. #include "bbq_runtime.h" .)
```

Emitted verbatim near the top of the generated translation units —
where the operand decoders and substrate types come from.

### Directives

**`backend "header.h"`**
: The VM substrate. opgen `#include`s it into `runtime_api.h` at the
  value-model's substrate point, so a real VM supplies its own
  `frame_t`/`vm_t`/`heap_t` from one place. Absent → a default
  self-test frame (used by opgen's own tests). This replaces the older
  per-translation-unit `-D…_BACKEND_TYPES` macro — declare it once in
  the spec, not at every compile.

**`status <c-type> <OK> <ERR> <HALT>`**
: The control-flow status type and its three outcomes. A `native`
  method returning `status` is thereby a control transfer: the handler
  runs it, then dispatches on the outcome (continue / error / halt).

**`type <value-type> <c-scalar> <c-unsigned> <slots> <field>`**
: One row of the value/slot table — the C scalar a value type lowers
  to, its unsigned form, how many machine slots it occupies, and the
  union field name. This is the seam that swaps the slot model between
  targets.

**`native <ret> name(<params>);`**
: A built-in (ddcgc's *Aux*): signature in the spec, C body in the
  runtime, called by name from opcode bodies. `native status` ones are
  the control transfers (branch, call, return).

**`<ret> name(<params>) { … }`**
: A helper (ddcgc's *Fun*): same calling convention, but its body is
  written in the action language and shared across opcodes.

### Opcode entries

```
mnemonic 0xNN [subop] [ <operands> ] ( <stack_in> -- <stack_out> )
    <annotations>
    (. <semantic body> .)
```

- **opcode value** `0xNN`, with an optional second integer for a
  prefixed sub-opcode (`0xFC <subop>`); absent → a one-byte op.
- **operands** in `[ … ]` are *immediates* decoded from the bytecode
  stream: `sleb128`/`uleb128`/`u8`/`f32`/`memarg`/…. The JIT bakes only
  operands that have a data-hole kind (the leb/u8/float forms); use
  those rather than fixed-width `i32` immediates, which lower to a
  runtime `FETCH` with no hole.
- **stack effect** `( a, b -- r )` names the inputs popped and outputs
  pushed; the body reads the inputs and assigns the outputs by name.
- **annotations**: `error: "cond" -> handler` (a guarded fault),
  `flag: name` (an opcode flag bit), `local_value: int` (declares the
  body touches the `locals[]` array with the given element kind).

A **`family ( T a -- T r ) { i32 iadd 0x60  i64 ladd 0x61  … }`** block
expands at parse time into one ordinary opcode per member, sharing a
type-agnostic body — for instruction sets with parallel typed variants.

### Semantic action language

Opcode and helper bodies are straight-line statements over the full
Java expression language: locals, `if`/`while`/`for`, assignment,
arithmetic/logical/shift/relational operators, casts to primitive
types, array indexing, and calls to natives/helpers by name. A bare
`trap;` is the explicit fault statement. Object/field/array effects go
through built-in calls (`get_field(o,ref)`, `array_load(a,i)`, …), never
dotted access. Loops-as-control-flow inside the dispatch loop, `new`,
`this`, `++`/`--`, and compound assignment are deliberately excluded.
A body that does not parse is a hard error — there is no raw-C escape.

### Verbatim trailer

```
%%
… C pasted verbatim at the end of opcode_tables.c …
```

## GENERATED OUTPUTS

opgen writes a fixed set of files into *out_dir*:

| file | contents |
|---|---|
| `opcodes.h` | the `OP_*` mnemonic → value enum |
| `opcode_tables.c` | dispatch/metadata tables (+ the `%%` trailer) |
| `runtime_api.h` | value model + the `backend` substrate include |
| `gen_interp.h` / `gen_interp.c` | the threaded-dispatch interpreter |
| `wasm_stencils.c` | the copy-and-patch JIT stencils + machinery stencils |
| `wasm_jit_meta.h` | per-opcode JIT operand/hole metadata |
| `wasm_jit_symbols.h` | `_HOLE_<native>` → address symbol table |
| `wasm_valtype.h` / `wasm_type_meta.h` | the value-type / slot tables |

The interpreter tier is a musttail threaded loop: `wasm_next` reads the
next opcode through the substrate's code cursor and tail-calls the
handler. The JIT tier stamps each opcode's stencil, backpatches the
data holes (immediates, continuation/trap addresses), and runs the
resulting machine code. Because both are emitted from one body, the
tiers agree.

## EXAMPLE

`jitterator/example/grammar/calc.def` is the worked reference — a small
stack machine ("WASM lite") whose control transfers (`calc_br`,
`calc_ret`, `calc_call`) are `native status` built-ins:

```
backend "calc_backend.h"
status calc_status_t CALC_OK CALC_ERR CALC_HALT
type   i32 int32_t uint32_t 1 i

native status calc_br(int off);
native status calc_call(int entry, int nargs);

const 0x01 [ sleb128 v ] ( -- i32 r )   (. r = v; .)
add   0x02 (i32 a, i32 b -- i32 r)      (. r = a + b; .)
load  0x0C [ u8 idx ] ( -- i32 r )
    local_value: int
    (. r = locals[idx]; .)
br_if 0x0E [ sleb128 off ] (i32 c -- )  (. if (c != 0) calc_br(off); .)
call  0x13 [ sleb128 entry, u8 nargs ] ( -- )
    (. calc_call(entry, nargs); .)
```

`opgen -i calc.def -o <gen>` produces the calc's two tiers; the burgc
matcher (`calc.burg`) lowers the ddcg continuation graph to this
bytecode, and `calc_vm_e2e` / `calc_vm_jit` run real parsed programs —
arithmetic, locals, branches, and recursive function calls — proving
`interp == jit`.

## SEE ALSO

**pegc**(1), **asdl**(1), **ddcgc**(1), **burgc**(1) — the upstream
pipeline stages. opgen consumes the op stream that a `.burg` matcher
emits.
