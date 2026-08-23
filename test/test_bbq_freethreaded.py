#!/usr/bin/env python3
"""Free-threading stress: the module's shared mutable state, hammered without a GIL.

Run under a free-threaded interpreter. Until the module declares
Py_MOD_GIL_NOT_USED, importing it re-enables the GIL, which would make every
stressor here vacuous — so this refuses to run unless the GIL is actually off
(use `-Xgil=0` to force it while support is still undeclared).

Each stressor targets one piece of state that is shared across threads:

  S1  PyBBQSpec::wops      the lazily lowered writer op-list, one per Spec
  S2  the extern registry  register_extern reallocs the array a parse is reading
  S3  PyBBQResult::zcow    the overlay, lazily created and then mutated
  S4  bbq.build containers in-place list mutation on a shared Struct/Array
  S5  independent parses   the case that must simply be correct AND parallel

A failure is a crash, an interpreter-level corruption, or a value that no
serial interleaving could have produced. Racy-but-benign outcomes (which of two
concurrent writes wins) are not failures.
"""

import sys
import threading
import traceback

import bbq
from bbq import build as b


THREADS = 8
ITERS = 400


def _run(fn, threads=THREADS):
    """Run `fn(i)` on `threads` threads released simultaneously; re-raise the first error."""
    barrier = threading.Barrier(threads)
    errors = []

    def worker(i):
        try:
            barrier.wait()
            fn(i)
        except BaseException:                       # noqa: BLE001 - reported below
            errors.append(traceback.format_exc())

    ts = [threading.Thread(target=worker, args=(i,)) for i in range(threads)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    if errors:
        raise AssertionError(f"{len(errors)} thread(s) failed:\n\n" + "\n".join(errors[:3]))


# ── S1: the lazily lowered writer op-list ────────────────────────────────────
# One Spec, many threads, all reaching emit() for the first time at once. An
# unguarded `if (!wops) wops = lower(...)` lowers twice and leaks one of them;
# a torn read hands run_writer a half-built object.

def s1_wops_cache():
    spec = bbq.compile(_fixture())
    data = bytes([0x03, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00])

    def work(_i):
        for _ in range(20):
            r = spec.parse(data, rule="Arr")
            assert r.emit() == data, "GetPut broke under concurrency"

    _run(work)


# ── S2: the extern registry ──────────────────────────────────────────────────
# register_extern PyMem_Reallocs ext_entries while do_parse holds &ext_table,
# and an override Py_DECREFs a callable a running parse may be calling. This is
# a use-after-free, not merely a lost update.

EXTERN_SPEC = 'Msg = struct { header: uint32, payload: extern("validate", "void") }'
EXTERN_DATA = b"\x01\x00\x00\x00\xAA\xBB\xCC\xDD"


def s2_extern_registry():
    spec = bbq.compile_string(EXTERN_SPEC)
    spec.register_extern("validate", lambda mv: 4)
    stop = threading.Event()

    def churn():
        # Fresh closures under fresh names: grows the array (realloc) and
        # repeatedly overrides "validate" (DECREF of a live callable).
        i = 0
        while not stop.is_set():
            spec.register_extern("validate", lambda mv, k=i: 4)
            i += 1

    def parse(_i):
        for _ in range(ITERS):
            r = spec.parse(EXTERN_DATA)
            assert r.success and r.bytes_consumed == 8, "extern parse corrupted"

    churner = threading.Thread(target=churn, daemon=True)
    churner.start()
    try:
        _run(parse)
    finally:
        stop.set()
        churner.join(timeout=5)


# ── S3: the per-result overlay ───────────────────────────────────────────────
# zcow is created lazily at three sites and then mutated. Concurrent edits to
# ONE result must not crash or corrupt; which write wins is up to the schedule.

def s3_overlay():
    spec = bbq.compile_string("Foo = struct { x: uint8, y: uint8, z: uint8 }")
    data = bytes([1, 2, 3])

    for _ in range(30):
        r = spec.parse(data)

        def work(i):
            for n in range(50):
                r.x = (i * 7 + n) & 0xFF
                r.y = (i * 11 + n) & 0xFF
                out = r.emit()
                assert len(out) == 3, f"overlay produced {len(out)} bytes, want 3"

        _run(work, threads=4)
        # z was never written, so it must still read as authored no matter how
        # x and y interleaved.
        assert int(r.z) == 3, f"untouched field corrupted: z={int(r.z)}"


# ── S4: bbq.build containers ─────────────────────────────────────────────────
# Struct/Array hold Python lists mutated in place; concurrent append on one
# object is a list mutation race.

def s4_build_containers():
    # Array.append is a single PyList_Append, which the interpreter makes atomic.
    for _ in range(30):
        arr = b.Array()

        def work(i):
            for n in range(50):
                arr.append(b.u8((i * 13 + n) & 0xFF))

        _run(work, threads=4)
        assert len(arr) == 4 * 50, f"lost appends: len={len(arr)} want {4 * 50}"
        assert len(bytes(arr)) == 4 * 50, "serialized length disagrees with len()"

    # Struct field assignment is check-then-act across TWO parallel lists: look the name
    # up, then append to `names` and to `children` as separate operations. Interleave two
    # threads there and the lists desynchronize — every field must still have a value.
    for _ in range(60):
        s = b.Struct()

        def work(i):
            for n in range(40):
                s[f"f{i}_{n}"] = b.u8((i * 17 + n) & 0xFF)   # distinct names: pure growth
                s["shared"] = b.u8(i & 0xFF)                 # same name: the lookup races

        _run(work, threads=4)
        keys, vals = list(s.keys()), list(s.values())
        assert len(keys) == len(vals), (
            f"names/children desynchronized: {len(keys)} names, {len(vals)} values")
        assert len(keys) == len(s), f"len() disagrees: len={len(s)} keys={len(keys)}"
        assert len(set(keys)) == len(keys), "a field name was recorded twice"
        bytes(s)                                             # must not crash or read past


# ── S5: independent work ─────────────────────────────────────────────────────
# The reason to support free-threading at all: separate documents, one Spec,
# genuinely parallel. Must be correct.

def s5_independent_parses():
    spec = bbq.compile(_fixture())

    def work(i):
        for n in range(ITERS):
            k = (i + n) % 4 + 1
            data = bytes([k]) + bytes([0x10 + j for j in range(k)])
            r = spec.parse(data, rule="Cnt")
            assert r.success, f"parse failed: {r.error_message}"
            assert int(r.n) == k and len(r.xs) == k
            assert r.emit() == data

    _run(work)


def _fixture():
    import os
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "zcow_reader.bbq")


STRESSORS = [
    ("S1 wops cache", s1_wops_cache),
    ("S2 extern registry", s2_extern_registry),
    ("S3 result overlay", s3_overlay),
    ("S4 build containers", s4_build_containers),
    ("S5 independent parses", s5_independent_parses),
]


def main():
    if not sysconfig_free_threaded():
        print("SKIP: not a free-threaded build", file=sys.stderr)
        return 0
    if sys._is_gil_enabled():
        print("FAIL: the GIL is enabled, so nothing here is actually concurrent.\n"
              "      The module has not declared Py_MOD_GIL_NOT_USED, or this was not\n"
              "      run with -Xgil=0.", file=sys.stderr)
        return 1

    selected = sys.argv[1:]
    failures = 0
    for name, fn in STRESSORS:
        if selected and not any(name.startswith(s) for s in selected):
            continue
        try:
            fn()
        except BaseException as e:                  # noqa: BLE001 - reported, not raised
            failures += 1
            print(f"FAIL {name}: {e}", file=sys.stderr)
        else:
            print(f"ok   {name}")
    return 1 if failures else 0


def sysconfig_free_threaded():
    import sysconfig
    return bool(sysconfig.get_config_var("Py_GIL_DISABLED"))


if __name__ == "__main__":
    sys.exit(main())
