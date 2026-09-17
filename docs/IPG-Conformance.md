# IPG Conformance

BBQ against *Interval Parsing Grammars for File Format Parsing* (Zhang, Morrisett,
Tan; PLDI 2023, `doi:10.1145/3591264`).

The list below is enumerated from the paper — one entry per rule in Figures 5, 7
and 8, per check in §3.2, per full-language feature in §3.4, per case study in §4,
and per clause of the termination argument in §5 — and then marked against BBQ.
It is written that way round on purpose: a list drawn from what BBQ implements
cannot show what BBQ is missing.

Every entry carries a test. `ipg_law_tests` (`test/ipg_law_test.cpp`) is the suite
that holds the laws themselves; where an existing suite already pins one, that is
the anchor named. A law with no test is a law nobody is keeping.

A law needs a case that would fail if the law did not hold, which for a parser is
usually a *value* and not a span: a test that checks where a field landed passes
on a parser that went to the right place and read the wrong thing. So the laws
here read values back, vary the input so a constant cannot satisfy two cases, and
state the malformed input the law rejects alongside the one it accepts. A
construct that selects among branches — a choice, a switch — also has to say which
branch ran, because arms that leave the same tree witness nothing.

The laws are stated against the CEK because it is the semantics the generators are
generated against, with a backend arm wherever the mechanism differs enough that
the two could agree on shape and disagree on bytes (path resolution, whole-
construct predicates, unbounded loops). Backend agreement in the large is not this
file's job: `render_view_parser_test` checks the C++ view parser against the CEK
construct by construct, `render_c_test` and `c_backend_e2e_test` do the same for
the C reader and writer, and `cross_backend_test` runs the fixture grammar between
them.

Three verdicts appear:

- **holds** — BBQ implements the law and a test pins it.
- **differs** — BBQ answers the same question differently, on purpose. The entry
  says how, and the test pins BBQ's answer, not the paper's.
- **not claimed** — BBQ does not have the feature. The entry says what stands in
  its place, if anything.

## Vocabulary

The paper's core language and BBQ's surface differ in spelling, not in shape:

| IPG | BBQ |
|---|---|
| `A[e_l, e_r]` — nonterminal with an interval | `f: T[start, end]` |
| `s[e_l, e_r]` — terminal string with an interval | `f: uint32be where f == 0x89504E47`, `bytes[n]` |
| `{id=e}` — attribute definition | `f: compute(e : T)` |
| `⟨e⟩` — predicate | `where e` (on a field, a struct, an array) |
| `for id=e1 to e2 do A[e_l,e_r]` — array term | `array<T>[n] @ [lo, hi]` |
| `alt1 / alt2` — biased choice | `T1 \| T2`, `union { … }` |
| `A.id` — attribute of a nonterminal | `f.g` |
| `A(e).id` — attribute of an array element | `a[e].g` |
| `EOI` | `EOI` |
| `A.start`, `A.end` | `@start`, `@end`, `@pos` |
| `switch(e1:A1 / … / An+1)` | `switch(e) { … default: … }` |
| blackbox parser | `extern("fn", "T")` |
| `where D → …` — local rule | a nested rule, resolving names outward |

The loop variable is `@index`. A bare identifier is *always* a field reference
(Grammar §7.3), so the parse-state environment lives behind the `@` sigil and a
spec may name a field `pos`, `buffer` or `i` freely.

## §3.1 — Core syntax (Figure 5)

| Law | Verdict | Test |
|---|---|---|
| A nonterminal's interval says which slice of the input its rule describes | holds | `IpgTerm.NonterminalCarriesAnInterval` |
| A terminal matches over its interval, or fails | holds | `IpgTerm.TerminalMatchesOrFails` |
| `{id=e}` binds a value | holds | `IpgTerm.AttributeDefinitionBindsAValue` |
| `⟨e⟩` gates the parse | holds | `IpgTerm.PredicateGatesTheParse` |
| An array places each element by an interval that may mention the loop variable | holds | `IpgTerm.ArrayPlacesEachElementByItsOwnInterval` |
| `A(e).id` — the attribute of an array element — is reachable | holds | `IpgTerm.ArrayElementAttributeIsReachable`, `…OfAStructElement` |
| Expressions: literals, `+ - * / = > < ∧ ∨`, ternary | holds | `Sema.IntegerArithmeticValid`, `CEKLayer2Binop.*`, `CEKLayer2Ternary.*` |
| `EOI`, `A.start`, `A.end` | differs — see §3.3 | `IpgInterval.EoiIsTheWholeInputAndTheWindowIsNamedByAtEnd` |

BBQ's expression language is wider than the paper's (bit operations, shifts, `%`,
`!=`, `<=`, `>=`, calls into user predicates); those are BBQ's own and are covered
by `sema_test.cpp` and `CEKLayer2*`.

## §3.2 — Attribute checking

| Law | Verdict | Test |
|---|---|---|
| Property 1: every reference names a defined attribute | holds | `IpgAttrCheck.AReferenceToANameNoRuleDefinesIsRejected`, `…AnUndefinedNameInAnIntervalIsRejected`, `…AnUndefinedNameInAComputeIsRejected`, `Sema.ReferenceToANameNoRuleBindsIsRejected` |
| …at every step of a dotted path, and through a subscript | holds | `IpgAttrCheck.ADeepPathIsCheckedAtEveryStep`, `…APathThroughASubscriptIsChecked` |
| …while a name from a calling rule's scope stays permissive | holds | `IpgAttrCheck.ACrossRuleNameIsStillAccepted`, `Sema.CrossRuleRefAllowed` |
| A name not bound locally may come from a calling rule's scope | holds | `Sema.CrossRuleRefAllowed`, `IpgLocal.ANestedRuleSeesTheEnclosingScope` |
| `def(A)` is the intersection over alternatives — a name only some arms bind is not an attribute of the rule | holds | `IpgAttrCheck.DefIsTheIntersectionOverAlternatives`, `…ANameNoAlternativeBindsIsRejected` |
| …and a name every arm binds is | **differs** | `IpgAttrCheck.ANameEveryAlternativeBindsIsRefusedWithTheLift`, `…TheLiftedFieldReadsWhicheverArmMatched` |
| Property 2: no circular definitions | holds, at rule granularity | `IpgAttrCheck.ACycleAmongRulesIsRejected`, `Sema.CircularDependency` |
| Terms are reordered into the dependency graph's topological order | **differs** | `IpgAttrCheck.TermsAreNotReorderedAForwardReferenceIsAnError` |

**Why the intersection's positive half differs.** `def(A)` being the intersection
makes `Inner.p` legal when every alternative binds `p`. BBQ refuses it. A choice
is a tagged union in the generated types, so a field *of the choice* would have to
dispatch on the variant tag at every use, in every emitter — and a name every arm
binds is a field the format has in common, which belongs in front of the choice
where it is one field rather than one per arm. The diagnostic says so. The
negative half — a name only *some* arms bind — is refused for the paper's reason,
because which arm matched is the input's to decide.

**Why the reordering differs.** The paper rewrites `B1[0, B2.a] B2[a1, EOI]
{a1=2}` into `{a1=2} B2[a1, EOI] B1[0, B2.a]`, because in a grammar over an
abstract string the term order is free. In BBQ the term order *is* the byte
order: reordering fields would change the format. So a reference to a later field
is an error, and the spec is written in the order the bytes appear. A field that
genuinely needs a later value reaches it by interval (`[start, end]`) instead —
which is what intervals are for.

## §3.3 — Parsing semantics (Figure 8)

| Rule | Law | Verdict | Test |
|---|---|---|---|
| G-NT | A nonterminal parses by its rule | holds | `CEKLayer3.MinimalStruct`, `…MultiRuleCrossReference` |
| R-AltSucc | The first alternative that succeeds is the result; order is observable | holds | `IpgAlt.TheFirstSuccessWinsAndOrderIsObservable` |
| R-AltFail | A failing alternative falls through to the next | holds | `IpgAlt.AFailingAlternativeFallsThroughToTheNext` |
| R-AltFail + A-Seq1 | …and the enclosing parse carries on afterwards, whichever arm won | holds | `IpgAlt.TheEnclosingParseContinuesAfterALaterArmWins`, `…AfterALaterUnionVariantWins` |
| R-Emp | No alternatives left ⇒ Fail | holds | `IpgAlt.WhenEveryAlternativeFailsTheRuleFails` |
| R-Alt\* | Each alternative starts from a fresh environment and an empty tree list | holds | `IpgAlt.AFailedArmLeavesNoBindingsBehind` |
| A-Seq1/2 | Terms thread left to right; later terms see earlier bindings | holds | `IpgSeq.LaterTermsSeeEarlierBindings` |
| A-Fail | One failing term fails the whole alternative | holds | `IpgSeq.OneFailingTermFailsTheWholeAlternative` |
| A-Emp | A completed alternative yields the node | holds | `CEKLayer3.MinimalStruct` |
| T-Ter / T-NTSucc | `0 ≤ l ≤ r ≤ \|s\|`, and `[n, n]` is valid (fn. 1) | holds | `IpgInterval.AnEmptyIntervalIsValid`, `…AnEmptyIntervalComputedFromDataIsValid` |
| T-TerFail / T-NTFail | An inverted interval, or an end past the input, fails | holds | `IpgInterval.StartAfterEndFails`, `…StartAfterEndIsRejectedStaticallyWhenItIsLiteral`, `…AnEndPastTheInputFails` |
| T-NTSucc | The subparser sees `s[l, r]` and nothing else | holds | `IpgInterval.ASubparserCannotReadPastItsWindow`, `…ANestedWindowMustLieInsideItsParent` |
| T-NTSucc | The subparse is re-based: `EOI` inside `B[l,r]` is `r − l`, offsets come back `+l` | **differs** | `IpgInterval.EoiIsTheWholeInputAndTheWindowIsNamedByAtEnd` |
| T-Attr | An attribute definition consumes nothing | holds | `IpgAttr.AComputeConsumesNoInput` |
| T-Pred / T-PredFail | A predicate consumes nothing and fails the alternative when it does not hold | holds | `IpgPred.APredicateConsumesNoInput`, `…AStructLevelPredicateIsEnforced`, `…AnArrayLevelPredicateIsEnforced`, `…AnElementPredicateIsEnforced`, `…AnArrayLevelPredicateIsEnforcedByTheCReaderToo` |
| T-Array-Empty | `e2 ≤ e1` ⇒ no iterations, no constraints | holds | `IpgArray.ZeroTripsImposeNoConstraint` |
| T-Array | The loop variable ranges over `e1 … e2 − 1` | holds | `IpgArray.TheLoopVariableRangesOverTheCountExclusive` |
| §3.3 | Memoisation gives the generated parser `O(n²)` | **not claimed** | — |

**Why the re-basing differs.** The paper hands a subparser its own string, so
inside `B[l, r]` the input *is* `s[l, r]`: `EOI` there is `r − l`, and `B.start`
and `B.end` come back rebased by `+l`. BBQ keeps one absolute coordinate system.
`EOI` is the whole input, and the window is named by `@start`, `@end` and
`@remaining`; confinement is identical (a read cannot cross `@end`, and a nested
window must lie inside its parent), but an offset means the same thing everywhere
it is written, which is what a format's own documentation assumes. `@start`/`@end`
are the construct's entry offset and active bound rather than the paper's
min/max-over-what-was-touched accumulators, since BBQ's terms are sequential and
the cursor already carries that.

**Memoisation.** The paper memoises `S[l, r]` per start nonterminal for an `O(n²)`
bound. BBQ does not, and does not claim the bound. Nothing in the semantics
depends on it; an overlapping two-pass grammar re-reads its bytes (§4.3).

## §3.4 — The full language

| Feature | Verdict | Test |
|---|---|---|
| Implicit intervals: first term at 0, each later term after the previous | holds | `IpgImplicit.TermsFollowOneAnotherWithoutWrittenIntervals` |
| A lone expression in brackets is a length | holds | `IpgImplicit.ALoneExpressionIsALength`, `…ALengthWindowConfinesWhatIsInsideIt` |
| Local rules (`where D → …`) see the enclosing scope | holds, as nested rules resolving names outward | `IpgLocal.ANestedRuleSeesTheEnclosingScope`, `RenderC.AncestorAccess` |
| Switch terms: left to right, first true wins, remaining skipped | holds | `IpgSwitch.TheFirstMatchingCaseWinsAndLaterOnesAreSkipped` |
| The default choice is taken when every condition fails | holds | `IpgSwitch.TheDefaultIsTakenWhenNoCaseMatches` |
| A default with an always-invalid interval fails closed | holds, spelled `default: reject` | `IpgSwitch.ARejectDefaultFailsClosed` |
| Blackbox parsers see only what their interval gives them | holds | `IpgBlackbox.AnExternSeesOnlyItsInterval` |
| Existentials `∃id.e1?e2:e3` | **not claimed** | — |

**Existentials.** The paper introduces `∃id.e1?e2:e3` to reach the array element
satisfying a condition — its use is the two-pass PDF grammar of §4.3, where an
object's length lives in whichever header links to it. BBQ has no such search: an
element interval indexes an array directly (`shs[@index].ofs`), which covers the
positional case, and the size-prefix case is `@rest`. A grammar that needs to
*find* the entry by predicate has to hoist the search into a `compute` or a user
predicate.

## §3.5 — Expressiveness

| Law | Verdict | Test |
|---|---|---|
| `{aⁿbⁿcⁿ}` is an IPG, so IPGs ⊄ CFGs | holds | `IpgExpressive.AnBnCnIsExpressible` |
| Left recursion is admitted when the interval strictly shrinks | **differs** | `IpgExpressive.UnguardedRecursionIsRejected`, `…RecursionGuardedByAnArrayIsAccepted` |

**Why left recursion differs.** The paper accepts `Int → Int[0, EOI-1]
Digit[EOI-1, EOI]` because the interval shrinks each time, and proves it with the
§5 cycle check. BBQ runs no such check, so it takes the stronger structural rule
instead: recursion must pass through a construct that can stop — an array's count
or terminator, an optional's absence. `Blocks → Block Blocks / Block` is an
`array<Block>(none, eof)`; a digit-at-a-time left recursion is not expressible and
is refused rather than accepted-and-maybe-looping.

## §4 — Case studies

| Pattern | Verdict | Test |
|---|---|---|
| §4.1 / Fig. 2 — random access: data lives where a parsed offset says, including behind the cursor | holds | `IpgCaseStudy.RandomAccessFromAParsedOffset`, `…TheCReaderTakesTheSameRandomAccess` |
| §4.1 / Fig. 9b — sections placed by the section-header table's own entries | holds | `IpgCaseStudy.TheSectionTablePlacesSectionsByItsOwnEntries` |
| §4.2 — type-length-value: the type picks the subparser, the length bounds it | holds | `IpgCaseStudy.TypeLengthValue` |
| §4.3 — backward parsing from the end of the file | holds | `IpgCaseStudy.BackwardParsingFromTheEndOfInput` |
| §4.3 — two-pass: overlapping intervals read the same bytes twice | holds | `IpgCaseStudy.OverlappingIntervalsParseTheSameBytesTwice` |
| §3.4 / §7 — a blackbox decompressor over a bounded window | holds | `IpgBlackbox.AnExternSeesOnlyItsInterval`, `RenderC.SweepExtern` |

## §5 — Termination

The paper builds a nonterminal dependency graph, enumerates its elementary cycles,
and asks Z3 whether the intervals around each cycle can all stay `[0, EOI]`. If
none can, Theorem 5.1 says parsing terminates for every input. One syntactic
extension is added: a rule that consumes at least one terminal contributes
`A.end > 0`, which is what admits the GIF block list.

BBQ runs no cycle enumeration and no solver. It takes a stronger structural rule
that needs neither, and the paper's own nontermination witnesses all fall to it:

| Clause | Verdict | Test |
|---|---|---|
| Nonterminal dependency graph, elementary cycles, SMT check (T1–T3, Thm 5.1) | **not claimed** — replaced by the structural rule below | — |
| Fig. 11b — `S → num[0,1] S[num.val, EOI]`, a seek back to a data-controlled offset | rejected | `IpgTermination.ASeekingSelfReferenceIsRejected` |
| Fig. 11d — `S → ""[0,0] S[0, EOI]`, recursion on the same interval | rejected | `IpgTermination.RecursionOnTheSameIntervalIsRejected` |
| §5 — `A → B[0,EOI] / s; B → A[0,EOI] / s`, a mutual cycle on one interval | rejected | `IpgTermination.AMutualCycleOnTheSameIntervalIsRejected` |
| Fig. 11c — repeating a subparser that consumes nothing | rejected | `IpgTermination.AnEofArrayOfAZeroWidthElementIsRejected`, `…AnEofArrayOfAnEmptyByteRunIsRejected`, `…AnUntilArrayOfAZeroWidthElementIsRejected`, `…AnElementWhoseWidthOnlyTheDataDecidesIsRejected`, `…AnElementWithOneZeroWidthArmIsRejected`, `…AnUnboundedArrayOfABlackboxIsRejected` |
| The extension's positive case — the GIF block list terminates | holds | `IpgTermination.AnEofArrayOfAProductiveElementTerminates`, `…AnElementWithAFixedWidthPrefixIsAccepted`, `…TheCReaderWalksTheSameEofArrayToTheEnd`, `…ABlackboxBehindAFixedWidthFieldIsFine` |
| A bounded repeat needs no such measure | holds | `IpgTermination.ACountedArrayOfAZeroWidthElementIsFine` |
| T6 — blackbox parsers are assumed to terminate | holds, with the width still required of the element | `IpgTermination.AnUnboundedArrayOfABlackboxIsRejected`, `…ABlackboxBehindAFixedWidthFieldIsFine` |

**BBQ's rule.** Recursion must pass through a construct that can stop, and an
array with no count — `(none, eof)`, `(none, until(…))` — must have an element
that always reads. "Always" is the operative word and the check is a refusal, not
a runtime watchdog: one arm of a biased choice that reads nothing is enough to
refuse, and a width that only the data decides is not an established one. That is
the same posture as Theorem 5.1, which admits exactly the grammars whose
termination is settled before the parse starts. It costs some grammars the paper
would accept (`array<bytes[n]>(none, eof)` is refused even though most inputs
terminate) and buys the absence of any input that makes the parser spin.

The counted forms — `[n]`, `count(e)` — need none of this: the count is the
measure that decreases, exactly as the paper's `for` loop is.

## Out of scope in the paper too

§1 notes that a format may impose semantic properties beyond parsing — the SVG
`href` acyclicity, the PDF page-tree inheritance — and puts them in a separate
validation pass. BBQ's `where` predicates and `@header`/`@source` code blocks are
where such a pass would attach, and checksums are the everyday case
(`CBackendE2E.IPv4ChecksumWhere`). Nothing here claims to decide them.

## Noticed, not addressed

**Overlapping switch ranges draw no diagnostic.** `switch(t) { 0 .. 5: X; 3 .. 9:
Y; … }` compiles silently; only an exactly duplicated case value is reported
(`Sema.SwitchDuplicateCaseValues`). First-match-wins makes the overlap
well-defined, and the law that it is first-match is tested
(`IpgSwitch.TheFirstMatchingCaseWinsAndLaterOnesAreSkipped`), so nothing here is
wrong. But a range silently shadowed by an earlier one is the shape of §1's
opening complaint — two readers of the same spec disagreeing about what it says —
and a warning would cost nothing. Left alone because it is a new diagnostic rather
than a law this file is auditing.

**The write side has no laws here.** The paper is about parsing, and so is this
file. BBQ also serializes, recomputes dependent fields, and claims round-trip
properties; those are lens laws (GetPut/PutGet) rather than IPG ones, and they are
kept by `CEKLaw.*`, `CppLaw.*` and `ZCowLaw.*`. No IPG rule is missing from here
because of it, but "IPG conformance" should not be read as "the whole of BBQ is
audited".

## Reading this file

The verdicts are checked by `ctest -R ipg_law_tests`. A **differs** row is a
decision, not a gap; a **not claimed** row is a gap with its reason. If a row's
test is deleted, the row is a claim again rather than a fact.
