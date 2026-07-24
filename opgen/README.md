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

  Write the scalars as opgen's **own** spellings — `s1`/`u1` … `s8`/`u8`,
  `f4`/`f8`, the typedefs the generated value model emits. Widths and
  float-ness are read back off those names, so a row written `int64_t`
  is silently treated as 32 bits.

  A row whose field is `v` also emits the `v128_t` lane union and makes
  the runtime-typed carrier (`any_t`) carry both 64-bit halves — see
  *The runtime-typed carrier* below.

**`sidetable`**
: This op set has **structured control**, so opgen owns the Titzer
  side-table entry format and the §4.4.8 walk over it, and emits both:

  ```c
  typedef struct { s4 delta_ip; s4 delta_stp; u2 vals; u2 pop; } opgen_st_entry_t;
  static inline void opgen_do_transfer(frame_t* f);
  ```

  A taken branch keeps the top `vals` operands, drops `pop` beneath
  them, then advances the code cursor and the side-table pointer by
  their deltas. The backend supplies only *where* the three cursors
  live, through `OPGEN_ST_TABLE` / `OPGEN_ST_PTR` / `OPGEN_IP` (the
  same overridable-accessor seam as `LOCAL_SLOT`), and calls
  `opgen_do_transfer()` from its own transfer built-in — which should
  return `status`, so both tiers route through their control path (the
  JIT stencil bakes `_HOLE_ip`, calls, and resyncs to the new pc; a
  `void` transfer would move the cursor and then fall into the *next*
  stencil).

  Declared rather than inferred from a built-in's name: opgen bakes in
  no target identifiers, and a substrate with no side table must not be
  handed a walk that reads frame fields it does not have. The cursor
  accessors are emitted either way, because a body may name `stp`
  without asking for the walk.

**`native <ret> name(<params>);`**
: A built-in (ddcgc's *Aux*): signature in the spec, C body in the
  runtime, called by name from opcode bodies. `native status` ones are
  the control transfers (branch, call, return).

  Parameter and return types are the action language's:
  `byte`/`short`/`char`/`int`/`uint`/`long`/`ulong`/`float`/`double`/
  `boolean`/`ref`/`any`/`v128`/`void`/`status`.

**`inline native <ret> name(<params>);`**
: The same, but lowered to a **direct** call in both tiers rather than
  a `_HOLE_` patch point, so a backend `static inline` folds into the
  handler *and* the JIT stencil. No extern prototype, no jit-symbol
  entry — and no hole, which is why an inline native is invisible to
  the stencil's extern table.

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

  A **`brtable`** operand is a variable-length tail (a `vec(labelidx)`).
  Its label *targets* live in the side table, so the vector itself is
  pure decode and opgen owns it: name `<operand>_count` in the body and
  opgen reads the vector's length into it (a baked hole in the stencil,
  a ULEB read in the interpreter) and skips the `count+1` labels. A body
  that never names the count gets neither — an unread immediate would
  be a wasted patch point.
- **stack effect** `( a, b -- r )` names the inputs popped and outputs
  pushed; the body reads the inputs and assigns the outputs by name.
  Three forms carry extra information:

  `addr(<is64-expr>) name`
  : An **addrtype**-width integer (WASM §2.3.11) whose width is the one
    *declared* by the module entry it addresses — `addr(mem_is64(m)) a`
    — and not the operand's runtime tag. Every typing rule reads the
    width from the module, and §4.6.8 executes it as an assertion
    ("Due to validation … Pop the value (`at.const i`)"), so naming the
    source here is what lets the generated pop read the same fact the
    validator read. There is no default: a bare `addr` is a hard error,
    because falling back to the tag is exactly the divergence this form
    exists to remove — a tested fact can disagree with a declared type,
    an asserted one cannot.

  `[<count-expr>] ty name` on the **input** side
  : §3(b) dynamic arity: pop `count-expr` operands. They are *not*
    popped before the body — they stay on the value stack, where they
    remain GC roots, and the body reads the i-th in place as `name[i]`
    (a runtime-typed `any_t`). opgen drops `sp` afterwards. `flag:
    pops_first` moves that drop to *before* the body, which the call
    family needs because the callee frame is carved at `sp`; it is only
    sound when nothing in between allocates, since `sp` bounds the root
    scan.

  `[<count-expr>] ty name` on the **output** side
  : A variadic *result*: the values are already at the frame top (a
    callee left them there), so exposing them IS the marshaling. opgen
    raises `sp` and pushes nothing — the name denotes a count, not a
    value, so there is nothing to declare. Only sound when the body is
    not terminal; a `status` native tail-returns and would strand it.

  `word` / `any`
  : `word` is a whole **slot** — used when the result's width is decided
    at run time. Assigning another slot to it (`result = c ? v1 : v2`)
    is a slot move; assigning a *scalar* goes through the value model's
    field selector, and the body sets the tag itself via `<name>_wt`.
    `any` is the runtime-typed carrier; see below.
- **annotations**: `error: (. cond .) -> Name` (a guarded fault),
  `flag: name` (an opcode flag bit), `local_value: int` (declares the
  body touches the `locals[]` array with the given element kind).

  A guard's condition is an expression in the same action language as a
  body, and it is emitted *verbatim* — opgen never infers a guard from
  the body's shape, so any condition the language can state is a
  guard. Guards emit in declaration order, ahead of the body, into
  **both** tiers, so the interpreter and the JIT stencil trap
  identically. An input named only by a condition is still popped into
  a variable. A condition is lowered at the type of the first stack
  input or operand it names (i32 if it names none), which is what lets
  one guard on a `family` cover every member width.

  `Name` becomes `OPGEN_ERR_<Name>` in the generated `opgen_err_t`
  enum. A fired guard passes its code to `OPGEN_GUARD_TRAP(vm, err)`
  and then tails to the trap continuation; the generated default
  discards the code, so a backend with a single undifferentiated trap
  needs nothing, and one that reports distinct causes overrides the
  macro. `error: Name` with no condition declares a name without a
  guard.

  A **float constant inside a condition** gets a synthesized data hole,
  `_HOLE_k<bit pattern>`, named by its value. Left as a C literal it
  would be materialized out of clang's `.rodata` constant pool, and a
  copy-and-patch stencil cannot carry that relocation — the JIT would
  bake 0 and the guard would silently stop firing *in JIT code only*.
  (A constant in a *body* already has a name: a literal-init local.)
  A negated literal folds into one constant rather than a negation, so
  it stays off the sign-mask path; a condition that negates a float at
  run time gets `_HOLE_fsignmask32`/`64` instead, sized from the
  condition's type rather than the op's result type.

A **`family ( T a -- T r ) { i32 iadd 0x60  i64 ladd 0x61  … }`** block
expands at parse time into one ordinary opcode per member, sharing a
type-agnostic body — for instruction sets with parallel typed variants.

### The runtime-typed carrier

A value whose type is known only at run time crosses opgen's boundaries
as an `any_t`:

```c
typedef struct { s8 bits; s8 hi; u1 kind; } any_t;
```

`hi` is the second 64-bit half, live only for a `v128` (compound
literals zero it otherwise). It exists because the carrier is *wider
than one 64-bit payload* whenever the spec declares a `v128` row:
without it a v128 crossing the carrier — a struct/array field, a
variadic operand read in place, a `pop` — kept its low lane and
silently lost lanes 2 and 3.

The slot-union member the halves are read through comes from the
spec's own type table, never a hardcoded name, and a spec with no
`v128` row gets the single-word form.

### Hard errors

A body opgen cannot lower is a **hard error**, never a silent drop: a
call to an undeclared built-in, an index into anything but
`locals[]`/`code[]`, a slot store from a non-slot source, an
unsupported assignment target, an expression statement that is not a
call, an `addr` operand with no declared source, and more than eight
constant holes on one opcode. Each of these used to print a diagnostic
and emit nothing, which produced a handler that compiled and quietly
did the wrong thing — and, for a dropped hole, a stencil that still
names the hole and gets 0 patched into it.

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

Intrinsics available as calls: `sqrt`, `fabs`, `ceil`, `floor`,
`ftrunc`, `nearest`, `fmin`, `fmax`, `copysign`, `clz`, `ctz`,
`popcnt`, `rotl`, `rotr`, `reinterpret`, `push`, `pop`, and
`int_min()`. `int_min()` is nullary and takes its width from the
surrounding expression's type, so an overflow guard written once —
`error: (. b == -1 && a == int_min() .) -> Overflow` — holds for
every member of a `family`.

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

Running off the end of a function stashes nothing: the results ARE the
top of the frame stack, where the caller reads them.

## EXAMPLE

`jitterator/example/grammar/calc.def` is the worked reference — a small
stack machine ("WASM lite") whose control transfers (`calc_br`,
`calc_ret`, `calc_call`, `transfer`) are `native status` built-ins:

```
backend "calc_backend.h"
status calc_status_t CALC_OK CALC_ERR CALC_HALT [CALC_TRAP]
sidetable

type i32  s4     u4     1 i
type i64  s8     u8     1 l
type f64  f8     f8     1 d
type v128 v128_t v128_t 1 v

native status calc_br(int off);
native status transfer();
native int    calc_mem_is64(int m);

const 0x01 [ sleb128 v ] ( -- i32 r )   (. r = v; .)
add   0x02 (i32 a, i32 b -- i32 r)      (. r = a + b; .)
load  0x0C [ u8 idx ] ( -- i32 r )
    local_value: int
    (. r = locals[idx]; .)
br_if 0x0E [ sleb128 off ] (i32 c -- )  (. if (c != 0) calc_br(off); .)

# §3(b) dynamic arity + pops_first: opgen owns the arg marshaling.
call  0x13 [ sleb128 entry, u8 nargs ] ( [nargs] i32 args -- )
    flag: pops_first
    (. calc_call(entry, nargs); .)

# A guard whose float constants ride named holes into the stencil.
trunc 0x1A (f64 a -- i32 r)
    error: (. !(a >= -2147483648.0 && a < 2147483648.0) .) -> TruncOverflow
    (. r = (int)a; .)

# The pop width comes from the addressed memory's DECLARED addrtype.
load32 0x23 [ u8 m ] (addr(calc_mem_is64(m)) a -- i32 r)
    (. r = calc_load32(m, a); .)

# The label targets live in the side table; the vector is pure decode.
br_table 0x28 [ brtable labels ] (i32 key -- )
    (. stp = stp + (((uint)key > labels_count) ? labels_count : (uint)key); transfer(); .)
```

`opgen -i calc.def -o <gen>` produces the calc's two tiers; the burgc
matcher (`calc.burg`) lowers the ddcg continuation graph to this
bytecode, and `calc_vm_e2e` / `calc_vm_jit` run it — arithmetic,
locals, branches, recursive calls, guards, wider values, the
runtime-typed carrier, linear memory, variadic operands and results,
and structured control — every case on **both** tiers, proving
`interp == jit`. The calc is deliberately maximalist for this reason:
each capability is there to hold a generator feature down end to end.

## SEE ALSO

**pegc**(1), **asdl**(1), **ddcgc**(1), **burgc**(1) — the upstream
pipeline stages. opgen consumes the op stream that a `.burg` matcher
emits.
