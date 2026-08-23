# The Write Side

BBQ reads and writes binary from one grammar. This document names what that pair is, which
laws it obeys, and what it does not claim.

The read side is an implementation of Interval Parsing Grammars [IPG]. The write side is two
different things depending on the backend, and they are worth keeping apart:

| Backend | `get` | write | Shape |
|---|---|---|---|
| C++ ZCow (`backends/cpp`) | `<Rule>_read` → `FieldCapture` index over the input | `<Rule>_write(root, buf, len, zc)` | **lens** |
| CEK / Python (`bindings/python`) | `Spec.parse` → the same index | `Result.emit()` | **lens** |
| owning C (`backends/c`) | `<rule>_read` → an owning struct | `<rule>_write(wctx, in)` | **parser / serializer pair** |
| c-lite (`backends/c`, view) | `<pfx>_<rule>_read` → a span index | — | reader only |

A lens edits a document. A serializer builds one. The difference is whether the original
bytes are an input to the write.

## Lenses

A lens [LENS, Def 3.1] between a concrete set `C` and an abstract set `A` is a pair of
partial functions:

```
get : C ⇀ A            parse — bytes to a capture view
put : A × C ⇀ C        emit  — edited view AND THE ORIGINAL BYTES to new bytes
```

`put` takes the original concrete structure because `get` discards information. That is the
entire reason `bbq::zcow` is an overlay over a retained baseline rather than a value tree:
the Borrowed spans *are* the discarded information, kept so `put` can put them back.

Def 3.2 is **two typing conditions and two laws**, and the typings do most of the work:

| | Statement | In BBQ | Anchored by |
|---|---|---|---|
| **(GET)** | `get(C) ⊆ A` | a parse yields a capture view | the reader; every parse test |
| **(PUT)** | `put(A × C) ⊆ C` | emit must yield a document **the grammar accepts** | `CrossBackend.WriterRefusesWhenTheEditLeavesTheGrammar`, `TestLensLaws.test_put_refuses_*` |
| **GetPut** | `put(get(c), c) ⊑ c` | an unedited re-emit is **byte-identical** | `CrossBackend.CppWriterToCReaderFullFixture` (all 49 rules), `TestLensLaws._lens` |
| **PutGet** | `get(put(a, c)) ⊑ a` | re-parsing an edit yields **the edit that was made** | `CrossBackend.CppWriterMutateRoundTrip`, `TestLensLaws` |
| Def 3.3 **Totality** | `C ⊆ dom(get)` and `A × C ⊆ dom(put)` | `get` is partial (a parse can reject); `put` is total — it always answers, with bytes or a refusal | — |
| **PutPut** | `put(a', put(a, c)) ⊑ put(a', c)` | **not claimed** | — |

**(PUT) is the condition that forbids a broken write, and it is easy to miss.** `⊑` holds
vacuously when its left side is undefined, so `get(put(a,c)) ⊑ a` is satisfied by emitting
garbage that fails to parse — PutGet alone rules out nothing. Foster et al. make the point
directly (§3.3, fn. 3): well-behavedness without totality is near-trivial, since a `put`
undefined on all inputs satisfies both laws.

So the writer tests membership in `C` itself and **refuses** rather than returning a
non-member. It has to: the walk replays the shape the *parse* recorded, so an edit that
changes which shape the grammar selects — a switch discriminant, a value a `where` now
rejects — leaves it emitting bytes that will not re-parse. Membership is what `get`
decides, so `get` is what it asks:

- **C++** — `<Rule>_write` re-reads its own output with `<Rule>_read` and returns an empty
  vector if it is not a member. The generated writer therefore depends on the generated
  reader.
- **CEK/Python** — `emit()` re-parses with the CEK and raises `bbq.ParseError`. For a spec
  with `extern`, that runs the registered callback once more, on the output.

The test is *not* "consumes every byte". A rule may legitimately stop early — `Unt =
struct { xs: array<uint8>(none, until(@remaining < 2)) }` accounts for 2 of 3 bytes by
design, and the CEK does the same — which is EverParse's caveat about parsers that do not
consume their whole input [EVERPARSE, §5]. What must not happen is an edit turning
**accounted-for bytes into unaccounted-for ones**, so the check compares the leftover
against the leftover the original parse had.

**PutPut is not claimed.** `put(a', put(a, c)) ⊑ put(a', c)` — "very well behaved"
[LENS, §3.2] — does not hold for BBQ's array edits, and Foster et al. note that their own
`map`, `flatten`, `merge` and conditional combinators fail it "for reasons that seem
pragmatically unavoidable". BBQ is well behaved. It is not very well behaved.

**Creation is not modelled.** Foster et al. handle "make a new concrete view from an
abstract one with no old concrete view" with a distinguished `Ω` ("missing") value
[LENS, §3, *Dealing with Creation*]. BBQ has no `put(a, Ω)`: `bbq.build` constructs bytes
with no grammar involved, and the two layers meet only at bytes.

## Dependent fields

A **dependent field** [NAIL, §3.3] is a length, count or offset field: during parsing another
parser depends on it, and while writing its value depends on other structure in the document.
Nail's rule is that such a field

> [is] not exposed in the data model, but instead transparently computed. This frees the
> developer from checking that these fields are correct (for input) or having to keep their
> values in sync with the rest of the data structure (for output).

BBQ follows this. A count is a *result* of the array, never an input the caller maintains,
and **assigning to one is refused** (`TypeError` from the Python face). Accepting the
assignment and then overwriting it would be a PutGet violation the caller could not see —
the document parses, it just is not the view that was asked for. `bbq::writer::derived`
records the nodes the walk writes, so the refusal is by node identity, not by name.

The algorithm is Nail's [NAIL, §4]: reserve the field, overwrite it when the construct that
determines it is reached, and let anything downstream read the corrected value. In BBQ that
is a four-call vocabulary on `bbq::writer` (`backends/cpp/runtime/bbq_writer.h`):

| Call | When | Effect |
|---|---|---|
| `mark_count(id, field)` | the count field's read is visited | remember where the count lives, in scope |
| `set_count(id, n, enc)` | the array it counts begins | write the array's effective length there |
| `mark_rest(field)` | an `@rest` size field's read is visited | remember where the size lives |
| `set_rest(enc)` | the window closes | measure the edited window, write its length back |

What is enforced: array counts, including a count reached by a path (`array<uint8>[h.n]`),
counts nested inside array element bodies (the walk descends into each element), and `@rest`
window sizes. The encoding is respected — a `uleb128` count is rewritten as a varint.

What is not derived: `compute` fields are not in the byte stream at all. A `where` constraint
is not a dependent field either — nothing recomputes it — but an edit that violates one is
caught by the (PUT) membership test above, which refuses rather than emitting.

The owning C writer solves the same problem in the serializer shape — `bbq_write_ctx_t`
carries `count_holes` and `rest_holes` and back-patches the prefix once the content length is
known.

### One op-list, two consumers

The enforcement walk is the *same walk as the parse*, so it comes off the same lowering. The
kont graph lowers once, through `lower_writer_ops` (`backends/render/RenderEmit.h`), to a flat
op-list carrying the count↔array and `@rest` linkage. Two consumers ride it:

```
kont graph ──burg──▶ lower_writer_ops ──┬──▶ writer_view_cpp.inja ──▶ generated <Rule>_write
                                        └──▶ WriterInterp::run_writer ─▶ CEK / Python emit()
```

The template **renders** the list into one C++ stencil per op; the interpreter **executes**
it, because a grammar compiled at runtime has no generated writer to call. A dynamic backend
that lowered the kont graph itself would be a second grammar — which is what Nail's
"one description, both directions" exists to prevent. The two are held byte-equal on every
fixture rule and every edit in `CrossBackend.CppWriterToCReaderFullFixture` and
`CrossBackend.CppWriterMutateRoundTrip`.

## Parser and serializer as a security pair

Lens laws say the edit round-trips. They say nothing about whether a *message* has a unique
encoding. That is a separate lattice [EVERPARSE, §2.1], for a parser
`p : {0,1}* → V ⊎ {⊥}` and a serializer `s : V → {0,1}*`:

| Property | Statement |
|---|---|
| correct | `∀m ∈ V. p(s(m)) = m` |
| exact | accepts at most serialized messages — `p⁻¹(V) = s(V)` |
| non-malleable (injective) | `p(x) = p(y) ⇒ (x = y ∨ p(x) = ⊥)` — one encoding per message |
| complete (surjective) | accepts at least one encoding of each message |
| **secure parser** | non-malleable ∧ complete; then `s = p⁻¹` is the unique secure serializer |

BBQ's parser is **not** non-malleable in general, and which constructs are malleable is a
property of the construct, not of a backend. Determined by parsing pairs of distinct inputs
and comparing the resulting value trees:

| Construct | Non-malleable? | Evidence |
|---|---|---|
| fixed-width integers (`uint8`…`int64`, LE/BE) | yes | one encoding per value by definition |
| `uleb128` / `sleb128` | **no** on read | `00` and `80 00` both parse to `0`; the writer emits the canonical form |
| `bool` | **no** | `01`, `02` and `ff` all parse to `true` |
| `float32` / `float64` | **no** | `+0.0` and `-0.0` share a value; NaN payloads share a value |
| `@rest` window | yes | the window must be consumed exactly — a leftover byte fails with `interval: size mismatch` |
| explicit interval `[s, e]` | **no** | bytes inside the window that no field reads are unconstrained: `02 ff 0a bb` and `02 ff 0a cc` both parse to `{off: 2, val: 10}` (`bytes_consumed` reports the shortfall) |
| `resync` arrays | **no** | the skipped bytes are not represented: `00 0a ff 00 0b` and `00 0a ee 00 0b` yield the same elements |
| `extern` | unknown | the registered callback decides; BBQ cannot claim a property it does not implement |

The malleable rows are not defects to be fixed silently — several are the format's, not
BBQ's. They are recorded so a spec author writing a security-sensitive grammar knows which
constructs admit more than one encoding of the same message.

### A malleable parser still gives a well-behaved lens

These two frames are independent, and the difference is exactly the baseline. `uleb128` is
malleable, so `p` is not injective and `s` (which emits canonically) is not `p⁻¹`. The lens
is nonetheless well behaved, because `put` never re-encodes what was not edited:

```python
data = b"\x80\x00\x42"          # v = 0, encoded non-canonically; tail = 0x42
r = spec.parse(data)            # T = struct { v: uleb128, tail: uint8 }
r.emit() == data                # True  — GetPut, the padded varint survives
r.tail = 0x43
r.emit()                        # b"\x80\x00\x43" — the varint is still as authored
```

The owning C writer cannot do this: its `get` produces a struct of values, so the non-canonical
encoding is gone by the time the writer runs. That is Nail's asymmetry [NAIL, §3.1] —
"parsing the generator's output will yield the generator's input, but generating the parser's
output does not necessarily yield the parser's input", because *constants* (padding,
alternative encodings) are discarded. This is why the C writer is judged on value fidelity
against the CEK rather than on byte identity, and why the C++/Python writers are judged on
byte identity.

## Why there are no suspensions

DFDL's unparser [DFDL] faces the same dependent-field problem while *streaming*: an
`dfdl:outputValueCalc` whose value is not yet computable creates a **suspension** — a snapshot
of the unparser state plus the unevaluated expression — output continues into a chain of
buffered data output streams split off from the direct stream, and a buffer collapses back
into the direct stream once it is final. Circular dependencies deadlock and are a schema
definition error.

BBQ needs none of that because it does not stream. The ZCow overlay holds the whole edited
document with random access, so an `@rest` window's length is *measured* by reserializing it
(`zcow::window_len`) rather than deferred; the owning C writer likewise writes into a buffer
and back-patches a hole. BBQ trades streaming for random access. A streaming BBQ writer would
have to adopt something like DFDL's suspensions.

## References

- **[IPG]** Zhang, Morrisett & Tan. *Interval Parsing Grammars for File Format Parsing.*
  PLDI 2023. The read side. Parsing only — it defines no serialization judgement.
- **[LENS]** Foster, Greenwald, Moore, Pierce & Schmitt. *Combinators for Bidirectional Tree
  Transformations: A Linguistic Approach to the View-Update Problem.* TOPLAS 29(3), 2007.
  `get`/`put`, GetPut/PutGet/PutPut, well-behaved vs very well behaved.
- **[NAIL]** Bangert & Zeldovich. *Nail: A practical interface generator for data formats.*
  IEEE S&P Workshops (LangSec) 2014; *Nail: A Practical Tool for Parsing and Generating Data
  Formats*, OSDI 2014. Dependent fields, the reserve/overwrite algorithm, the semantic
  bijection and its asymmetry.
- **[EVERPARSE]** Delignat-Lavaud, Fournet, Ramananandro et al. *EverParse: Verified Secure
  Zero-Copy Parsers for Authenticated Message Formats.* USENIX Security 2019. The
  correct / exact / non-malleable / complete lattice for parser-serializer pairs.
- **[DFDL]** Apache Daffodil. *Unparsing: Data Output Stream Buffering and
  dfdl:outputValueCalc.* The suspension model for streaming unparsers.
