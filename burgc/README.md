```
BURGC(1)                BURG Code Generator                 BURGC(1)
```

## NAME

**burgc** -- BURG (Bottom-Up Rewrite Grammar) code generator with
unified completeness analysis

## SYNOPSIS

```
burgc -i grammar.burg [options]
```

## DESCRIPTION

burgc compiles `.burg` BURG specifications into standalone C++ or C
matchers that perform tree-pattern matching with cost minimization.
Generated matchers implement Aho-Ganapathi-Tjiang dynamic-programming
labeling: each node gets a state vector of best (cost, rule) per
nonterminal, and a tree rewrite phase (with optional semantic
actions) consumes the labeled tree.

Each `.burg` file declares terminals, an optional start nonterminal,
and a set of rules of the form `nonterm: pattern = cost (. action .)`.
Rules with the lowest accumulated cost win; chain rules (leaf
non-terminal patterns) propagate matches between nonterminals.

burgc additionally runs a unified semantic analysis over each grammar:

- **Duplicate-rule detection** flags rules with identical
  `(nonterm, pattern, guard)` as errors.
- **Top-down liveness analysis** flags rules whose LHS is never
  demanded as a child of a START-rooted derivation as dead.
- **Brandner-style completeness** (opt-in) enumerates concrete tree
  shapes no rule covers and reports them as warnings. Optionally
  consumes an ASDL JSON sidecar (`-asdl`) to make the enumeration
  precise instead of heuristic.

## OPTIONS

**-i** *file*
: Input `.burg` grammar file. Required.

**-o** *file*
: Output file. For C++, writes a single `.h`. For C, writes a
  `.h` plus a sibling `.c`. If omitted along with `-frames`,
  burgc runs analysis only and skips codegen.

**-lang** *c++* | *c*
: Target language. Default: `c++`. C output is pure C11 using the
  `bbq_arena` / `bbq_htree` runtime from `crt/`.

**-frames** *dir*
: Directory containing frame templates (`CppBurg.frame`, `CBurg.frame`,
  `CBurgImpl.frame`). Required for codegen.

**-asdl** *file*.json
: ASDL JSON sidecar produced by `asdl --json`. When supplied,
  completeness analysis uses precise per-position type demand from
  the IR's constructor schema instead of a heuristic derived from
  the rule set alone. See **ASDL INTEGRATION** below.

**--strict**
: Treat warnings (uncovered shapes, dead rules) as errors. Use for
  CI gating.

**--coverage**
: Enable shape-enumeration warnings. Off by default — without an
  ASDL schema these can surface IR-impossible shapes as false
  positives.

**-h**
: Show help.

## GRAMMAR FORMAT

```
TERM Name = N             // numeric ID
TERM Name = NS::CONST     // qualified symbolic ID

NAMESPACE my_ns           // optional: wrap C++ output in namespace

(. #include "..." .)      // optional: header injection

MEMBERS (. ... .)         // optional: extra public members on
                          //   BurgMatcher (C++) or fields on
                          //   burg_ctx_t (C)

PRIVATE (. ... .)         // optional: helper code that is private
                          //   to the matcher class (C++) or
                          //   file-local in the .c (C)

START nonterm             // optional: explicit start nonterminal
                          //   (default: first nonterm seen)

RULES

nt: pattern [where (. predicate .)] = cost [(. action .)] ;
...
```

### Rules

A rule has shape:

```
nonterm: pattern [where (. C predicate .)] = cost [(. C action .)];
```

- **`pattern`** is a tree pattern: a leaf nonterm, a leaf terminal,
  or `Term(child, child, ...)` recursively.
- **`where`** clauses are opaque C predicates. Treated as may-fire
  by the analysis (any guarded rule contributes its bit).
- **`cost`** is a non-negative integer added when this rule fires.
  Lower-cost matches win.
- **`action`** is opaque C code emitted into the reducer.

### Chain Rules

A rule whose pattern is a single leaf nonterminal is a chain rule:

```
reg: ireg = 0;     // promotes any ireg-derivation to a reg-derivation
```

Chain rules close transitively in the matcher state.

## ANALYSIS

burgc runs three checks on every grammar.

### Duplicate Rules (error)

Two rules with identical `(nonterm, pattern, guard text)` cannot
both fire — the matcher would arbitrarily pick one. Reported as a
hard error so collisions don't silently miscompile.

### Dead Rules (warning, always on)

A rule R is dead if its LHS nonterminal is never demanded as a
child position of any START-rooted derivation. Bottom-up
reachability says R produces *something*; top-down liveness says
*nobody wants what R produces*.

Computed by building the spec automaton (BURG's own runtime DP,
evaluated symbolically with cost erased) and walking demand
backward from states where `rule[start_nt]` fires.

### Uncovered Tree Shapes (warning, opt-in via `--coverage`)

For every (terminal, child-state-tuple) consistent with the
grammar's declared arities, check whether at least one rule
matches. Combinations landing in the empty state are concrete
witnesses for "your IR could produce this shape and your grammar
won't match it".

Off by default because without an ASDL schema (next section), the
enumeration emits false positives on shapes that combinatorially
exist but the consumer's IR could never construct.

## ASDL INTEGRATION

When `-asdl <file>.json` is supplied, burgc loads the JSON sidecar
emitted by `asdl --json` and uses the IR's constructor schema to
drive the per-position demand filter precisely.

For an AST-shaped IR — where each ASDL constructor's `node`-typed
fields directly correspond to the BURG pattern's children — the
schema-aware filter enumerates only constructors that produce the
expected node type at each position. Missing-rule warnings become
real coverage gaps, not combinatorial noise.

For slot-based IRs (where BURG operands are virtual, e.g. slot
indices, and the ASDL's `node` fields describe control-flow edges
instead), the schema's node-field count won't match the BURG
arity. burgc detects the mismatch and falls back to its heuristic
demand filter for that terminal — the schema is consulted but
doesn't constrain.

## EXAMPLES

Basic codegen:

```
burgc -i myrules.burg -frames burgc/frames -o MyMatcher.h
```

CI-gated analysis (no codegen):

```
burgc -i myrules.burg --strict --coverage
```

End-to-end with ASDL:

```
asdl  -i ir.asdl --json ir.json
burgc -i myrules.burg -asdl ir.json -frames burgc/frames -o Matcher.h
```

## FILES

```
burgc/
  grammar/
    burg.peg                  Self-hosted grammar (authoritative)
    frames/
      Parser.frame            pegc frames for the parser
      ParserImpl.frame
  frontend/
    Parser.h, Parser.cpp      Generated parser (from burg.peg)
    BurgAST.h                 AST definition (handwritten)
  src/
    generator.h, .cpp         Frontend + shared backend helpers
    completeness.h, .cpp      Unified semantic analysis
    cpp_backend.cpp           C++ matcher emitter (BurgMatcher class)
    c_backend.cpp             C matcher emitter (burg_ctx_t struct)
    main.cpp                  CLI driver
  frames/
    CppBurg.frame             C++ class scaffold
    CBurg.frame               C header scaffold
    CBurgImpl.frame           C impl scaffold
  tests/
    burgc_test.cpp            All burgc tests (gtest)
    data/
      simple.burg, chain.burg, actions.burg, costs.burg
```

## SEE ALSO

- `asdl(1)` -- ASDL processor; `--json` produces the sidecar
  consumed by `-asdl`.
- `pegc(1)` -- generates the parser burgc uses for `.burg` files.
- `lib/Worklist.h` -- the discover-as-you-go worklist primitive
  the completeness analysis uses.
