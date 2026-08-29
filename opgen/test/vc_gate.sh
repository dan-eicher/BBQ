#!/bin/sh
# The opgen VC gate: the calc certification plus the seeded-liar reds.
#
# usage: vc_gate.sh <opgen-binary> <opgen-source-dir> <scratch-dir>
#
# 1. calc.def certifies: run_vcs over its manifest ends with ZERO refuted
#    and at least one PROVED row (the pipeline demonstrably ran).
# 2. The seeded-liar defs go RED exactly as designed: the contradicted edge
#    and the dead guard come back REFUTED (with a model on the sat side),
#    and the unassigned-output def is REFUSED by opgen outright.
# 3. The effect-free liar is metered UNVERIFIABLE, never CLOSED.
set -u

OPGEN=$1
SRC=$2
OUT=$3
RUN="$SRC/verify/run_vcs"

die() { echo "vc_gate FAIL: $*" >&2; exit 1; }

# ── 1. calc certification ──────────────────────────────────────────────
mkdir -p "$OUT/calc" || die "mkdir"
"$OPGEN" -i "$SRC/test/data/calc.def" -prefix calc -o "$OUT/calc" \
    > "$OUT/calc.log" 2>&1 || die "opgen refused calc.def (see $OUT/calc.log)"
"$RUN" "$OUT/calc/calc_vc_manifest.txt" > "$OUT/calc_run.log" 2>&1 \
    || die "calc.def REFUTED — see $OUT/calc/calc_vc_results.txt"
grep -q '^PROVED' "$OUT/calc/calc_vc_results.txt" \
    || die "calc run produced no PROVED rows — the pipeline did not run"

# ── 2a. the contradicted edge and the dead guard are REFUTED ───────────
mkdir -p "$OUT/liar" || die "mkdir"
"$OPGEN" -i "$SRC/test/data/vc_liar_edge.def" -prefix liar -o "$OUT/liar" \
    > "$OUT/liar.log" 2>&1 || die "opgen refused vc_liar_edge.def"
if "$RUN" "$OUT/liar/liar_vc_manifest.txt" > "$OUT/liar_run.log" 2>&1; then
    die "seeded liars NOT refuted — the gate cannot go red"
fi
grep -q '^REFUTED .* liar_add edge' "$OUT/liar/liar_vc_results.txt" \
    || die "liar_add edge not REFUTED"
grep '^REFUTED .* liar_add edge' "$OUT/liar/liar_vc_results.txt" | grep -q 'define-fun' \
    || die "liar_add refutation carries no model"
grep -q '^REFUTED .* dead_guard guard-sat' "$OUT/liar/liar_vc_results.txt" \
    || die "dead_guard guard-sat not REFUTED"

# ── 2b. the unassigned output is REFUSED by the emitter ────────────────
mkdir -p "$OUT/half" || die "mkdir"
if "$OPGEN" -i "$SRC/test/data/vc_liar_unassigned.def" -prefix half -o "$OUT/half" \
    > "$OUT/half.log" 2>&1; then
    die "unassigned-output def NOT refused"
fi
grep -q "not assigned on every path" "$OUT/half.log" \
    || die "refusal does not name the unassigned output"

# ── 3. the effect-free liar is metered ─────────────────────────────────
mkdir -p "$OUT/sneaky" || die "mkdir"
"$OPGEN" -i "$SRC/test/data/vc_liar_effectfree.def" -prefix sneaky -o "$OUT/sneaky" \
    > "$OUT/sneaky.log" 2>&1 || die "opgen refused vc_liar_effectfree.def"
grep -q '^UNVERIFIABLE sneaky' "$OUT/sneaky/sneaky_vc_manifest.txt" \
    || die "sneaky not metered UNVERIFIABLE"
grep -q '^CLOSED sneaky' "$OUT/sneaky/sneaky_vc_manifest.txt" \
    && die "sneaky wrongly classified CLOSED"

echo "vc_gate: all checks passed"
