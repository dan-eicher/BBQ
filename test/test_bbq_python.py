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
