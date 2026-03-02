# CEK Machine Design for BBQ

An interpretive backend for BBQ that compiles format specifications into
continuation graphs and executes them against byte buffers, producing zero-copy
capture metadata.

The CEK machine was always part of the BBQ design — the original design docs
(in `/home/dan/Source/IPG/`) spec out the full VM architecture, DDCG
compilation rules, capture metadata, operational semantics, and implementation
plan. The static C++ code generator (`bbqc`) was built first to nail down the
frontend (grammar, AST, Sema) before tackling the VM. This document covers
the VM design as adapted for the actual BBQ AST and Sema output.

All 13 implementation phases are complete. The CEK backend is fully operational
and powers the Python extension module (`bindings/python/bbq_python.cpp`).

---

## 1. Architecture Overview

The CEK machine is the second BBQ backend — an interpreter that complements
the static C++ code generator. Both backends share the same frontend (Scanner,
Parser, Sema); the split happens at the validated AST.

```
                        ┌─────────────────┐
  format.bbq ──────────▶│  BBQ Frontend    │
                        │  Scanner + Parser│
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
          │ Static Backend   │             │ CEK Backend        │
          │ TypeEmitter      │             │ CEK Compiler       │
          │ ParserEmitter    │             │ AST → Kont Graph   │
          │ → C++ source     │             └─────────┬──────────┘
          └──────────────────┘                       │
                                            Immutable continuation graph
                                                     │
                                            ┌────────▼─────────┐
                                            │ CEK Interpreter   │
                                            │ Kont Graph × Input│
                                            │ → Capture Metadata│
                                            └──────────────────┘
```

**Input:** Validated, topologically sorted BBQ AST + byte buffer.

**Output:** `CaptureMetadata` — a hierarchical zero-copy file index recording
`[start, end)` offsets into the original buffer. No data is copied during
parsing; the metadata IS the parse result.

**Key principle:** continuations ARE the compiled
representation. There is no separate IR layer between the AST and the
executable graph — the DDCG compiler emits continuation nodes directly.

### Why both backends?

The static backend generates C++ source — maximum performance, zero runtime
dependency, but requires a C++ compiler. The CEK backend fills complementary
roles:

- **Interactive exploration** — Load a `.bbq` spec and parse files immediately,
  without a C++ compilation step.
- **Hot-reload** — Recompile the continuation graph when the spec changes,
  without restarting.
- **Embedding** — Ship a single library that accepts BBQ specs at runtime
  (no build toolchain dependency).
- **Round-trip testing** — Validate that both backends produce identical parse
  results for the same input.

---

## 2. Memory Management

Three-tier model, validated by AiPL's production experience.

### Tier 1: Immutable Compiled Grammar (Reference-Counted)

```cpp
struct CompiledGrammar {
    std::atomic<int> refcount;

    // Entry point for each rule, indexed by rule name
    std::unordered_map<std::string, KontNode*> entries;

    // Compiled expression trees (shared by continuation nodes)
    std::vector<std::unique_ptr<CompiledExpr>> expr_pool;

    // Interned strings (field names, rule names, error messages)
    StringPool strings;

    // External parser + function registries
    ExternalParserTable ext_parsers;
    FunctionTable ext_functions;

    // Arena that owns all continuation nodes
    Arena graph_arena;
};
```

Allocated once during compilation. Shared across concurrent parses (thread-safe
because immutable). Reference-counted so grammars can be swapped at runtime
without dangling pointers.

### Tier 2: Per-Parse Arena (Bump Allocator)

```cpp
class ParseArena {
    uint8_t* memory;
    size_t used;
    size_t capacity;

public:
    explicit ParseArena(size_t capacity);
    ~ParseArena();

    template<typename T, typename... Args>
    T* alloc(Args&&... args) {
        size_t aligned = align_up(used, alignof(T));
        if (aligned + sizeof(T) > capacity) throw std::bad_alloc();
        T* ptr = new (memory + aligned) T(std::forward<Args>(args)...);
        used = aligned + sizeof(T);
        return ptr;
    }

    void reset() { used = 0; }      // O(1) — no destructors needed
    size_t bytes_used() const { return used; }
};
```

All mutable per-parse state lives here: environments, capture nodes, frame
stack entries. Reset in O(1) after each parse — a single pointer assignment.
No fragmentation, no GC overhead.

**Sizing:** AiPL uses 64KB for ephemeral expression evaluation; binary parsing
workloads are heavier (deeper nesting, larger capture trees). Default to
**256KB**, growing on demand. Most binary format parses (ELF headers, PNG
chunks, TLV streams) will fit within a single block.

**Why no GC?** Unlike AiPL's APL interpreter, the parsing use case creates no
long-lived closures, no user-defined functions, no cyclic structures. All
per-parse allocations are provably short-lived — the arena reset after each
parse is sufficient. This is a major simplification over AiPL's generational
GC, which was necessary only because APL has persistent closures and mutable
arrays.

### Tier 3: Expression Evaluation Stack (No Allocation)

```cpp
static constexpr size_t EVAL_STACK_SIZE = 256;

int64_t eval_stack[EVAL_STACK_SIZE];
size_t eval_top = 0;
```

Fixed-size stack for expression evaluation. Operations push/pop `int64_t`
values. 256 entries is far more than any reasonable BBQ expression requires
(the deepest expressions are `where` constraints with a few operators).
Stack overflow during expression evaluation is a spec authoring error, not
a runtime concern.

---

## 3. Core Data Structures

### 3.1 Machine State

```cpp
struct CEKMachine {
    // ── C: Control ──────────────────────────────────
    // Current continuation node. nullptr means halt.
    KontNode* control;

    // ── E: Environment ──────────────────────────────
    // Variable bindings + capture intervals. Immutable
    // linked list — never mutated, only extended.
    Environment* env;

    // ── K: Kontinuation (frame stack) ───────────────
    // Call/return, backtracking, loop iteration frames.
    Frame* frame_top;

    // ── Input ───────────────────────────────────────
    const uint8_t* input;
    size_t input_length;
    size_t pos;

    // ── Endianness ──────────────────────────────────
    // Resolved from @endian directive or EndianSwitch.
    // true = little-endian, false = big-endian.
    bool little_endian;

    // ── Expression evaluation ───────────────────────
    int64_t eval_stack[EVAL_STACK_SIZE];
    size_t eval_top;

    // ── Output ──────────────────────────────────────
    CaptureNode* capture_root;
    CaptureBuilder builder;

    // ── Memory ──────────────────────────────────────
    ParseArena* arena;
    const CompiledGrammar* grammar;

    // ── Error state ─────────────────────────────────
    ParseError error;
    size_t best_error_pos;      // Furthest position reached before failure
    const char* best_error_msg;
};
```

The CEK acronym maps directly:
- **C** (Control) — `control` pointer to the current continuation node.
- **E** (Environment) — `env` pointer to the current environment.
- **K** (Kontinuation) — `frame_top` pointer to the frame stack.

### 3.2 Continuation Nodes (Immutable, Compiled)

Each BBQ construct compiles to one or more continuation node types. Nodes are
linked into chains via a `next` pointer. Each node carries immutable data
(field names, type info, compiled expressions) allocated in the grammar arena.

```cpp
enum class KontType : uint8_t {
    MatchPrimitive,
    MatchBytes,
    BeginStruct,
    EndStruct,
    InvokeRule,
    ReturnRule,
    EvalConstraint,
    BindCompute,
    BeginChoice,
    CommitChoice,
    Backtrack,
    BeginArray,
    ArrayNext,
    ArraySeparator,
    EndArray,
    BeginOptional,
    EndOptional,
    SwitchDispatch,
    BitfieldRead,
    SetEndian,
    ExternalCall,
    Halt
};

struct KontNode {
    KontType type;
    KontNode* next;         // Chain to next node in sequence

    void invoke(CEKMachine* m) const;
};
```

**Type-specific data** is stored in substructs that extend `KontNode`:

```cpp
struct MatchPrimitiveNode : KontNode {
    const char* field_name;     // Interned string
    PrimitiveInfo prim;         // Width, signedness, endianness
    CompiledExpr* constraint;   // nullptr if no where clause
    IntervalInfo* interval;     // nullptr if no interval
};

struct InvokeRuleNode : KontNode {
    const char* rule_name;
    KontNode* entry;            // Entry point of target rule
    IntervalInfo* interval;
};

struct BeginChoiceNode : KontNode {
    KontNode** alternatives;    // Remaining alternatives (after the first)
    int alt_count;
    // First alternative is wired as `next`. On backtrack, tries alternatives[0..].
    // Pushes AlternativeFrame with join=nullptr (exhaustion = real failure).
};

struct CommitChoiceNode : KontNode {
    // Pops the AlternativeFrame after a successful choice alternative.
    // Without this, the frame lingers and a later failure could incorrectly
    // backtrack into the already-committed choice.
    // Pattern (a la PEG VM's OP_COMMIT):
    //   BeginChoice [alt2, alt3] → alt1 → Commit → join
    //                               alt2 → Commit → join
    //                               alt3 → join   (last alt: no commit needed)
};

struct SwitchDispatchNode : KontNode {
    CompiledExpr* discriminator;
    struct Case {
        int64_t int_val;        // For int cases
        const char* str_val;    // For string/ident cases
        KontNode* target;
    };
    Case* cases;
    int case_count;
    KontNode* default_target;   // nullptr if no default
};

struct BeginArrayNode : KontNode {
    enum class Mode { FixedCount, SepTerm } mode;
    CompiledExpr* count_expr;   // For FixedCount
    KontNode* element_entry;    // Continuation for one element
    KontNode* separator;        // nullptr if no separator
    TerminatorInfo* terminator; // For SepTerm mode
    IntervalInfo* element_interval; // Per-element interval template
};

struct BitfieldReadNode : KontNode {
    PrimitiveInfo container;
    struct Entry {
        const char* name;       // Interned
        int width_bits;
    };
    Entry* entries;
    int entry_count;
    bool msb_first;             // true for BE containers
};

struct ExternalCallNode : KontNode {
    const char* func_name;      // Interned, looked up in registry
    const char* field_name;
};
```

### 3.3 Environment (Arena-Allocated, Immutable Linked List)

The environment serves a dual purpose:
it holds both **variable bindings** for use in expressions AND **capture
intervals** that form the zero-copy file index. After parsing completes, the
final environment IS the metadata.

```cpp
struct Binding {
    const char* name;       // Interned string
    int64_t value;          // Scalar value (for expression evaluation)
    size_t start_offset;    // Capture interval start
    size_t end_offset;      // Capture interval end
    CaptureType type;       // How to interpret the captured bytes
};

struct Environment {
    Binding binding;
    Environment* parent;    // Lexical scope chain
};
```

**Never mutated** — only extended by creating new `Environment` nodes that
point to the previous head. This makes backtracking trivial: restoring the
environment is a single pointer assignment back to the saved state.

**Lookup** walks the parent chain:

```cpp
const Binding* Environment::lookup(const char* name) const {
    // Pointer comparison — names are interned
    for (const Environment* e = this; e; e = e->parent) {
        if (e->binding.name == name) return &e->binding;
    }
    return nullptr;
}
```

### 3.4 Frame Stack (Arena-Allocated)

Frames save machine state for operations that need to restore it later.
Three frame types cover all control flow needs:

```cpp
enum class FrameType : uint8_t {
    Return,         // Nonterminal call — save continuation + env
    Alternative,    // Backtracking choice — save pos + env + remaining alts
    Loop            // Array iteration — save counter + limits
};

struct Frame {
    FrameType type;
    Frame* prev;            // Stack linkage
};

struct ReturnFrame : Frame {
    KontNode* return_to;    // Where to resume after rule completes
    Environment* saved_env; // Caller's environment
    size_t start_pos;       // For computing capture interval [start, end)
    CaptureBuilder::Mark capture_mark; // Capture tree insertion point
};

struct AlternativeFrame : Frame {
    KontNode** remaining;   // Untried alternatives
    int remaining_count;
    KontNode* join;         // Where to continue after any alternative succeeds
    Environment* saved_env;
    size_t saved_pos;
    bool saved_endian;
    CaptureBuilder::Mark capture_mark;
};

struct LoopFrame : Frame {
    int64_t counter;        // Current iteration index
    int64_t limit;          // For FixedCount: total count
    KontNode* body_entry;   // Loop body continuation
    KontNode* separator;    // nullptr if no separator
    KontNode* loop_end;     // Exit continuation
    Environment* body_env;  // Environment at loop start
    CaptureBuilder::Mark capture_mark;
    // For SepTerm arrays:
    TerminatorInfo* terminator;
    // For per-element intervals:
    IntervalInfo* element_interval;
};
```

### 3.5 Capture Metadata (Arena-Allocated Output)

The capture metadata IS the zero-copy file index — the entire point of
parsing. It records where things are in the input buffer, not what they
contain.

```cpp
enum class CaptureType : uint8_t {
    UInt8, UInt16LE, UInt16BE, UInt32LE, UInt32BE, UInt64LE, UInt64BE,
    Int8,  Int16LE,  Int16BE,  Int32LE,  Int32BE,  Int64LE,  Int64BE,
    Float32LE, Float32BE, Float64LE, Float64BE,
    Bool, Bytes, String,
    Struct, Array, Bitfield, Computed, External
};

struct FieldCapture {
    const char* name;           // Interned field name
    size_t start_offset;
    size_t end_offset;
    CaptureType type;

    // For nested captures (Struct, Array):
    FieldCapture* children;
    int child_count;

    // For Computed fields:
    int64_t computed_value;
};

struct CaptureMetadata {
    bool success;
    size_t bytes_consumed;

    FieldCapture* root;         // Top-level rule capture

    // Error info (populated on failure):
    const char* error_message;
    size_t error_offset;
};
```

**Usage after parsing:**

```cpp
CaptureMetadata result = vm.parse(grammar, "Header", input, length);
if (result.success) {
    FieldCapture* magic = result.root->child("magic");
    // magic->start_offset == 0, magic->end_offset == 4
    // magic->type == CaptureType::UInt32BE

    // Read the actual value lazily from the input buffer:
    uint32_t magic_val = read_uint32be(input + magic->start_offset);
}
```

### 3.6 Capture Builder

A helper that accumulates capture metadata during parsing, managing the
nesting stack of struct/array scopes:

```cpp
class CaptureBuilder {
public:
    struct Mark { int depth; int child_index; };

    void begin_struct(const char* name, size_t pos);
    void end_struct(size_t pos);
    void begin_array(const char* name, size_t pos);
    void end_array(size_t pos);
    void add_field(const char* name, size_t start, size_t end,
                   CaptureType type);
    void add_computed(const char* name, int64_t value);
    void add_bitfield_entries(const char* container_name,
                              size_t start, size_t end,
                              const BitfieldEntry* entries, int count);

    Mark mark() const;          // Snapshot for backtracking
    void restore(Mark m);       // Discard captures added since mark

    FieldCapture* finish();     // Return the root capture
};
```

### 3.7 String Interning

Following AiPL's proven pattern, all identifier strings (field names, rule
names, error messages) are interned in the `CompiledGrammar::strings` pool.
This enables O(1) pointer-equality comparisons during environment lookup
instead of O(n) string comparisons.

```cpp
class StringPool {
    std::unordered_map<std::string_view, const char*> table;
    Arena storage;

public:
    const char* intern(std::string_view s);
};
```

All `const char*` pointers in the system (field names in `Binding`, `KontNode`
substructs, `FieldCapture`) are interned — pointer comparison is always
sufficient.

---

## 4. Continuation Node Types

Every BBQ construct maps to one or more continuation nodes. The table below
shows the complete mapping:

| Node Type | BBQ Construct | Behavior |
|-----------|--------------|----------|
| `MatchPrimitive` | Primitive fields (`uint32le`, `int16be`, `float64`, `bool`) | Read N bytes at current position (or interval), decode value, record capture, bind value in environment. Endianness from node data or machine state. |
| `MatchBytes` | `bytes[N]`, `string[N]` | Read variable-length data (length from interval expression). Record capture. Bind length as value. |
| `BeginStruct` | `struct { ... }` | Push a new capture scope. Subsequent field captures nest under this struct. |
| `EndStruct` | (closing `}`) | Pop the capture scope. Record the struct's `[start, end)` interval. |
| `InvokeRule` | Rule reference (`Header`, `Entry`, etc.) | Push a `ReturnFrame` saving current continuation, environment, and position. Jump control to the rule's entry point. Apply interval if specified. |
| `ReturnRule` | (end of rule body) | Pop the `ReturnFrame`. Record the rule's capture interval. Restore the caller's continuation and environment (extended with the new binding). |
| `EvalConstraint` | `where` clause | Evaluate the constraint expression using the current environment. If false, trigger failure (backtrack or error). |
| `BindCompute` | `compute(expr : type)` | Evaluate expression, bind result in environment. No bytes consumed, no capture interval — only a computed value. |
| `BeginChoice` | `union { ... }`, `alt1 \| alt2` | Push an `AlternativeFrame` (with `join=nullptr`) saving position, environment, endianness, and the list of remaining alternatives. Jump to the first alternative. |
| `CommitChoice` | (successful alternative) | Pop the `AlternativeFrame` after a successful choice alternative. Prevents later failures from backtracking into the committed choice. All alternatives except the last chain through `CommitChoice` before reaching the join point. (PEG VM OP_COMMIT pattern.) |
| `Backtrack` | (failure during alternative) | Pop the most recent `AlternativeFrame`. Restore saved state. If alternatives remain, try the next one. If none remain (and `join=nullptr`), propagate failure upward. If `join` is set (optional), skip to join. |
| `BeginArray` | `array<T>[N]`, `array<T>(sep, term)` | Push a `LoopFrame` with counter, limit (or terminator info), and body entry. Initialize counter to 0. Begin array capture scope. |
| `ArrayNext` | (end of array element) | Increment counter. Check termination condition (count reached, terminator matched, `until` condition true, EOF). If not done, loop back to body entry (with optional separator). If done, fall through to `EndArray`. |
| `ArraySeparator` | `sep` in `array<T>(sep, term)` | Parse the separator type between elements. On failure in a sep/term array, treat as end-of-array. |
| `EndArray` | (array complete) | Pop the `LoopFrame`. Close array capture scope. |
| `BeginOptional` | `optional<T>` | Push an `AlternativeFrame` with zero remaining alternatives (so failure means "absent, not error"). Jump to the inner type. |
| `EndOptional` | (optional element parsed) | Pop the `AlternativeFrame` (success path). Mark optional as present. |
| `SwitchDispatch` | `switch(expr) { ... }` | Evaluate discriminator expression. Look up matching case (int, string, or ident). Jump to the matching case's continuation, or the default. No backtracking — switch is deterministic. |
| `BitfieldRead` | `bitfield<container> { ... }` | Read the container (e.g. `uint16be`). Extract each named bit range using shift-mask. Bind each entry's value in the environment. Record captures for container and entries. |
| `SetEndian` | `@endian` directive, `@endian: expr` | Update `machine->little_endian`. For directive: set at compile time. For EndianSwitch: evaluate expression at runtime. |
| `ExternalCall` | `extern("func", "type")` | Look up the named function in the grammar's external parser registry. Call it with the current input position and arena. On success, record capture and advance position. |
| `Halt` | (end of top-level rule) | Set `control = nullptr` to exit the trampoline loop. Parse succeeded. |

---

## 5. Compilation Rules (BBQ AST → Continuation Graph)

The DDCG (Destination-Driven Code Generation) compiler transforms the validated
BBQ AST into a linked graph of continuation nodes. Each AST node type has a
compilation rule that emits nodes into the grammar arena.

The compiler maintains a **continuation parameter** — the "what comes next"
pointer that each emitted node chains to. This is the destination-driven aspect:
the compiler knows where each subexpression's control flows to before emitting it.

**Theoretical connection:** The DDCG pattern originates in PEG VM research
(Medeiros & Ierusalimschy, "A Parsing Machine for PEGs"), where the key insight
is that **destinations are continuations**. A DDCG compiler threads a "where to
go next" parameter through every compilation rule — this is exactly a
continuation. The BBQ CEK machine makes this explicit: the `next` pointer on
every `KontNode` *is* the destination, and the compilation rules in §5.1–§5.8
are DDCG rules that wire these destination-continuations. The `CommitChoice`
and `AlternativeFrame` patterns also derive from the PEG VM operational
semantics (`OP_CHOICE`/`OP_COMMIT`/`OP_PARTIAL_COMMIT`), adapted from bytecode
offsets to linked continuation nodes.

### 5.1 Grammar Compilation

```
compile_grammar(Grammar{directives, rules}) →
    For each directive: record endian default
    For each rule (in topological order):
        entries[rule.name] = compile_rule(rule)
```

### 5.2 Rule Compilation

```
compile_rule(Rule{name, body}, join) →
    body_chain = compile_type_expr(body, ReturnRule → join)
    return body_chain    // Entry point for this rule
```

### 5.3 Struct

```
compile(Struct{fields}, next) →
    BeginStruct(name) →
    compile_field(fields[0]) →
    compile_field(fields[1]) →
    ... →
    compile_field(fields[N]) →
    EndStruct → next
```

Each field is compiled as:

```
compile_field(Field{name, body, constraint, interval}, next) →
    body_chain = compile_type_expr(body, next)
    if constraint:
        body_chain = compile_type_expr(body,
            EvalConstraint(constraint) → next)
    if interval:
        // Wrap in position save/restore for [start, end] intervals
    return body_chain
```

Fields that are `EndianSwitch` emit a `SetEndian` node instead of a field parse.

### 5.4 Union / Alternatives

```
compile(Union{variants: [v1, v2, ..., vN]}, next) →
    commit = CommitChoice → next
    alt_chains = [compile_variant(v2, commit),
                  compile_variant(v3, commit),
                  ...,
                  compile_variant(v(N-1), commit),
                  compile_variant(vN, next)]    // last alt: no commit needed
    BeginChoice(alt_chains) →
    compile_variant(v1, commit)
```

The first variant is tried immediately. If it succeeds, `CommitChoice` pops
the `AlternativeFrame` so that later failures don't backtrack into the
already-committed choice (PEG VM OP_COMMIT pattern). If the first variant
fails, the machine backtracks to the `AlternativeFrame` and tries the next
chain from `alt_chains`. The last alternative needs no `CommitChoice` because
the frame was already consumed by backtracking to reach it.

Rule-level alternatives (`A = X | Y | Z`) compile identically:

```
compile(Alternatives{alts: [a1, a2, ..., aN]}, next) →
    commit = CommitChoice → next
    alt_chains = [compile_type_expr(a2, commit),
                  ...,
                  compile_type_expr(a(N-1), commit),
                  compile_type_expr(aN, next)]
    BeginChoice(alt_chains) →
    compile_type_expr(a1, commit)
```

### 5.5 Array (Fixed Count)

```
compile(Array{element, FixedCount(count_expr)}, next) →
    end = EndArray → next
    body = compile_type_expr(element, ArrayNext → ...)
    BeginArray(mode=FixedCount, count=count_expr, body=body, end=end) → body
```

The `BeginArray` node pushes a `LoopFrame` with counter=0 and limit=eval(count_expr).
`ArrayNext` increments the counter; if counter < limit, it resets control to `body`.

### 5.6 Array (Separator/Terminator)

```
compile(Array{element, SepTerm(sep, term)}, next) →
    end = EndArray → next
    sep_chain = compile_separator(sep)   // or nullptr for NoneSep
    body = compile_type_expr(element, ArrayNext → ...)
    BeginArray(mode=SepTerm, body=body, sep=sep_chain, term=term, end=end) → body
```

`ArrayNext` in SepTerm mode:
1. Check terminator condition (EOF, count, until-expr, or try-parse type terminator).
2. If terminated: jump to `EndArray`.
3. If separator exists: parse separator; on separator failure, jump to `EndArray`.
4. Loop back to `body`.

### 5.7 Array (Per-Element Intervals)

When `element_interval` is present (only on fixed-count arrays), each iteration
evaluates the interval expressions with `i` bound to the current loop counter:

```
ArrayNext with element_interval:
    i = frame->counter
    start = eval(interval.start, env + {i → counter})
    end = eval(interval.end, env + {i → counter})
    seek to start, set scope end to end
    jump to body
```

### 5.8 Optional

```
compile(Optional{element, constraint}, next) →
    if constraint:
        // Conditional presence: evaluate constraint, skip if false
        EvalConstraint(constraint,
            on_true:  compile_type_expr(element, next),
            on_false: next)
    else:
        // Try-parse with backtracking
        end = EndOptional → next
        inner = compile_type_expr(element, end)
        BeginOptional → inner
        // BeginOptional pushes an AlternativeFrame with 0 remaining
        // alternatives. Failure → restore pos, skip to next.
```

### 5.9 Switch

```
compile(Switch{disc, cases, default}, next) →
    case_targets = [
        {value: c.value, target: compile_type_expr(c.target, next)}
        for c in cases
    ]
    default_target = default ? compile_type_expr(default.target, next) : nullptr
    SwitchDispatch(disc, case_targets, default_target)
```

No backtracking — the discriminator is evaluated once and the matching case is
selected deterministically. If no case matches and there is no default, parsing
fails with an error.

### 5.10 Primitive

```
compile(Primitive{kind, constraint, interval}, next) →
    node = MatchPrimitive(kind, interval)
    if constraint:
        node.next = EvalConstraint(constraint) → next
    else:
        node.next = next
    return node
```

Endianness resolution:
- Explicit suffix (`uint32le`, `uint32be`) → baked into the node.
- `Default` endianness → node stores `Default`, resolved at execution time
  from `machine->little_endian`.

### 5.11 Bytes / String

```
compile(Primitive{BytesKind or StringKind, constraint, interval}, next) →
    MatchBytes(interval) → [EvalConstraint(constraint) →] next
```

The interval is required for `bytes` and `string` — Sema enforces this.

### 5.12 Rule Reference

```
compile(RuleRef{name, constraint, interval}, next) →
    InvokeRule(name, entry=grammar.entries[name], interval) →
    [EvalConstraint(constraint) →] next
```

`InvokeRule` pushes a `ReturnFrame` and jumps to the target rule's entry point.
When the rule completes, `ReturnRule` pops the frame and resumes at the
continuation stored in the frame.

### 5.13 Compute

```
compile_field(Field{name, body=Compute{expr, type}}, next) →
    BindCompute(name, expr, type) → next
```

No bytes consumed. The expression is evaluated using current environment
bindings, and the result is bound as a new environment entry (with a zero-width
capture interval at the current position).

### 5.14 Bitfield

```
compile(Bitfield{container, entries}, next) →
    BitfieldRead(container, entries, msb_first=is_big_endian(container)) → next
```

Execution:
1. Read the container value (e.g. 2 bytes for `uint16be`).
2. For each entry, extract bits using shift-mask:
   - BE (MSB-first): shift right from highest bits, first entry at top.
   - LE (LSB-first): shift right from lowest bits, first entry at bottom.
3. Bind each entry's value in the environment.
4. Record a container-level capture with child entries.

### 5.15 Extern

```
compile(Extern{func_name, cpp_type, constraint, interval}, next) →
    ExternalCall(func_name) → [EvalConstraint(constraint) →] next
```

### 5.16 EndianSwitch

```
compile_field(Field{name="", body=EndianSwitch{expr}}, next) →
    SetEndian(expr) → next
```

---

## 6. Expression Evaluation

Expressions are compiled into `CompiledExpr` trees during grammar compilation
and evaluated at runtime using a stack-based evaluator. No allocation occurs
during expression evaluation — only the fixed `eval_stack` is used.

### 6.1 Compiled Expression Tree

```cpp
enum class ExprOp : uint8_t {
    // Literals
    IntLit, FloatLit, StrLit, BoolLit,
    // Variable reference (interned name)
    Ref,
    // Binary operators
    Add, Sub, Mul, Div, Mod,
    BitAnd, BitOr, BitXor, Shl, Shr,
    And, Or, Eq, Ne, Lt, Le, Gt, Ge,
    // Unary operators
    Neg, BitNot, Not, Abs,
    // Ternary
    Ternary,
    // Function call
    Call,
    // Special references
    FieldAccess,    // a.b
    IndexAccess,    // a[i]
    Pos,            // current byte position
    Remaining,      // bytes remaining in scope
    AtEnd,          // true if no bytes remain
    EOI,            // total input length
    LoopVar         // i (loop counter)
};

struct CompiledExpr {
    ExprOp op;
    union {
        int64_t int_val;
        double float_val;
        const char* str_val;        // Interned
        bool bool_val;
        const char* ref_name;       // Interned variable name
        struct { CompiledExpr* left; CompiledExpr* right; } binary;
        struct { CompiledExpr* operand; } unary;
        struct { CompiledExpr* cond; CompiledExpr* then_; CompiledExpr* else_; } ternary;
        struct { const char* func_name; CompiledExpr** args; int arg_count; } call;
        struct { CompiledExpr* base; const char* field; } field_access;
        struct { CompiledExpr* base; CompiledExpr* index; } index_access;
    };
};
```

### 6.2 Stack-Based Evaluator

```cpp
int64_t eval(const CompiledExpr* expr, CEKMachine* m) {
    switch (expr->op) {
    case ExprOp::IntLit:
        return expr->int_val;
    case ExprOp::Ref: {
        const Binding* b = m->env->lookup(expr->ref_name);
        return b->value;
    }
    case ExprOp::Add:
        return eval(expr->binary.left, m) + eval(expr->binary.right, m);
    case ExprOp::Eq:
        return eval(expr->binary.left, m) == eval(expr->binary.right, m) ? 1 : 0;
    case ExprOp::Ternary:
        return eval(expr->ternary.cond, m)
            ? eval(expr->ternary.then_, m)
            : eval(expr->ternary.else_, m);
    case ExprOp::Call:
        return eval_call(expr, m);
    case ExprOp::Pos:
        return (int64_t)m->pos;
    case ExprOp::Remaining:
        return (int64_t)(m->input_length - m->pos);
    case ExprOp::EOI:
        return (int64_t)m->input_length;
    // ... remaining cases
    }
}
```

The recursive call depth is bounded by expression tree depth — BBQ expressions
are shallow (typically 3–5 levels). No stack overflow risk. The alternative
(iterative stack-based postorder traversal) adds complexity without benefit
for these shallow trees.

### 6.3 Built-in Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `crc32` | `crc32(data_ref, length) → uint32` | CRC-32 over a byte range |
| `checksum_xor` | `checksum_xor(data_ref, length) → uint8` | XOR checksum |
| `abs` | `abs(x) → int64` | Absolute value |
| `min` | `min(a, b) → int64` | Minimum |
| `max` | `max(a, b) → int64` | Maximum |

### 6.4 Host-Extensible Function Table

Users can register custom functions for expression evaluation:

```cpp
using ExprFunction = int64_t (*)(const int64_t* args, int arg_count,
                                  const CEKMachine* machine);

struct FunctionTable {
    std::unordered_map<const char*, ExprFunction> functions;

    void register_function(const char* name, ExprFunction fn);
    ExprFunction lookup(const char* name) const;
};
```

---

## 7. Trampoline Execution Loop

The core execution model is a trampoline — a flat loop that dispatches to
continuation nodes without recursive function calls. This is the same pattern
proven in AiPL's 70-continuation machine.

```cpp
ParseResult CEKMachine::execute(const CompiledGrammar* grammar,
                                 const char* rule_name,
                                 const uint8_t* input,
                                 size_t length) {
    // Initialize machine state
    this->grammar = grammar;
    this->input = input;
    this->input_length = length;
    this->pos = 0;
    this->little_endian = grammar->default_little_endian;
    this->env = nullptr;
    this->frame_top = nullptr;
    this->eval_top = 0;
    this->error = {};
    this->best_error_pos = 0;

    arena->reset();
    builder.reset();

    // Set control to the rule's entry point, with Halt as the final continuation
    KontNode halt_node{KontType::Halt, nullptr};
    control = grammar->entries.at(rule_name);
    // The rule's ReturnRule will chain to Halt

    // ── Trampoline loop ──────────────────────────
    while (control != nullptr) {
        control->invoke(this);
    }

    // ── Build result ─────────────────────────────
    if (error.type == ErrorType::None) {
        return ParseResult{
            .success = true,
            .bytes_consumed = pos,
            .root = builder.finish(),
        };
    } else {
        return ParseResult{
            .success = false,
            .error_message = best_error_msg,
            .error_offset = best_error_pos,
        };
    }
}
```

**Key properties** (from AiPL lessons):

1. **No recursive calls.** Every `invoke()` method modifies machine state and
   returns — it never calls another `invoke()`. Nonterminal calls push frames
   and redirect `control`; they don't recurse.

2. **Each `invoke()` may:** advance `control` (to `next`), redirect `control`
   (to a different node), push/pop frames, extend `env`, update `pos`, trigger
   backtracking.

3. **Backtracking** pops `AlternativeFrame`s to restore state and try the next
   alternative. If no alternatives remain, the parse fails.

4. **No GC/cleanup during execution.** The arena handles all allocation; no
   objects are freed until arena reset after the parse completes.

### 7.1 Continuation Dispatch

Each `KontNode::invoke()` is a short, focused function. Following AiPL's
pattern, communication between continuations happens through machine state
(not return values):

```cpp
void MatchPrimitiveNode::invoke(CEKMachine* m) const {
    // Check bounds
    size_t byte_count = prim_byte_count(prim);
    if (m->pos + byte_count > m->input_length) {
        m->fail("unexpected end of input reading %s", field_name);
        return;
    }

    // Read and decode value
    int64_t value = read_primitive(m->input + m->pos, prim,
                                    m->little_endian);

    // Record capture
    size_t start = m->pos;
    m->pos += byte_count;
    m->builder.add_field(field_name, start, m->pos,
                          primitive_capture_type(prim));

    // Bind value in environment
    Binding b{field_name, value, start, m->pos,
              primitive_capture_type(prim)};
    m->env = m->arena->alloc<Environment>(b, m->env);

    // Advance control
    m->control = next;
}

void BeginChoiceNode::invoke(CEKMachine* m) const {
    // Push alternative frame with remaining options
    auto* frame = m->arena->alloc<AlternativeFrame>();
    frame->type = FrameType::Alternative;
    frame->remaining = alternatives;
    frame->remaining_count = alt_count;
    frame->join = nullptr;          // Choice: exhaustion = real failure (not skip)
    frame->saved_env = m->env;
    frame->saved_pos = m->pos;
    frame->saved_endian = m->little_endian;
    frame->capture_mark = m->builder.mark();
    frame->prev = m->frame_top;
    m->frame_top = frame;

    // Try first alternative (already wired as next in the chain)
    m->control = next;
}

void CommitChoiceNode::invoke(CEKMachine* m) const {
    // Pop the AlternativeFrame — this alternative succeeded.
    // Prevents later failures from backtracking into the choice.
    // (Equivalent to PEG VM's OP_COMMIT instruction.)
    m->frame_top = m->frame_top->prev;
    m->control = next;
}

void InvokeRuleNode::invoke(CEKMachine* m) const {
    // Apply interval if present
    if (interval) {
        // Save/restore position handled by frame
    }

    // Push return frame
    auto* frame = m->arena->alloc<ReturnFrame>();
    frame->type = FrameType::Return;
    frame->return_to = next;       // Resume here after rule completes
    frame->saved_env = m->env;
    frame->start_pos = m->pos;
    frame->capture_mark = m->builder.mark();
    frame->prev = m->frame_top;
    m->frame_top = frame;

    // Jump to rule entry
    m->control = entry;
}
```

### 7.2 Backtracking

When any continuation detects failure (bounds check, constraint violation,
match failure), it calls `m->fail()`:

```cpp
void CEKMachine::fail(const char* message) {
    // Track best error position for diagnostics
    if (pos >= best_error_pos) {
        best_error_pos = pos;
        best_error_msg = message;
    }

    // Search for an AlternativeFrame to backtrack to
    while (frame_top != nullptr) {
        if (frame_top->type == FrameType::Alternative) {
            auto* alt = static_cast<AlternativeFrame*>(frame_top);
            if (alt->remaining_count > 0) {
                // Restore state and try next alternative
                pos = alt->saved_pos;
                env = alt->saved_env;
                little_endian = alt->saved_endian;
                builder.restore(alt->capture_mark);
                control = alt->remaining[0];
                alt->remaining++;
                alt->remaining_count--;
                return;
            }
            // Frame with 0 remaining alternatives:
            //   join != nullptr → optional (restore state, skip to join)
            //   join == nullptr → choice (all alts exhausted, keep searching)
            if (alt->join) {
                pos = alt->saved_pos;
                env = alt->saved_env;
                little_endian = alt->saved_endian;
                builder.restore(alt->capture_mark);
                frame_top = alt->prev;
                control = alt->join;
                return;
            }
        }
        // Pop non-alternative frames (ReturnFrame, LoopFrame)
        // and exhausted choice frames (join == nullptr)
        frame_top = frame_top->prev;
    }

    // No alternatives remain — parse fails
    failed = true;
    control = nullptr;
}
```

**Choice vs Optional frame semantics.** Both use `AlternativeFrame`, but they
differ in the `join` field:

- **Choice** (`BeginChoiceNode`): `join = nullptr`. When all alternatives are
  exhausted, `fail()` pops the frame and continues searching — there is no skip
  point. Successful alternatives must execute a `CommitChoiceNode` to pop the
  frame before continuing, otherwise a later failure could incorrectly backtrack
  into the already-succeeded choice.

- **Optional** (`BeginOptionalNode`): `join = skip_point`. When the optional body
  fails, `fail()` restores state and jumps to `join` (the continuation after the
  optional). Successful bodies execute `EndOptionalNode` to pop the frame.

This is the same pattern as PEG VM's `OP_CHOICE`/`OP_COMMIT` (see Peggy):

```
Choice codegen pattern for (a | b | c):

  BeginChoice [alt2, alt3]
    alt1_body → CommitChoice → join    // commit pops frame on success
  alt2:
    alt2_body → CommitChoice → join
  alt3:
    alt3_body → join                   // last alt: frame already consumed
  join:
    ...continuation...
```

**Zero-copy restoration:** backtracking only restores pointers (`pos`, `env`,
`capture_mark`). No data is copied or reconstructed — the immutable
environment chain and arena allocation model make this free.

### 7.3 Optimization Patterns from PEG Operational Semantics

Three additional patterns from the PEG VM operational semantics (Medeiros &
Ierusalimschy; see also `../Peggy/Operational Semantics.txt`) are directly
applicable to the BBQ CEK machine. These are optimization opportunities for
future phases, not required for correctness.

#### 7.3.1 PartialCommit (Arrays)

PEG VM's `OP_PARTIAL_COMMIT` updates the saved position in an existing
`AlternativeFrame` without popping and re-pushing it. This is the loop body
pattern: instead of `Commit → BeginChoice` on every iteration (pop frame, push
new frame), `PartialCommit` updates `saved_pos`, `saved_env`, and
`capture_mark` in place.

Applicable to BBQ arrays (Phase 7). The `ArrayNext` continuation should update
the `LoopFrame`'s saved state rather than cycling frames:

```
Array codegen: array<T>[N]
  BeginArray(N)
    element_body → PartialCommit → (loop back to element_body)
  EndArray
```

This eliminates N frame push/pop pairs per array, replacing them with N
in-place pointer updates. For large arrays (packet payloads, table entries),
this is a significant win.

#### 7.3.2 Head-Fail Optimization (Discriminated Unions)

When the first byte (or first few bytes) of each alternative is a known
constant, the compiler can test the discriminator *before* pushing an
`AlternativeFrame`. If it doesn't match, skip to the next alternative without
any frame overhead.

This is effectively a `switch` on the first byte, falling back to
`BeginChoice` only when alternatives share a common prefix. The BBQ `switch`
construct already handles explicit discrimination; this optimization would
apply to implicit unions where alternatives start with distinct primitives.

```
Head-fail for (a | b | c) where each starts with a distinct byte:
  if input[pos] == a_head → alt_a_body → join
  if input[pos] == b_head → alt_b_body → join
  if input[pos] == c_head → alt_c_body → join
  fail()   // no match
```

No `AlternativeFrame` pushed at all — pure sequential tests with direct jumps.

#### 7.3.3 Tail-Call Optimization (Rule Chaining)

When the last action in a rule body is an `InvokeRule` call, the compiler can
reuse the current `ReturnFrame` instead of pushing a new one. The callee's
`ReturnRule` returns directly to the caller's caller.

```
Without tail-call:  push ReturnFrame → jump to callee → ... → pop → return
With tail-call:     rewrite ReturnFrame.return_to → jump to callee → ... → pop → return
```

This bounds the frame stack depth for recursive grammars (e.g., recursive
descent through nested TLV structures) and is a direct application of the PEG
`OP_JUMP` instruction (call without pushing a return address).

---

## 8. Zero-Copy Capture Output

The capture metadata IS the parse result. After a successful parse, the caller
receives a `CaptureMetadata` tree that maps the hierarchical structure of the
format specification onto byte ranges in the original input buffer. No bytes
are copied during parsing or after.

### 8.1 Lazy Access Pattern

```cpp
// After parsing:
ParseResult result = vm.execute(grammar, "PNGFile", buffer, length);

// Navigate the capture tree:
FieldCapture* header = result.root->child("header");
FieldCapture* magic = header->child("magic");
// magic->start_offset == 0, magic->end_offset == 4
// magic->type == CaptureType::UInt32BE

// Read values lazily (only when needed):
uint32_t magic_val = read_uint32be(buffer + magic->start_offset);

// Array access:
FieldCapture* chunks = result.root->child("chunks");
for (int i = 0; i < chunks->child_count; i++) {
    FieldCapture* chunk = &chunks->children[i];
    FieldCapture* length = chunk->child("length");
    uint32_t len = read_uint32be(buffer + length->start_offset);
}
```

### 8.2 Capture Tree Navigation

```cpp
struct FieldCapture {
    // ... (fields from §3.5)

    FieldCapture* child(const char* name) const {
        // Linear scan — struct fields are few; interned pointer comparison
        for (int i = 0; i < child_count; i++) {
            if (children[i].name == name) return &children[i];
        }
        return nullptr;
    }

    // Convenience readers (interpret bytes from original buffer):
    uint8_t  read_uint8(const uint8_t* buf) const;
    uint16_t read_uint16le(const uint8_t* buf) const;
    uint32_t read_uint32be(const uint8_t* buf) const;
    // ... etc for all primitive types, dispatching on this->type
};
```

### 8.3 Lifetime Management

The capture tree is arena-allocated. The caller must keep the `ParseArena`
alive (or copy the data out) for as long as they access the captures. A
typical usage pattern:

```cpp
{
    ParseArena arena(256 * 1024);
    CEKMachine vm(&arena);
    ParseResult result = vm.execute(grammar, "Header", buf, len);
    // Use result.root while arena is alive
    process(result.root, buf);
}   // Arena destroyed — all captures freed
```

Or for persistent access, serialize the capture tree to a flat structure
before destroying the arena.

---

## 9. External Parser Interface

External parsers let BBQ specs delegate to user-provided C++ functions for
formats that can't be expressed in BBQ (compression, encryption, checksums).

```cpp
using ExternalParseFn = bool (*)(
    const uint8_t* data,    // Input at current position
    size_t length,          // Remaining bytes
    size_t* bytes_consumed, // Out: how many bytes were consumed
    ParseArena* arena,      // For allocating output data
    void* user_data         // Per-entry user context
);

struct ExternalParserTable {
    struct Entry {
        const char* name;       // Interned
        ExternalParseFn fn;
        void* user_data;        // Per-entry context
    };
    Entry* entries = nullptr;   // User-allocated array
    int count = 0;

    const Entry* lookup(const char* name) const;  // Linear scan, pointer equality
};
```

Each `Entry` carries its own `user_data`, so different external parsers can
have independent contexts (e.g., the Python bindings store the callable
object pointer here).

**Execution in `ExternalCallNode::invoke()`:**

```cpp
void ExternalCallNode::invoke(CEKMachine* m) {
    if (!m->ext_parsers) {
        m->fail("external parser table not configured");
        return;
    }

    auto* entry = m->ext_parsers->lookup(func_name);
    if (!entry) {
        m->fail("external parser not registered");
        return;
    }

    size_t remaining = m->input_length - m->pos;
    size_t consumed = 0;
    bool ok = entry->fn(m->input + m->pos, remaining, &consumed,
                        m->arena, entry->user_data);
    if (!ok) {
        m->fail("external parser failed");
        return;
    }

    size_t start = m->pos;
    m->pos += consumed;
    m->builder.add_field(field_name, start, m->pos, CaptureType::External);
    // Bind in environment + advance control
    ...
}
```

---

## 10. Error Handling

Error handling is simplified from AiPL's completion record pattern. AiPL needs
BREAK, CONTINUE, RETURN, and THROW completion types for a general-purpose
language. BBQ parsing only needs two outcomes: **success** or **failure**.

### 10.1 Failure Modes

| Failure | Source | Recovery |
|---------|--------|----------|
| Match failure | Unexpected bytes | Backtrack to nearest `AlternativeFrame` |
| Constraint violation | `where` clause evaluates false | Backtrack or report error |
| End of input | Reading past buffer end | Backtrack or report error |
| External parser failure | User function returns false | Backtrack or report error |
| Switch miss | No case matches, no default | Fatal error (no backtracking) |
| Expression error | Division by zero, undefined ref | Fatal error |

### 10.2 Best-Error Tracking

The machine tracks the **furthest position reached** before any failure. This
is the most useful error position for the user — it indicates where the parser
made the most progress before giving up.

```cpp
struct ParseError {
    ErrorType type;
    const char* message;
    size_t offset;              // Best error position

    // Context stack for nested rule diagnostics:
    struct Context {
        const char* rule_name;
        const char* field_name;
        size_t offset;
    };
    Context context_stack[32];
    int context_depth;
};
```

### 10.3 Context Stack

As the machine enters and exits rules (via `InvokeRule`/`ReturnRule`), it
maintains a context stack showing the path of nesting. On error, this gives
a diagnostic like:

```
parse error at offset 42:
  in PNGFile.chunks[3].data:
    expected 4 bytes for uint32be, got 2 bytes remaining
```

### 10.4 Constraint Error Messages

When a `where` constraint fails, the error includes the constraint expression
and the actual values:

```
constraint violation at offset 8:
  in Header.magic:
    where magic == 0x89504E47
    actual value: 0x00000000
```

---

## 11. Integration with BBQ Frontend

### 11.1 CLI Integration

```
bbqc -backend cpp    format.bbq    # Existing: generate C++ source
bbqc -backend cek    format.bbq    # New: compile + run interactively
bbqc -backend both   format.bbq    # Both backends (for round-trip testing)
```

Or as a standalone tool:

```
bbq-vm format.bbq input.bin        # Parse input.bin using format.bbq
bbq-vm format.bbq -rule Header input.bin   # Parse only a specific rule
bbq-vm format.bbq -hexdump input.bin       # Hex dump with field annotations
```

### 11.2 Library API

```cpp
#include "bbq/vm.h"

// Compile a BBQ spec (reusable across parses)
CompiledGrammar* grammar = bbq_compile_file("format.bbq");
// or: bbq_compile_string(bbq_source, source_length)

// Parse a buffer
ParseArena arena(256 * 1024);
CEKMachine vm(&arena);
ParseResult result = vm.execute(grammar, "Header", buffer, buffer_length);

if (result.success) {
    // Walk the capture tree
    FieldCapture* magic = result.root->child("magic");
    uint32_t magic_val = magic->read_uint32be(buffer);
}

// Reuse for another parse (arena resets internally)
ParseResult result2 = vm.execute(grammar, "Header", buffer2, len2);

// Clean up
bbq_grammar_release(grammar);
```

### 11.3 Frontend Sharing

Both backends consume the same validated AST from the Sema phase:

```cpp
// In bbqc main():
BBQ::Grammar* ast = parse(source);
Sema sema;
sema.analyze(ast);

if (backend == "cpp") {
    TypeEmitter te;
    te.emit(ast, output_dir);
    ParserEmitter pe;
    pe.emit(ast, output_dir);
} else if (backend == "cek") {
    CEKCompiler compiler;
    CompiledGrammar* grammar = compiler.compile(ast);
    // ... interactive mode or parse files
}
```

### 11.4 Round-Trip Testing

With both backends available, we can verify equivalence:

```cpp
// For each test case:
//   1. Parse with static backend (compile C++, run)
//   2. Parse with CEK backend (interpret)
//   3. Compare capture metadata (field names, offsets, values)
```

This is a powerful validation strategy — two independent implementations
of the same specification should produce identical results.

---

## 12. Thread Safety

Thread-safety model:

**Thread-safe (immutable, shared):**
- `CompiledGrammar` — continuation graph, expression trees, string pool
- All `KontNode` substructs — read-only after compilation
- `CompiledExpr` trees — read-only after compilation

**Per-thread (not shared):**
- `CEKMachine` — all mutable state
- `ParseArena` — allocations are thread-local
- `CaptureBuilder` — accumulates per-parse
- `Environment` chain — grows per-parse

**Pattern:** Compile once globally, each thread parses independently with
its own `CEKMachine` + `ParseArena`.

```cpp
// Main thread:
auto* grammar = compiler.compile(ast);

// Worker threads:
void parse_worker(const CompiledGrammar* grammar,
                  const uint8_t* input, size_t len) {
    ParseArena arena(256 * 1024);   // Thread-local
    CEKMachine vm(&arena);          // Thread-local
    auto result = vm.execute(grammar, "Root", input, len);
    // Process result...
}
```

---

## 13. Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Arena allocation | O(1) | Bump pointer |
| Arena reset | O(1) | Single pointer assignment |
| Primitive match | O(1) | Fixed-size read + decode |
| Bytes/string match | O(n) | n = byte count |
| Environment lookup | O(d) | d = scope depth (typically 3–10) |
| Expression evaluation | O(e) | e = expression tree size (typically 3–7 nodes) |
| Backtracking | O(1) | Pointer restoration |
| Frame push/pop | O(1) | Arena allocation + pointer update |
| String comparison | O(1) | Interned pointer equality |
| Overall parse | O(n) | n = input size, single pass (modulo backtracking) |

**Memory overhead per parse:** ~256KB arena (typical). The arena grows on
demand for deeply nested formats but resets to zero after each parse.

---

## 14. Implementation Phases

Condensed from the original 13-phase plan, adapted for the actual BBQ AST and
Sema output.

### Phase 1: Arena + Core Types
- `ParseArena` — bump allocator with O(1) reset
- `KontNode` base struct + all node subtypes (§4)
- `Environment` — immutable linked list with `Binding`
- `Frame` hierarchy — `ReturnFrame`, `AlternativeFrame`, `LoopFrame`
- `CaptureNode` / `FieldCapture` / `CaptureBuilder`
- `StringPool` — interning for identifiers

### Phase 2: Trampoline + Primitive Matching
- `CEKMachine` struct with all state fields
- Trampoline loop (`while (control) control->invoke(this)`)
- `MatchPrimitive` — read uint8/16/32/64, int8/16/32/64, float32/64, bool
- `Halt` — successful termination
- Endianness-aware reads (LE/BE/Default)
- Bounds checking

### Phase 3: Struct + Field Capture
- `BeginStruct` / `EndStruct` — push/pop capture scope
- Field compilation chain — sequential field parsing
- Capture recording — `add_field()` with offsets and types
- Environment binding — each parsed field extends env

### Phase 4: Environment + Expressions
- `CompiledExpr` tree structure
- Expression evaluator — all operators, references, special variables
- `EvalConstraint` — evaluate where-clause, trigger failure
- `BindCompute` — evaluate expression, bind without consuming input
- Built-in functions (crc32, abs, min, max)

### Phase 5: Nonterminal Calls
- `InvokeRule` — push `ReturnFrame`, jump to rule entry
- `ReturnRule` — pop frame, record interval, restore caller env
- Interval application on rule references (`[start, end]`)

### Phase 6: Backtracking
- `fail()` — search for `AlternativeFrame`, restore state
- `BeginChoice` / `Backtrack` — union and alternatives
- Best-error tracking (furthest position)
- `BeginOptional` / `EndOptional` — try-parse with restore

### Phase 7: Arrays
- `BeginArray` / `ArrayNext` / `EndArray` — loop iteration
- `LoopFrame` — counter, limit, body entry
- Fixed-count arrays (`array<T>[N]`)
- Sep/term arrays (`array<T>(sep, term)`)
- `ArraySeparator` — parse separator between elements
- Terminator modes: EOF, count, until-expr, type-match
- Per-element intervals with loop variable `i`

### Phase 8: Switch + Optional
- `SwitchDispatch` — evaluate discriminator, jump to case
- Case matching: int, string, ident, dotted-ref
- Optional with constraint (conditional presence)

### Phase 9: Bitfield + Endian
- `BitfieldRead` — read container, extract bit fields
- MSB-first (BE) and LSB-first (LE) ordering
- Bind each entry value in environment
- `SetEndian` — runtime endianness switching

### Phase 10: External Parsers
- `ExternalCall` — dispatch to registered function
- `ExternalParserTable` — name → function registry
- `FunctionTable` — user expression functions

### Phase 11: CEK Compiler
- `CEKCompiler` class — AST visitor that emits continuation graphs
- Compilation rules from §5, one method per AST node type
- Expression compiler — `Expr*` AST → `CompiledExpr*` tree
- `CompiledGrammar` construction with string interning

### Phase 12: Expression Completion
- FieldAccess/IndexAccess evaluation via CaptureBuilder navigation
- Defensive null guards (InvokeRuleNode entry, execute_from arena)
- Compile/execute separation: `CEKCompiler::compile()` + `CEKMachine::execute_from()`

### Phase 13: Test Audit & Completion
- Hand-built MatchBytesNode, float, signed int, bitwise expression tests
- FieldAccess/IndexAccess tests (hand-built + compiler end-to-end)
- Conditional optional compiler test

---

## Appendix A: Complete BBQ AST → Continuation Node Mapping

For verification that every AST construct has a compilation rule:

| BBQ AST Node | §5 Rule | Continuation Nodes |
|-------------|---------|-------------------|
| `Grammar` | §5.1 | (orchestrates rule compilation) |
| `EndianDirective` | §5.1 | Sets `grammar->default_little_endian` |
| `Rule` | §5.2 | Entry point + body chain + `ReturnRule` |
| `Alternatives` | §5.4 | `BeginChoice` + per-alternative chains |
| `Struct` | §5.3 | `BeginStruct` + fields + `EndStruct` |
| `Union` | §5.4 | `BeginChoice` + per-variant chains |
| `Array` | §5.5–§5.7 | `BeginArray` + element + `ArrayNext` + `EndArray` |
| `Optional` | §5.8 | `BeginOptional` + inner + `EndOptional` (or conditional) |
| `Switch` | §5.9 | `SwitchDispatch` + per-case chains |
| `Compute` | §5.13 | `BindCompute` |
| `Primitive` | §5.10 | `MatchPrimitive` [+ `EvalConstraint`] |
| `RuleRef` | §5.12 | `InvokeRule` [+ `EvalConstraint`] |
| `Extern` | §5.15 | `ExternalCall` [+ `EvalConstraint`] |
| `Bitfield` | §5.14 | `BitfieldRead` |
| `EndianSwitch` | §5.16 | `SetEndian` |
| `Field` | §5.3 | (delegates to body's compilation) |
| `Variant` | §5.4 | (delegates to body's compilation) |
| `FixedCount` | §5.5 | `BeginArray(mode=FixedCount)` |
| `SepTerm` | §5.6 | `BeginArray(mode=SepTerm)` |
| `TypeSep` | §5.6 | `ArraySeparator` with type parse |
| `NoneSep` | §5.6 | (no separator node emitted) |
| `EofTerm` | §5.6 | EOF check in `ArrayNext` |
| `CountTerm` | §5.6 | Count check in `ArrayNext` |
| `UntilTerm` | §5.6 | Condition eval in `ArrayNext` |
| `TypeTerm` | §5.6 | Try-parse terminator in `ArrayNext` |
| `SwitchCase` | §5.9 | Case entry in `SwitchDispatch` |
| `SwitchDefault` | §5.9 | Default entry in `SwitchDispatch` |
| `IntValue` | §5.9 | Int comparison in dispatch |
| `StrValue` | §5.9 | String comparison in dispatch |
| `IdentValue` | §5.9 | Ident comparison in dispatch |
| `RefValue` | §5.9 | Dotted-ref comparison in dispatch |
| `StartEnd` | §5.3 | Position save/seek/restore around field |
| `Length` | §5.11 | Length expression in `MatchBytes` |
| `IntegerKind` | §5.10 | `MatchPrimitive` with width/sign/endian |
| `FloatKind` | §5.10 | `MatchPrimitive` with width/endian |
| `BytesKind` | §5.11 | `MatchBytes` |
| `StringKind` | §5.11 | `MatchBytes` (string variant) |
| `BoolKind` | §5.10 | `MatchPrimitive` (1-byte read) |
| `BinOp` | §6.2 | `CompiledExpr` binary node |
| `UnaryOp` | §6.2 | `CompiledExpr` unary node |
| `Ternary` | §6.2 | `CompiledExpr` ternary node |
| `IntLit` | §6.2 | `CompiledExpr::IntLit` |
| `FloatLit` | §6.2 | `CompiledExpr::FloatLit` |
| `StrLit` | §6.2 | `CompiledExpr::StrLit` |
| `BoolLit` | §6.2 | `CompiledExpr::BoolLit` |
| `Ref` | §6.2 | `CompiledExpr::Ref` / `FieldAccess` / `IndexAccess` |
| `Call` | §6.3 | `CompiledExpr::Call` |
| `EndianLit` | §6.2 | `CompiledExpr::IntLit` (0 or 1) |
| `EOI` | §6.2 | `CompiledExpr::EOI` |
| `Simple` | §6.2 | `CompiledExpr::Ref` |
| `FieldAcc` | §6.2 | `CompiledExpr::FieldAccess` |
| `IndexAcc` | §6.2 | `CompiledExpr::IndexAccess` |

---

## Appendix B: Design Decisions and Tradeoffs

### B.1 Continuations vs. Bytecode

Both approaches were considered during the original design. Continuations
(linked node graphs) were chosen over flat bytecode because:

- **No dispatch overhead** — direct function pointer calls, no opcode decoding.
- **Natural graph structure** — switch dispatch, backtracking alternatives, and
  nonterminal calls map naturally to graph edges.
- **Shared subgraphs** — multiple switch cases can share tail continuations
  without duplication.
- **Simpler compiler** — emit nodes and link them, no jump offset calculation.

The tradeoff is slightly higher memory usage (pointers between nodes) and
less cache locality than a flat bytecode array. For binary format parsing
workloads (where I/O dominates), this is negligible.

### B.2 Arena vs. GC

AiPL requires a generational GC because APL has persistent closures, mutable
arrays, and long-running computations. BBQ parsing is fundamentally different:

- All per-parse allocations are provably short-lived.
- No closures, no mutable arrays, no user-defined functions.
- Parse duration is bounded by input size.

Arena-only management eliminates GC pause times and GC bookkeeping entirely.
The O(1) reset is strictly superior for this use case.

### B.3 Immutable Environments vs. Mutable Symbol Tables

AiPL uses mutable `unordered_map`-based environments because APL requires
variable reassignment. BBQ's environments are immutable linked lists because:

- **Fields are write-once** — once parsed, a field's value never changes.
- **Backtracking is free** — restore an old environment pointer, done.
- **No hash table overhead** — linked list lookup is O(d) but d is small
  (typically 5–20 fields per struct).

### B.4 Two-Phase Continuations (Not Needed)

AiPL uses two-phase continuations extensively (e.g., `DyadicK` →
`EvalDyadicLeftK` → `ApplyDyadicK`) because APL expressions require
evaluating subexpressions before applying operators, and each subexpression
can itself be arbitrarily complex.

BBQ expressions are evaluated atomically by the stack-based evaluator — they
don't involve parsing sub-operations or control flow. Therefore the two-phase
pattern is unnecessary. Each BBQ continuation is single-phase: it does its
work and advances control.

The one exception is `InvokeRule`, which is inherently two-phase (push frame +
jump, then later return). But this is handled by the frame stack, not by
splitting into two continuation types.

### B.5 Visitor Pattern

AiPL uses a `ContinuationVisitor` with 70 `visit()` overloads for debugging,
optimization, and analysis. BBQ should adopt the same pattern:

```cpp
struct KontVisitor {
    virtual void visit(const MatchPrimitiveNode*) = 0;
    virtual void visit(const BeginStructNode*) = 0;
    // ... one per node type
};
```

This enables:
- **Pretty-printing** continuation graphs for debugging.
- **Optimization passes** (dead node elimination, constant folding) as
  separate visitors.
- **Graph serialization** for caching compiled grammars to disk.

### B.6 String Interning Scope

AiPL interns all strings globally (GC-managed, never freed). BBQ interns
strings in the `CompiledGrammar`'s string pool — they live as long as the
grammar and are freed when the grammar is released. Per-parse strings (error
messages) use the parse arena and are transient.

---

## Appendix C: Example Execution Trace

Parsing a simple TLV entry:

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
  Action: push capture scope "TLVEntry"

STEP 2: MatchPrimitive("type", uint8)
  pos=0 → read 1 byte → value=1
  pos=1, env={type: 1, [0,1), UInt8}
  Action: capture field "type" [0,1)

STEP 3: MatchPrimitive("length", uint16le)
  pos=1 → read 2 bytes LE → value=5
  pos=3, env={length: 5, [1,3), UInt16LE} → {type: ...}
  Action: capture field "length" [1,3)

STEP 4: MatchBytes("data", interval=eval(length)=5)
  pos=3 → read 5 bytes
  pos=8, env={data: 5, [3,8), Bytes} → {length: ...} → {type: ...}
  Action: capture field "data" [3,8)

STEP 5: EndStruct
  Action: close capture scope "TLVEntry" [0,8)

STEP 6: Halt
  control=nullptr, success=true

Result CaptureMetadata:
  TLVEntry [0, 8)
    type:   [0, 1) UInt8
    length: [1, 3) UInt16LE
    data:   [3, 8) Bytes
```

---

## Appendix D: Source References

The IPG column references the original design docs (`/home/dan/Source/IPG/`).
The AiPL column references the working CEK machine (`/home/dan/Source/AiPL/`)
that validates the patterns.

| Concept | Original Design Docs (IPG/) | AiPL Implementation |
|---------|----------------------------|---------------------|
| Three-tier memory | `IPG Virtual Machine Design.md` §Memory Management | `machine.h` arena + GC |
| Continuation types | `IMPLEMENTATION_PLAN.md` §Continuation Types | `continuations.h` (70 types) |
| Trampoline loop | `IPG Virtual Machine Design.md` §Big-Step Evaluation | `machine.cpp:142-229` |
| Capture metadata | `IPG Virtual Machine Design.md` §Capture Metadata Output | — |
| DDCG compilation | `OperationalSemantics.md` §Compilation Rules | — |
| Environment as index | `IMPLEMENTATION_PLAN.md` Phase 11 | `environment.h` |
| Backtracking | `IPG Virtual Machine Design.md` §Backtracking Mechanism | — |
| Visitor pattern | — | `continuation_visitor.h` |
| Two-phase continuations | — | `continuations.h` DyadicK pattern |
| String interning | — | `string_pool.h` |
| Completion records | `IPG Virtual Machine Design.md` §Error Handling | `completion.h` |
| Arena sizing | — | `machine.h` ARENA_SIZE=65536 |
| Operational semantics | `IPG Formal Operational Semantics.md` | — |
| Thread safety | `IPG Virtual Machine Design.md` §Thread-Safety Model | — |
| Expression evaluation | `IPG Virtual Machine Design.md` §Expression System | `expression_evaluator.cpp` |
| External parsers | `IPG Virtual Machine Design.md` §External Parser Interface | — |
| PEG VM patterns | — | `../Peggy/Operational Semantics.txt` |

**Peggy** (`/home/dan/Source/Peggy/`) is a PEG parser generator whose
operational semantics document (`Operational Semantics.txt`) defines the
bytecode VM instructions (`OP_CHOICE`, `OP_COMMIT`, `OP_PARTIAL_COMMIT`,
`OP_JUMP`) that informed the `CommitChoiceNode` pattern (§7.1) and the
optimization patterns in §7.3. The key theoretical insight — that DDCG
destinations *are* continuations — connects PEG compilation theory directly
to the CEK machine architecture.
