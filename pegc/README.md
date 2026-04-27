```
PEGC(1)                     PEG Compiler                     PEGC(1)
```

## NAME

**pegc** -- PEG parser generator for C++

## SYNOPSIS

```
pegc [options] grammar.peg
```

## DESCRIPTION

pegc compiles `.peg` grammar specifications into standalone C++ parsers.
The generated parsers implement PEG (Parsing Expression Grammar) semantics
via recursive descent: each production becomes a `parse_X()` method, ordered
choice backtracks via `save()`/`restore()`, and `&`/`!` predicates provide
unlimited lookahead. There is no separate lexer stage (scanner-less).
Generated parsers inherit from `peg::PegCursor`, which provides cursor-based
parsing, whitespace+comment skipping, and built-in token functions.

pegc is self-hosting: it compiles its own grammar (`peg.peg`), and regenerating
produces identical output (fixpoint).

## OPTIONS

**-o** *dir*
: Output directory for generated files. Default: `.`

**-frames** *dir*
: Directory containing frame template files (`Parser.frame`, `ParserImpl.frame`).
  Default: `./frames`

**-prefix** *name*
: Filename prefix for generated files. Default: derived from grammar filename
  (e.g., `expr.peg` produces `exprParser.h` and `exprParser.cpp`).

**-namespace** *ns*
: Wrap generated code in a C++ namespace.

**-trace** *flags*
: Diagnostic output. Flags: **A** (AST dump), **P** (generated file paths),
  **N** (analysis info: production dependencies, first-set testability).

**--strict**
: Treat analysis warnings as errors. Use for CI gating.

**--coverage**
: Enable shape-comparison checks: ordered-choice masking (alternatives
  whose FIRST sets are subsumed by an earlier alternative) and
  Star/Plus + suffix conflicts (greedy repetition that consumes what
  the suffix needs). Off by default — these fire on legitimate
  shared-prefix alternatives in real grammars where PEG correctly
  backtracks, so the warnings are informational.

## ANALYSIS

pegc runs a unified semantic-analysis pass on every grammar:

- **Duplicate / left-recursive productions** — errors.
- **Undeclared identifiers** — error if a `RuleCall` references a
  name that isn't a declared production, charset, token, or pegc
  built-in (`ident`, `integer`, `qualified_ident`, `string_lit`,
  `char_lit`, `scan_to`, `match`, `keyword`, `at_end`,
  `match_charset`, `peek_at`, `peek_charset`, `peek_keyword`,
  `floating`).
- **Always-failing productions** — warning when a production has
  empty FIRST and is not nullable (it can never match).
- **Useless lookahead predicates** — warning when `&X` always
  succeeds (X nullable) or `!X` always fails (X nullable) /
  always succeeds (X has empty FIRST and isn't nullable).
- **Unused tokens / charsets** — warning when declared but never
  referenced by any production or other token.
- **Unreachable productions** — warning (existing).

FIRST and FOLLOW sets are computed via Kildall worklist iteration
using `lib/Worklist.h`, propagating until quiescent. Nullable is
tracked alongside FIRST as `ε ∈ FIRST(P)`. The reverse call graph
is built once; FIRST changes re-process callers, FOLLOW changes
re-process callees.

`--coverage` adds two more checks gated behind opt-in (real
grammars use these patterns intentionally and the warnings are
noisy without further annotation):

- **Ordered-choice masking** — alternative *j* of a Choice may be
  masked by an earlier alternative *i* if FIRST(i) ⊇ FIRST(j).
  PEG can backtrack after a partial commit, so this is informational
  only.
- **Star/Plus + suffix conflict** — `Star(B) suffix` where
  FIRST(suffix) ⊆ FIRST(B). The greedy repetition will consume
  what the suffix needs to match.

## GRAMMAR FORMAT

A `.peg` file has this overall structure:

```
COMPILER Name

(. #include <header> .)      // optional header code blocks

CHARACTERS                    // optional
  letter = 'a'..'z' + 'A'..'Z' + '_' .
  digit  = '0'..'9' .

TOKENS                        // optional
  ident  = letter { letter | digit } .
  number = digit { digit } .

COMMENTS FROM "//" TO "\n"    // optional, repeatable
COMMENTS FROM "/*" TO "*/"

IGNORE '\r' + '\n' + '\t' + ' '   // optional

PRODUCTIONS
  Main = ... .

END Name.
```

### Character Sets

Character sets define character classes used in token patterns.

| Syntax        | Meaning          |
|---------------|------------------|
| `'a'..'z'`    | Character range  |
| `'x'`         | Single character |
| `ANY`         | Any character    |
| *name*        | Named reference  |
| *a* `+` *b*   | Union            |
| *a* `-` *b*   | Difference       |

```
letter = 'a'..'z' + 'A'..'Z' + '_' .
hex    = digit + 'a'..'f' + 'A'..'F' .
nonzero = digit - '0' .
```

### Token Patterns

Token definitions specify lexical patterns using character sets.

| Syntax          | Meaning          |
|-----------------|------------------|
| `"lit"`         | String literal   |
| `'c'`           | Char literal     |
| *name*          | Charset ref      |
| *e1* *e2*       | Sequence         |
| *e1* `\|` *e2*  | Alternation      |
| `{` *e* `}`     | Zero-or-more     |
| `[` *e* `]`     | Optional         |
| `'a'..'z'`      | Char range       |
| `(` *e* `)`     | Grouping         |

```
number = digit { digit } .
ident  = letter { letter | digit } .
```

### Comment Declarations

```
COMMENTS FROM "//" TO "\n"
COMMENTS FROM "/*" TO "*/" NESTED
```

Multiple comment styles can be declared. `NESTED` enables nesting for
block comments.

### Ignore (Whitespace)

```
IGNORE '\r' + '\n' + '\t' + ' '
```

Defines which characters are skipped between tokens. Uses charset expression
syntax. Generates a `setup_skip()` method that configures `PegCursor`.

### Productions

```
Name <. formal params .> (. local vars .) = body .
```

- **Formal parameters** (`<. ... .>`) declare the production's C++ signature.
  Types and names are extracted; `&` marks reference parameters.
- **Local variables** (`(. ... .)`) are emitted at the top of the parse method body.
- **Body** is a PEG expression terminated by `.`

Each production compiles to a `bool parse_Name(...)` method. The first
production is the entry point, called by `Parser::parse()`.

### PEG Expressions

| Syntax                   | Meaning                                        |
|--------------------------|------------------------------------------------|
| `"lit"`                  | Literal match (`keyword()` if identifier-shaped)|
| `'c'`                    | Char literal                                   |
| `ANY`                    | Match any character                            |
| *Name*                   | Rule call (`parse_Name()`)                     |
| *Name*`<`*a*`,`*b*`>`   | Parameterized call                             |
| *e1* *e2*                | Sequence                                       |
| *e1* `\|` *e2*           | Ordered choice (PEG alternation)               |
| `{` *e* `}`              | Zero-or-more (star)                            |
| `{` *e* `}+`            | One-or-more (plus)                             |
| `[` *e* `]`              | Optional                                       |
| `(` *e* `)`              | Grouping                                       |
| `&` *e*                  | And-predicate (zero-width lookahead)           |
| `!` *e*                  | Not-predicate (zero-width negative lookahead)  |
| `(. code .)`             | Semantic action (inline C++)                   |
| `IF ( (. cond .) )` *e*  | Resolver (conditional parse)                   |

Identifier-shaped literals (e.g., `"struct"`, `"END"`) are automatically
matched with `keyword()`, which checks that the literal is not followed by an
identifier-continue character. This prevents `"int"` from matching the prefix
of `"internal"`.

### Built-in Token Functions

These are methods on `peg::PegCursor`, called directly from productions (not
defined as grammar productions):

| Call                      | Matches                          | Output type       |
|---------------------------|----------------------------------|--------------------|
| `ident<name>`            | `[a-zA-Z_][a-zA-Z0-9_]*`        | `std::string`      |
| `integer<val>`           | Decimal, `0x` hex, `0b` binary   | `int64_t`          |
| `floating<val>`          | `digits.digits`                  | `double`           |
| `string_lit<s>`          | `"..."` with escape decoding     | `std::string`      |
| `char_lit<ch>`           | `'.'` with escape decoding       | `char`             |
| `scan_to("delim", out)`  | Scan to delimiter, capture content| `std::string`     |

## FRAME TEMPLATES

Generated code is injected into frame templates via `-->marker` directives.
The frame processor copies the template verbatim, replacing each marker with
generated code.

### Header Frame (Parser.frame)

| Marker               | Content                                        |
|----------------------|------------------------------------------------|
| `-->headers`         | `#include` directives from `(. ... .)` blocks  |
| `-->namespace_open`  | `namespace ns {` (if `-namespace` given)        |
| `-->public_decls`    | Public member declarations                     |
| `-->private_decls`   | Private member declarations                    |
| `-->parse_methods`   | Parse method declarations (`bool parse_X(...)`) |
| `-->namespace_close` | Closing `}`                                    |

### Implementation Frame (ParserImpl.frame)

| Marker                    | Content                                     |
|---------------------------|---------------------------------------------|
| `-->parser_include`       | `#include "PrefixParser.h"`                 |
| `-->namespace_open`       | `namespace ns {`                            |
| `-->entry_call`           | `return parse_StartRule(...);`              |
| `-->skip_setup`           | `setup_skip()` with whitespace/comment config|
| `-->parse_implementations`| All `parse_X(...)` method bodies            |
| `-->namespace_close`      | Closing `}`                                 |

### Custom Frames

To customize the generated parser class (add members, includes, helper methods),
provide custom frame files via `-frames`. See `pegc/grammar/frames/` for the
self-hosting frames, which add AST-building helpers like `ParseParams()` and
`TrimSemAction()`.

## RUNTIME

`PegRuntime.h` is shipped with generated parsers (not generated itself).
It provides the `peg::PegCursor` base class:

- **Cursor**: `init()`, `at_end()`, `pos()`, `remaining()`, `line()`, `col()`
- **Save/Restore**: `save()` returns a `Mark`; `restore(mark)` backtracks
- **Matching**: `match(char)`, `match(str)`, `match_charset(fn)`, `keyword(str)`
- **Lookahead**: `peek()`, `peek_at(str)`, `peek_charset(fn)`, `peek_keyword(str)`
- **Span**: `span(fn)` -- consume while charset matches (tight loop)
- **Skip**: `set_whitespace(fn)`, `set_comments(specs)`, `skip()`
- **Errors**: `expected(what)`, `error(msg)`, `errors()`, `has_errors()`, `furthest()`
- **Built-in tokens**: `ident()`, `integer()`, `floating()`, `string_lit()`,
  `char_lit()`, `scan_to()`

## EXAMPLES

### Minimal Grammar

```
COMPILER Hello
PRODUCTIONS
Hello = "hello" "world" .
END Hello.
```

### Expression Parser

A four-function calculator with operator precedence (from `tests/grammars/expr.peg`):

```
COMPILER Expr

(. #include <string> .)
(. #include <cstdint> .)

CHARACTERS
  letter = 'a'..'z' + 'A'..'Z' + '_' .
  digit  = '0'..'9' .

TOKENS
  ident  = letter { letter | digit } .
  number = digit { digit } .

COMMENTS FROM "//" TO "\n"

IGNORE '\r' + '\n' + '\t' + ' '

PRODUCTIONS

Expr <. int &result .>
  (. int rhs; .)
  = Term<result>
    { "+" Term<rhs>  (. result += rhs; .)
    | "-" Term<rhs>  (. result -= rhs; .)
    }
.

Term <. int &result .>
  (. int rhs; .)
  = Factor<result>
    { "*" Factor<rhs>  (. result *= rhs; .)
    }
.

Factor <. int &result .>
  = number    (. result = std::stoi(Str()); .)
  | "(" Expr<result> ")"
.

END Expr.
```

### Generating and Using

```sh
# Generate parser from grammar
pegc -o gen/ -namespace calc expr.peg

# Produces: gen/exprParser.h, gen/exprParser.cpp
```

```cpp
// driver.cpp
#include "exprParser.h"
#include <cstdio>

int main() {
    const char* input = "2 + 3 * 4";
    calc::Parser parser;
    parser.init(input, strlen(input));
    if (parser.parse()) {
        printf("OK\n");
    } else {
        for (auto& e : parser.errors())
            fprintf(stderr, "%d:%d: %s\n", e.line, e.col, e.message.c_str());
    }
}
```

Compile with `PegRuntime.h` on the include path:

```sh
g++ -std=c++17 -I/path/to/pegc/runtime driver.cpp gen/exprParser.cpp -o calc
```

## SELF-HOSTING

pegc compiles its own grammar. The bootstrap chain:

1. Original parser was generated by Coco/R from `peg.atg`
2. `peg.peg` (the self-hosted grammar) is compiled by pegc itself
3. Regenerating produces identical output (fixpoint verified)

Regeneration command:

```sh
./build/pegc pegc/grammar/peg.peg \
    -frames pegc/grammar/frames/ \
    -o pegc/frontend/ \
    -prefix ""
```

## FILES

```
pegc/
  tools/pegc.cpp              CLI entry point
  grammar/
    peg.peg                   Self-hosted grammar (authoritative)
    peg.atg                   Original Coco/R grammar (reference)
    grammar.asdl              AST definition
    frames/                   Custom frames for self-hosting
      Parser.frame
      ParserImpl.frame
  frontend/
    Parser.h, Parser.cpp      Generated parser (from peg.peg)
    GrammarAST.h              Generated AST (from grammar.asdl)
    PegAnalysis.h/.cpp        PEG analysis (first sets, left-recursion)
  backend/
    ParserEmitter.h/.cpp      C++ code generation
    frames/                   Default frame templates
      Parser.frame
      ParserImpl.frame
  runtime/
    PegRuntime.h              Runtime library (shipped, not generated)
  tests/
    test_grammar_parser.cpp   Grammar parsing tests
    test_peg_analysis.cpp     Analysis tests
    test_codegen.cpp          Code generation tests
    test_runtime.cpp          Runtime tests
    grammars/
      expr.peg                Example grammar
```

## SEE ALSO

- `docs/Operational Semantics.txt` -- DDCG theory underlying code generation
- `docs/CEK Machine Design.md` -- BBQ VM design (sibling project)
