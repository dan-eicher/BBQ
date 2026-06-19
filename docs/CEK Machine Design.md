# CEK Machine Design for BBQ

The interpretive backend for BBQ. Compiles format specifications into typed
continuation graphs and executes them against byte buffers, producing zero-
copy capture metadata.

The architecture is the canonical CEK machine of Felleisen & Friedman 1987 —
state transitions `(C, E, K) → (C', E', K')` driven by syntactic
decomposition of the current continuation, plus a typed accumulator register
holding values in flight. Expression evaluation is defunctionalized per
Reynolds 1972 and Ager-Biernacki-Danvy-Midtgaard 2003: every continuation
that would sit on the host stack in a recursive interpreter is an explicit
data constructor on the runtime K stack.

This document describes what the CEK backend does today. Variant catalogs
and rule bodies live in their generator input files (referenced in §4 and
§5); this document sits above them, naming categories and the architectural
discipline they realize.

---

## 1. Architecture Overview

The CEK machine is the second BBQ backend — an interpreter that complements
the static C++ code generator. Both backends share the same frontend
(Scanner, Parser, Sema); the split happens at the validated AST.

```
                        ┌─────────────────┐
  format.bbq ──────────▶│  BBQ Frontend    │
                        │  Parser          │
                        └────────┬─────────┘
                                 │ BBQ AST
                        ┌────────▼─────────┐
                        │ Semantic Analysis │
                        │ - type checking   │
                        │ - constraint scope│
                        │ - dependency order│
                        └────────┬─────────┘
                                 │ Validated, topo-sorted AST
                   ┌─────────────┴─────────────────┐
                   │                                │
          ┌────────▼────────┐             ┌─────────▼─────────┐
          │ Render Backend   │             │ CEK Backend        │
          │ lower → JSON     │             │ DDCG Compiler      │
          │ → inja → C/C++   │             │ AST → Kont Graph   │
          │   (zero-copy)    │             └─────────┬──────────┘
          └──────────────────┘                       │
                                            Immutable continuation graph
                                                     │
                                            ┌────────▼─────────┐
                                            │ CEK Interpreter   │
                                            │ Kont Graph × Input│
                                            │ → Capture Metadata│
                                            └──────────────────┘
```

**Input.** Validated, topologically sorted BBQ AST + byte buffer.

**Output.** `CaptureMetadata` — a hierarchical zero-copy file index recording
`[start, end)` offsets into the original buffer plus typed computed values.
No bytes are copied; the metadata IS the parse result.

**Toolchain at a glance.** The compiler and IR are themselves generated from
declarative inputs:

- `backends/cek/bbq_ir.asdl` declares the IR variants — typed value sum
  plus static/dynamic kont sums under a shared root. The `asdl` tool
  generates the C++ headers via `templates/bbq_ir.inja`.
- `backends/cek/bbq.ddcg` declares the AST→IR compilation rules using
  Destination-Driven Code Generation. The `ddcgc` tool, with the
  `dybvig.ddcg` primitive library, generates the `bbq::Compiler` class.
- `backends/cek/Machine.cpp`, `Frame.h`, `Capture.h`, `Environment.h`,
  `ParseArena.h`, `StringPool.h` are hand-written runtime: the trampoline,
  the failure path, the capture builder, error tracking, external parser
  dispatch.

### Why both backends?

The static backend generates C++ source — maximum performance, zero runtime
dependency, but requires a C++ compiler. The CEK backend fills complementary
roles:

- **Interactive exploration** — Load a `.bbq` spec and parse files
  immediately, without a C++ compilation step.
- **Hot-reload** — Recompile the continuation graph when the spec changes,
  without restarting.
- **Embedding** — Ship a single library that accepts BBQ specs at runtime.
- **Round-trip testing** — Validate that both backends produce identical
  parse results for the same input.

---

## 2. Memory Management

Three-tier model.

### Tier 1: Immutable Compiled Grammar (Reference-Counted)

```cpp
struct CompiledGrammar {
    std::atomic<int> refcount;

    // Entry point for each rule, indexed by rule name
    std::unordered_map<std::string, KontNode*> entries;

    // Interned strings (field names, rule names, error messages)
    StringPool strings;

    // External parser + function registries
    ExternalParserTable ext_parsers;
    FunctionTable ext_functions;

    // Arena owning all compile-time-emitted (static_kont) nodes
    Arena graph_arena;
};
```

Allocated once during compilation. Shared across concurrent parses (thread-
safe because immutable). Reference-counted so grammars can be swapped at
runtime without dangling pointers.

### Tier 2: Per-Parse Arena (Bump Allocator)

All mutable per-parse state lives here: environments, capture nodes, frame
stack entries, runtime-allocated (`dynamic_kont`) nodes, in-flight typed
Values. Reset in O(1) after each parse — a single pointer assignment.
No fragmentation, no GC overhead.

**Sizing.** AiPL uses 64KB for ephemeral expression evaluation; binary
parsing workloads are heavier (deeper nesting, larger capture trees).
Default to **256KB**, growing on demand.

**Why no GC?** All per-parse allocations are provably short-lived — no
long-lived closures, no user-defined functions, no cyclic structures. Arena
reset is sufficient.

### Tier 3: Runtime K Stack and Typed Accumulator

The third tier is the literal **K** of the CEK machine — the runtime
continuation stack — paired with a single typed accumulator register.

```cpp
struct CEKMachine {
    // …
    KontNode* control;                    // C — currently dispatched kont
    std::vector<KontNode*> kont_stack;   // K — runtime continuation stack
    Value* result;                        // ac — typed accumulator
};
```

**The K stack** is a LIFO stack of `KontNode*` pointers. Each iteration
the trampoline loads `control` from K's top, pops, and invokes
`control->invoke(this)`. The kont's `invoke()` may push further konts
(in reverse execution order — the next-to-run is last pushed). Control
is overwritten each iteration from K; it is never advanced via
`control->next`. The pointers live on the stack-vector spine; the konts
they point to live in the Tier 1 grammar arena (for `static_kont`
variants emitted at compile time) or the Tier 2 per-parse arena (for
`dynamic_kont` variants allocated mid-parse).

**The accumulator `ac`** holds a `Value*` — a pointer to a typed,
arena-allocated value. Producer konts (literals, refs, the apply variants
of operators) allocate a fresh `Value` and write the pointer to `ac`.
Consumer konts read `ac` and dispatch on the value's type. This is the
typed accumulator of the canonical CEK machine; there is no separate
evaluation stack.

**Backtracking** saves the K stack size and the environment pointer.
Restoration truncates K back to the saved size (dropping any konts pushed
during the speculative branch) and resets the env pointer (dropping any
bindings extended during the branch). Speculative arena allocations
(Values, dynamic konts) are not freed — the per-parse arena is monotonic
within a parse and reset only at parse end. This is sound because nothing
restored points at speculative allocations.

---

## 3. State Model

### 3.1 The CEK machine

The state has four components — the literal C/E/K of Felleisen-Friedman
1987 plus the typed accumulator:

- **C (control)** — the kont currently being dispatched. A private
  field on the machine with `friend class CEKMachine` access only:
  the trampoline (`execute_from()`) is the sole writer. Loaded from
  K's top each iteration. Never assigned inside any kont's `invoke()`
  body, never advanced via `control->next` — both are compile errors
  by construction.
- **E (environment)** — the binding/record channel. Immutable linked list
  of `Binding` records. Persists for the lifetime of the parse.
- **K (kontinuation)** — the runtime continuation stack `kont_stack`.
  Real LIFO stack of pending konts.
- **ac (accumulator)** — the typed register `result`. Single `Value*` slot
  for in-flight expression values.

The frame stack (`frame_top`) is a separate auxiliary structure carrying
saved-state for call/return, alternative branches, and loop iterations. It
is consulted by the failure path; the trampoline does not pop from it.

### 3.2 The value channel and the binding channel

A defunctionalized CEK machine for BBQ has two distinct channels for data:

- **The value channel — `ac`** is the workspace where in-flight
  expression values live. Producer konts write a typed Value pointer to ac;
  consumer konts read it.
- **The binding channel — `E`** is the persistent name→value store.
  Bindings hold typed Value pointers and capture intervals. Once written,
  bindings are immutable — extending E creates a new head.

The handoff between ac and E happens at exactly two kont families:

- **`Ref`** reads E → copies the binding's `Value*` into ac. **`PathStart`**
  reads the capture builder → wraps the capture pointer in a fresh
  `FieldCaptureValue` and writes that to ac. Both are gateways from E into
  expression evaluation.
- **`BindComputeStore`** reads ac → extends E with a new binding holding
  that Value pointer + records the value in the capture builder. This is
  how a freshly-computed typed value commits to a name.

Every other kont stays on one side: structural konts (`BeginStruct`,
`BeginArray`, etc.) only touch E and the capture builder; expression konts
only touch ac.

This split is the canonical CEK shape. ac handles workspace; E handles
bindings; a third channel (an evaluation stack) would be neither, which is
why none exists.

### 3.3 The eval-and-apply discipline

Expression evaluation is defunctionalized per Ager-Biernacki-Danvy-
Midtgaard 2003 §3. Every recursive call in a tree-walker becomes a kont
allocation; every "apply this operator after both operands evaluate"
becomes an apply kont on K.

For a binary operator `Add(left, right)`, the operator-shape kont
(`AddKont`, emitted by the compiler) on invoke:

1. Allocates a fresh `ApplyAddKont` (carries a slot for the saved right
   value).
2. Allocates a fresh `EvalLeftKont` (will fire after right evaluates,
   moves ac into the apply's slot, then pushes left).
3. Pushes onto K in reverse execution order: apply, eval-left, right.

Right evaluates first (it sits on top), leaves its Value in ac; eval-left
fires, copies ac into apply's saved slot, pushes left; left evaluates,
leaves its Value in ac; apply fires, dispatches on the (left, right) type
pair, produces the typed result Value, writes it to ac.

Every binop, unary, ternary, call, and path-navigation step instantiates
this discipline. Logical and/or short-circuit via a check kont rather than
an apply (the right operand is conditionally pushed). The seven structural
sites that consume an expression value (constraint, switch discriminator,
bind compute, array count, array until, bytes length, endian switch) each
have a dedicated runtime "consumer" kont that reads ac after the
expression IR finishes.

Apply konts are **allocated fresh per evaluation**. There is no shared
mutable slot between evaluations of the same expression — re-evaluating
an array's per-iteration count expression simply re-pushes the IR
sub-tree onto K, which freshly allocates apply konts each time.

### 3.4 Hand-written runtime types

The IR variants are generated (§4). The remaining runtime types are
hand-written; their C++ shape is the spec.

**Environment.** Immutable linked list of bindings.

```cpp
struct Binding {
    const char* name;       // Interned string
    Value* value;            // Typed value pointer
    size_t start_offset;    // Capture interval start
    size_t end_offset;      // Capture interval end
    BindingSource source;
};

struct Environment {
    Binding binding;
    Environment* parent;
};
```

Lookup walks the parent chain via interned-pointer comparison.

**Frame stack.** Three frame types — `ReturnFrame`, `Savepoint`, `LoopFrame`
— cover all saved-state needs. See `backends/cek/Frame.h` for exact field
layouts. Their roles:

- **`ReturnFrame`** is pushed by `InvokeRule` and popped by `ReturnRule`.
  Carries the caller's return-to kont, saved environment, the start
  position, and a capture mark. Cross-rule failure walks past these
  transparently.

- **`Savepoint`** is pushed by `BeginChoice`, `BeginOptional`, and (in
  resync mode) `BeginArray` for backtrack scopes. Captures `pos`, `env`,
  `little_endian`, the K-stack depth, and a capture mark. Carries one
  additional pointer — `on_fail_target` — that names the static
  `OnFailKont` to dispatch on failure (§7.2). `CommitChoice` /
  `EndOptional` / `EndArray` pop it on success.

- **`LoopFrame`** is pushed by `BeginArray` for every array. Holds the
  iteration counter, the limit (or −1 for EOF/until-bounded arrays), the
  array's `EndArrayNode`, and the outer environment to restore when the
  array ends. In resync mode the per-iteration `Savepoint` sits on top of
  the `LoopFrame`; in standard mode there is no `Savepoint` for the
  array.

**Capture metadata.** The output structure.

```cpp
enum class CaptureType : uint8_t {
    UInt8, UInt16LE, UInt16BE, UInt32LE, UInt32BE, UInt64LE, UInt64BE,
    Int8,  Int16LE,  Int16BE,  Int32LE,  Int32BE,  Int64LE,  Int64BE,
    Float32LE, Float32BE, Float64LE, Float64BE,
    Bool, Bytes, String,
    Struct, Array, Computed, External
};

struct FieldCapture {
    const char* name;           // Interned
    size_t start_offset;
    size_t end_offset;
    CaptureType type;
    FieldCapture* children;     // Nested captures (Struct, Array)
    int child_count;
    Value* computed_value;      // Typed value for Computed fields
};

struct CaptureMetadata {
    bool success;
    size_t bytes_consumed;
    FieldCapture* root;
    const char* error_message;
    size_t error_offset;
};
```

The capture builder accumulates this tree during parsing and supports
mark/restore for backtracking.

**String interning.** All identifier strings (field names, rule names) are
interned in `CompiledGrammar::strings`. Identifier comparison is interned-
pointer equality.

---

## 4. The IR Layer

### 4.1 Why the IR is generated

`backends/cek/bbq_ir.asdl` declares the typed value sum and the kont
variant catalog. The `asdl` tool generates the C++ headers via the
`bbq_ir.inja` template. Single source of truth for: runtime `invoke()`
dispatch, ddcg-generated emit functions, future tooling.

The design content here is what categories of variants exist, why the
shape is what it is, and which architectural roles each variant family
plays. The catalog itself is in the .asdl.

### 4.2 Variant categories

**`static_kont`** — variants the compiler emits at compile time. Live in
the grammar arena, immutable across parses.

- **Structural** — struct/array/choice/optional/switch/bitfield/extern/
  invoke-rule/return-rule/halt and the transition konts between them.
- **Failure-handler chains** — `OnFailKont` links built at compile time
  by `compile_alternatives` / `compile_optional`. Each link is reachable
  via two paths that converge on the same value: as a runtime
  `Savepoint::on_fail_target` (set at BeginChoice/BeginOptional push,
  mutated through the chain as arms try-and-fail) and as a static
  `in_alt::next_arm` / `in_optional::skip` in ρ (so where-checks can
  dispatch directly via on_false without a frame walk). See §7.2.
- **Cross-rule failure** — `AbortRuleKont` is the on_false target when
  a where-check at a rule's top level has no in-rule backtrack scope.
  It walks `ReturnFrame`s for the nearest `rule_fail_target` (baked at
  the matching InvokeRule emit site via `find_backtrack_target` over
  the caller's ρ) and dispatches there.
- **Resync recovery** — `ResyncKont` is the static failure handler for
  per-iteration savepoints inside `array<T> ... resync` (§7.4). It's
  carried both as `Savepoint::on_fail_target` (runtime byte-fail
  dispatch) and as `in_array_elem::skip` in ρ (compile-time on_false
  resolution).
- **Expression leaves** — integer/float/string/bool literals; refs;
  position queries (`pos`, `remaining`, `at_end`, `eoi`); the loop
  counter `i`.
- **Expression operators** — unary, binary arithmetic/bitwise/comparison,
  logical and/or, ternary, function call, path-nav steps (field access,
  index access, capture-value).

**`dynamic_kont`** — variants the interpreter allocates when a static
kont's `invoke()` fires. Live in the per-parse arena, freed at parse end.

- **Apply variants** paired 1:1 with operator variants. Each apply carries
  the slot eval-left writes the saved right operand into, and dispatches
  on the (left, right) type pair to produce the result.
- **Site-consumer variants** for the structural sites that take an
  expression: `EvalConstraintCheck`, `SwitchSelect`, `BindComputeStore`,
  `BeginArrayWithLimit`, `ArrayCountCheck`, `ArrayUntilCheck`,
  `MatchBytesApply`, `SetEndianApply`.

### 4.3 Why two sums under shared root

The split reflects a real lifetime/memory-tier distinction: static konts
live in the grammar arena (refcounted, immutable, shared across parses);
dynamic konts live in the per-parse arena (ephemeral, freed at parse end).
Different lifetimes, different concerns.

Both sums inherit from a shared `KontNode` root. The trampoline dispatches
`KontNode::invoke` virtually and only sees the shared root, so the split
costs nothing at runtime.

### 4.4 The Value sum

`Value` is a separate sum, not under `KontNode`. Values are not
continuations — they don't go on K, they have no `invoke()`, they're tag-
dispatched data records allocated from the per-parse arena.

| Variant | Carries | Role |
|---|---|---|
| `IntValue` | `int64_t` | uN/iN fields, int literals, count/length/discriminator/index results |
| `FloatValue` | `double` | f32/f64 fields, float literals, float arithmetic |
| `StringValue` | interned pointer | string fields, string literals (equality via pointer) |
| `BoolValue` | `bool` | comparison results, constraint outcomes, true/false literals |
| `FieldCaptureValue` | `FieldCapture*` | path-nav intermediate; converted to a typed scalar by `CaptureValueApplyKont` before leaving the path sub-chain |

`ac` holds `Value*`. Env bindings hold `Value*`. FieldCapture's
`computed_value` is `Value*`. The same Value type flows everywhere typed
data lives.

### 4.5 Reference

Variant catalog: `backends/cek/bbq_ir.asdl`.

---

## 5. The Compilation Pass

### 5.1 Why DDCG

The AST→IR compiler is written as a Destination-Driven Code Generation
rule set (Dybvig-Hieb-Butler 1990) compiled by `ddcgc`, rather than as
hand-written C++. The discipline forces paper-strict semantics: every
rule is a destination-driven transformation parameterized by where the
value goes, where control goes, and what loop-break label is in scope.
Hand-written compilers accumulate weasel structure (ad-hoc state
threading, mid-rule program-exit invention, γ=ret/pair fallbacks);
the .ddcg shape excludes those by construction.

### 5.2 δ, γ, ρ in BBQ

DDCG parameterizes each rule by three destinations:

- **δ (delta)** — where the value goes. The full lattice is
  `effect | ac | stack | local(i)`. BBQ uses **only `effect` and `ac`**:
  `effect` for spine work that produces no value, `ac` for expression
  rules whose value flows via the typed accumulator.
- **γ (gamma)** — where control goes. The full lattice is
  `jump(L) | pair(Lt, Lf) | ret`. BBQ uses **only `jump(L)`** at the top
  level. `pair(Lt, Lf)` short-circuits boolean expressions in test
  position — a worthwhile follow-up but excluded by default in favor of
  the explicit `EvalConstraintCheck` runtime kont. `ret` is unused.
- **ρ (rho)** — lexical scope chain consumed at compile time. BBQ's
  ρ is a recursive sum tracking the surrounding backtrackable scope:
  `root | in_field(name, parent) | in_alt(next_arm, parent) |
  in_optional(skip, parent) | in_array_elem(skip, parent)`. ρ never
  escapes into the emitted kont graph — purely a compile-time
  threading parameter — but it carries pointers into the kont graph
  so compile-time walks can resolve runtime targets statically.

The narrow δ/γ usage is what keeps BBQ rules paper-strict. The single
γ form means rules never branch on which γ they got. ρ as a recursive
sum is consumed by two compile-time walkers:

- `intern_field_or_anon(rho)` walks to the innermost `in_field` for
  capture metadata names. `in_alt` and `in_optional` are transparent
  (a where-check inside an arm still belongs to the enclosing field);
  `in_array_elem` is a name barrier (elements are anonymous).
- `find_backtrack_target(rho)` walks to the innermost backtrackable
  scope's static failure target. `in_alt.next_arm`, `in_optional.skip`,
  and `in_array_elem.skip` (when non-null) each point to an
  `OnFailKont` link that the runtime would otherwise reach via frame-
  walking. Used by `compile_primitive` to resolve a where-check's
  `on_false` and by `make_invoke_rule` to bake `caller_fail_target`
  on the InvokeRuleNode at compile time.

The yoctojc lineage: this is the same compile-time-walks-ρ pattern
yoctojc uses for `break`/`continue`/`finally` resolution. BBQ adapts
it to backtrack-on-failure scopes, which are the analog in a
parser.

### 5.3 The eval-and-apply emission pattern

For each binary-operator AST node, the rule:

1. Generates the right operand's IR with δ=ac.
2. Generates the left operand's IR with δ=ac.
3. Emits an operator variant carrying both as sub-trees.

The runtime sequences "right evaluates first, then eval-left moves ac
into apply's slot, then left, then apply" via K push order (§6).

For each of the seven structural sites that consume an expression
(constraint, switch discriminator, bind-compute expression, array count,
array until, bytes length, endian-switch expression), the structural
kont carries the expression IR sub-tree. The runtime arranges
"evaluate, then consume" by pushing the consumer kont onto K before the
expression IR.

### 5.4 The driver owns Halt wrapping

DDCG rules emit kont chains with γ=`jump(Lnext)` — they never invent
program-exit konts. The top-level driver wraps each rule's compilation
with γ=`jump(Halt)` by allocating a `HaltKont` and a `ReturnRuleKont`
chain at the top, and threading those through as the rule body's Lnext.

This rule is durable: any γ=`ret` or `pair` fallback inside a DDCG rule
body is weasel structure. If a rule needs program exit, the driver
provides it through Lnext.

### 5.5 References

Compilation rules: `backends/cek/bbq.ddcg`. Codegen primitive library
(`cg_deliver`, `cg_jump`, `cg_call`, etc.): `ddcgc/lib/dybvig.ddcg`.
Calc compiler as a worked example: `jitterator/example/grammar/calc.ddcg`.

---

## 6. Expression Evaluation

### 6.1 The eval-and-apply mechanism

Operator-shape konts produced by the compiler carry their operands as
sub-tree pointers. On invoke, they allocate the corresponding apply kont
(plus an eval-left for binops), then push onto K in reverse execution
order.

The runtime apply kont reads ac and the saved-right slot, dispatches on
the type pair, allocates a fresh result Value, and writes it to ac.

Re-evaluation is trivial — pushing the IR sub-tree onto K freshly
allocates apply konts each time. Per-iteration array count expressions
re-evaluate without compile-time chain duplication or runtime mutable
state.

### 6.2 Type semantics

BBQ expressions are typed. The type semantics are strict-by-default:

| Operation | Rule |
|---|---|
| Arithmetic (`+ - * / %`) | Same-type required; mixing Int and Float fails |
| Comparison (`==`, `!=`) | Same-type compares as expected; cross-type → `BoolValue(false)` for `==`, `BoolValue(true)` for `!=` |
| Ordered comparison (`< <= > >=`) | Same-type required; cross-type fails |
| Logical (`&& || !`) | Operands must be Bool; coerce IntValue→bool (nonzero=true); other types fail |
| Constraint check | Bool or coerce-from-int; other types fail |
| Switch dispatch | Int discriminator → IntValue cases; string discriminator → StringValue (interned-pointer equality) |
| Array count / length / index | IntValue required |
| Endian switch | Bool or coerce-from-int |

Type errors call `m->fail("type error: …")` — fail-loud, no silent
coercion or zero-fallback. Type checking happens at value boundaries
(apply konts, consumer konts), not at every kont.

### 6.3 Path navigation

Dotted/indexed path expressions (`a.b[i].c`) walk the capture tree using
the same eval-and-apply discipline. `PathStart(name)` produces a
`FieldCaptureValue` in ac by looking up the named field in the capture
builder. `FieldAccess(field)` and `IndexAccess(index_expr)` consume the
current `FieldCaptureValue` in ac and produce the next one, walking one
step deeper. `CaptureValue` at the outer wraps the final
`FieldCaptureValue` by reading its `computed_value` (a typed Value of
whatever type the field declares) and writing that to ac.

`FieldCaptureValue` is a real Value variant — path navigation has the
same typed-data discipline as everything else. There is no reinterpret-
cast through int64.

### 6.4 The structural sites that consume expressions

Each of the following AST sites carries an expression IR sub-tree; the
structural kont's `invoke()` pushes a runtime consumer kont onto K, then
pushes the expression IR. The expression evaluates first and leaves a
typed Value in ac; the consumer reads ac and acts:

| Site | Consumer | Reads from ac |
|---|---|---|
| `where` constraint | `EvalConstraintCheck` | Bool (or coerced); on false pushes `on_false` (compile-time-baked failure target) |
| `switch` discriminator | `SwitchSelect` | Int or String |
| `compute(name : T = expr)` | `BindComputeStore` | Typed Value |
| Array `count(N)` | `BeginArrayWithLimit` / `ArrayCountCheck` | Int |
| Array `until(cond)` | `ArrayUntilCheck` | Bool (or coerced) |
| `bytes[length]` / `string[length]` | `MatchBytesApply` | Int |
| `@endian: expr` | `SetEndianApply` | Bool (or coerced) |

`EvalConstraintCheck` carries an `on_false` pointer set at compile
time by `compile_primitive` via `find_backtrack_target(rho)`. On false
the kont pushes `on_false` directly — no frame walk. The `on_false`
target is the same `OnFailKont` link the surrounding `Savepoint`
carries as `on_fail_target`, so the static path and the runtime
byte-fail path converge on the same handler. When ρ bottoms out at
root with no backtrackable frame, `compile_primitive` substitutes
`AbortRuleKont` so the failure crosses the rule boundary into the
caller's chain (§7.2).

---

## 7. Runtime Semantics

### 7.1 Trampoline

```cpp
void CEKMachine::execute_from(KontNode* entry) {
    kont_stack.clear();
    kont_stack.push_back(entry);
    while (!kont_stack.empty() && !failed) {
        control = kont_stack.back();
        kont_stack.pop_back();
        control->invoke(this);
    }
}
```

A leaf kont writes ac and pushes nothing. A compound kont pushes its
sub-konts in reverse execution order. Spine konts push their successor
onto K as part of their invoke. K is a real stack, not a static graph
traversal — `control` is loaded fresh from K each iteration and never
advanced via `control->next`.

### 7.2 Backtracking

Backtracking has two convergent paths — a static path resolved at
compile time via ρ, and a runtime path via `m->fail()` walking the
frame stack. Both converge on the same `OnFailKont` link, so the
behavior is identical regardless of which path the failure came from.

**The static path — where-check failure.** `EvalConstraintCheckKont`
carries an `on_false` field set at compile time by `compile_primitive`
via `find_backtrack_target(rho)`. The walker descends ρ to the
innermost backtrackable scope and returns its statically-baked
failure handler:

- `in_alt(next_arm, …)` — returns `next_arm` (the OnFailKont link to
  the next arm).
- `in_optional(skip, …)` — returns `skip` (the OnFailKont link that
  pops + skips past the optional).
- `in_array_elem(skip, …)` — returns `skip` when non-null (the
  ResyncKont for resync arrays); otherwise transparent.
- `in_field(_, parent)` — transparent.
- `root` — returns null. `compile_primitive` substitutes
  `AbortRuleKont` (§cross-rule).

When the where-check fails, it pushes `on_false` directly onto K. No
frame walk.

**The runtime path — byte-fails and other implicit failures.**
Bounds checks, type errors, external parser failures, and the like
call `m->fail()`. `fail()` walks `frame_top` for the innermost
`Savepoint` whose `on_fail_target` is non-null and pushes that
target. Non-Savepoint frames (ReturnFrame, LoopFrame) are walked
past; Savepoints with null `on_fail_target` (exhausted scopes) are
popped and walking continues outward.

The runtime path's target is the same `OnFailKont` link the static
path resolves to, because `compile_alternatives` /
`compile_optional` / `compile_array(resync)` set the Savepoint's
`on_fail_target` to the same link they put in ρ as the corresponding
`next_arm`/`skip`. The two paths name the same kont in two ways.

**OnFailKont semantics** (the link both paths reach):

1. Restore from the topmost Savepoint:
   - `pos` ← saved position
   - `env` ← saved environment
   - `little_endian` ← saved endian flag
   - `builder` ← saved capture mark
   - `kont_stack` ← truncated to saved K size
2. If `target` is null (scope exhausted) — pop the Savepoint and
   re-fail outward.
3. If `pop_frame` is set (optional skip, last-arm cleanup) — pop the
   Savepoint.
4. Otherwise — mutate `Savepoint::on_fail_target` to `next_on_fail`
   so the next failure in this scope advances along the chain.
5. Push `target` onto K.

The per-parse arena is **monotonic within a parse**: speculative
allocations from a failed branch are not freed but become unreachable
when K is truncated and `env` is reset.

**Choice and Optional chain shapes.** `compile_alternatives` builds
the chain right-to-left so each link knows the next arm's entry: head
→ `OnFailKont(target=arm1, next=…)` → … → `OnFailKont(target=null,
pop_frame=true)`. Each non-last arm's compile uses `in_alt(link, …)`
as ρ so where-checks inside the arm find that link via
`find_backtrack_target`. `compile_optional` builds a single
`OnFailKont(target=Lnext, pop_frame=true)` and the inner body
compiles with `in_optional(link, …)` as ρ. Successful arms run
`CommitChoice` to pop the Savepoint and prevent later failures from
re-entering the committed choice (PEG VM `OP_CHOICE` / `OP_COMMIT`,
Medeiros & Ierusalimschy).

**Cross-rule failure.** Each `InvokeRuleNode` carries
`caller_fail_target` set at the call site by
`make_invoke_rule(name, find_backtrack_target(rho), next)`. At
runtime `InvokeRuleNode::invoke` copies it into the new ReturnFrame's
`rule_fail_target`. When a where-check at a rule's top level has no
in-rule backtrack scope, `compile_primitive` emits `AbortRuleKont` as
the `on_false`. `AbortRuleKont::invoke` walks ReturnFrames for the
innermost `rule_fail_target` and dispatches there — the caller's
chain takes over. The runtime path (`m->fail()` for byte-fails)
reaches the caller's Savepoint via the same frame walk, so cross-rule
failure also converges.

If neither path finds a target — no in-rule scope, no caller scope,
nothing — the parse fails.

### 7.3 Optimization patterns

PEG-machine optimization patterns from Medeiros & Ierusalimschy:

- **PartialCommit** — savepoint state is mutated in place per iteration
  rather than push/popping per element. The CEK backend uses this for
  resync arrays (§7.4): `BeginArray` pushes one Savepoint, `ArrayNext`
  refreshes it on every successful iteration, `EndArray` pops it once.
- **Head-fail** — when alternatives have known distinct first bytes,
  test the discriminator before pushing a Savepoint. Effectively a
  switch on the first byte. **Not implemented**; would be a
  compile-time analysis pass.
- **Tail-call** — when the last action in a rule is `InvokeRule`, reuse
  the current `ReturnFrame` rather than pushing a new one. Bounds the
  frame stack for recursive grammars. **Not implemented**.

### 7.4 Resync arrays

`array<T> ... resync` is the per-element-skip parsing mode. When a
record fails to parse at the current position, the array advances by
one byte and retries the same element type from the new position.
Useful for forensic and exploratory parsing of corrupted streams.

Sema rejects `resync` combined with `until`: the two have contradictory
semantics around per-element termination.

The compiler emits a `ResyncKont` and threads it as the
`resync_target` of `BeginArray`. At runtime:

- `BeginArray` pushes a `LoopFrame` and, in resync mode, a `Savepoint`
  with `on_fail_target = ResyncKont`.
- `ArrayNext` refreshes the Savepoint's saved state at the start of
  each new iteration, then drives the loop's termination check (count,
  until, eof).
- `ResyncKont::invoke` restores from the Savepoint, advances `pos` by
  one (or pops the savepoint and exits via `end_node` on EOF), refreshes
  the savepoint to the new attempt position, and pushes the body entry.
- `EndArray` pops the Savepoint (if any) and the LoopFrame, finalizes
  the array's capture group.

The Savepoint is the per-iteration backtrack point: a successful
element advances `pos` past the parsed bytes, then `ArrayNext` updates
the savepoint to the new `pos` so the next failure recovers from
*there*, not from the array's start.

---

## 8. Zero-Copy Capture Output

The capture metadata IS the parse result. After a successful parse, the
caller receives a `CaptureMetadata` tree mapping the hierarchical
structure of the format specification onto byte ranges in the original
input buffer. No bytes are copied during or after parsing.

```cpp
ParseResult result = vm.execute(grammar, "PNGFile", buffer, length);

FieldCapture* header = result.root->child("header");
FieldCapture* magic  = header->child("magic");
// magic->start_offset == 0, magic->end_offset == 4
// magic->type == CaptureType::UInt32BE

// Read the actual value lazily from the input buffer:
uint32_t magic_val = read_uint32be(buffer + magic->start_offset);

FieldCapture* chunks = result.root->child("chunks");
for (int i = 0; i < chunks->child_count; i++) {
    FieldCapture* chunk = &chunks->children[i];
    // …
}
```

Computed fields (from `compute(name : T = expr)`) hold a typed `Value*`
in `computed_value` rather than referencing buffer bytes. The user sees
the actual typed value (Int, Float, String, Bool) directly.

The capture tree is arena-allocated; the caller must keep the
`ParseArena` alive for as long as they access captures, or copy data out
before releasing.

---

## 9. External Parser Interface

External parsers let BBQ specs delegate to user-provided functions for
formats that can't be expressed in BBQ (compression, encryption,
checksums).

```cpp
using ExternalParseFn = bool (*)(
    const uint8_t* data,
    size_t length,
    size_t* bytes_consumed,
    ParseArena* arena,
    void* user_data
);

struct ExternalParserTable {
    struct Entry {
        const char* name;       // Interned
        ExternalParseFn fn;
        void* user_data;
    };
    Entry* entries;
    int count;
    const Entry* lookup(const char* name) const;
};
```

External parsers are **input verification only** — they consume bytes
and record a zero-copy `[start, end)` capture. They do not produce
transformed data that flows back into expression evaluation. Each entry
carries its own `user_data`, so different external parsers can have
independent contexts (e.g., the Python bindings store the callable
object pointer here).

User expression functions (the `FunctionTable`) follow the same pattern
but operate on typed Values:

```cpp
using ExprFunction = Value* (*)(Value** args, int arg_count,
                                 CEKMachine* m, ParseArena* arena);
```

Built-ins (`abs`, `min`, `max`, `clamp`) dispatch on argument types and
produce typed results.

---

## 10. Error Handling

Two outcomes: success or failure. No completion-record machinery for
break/continue/return/throw — BBQ parsing has no such concepts.

### 10.1 Failure modes

| Failure | Source | Recovery |
|---|---|---|
| Match failure | Unexpected bytes | Backtrack via Savepoint chain (§7.2) |
| Constraint violation | `where` clause false | Same |
| End of input | Reading past buffer end | Same |
| External parser failure | User function returned false | Same |
| Per-element parse failure | Inside `array<T> ... resync` | Skip one byte, retry (§7.4) |
| Switch miss | No case matches, no default | Fatal |
| Type error | Value-channel type mismatch | Fatal |
| Expression error | Division by zero, undefined ref | Fatal |

"Backtrack" here means the topmost `Savepoint`'s `on_fail_target`
chain dispatches to the next arm or skip target. If no Savepoint with
a non-null target is in scope, the failure is reported as the parse
result.

### 10.2 Best-error tracking

The machine tracks the **furthest position reached** before any failure.
This is the most useful error position for the user — it indicates
where the parser made the most progress before giving up.

### 10.3 Context stack

As the machine enters and exits rules, it maintains a context stack of
(rule, field, offset) showing the path of nesting. On error, this gives
a diagnostic like:

```
parse error at offset 42:
  in PNGFile.chunks[3].data:
    expected 4 bytes for uint32be, got 2 bytes remaining
```

---

## 11. Thread Safety

**Thread-safe (immutable, shared):**

- `CompiledGrammar` and the entire static-kont graph
- The string pool

**Per-thread (not shared):**

- `CEKMachine` and all mutable state
- `ParseArena` (per-parse allocations including dynamic_kont nodes)
- `CaptureBuilder`, `Environment` chain

Pattern: compile once globally, each thread parses independently with
its own machine + arena.

---

## 12. Performance Characteristics

| Operation | Complexity |
|---|---|
| Arena allocation | O(1) bump |
| Arena reset | O(1) pointer assignment |
| Primitive match | O(1) read + decode |
| Bytes/string match | O(n) byte copy not performed (zero-copy) |
| Environment lookup | O(d) where d = scope depth (typically 3–10) |
| Expression evaluation | O(e) where e = expression tree size (typically 3–7 nodes) |
| Backtracking | O(1) — restore three pointers + truncate K |
| String comparison | O(1) interned pointer equality |
| Overall parse | O(n) input size, single pass (modulo backtracking) |

For binary parsing workloads, file I/O dominates. The interpreter is not
the bottleneck, and no JIT is in scope.

---

## 13. Design Invariants

Forbidden patterns. Any code matching them is weasel structure to remove.

**No third state component beyond C/E/K + ac.** Workspace lives in ac;
bindings live in E. A separate evaluation stack would be neither — it
has nowhere to fit in the channel split (§3.2).

**No chain-following advance.** `control` is loaded from K's top each
trampoline iteration. The field is private with `friend class
CEKMachine`; only the trampoline writes it. Konts schedule successors
via `m->push_kont(next)`, never by assigning `control` or chasing
`control->next`.

**No `reinterpret_cast` of values through `int64`.** Typed Values flow
through ac. Path navigation goes through `FieldCaptureValue` as a real
Value variant.

**No `CompiledExpr` and no recursive `eval()` tree-walker.** Expression
evaluation is K push/pop only.

**No γ=ret or γ=pair fallbacks in DDCG rules.** The driver wraps γ at
the top with `jump(Halt)`; rules emit `cg_deliver(_, δ, γ, Lnext)` and
trust their parameters. Rule bodies do not branch on γ.tag.

**No mid-rule Halt or Return invention.** Program exit is the driver's
responsibility (§5.4). DDCG rules do not allocate `HaltKont`.

**No shared mutable apply-kont slots.** Apply konts are allocated fresh
per evaluation. Pre-allocated apply nodes with reusable `saved_right`
slots break re-evaluation in arrays.

**No silent type coercion.** Type errors fail loudly. Float-truncate-to-
int, string-zero-fallback, cross-type-implicit-promotion are all wrong.

**No runtime arm enumeration for backtracking.** Arm sequencing is
encoded statically in the `OnFailKont` chain built at compile time.
The Savepoint's `on_fail_target` advances through the chain by
mutation as arms try-and-fail; there is no `remaining[]` array of
arms to scan and no runtime reconstruction of arm order. A frame that
holds an explicit arm-list to scan at fail time is a regression.

**ρ is compile-time only and never escapes into the kont graph.**
`rho_t` is threaded through `compile_*` as a value parameter and
discarded at compile end. The kont graph that comes out of
compilation has all its `on_false` / `on_fail_target` /
`caller_fail_target` pointers resolved to plain `KontNode*`. Any
field on a kont that holds a `rho_t` or attempts to do compile-time
walks at runtime is wrong.

---

## Appendix A: Design Decisions and Tradeoffs

### A.1 Continuations vs. bytecode

Continuations were chosen over flat bytecode because:

- **No dispatch overhead** — direct virtual calls, no opcode decoding.
- **Natural graph structure** — switch dispatch, backtracking
  alternatives, and nonterminal calls map naturally to graph edges.
- **Shared sub-graphs** — multiple switch cases share tail continuations
  without duplication.
- **Simpler compiler** — emit nodes and link them, no jump-offset
  calculation.

The tradeoff is slightly higher memory usage (pointers between nodes)
and less cache locality than a flat bytecode array. For binary format
parsing (where I/O dominates), this is negligible.

### A.2 Arena vs. GC

All per-parse allocations are provably short-lived: no closures, no
mutable arrays, no user-defined functions. Parse duration is bounded by
input size. Arena-only management eliminates GC pause times and
bookkeeping.

### A.3 Immutable environments vs. mutable symbol tables

- **Fields are write-once** — once parsed, a field's value never changes.
- **Backtracking is free** — restore an old environment pointer, done.
- **No hash-table overhead** — linked list lookup is O(d), d small.

### A.4 Defunctionalized expressions vs. tree-walking interpreter

A recursive `eval(CompiledExpr*)` tree-walker fits BBQ's shallow
expressions but breaks the CEK shape — there is no real K stack
threading evaluation contexts; the machine stops being a CEK machine
and becomes a hybrid spine+walker. Defunctionalizing expressions per
Reynolds 1972 / Ager-Biernacki-Danvy-Midtgaard 2003 puts evaluation
contexts onto the runtime K, where they belong, and lets the typed
accumulator carry typed Values cleanly. The overhead — a few arena
allocations per binop — is negligible against parse cost.

This is the same eval-and-apply pattern AiPL uses (see `MonadicK`,
`DyadicK`, `EvalDyadicLeftK`, `ApplyDyadicK`, `ArgK` in
`AiPL/include/continuation.h`).

### A.5 Typed Values vs. int64-only accumulator

BBQ field types are typed (uN/iN/fN/string/bytes/bool). An int64-only
accumulator would silently truncate floats, zero-fallback strings, and
require `reinterpret_cast` for capture pointers. A typed `Value*` sum
preserves type information end-to-end at trivial cost (one arena
allocation per produced value).

### A.6 String interning scope

Strings are interned in the `CompiledGrammar`'s string pool — they live
as long as the grammar and are freed when the grammar is released. Per-
parse strings (error messages) use the parse arena and are transient.

---

## Appendix B: Worked Examples

### B.1 Structural parse trace

A simple TLV entry:

```bbq
TLVEntry = struct {
    type:   uint8,
    length: uint16le,
    data:   bytes[length]
}
```

Input: `01 05 00 48 65 6C 6C 6F` (type=1, length=5, data="Hello")

```
STEP 1: BeginStruct("TLVEntry")
  pos=0, env=∅
  Action: push capture scope "TLVEntry", push next on K

STEP 2: MatchPrimitive("type", uint8)
  pos=0 → read 1 byte → IntValue(1) in ac
  pos=1, env={type → IntValue(1), [0,1)}
  Action: capture field "type" [0,1), push next on K

STEP 3: MatchPrimitive("length", uint16le)
  pos=1 → read 2 bytes LE → IntValue(5) in ac
  pos=3, env={length → IntValue(5), [1,3)} → {type → …}
  Action: capture field "length" [1,3), push next on K

STEP 4: MatchBytes("data") with length_expr sub-tree
  Pushes MatchBytesApply consumer + Ref("length") leaf.
  Ref leaf fires: ac = IntValue(5).
  MatchBytesApply fires: reads ac, length=5, bounds-checks, captures.
  pos=8, env={data → …, [3,8)} → ...

STEP 5: EndStruct
  Action: close capture scope "TLVEntry" [0,8)

STEP 6: Halt
  K empty → trampoline exits, success
```

### B.2 Binop K-trace

Expression `version == 1` in a `where` clause. The compiled IR is
`EqKont(RefKont("version"), IntLitKont(1))` wrapped by an
`EvalConstraintKont` whose `invoke` pushes `EvalConstraintCheck` then
the EqKont.

```
Initial K (top → bottom):  [EqKont, EvalConstraintCheck, …]

POP EqKont:
  Allocate ApplyEqKont (saved_right slot empty).
  Allocate EvalLeftKont(apply, RefKont).
  Push apply, eval-left, IntLitKont in reverse exec order.
  K: [IntLitKont(1), EvalLeftKont, ApplyEqKont, EvalConstraintCheck, …]

POP IntLitKont:
  ac ← IntValue(1).
  K: [EvalLeftKont, ApplyEqKont, EvalConstraintCheck, …]

POP EvalLeftKont:
  apply.saved_right ← ac (= IntValue(1)).
  Push RefKont.
  K: [RefKont, ApplyEqKont, EvalConstraintCheck, …]

POP RefKont("version"):
  ac ← env.lookup("version").value (= IntValue(N)).
  K: [ApplyEqKont, EvalConstraintCheck, …]

POP ApplyEqKont:
  left = ac, right = saved_right. Both IntValue.
  Compare → BoolValue(left == right).
  ac ← BoolValue(...).
  K: [EvalConstraintCheck, …]

POP EvalConstraintCheck:
  Read ac, coerce to bool.
  If true: push next.
  If false: push on_false (an OnFailKont link or AbortRuleKont,
            resolved at compile time via find_backtrack_target(ρ)).
```

### B.3 Backtracking K-trace

A `Choice` with three alternatives — `Item = A | B | C`. The first
arm fails after consuming bytes. At compile time `compile_alternatives`
builds the chain:

```
exhausted = OnFailKont(target=null,    next=null,        pop_frame=true)
linkB     = OnFailKont(target=C.entry, next=exhausted,   pop_frame=false)
linkA     = OnFailKont(target=B.entry, next=linkB,       pop_frame=false)
BeginChoice.on_fail_target = linkA
BeginChoice.next            = A.entry  (first arm, no failure setup)
```

At runtime:

```
Initial:  K=[…BeginChoice…], frame_top=…outer…

BeginChoice → push Savepoint{
    saved_pos=10, saved_env=E0, saved_endian=…,
    saved_kont_size=K.size, capture_mark=M0,
    on_fail_target=linkA
} onto frame_top.
                push K: A.entry

Arm A starts. K grows with A's body konts; pos advances to 14.
Some Values + dynamic konts allocated in arena.

A's third kont fails: m->fail("...") called.
  Walk frame_top → topmost Savepoint, on_fail_target=linkA.
  Push K: linkA.

POP linkA:
  Find topmost Savepoint. Restore:
    pos        ← 10
    env        ← E0
    endian     ← saved_endian
    builder    ← M0  (drops captures added during arm A)
    K.resize(saved_kont_size) → drops all arm A entries
  linkA.target = B.entry, pop_frame = false:
    Savepoint.on_fail_target ← linkB  (mutate for next failure)
  Push K: B.entry

Arm B runs. If it fails, linkB fires next: restores, mutates
on_fail_target ← exhausted, pushes C.entry. If C also fails,
exhausted fires: target is null → pop the Savepoint and re-fail
outward, walking frame_top for the next enclosing scope.

Speculatively-allocated Values + dynamic konts from failed arms are
unreachable but still occupy arena. They live until parse end. Sound:
nothing restored points at them.
```

For an `optional<T>` the chain is just `linkA = OnFailKont(target=Lnext,
next=null, pop_frame=true)` — single link, pops the Savepoint
immediately on the failed inner.

---

## Appendix C: References

**Files**

- `backends/cek/bbq_ir.asdl` — IR variant declarations (Value sum,
  static_kont, dynamic_kont)
- `backends/cek/bbq.ddcg` — AST→IR compilation rules
- `backends/cek/templates/bbq_ir.inja` — code generation template
- `ddcgc/lib/dybvig.ddcg` — DDCG codegen primitive library
- `backends/cek/Machine.cpp`, `Compiler.cpp`, `Capture.h`,
  `Environment.h`, `Frame.h` — hand-written runtime
- `frontend/Sema.cpp` — semantic analysis producing the validated AST
- `jitterator/example/grammar/calc.ddcg` — calc compiler as a worked
  DDCG example

**Papers**

- Felleisen & Friedman, *Control Operators, the SECD Machine, and the
  λ-calculus*, Indiana TR 78 (1986); *A Calculus for Assignments in
  Higher-Order Languages* (1987) — the CEK machine.
- Reynolds, *Definitional Interpreters for Higher-Order Programming
  Languages*, ACM '72 — defunctionalization.
- Ager, Biernacki, Danvy & Midtgaard, *A Functional Correspondence
  Between Evaluators and Abstract Machines*, BRICS RS-03-13 (2003) —
  the recipe used to defunctionalize the BBQ expression evaluator.
- Dybvig, Hieb & Butler, *Destination-Driven Code Generation* (1990) —
  the DDCG framework used by `bbq.ddcg`.
- Medeiros & Ierusalimschy, *A Parsing Machine for PEGs* — the PEG VM
  patterns (`OP_CHOICE`, `OP_COMMIT`, `OP_PARTIAL_COMMIT`, `OP_JUMP`)
  that inform Choice/Optional and array-iteration optimizations.

**Implementations**

- `/home/dan/Source/AiPL/` — production CEK machine (~70 continuation
  variants) that validates the eval-and-apply discipline at scale.
- `/home/dan/Source/IPG/` — the original BBQ design docs (interval
  parsing grammars).
- `/home/dan/Source/Peggy/` — PEG parser generator with operational-
  semantics document for the PEG VM patterns.
