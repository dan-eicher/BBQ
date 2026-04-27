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

ASDL describes algebraic data types: sum types (tagged unions),
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

**-lang** *c* | *c++*
: Target language for type registration. Default: `c++`. With
  `-lang c`, type names are registered using snake_case
  conventions (`my_module_my_type_t` etc.).

**--json** *file*
: Write the processed module as JSON to *file*. The schema matches
  what other tools (e.g. `burgc -asdl`) consume. See **JSON FORMAT**
  below.

**-h**
: Show help.

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
