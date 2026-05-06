```
DDCGC(1)             DDCG Compiler-Compiler              DDCGC(1)
```

## NAME

**ddcgc** -- Destination-Driven Code Generation compiler-compiler

## SYNOPSIS

```
ddcgc -i spec.ddcg -ast ast.json -ir ir.json [-o out.h]
```

## DESCRIPTION

ddcgc compiles `.ddcg` specifications into class-wrapped C++
dispatchers that walk a typed source AST and produce a target IR.
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

Generated shape:

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

The runtime author defines `<delta>_t`, `<gamma>_t`, their variant
constructor functions, and the source/target schemas at namespace
scope; everything else (per-compile state, aux/pred bodies,
`fresh_label()`, `current_loc`) lives in the user's MEMBERS block.

## OPTIONS

**-i** *file*
: Input `.ddcg` file. Required.

**-ast** *file*.json
: ASDL JSON sidecar for the source AST schema. Matched positionally
  against the .ddcg's first IMPORTS entry. Required.

**-ir** *file*.json
: ASDL JSON sidecar for the target IR schema. Matched against the
  second IMPORTS entry. Required.

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
  env_dest  rho   { anon | in_scope(name: ident) }     // ρ — optional
  ir_root   <T>                                        // primary aux return type

AUXILIARIES
  cg_x : (int, ...) -> <T>                     // typed signatures only;
  ...                                          //   bodies live in MEMBERS

PREDICATES                                     // optional; bool-return only
  is_zero : (int) -> bool

MEMBERS (.
    // verbatim C++; everything callable from rule bodies must be
    // declared in AUXILIARIES or PREDICATES with matching signature
    using label_t = int;
    SourceLoc current_loc;
    int next_label = 0;
    label_t fresh_label() { return next_label++; }
    int cg_x(int n) { ... return ...; }
    bool is_zero(int n) { return n == 0; }
.)

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
| `label L;` | bind a fresh label via `fresh_label()` |
| `expr;` | expression statement; in tail position becomes `return expr;` |

### Expressions

Standard precedence cascade: ternary, `or`/`and`, `==`/`!=`,
`<`/`>`/`<=`/`>=`, `+`/`-`, `*`/`/`/`%`, unary `not`/`-`, postfix
calls/`.field`/`[i]`. Plus list literals `[a, b, c]`, tuple
literals `(a, b)`, `nil`, dest-variant calls (e.g. `restore(0)`,
or bare `effect` for zero-payload variants), and `gen(subject, δ,
γ, [Lnext])` — the recursive code-generation call. `+` between two
list-typed operands lowers to a generated `_ddcgc_concat` helper.

## ANALYSIS

### Type-check (always)

Every expression and binding is typed against the loaded schemas
plus the AUXILIARIES / PREDICATES / DESTINATIONS declarations.
Pattern field types come from the asdl JSON; aux/pred return types
must match where they are used. Aux/pred call arity and arg types
are checked. Dest-variant calls validate against the declared
variant payload shape. List/tuple element types propagate
structurally; an empty list literal bound to a typed variable picks
up the annotation's element type.

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
`env_dest` (paper's ρ) is optional. All three are named tagged
unions with the same shape: variants may be zero-arg (`effect`,
`ac`, `fail`, `anon`) or carry typed payloads (`stack(slot: int)`,
`restore(save: int)`, `jump(target: label)`, `in_scope(name: ident)`).

The runtime author provides `<dest>_t` and one constructor function
per variant at namespace scope; ddcgc emits zero-arg references as
constructor calls (`effect()`) and typed-payload references as the
obvious call (`stack(0)`).

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

The `gen(subject, ρ, δ, γ, [Lnext])` call type-checks δ against the
declared `data_dest`, γ against `ctrl_dest`, and ρ against `env_dest`.
The optional fifth argument is the paper's `Lnext` — the next-statement
fall-through label for redundant-jump elimination — passed verbatim
to the recursive dispatch.

## EXAMPLES

Compile a tiny .ddcg against its schemas:

```
asdl  -i src.asdl --json src.json
asdl  -i ir.asdl  --json ir.json
ddcgc -i mylang.ddcg -ast src.json -ir ir.json -o MyCompiler.h
```

Use the generated class:

```
#include "MyCompiler.h"
mylang::Compiler compiler;
auto result = compiler.compile_expr(&root, mylang::ac(), mylang::fail());
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
  src/
    schema.h, .cpp            asdl JSON loader
    typecheck.h, .cpp         Type system + coverage + overlap + duplicate
    backend.h                 Pluggable backend interface (one of two
                              planned; C deferred)
    cpp_backend.h, .cpp       C++ class-wrapped emitter
    main.cpp                  CLI driver
  tests/
    ddcgc_test.cpp            Unit tests (parser, schema, typecheck,
                              codegen, destinations, patterns)
    data/
      tiny.ddcg               Minimal end-to-end fixture
      big.ddcg                Self-reporting integration probe; each
                              rule calls trace_fire("name") and the
                              driver verifies every catalogued feature
                              fires
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
