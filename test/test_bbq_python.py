"""Tests for the bbq Python extension module."""

import gc
import os
import struct
import sys
import tempfile

import pytest

# The built module lives in the build directory; allow the caller to point at it
# via PYTHONPATH or by running from the build dir.
import bbq


# ── Helpers ──────────────────────────────────────────────────────────────────

TRIVIAL_SPEC = "Foo = struct { magic: uint32 }"

BIG_ENDIAN_SPEC = "@endian big\nFoo = struct { magic: uint32 }"

MULTI_FIELD_SPEC = """\
Msg = struct {
    tag:    uint8,
    length: uint16,
    value:  uint32
}
"""

ARRAY_SPEC = """\
Arr = struct {
    count: uint8,
    items: array<uint16>[count]
}
"""

NESTED_SPEC = """\
Inner = struct { x: uint8, y: uint8 }
Outer = struct { hdr: Inner, z: uint16 }
"""

COMPUTE_SPEC = """\
Foo = struct {
    raw: uint8,
    doubled: compute(raw * 2 : uint8)
}
"""

STRING_SPEC = """\
Foo = struct {
    len: uint8,
    name: string[len]
}
"""

BOOL_SPEC = "Foo = struct { flag: bool }"

FLOAT_SPEC = "Foo = struct { val: float32 }"

SIGNED_SPEC = "Foo = struct { val: int16 }"

UNION_SPEC = """\
@endian big
SmallMsg = struct { tag: uint8 where (tag == 0x01), val: uint8 }
LargeMsg = struct { tag: uint8 where (tag == 0x02), val: uint16be }
Msg = union { small: SmallMsg, large: LargeMsg }
"""


# ── Compile ──────────────────────────────────────────────────────────────────

class TestCompile:
    def test_compile_string(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        assert isinstance(spec, bbq.Spec)

    def test_compile_file(self, tmp_path):
        p = tmp_path / "test.bbq"
        p.write_text(TRIVIAL_SPEC)
        spec = bbq.compile(str(p))
        assert isinstance(spec, bbq.Spec)

    def test_compile_string_error(self):
        with pytest.raises(bbq.ParseError):
            bbq.compile_string("not valid bbq !!!")

    def test_compile_file_missing(self):
        with pytest.raises(OSError):
            bbq.compile("/nonexistent/path.bbq")

    def test_rules_property(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        assert spec.rules == ["Foo"]

    def test_default_endian_little(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        assert spec.default_endian == "little"

    def test_default_endian_big(self):
        spec = bbq.compile_string(BIG_ENDIAN_SPEC)
        assert spec.default_endian == "big"

    def test_multiple_rules(self):
        spec = bbq.compile_string(NESTED_SPEC)
        assert "Inner" in spec.rules
        assert "Outer" in spec.rules


# ── Parse basics ─────────────────────────────────────────────────────────────

class TestParse:
    def test_parse_bytes(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x89PNG", rule="Foo")
        assert result.success

    def test_parse_bytearray(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(bytearray(b"\x89PNG"), rule="Foo")
        assert result.success

    def test_parse_memoryview(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(memoryview(b"\x89PNG"), rule="Foo")
        assert result.success

    def test_parse_default_rule(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x89PNG")
        assert result.success

    def test_parse_unknown_rule(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        with pytest.raises(bbq.ParseError, match="unknown rule"):
            spec.parse(b"\x89PNG", rule="NoSuchRule")

    def test_bytes_consumed(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x89PNG")
        assert result.bytes_consumed == 4

    def test_parse_failure(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        result = spec.parse(b"\x01")  # too short
        assert not result.success

    def test_error_fields_on_failure(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        result = spec.parse(b"\x01")
        assert not result.success
        assert result.error_offset >= 0

    def test_success_error_message_none(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x89PNG")
        assert result.error_message is None

    def test_switch_default_reject(self):
        # `default: reject;` — author-visible explicit-fail. A tag
        # value not in the case list must fail the parse with a
        # meaningful error message. Uses peek() as the discriminator
        # (read tag without consuming, then dispatch — the canonical
        # tag-prefixed-union shape used by cap.bbq's CpInfo etc.).
        spec = bbq.compile_string(
            "Doc = switch(peek()) {\n"
            "    1: Body;\n"
            "    2: Body;\n"
            "    default: reject;\n"
            "}\n"
            "Body = struct { tag: uint8, payload: uint8 }"
        )
        # Tag 1 — accepted.
        ok = spec.parse(b"\x01\x42")
        assert ok.success
        # Tag 99 — rejected at parse time. The exact error message
        # is backend-specific (C backend says "rejected", CEK says
        # "switch: no matching case"); the test asserts only the
        # core guarantee — the parse fails.
        fail = spec.parse(b"\x63\x42")
        assert not fail.success

    def test_reject_is_not_a_reserved_identifier(self):
        # `reject` is a contextual keyword — only meaningful as the
        # body of a `default:` clause. As a struct field name it
        # must remain a valid identifier.
        spec = bbq.compile_string(
            "Flags = struct { reject: uint8, accept: uint8 }"
        )
        result = spec.parse(b"\x01\x02")
        assert result.success


# ── Node attribute access ────────────────────────────────────────────────────

class TestNodeAccess:
    def test_attribute_access(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x89PNG")
        node = result.magic
        assert isinstance(node, bbq.Node)

    def test_missing_attribute(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x89PNG")
        with pytest.raises(AttributeError):
            result.nonexistent

    def test_nested_access(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        assert result.success
        assert int(result.hdr.x) == 1
        assert int(result.hdr.y) == 2


# ── Value materialization ────────────────────────────────────────────────────

class TestValues:
    def test_int_uint32le(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x01\x00\x00\x00")
        assert int(result.magic) == 1

    def test_int_uint32be(self):
        spec = bbq.compile_string(BIG_ENDIAN_SPEC)
        result = spec.parse(b"\x00\x00\x00\x01")
        assert int(result.magic) == 1

    def test_int_uint8(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        assert int(result.tag) == 0x42

    def test_int_uint16(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        assert int(result.length) == 7

    def test_int_signed(self):
        spec = bbq.compile_string(SIGNED_SPEC)
        data = struct.pack("<h", -1234)
        result = spec.parse(data)
        assert int(result.val) == -1234

    def test_float(self):
        spec = bbq.compile_string(FLOAT_SPEC)
        data = struct.pack("<f", 3.14)
        result = spec.parse(data)
        assert abs(float(result.val) - 3.14) < 0.001

    def test_float_promotion(self):
        """int nodes should be convertible to float."""
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x05\x00\x00\x00")
        assert float(result.magic) == 5.0

    def test_bool_true(self):
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x01")
        assert result.flag.value is True

    def test_bool_false(self):
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x00")
        assert result.flag.value is False

    def test_str_string(self):
        spec = bbq.compile_string(STRING_SPEC)
        data = b"\x05hello"
        result = spec.parse(data)
        assert str(result.name) == "hello"

    def test_str_int_node(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x2a\x00\x00\x00")
        assert str(result.magic) == "42"

    def test_computed(self):
        spec = bbq.compile_string(COMPUTE_SPEC)
        result = spec.parse(b"\x05")
        assert int(result.doubled) == 10

    def test_value_property_int(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x2a\x00\x00\x00")
        assert result.magic.value == 42

    def test_value_property_string(self):
        spec = bbq.compile_string(STRING_SPEC)
        result = spec.parse(b"\x03abc")
        assert result.name.value == "abc"


# ── Node properties ──────────────────────────────────────────────────────────

class TestNodeProperties:
    def test_offset(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        assert result.tag.offset == (0, 1)
        assert result.length.offset == (1, 3)
        assert result.value.offset == (3, 7)

    def test_raw(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        data = b"\x89PNG"
        result = spec.parse(data)
        assert result.magic.raw == b"\x89PNG"

    def test_capture_type(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        assert result.magic.capture_type in ("uint32le", "uint32be")

    def test_name(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        assert result.magic.name == "magic"


# ── ZCow mutation (copy-on-write through the Python face) ─────────────────────
# `result.field = v` / `node.field = v` owns that field, so it carries its own value
# instead of the span it was parsed from; everything off the path keeps its span and
# is still shared. emit() settles the dependent fields the edit invalidated before
# serializing — see TestLensLaws for the laws that pin that down.

class TestZCowMutation:
    def test_scalar_set(self):
        spec = bbq.compile_string("Foo = struct { x: uint8, y: uint16le, z: uint8 }")
        r = spec.parse(bytes([5, 2, 0, 9]))
        assert int(r.x) == 5 and int(r.y) == 2 and int(r.z) == 9
        r.x = 42
        assert int(r.x) == 42      # override read back
        assert int(r.y) == 2 and int(r.z) == 9   # siblings untouched

    def test_nested_set(self):
        spec = bbq.compile_string(
            "Inner = struct { a: uint8, b: uint8 }\n"
            "Outer = struct { tag: uint8, inner: Inner }")
        o = spec.parse(bytes([7, 1, 2]), rule="Outer")
        assert int(o.inner.a) == 1 and int(o.inner.b) == 2
        o.inner.a = 99             # mutate nested field via the shared overlay
        assert int(o.inner.a) == 99
        assert int(o.inner.b) == 2 and int(o.tag) == 7

    def test_set_unknown_field_raises(self):
        spec = bbq.compile_string("Foo = struct { x: uint8 }")
        r = spec.parse(bytes([1]))
        with pytest.raises(AttributeError):
            r.nope = 5

    def test_deltas_diff(self):
        spec = bbq.compile_string(
            "Inner = struct { a: uint8, b: uint8 }\n"
            "Foo = struct { tag: uint8, m: uint16le, inner: Inner }")
        r = spec.parse(bytes([7, 2, 0, 1, 2]), rule="Foo")
        assert r.deltas() == []              # unmutated -> no deltas
        r.tag = 99
        r.inner.a = 42
        paths = {d["path"]: (d["old"], d["new"]) for d in r.deltas()}
        assert paths == {"$.tag": (7, 99), "$.inner.a": (1, 42)}
        # offsets are present and point at the changed bytes
        off = {d["path"]: d["offset"] for d in r.deltas()}
        assert off["$.tag"] == (0, 1) and off["$.inner.a"] == (3, 4)

    def test_set_all_leaf_types(self):
        # node.field = v works for every leaf type, not just ints.
        spec = bbq.compile_string(
            "@endian little\nP = struct { b: bool, n: uint16le, f: float32, data: bytes[2] }")
        r = spec.parse(bytes([1, 5, 0, 0, 0, 0x80, 0x3F, 0xAA, 0xBB]))
        r.b = False
        r.n = 300
        r.f = 2.5
        r.data = b"\x01\x02"
        r2 = spec.parse(r.emit())
        assert (int(r2.n) == 300 and abs(float(r2.f) - 2.5) < 1e-6
                and bytes(r2.data) == b"\x01\x02" and not bool(r2.b))

    def test_subscript_set(self):
        spec = bbq.compile_string("Tab = struct { n: uint8, items: array<uint16le>[n] }")
        r = spec.parse(bytes([2, 1, 0, 2, 0]))
        r.items[0] = 0x0102
        r.items[1] = 0x0304
        r2 = spec.parse(r.emit())
        assert [int(x) for x in r2.items] == [0x0102, 0x0304]

    def test_delete_element(self):
        # del removes the element; the container's len updates live (like a list). `n` is a
        # DEPENDENT field — derived from the array, not maintained by the caller — so emit()
        # recomputes it.
        spec = bbq.compile_string("Tab = struct { n: uint8, items: array<uint8>[n] }")
        r = spec.parse(bytes([3, 10, 20, 30]))
        del r.items[1]
        assert len(r.items) == 2                            # len is live, like a list
        r2 = spec.parse(r.emit())                           # structure is read back after reparse
        assert int(r2.n) == 2 and [int(x) for x in r2.items] == [10, 30]

    def test_append_scalar_from_sibling_type(self):
        # a scalar append takes the array's existing element type (pure data, no grammar);
        # len updates live, and the dependent count follows the array.
        spec = bbq.compile_string("Tab = struct { n: uint8, items: array<uint8>[n] }")
        r = spec.parse(bytes([1, 10]))
        r.items.append(20)
        r.items.append(30)
        assert len(r.items) == 3
        r2 = spec.parse(r.emit())
        assert int(r2.n) == 3 and [int(x) for x in r2.items] == [10, 20, 30]

    def test_append_bytes_element(self):
        # variable-width (LEB) and composite elements have no by-value form — ZCow is
        # byte-level, so the caller supplies the bytes. (300 as uleb128 = b"\xac\x02".)
        # The count is dependent either way and is recomputed on emit.
        sl = bbq.compile_string("T = struct { n: uint8, xs: array<uleb128>[n] }")
        rl = sl.parse(bytes([1, 5]))
        rl.xs.append(b"\xac\x02")
        assert len(rl.xs) == 2
        r2 = sl.parse(rl.emit())
        assert int(r2.n) == 2 and [int(x) for x in r2.xs] == [5, 300]
        # a struct element, appended as raw bytes
        ss = bbq.compile_string(
            "It = struct { v: uint8, w: uint8 }\nT = struct { n: uint8, items: array<It>[n] }")
        rs = ss.parse(bytes([1, 1, 2]), rule="T")
        rs.items.append(b"\x03\x04")
        r3 = ss.parse(rs.emit(), rule="T")
        assert int(r3.n) == 2 and int(r3.items[1].v) == 3 and int(r3.items[1].w) == 4

    def test_splice_element_bytes(self):
        # arr[i] = b"..." replaces a composite element with raw bytes (the ZCow splice /
        # splice): path-copy, re-serialize on emit, neighbors intact.
        ss = bbq.compile_string(
            "It = struct { v: uint8, w: uint8 }\nT = struct { n: uint8, items: array<It>[n] }")
        r = ss.parse(bytes([2, 1, 2, 3, 4]), rule="T")     # items = [{1,2}, {3,4}]
        r.items[0] = b"\x09\x08"                            # splice element 0
        r2 = ss.parse(r.emit(), rule="T")
        assert int(r2.n) == 2
        assert int(r2.items[0].v) == 9 and int(r2.items[0].w) == 8
        assert int(r2.items[1].v) == 3 and int(r2.items[1].w) == 4   # neighbor preserved

    def test_splice_compose_with_remove_and_append(self):
        # splice + remove + append compose — each is a write into the one tree, so the
        # length, the contents and what serializes all stay in step with each other.
        ss = bbq.compile_string(
            "It = struct { v: uint8, w: uint8 }\nT = struct { n: uint8, items: array<It>[n] }")
        r = ss.parse(bytes([3, 1, 2, 3, 4, 5, 6]), rule="T")   # [{1,2},{3,4},{5,6}]
        r.items[1] = b"\x1e\x28"             # splice the middle element -> (30, 40)
        del r.items[0]                       # remove the first (drops its subtree)
        r.items.append(b"\x07\x08")          # grow -> (7, 8)
        assert len(r.items) == 3
        r2 = ss.parse(r.emit(), rule="T")
        got = [(int(it.v), int(it.w)) for it in r2.items]
        assert int(r2.n) == 3 and got == [(30, 40), (5, 6), (7, 8)]

    def test_emit_roundtrip(self):
        spec = bbq.compile_string("Foo = struct { x: uint8, y: uint16le, z: uint32be }")
        data = bytes([5, 2, 0, 0, 0, 0, 9])
        r = spec.parse(data)
        assert r.emit() == data            # unmutated -> byte-identical
        r.x = 42
        r.z = 0x11223344
        out = r.emit()
        assert out != data
        r2 = spec.parse(out)               # mutation survives the round-trip
        assert int(r2.x) == 42 and int(r2.y) == 2 and int(r2.z) == 0x11223344


# ── Lens laws ────────────────────────────────────────────────────────────────
# parse/emit is a lens (Foster et al., TOPLAS 2007): get = parse (bytes -> capture
# view), put = emit (edited view x ORIGINAL bytes -> new bytes). `put` takes the
# original because `get` discards information — which is exactly what the spans a
# document keeps are. Two laws:
#
#   GetPut   put(get(c), c) = c      an unedited re-emit is byte-identical
#   PutGet   get(put(a, c)) = a      re-parsing an edit yields the edit you made
#
# PutGet is what forces dependent fields (Nail's term: length/count/offset fields,
# "not exposed in the data model, but instead transparently computed"). An append
# that leaves the count field stale fails it. These mirror the C++ ZCow writer's
# CppWriterMutateRoundTrip suite case for case — the same fixture, the same edits.

class TestLensLaws:
    """get/put must be well behaved: GetPut byte-exact, PutGet through a re-parse."""

    @classmethod
    def setup_class(cls):
        cls.spec = bbq.compile(ZCOW_READER_BBQ)

    def _lens(self, rule, data, edit):
        """GetPut on `data`, then apply `edit` and return the re-parsed result (PutGet)."""
        assert self.spec.parse(data, rule=rule).emit() == data, "GetPut: re-emit not byte-identical"
        r = self.spec.parse(data, rule=rule)
        assert r.success
        edit(r)
        out = r.emit()
        r2 = self.spec.parse(out, rule=rule)
        assert r2.success, f"re-parse of the edited document failed: {r2.error_message}"
        assert r2.bytes_consumed == len(out), (
            f"edited document not fully consumed: {r2.bytes_consumed} of {len(out)}")
        return r2

    def test_scalar_value_edit(self):
        # (1) same-width scalar edit: no dependent field involved.
        data = bytes([0x12, 0x56, 0x34, 0x00, 0x01, 0x00, 0x00, 0x01, 0x02, 0xFC, 0xFF, 0xFF, 0xFF])
        def edit(r):
            r.u8 = 0x99
        r2 = self._lens("Flat", data, edit)
        assert int(r2.u8) == 0x99 and int(r2.u16) == 0x3456

    def test_array_element_value_edit(self):
        # (2) element value edit — structure unchanged, count untouched.
        data = bytes([0x03, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00])
        def edit(r):
            r.xs[1] = 0x0099
        r2 = self._lens("Arr", data, edit)
        assert len(r2.xs) == 3 and int(r2.xs[1]) == 0x0099

    def test_array_remove_recomputes_count(self):
        # (3) remove: the count field is derived, so `n` must fall to 2 on its own.
        data = bytes([0x03, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00])
        def edit(r):
            del r.xs[0]
        r2 = self._lens("Arr", data, edit)
        assert int(r2.n) == 2
        assert [int(x) for x in r2.xs] == [0x0020, 0x0030]

    def test_array_append_recomputes_count(self):
        # (4) append: `n` must rise to 4 without the caller touching it.
        data = bytes([0x03, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00])
        def edit(r):
            r.xs.append(0x0040)
        r2 = self._lens("Arr", data, edit)
        assert int(r2.n) == 4
        assert len(r2.xs) == 4 and int(r2.xs[3]) == 0x0040

    def test_path_count_recomputed(self):
        # (5) the count lives in a sub-struct — `array<uint8>[h.n]`, so the enforcement
        # has to resolve the path, not just a sibling name.
        data = bytes([0x03, 0xAA, 0xBB, 0xCC])
        def edit(r):
            r.xs.append(0xDD)
        r2 = self._lens("Np", data, edit)
        assert int(r2.h.n) == 4
        assert len(r2.xs) == 4 and int(r2.xs[3]) == 0xDD

    def test_rest_size_recomputed(self):
        # (6) @rest: `sz` is the byte length of the window that follows it, so growing
        # the window's content must grow `sz` (3 -> 4).
        data = bytes([0x03, 10, 20, 30])
        def edit(r):
            r.xs.append(40)
        r2 = self._lens("RestEof", data, edit)
        assert int(r2.sz) == 4
        assert [int(x) for x in r2.xs] == [10, 20, 30, 40]

    def test_nested_per_element_count_recomputed(self):
        # (7) the walk must DESCEND into array element bodies: editing gs[0].xs fixes
        # gs[0].n, leaves the outer count and the sibling element alone.
        data = bytes([0x02, 0x01, 0x11, 0x02, 0x22, 0x33])
        def edit(r):
            r.gs[0].xs.append(0x99)
        r2 = self._lens("NestArr", data, edit)
        assert int(r2.m) == 2                                  # outer count unchanged
        assert int(r2.gs[0].n) == 2                            # inner count fixed
        assert [int(x) for x in r2.gs[0].xs] == [0x11, 0x99]
        assert int(r2.gs[1].n) == 2                            # sibling intact
        assert [int(x) for x in r2.gs[1].xs] == [0x22, 0x33]


# ── Union backtracking ───────────────────────────────────────────────────────
# Guards the CEK path through the union/choice kont graph (BeginChoice / OnFail /
# CommitChoice / BeginVariant): try an arm, and on a where-fail backtrack and try
# the next. Only the matched variant appears in the capture tree.

class TestUnion:
    def test_second_variant_after_backtrack(self):
        spec = bbq.compile_string(UNION_SPEC)
        result = spec.parse(bytes([0x02, 0xAB, 0xCD]), rule="Msg")
        assert result.success
        assert result.bytes_consumed == 3
        # The first arm (small, tag==1) fails on 0x02; backtrack picks large.
        assert result.large.tag.value == 0x02
        assert result.large.val.value == 0xABCD
        assert not hasattr(result, "small")

    def test_first_variant(self):
        spec = bbq.compile_string(UNION_SPEC)
        result = spec.parse(bytes([0x01, 0x42]), rule="Msg")
        assert result.success
        assert result.bytes_consumed == 2
        assert result.small.tag.value == 0x01
        assert result.small.val.value == 0x42
        assert not hasattr(result, "large")

    def test_no_variant_matches(self):
        spec = bbq.compile_string(UNION_SPEC)
        result = spec.parse(bytes([0x09, 0x00]), rule="Msg")
        assert not result.success


# ── Subscript, len, contains ─────────────────────────────────────────────────

class TestSubscript:
    def test_subscript_int(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        assert int(result.items[0]) == 1
        assert int(result.items[1]) == 2
        assert int(result.items[2]) == 3

    def test_subscript_negative(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        assert int(result.items[-1]) == 3

    def test_subscript_string(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        assert int(result.hdr["x"]) == 1

    def test_result_subscript_string(self):
        """ParseResult supports result['field'] for Python-keyword field names."""
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        assert int(result["tag"]) == 0x42
        assert int(result["length"]) == 7

    def test_result_subscript_int(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        assert int(result[0]) == 0x42  # first child by index

    def test_result_len(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        assert len(result) == 3  # tag, length, value

    def test_subscript_out_of_range(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x01\x42\x00"
        result = spec.parse(data)
        with pytest.raises(IndexError):
            result.items[99]

    def test_subscript_key_missing(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        with pytest.raises(KeyError):
            result.hdr["nonexistent"]

    def test_len_array(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        assert len(result.items) == 3

    def test_len_struct(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        # Struct node accessed via result — need nested struct
        spec2 = bbq.compile_string(NESTED_SPEC)
        data2 = b"\x01\x02\x03\x00"
        r2 = spec2.parse(data2, rule="Outer")
        assert len(r2.hdr) == 2  # x, y

    def test_contains(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        assert "x" in result.hdr
        assert "z" not in result.hdr


# ── Iteration ────────────────────────────────────────────────────────────────

class TestIteration:
    def test_iterate_array(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        values = [int(node) for node in result.items]
        assert values == [1, 2, 3]

    def test_iterate_struct(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        fields = list(result.hdr)
        assert len(fields) == 2
        # Struct yields (name, node) tuples
        assert fields[0][0] == "x"
        assert fields[1][0] == "y"
        assert int(fields[0][1]) == 1
        assert int(fields[1][1]) == 2


# ── Buffer protocol ──────────────────────────────────────────────────────────

class TestBufferProtocol:
    def test_bytes_node(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x89PNG")
        assert bytes(result.magic) == b"\x89PNG"

    def test_memoryview_node(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x89PNG")
        mv = memoryview(result.magic)
        assert bytes(mv) == b"\x89PNG"
        assert mv.readonly


# ── Rich comparison ──────────────────────────────────────────────────────────

class TestRichCompare:
    def test_int_eq(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x2a\x00\x00\x00")
        assert result.magic == 42
        assert not (result.magic == 43)

    def test_int_ne(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x2a\x00\x00\x00")
        assert result.magic != 0

    def test_int_lt(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x2a\x00\x00\x00")
        assert result.magic < 100
        assert not (result.magic < 10)

    def test_string_eq(self):
        spec = bbq.compile_string(STRING_SPEC)
        result = spec.parse(b"\x05hello")
        assert result.name == "hello"
        assert result.name != "world"

    def test_bool_eq(self):
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x01")
        assert result.flag == True  # noqa: E712

    def test_repr(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        r = repr(result.magic)
        assert "bbq.Node" in r
        assert "magic" in r


# ── parse_file ───────────────────────────────────────────────────────────────

class TestParseFile:
    def test_parse_file(self, tmp_path):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        p = tmp_path / "test.bin"
        p.write_bytes(b"\x2a\x00\x00\x00")
        result = spec.parse_file(str(p))
        assert result.success
        assert int(result.magic) == 42

    def test_parse_file_missing(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        with pytest.raises(OSError):
            spec.parse_file("/nonexistent/file.bin")

    def test_parse_file_with_rule(self, tmp_path):
        spec = bbq.compile_string(NESTED_SPEC)
        p = tmp_path / "test.bin"
        p.write_bytes(b"\x01\x02\x03\x00")
        result = spec.parse_file(str(p), rule="Outer")
        assert result.success
        assert int(result.hdr.x) == 1


# ── nb_bool ──────────────────────────────────────────────────────────────────

class TestNbBool:
    def test_bool_node_true(self):
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x01")
        assert bool(result.flag)

    def test_bool_node_false(self):
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x00")
        assert not bool(result.flag)

    def test_int_node_truthy(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x01\x00\x00\x00")
        assert bool(result.magic)

    def test_int_node_zero_falsy(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        assert not bool(result.magic)


# ── Bool str capitalization ───────────────────────────────────────────────────

class TestBoolStr:
    def test_str_bool_true(self):
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x01")
        assert str(result.flag) == "True"

    def test_str_bool_false(self):
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x00")
        assert str(result.flag) == "False"

    def test_format_bool_consistent(self):
        """str() and format() should agree on capitalization."""
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x01")
        assert str(result.flag) == f"{result.flag}"


# ── Leaf TypeError guards ────────────────────────────────────────────────────

class TestLeafTypeErrors:
    def test_len_leaf_raises(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        with pytest.raises(TypeError, match="has no len"):
            len(result.magic)

    def test_iter_leaf_raises(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        with pytest.raises(TypeError, match="not iterable"):
            list(result.magic)

    def test_subscript_leaf_raises(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        with pytest.raises(TypeError, match="not subscriptable"):
            result.magic[0]

    def test_a_selector_over_a_leaf_selects_nothing(self):
        """Changed deliberately when the selector vocabulary landed.

        A slice is a SELECTOR, and a selector over something with nothing to
        select answers with an empty nodelist rather than raising — that is what
        lets `root[..., "x"]` walk a whole document without the caller guarding
        every leaf it meets. Asking a leaf for a named field or an index is a
        different act, and those still raise (above and below).
        """
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        assert len(result.magic[0:1]) == 0
        assert len(result.magic[:]) == 0
        assert len(result.magic[..., "anything"]) == 0
        assert len(result.magic[...]) == 1, "a leaf is still its own descendant"

    def test_string_key_leaf_raises(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        with pytest.raises(TypeError, match="not subscriptable"):
            result.magic["foo"]


# ── ParseResult repr ─────────────────────────────────────────────────────────

class TestResultRepr:
    def test_repr_success(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        r = repr(result)
        assert "ok" in r
        assert "7 bytes" in r
        assert "tag" in r
        assert "length" in r

    def test_repr_failure(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        result = spec.parse(b"\x01")
        r = repr(result)
        assert "failed" in r


# ── ParseResult iteration and contains ────────────────────────────────────────

class TestResultIteration:
    def test_iter_result(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        fields = list(result)
        assert len(fields) == 3
        assert fields[0][0] == "tag"
        assert int(fields[0][1]) == 0x42

    def test_contains_result(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        assert "tag" in result
        assert "length" in result
        assert "nonexistent" not in result


# ── ParseResult root property ─────────────────────────────────────────────────

class TestResultRoot:
    def test_root_is_node(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        root = result.root
        assert isinstance(root, bbq.Node)

    def test_root_has_children(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        root = result.root
        assert "tag" in root
        assert int(root["tag"]) == 0x42


# ── Node unhashable ───────────────────────────────────────────────────────────

class TestNodeHash:
    def test_node_not_hashable(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        with pytest.raises(TypeError, match="unhashable"):
            hash(result.magic)


# ── Array .value returns lazy Node ────────────────────────────────────────────

class TestArrayValueLazy:
    def test_array_value_is_node(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        val = result.items.value
        assert isinstance(val, bbq.Node)

    def test_array_value_iterable(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        val = result.items.value
        values = [int(n) for n in val]
        assert values == [1, 2, 3]


# ── __dir__ ───────────────────────────────────────────────────────────────────

class TestDir:
    def test_dir_node(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        d = dir(result.hdr)
        assert "x" in d
        assert "y" in d
        assert "offset" in d
        assert "raw" in d
        assert "value" in d
        assert "keys" in d

    def test_dir_result(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        d = dir(result)
        assert "success" in d
        assert "bytes_consumed" in d
        assert "tag" in d
        assert "length" in d
        assert "value" in d


# ── hex/bin via __format__ ────────────────────────────────────────────────────

class TestHexBin:
    def test_format_hex_alt(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x2a\x00\x00\x00")
        assert f"{result.magic:#x}" == "0x2a"

    def test_format_bin_alt(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x05\x00\x00\x00")
        assert f"{result.magic:#b}" == "0b101"

    def test_as_list_index_via_int(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x02\x00\x00\x00")
        items = ["a", "b", "c", "d"]
        assert items[int(result.magic)] == "c"


# ── __format__ ────────────────────────────────────────────────────────────────

class TestFormat:
    def test_format_hex(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x2a\x00\x00\x00")
        assert f"{result.magic:08x}" == "0000002a"

    def test_format_float(self):
        spec = bbq.compile_string(FLOAT_SPEC)
        data = struct.pack("<f", 3.14)
        result = spec.parse(data)
        assert f"{result.val:.1f}" == "3.1"

    def test_format_string(self):
        spec = bbq.compile_string(STRING_SPEC)
        data = b"\x05hello"
        result = spec.parse(data)
        assert f"{result.name:>10s}" == "     hello"

    def test_format_empty(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x2a\x00\x00\x00")
        assert format(result.magic, "") == "42"


# ── dict methods ──────────────────────────────────────────────────────────────

class TestDictMethods:
    def test_keys(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        assert result.hdr.keys() == ["x", "y"]

    def test_values(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        vals = result.hdr.values()
        assert len(vals) == 2
        assert int(vals[0]) == 1
        assert int(vals[1]) == 2

    def test_items(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        pairs = result.hdr.items()
        assert len(pairs) == 2
        assert pairs[0][0] == "x"
        assert int(pairs[0][1]) == 1
        assert pairs[1][0] == "y"
        assert int(pairs[1][1]) == 2

    def test_dict_conversion(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        d = dict(result.hdr)
        assert "x" in d
        assert "y" in d
        assert int(d["x"]) == 1


# ── Slicing ───────────────────────────────────────────────────────────────────

class TestSlice:
    def test_slice_array(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        first_two = result.items[0:2]
        assert len(first_two) == 2
        assert int(first_two[0]) == 1
        assert int(first_two[1]) == 2

    def test_slice_step(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        every_other = result.items[::2]
        assert len(every_other) == 2
        assert int(every_other[0]) == 1
        assert int(every_other[1]) == 3

    def test_slice_reverse(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        rev = result.items[::-1]
        assert len(rev) == 3
        assert int(rev[0]) == 3
        assert int(rev[2]) == 1

    def test_result_slice(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        first_two = result[0:2]
        assert len(first_two) == 2
        assert int(first_two[0]) == 0x42


# ── .value property coverage ─────────────────────────────────────────────────

class TestValueProperty:
    def test_value_float(self):
        """`.value` on a float node returns a Python float."""
        spec = bbq.compile_string(FLOAT_SPEC)
        data = struct.pack("<f", 2.5)
        result = spec.parse(data)
        v = result.val.value
        assert isinstance(v, float)
        assert abs(v - 2.5) < 0.001

    def test_value_struct(self):
        """`.value` on a struct node returns lazy Node (same as array)."""
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        v = result.hdr.value
        assert isinstance(v, bbq.Node)
        assert int(v.x) == 1

    def test_value_bool_true(self):
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x01")
        assert result.flag.value is True

    def test_value_bool_false(self):
        spec = bbq.compile_string(BOOL_SPEC)
        result = spec.parse(b"\x00")
        assert result.flag.value is False


# ── Type conversion errors ───────────────────────────────────────────────────

class TestTypeConversionErrors:
    def test_int_on_string_raises(self):
        spec = bbq.compile_string(STRING_SPEC)
        result = spec.parse(b"\x05hello")
        with pytest.raises(TypeError, match="cannot convert"):
            int(result.name)

    def test_int_on_struct_raises(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        with pytest.raises(TypeError, match="cannot convert"):
            int(result.hdr)

    def test_float_on_string_raises(self):
        spec = bbq.compile_string(STRING_SPEC)
        result = spec.parse(b"\x05hello")
        with pytest.raises(TypeError, match="cannot convert"):
            float(result.name)

    def test_float_on_struct_raises(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        with pytest.raises(TypeError, match="cannot convert"):
            float(result.hdr)


# ── str() on containers ──────────────────────────────────────────────────────

class TestStrContainers:
    def test_str_struct(self):
        """str() on a struct node falls through to repr."""
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        s = str(result.hdr)
        assert "bbq.Node" in s
        assert "struct" in s

    def test_str_array(self):
        """str() on an array node falls through to repr."""
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        s = str(result.items)
        assert "bbq.Node" in s
        assert "array" in s

    def test_str_float(self):
        spec = bbq.compile_string(FLOAT_SPEC)
        data = struct.pack("<f", 3.14)
        result = spec.parse(data)
        s = str(result.val)
        assert "3.14" in s


# ── Rich comparison on structs ───────────────────────────────────────────────

class TestRichCompareStruct:
    def test_struct_eq_self(self):
        """Struct node == same struct node (identity)."""
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        node = result.hdr
        assert node == node

    def test_struct_ne_different(self):
        """Different struct nodes are not equal."""
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        # root and hdr are different struct nodes
        assert result.root != result.hdr

    def test_struct_lt_not_implemented(self):
        """Ordering on structs raises TypeError."""
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        with pytest.raises(TypeError):
            result.hdr < result.hdr

    def test_struct_eq_non_node(self):
        """Struct compared to non-Node returns NotImplemented (False)."""
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        assert result.hdr != 42
        assert result.hdr != "hello"


# ── Subscript bad key type ───────────────────────────────────────────────────

class TestSubscriptBadKeyType:
    def test_node_float_key_raises(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        with pytest.raises(TypeError, match="integers or strings"):
            result.items[3.14]

    def test_result_float_key_raises(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        with pytest.raises(TypeError, match="integers or strings"):
            result[3.14]


# ── ParseResult subscript edge cases ─────────────────────────────────────────

class TestResultSubscriptEdge:
    def test_result_negative_index(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        assert int(result[-1]) == 1  # last field is 'value'

    def test_result_key_missing(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        with pytest.raises(KeyError):
            result["nonexistent"]

    def test_result_index_out_of_range(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x42\x07\x00\x01\x00\x00\x00"
        result = spec.parse(data)
        with pytest.raises(IndexError):
            result[99]


# ── Error message on failure ─────────────────────────────────────────────────

class TestErrorMessage:
    def test_error_message_on_failure(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        result = spec.parse(b"\x01")
        assert not result.success
        # error_message should be a string (not None) on failure
        assert result.error_message is None or isinstance(result.error_message, str)

    def test_error_offset_valid(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        result = spec.parse(b"\x01")
        assert isinstance(result.error_offset, int)
        assert result.error_offset >= 0


# ── Failed parse result operations ───────────────────────────────────────────

class TestFailedResult:
    def test_root_on_failure(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        result = spec.parse(b"\x01")
        # root may be None on failure (depends on VM behavior)
        root = result.root
        assert root is None or isinstance(root, bbq.Node)

    def test_len_on_failure(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        result = spec.parse(b"\x01")
        # len returns 0 if no root, or child_count if partial root
        n = len(result)
        assert isinstance(n, int)

    def test_contains_on_failure(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        result = spec.parse(b"\x01")
        # Should not crash — returns False if no root
        assert ("tag" in result) or ("tag" not in result)

    def test_attr_on_failure(self):
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        result = spec.parse(b"\x01")
        if not result.root:
            with pytest.raises(AttributeError):
                result.tag

    def test_emit_on_failure(self):
        # A failed parse produced no document, so there is nothing to serialize and
        # nothing to settle — emit() gives back the input, unchanged.
        spec = bbq.compile_string(MULTI_FIELD_SPEC)
        data = b"\x01"
        result = spec.parse(data)
        assert not result.success
        assert result.emit() == data


# ── keys/values/items on non-struct ──────────────────────────────────────────

class TestDictMethodsEdge:
    def test_keys_on_leaf(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        assert result.magic.keys() == []

    def test_values_on_leaf(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        assert result.magic.values() == []

    def test_items_on_leaf(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        assert result.magic.items() == []

    def test_keys_on_array(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        # Array children may or may not have names
        k = result.items.keys()
        assert isinstance(k, list)


# ── Node.name returning None (array element) ────────────────────────────────

class TestNodeNameNone:
    def test_array_element_name_is_none(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x03\x01\x00\x02\x00\x03\x00"
        result = spec.parse(data)
        elem = result.items[0]
        assert elem.name is None


# ── dir() on leaf node ───────────────────────────────────────────────────────

class TestDirLeaf:
    def test_dir_leaf_has_properties(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        d = dir(result.magic)
        assert "offset" in d
        assert "raw" in d
        assert "value" in d
        assert "capture_type" in d
        assert "name" in d
        # No child field names
        assert "magic" not in d


# ── nb_bool additional branches ──────────────────────────────────────────────

class TestNbBoolExtra:
    def test_bool_struct_truthy(self):
        spec = bbq.compile_string(NESTED_SPEC)
        data = b"\x01\x02\x03\x00"
        result = spec.parse(data, rule="Outer")
        assert bool(result.hdr)

    def test_bool_string_truthy(self):
        spec = bbq.compile_string(STRING_SPEC)
        result = spec.parse(b"\x05hello")
        assert bool(result.name)

    def test_bool_computed_truthy(self):
        spec = bbq.compile_string(COMPUTE_SPEC)
        result = spec.parse(b"\x05")
        assert bool(result.doubled)

    def test_bool_array_truthy(self):
        spec = bbq.compile_string(ARRAY_SPEC)
        data = b"\x01\x42\x00"
        result = spec.parse(data)
        assert bool(result.items)


# ── Extern parser support ─────────────────────────────────────────────────────

EXTERN_SPEC = '''\
Msg = struct {
    header: uint32,
    payload: extern("validate", "void")
}
'''

EXTERN_TWO_SPEC = '''\
Msg = struct {
    a: extern("parse_a", "void"),
    b: extern("parse_b", "void")
}
'''


class TestExtern:
    def test_register_during_parse_does_not_disturb_it(self):
        """A parse runs against the registry as it stood when the parse began.

        The extern callback registering more externs used to grow the live arrays that
        the running machine was pointed at — a PyMem_Realloc under the parse's feet, and
        an override could free a callable the parse still held. The parse now takes a
        snapshot with strong references, so registrations land for the NEXT parse.
        """
        spec = bbq.compile_string(EXTERN_SPEC)
        seen = []

        def validate(mv):
            seen.append(bytes(mv))
            # Reallocs the registry (past its capacity of 4) and replaces "validate"
            # itself, all while this very parse is using it.
            for i in range(8):
                spec.register_extern(f"other{i}", lambda m: 0)
            spec.register_extern("validate", lambda m: 1)
            return 4

        spec.register_extern("validate", validate)
        data = b"\x01\x00\x00\x00\xAA\xBB\xCC\xDD"
        r = spec.parse(data)
        assert r.success and r.bytes_consumed == 8
        assert seen == [b"\xAA\xBB\xCC\xDD"]
        # The replacement takes effect from the next parse on.
        r2 = spec.parse(data)
        assert r2.success and r2.bytes_consumed == 5

    def test_success(self):
        """Extern callable returns 4 → consumes 4 bytes after header."""
        spec = bbq.compile_string(EXTERN_SPEC)
        spec.register_extern("validate", lambda mv: 4)
        data = b"\x01\x00\x00\x00\xAA\xBB\xCC\xDD"
        result = spec.parse(data)
        assert result.success
        assert result.bytes_consumed == 8
        assert result.payload.offset == (4, 8)

    def test_failure_returns_none(self):
        """Extern callable returns None → parse fails."""
        spec = bbq.compile_string(EXTERN_SPEC)
        spec.register_extern("validate", lambda mv: None)
        data = b"\x01\x00\x00\x00\xAA\xBB\xCC\xDD"
        result = spec.parse(data)
        assert not result.success

    def test_not_registered(self):
        """No extern registered → parse fails."""
        spec = bbq.compile_string(EXTERN_SPEC)
        data = b"\x01\x00\x00\x00\xAA\xBB\xCC\xDD"
        result = spec.parse(data)
        assert not result.success

    def test_exception_propagation(self):
        """Callable raises → parse fails (exception cleared internally)."""
        def bad_fn(mv):
            raise ValueError("boom")
        spec = bbq.compile_string(EXTERN_SPEC)
        spec.register_extern("validate", bad_fn)
        data = b"\x01\x00\x00\x00\xAA\xBB\xCC\xDD"
        result = spec.parse(data)
        assert not result.success

    def test_bounds_check(self):
        """Callable returns more than remaining length → parse fails."""
        spec = bbq.compile_string(EXTERN_SPEC)
        spec.register_extern("validate", lambda mv: 9999)
        data = b"\x01\x00\x00\x00\xAA\xBB\xCC\xDD"
        result = spec.parse(data)
        assert not result.success

    def test_multiple_externs(self):
        """Two different extern parsers both work."""
        spec = bbq.compile_string(EXTERN_TWO_SPEC)
        spec.register_extern("parse_a", lambda mv: 2)
        spec.register_extern("parse_b", lambda mv: 3)
        data = b"\x01\x02\x03\x04\x05"
        result = spec.parse(data)
        assert result.success
        assert result.bytes_consumed == 5
        assert result.a.offset == (0, 2)
        assert result.b.offset == (2, 5)

    def test_callable_validation(self):
        """Non-callable raises TypeError."""
        spec = bbq.compile_string(EXTERN_SPEC)
        with pytest.raises(TypeError, match="callable"):
            spec.register_extern("validate", 42)

    def test_override(self):
        """Registering same name twice replaces the callable."""
        spec = bbq.compile_string(EXTERN_SPEC)
        spec.register_extern("validate", lambda mv: None)  # will fail
        spec.register_extern("validate", lambda mv: 4)     # override with success
        data = b"\x01\x00\x00\x00\xAA\xBB\xCC\xDD"
        result = spec.parse(data)
        assert result.success

    def test_zero_consume(self):
        """Extern callable returns 0 → succeeds with zero-length capture."""
        spec = bbq.compile_string(EXTERN_SPEC)
        spec.register_extern("validate", lambda mv: 0)
        data = b"\x01\x00\x00\x00"
        result = spec.parse(data)
        assert result.success
        assert result.payload.offset == (4, 4)

    def test_memoryview_contents(self):
        """Callable receives correct data slice via memoryview."""
        received = {}
        def capture_fn(mv):
            received["data"] = bytes(mv)
            return 4
        spec = bbq.compile_string(EXTERN_SPEC)
        spec.register_extern("validate", capture_fn)
        data = b"\x01\x00\x00\x00\xAA\xBB\xCC\xDD"
        result = spec.parse(data)
        assert result.success
        # The extern sees bytes after the header (pos=4 onwards)
        assert received["data"] == b"\xAA\xBB\xCC\xDD"


# Construction (from-scratch → bytes) is the `bbq.build` factory — its own complete
# regime lives in TestBuild below. Grammar-feature coverage lives in TestReadParity
# (the parser surfaces every feature into a navigable ZCow container).


ZCOW_READER_BBQ = os.path.join(os.path.dirname(__file__), "fixtures", "zcow_reader.bbq")


class TestReadParity:
    """The parser must surface EVERY grammar feature into a navigable ZCow container.
    Inputs + asserted shapes mirror the C++ reader's document (cross_backend_test)."""

    @classmethod
    def setup_class(cls):
        cls.spec = bbq.compile(ZCOW_READER_BBQ)

    def test_flat_primitives(self):
        r = self.spec.parse(bytes([1, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                   0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D]), rule="Flat")
        assert r.success
        assert int(r.u8) == 1 and int(r.u16) == 0x0302 and int(r.u32) == 0x07060504
        assert int(r.be16) == 0x0809 and int(r.i32) == 0x0D0C0B0A

    def test_all_primitive_matrix(self):
        import struct
        data = (struct.pack("<B", 200) + struct.pack("<b", -5)
                + struct.pack("<H", 0x0102) + struct.pack(">H", 0x0304) + struct.pack("<h", -300)
                + struct.pack("<I", 0x01020304) + struct.pack(">I", 0x05060708) + struct.pack("<i", -70000)
                + struct.pack("<Q", 0x0102030405060708) + struct.pack("<q", -1)
                + struct.pack("<f", 1.5) + struct.pack("<d", 2.5) + struct.pack("<B", 1))
        r = self.spec.parse(data, rule="AllPrim")
        assert r.success
        assert int(r.u8) == 200 and int(r.i8) == -5
        assert int(r.u16) == 0x0102 and int(r.u16b) == 0x0304 and int(r.i16) == -300
        assert int(r.u32) == 0x01020304 and int(r.u32b) == 0x05060708 and int(r.i32) == -70000
        assert int(r.u64) == 0x0102030405060708 and int(r.i64) == -1
        assert abs(float(r.f32) - 1.5) < 1e-6 and abs(float(r.f64) - 2.5) < 1e-12
        assert r.fl.value is True

    def test_nested_inline_struct(self):
        r = self.spec.parse(bytes([1, 0x02, 0x03, 4]), rule="Nest")
        assert int(r.hdr.a) == 1 and int(r.hdr.b) == 0x0302 and int(r.c) == 4

    def test_counted_array(self):
        r = self.spec.parse(bytes([2, 1, 0, 2, 0]), rule="Arr")
        assert len(r.xs) == 2 and [int(x) for x in r.xs] == [1, 2]

    def test_ruleref_and_array_of_rule(self):
        o = self.spec.parse(bytes([9, 5, 6]), rule="Outer")
        assert int(o.x) == 9 and int(o.inner.p) == 5 and int(o.inner.q) == 6
        ra = self.spec.parse(bytes([2, 1, 2, 3, 4]), rule="RArr")
        assert len(ra.items) == 2 and int(ra.items[1].p) == 3 and int(ra.items[1].q) == 4

    def test_bytes_and_string(self):
        b = self.spec.parse(bytes([3, 0xAA, 0xBB, 0xCC]), rule="Bytes")
        assert bytes(b.data) == b"\xaa\xbb\xcc" and b.data.capture_type == "bytes"
        s = self.spec.parse(b"abc", rule="Str")
        assert str(s.s) == "abc" and s.s.value == "abc"

    def test_computed_int(self):
        r = self.spec.parse(bytes([0xA5]), rule="Comp")
        assert r.hi.capture_type == "computed" and int(r.hi) == 0xA and int(r.lo) == 0x5
        assert r.hi.value == 0xA

    def test_computed_non_int_kinds(self):
        # Computed projects to its real kind (bool/float), not always int.
        spec = bbq.compile_string(
            "Foo = struct { x: uint8, big: compute(x > 10 : bool), half: compute(x / 2.0 : float64) }")
        r = spec.parse(bytes([20]))
        assert r.big.value is True and isinstance(r.big.value, bool)
        assert isinstance(r.half.value, float) and abs(r.half.value - 10.0) < 1e-9

    def test_optional_predicated(self):
        present = self.spec.parse(bytes([1, 0x02, 0x01, 5, 6]), rule="Op")
        assert int(present.v) == 0x0102 and int(present.r.p) == 5 and int(present.r.q) == 6
        absent = self.spec.parse(bytes([0]), rule="Op")
        assert "v" not in absent and "r" not in absent

    def test_optional_bare(self):
        p = self.spec.parse(bytes([5, 9]), rule="Bare")
        assert int(p.x) == 5 and int(p.v) == 9
        a = self.spec.parse(bytes([5]), rule="Bare")
        assert int(a.x) == 5 and "v" not in a

    def test_switch_field(self):
        one = self.spec.parse(bytes([1, 0x02, 0x01]), rule="Sw")
        assert int(one.tag) == 1 and int(one.body) == 0x0102 and one.body.variant_tag == 0
        two = self.spec.parse(bytes([2, 5, 6]), rule="Sw")
        assert int(two.body.p) == 5 and int(two.body.q) == 6 and two.body.variant_tag == 1

    def test_union_and_alternatives(self):
        u = self.spec.parse(bytes([1, 10]), rule="U")
        assert int(u.asA.t) == 1 and int(u.asA.a) == 10 and u.asA.variant_tag == 0
        alt = self.spec.parse(bytes([2, 11]), rule="Alts")
        # the matched arm is the single named child; its variant_tag is the arm ordinal
        name, arm = next(iter(alt))
        assert arm.variant_tag == 1 and int(arm.b) == 11

    def test_bitfield(self):
        # members are navigable Computed children; bitfields fill low-bits-first, so
        # for 0xA5 the first field `hi` takes the low nibble (0x5), `lo` the high (0xA).
        bf = self.spec.parse(bytes([0xA5]), rule="Bf")
        assert int(bf.hi) == 0x5 and int(bf.lo) == 0xA and bf.hi.capture_type == "computed"
        inl = self.spec.parse(bytes([1, 0xA5, 2]), rule="InlineBf")
        assert int(inl.lead) == 1 and int(inl.flags.hi) == 0x5 and int(inl.flags.lo) == 0xA and int(inl.tail) == 2

    def test_bitfield_endianness(self):
        # a multi-byte bitfield decodes the container with its endianness, then fills:
        # big-endian is MSB-first (first field = high bits), little-endian LSB-first.
        be = bbq.compile_string("F = bitfield<uint16be> { hi: 4, mid: 8, lo: 4 }")
        r = be.parse(bytes([0x12, 0x34]), rule="F")               # uint16be = 0x1234
        assert int(r.hi) == 0x1 and int(r.mid) == 0x23 and int(r.lo) == 0x4
        le = bbq.compile_string("F = bitfield<uint16le> { lo: 4, mid: 8, hi: 4 }")
        r2 = le.parse(bytes([0x34, 0x12]), rule="F")              # uint16le = 0x1234
        assert int(r2.lo) == 0x4 and int(r2.mid) == 0x23 and int(r2.hi) == 0x1

    def test_extern(self):
        self.spec.register_extern("readit", lambda mv: 4)
        r = self.spec.parse(bytes([0xAA, 1, 2, 3, 4, 0xBB]), rule="Ext")
        assert int(r.tag) == 0xAA and int(r.tail) == 0xBB
        assert r.blob.capture_type == "external" and r.blob.value == b"\x01\x02\x03\x04"

    def test_rest_window(self):
        r = self.spec.parse(bytes([2, 0x02, 0x01]), rule="Rest")
        assert int(r.size) == 2 and int(r.a) == 0x0102

    def test_top_level_switch_peek(self):
        r = self.spec.parse(bytes([1, 5]), rule="TopSw")
        # peek discriminant; arm is root's anonymous children[0] + variant_tag
        assert int(r[0].p) == 1 and int(r[0].q) == 5 and r[0].variant_tag == 0

    def test_nested_array(self):
        r = self.spec.parse(bytes([2, 3, 1, 2, 3, 4, 5, 6]), rule="Mat")
        assert int(r.rows) == 2 and int(r.cols) == 3
        assert [int(x) for x in r.data[0]] == [1, 2, 3]
        assert int(r.data[1][2]) == 6

    def test_optional_span(self):
        r = self.spec.parse(bytes([1, 0xAA, 0xBB, 0x61, 0x62, 0x63]), rule="OptSpan")
        assert bytes(r.ob) == b"\xaa\xbb" and str(r.os) == "abc"

    def test_leb(self):
        r = self.spec.parse(bytes([0xAC, 0x02, 0x7B, 7]), rule="Leb")
        assert int(r.a) == 300 and int(r.b) == -5 and int(r.c) == 7 and r.a.capture_type == "computed"
        arr = self.spec.parse(bytes([2, 5, 0xAC, 0x02]), rule="LebArr")
        assert [int(x) for x in arr.xs] == [5, 300]

    def test_top_level_typedef_and_alias(self):
        tb = self.spec.parse(bytes([0x42]), rule="TyByte")
        assert int(tb[0]) == 0x42                       # root wraps the scalar as child[0]
        tr = self.spec.parse(bytes([5, 6]), rule="TyRule")
        assert int(tr[0].p) == 5 and int(tr[0].q) == 6

    def test_interval_field(self):
        r = self.spec.parse(bytes([1, 0xAA]), rule="Iv")
        assert int(r.off) == 1 and int(r.val) == 0xAA

    def test_endian_switch(self):
        r = self.spec.parse(bytes([0x02, 0x01, 0x03, 0x04]), rule="Es")
        assert int(r.a) == 0x0102 and int(r.b) == 0x0304

    def test_uncounted_eof_until_count(self):
        eof = self.spec.parse(bytes([1, 2, 3, 4]), rule="Eof")
        assert len(eof.items) == 2 and int(eof.items[1].p) == 3
        cnt = self.spec.parse(bytes([3, 10, 11, 12]), rule="Cnt")
        assert [int(x) for x in cnt.xs] == [10, 11, 12]

    def test_ternary_compute(self):
        r = self.spec.parse(bytes([5]), rule="Tern")
        assert int(r.g) == 10
        z = self.spec.parse(bytes([0]), rule="Tern")
        assert int(z.g) == 99

    def test_path_ref_count(self):
        r = self.spec.parse(bytes([2, 10, 11]), rule="Np")
        assert int(r.h.n) == 2 and [int(x) for x in r.xs] == [10, 11]

    def test_nested_counted_array(self):
        r = self.spec.parse(bytes([1, 2, 10, 11]), rule="NestArr")
        assert int(r.m) == 1 and int(r.gs[0].n) == 2 and [int(x) for x in r.gs[0].xs] == [10, 11]


from bbq import build as B


class TestBuild:
    """bbq.build — grammar-free byte construction. Full surface: every leaf factory,
    both containers, mutation, nesting, and the build->parse integration."""

    def test_int_leaves_little(self):
        assert bytes(B.u8(0x12)) == b"\x12"
        assert bytes(B.i8(-1)) == b"\xff"
        assert bytes(B.u16(0x0102)) == b"\x02\x01"
        assert bytes(B.u32(0x01020304)) == b"\x04\x03\x02\x01"
        assert bytes(B.u64(0x0102030405060708)) == b"\x08\x07\x06\x05\x04\x03\x02\x01"
        assert bytes(B.i16(-2)) == b"\xfe\xff"
        assert bytes(B.i32(-2)) == b"\xfe\xff\xff\xff"
        assert bytes(B.i64(-2)) == b"\xfe\xff\xff\xff\xff\xff\xff\xff"

    def test_int_leaves_big(self):
        assert bytes(B.u16(0x0102, endian="big")) == b"\x01\x02"
        assert bytes(B.u32(0x01020304, endian="big")) == b"\x01\x02\x03\x04"
        assert bytes(B.u64(0x0102030405060708, endian="big")) == b"\x01\x02\x03\x04\x05\x06\x07\x08"
        assert bytes(B.i16(-2, endian="big")) == b"\xff\xfe"

    def test_float_leaves(self):
        import struct
        assert bytes(B.f32(1.5)) == struct.pack("<f", 1.5)
        assert bytes(B.f32(1.5, endian="big")) == struct.pack(">f", 1.5)
        assert bytes(B.f64(2.5)) == struct.pack("<d", 2.5)
        assert bytes(B.f64(2.5, endian="big")) == struct.pack(">d", 2.5)

    def test_leb_leaves(self):
        assert bytes(B.leb(0)) == b"\x00"
        assert bytes(B.leb(300)) == b"\xac\x02"
        assert bytes(B.leb(0x4000)) == b"\x80\x80\x01"
        assert bytes(B.sleb(-1)) == b"\x7f"
        assert bytes(B.sleb(-5)) == b"\x7b"
        assert bytes(B.sleb(63)) == b"\x3f"

    def test_bytes_and_text(self):
        assert bytes(B.raw(b"\xaa\xbb")) == b"\xaa\xbb"
        assert bytes(B.text("hi")) == b"hi"
        assert bytes(B.text("héllo")) == "héllo".encode("utf-8")

    def test_leaf_introspection(self):
        x = B.u32(0x2a)
        assert int(x) == 0x2a and float(x) == 42.0 and x.value == 0x2a and x.type == "u32le"
        assert B.u16(1, endian="big").type == "u16be"
        assert B.leb(5).type == "leb" and B.sleb(5).type == "sleb"
        assert B.raw(b"x").type == "bytes" and B.text("x").type == "string"
        assert B.raw(b"xy").value == b"xy"
        f = B.f64(2.5)
        assert float(f) == 2.5 and f.value == 2.5
        with pytest.raises(TypeError):
            int(B.raw(b"x"))

    def test_leaf_not_constructible_directly(self):
        with pytest.raises(TypeError):
            type(B.u8(1))()

    def test_struct_basic(self):
        s = B.Struct(a=B.u8(1), b=B.u16(0x0203))
        assert bytes(s) == b"\x01\x03\x02"
        assert len(s) == 2 and "a" in s and "z" not in s
        assert int(s.a) == 1 and int(s["b"]) == 0x0203
        assert int(s[0]) == 1
        assert s.keys() == ["a", "b"]
        assert [int(v) for v in s.values()] == [1, 0x0203]
        assert [(n, int(v)) for n, v in s.items()] == [("a", 1), ("b", 0x0203)]
        assert [(n, int(v)) for n, v in s] == [("a", 1), ("b", 0x0203)]

    def test_struct_mutation(self):
        s = B.Struct(a=B.u8(1))
        s.a = B.u8(9)
        assert int(s.a) == 9
        s["b"] = B.u8(2)
        assert int(s.b) == 2 and len(s) == 2
        s[0] = B.u8(7)
        assert int(s.a) == 7
        assert bytes(s) == b"\x07\x02"

    def test_struct_requires_typed_value(self):
        with pytest.raises(TypeError):
            B.Struct(a=5)
        s = B.Struct(a=B.u8(1))
        with pytest.raises(TypeError):
            s.a = 5
        s.a = b"\xff\xfe"
        assert bytes(s) == b"\xff\xfe"

    def test_array_basic(self):
        a = B.Array(B.u8(1), B.u8(2), B.u8(3))
        assert bytes(a) == b"\x01\x02\x03"
        assert len(a) == 3 and int(a[1]) == 2 and int(a[-1]) == 3
        assert [int(x) for x in a] == [1, 2, 3]
        a2 = B.Array([B.u16(10), B.u16(20)])
        assert bytes(a2) == b"\x0a\x00\x14\x00"

    def test_array_mutation(self):
        a = B.Array(B.u8(1), B.u8(2))
        a.append(B.u8(3))
        assert len(a) == 3
        a[0] = B.u8(9)
        assert int(a[0]) == 9
        del a[1]
        assert len(a) == 2 and [int(x) for x in a] == [9, 3]
        assert bytes(a) == b"\x09\x03"

    def test_array_requires_typed_value(self):
        with pytest.raises(TypeError):
            B.Array(1, 2, 3)
        a = B.Array(B.u8(1))
        with pytest.raises(TypeError):
            a.append(5)

    def test_nesting_and_bytes_children(self):
        s = B.Struct(
            hdr=B.Struct(magic=B.u32(0xCAFE, endian="big"), n=B.leb(2)),
            items=B.Array(B.u8(1), b"\x02\x03"),
            tail=B.text("ok"),
        )
        assert bytes(s) == b"\x00\x00\xca\xfe" + b"\x02" + b"\x01\x02\x03" + b"ok"

    def test_build_into_parse(self):
        spec = bbq.compile_string(
            "It = struct { v: uint8, w: uint8 }\nT = struct { n: uint8, items: array<It>[n] }")
        r = spec.parse(bytes([1, 1, 2]), rule="T")
        r.items.append(B.Struct(v=B.u8(3), w=B.u8(4)))
        r.items[0] = B.Struct(v=B.u8(9), w=B.u8(8))
        r.n = 2
        r2 = spec.parse(r.emit(), rule="T")
        got = [(int(it.v), int(it.w)) for it in r2.items]
        assert int(r2.n) == 2 and got == [(9, 8), (3, 4)]

    def test_built_value_joins_the_document_as_structure(self):
        # A constructed value IS a node, so adding one to a parse grafts structure rather
        # than the bytes it would have serialized to: it stays navigable and writable, and
        # the count it changed is derived like any other.
        spec = bbq.compile_string(
            "It = struct { v: uint8, w: uint8 }\nT = struct { n: uint8, items: array<It>[n] }")
        r = spec.parse(bytes([1, 1, 2]), rule="T")
        r.items.append(B.Struct(v=B.u8(3), w=B.u8(4)))

        el = r.items[1]
        assert el.capture_type == "struct" and len(el) == 2   # not an opaque blob
        assert (int(el.v), int(el.w)) == (3, 4)
        el.v = 9                                              # and writable in place
        assert int(el.v) == 9

        r2 = spec.parse(r.emit(), rule="T")
        assert int(r2.n) == 2
        assert [(int(i.v), int(i.w)) for i in r2.items] == [(1, 2), (9, 4)]

    def test_built_value_replacing_an_element_is_also_structure(self):
        spec = bbq.compile_string(
            "It = struct { v: uint8, w: uint8 }\nT = struct { n: uint8, items: array<It>[n] }")
        r = spec.parse(bytes([2, 1, 2, 3, 4]), rule="T")
        r.items[0] = B.Struct(v=B.u8(9), w=B.u8(8))
        assert (int(r.items[0].v), int(r.items[0].w)) == (9, 8)
        assert (int(r.items[1].v), int(r.items[1].w)) == (3, 4)   # neighbour untouched
        assert spec.parse(r.emit(), rule="T").emit() == bytes([2, 9, 8, 3, 4])

    def test_build_new_file_from_scratch(self):
        spec = bbq.compile_string("Rec = struct { magic: uint32be, n: uint8, xs: array<uint16le>[n] }")
        data = bytes(B.Struct(magic=B.u32(0xCAFEBABE, endian="big"), n=B.u8(3),
                              xs=B.Array(B.u16(10), B.u16(20), B.u16(30))))
        r = spec.parse(data, rule="Rec")
        assert r.success and int(r.magic) == 0xCAFEBABE and int(r.n) == 3
        assert [int(x) for x in r.xs] == [10, 20, 30]


class TestContainerProtocol:
    """An array is a container, and the mapping view has to say so.

    `keys`/`values`/`items` used to filter children by NAME, and an array's
    elements are positional. A primitive element has no name and vanished; a
    struct element has an empty one and came back under the key "". Either way a
    generic walk — `for k, v in node.items()` — silently skipped everything every
    array held, while `len()`, `node[0]` and iteration all reported it.
    """

    SPEC = "E = struct { v: uint8 }\nTop = struct { n: uint8, xs: array<E>[n], f: uint8 }"
    PRIM = "Top = struct { n: uint8, xs: array<uint8>[n], f: uint8 }"

    def _arr(self, spec_src):
        spec = bbq.compile_string(spec_src)
        r = spec.parse(bytes([2, 0xAA, 0xBB, 0x44]), rule="Top")
        assert r.success
        return r, r.root["xs"]

    @pytest.mark.parametrize("spec_src", [SPEC, PRIM])
    def test_array_mapping_view_is_positional(self, spec_src):
        r, xs = self._arr(spec_src)
        # The same keys `xs[k]` accepts, for both element kinds.
        assert list(xs.keys()) == [0, 1]
        assert len(list(xs.values())) == 2
        assert [k for k, _ in xs.items()] == [0, 1]
        assert sorted(dict(xs).keys()) == [0, 1]

    @pytest.mark.parametrize("spec_src", [SPEC, PRIM])
    def test_array_views_agree_with_len_and_indexing(self, spec_src):
        r, xs = self._arr(spec_src)
        assert len(xs) == len(list(xs.values())) == len(list(xs.items()))
        # items() must hand back the same nodes indexing does.
        for k, v in xs.items():
            assert v.offset == xs[k].offset

    @pytest.mark.parametrize("spec_src", [SPEC, PRIM])
    def test_a_generic_walk_reaches_array_elements(self, spec_src):
        """The property the bug broke: walking items() sees the whole document."""
        r, _ = self._arr(spec_src)
        seen = []

        def walk(node):
            kids = list(node.items())
            if not kids:
                seen.append(node)
                return
            for _, child in kids:
                walk(child)

        walk(r.root)
        # n, f, and the two elements' `v` (or the two primitive elements).
        assert len(seen) == 4

    def test_struct_mapping_view_is_unchanged(self):
        r, _ = self._arr(self.SPEC)
        assert [k for k, _ in r.root.items()] == ["n", "xs", "f"]
        assert list(r.root.keys()) == ["n", "xs", "f"]

    @pytest.mark.parametrize("spec_src", [SPEC, PRIM])
    def test_dir_does_not_offer_an_array_element_as_an_attribute(self, spec_src):
        """`dir()` is attribute names; an element has none, and "" is not one."""
        r, xs = self._arr(spec_src)
        assert "" not in dir(xs)


# ── The integer boundary matrix ──────────────────────────────────────────────
#
# Every fixed-width integer leaf the grammar can name. The suite above covers
# every one of these types — and reads u64 as 0x0102030405060708 and i64 as -1,
# both inside the signed range, so nothing it asserts can tell a correct decode
# from one that lost the sign. These rows are the boundaries, which is where the
# decode and the range check are the only things holding.
#
# (grammar spelling, byte width, signed, big-endian)
INT_TYPES = [
    ("uint8",    1, False, False), ("int8",     1, True,  False),
    ("uint16le", 2, False, False), ("uint16be", 2, False, True),
    ("int16le",  2, True,  False), ("int16be",  2, True,  True),
    ("uint32le", 4, False, False), ("uint32be", 4, False, True),
    ("int32le",  4, True,  False), ("int32be",  4, True,  True),
    ("uint64le", 8, False, False), ("uint64be", 8, False, True),
    ("int64le",  8, True,  False), ("int64be",  8, True,  True),
]

INT_IDS = [t[0] for t in INT_TYPES]


def int_limits(width, signed):
    """(min, max) a leaf of this width and signedness can hold."""
    if signed:
        return -(1 << (width * 8 - 1)), (1 << (width * 8 - 1)) - 1
    return 0, (1 << (width * 8)) - 1


def int_bytes(value, width, big):
    """`value`'s two's-complement bytes at this width."""
    masked = value & ((1 << (width * 8)) - 1)
    return masked.to_bytes(width, "big" if big else "little")


def int_spec(spelling):
    return bbq.compile_string(f"Foo = struct {{ v: {spelling} }}")


@pytest.mark.parametrize("spelling,width,signed,big", INT_TYPES, ids=INT_IDS)
class TestIntegerBoundaries:
    """Both extremes of every integer leaf, through every path that reads one.

    A 64-bit UNSIGNED leaf is the case the rest of the suite cannot reach: its
    value does not fit the int64_t the decode carries it in, so a consumer that
    does not branch on signedness reports 2**64-1 as -1 — and every conversion
    (int, float, str, format, ==, <) goes through that one decode.
    """

    def _node(self, spelling, width, signed, big, value):
        r = int_spec(spelling).parse(int_bytes(value, width, big))
        assert r.success, r.error_message
        return r, r.root["v"]

    def test_min_reads_back(self, spelling, width, signed, big):
        lo, _ = int_limits(width, signed)
        _, n = self._node(spelling, width, signed, big, lo)
        assert n.value == lo

    def test_max_reads_back(self, spelling, width, signed, big):
        _, hi = int_limits(width, signed)
        _, n = self._node(spelling, width, signed, big, hi)
        assert n.value == hi

    def test_every_conversion_agrees_at_the_maximum(self, spelling, width, signed, big):
        """One decode feeds them all, so one sign bug shows up in every one."""
        _, hi = int_limits(width, signed)
        _, n = self._node(spelling, width, signed, big, hi)
        assert int(n) == hi
        assert str(n) == str(hi)
        assert format(n, "d") == format(hi, "d")
        assert float(n) == float(hi)
        assert n == hi
        assert not (n < hi)
        assert n >= hi

    def test_ordering_holds_at_the_maximum(self, spelling, width, signed, big):
        """> 0 is false for a u64 max read as -1, which is how the bug hid."""
        _, hi = int_limits(width, signed)
        _, n = self._node(spelling, width, signed, big, hi)
        if hi > 0:
            assert n > 0
        assert n > (hi - 1)

    def test_extremes_survive_a_write_and_a_re_parse(self, spelling, width, signed, big):
        lo, hi = int_limits(width, signed)
        spec = int_spec(spelling)
        for value in (lo, hi, 0):
            r = spec.parse(int_bytes(0, width, big))
            r.root["v"] = value
            out = bytes(r)
            assert len(out) == width
            again = spec.parse(out)
            assert again.success
            assert again.root["v"].value == value, (
                f"{spelling}: wrote {value}, re-parsed {again.root['v'].value}")

    def test_the_document_agrees_with_its_own_serialization(self, spelling, width, signed, big):
        """What a node reports and what emit() writes must be one document.

        A written node keeps the value it was given; emit writes leaf_width
        bytes. Nothing reconciles those two, so an unrepresentable value is
        reported back verbatim while the file holds it truncated.
        """
        lo, hi = int_limits(width, signed)
        spec = int_spec(spelling)
        for value in (lo, hi):
            r = spec.parse(int_bytes(0, width, big))
            r.root["v"] = value
            in_memory = r.root["v"].value
            re_parsed = spec.parse(bytes(r)).root["v"].value
            assert in_memory == re_parsed, (
                f"{spelling}: node says {in_memory}, its own bytes say {re_parsed}")

    def test_a_value_the_leaf_cannot_hold_is_refused(self, spelling, width, signed, big):
        """Truncating it silently makes the document disagree with the file."""
        lo, hi = int_limits(width, signed)
        spec = int_spec(spelling)
        for value in (hi + 1, lo - 1):
            r = spec.parse(int_bytes(0, width, big))
            with pytest.raises((OverflowError, ValueError)):
                r.root["v"] = value

    def test_a_refused_write_leaves_the_document_alone(self, spelling, width, signed, big):
        lo, hi = int_limits(width, signed)
        spec = int_spec(spelling)
        r = spec.parse(int_bytes(hi, width, big))
        try:
            r.root["v"] = hi + 1
        except (OverflowError, ValueError):
            pass
        assert r.root["v"].value == hi
        assert bytes(r) == int_bytes(hi, width, big)


# bbq.build is the second consumer of the same law: it mints nodes directly
# rather than editing parsed ones, and reaches set_int by its own road.
BUILD_INTS = [
    ("u8", 1, False), ("i8", 1, True),
    ("u16", 2, False), ("i16", 2, True),
    ("u32", 4, False), ("i32", 4, True),
    ("u64", 8, False), ("i64", 8, True),
]


@pytest.mark.parametrize("factory,width,signed", BUILD_INTS, ids=[b[0] for b in BUILD_INTS])
class TestBuildIntegerBoundaries:
    """The same range law, through the builder. Two consumers, one law."""

    def test_extremes_build_to_the_right_bytes(self, factory, width, signed):
        lo, hi = int_limits(width, signed)
        for value in (lo, hi, 0):
            leaf = getattr(bbq.build, factory)(value)
            assert bytes(bbq.build.Struct(v=leaf)) == int_bytes(value, width, False)

    def test_a_value_the_leaf_cannot_hold_is_refused(self, factory, width, signed):
        lo, hi = int_limits(width, signed)
        for value in (hi + 1, lo - 1):
            with pytest.raises((OverflowError, ValueError)):
                getattr(bbq.build, factory)(value)


class TestVarintsAreNotRangeChecked:
    """A varint's width follows its value, so the fixed-width law does not apply.

    The range check is about a leaf that has `leaf_width` bytes to put a value
    in. uleb128/sleb64 encode as many bytes as the value needs, so a bound there
    would be inventing one — the only limit is the 64 bits the value is carried
    in. Pinned so the check cannot be widened onto them by accident.
    """

    def test_a_large_uleb_is_accepted(self):
        big = (1 << 64) - 1
        assert bytes(bbq.build.Struct(v=bbq.build.leb(big))) == b"\xff" * 9 + b"\x01"

    def test_a_negative_sleb_is_accepted(self):
        assert bytes(bbq.build.Struct(v=bbq.build.sleb(-1))) == b"\x7f"


class TestLensLawsAtTheBoundary:
    """The lens harness, fed values that sit exactly on the field's edge.

    TestLensLaws already does the right thing — edit, emit, re-parse, compare —
    and every edit it makes is a value comfortably inside the field. That is
    what let a write of 999 into a uint8 pass through it: the harness was never
    given a value whose representability was in question.
    """

    SPEC = "Foo = struct { a: uint8, b: uint16le, c: uint32le, d: uint64le, e: int8 }"
    EDGES = {"a": (0, 255), "b": (0, 65535), "c": (0, 2**32 - 1),
             "d": (0, 2**64 - 1), "e": (-128, 127)}

    def _fresh(self):
        spec = bbq.compile_string(self.SPEC)
        return spec, spec.parse(bytes(16))

    def test_every_field_survives_its_own_extremes(self):
        spec, _ = self._fresh()
        for field, (lo, hi) in self.EDGES.items():
            for value in (lo, hi):
                r = spec.parse(bytes(16))
                r.root[field] = value
                again = spec.parse(bytes(r))
                assert again.success
                assert again.root[field].value == value, f"{field} = {value}"

    def test_all_fields_at_their_maximum_at_once(self):
        """Every byte set: the one shape where a neighbour's truncation shows."""
        spec, _ = self._fresh()
        r = spec.parse(bytes(16))
        for field, (_, hi) in self.EDGES.items():
            r.root[field] = hi
        again = spec.parse(bytes(r))
        assert again.success
        for field, (_, hi) in self.EDGES.items():
            assert again.root[field].value == hi, field

    def test_an_overshoot_does_not_disturb_a_neighbour(self):
        spec, _ = self._fresh()
        r = spec.parse(bytes(16))
        r.root["b"] = 4660
        before = bytes(r)
        with pytest.raises((OverflowError, ValueError)):
            r.root["a"] = 256
        assert bytes(r) == before
        assert r.root["b"].value == 4660


class TestGarbageCollection:
    """The module's types hold Python objects, so they have to be collectable.

    A Spec keeps STRONG references to the extern callables handed to it, and the
    natural way to write a stateful extern — a closure, a bound method — refers
    to the Spec right back. Untracked by the collector that cycle is not just
    uncollected but invisible: it never reaches gc.garbage, and the Spec's
    grammar, AST and sema leak for the life of the process.
    """

    EXTERN = 'Msg = struct { header: uint32, payload: extern("validate", "void") }'

    def test_a_spec_in_a_cycle_with_its_own_extern_is_collected(self):
        freed = []

        class Sentinel:
            def __del__(self):
                freed.append(1)

        def build():
            spec = bbq.compile_string(self.EXTERN)
            sentinel = Sentinel()
            # The closure names the spec, so spec → callable → spec.
            spec.register_extern("validate", lambda mv, _s=spec, _t=sentinel: 4)

        build()
        gc.collect()
        gc.collect()
        assert freed, "the Spec/extern cycle was never collected"
        assert not gc.garbage

    def test_a_node_in_a_cycle_with_its_own_result_is_collected(self):
        freed = []

        class Holder:
            def __init__(self):
                self.node = None

            def __del__(self):
                freed.append(1)

        def build():
            spec = bbq.compile_string("Foo = struct { v: uint8 }")
            holder = Holder()
            holder.node = spec.parse(bytes([1])).root["v"]
            holder.self_ref = holder      # holder → node → result, holder → holder

        build()
        gc.collect()
        assert freed, "the Node/Result cycle was never collected"

    def test_the_types_that_hold_python_objects_are_tracked(self):
        """What the cycles above rely on, asserted directly."""
        spec = bbq.compile_string("Foo = struct { v: uint8 }")
        r = spec.parse(bytes([1]))
        assert gc.is_tracked(spec), "Spec holds extern callables"
        assert gc.is_tracked(r), "ParseResult holds the input buffer"
        assert gc.is_tracked(r.root), "Node holds its ParseResult"


# ── The selector surface ─────────────────────────────────────────────────────
#
# RFC 9535 defines a string syntax; this is the same VOCABULARY spelled with
# Python's subscript protocol, which is where a query in this module lives. The
# mapping, and the rule that decides the return type:
#
#   node["name"]        $.name / $['name']   name       → Node      (one, strict)
#   node[3] / node[-1]  $[3]                 index      → Node      (one, strict)
#   node[1:3:2]         $[1:3:2]             slice      → NodeList
#   node[:]             $[*]                 wildcard   → NodeList
#   node[0, 2]          $[0,2]               several    → NodeList
#   node[...]           the `..` node set                → NodeList
#   node[..., "name"]   $..name              descendant → NodeList
#   node[bbq.this.x>1]  $[?@.x > 1]          filter     → NodeList
#
# A selector that can only ever match once answers with a Node; one that can
# match more answers with a NodeList, and a segment applied to a NodeList is
# that segment applied to each of its nodes (§2.1.2), which is what lets them
# chain.

SELECTOR_SPEC = """\
Pt  = struct { x: uint8, y: uint8 }
Top = struct { n: uint8, pts: array<Pt>[n], tag: uint8 }
"""


@pytest.fixture
def doc():
    spec = bbq.compile_string(SELECTOR_SPEC)
    r = spec.parse(bytes([3, 1, 9, 5, 2, 7, 7, 0x99]), rule="Top")
    assert r.success, r.error_message
    return r


def xy(nodelist):
    return [(p["x"].value, p["y"].value) for p in nodelist]


class TestSelectorReturnTypes:
    """The rule that decides whether you get a Node or a NodeList."""

    def test_a_name_gives_one_node(self, doc):
        assert isinstance(doc.root["tag"], bbq.Node)
        assert doc.root["tag"].value == 0x99

    def test_an_index_gives_one_node(self, doc):
        assert isinstance(doc.root["pts"][0], bbq.Node)
        assert isinstance(doc.root["pts"][-1], bbq.Node)

    def test_the_multi_valued_selectors_give_a_nodelist(self, doc):
        root = doc.root
        for key in (slice(None), (0, 2), Ellipsis, (Ellipsis, "x")):
            assert isinstance(root[key], bbq.NodeList), key

    def test_a_missing_name_raises_for_one_but_not_for_many(self, doc):
        """The strict accessor is how you ask for a field and expect it.

        Inside a multi-selector a miss contributes nothing instead, which is
        §2.3.1.2: "selects nothing if there is no such member".
        """
        with pytest.raises(KeyError):
            doc.root["nope"]
        assert len(doc.root["nope",]) == 0
        assert len(doc.root[..., "nope"]) == 0


class TestSelectors:
    def test_wildcard(self, doc):
        assert doc.root[:].values[0] == 3
        assert len(doc.root[:]) == 3                      # n, pts, tag
        assert len(doc.root["pts"][:]) == 3               # three points

    def test_several_selectors_in_one_segment(self, doc):
        assert doc.root[0, 2].values == [3, 0x99]
        assert doc.root["n", "tag"].values == [3, 0x99]
        assert doc.root["tag", "n"].values == [0x99, 3], "order follows the selectors"

    def test_a_repeated_selector_matches_repeatedly(self, doc):
        """§2.5.1.2: the results are concatenated, duplicates and all."""
        assert doc.root["n", "n"].values == [3, 3]

    def test_slice_selectors(self, doc):
        pts = doc.root["pts"]
        assert xy(pts[0:2]) == [(1, 9), (5, 2)]
        assert xy(pts[::-1]) == [(7, 7), (5, 2), (1, 9)]
        assert xy(pts[::2]) == [(1, 9), (7, 7)]
        assert len(pts[10:20]) == 0

    def test_the_descendant_node_set(self, doc):
        """`node[...]` is the node itself and everything under it."""
        everything = doc.root[...]
        # root, n, pts, 3 points, 6 coordinates, tag
        assert len(everything) == 1 + 1 + 1 + 3 + 6 + 1
        assert everything.nodes[0].offset == doc.root.offset

    def test_the_descendant_segment(self, doc):
        assert doc.root[..., "x"].values == [1, 5, 7]
        assert doc.root[..., "y"].values == [9, 2, 7]
        assert doc.root[..., "x", "y"].values == [1, 9, 5, 2, 7, 7]

    def test_a_descendant_search_reaches_through_arrays(self, doc):
        """The thing a flat accessor cannot do: find a field at any depth."""
        assert doc.root[..., "tag"].values == [0x99]

    def test_segments_chain_through_a_nodelist(self, doc):
        assert doc.root["pts"][:]["x"].values == [1, 5, 7]
        assert doc.root[...]["x"].values == [1, 5, 7]

    def test_a_leaf_selects_nothing_rather_than_raising(self, doc):
        leaf = doc.root["tag"]
        assert len(leaf[:]) == 0
        assert len(leaf[..., "x"]) == 0
        assert len(leaf[...]) == 1, "a leaf is still its own descendant set"

    def test_a_nodelist_exposes_its_nodes_and_values(self, doc):
        nl = doc.root[..., "x"]
        assert [n.value for n in nl] == [1, 5, 7]
        assert nl.values == [1, 5, 7]
        assert isinstance(nl.nodes, list) and isinstance(nl.nodes[0], bbq.Node)
        assert len(nl) == 3
        assert "NodeList" in repr(nl)


class TestFilterSelector:
    """$[?...] — the one selector that needs an expression object.

    Node.__eq__ materialises the value and compares it, so `node.x == 1` is a
    real bool and Node cannot double as the expression builder. bbq.this is the
    placeholder that builds a tree instead, which is how pandas and SQLAlchemy
    spell the same thing.
    """

    def test_comparisons(self, doc):
        pts = doc.root["pts"]
        assert xy(pts[bbq.this.x > 1]) == [(5, 2), (7, 7)]
        assert xy(pts[bbq.this.x == 5]) == [(5, 2)]
        assert xy(pts[bbq.this.x != 5]) == [(1, 9), (7, 7)]
        assert xy(pts[bbq.this.y <= 2]) == [(5, 2)]
        assert xy(pts[bbq.this.x >= 5]) == [(5, 2), (7, 7)]

    def test_logical_combinators(self, doc):
        pts = doc.root["pts"]
        assert xy(pts[(bbq.this.x > 1) & (bbq.this.y < 5)]) == [(5, 2)]
        assert xy(pts[(bbq.this.x == 7) | (bbq.this.y == 9)]) == [(1, 9), (7, 7)]
        assert xy(pts[~(bbq.this.x == 5)]) == [(1, 9), (7, 7)]

    def test_existence_test(self, doc):
        """A path with no comparison asks whether it matched anything."""
        pts = doc.root["pts"]
        assert len(pts[bbq.this.x]) == 3
        assert len(pts[bbq.this.nope]) == 0

    def test_nothing_compares_only_with_nothing(self, doc):
        """§2.3.5.2: a query matching no node is Nothing, and orderings on it
        are false while `== Nothing` is true."""
        pts = doc.root["pts"]
        assert len(pts[bbq.this.nope == bbq.this.alsonope]) == 3
        assert len(pts[bbq.this.nope == 1]) == 0
        assert len(pts[bbq.this.nope < 1]) == 0
        assert len(pts[bbq.this.nope != 1]) == 3

    def test_count_and_length(self, doc):
        pts = doc.root["pts"]
        assert len(pts[bbq.count(bbq.this[:]) == 2]) == 3   # each Pt has two fields
        assert len(pts[bbq.count(bbq.this[:]) == 3]) == 0
        assert len(pts[bbq.length(bbq.this) == 2]) == 3

    def test_a_filter_applies_to_children_not_to_the_node(self, doc):
        """§2.3.5.2 — the filter selects from among the children."""
        assert len(doc.root[bbq.this.x > 0]) == 0      # root's children are n/pts/tag
        assert len(doc.root["pts"][bbq.this.x > 0]) == 3

    def test_a_filter_composes_with_the_other_segments(self, doc):
        assert xy(doc.root[..., "pts"][bbq.this.x > 4]) == [(5, 2), (7, 7)]
        assert doc.root["pts"][bbq.this.x > 1]["y"].values == [2, 7]

    def test_a_query_is_not_a_truth_value(self):
        """`and`/`or`/`not` cannot be overloaded, so they must not look like
        they worked: Python would evaluate the operand for truthiness and throw
        the expression away."""
        with pytest.raises(TypeError):
            bool(bbq.this.x == 1)
        with pytest.raises(TypeError):
            bbq.this.x and bbq.this.y
        with pytest.raises(TypeError):
            not bbq.this.x

    def test_a_placeholder_is_reusable_and_never_mutated(self):
        base = bbq.this.a
        one, two = base.b, base.c
        assert repr(base) == "<bbq.query path>"
        assert one is not two

    def test_navigating_a_finished_expression_is_refused(self):
        with pytest.raises(TypeError):
            (bbq.this.x == 1).y

    def test_indexing_and_naming_inside_an_expression(self, doc):
        pts = doc.root["pts"]
        assert xy(pts[bbq.this["x"] > 1]) == [(5, 2), (7, 7)]
        assert xy(pts[bbq.this[0] > 1]) == [(5, 2), (7, 7)]

    def test_an_underscore_field_is_reachable_by_subscript(self):
        """Attribute access keeps dunders and privates for the object itself, so
        a field whose name starts with _ is named through the brackets."""
        spec = bbq.compile_string("E = struct { _v: uint8 }\nT = struct { n: uint8, xs: array<E>[n] }")
        r = spec.parse(bytes([2, 7, 8]), rule="T")
        assert r.root["xs"][bbq.this["_v"] > 7].values != []


class TestSelectorsAreNotAQueryParser:
    """What this surface deliberately is not.

    It is RFC 9535's selector vocabulary reached through Python's protocols, not
    an implementation of the RFC: there is no query string, no `$`, and no
    match()/search() (those need RFC 9485 I-Regexp). Asserted so the difference
    is a decision on the record rather than something a reader has to infer.
    """

    def test_there_is_no_query_string_entry_point(self):
        assert not hasattr(bbq, "jsonpath")
        assert not hasattr(bbq, "query_string")

    def test_the_regexp_functions_are_absent(self):
        assert not hasattr(bbq, "match")
        assert not hasattr(bbq, "search")

    def test_a_name_selector_still_answers_with_one_node(self):
        """Duplicate field names: the grammar admits them, keys() shows both,
        and the name selector answers with the first — which §2.3.1.2 permits
        ("selects a member value"). The wildcard is how you reach both."""
        r = bbq.compile_string("Foo = struct { a: uint8, a: uint8 }").parse(bytes([1, 2]))
        assert list(r.root.keys()) == ["a", "a"]
        assert r.root["a"].value == 1
        assert r.root[:].values == [1, 2]
        assert r.root[..., "a"].values == [1], "the name selector picks one per node"

    def test_an_exhausted_iterator_stays_exhausted(self, doc):
        it = iter(doc.root["pts"])
        assert len(list(it)) == 3
        assert list(it) == []

    def test_a_slice_step_of_zero_is_pythons_error(self, doc):
        with pytest.raises(ValueError):
            doc.root["pts"][::0]

    def test_querying_does_not_change_the_document(self, doc):
        """Navigating claims ownership of the containers it passes through, so a
        purely READ query must still re-emit byte-identically — otherwise every
        query is an edit and GetPut only holds for documents nobody looked at.
        """
        before = bytes(doc)
        doc.root[...]                      # the whole tree
        doc.root[..., "x"].values
        doc.root["pts"][bbq.this.y > 0]
        assert bytes(doc) == before
        assert doc.deltas() == []


# ── Where a node is, and what the document looks like ────────────────────────

NAV_SPEC = """\
Chunk = struct { kind: uint8, len: uint8, data: bytes[len] }
File  = struct { magic: uint32be, n: uint8, chunks: array<Chunk>[n] }
"""

NAV_DATA = bytes([0xDE, 0xAD, 0xBE, 0xEF, 2, 1, 2, 0xAA, 0xBB, 7, 1, 0xCC])


@pytest.fixture
def navdoc():
    r = bbq.compile_string(NAV_SPEC).parse(NAV_DATA, rule="File")
    assert r._success, r._error_message
    return r


class TestProvenance:
    """A query result has to be able to say where it came from.

    A descendant search over a real file hands back dozens of nodes with the
    same name; without a path, telling them apart means having tracked the walk
    by hand, which is the work the search was supposed to do. This is what RFC
    9535 defines normalized paths (§2.7) for.
    """

    def test_a_search_result_knows_its_path(self, navdoc):
        hits = navdoc._root[..., "kind"]
        assert [h._path for h in hits] == ["$.chunks[0].kind", "$.chunks[1].kind"]

    def test_steps_are_actionable(self, navdoc):
        """The path as data: reduce(getitem, steps, root) is the node again."""
        import functools, operator
        for hit in navdoc._root[..., "kind"]:
            back = functools.reduce(operator.getitem, hit._steps, navdoc._root)
            assert back._offset == hit._offset

    def test_an_array_element_is_identified_by_position(self, navdoc):
        """Decided by the PARENT's type, not by whether the child has a name:
        an array element carries an EMPTY name, so keying on that rendered
        every element as "." and made sibling paths identical."""
        elem = navdoc._root["chunks"][1]
        assert elem._steps == ("chunks", 1)
        assert elem._path == "$.chunks[1]"

    def test_the_root_has_an_empty_path(self, navdoc):
        assert navdoc._root._steps == ()
        assert navdoc._root._path == "$"

    def test_a_node_reached_any_way_has_the_same_path(self, navdoc):
        """Attribute, subscript, iteration and query must agree."""
        by_attr = navdoc._root.chunks[0].kind
        by_item = navdoc._root["chunks"][0]["kind"]
        by_iter = list(navdoc._root["chunks"])[0]["kind"]
        by_query = navdoc._root[..., "kind"].nodes[0]
        assert by_attr._path == by_item._path == by_iter._path == by_query._path


class TestNavigation:
    def test_parent_and_root(self, navdoc):
        data = navdoc._root["chunks"][1]["data"]
        assert data._parent._path == "$.chunks[1]"
        assert data._root._path == "$"
        assert data._root._offset == navdoc._root._offset

    def test_the_root_has_no_parent(self, navdoc):
        assert navdoc._root._parent is None

    def test_walking_up_reaches_the_root(self, navdoc):
        node, seen = navdoc._root["chunks"][0]["len"], 0
        while node._parent is not None:
            node, seen = node._parent, seen + 1
        assert seen == 3                      # len -> chunk -> chunks -> root
        assert node._path == "$"

    def test_going_up_then_down_reaches_a_sibling(self, navdoc):
        """The thing one-way navigation cannot do: look at a neighbour."""
        length = navdoc._root["chunks"][0]["len"]
        assert length._parent["kind"].value == 1

    def test_a_node_keeps_its_ancestors_alive(self, navdoc):
        """The chain is strong refs, so a node handed out of a function does not
        leave dangling parents behind it."""
        def find():
            return bbq.compile_string(NAV_SPEC).parse(NAV_DATA, rule="File")._root[..., "kind"].nodes[0]
        hit = find()
        gc.collect()
        assert hit._path == "$.chunks[0].kind"
        assert hit._root._path == "$"


class TestDump:
    """A library whose job is "what is in this file" has to be able to show you.

    Every walker in this repo's own suite — dump_node in cross_backend_test.cpp,
    the walk in TestContainerProtocol — is this function written again by hand.
    """

    def test_dump_shows_the_whole_tree(self, navdoc):
        out = navdoc.dump()
        assert "$: struct(3)" in out
        assert "magic: uint32be = 3735928559" in out
        assert "[0]: struct(3)" in out and "[1]: struct(3)" in out
        assert out.count("kind: uint8") == 2

    def test_dump_carries_offsets(self, navdoc):
        assert "@0..4" in navdoc.dump()          # magic
        assert "@11..12" in navdoc.dump()        # the last chunk's data

    def test_dump_indents_by_depth(self, navdoc):
        lines = {l.strip().split(":")[0]: len(l) - len(l.lstrip())
                 for l in navdoc.dump().splitlines()}
        assert lines["$"] == 0
        assert lines["magic"] == 2
        assert lines["[0]"] == 4
        assert lines["kind"] == 6

    def test_dump_can_be_depth_limited(self, navdoc):
        shallow = navdoc.dump(depth=1)
        assert "chunks: array(2)" in shallow
        assert "kind" not in shallow

    def test_a_node_dumps_its_own_subtree(self, navdoc):
        out = navdoc._root["chunks"][1].dump()
        assert out.startswith("[1]: struct(3)")
        assert "kind: uint8 = 7" in out
        assert "magic" not in out

    def test_a_long_value_is_truncated(self):
        spec = bbq.compile_string("Foo = struct { n: uint8, blob: bytes[n] }")
        r = spec.parse(bytes([200]) + bytes(range(200)))
        line = [l for l in r.dump().splitlines() if "blob" in l][0]
        assert "..." in line and len(line) < 120


class TestNamesDoNotShadowFields:
    """A format names its own fields, and real formats have fields called
    `name`, `value`, `offset`, `length`, `type`, `parent`.

    The rule, which Kaitai Struct answers the same way with _parent/_root/_io
    and namedtuple with _fields/_replace:

      node["x"]  is ALWAYS the field — never a library member.
      node._x    is ALWAYS the library member — a field cannot take it.
      node.x     is the field when there is one, else the library member.

    Before this, .name/.value/.offset/.raw were silently unreachable as fields:
    a format with a `name` field read the node's own name instead and said
    nothing about it.
    """

    SPEC = ("Foo = struct { name: uint8, value: uint8, offset: uint8, raw: uint8, "
            "parent: uint8, root: uint8, path: uint8, steps: uint8, dump: uint8, "
            "capture_type: uint8, parsed: uint8, keys: uint8, _value: uint8 }")
    FIELDS = ["name", "value", "offset", "raw", "parent", "root", "path", "steps",
              "dump", "capture_type", "parsed", "keys", "_value"]

    @pytest.fixture
    def hostile(self):
        r = bbq.compile_string(self.SPEC).parse(bytes(range(1, len(self.FIELDS) + 1)))
        assert r._success
        return r

    @pytest.mark.parametrize("field", FIELDS)
    def test_the_field_wins_attribute_access(self, hostile, field):
        if field.startswith("_"):
            pytest.skip("the underscored canon is the library's; see the subscript test")
        got = getattr(hostile._root, field)
        assert isinstance(got, bbq.Node), f"node.{field} gave the library, not the field"
        assert got.value == self.FIELDS.index(field) + 1

    @pytest.mark.parametrize("field", FIELDS)
    def test_subscript_always_reaches_the_field(self, hostile, field):
        assert hostile._root[field].value == self.FIELDS.index(field) + 1

    def test_the_underscored_canon_still_works(self, hostile):
        root = hostile._root
        assert root._name is None
        assert root._offset == (0, len(self.FIELDS))
        assert root._path == "$"
        assert root._parent is None
        assert root._capture_type == "struct"
        assert root._keys() == self.FIELDS
        assert "name: uint8 = 1" in root._dump()

    def test_the_result_follows_the_same_rule(self, hostile):
        assert hostile.root.value == self.FIELDS.index("root") + 1   # the field
        assert hostile._root._capture_type == "struct"               # the document
        assert hostile._success is True

    def test_a_format_without_those_names_is_unaffected(self, navdoc):
        """The convenience spelling still works wherever nothing claims it."""
        assert navdoc._root["chunks"][0].kind.value == 1
        assert navdoc._root["chunks"][0]["kind"].name == "kind"
        assert navdoc.root._capture_type == "struct"

    def test_dir_offers_both(self, hostile):
        names = dir(hostile._root)
        assert "name" in names and "_name" in names


class TestOnePathSpelling:
    """Everything that names a node names it the same way.

    Two walks produce paths — `_path` ascends a node's parent chain, `deltas()`
    descends the tree looking for leaves that stopped being spans — and they each
    used to format their own. They disagreed (`pts[1].y` against `$.pts[1].y`),
    and the delta side decided positional-vs-named by whether the CHILD carried a
    name, which is wrong for the same reason it was wrong in `_steps`: an array
    element carries an EMPTY name. They share the step formatter now.
    """

    SPEC = "Pt = struct { x: uint8, y: uint8 }\nT = struct { n: uint8, pts: array<Pt>[n] }"

    @pytest.fixture
    def edited(self):
        r = bbq.compile_string(self.SPEC).parse(bytes([2, 1, 2, 3, 4]), rule="T")
        r._root["pts"][1]["y"] = 9
        r._root["n"] = 5
        return r

    def test_deltas_and_nodes_agree(self, edited):
        from_deltas = sorted(d["path"] for d in edited.deltas())
        from_nodes = sorted([edited._root["n"]._path,
                             edited._root["pts"][1]["y"]._path])
        assert from_deltas == from_nodes == ["$.n", "$.pts[1].y"]

    def test_a_delta_path_is_rooted_like_a_node_path(self, edited):
        assert all(d["path"].startswith("$") for d in edited.deltas())

    def test_an_array_element_is_positional_on_both_sides(self, edited):
        """The bug the shared formatter fixes: deciding by the child's name
        rather than the parent's type."""
        assert "$.pts[1].y" in [d["path"] for d in edited.deltas()]
        assert edited._root["pts"][1]["y"]._path == "$.pts[1].y"

    def test_a_delta_path_resolves_back_to_its_node(self, edited):
        """The property that makes a path an answer and not a label."""
        import functools, operator
        for d in edited.deltas():
            node = [n for n in edited._root[...] if n._path == d["path"]]
            assert len(node) == 1, d["path"]
            assert node[0]._offset == d["offset"]
            assert functools.reduce(operator.getitem, node[0]._steps,
                                    edited._root)._offset == d["offset"]
