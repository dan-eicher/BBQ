```
DDCGC(1)             DDCG Compiler-Compiler              DDCGC(1)
```

## NAME

**ddcgc** -- Destination-Driven Code Generation compiler-compiler

## SYNOPSIS

```
ddcgc -i spec.ddcg -ast ast.json -ir ir.json
      [-lang c|c++] [-L libdir]... [-o out.h]
```

## DESCRIPTION

ddcgc compiles `.ddcg` specifications into C or C++ dispatchers
that walk a typed source AST and produce a target IR.
The technique is Dybvig, Hieb & Butler's *Destination-Driven Code
Generation* (Indiana CS TR-302, 1990): each AST node is compiled
under an explicit data destination δ (where the value goes) and
control destination γ (where control transfers next), recursing
into sub-expressions with new δ/γ as appropriate, composing the
result via user-supplied auxiliaries. ρ (the paper's environment
component) is folded into the generated class itself.

The DSL is a thin pure-transformation tree-walker. It does not plan,
score, or rewrite — that is `burgc`'s job, downstream of ddcgc in
BBQ's canonical compile pipeline (parser → AST → ddcgc → IR → burgc
→ target). Branching at the generator level is expressed as multiple
rules with disjoint `where` guards, never as imperative control flow
inside a rule body. Branching at runtime lives in the consumer's
interpretation of γ (jump, branch, restore, return, etc.).

Generated shape (C++ mode):

```
namespace <COMPILER name> {
    class Compiler {
    public:
        <MEMBERS verbatim>                     // user state + aux/pred bodies
        <ir_root> compile_<sum>(<Sum>* node,
                                <delta>_t d,
                                <gamma>_t g);
        ...
    };
}
```

C mode emits a `<COMPILER>_ctx_t` struct holding MEMBERS data plus
free functions taking `<COMPILER>_ctx_t* ctx` as their first
parameter; aux/pred bodies live in a separate `.c` file the
consumer ships, signature-matched against the AUXILIARIES /
PREDICATES declarations.

The runtime author defines `<delta>_t`, `<gamma>_t`, their variant
constructor functions, and the source/target schemas at namespace
scope; everything else (per-compile state, aux/pred bodies,
`fresh_label()`, `current_loc`) lives in the user's MEMBERS block
(C++) or split across MEMBERS-as-data + a sibling `.c` (C).

## OPTIONS

**-i** *file*
: Input `.ddcg` file. Required.

**-ast** *file*.json
: ASDL JSON sidecar for the source AST schema. Matched positionally
  against the .ddcg's first IMPORTS entry. Required.

**-ir** *file*.json
: ASDL JSON sidecar for the target IR schema. Matched against the
  second IMPORTS entry. Required.

**-lang** `c`|`c++`
: Output language. Default `c++`. C++ mode produces a class-wrapped
  emit (`class Compiler`) where MEMBERS holds both data fields and
  aux/pred bodies. C mode produces a `BurgContext`-style struct +
  static functions taking `BurgContext* ctx`; aux/pred bodies live
  in a separate `.c` file the consumer ships, and MEMBERS is data
  fields only. Both modes share frontend, typecheck, and rule
  emission via the EmitBackend Template Method base.

**-L** *dir*
: Add a library search path for `import "...ddcg"` clauses.
  Repeatable; directories are searched in declaration order after
  the importing file's own directory. Used by consumers that pull
  shared destination/aux declarations from `ddcgc/lib/` (e.g.
  `dybvig.ddcg`).

**-o** *file*
: Output file (typically `.h` since the emit is class-wrapped with
  inline method bodies). Default: stdout.

**-h**
: Show help.

## DSL FORMAT

```
COMPILER name                                  // becomes namespace + class

(. #include "...header..." .)                  // optional verbatim headers

IMPORTS
  ast    = "src.json"                          // first import: source AST
  target = "ir.json"                           // second import: target IR

DESTINATIONS
  data_dest delta { effect | ac | stack(slot: int) }   // δ — required
  ctrl_dest gamma { fail | jump(target: label) }       // γ — required
  env_dest  rho   { top                                // ρ — optional
                  | in_scope(label: int, parent: rho)  //   may self-reference
                  }                                    //   (recursive ρ)
  sum demo_op { OpNop | OpStep(delta: int) }   // user-defined closed sum;
                                               //   same machinery as δ/γ/ρ
                                               //   but doesn't enter the
                                               //   dispatcher signature
  ir_root   <T>                                // primary aux return type

AUXILIARIES
  cg_x : (int, ...) -> <T>                     // typed signatures only;
  ...                                          //   bodies live in MEMBERS
  on_dispatch_result : (int) -> int            // optional hook: every
                                               //   dispatcher wraps its
                                               //   result through this aux

PREDICATES                                     // optional; bool-return only
  is_zero : (int) -> bool

MEMBERS (.
    // C++ mode: verbatim C++; everything callable from rule bodies
    // must be declared in AUXILIARIES or PREDICATES with matching
    // signature. C mode: data fields only — aux/pred bodies live
    // in the consumer's `.c` file as free functions taking the
    // emitted `<COMPILER>_ctx_t* ctx` first arg.
    using label_t = int;
    SourceLoc current_loc;
    int next_label = 0;
    label_t fresh_label() { return next_label++; }
    int cg_x(int n) { ... return ...; }
    bool is_zero(int n) { return n == 0; }
.)

import "lib.ddcg"              // optional; splices a library's
                               //   DESTINATIONS / AUXILIARIES /
                               //   funs into this file (resolved
                               //   via the importing file's
                               //   directory then `-L` paths)

// Action-language funs — pure typed helpers callable from rule
// bodies and other funs. Body is statements terminated by a tail
// expression whose type matches the declared return type.
fun helper(arg : int) -> int {
    let doubled = arg + arg;
    doubled;
}

DESUGARED                      // optional; exempts source
  CtorA, CtorB                                  //   constructors from the
                                                //   coverage check

RULES

rule name : module.Ctor(field: pat, ...) [where guard] { body }
...
```

### Rules

```
rule <name> : <pattern> [, <pattern>]* [where <expr>] { <stmt>* }
```

A rule is one or more comma-separated head patterns sharing a body.
Multi-head rules must bind the same variables with the same types
across heads (Treecc-style). The `where` clause is a bool-typed
expression evaluated against pattern-bound variables only. Body is a
sequence of statements; the tail expression's type must match the
declared `ir_root`.

### Patterns

| Form | Meaning |
|---|---|
| `module.Ctor(field: subpat, ..., ...rest)` | constructor + named field patterns |
| `name` | bind to a fresh local with the field's declared type |
| `_` | wildcard — no bind, no check |
| `nil` | matches when the field is `nullptr` |
| `42`, `"x"` | literal value-equality match |
| nested `module.Ctor(...)` in a field position | recursive constructor match |

### Statements

| Form | Notes |
|---|---|
| `let v = expr;` | bind, type inferred from RHS |
| `let v: T = expr;` | bind with explicit annotation (used for empty-list typing) |
| `let (a, b) = expr;` | tuple destructuring |
| `v := expr;` | mutate a previously `let`-bound variable |
| `for v in xs { ... }` | iterate a list-typed expression |
| `for (i, v) in xs { ... }` | indexed iteration; `i` is a fresh int bound to the position |
| `if cond { ... } else { ... }` | conditional statement; both branches scope locally |
| `label L;` | bind a fresh label via `fresh_label()` |
| `expr;` | expression statement; in tail position becomes `return expr;` |

### Expressions

Standard precedence cascade: ternary, `or`/`and`, `==`/`!=`,
`<`/`>`/`<=`/`>=`, `+`/`-`, `*`/`/`/`%`, unary `not`/`-`, postfix
calls/`.field`/`[i]`. Plus:

| Form | Notes |
|---|---|
| `[a, b, c]` | list literal; element type is the join of arm types (sibling schema-ctors widen to their parent sum) |
| `(a, b)` | tuple literal |
| `nil` | matches an unset optional schema field; in `build` to an Optional field emits `std::nullopt` |
| `restore(0)`, `effect` | dest-variant calls (typed or zero-arg) |
| `xs.count` | list size (lowers to `xs.size()` in C++, `xs.count` in C) |
| `xs[i]` | list element access |
| `e.field` | schema field / tuple field access |
| `match (e) { Pat => arm | ... }` | match-as-expression; subject can be a dest tagged union, asdl enum, int, or non-enum schema sum |
| `cond ? a : b` | ternary; arm types widen via sibling-ctor → parent-sum |
| `{ stmts; tail; }` | block expression; lowers to a stmt-expr (C) or IIFE (C++) |
| `build module.Ctor(args...)` | construct a target IR value via the schema's factory |
| `gen(subject, ρ, δ, γ, [Lnext])` | recursive code-generation call into the auto-generated dispatcher |

`+` between two list-typed operands lowers to a generated
`_ddcgc_concat` helper.

## ANALYSIS

### Type-check (always)

Every expression and binding is typed against the loaded schemas
plus the AUXILIARIES / PREDICATES / DESTINATIONS declarations.
Pattern field types come from the asdl JSON; aux/pred return types
must match where they are used. Aux/pred call arity and arg types
are checked. Dest-variant calls validate against the declared
variant payload shape. List/tuple element types propagate
structurally; an empty list literal picks up its element type from
either an explicit `let` annotation or from the expected type at a
`build`/aux call site (whichever fires first via the
`refine_polymorphic` pass).

### Sibling-constructor widening

Two distinct constructors of the same asdl sum widen to the parent
sum at join points: ternary branches (`cond ? build mod.A(...) :
build mod.B(...)` types as `mod.parent`), match-arm result types,
and list literal element types (`[build mod.A(...), build
mod.B(...)]` types as `list<mod.parent>`). The codegen emits
explicit upcasts on each ternary arm so C++ compiles without
manual `static_cast`s.

### Where-guard purity (always)

`where` expressions must be side-effect-free. The body may use
PREDICATES (declared pure by signature), pure operators, literals,
let/pattern/fun-param-bound idents, dest-pattern construction,
field/index access, and tuple/list collection of pure values.
AUXILIARIES and `fun` calls are rejected — guards run on every
rule-try regardless of whether the rule wins, so an aux that
allocates or mutates state would fire spuriously. Two `gtest`
fixtures cover the positive and negative paths.

### Coverage (always)

Every constructor in the source-AST schema (the .ddcg's first
IMPORTS entry) must be the head of at least one rule, unless listed
in `DESUGARED`. Missing constructors are errors.

### Multi-pattern uniformity (always)

A rule with multiple comma-separated heads must bind the same set
of variables with the same types across all heads. A body
referencing a variable bound differently in different heads would
silently miscompile, so it errors at type-check time.

### Overlap (warning) and duplicates (error)

Two rules sharing a head pattern (modulo bind-variable names) are
flagged as overlapping when at least one is unguarded — a hint to
audit that the `where` guards are actually disjoint. When two rules
have identical heads *and* identical guard expressions, they are
duplicates and reported as errors (PEG ordered choice would make
the second unreachable).

## DESTINATIONS

ddcgc requires exactly one `data_dest` and one `ctrl_dest`. An
`env_dest` (paper's ρ) and any number of user-defined `sum`s are
optional. All four are named tagged unions with the same shape:
variants may be zero-arg (`effect`, `ac`, `fail`, `anon`) or carry
typed payloads (`stack(slot: int)`, `restore(save: int)`,
`jump(target: label)`, `in_scope(name: ident)`).

The runtime author provides `<dest>_t` and one constructor function
per variant at namespace scope; ddcgc emits zero-arg references as
constructor calls (`effect()`) and typed-payload references as the
obvious call (`stack(0)`). User `sum`s share this machinery — same
match dispatch and constructor-call shape — but their type doesn't
appear in the dispatcher signature, so they're useful for
defunctionalising a finite "what to do" tag set inside rule bodies
without touching the δ/γ/ρ contract.

`env_dest` carries compilation-context state (paper's ρ — variable
scope, current-field name, loop/finally frames, etc.). Save/restore
patterns the consumer would otherwise maintain via mutable MEMBERS
state are expressed by extending ρ at the recursive-call site:
```
chain = compile(body, in_scope(field_name), δ, γ, Lnext);
```
The previous ρ at the call site is unchanged when the recursion
returns — recursion's lexical scope handles the implicit "restore."
Rule bodies and funs read scope state by matching on ρ.

### Recursive env_dest (chained scopes)

A variant of `env_dest` may carry a self-typed field
(`parent: rho`); ddcgc treats that as a recursive type. The C
backend arena-allocates the parent pointer so the recursion's
lifetime survives the call chain; match arms binding the parent
field auto-deref to a `rho_t*`. Walking the chain is just
recursive matching — no MEMBERS-side stack push/pop, no forgot-to-
pop bug class. The `scoped` test fixture
(`tests/data/scoped.{ddcg,...}`) is the ground-check for this
shape end-to-end.

The `gen(subject, ρ, δ, γ, [Lnext])` call type-checks δ against the
declared `data_dest`, γ against `ctrl_dest`, and ρ against `env_dest`.
The optional fifth argument is the paper's `Lnext` — the next-statement
fall-through label for redundant-jump elimination — passed verbatim
to the recursive dispatch.

## BACKENDS

The `EmitBackend` Template Method base owns the shared visitor
(emit_expr / emit_stmt / emit_rule_branch and helpers) plus the
language-agnostic pre-emit setup. `CppBackend` and `CBackend` each
implement ~12 per-form hooks where the two languages diverge:

| Concern | C++ | C |
|---|---|---|
| Class | `class Compiler { ... }` with MEMBERS | `<comp>_ctx_t` struct + free fns taking `<comp>_ctx_t*` |
| Aux/pred bodies | inline in MEMBERS | free fns in user's `.c` |
| Container types | `std::vector<T>`, `std::optional<T>` | per-shape struct helpers (`<elem>_list_t`) emitted by a discovery pass |
| Match dispatch | `dynamic_cast<T*>` chain (C++) | `switch (n->tag)` against the asdl-c tagged union |
| Match-as-expr | IIFE lambda | stmt-expr + result var |
| Block expr | IIFE | GCC stmt-expr |
| String equality | `==` (std::string overload) | null-safe pointer-eq + `strcmp` |
| `list.count` | `.size()` | `.count` field |
| asdl factories | `new T(args)` | asdl-c `<mod>_<ctor>(arena, args)` |

A new AST form is one dispatch case in `emit_backend` plus the two
per-language hooks. Adding a third C-family backend would be the
same pattern: subclass `EmitBackend`, override the hooks, factory
into `main.cpp`.

## EXAMPLES

Compile a tiny .ddcg against its schemas:

```
asdl  -i src.asdl --json src.json
asdl  -i ir.asdl  --json ir.json
ddcgc -i mylang.ddcg -ast src.json -ir ir.json -o MyCompiler.h
```

Use the generated class (C++):

```
#include "MyCompiler.h"
mylang::Compiler compiler;
auto result = compiler.compile_expr(&root, mylang::ac(), mylang::fail());
```

C-mode emit + asdl-c-emitted schemas + per-arena state:

```
asdl  -i src.asdl --json src.json -lang c -o src_ast.h
asdl  -i ir.asdl  --json ir.json  -lang c -o ir_ast.h
ddcgc -i mylang.ddcg -ast src.json -ir ir.json -lang c -o mylang_compile.h
```

## FILES

```
ddcgc/
  grammar/
    ddcg.peg                  Self-hosted grammar (authoritative)
    ddcg.asdl                 DSL AST schema
    ddcg.json                 ASDL JSON sidecar (auto-generated)
    frames/
      Parser.frame            pegc frames for the parser
      ParserImpl.frame
  frontend/
    Parser.h, Parser.cpp      Generated parser (from ddcg.peg)
    ddcg_ast.h                Generated DSL AST (from ddcg.asdl)
  lib/
    dybvig.ddcg               Reusable destinations + auxes for the
                              paper-direct DDCG shape; consumers
                              `import` it via `-L ddcgc/lib`
  src/
    schema.h, .cpp            asdl JSON loader
    typecheck.h, .cpp         Type system + coverage + overlap + duplicate
                              + sibling-ctor widening + where-guard
                              purity + polymorphic refinement
    backend.h                 Pluggable backend interface
    emit_backend.h, .cpp      Template Method base — shared visitor
                              (emit_expr / emit_stmt / emit_rule_branch)
                              + per-language hooks
    cpp_backend.cpp           C++ subclass — class-wrapped emitter
    c_backend.cpp             C subclass — context-struct + free
                              functions; uses asdl-c factories
    main.cpp                  CLI driver
  tests/
    ddcgc_test.cpp            Unit tests (parser, schema, typecheck,
                              codegen, destinations, patterns,
                              guard purity)
    data/
      tiny.ddcg               Minimal end-to-end fixture
      big.ddcg                C++ self-reporting integration probe;
                              each rule calls trace_fire("name") and
                              the driver verifies every catalogued
                              feature fires
      kitchensink.ddcg        C-mode integration probe — exercises
                              every emitter form against asdl-c
                              factories
      scoped.ddcg             Recursive env_dest fixture (chained
                              scopes via `parent: rho` self-reference)
      paper_consumer.ddcg     Imports `dybvig.ddcg` to validate the
                              shared-library import path
```

## SEE ALSO

- `asdl(1)` -- ASDL processor; `--json` produces the sidecars
  consumed by `-ast` and `-ir`.
- `pegc(1)` -- generates the parser ddcgc uses for `.ddcg` files.
- `burgc(1)` -- the lowering stage downstream of ddcgc; consumes
  the IR ddcgc produces and rewrites/lowers it via cost-based
  tree-pattern matching.
- *Destination-Driven Code Generation*, Dybvig, Hieb & Butler,
  Indiana University Computer Science Department, Technical Report
  #302, February 1990.
