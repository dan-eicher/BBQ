#include <gtest/gtest.h>
#include "Parser.h"
#include "BBQ_AST.h"
#include "Sema.h"

using namespace BBQ;
using namespace bbqgen;

static Grammar* parse(const char* src) {
    auto* parser = new Parser();
    parser->init(src, static_cast<int>(strlen(src)));
    if (!parser->parse()) return nullptr;
    return parser->ast;
}

TEST(Sema, ValidSimpleStruct) {
    auto* g = parse("Foo = struct { x: uint8, y: uint16le }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    EXPECT_FALSE(errors.has_errors());
}

TEST(Sema, ValidConstraintOnEarlierField) {
    auto* g = parse(
        "H = struct {\n"
        "    a: uint8,\n"
        "    b: uint8 where a > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, UndefinedRuleRef) {
    auto* g = parse("F = struct { hdr: Nonexistent }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(errors.has_errors());
    // Should mention "undefined rule"
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("undefined rule") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, DuplicateRuleNames) {
    auto* g = parse(
        "Foo = struct { x: uint8 }\n"
        "Foo = struct { y: uint16le }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("duplicate rule") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, CircularDependency) {
    auto* g = parse(
        "A = struct { b: B }\n"
        "B = struct { a: A }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("circular") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, ForwardRefInConstraint) {
    // The parser puts the 'where' on the Primitive, not the Field.
    // So "a: uint8 where b > 0" puts the constraint on the Primitive node.
    // Sema needs to detect forward refs in Primitive constraints too.
    auto* g = parse(
        "H = struct {\n"
        "    a: uint8 where b > 0,\n"
        "    b: uint8\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("forward reference") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, ForwardRefInCompute) {
    auto* g = parse(
        "H = struct {\n"
        "    ihl: compute(ver_ihl & 0x0F : uint8),\n"
        "    ver_ihl: uint8\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("forward reference") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, SwitchDuplicateCaseValues) {
    auto* g = parse(
        "P = switch(t) {\n"
        "    1: FooType;\n"
        "    1: BarType;\n"
        "    default: BazType;\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("duplicate switch case") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, SwitchNoDefaultWarning) {
    auto* g = parse(
        "P = switch(t) {\n"
        "    1: FooType;\n"
        "    2: BarType;\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    // Should have a warning about no default
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.is_warning && e.message.find("no default") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, ValidDependencyOrdering) {
    auto* g = parse(
        "Header = struct { magic: uint32be }\n"
        "Body = struct { data: bytes[16] }\n"
        "File = struct { hdr: Header, body: Body }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    // Sorted should have Header and Body before File
    auto& sorted = sema.sorted_rules();
    ASSERT_EQ(sorted.size(), 3u);
    // Find indices
    int header_idx = -1, body_idx = -1, file_idx = -1;
    for (int i = 0; i < (int)sorted.size(); i++) {
        if (sorted[i]->name == "Header") header_idx = i;
        if (sorted[i]->name == "Body") body_idx = i;
        if (sorted[i]->name == "File") file_idx = i;
    }
    EXPECT_LT(header_idx, file_idx);
    EXPECT_LT(body_idx, file_idx);
}

TEST(Sema, ForwardRefInInterval) {
    auto* g = parse(
        "H = struct {\n"
        "    data: bytes[len],\n"
        "    len: uint16le\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("forward reference") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, ValidInterval) {
    auto* g = parse(
        "H = struct {\n"
        "    len: uint16le,\n"
        "    data: bytes[len]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, PrimitiveConstraintValidation) {
    // Constraint on primitive referencing a non-existent rule shouldn't crash
    auto* g = parse(
        "H = struct {\n"
        "    a: uint8,\n"
        "    b: uint8 where a < 10\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    // Should succeed — 'a' is defined before 'b'
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, EndianDirective) {
    auto* g = parse(
        "@endian big\n"
        "H = struct { x: uint32 }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    EXPECT_EQ(sema.default_endian(), Endianness::Big);
}

TEST(Sema, LookupRule) {
    auto* g = parse(
        "Header = struct { x: uint8 }\n"
        "Body = struct { y: uint16le }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    // Found rules
    EXPECT_NE(sema.lookup_rule("Header"), nullptr);
    EXPECT_EQ(sema.lookup_rule("Header")->name, "Header");
    EXPECT_NE(sema.lookup_rule("Body"), nullptr);
    // Not found
    EXPECT_EQ(sema.lookup_rule("Nonexistent"), nullptr);
}

TEST(Sema, ExternValid) {
    auto* g = parse(
        "Payload = extern(\"decompress_zlib\", \"std::vector<uint8_t>\")");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, ElementIntervalValid) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    num: uint32le,\n"
        "    off: uint32le,\n"
        "    sz: uint32le,\n"
        "    entries: array<Entry>[num] @ [off + i * sz, off + (i + 1) * sz]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, ElementIntervalOnEof) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "Items = array<Entry>(none, eof) @ [i * 4, (i + 1) * 4]");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("counted array") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, BitfieldValid) {
    auto* g = parse("Flags = bitfield<uint8> { a: 4, b: 4 }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    EXPECT_FALSE(errors.has_errors());
}

TEST(Sema, BitfieldWidthMismatch) {
    auto* g = parse("Flags = bitfield<uint8> { a: 4, b: 4, c: 4 }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("sum to 12 bits") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, BitfieldNotInteger) {
    auto* g = parse("Flags = bitfield<bytes> { a: 8 }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("integer type") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(Sema, BitfieldDuplicateEntryName) {
    auto* g = parse("Flags = bitfield<uint8> { a: 4, a: 4 }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("duplicate bitfield entry") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

// === Phase 2: Expression Type Checking Tests ===

// Helper to check for a specific error message substring
static bool has_error_containing(const ErrorReporter& errors, const std::string& substr) {
    for (auto& e : errors.errors()) {
        if (!e.is_warning && e.message.find(substr) != std::string::npos)
            return true;
    }
    return false;
}

static bool has_warning_containing(const ErrorReporter& errors, const std::string& substr) {
    for (auto& e : errors.errors()) {
        if (e.is_warning && e.message.find(substr) != std::string::npos)
            return true;
    }
    return false;
}

// --- Item 9: Expression type checking ---

TEST(Sema, ConstraintMustBeBoolean) {
    // String type is not boolean-compatible
    auto* g = parse(
        "H = struct {\n"
        "    s: string,\n"
        "    y: uint8 where s\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "constraint must be boolean"));
}

TEST(Sema, ConstraintWithComparisonValid) {
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8,\n"
        "    y: uint8 where x > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, BitwiseOnFloatErrors) {
    auto* g = parse(
        "H = struct {\n"
        "    f: float32le,\n"
        "    x: uint8 where (f & 0xFF) == 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "bitwise operand must be integer"));
}

TEST(Sema, IntegerArithmeticValid) {
    auto* g = parse(
        "H = struct {\n"
        "    a: uint8,\n"
        "    b: uint8 where (a * 2) + 1 > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, BuiltinRemainingIsInteger) {
    auto* g = parse(
        "H = struct {\n"
        "    data: bytes[remaining]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, BuiltinAtEndIsBoolean) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "Items = array<Entry>(none, until(at_end))");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, ExternFieldPermissive) {
    // Unknown types (extern) should not produce errors
    auto* g = parse(
        "Payload = extern(\"decompress_zlib\", \"std::vector<uint8_t>\")\n"
        "H = struct {\n"
        "    data: Payload,\n"
        "    x: uint8 where data > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, StringArithmeticErrors) {
    auto* g = parse(
        "H = struct {\n"
        "    s: string,\n"
        "    x: uint8 where s + 1 > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "arithmetic operand must be numeric"));
}

TEST(Sema, LogicalOnNumericErrors) {
    auto* g = parse(
        "H = struct {\n"
        "    x: float32le,\n"
        "    y: uint8 where x && true\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "logical operand must be boolean"));
}

// --- Item 10: Interval bounds validation ---

TEST(Sema, NonIntegerIntervalErrors) {
    auto* g = parse(
        "H = struct {\n"
        "    f: float32le,\n"
        "    data: bytes[f]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "interval length must be integer"));
}

TEST(Sema, StaticIntervalStartGEEnd) {
    auto* g = parse(
        "H = struct {\n"
        "    data: uint8[10, 5]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "interval start must be less than end"));
}

TEST(Sema, StaticIntervalNegativeStart) {
    auto* g = parse(
        "H = struct {\n"
        "    data: uint8[-1, 10]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "interval start must be non-negative"));
}

TEST(Sema, DynamicIntegerIntervalValid) {
    auto* g = parse(
        "H = struct {\n"
        "    off: uint32le,\n"
        "    len: uint32le,\n"
        "    data: bytes[off, off + len]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, BoolIntervalLengthErrors) {
    auto* g = parse(
        "H = struct {\n"
        "    flag: bool,\n"
        "    data: bytes[flag]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "interval length must be integer"));
}

// --- Item 11: Switch discriminator scope ---

TEST(Sema, SwitchDiscriminatorForwardRef) {
    auto* g = parse(
        "A = struct { x: uint8 }\n"
        "B = struct { x: uint8 }\n"
        "H = struct {\n"
        "    payload: switch(tag) {\n"
        "        1: A;\n"
        "        default: B;\n"
        "    },\n"
        "    tag: uint8\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "forward reference"));
}

TEST(Sema, SwitchDiscriminatorBackRefValid) {
    auto* g = parse(
        "A = struct { x: uint8 }\n"
        "B = struct { x: uint8 }\n"
        "H = struct {\n"
        "    tag: uint8,\n"
        "    payload: switch(tag) {\n"
        "        1: A;\n"
        "        default: B;\n"
        "    }\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

// --- Item 12: Array parameter type validation ---

TEST(Sema, NonIntegerCountErrors) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    flag: bool,\n"
        "    items: array<Entry>[flag]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "array count must be integer"));
}

TEST(Sema, IntegerCountValid) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    num: uint16le,\n"
        "    items: array<Entry>[num]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, NonBooleanUntilErrors) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    f: float32le,\n"
        "    items: array<Entry>(none, until(f + 1))\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "until condition must be boolean"));
}

TEST(Sema, UntilAtEndValid) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    items: array<Entry>(none, until(at_end))\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

// --- Item 13: Field/array access validation ---

TEST(Sema, ValidNestedFieldAccess) {
    auto* g = parse(
        "Header = struct { version: uint8 }\n"
        "File = struct {\n"
        "    hdr: Header,\n"
        "    data: uint8 where hdr.version > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, NonexistentFieldAccess) {
    auto* g = parse(
        "Header = struct { version: uint8 }\n"
        "File = struct {\n"
        "    hdr: Header,\n"
        "    data: uint8 where hdr.nonexistent > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "no field"));
}

TEST(Sema, NonIntegerArrayIndexErrors) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    f: float32le,\n"
        "    items: array<Entry>[3],\n"
        "    y: uint8 where items[f].x > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "array index must be integer"));
}

TEST(Sema, ExternFieldAccessPermissive) {
    // Accessing fields on an extern type should be permissive (Unknown)
    auto* g = parse(
        "Payload = extern(\"decompress_zlib\", \"std::vector<uint8_t>\")\n"
        "H = struct {\n"
        "    data: Payload,\n"
        "    x: uint8 where data.something > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, SelfReferenceValid) {
    // A field can reference itself in its constraint
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8 where x > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

// === Phase 3: Advanced Diagnostics Tests ===

// --- Item 14: Compute result type matching ---

TEST(Sema, ComputeStringToIntMismatch) {
    auto* g = parse(
        "H = struct {\n"
        "    s: string,\n"
        "    x: compute(s : uint8)\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "compute expression type incompatible"));
}

TEST(Sema, ComputeIntegerToIntegerValid) {
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8,\n"
        "    y: compute(x + 1 : uint32le)\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, ComputeNumericWideningValid) {
    // Integer expression with float result type is OK (numeric widening)
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8,\n"
        "    y: compute(x * 2 : float32le)\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, ComputeBoolResultValid) {
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8,\n"
        "    y: compute(x > 0 : bool)\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, ComputeBoolToIntMismatch) {
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8,\n"
        "    y: compute(x > 0 : uint32le)\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "compute expression type incompatible"));
}

// --- Item 15: Extern signature validation ---

TEST(Sema, ExternEmptyFuncName) {
    auto* g = parse("P = extern(\"\", \"int\")");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "extern function name must not be empty"));
}

TEST(Sema, ExternInvalidIdentifier) {
    auto* g = parse("P = extern(\"123bad\", \"int\")");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "not a valid identifier"));
}

TEST(Sema, ExternNamespaceQualifiedValid) {
    auto* g = parse("P = extern(\"ns::func\", \"MyType\")");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, ExternEmptyType) {
    auto* g = parse("P = extern(\"func\", \"\")");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "extern type must not be empty"));
}

// --- Item 16: Conflicting constraint detection ---

TEST(Sema, NoConflictingConstraintOnNormal) {
    // Normal case: constraint only on primitive (via parser)
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8 where x > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    EXPECT_FALSE(has_warning_containing(errors, "constraints on both"));
}

// --- Item 17: Unreachable case detection (mixed case types) ---

TEST(Sema, SwitchMixedCaseValues) {
    auto* g = parse(
        "A = struct { x: uint8 }\n"
        "B = struct { x: uint8 }\n"
        "P = switch(t) {\n"
        "    1: A;\n"
        "    \"x\": B;\n"
        "    default: A;\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    EXPECT_TRUE(has_error_containing(errors, "mixes string and integer"));
}

TEST(Sema, SwitchAllIntegerValid) {
    auto* g = parse(
        "A = struct { x: uint8 }\n"
        "B = struct { x: uint8 }\n"
        "P = switch(t) {\n"
        "    1: A;\n"
        "    2: B;\n"
        "    default: A;\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    EXPECT_FALSE(has_error_containing(errors, "mixes string"));
}

TEST(Sema, SwitchAllStringValid) {
    auto* g = parse(
        "A = struct { x: uint8 }\n"
        "B = struct { x: uint8 }\n"
        "P = switch(t) {\n"
        "    \"a\": A;\n"
        "    \"b\": B;\n"
        "    default: A;\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    EXPECT_FALSE(has_error_containing(errors, "mixes string"));
}

// --- Item 18: Did-you-mean suggestions ---

TEST(Sema, DidYouMeanRule) {
    auto* g = parse(
        "Header = struct { x: uint8 }\n"
        "F = struct { hdr: Headr }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "did you mean 'Header'"));
}

TEST(Sema, DidYouMeanField) {
    auto* g = parse(
        "Header = struct { version: uint8 }\n"
        "F = struct {\n"
        "    hdr: Header,\n"
        "    x: uint8 where hdr.versio > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "did you mean 'version'"));
}

TEST(Sema, NoSuggestionForCompletelyWrongName) {
    auto* g = parse("F = struct { hdr: Xyzzy }");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "undefined rule"));
    EXPECT_FALSE(has_error_containing(errors, "did you mean"));
}

// === Nested (inline) struct scope tests ===

TEST(Sema, NestedStructRefsParentField) {
    // Inner struct field can reference a parent field defined before the nested struct
    auto* g = parse(
        "H = struct {\n"
        "    len: uint16le,\n"
        "    inner: struct {\n"
        "        data: bytes[len]\n"
        "    }\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, NestedStructForwardRefToLaterParentField) {
    // Inner struct field referencing a parent field defined AFTER the nested struct → error
    auto* g = parse(
        "H = struct {\n"
        "    inner: struct {\n"
        "        data: bytes[len]\n"
        "    },\n"
        "    len: uint16le\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "forward reference"));
}

TEST(Sema, NestedStructTypeCheckThroughScopes) {
    // Type checking should work through nested scopes
    auto* g = parse(
        "H = struct {\n"
        "    version: uint16le,\n"
        "    inner: struct {\n"
        "        data: uint8 where version > 0\n"
        "    }\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, DeeplyNestedScopeResolution) {
    // Three levels of nesting: innermost can see outermost
    auto* g = parse(
        "H = struct {\n"
        "    len: uint32le,\n"
        "    mid: struct {\n"
        "        flag: uint8,\n"
        "        inner: struct {\n"
        "            data: bytes[len]\n"
        "        }\n"
        "    }\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, NestedComputeRefsParentField) {
    // Compute field inside nested struct referencing parent field → valid
    auto* g = parse(
        "H = struct {\n"
        "    ver_ihl: uint8,\n"
        "    inner: struct {\n"
        "        ihl: compute(ver_ihl & 0x0F : uint8)\n"
        "    }\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, NestedSwitchDiscriminatorRefsParentField) {
    // Switch discriminator in nested struct referencing parent field → valid
    auto* g = parse(
        "A = struct { x: uint8 }\n"
        "B = struct { x: uint8 }\n"
        "H = struct {\n"
        "    tag: uint8,\n"
        "    inner: struct {\n"
        "        payload: switch(tag) {\n"
        "            1: A;\n"
        "            default: B;\n"
        "        }\n"
        "    }\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

// === Runtime endian switching ===

TEST(Sema, EndianSwitchValid) {
    auto* g = parse(
        "TIFF = struct {\n"
        "    byte_order: uint16le,\n"
        "    @endian: byte_order == 0x4949 ? little : big,\n"
        "    magic: uint16,\n"
        "    offset: uint32\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, EndianSwitchNonEndianExpr) {
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8,\n"
        "    @endian: x + 1\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("endian-typed") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected endian-typed error";
}

TEST(Sema, EndianSwitchForwardRef) {
    auto* g = parse(
        "H = struct {\n"
        "    @endian: byte_order == 0x4949 ? little : big,\n"
        "    byte_order: uint16le\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    bool found = false;
    for (auto& e : errors.errors()) {
        if (e.message.find("forward reference") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected forward reference error";
}

TEST(Sema, EndianLitTernaryValid) {
    auto* g = parse(
        "H = struct {\n"
        "    flag: uint8,\n"
        "    @endian: flag == 1 ? little : big,\n"
        "    val: uint16\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

TEST(Sema, EndianLitBareLiteral) {
    auto* g = parse(
        "H = struct {\n"
        "    @endian: little,\n"
        "    val: uint16\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
}

// === Phase 5: Remaining validation items ===

// --- Self-referential constraint tautology ---

TEST(Sema, SelfRefTautologyWarning) {
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8 where x == x\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    EXPECT_TRUE(has_warning_containing(errors, "compares field to itself"));
    EXPECT_TRUE(has_warning_containing(errors, "always true"));
}

TEST(Sema, SelfRefContradictionWarning) {
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8 where x != x\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    EXPECT_TRUE(has_warning_containing(errors, "compares field to itself"));
    EXPECT_TRUE(has_warning_containing(errors, "always false"));
}

TEST(Sema, SelfRefMeaningfulValid) {
    // x > 0 is a meaningful self-referential constraint, no warning
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8 where x > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    EXPECT_FALSE(has_warning_containing(errors, "compares field to itself"));
}

// --- Element interval 'i' validation ---

TEST(Sema, ElementIntervalWithoutIWarning) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    entries: array<Entry>[3] @ [10, 20]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    EXPECT_TRUE(has_warning_containing(errors, "does not reference loop index"));
}

TEST(Sema, ElementIntervalWithIValid) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    entries: array<Entry>[3] @ [i * 4, (i + 1) * 4]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    EXPECT_FALSE(has_warning_containing(errors, "does not reference loop index"));
}

// --- Loop index 'i' outside array context ---

TEST(Sema, LoopVarOutsideArrayErrors) {
    auto* g = parse(
        "H = struct {\n"
        "    x: uint8 where i > 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "loop index 'i' referenced outside"));
}

TEST(Sema, LoopVarInElementIntervalValid) {
    auto* g = parse(
        "Entry = struct { x: uint8 }\n"
        "H = struct {\n"
        "    entries: array<Entry>[3] @ [i * 2, (i + 1) * 2]\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    EXPECT_FALSE(has_error_containing(errors, "loop index 'i' referenced outside"));
}

// === Cross-rule scope resolution ===

TEST(Sema, CrossRuleRefAllowed) {
    // Compute referencing a name that doesn't exist locally → allowed (cross-rule)
    auto* g = parse(
        "Inner = struct {\n"
        "    raw_status: uint8,\n"
        "    effective: compute(raw_status != 0 ? raw_status : parent_field : uint8)\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    // 'parent_field' is not in Inner's scope but that's OK — it's a cross-rule ref
    EXPECT_TRUE(sema.analyze(g));
    EXPECT_FALSE(has_error_containing(errors, "forward reference"));
}

TEST(Sema, ForwardRefStillErrors) {
    // Referencing a LATER LOCAL field is still an error
    auto* g = parse(
        "H = struct {\n"
        "    x: compute(y + 1 : uint8),\n"
        "    y: uint8\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_FALSE(sema.analyze(g));
    EXPECT_TRUE(has_error_containing(errors, "forward reference"));
}

TEST(Sema, LoopVarInCrossRuleCompute) {
    // 'i' in compute expression is valid — resolved via ctx.loop_index()
    auto* g = parse(
        "MidiEvent = struct {\n"
        "    raw_status: uint8,\n"
        "    effective: compute(i > 0 ? raw_status : 0 : uint8)\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    EXPECT_FALSE(has_error_containing(errors, "loop index 'i' referenced outside"));
}

TEST(Sema, PeekTypeIsInteger) {
    auto* g = parse(
        "Msg = struct {\n"
        "    data: array<uint8>(none, until(peek() == 0)),\n"
        "    null: uint8\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    EXPECT_TRUE(sema.analyze(g));
    EXPECT_FALSE(errors.has_errors());
}

TEST(Sema, PeekNoArgs) {
    auto* g = parse(
        "Msg = struct {\n"
        "    x: uint8 where peek(x) == 0\n"
        "}");
    ASSERT_NE(g, nullptr);
    ErrorReporter errors;
    Sema sema(errors);
    sema.analyze(g);
    EXPECT_TRUE(has_error_containing(errors, "peek() takes no arguments"));
}
