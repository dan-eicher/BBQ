"""Tests for the bbq Python extension module."""

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
# `result.field = v` / `node.field = v` detaches that field to Owned in the
# shared overlay (the same one the C++ handles use); reads see the override,
# siblings stay borrowed. emit() then enforces the grammar over the edited
# overlay before serializing — see TestLensLaws for the laws that pin that down.

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
        assert paths == {"tag": (7, 99), "inner.a": (1, 42)}
        # offsets are present and point at the changed bytes
        off = {d["path"]: d["offset"] for d in r.deltas()}
        assert off["tag"] == (0, 1) and off["inner.a"] == (3, 4)

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
        # set_node op): path-copy, re-serialize on emit, neighbors intact.
        ss = bbq.compile_string(
            "It = struct { v: uint8, w: uint8 }\nT = struct { n: uint8, items: array<It>[n] }")
        r = ss.parse(bytes([2, 1, 2, 3, 4]), rule="T")     # items = [{1,2}, {3,4}]
        r.items[0] = b"\x09\x08"                            # splice element 0
        r2 = ss.parse(r.emit(), rule="T")
        assert int(r2.n) == 2
        assert int(r2.items[0].v) == 9 and int(r2.items[0].w) == 8
        assert int(r2.items[1].v) == 3 and int(r2.items[1].w) == 4   # neighbor preserved

    def test_splice_compose_with_remove_and_append(self):
        # splice + remove + append compose — the overlay's baseline→node map stays
        # consistent (a removed element's whole subtree leaves the map; no dangling).
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
# parse/emit is a lens (Foster et al., TOPLAS 2007, Def 3.2): get = parse (bytes ->
# capture view), put = emit (edited view x ORIGINAL bytes -> new bytes). `put` takes
# the original because `get` discards information — which is exactly what the ZCow
# overlay retains.
#
# Def 3.2 is TWO typing conditions and TWO laws, and the typings do most of the work:
#
#   (GET)    get(C) ⊆ A
#   (PUT)    put(A × C) ⊆ C     put must land back in C — a document that PARSES
#   GetPut   put(get(c), c) ⊑ c  an unedited re-emit is byte-identical
#   PutGet   get(put(a, c)) ⊑ a  re-parsing an edit yields the edit you made
#
# `⊑` holds vacuously when its left side is undefined, so PutGet is satisfied by
# emitting garbage that fails to parse — which is why (PUT) is the condition that
# actually forbids a broken write, and why `_lens` below asserts the re-parse
# explicitly rather than trusting PutGet.
#
# PutGet also forces dependent fields (Nail's term: length/count/offset fields,
# "not exposed in the data model, but instead transparently computed"). An append
# that leaves the count field stale fails it; so does letting the caller set a count
# that emit() then overwrites. These mirror the C++ ZCow writer's
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

    # ── (PUT): put(A × C) ⊆ C — emit() may not hand back a non-member ──────────
    # An edit that changes what SHAPE follows it (a switch discriminant, a value a
    # `where` rejects) leaves the writer replaying the shape the parse recorded. The
    # result is bytes that do not parse. PutGet does not catch this — it is satisfied
    # vacuously — so the typing condition is the one that has to be enforced.

    def test_put_refuses_when_a_discriminant_changed(self):
        spec = bbq.compile_string(
            "T = struct { kind: uint8, body: switch (kind) { 1: uint8; 2: uint32le; } }")
        r = spec.parse(bytes([1, 0xAA]))
        r.kind = 2                              # now claims a 4-byte body over 1 byte
        with pytest.raises(bbq.ParseError):
            r.emit()

    def test_put_refuses_when_an_edit_violates_a_where(self):
        spec = bbq.compile_string("T = struct { ver: uint8 where ver == 1, x: uint8 }")
        r = spec.parse(bytes([1, 42]))
        r.ver = 9
        with pytest.raises(bbq.ParseError):
            r.emit()

    @pytest.mark.parametrize("label,src,data,field,value", [
        # The membership test is a re-parse, so it is not scoped to the shapes anyone
        # thought to enumerate. These are the OTHER ways an edit can decide a shape;
        # they are here so a cheaper approximation cannot quietly replace it.
        ("interval offset", "T = struct { off: uint8, val: uint8 [off, off + 1] }",
         bytes([1, 0xAA, 0xBB]), "off", 200),
        ("optional predicate", "T = struct { has: uint8, v: optional<uint16le> where has == 1 }",
         bytes([1, 0x34, 0x12]), "has", 0),
        ("where on a plain field", "T = struct { n: uint8 where n == 1, w: uint16be }",
         bytes([1, 0x12, 0x34]), "n", 2),
        ("switch discriminant",
         "T = struct { kind: uint8, body: switch (kind) { 1: uint8; 2: uint32le; } }",
         bytes([1, 0xAA]), "kind", 2),
    ])
    def test_put_refuses_every_kind_of_shape_change(self, label, src, data, field, value):
        spec = bbq.compile_string(src)
        r = spec.parse(data)
        setattr(r, field, value)
        with pytest.raises(bbq.ParseError):
            r.emit()

    def test_put_still_emits_for_a_legal_edit(self):
        # The refusal must not be a blanket "any edit is suspicious".
        spec = bbq.compile_string("T = struct { ver: uint8 where ver == 1, x: uint8 }")
        r = spec.parse(bytes([1, 42]))
        r.x = 43
        assert r.emit() == bytes([1, 43])

    # ── Dependent fields are derived, not settable ────────────────────────────
    # Nail: a count/length field is "not exposed in the data model, but instead
    # transparently computed". Accepting an assignment and then overwriting it is a
    # non-vacuous PutGet violation — the document parses, it just is not the view
    # that was asked for.

    def test_setting_a_dependent_count_is_refused(self):
        spec = bbq.compile_string("T = struct { n: uint8, xs: array<uint8>[n] }")
        r = spec.parse(bytes([2, 10, 20]))
        with pytest.raises(TypeError):
            r.n = 99

    def test_setting_a_rest_size_is_refused(self):
        spec = bbq.compile_string(
            "T = struct { sz: uint8 @rest, xs: array<uint8>(none, eof) }")
        r = spec.parse(bytes([2, 10, 20]))
        with pytest.raises(TypeError):
            r.sz = 99

    def test_a_free_field_beside_a_dependent_one_still_sets(self):
        spec = bbq.compile_string("T = struct { tag: uint8, n: uint8, xs: array<uint8>[n] }")
        r = spec.parse(bytes([7, 2, 10, 20]))
        r.tag = 9                                # not dependent — must still work
        assert spec.parse(r.emit()).tag == 9

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

    def test_slice_leaf_raises(self):
        spec = bbq.compile_string(TRIVIAL_SPEC)
        result = spec.parse(b"\x00\x00\x00\x00")
        with pytest.raises(TypeError, match="not subscriptable"):
            result.magic[0:1]

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
        # A failed parse produced no document, so there is no grammar to enforce over
        # it — emit() is the raw serialization of the (untouched) overlay: the input.
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
    Inputs + asserted shapes mirror the C++ reader's FieldCapture (cross_backend_test)."""

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
        r2 = spec.parse(r.emit(), rule="T")     # `n` is derived; emit() writes it
        got = [(int(it.v), int(it.w)) for it in r2.items]
        assert int(r2.n) == 2 and got == [(9, 8), (3, 4)]

    def test_build_new_file_from_scratch(self):
        spec = bbq.compile_string("Rec = struct { magic: uint32be, n: uint8, xs: array<uint16le>[n] }")
        data = bytes(B.Struct(magic=B.u32(0xCAFEBABE, endian="big"), n=B.u8(3),
                              xs=B.Array(B.u16(10), B.u16(20), B.u16(30))))
        r = spec.parse(data, rule="Rec")
        assert r.success and int(r.magic) == 0xCAFEBABE and int(r.n) == 3
        assert [int(x) for x in r.xs] == [10, 20, 30]
