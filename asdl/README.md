```
ASDL(1)                   ASDL Processor                    ASDL(1)
```

## NAME

**asdl** -- ASDL (Abstract-Syntax Description Language) processor
with template-driven codegen and JSON sidecar output

## SYNOPSIS

```
asdl -i input.asdl [-t template.inja] [-o output] [-lang c|c++] [--json file]
```

## DESCRIPTION

asdl reads an ASDL specification, validates it, and either generates
typed AST source via a Jinja-style template (Inja), writes a
processed JSON sidecar for downstream tools, or both.

ASDL is the Zephyr Abstract Syntax Description Language (Daniel C.
Wang, Andrew W. Appel, Jeff L. Korn, Christopher S. Serra, *The
Zephyr Abstract Syntax Description Language*, DSL'97, USENIX). It
describes algebraic data types: sum types (tagged unions),
product types (records), and primitive aliases. The processor
validates type references, registers C/C++ type names, and produces
a normalized JSON object suitable for codegen via Inja templates or
consumption by other tools (notably `burgc -asdl` for completeness
analysis).

## OPTIONS

**-i** *file*
: Input `.asdl` file. Required.

**-t** *file*
: Inja template for code generation. Required when generating
  source; optional when only `--json` is requested.

**-o** *file*
: Output file for the rendered template. Defaults to stdout when
  templating; ignored without `-t`.

**-lang** *c* | *c++* | *java*
: Target language for type registration. Default: `c++`. With
  `-lang c`, type names are registered using snake_case
  conventions (`my_module_my_type_t` etc.). With `-lang java`,
  primitives map to Java (`string`/`identifier` to `String`,
  `int64` to `long`, `bool` to `boolean`), sums register as plain
  references with no pointer suffix, and a field-free sum registers
  as `int` because Java has no enums before 1.5. Pair it with
  `templates/java.inja`.

**-java-package** *name*
: Emit a `package` declaration at the top of `-lang java` output.
  Rejected with any other `-lang`.

**--json** *file*
: Write the processed module as JSON to *file*. The schema matches
  what other tools (e.g. `burgc -asdl`) consume. See **JSON FORMAT**
  below.

**-h**
: Show help.

## TEMPLATES ARE MEANT TO BE FORKED

`templates/*.inja` are starting points, not a fixed backend. A consumer that
wants deep-copy, a visitor, an arena-free variant or anything else copies the
template next to its own grammar and adds it there, which is why this tool has
no flags for any of it. `javelina/compiler` does exactly that for its SIR.

A fork owns everything it copied, including the parts that are not about shape.
The one that matters:

**Every `bbq_arena_alloc` in the emitted code can return NULL.** The arena
refuses rather than aborting — that is the whole point of it, because the AST
being built came from input nobody here wrote — so a constructor that stamps
`_n->loc` without looking is a NULL dereference reachable from a large enough
source file. Guard the allocation, return NULL, and let the caller ask
`bbq_arena_oom()` at its own boundary.

The same applies to anything a fork adds that allocates. A deep-copy memoised on
a `bbq_vec`, for instance, has a sharper version of the problem: the memo is what
makes a back-edge terminate, so a refused entry that is ignored turns a cyclic
graph into unbounded recursion. Latch the refusal and unwind on it.

## ASDL FORMAT

```
module mymod {

    // Aliases register a foreign C/C++ type as a primitive.
    type_alias = "my_ns::Type";

    // Sum types: tagged union of constructors.
    expr = Add(expr left, expr right)
         | Const(int64 value)
         | Var(identifier name)
         attributes (loc location)

    // Product types: record.
    point = Point(int64 x, int64 y)
}
```

### Field Flags

A field's type may be suffixed with:

| Suffix | Meaning           |
|--------|-------------------|
| (none) | Required, single  |
| `?`    | Optional          |
| `*`    | Sequence          |

```
Block(stmt* statements, expr? trailing_expr)
```

### Attributes

`attributes(...)` on a sum type adds fields shared across all of
its constructors (e.g. source locations).

## JSON FORMAT

The `--json` output is a single object with this shape:

```json
{
    "name": "mymod",
    "aliases": { "type_alias": "my_ns::Type" },
    "definitions": [
        {
            "name": "expr",
            "type": "sum",
            "is_enum": false,
            "types": [
                {
                    "name": "Add",
                    "fields": [
                        {"type": "expr", "name": "left",  "flag": "none"},
                        {"type": "expr", "name": "right", "flag": "none"}
                    ]
                },
                ...
            ],
            "attributes": [
                {"type": "loc", "name": "location", "flag": "none"}
            ]
        }
    ]
}
```

Field types reference either:
- A primitive (e.g. `int64`, `string`, `identifier`)
- An alias declared in the same module
- The name of a defined sum or product type (i.e. a structural
  reference, what `burgc -asdl` treats as a node-typed field for
  per-position demand)

## EXAMPLES

Generate C++ AST from a template:

```
asdl -i mymod.asdl -t templates/cpp_ast.inja -o MyAST.h
```

Generate Java 1.0 AST classes into a package:

```
asdl -i peg.asdl -t templates/java.inja -lang java \
     -java-package com.example.peg -o Peg.java
```

The Java output targets the 1.0 language, not modern Java: every class is
top-level (nested classes are 1.1), sequences are arrays (collections are 1.2,
generics 1.5), a field-free sum becomes `int` constants (enums are 1.5), and no
field is declared `final` — JLS 1.0 section 8.3.1.2 requires a final field's
declarator to carry its initializer, so blank finals are 1.1. `tests/
test_java_generation.cpp` pins each of those.

Emit JSON sidecar for tooling:

```
asdl -i ir.asdl --json ir.json
```

Both at once:

```
asdl -i ir.asdl -t templates/cpp_ast.inja -o IR.h --json ir.json
```

## FILES

```
asdl/
  grammar/
    asdl.peg                  Self-hosted grammar (authoritative)
  frontend/
    Parser.h, Parser.cpp      Generated parser (from asdl.peg)
    AsdlAST.h                 AST definition (handwritten)
  src/
    generator.h, .cpp         Module → JSON + template engine
    validator.h, .cpp         File / type / structure validation
    type_registry.h, .cpp     Language-specific type name mapping
    string_utils.h, .cpp      Snake/camel/pascal-case helpers
    main.cpp                  CLI driver
  templates/
    *.inja                    Inja templates for typical AST shapes
  tests/
    test_generation.cpp       Codegen tests
    test_string_utils.cpp     String utility tests
```

## SEE ALSO

- `burgc(1)` -- consumes `--json` output via `-asdl` for
  schema-aware completeness analysis.
- `pegc(1)` -- generates the parser asdl uses for `.asdl` files.
- `lib/third_party/inja/` -- Inja template engine.
- `lib/third_party/nlohmann/` -- JSON library used for `--json`
  serialization and (in burgc) consumption.
