// Pins for the Java backend: `-lang java` type mapping and templates/java.inja.
//
// The generated Java has to compile under a Java 1.0 compiler (javelinac), which
// is a much narrower target than javac: no nested classes, no generics, no enum
// keyword, no collections. These pins encode that narrowness — a template that
// emits idiomatic modern Java passes javac and fails here, which is the point.
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include "generator.h"
#include "type_registry.h"

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

static std::string source_path(const char* relative) {
    return std::string(SOURCE_DIR) + "/" + relative;
}

// Render examples/java_fixture.asdl through templates/java.inja.
static std::string render_fixture(const char* package = nullptr) {
    Generator gen;
    gen.types().set_lang_java();
    auto* ast = gen.parse(source_path("examples/java_fixture.asdl"));
    EXPECT_NE(ast, nullptr);
    auto module = gen.to_json(ast);
    gen.process(module);
    if (package) module["java_package"] = package;
    std::stringstream out;
    gen.generate(module, source_path("templates/java.inja"), out);
    return out.str();
}

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// --- Type mapping ---

TEST(JavaTypeRegistry, MapsPrimitivesToJava) {
    TypeRegistry types;
    types.set_lang_java();
    EXPECT_EQ(types.get_type("int"), "int");
    EXPECT_EQ(types.get_type("int64"), "long");
    EXPECT_EQ(types.get_type("bool"), "boolean");
    EXPECT_EQ(types.get_type("float"), "float");
    EXPECT_EQ(types.get_type("string"), "String");
    EXPECT_EQ(types.get_type("identifier"), "String");
}

TEST(JavaTypeRegistry, IdentifiesJavaBaseTypes) {
    TypeRegistry types;
    types.set_lang_java();
    EXPECT_TRUE(types.is_base_type("int"));
    EXPECT_TRUE(types.is_base_type("int64"));
    EXPECT_TRUE(types.is_base_type("bool"));
    EXPECT_TRUE(types.is_base_type("string"));
    EXPECT_TRUE(types.is_base_type("identifier"));
}

// A sum type is a reference in Java — no pointer suffix leaking from C++ mode.
TEST(JavaTypeRegistry, SumTypeIsAPlainReference) {
    Generator gen;
    gen.types().set_lang_java();
    auto* ast = gen.parse(source_path("examples/java_fixture.asdl"));
    ASSERT_NE(ast, nullptr);
    auto module = gen.to_json(ast);
    gen.process(module);
    EXPECT_EQ(gen.types().get_type("expr"), "Expr");
    EXPECT_EQ(gen.types().get_type("span"), "Span");
    // An enum-like sum carries no payload, so it is an int constant domain.
    EXPECT_EQ(gen.types().get_type("color"), "int");
}

// --- asdl_java_sum_emits_kind_constants ---

TEST(JavaTemplate, SumEmitsKindConstants) {
    std::string out = render_fixture();
    EXPECT_TRUE(contains(out, "public static final int KIND_LIT"));
    EXPECT_TRUE(contains(out, "public static final int KIND_ADD"));
    EXPECT_TRUE(contains(out, "public static final int KIND_CALL"));
    EXPECT_TRUE(contains(out, "public static final int KIND_NIL"));
    // The tag lives on the base and is set by each subclass constructor, so a
    // switch on `kind` is a jump table rather than a megamorphic virtual call.
    EXPECT_TRUE(contains(out, "public int kind;"));
    // Qualified: an inherited static read inside an explicit constructor
    // invocation is the narrowest thing a 1.0 compiler has to resolve, so the
    // template never leans on it.
    EXPECT_TRUE(contains(out, "super(Expr.KIND_LIT)"));
}

TEST(JavaTemplate, EnumLikeSumIsIntConstantsOnly) {
    std::string out = render_fixture();
    EXPECT_TRUE(contains(out, "public static final int KIND_RED"));
    EXPECT_TRUE(contains(out, "public static final int KIND_GREEN"));
    EXPECT_TRUE(contains(out, "public static final int KIND_BLUE"));
    // No payload classes for a field-free sum.
    EXPECT_FALSE(contains(out, "class Red"));
    EXPECT_FALSE(contains(out, "class Green"));
}

// --- asdl_java_seq_is_array_not_vector ---

TEST(JavaTemplate, SequenceIsArrayNotVector) {
    std::string out = render_fixture();
    EXPECT_TRUE(contains(out, "public Expr[] args;"));
    EXPECT_TRUE(contains(out, "public Expr[] many;"));
    // java.util.Vector boxes every element and Java 1.0 has no generics; an
    // ASDL sequence is an array or it is a performance bug.
    EXPECT_FALSE(contains(out, "Vector"));
    EXPECT_FALSE(contains(out, "ArrayList"));
}

// --- asdl_java_is_java_1_0 ---

TEST(JavaTemplate, NoConstructsPastJava10) {
    std::string out = render_fixture();
    // Generics (1.5), enum (1.5), and the collections framework (1.2).
    EXPECT_FALSE(contains(out, "<"));
    EXPECT_FALSE(contains(out, "enum "));
    EXPECT_FALSE(contains(out, "@Override"));
    EXPECT_FALSE(contains(out, "java.util."));

    // Nested classes are 1.1 and javelinac does not parse them: every class
    // declaration must start at column zero.
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        size_t at = line.find("class ");
        if (at == std::string::npos) continue;
        EXPECT_EQ(line.find_first_not_of(" \t"), 0u)
            << "indented (nested) class declaration: " << line;
    }
}

// inja's lstrip_blocks eats whitespace next to a block tag, which silently
// fused every parameter type onto its name (`int value` as one token). The
// generated file parses as Java only because `intvalue` is a legal identifier,
// so nothing but a spelling pin catches it.
TEST(JavaTemplate, ConstructorParametersAreSpaced) {
    std::string out = render_fixture();
    EXPECT_TRUE(contains(out, "public Lit(int value)"));
    EXPECT_TRUE(contains(out, "public Add(Expr lhs, Expr rhs)"));
    EXPECT_TRUE(contains(out, "public Call(String name, Expr[] args)"));
    EXPECT_TRUE(contains(out, "public Span(int start, int end)"));
}

// A single-constructor sum is one final class. An abstract base would be named
// camelcase(def) — which is exactly the constructor name for the usual
// `unit = Unit(...)` shape, giving `class Unit extends Unit`.
TEST(JavaTemplate, SingleConstructorSumIsOneClass) {
    std::string out = render_fixture();
    EXPECT_TRUE(contains(out, "public final class Unit {"));
    EXPECT_FALSE(contains(out, "extends Unit"));
    EXPECT_FALSE(contains(out, "abstract class Unit"));
    EXPECT_FALSE(contains(out, "extends Prims"));
}

TEST(JavaTemplate, ProductIsFinalClass) {
    std::string out = render_fixture();
    EXPECT_TRUE(contains(out, "public final class Span"));
    EXPECT_TRUE(contains(out, "public int start;"));
    EXPECT_TRUE(contains(out, "public int end;"));
}

TEST(JavaTemplate, EveryPrimitiveLandsWithItsJavaType) {
    std::string out = render_fixture();
    EXPECT_TRUE(contains(out, "public boolean flag;"));
    EXPECT_TRUE(contains(out, "public int n;"));
    EXPECT_TRUE(contains(out, "public long big;"));
    EXPECT_TRUE(contains(out, "public float f;"));
    EXPECT_TRUE(contains(out, "public String s;"));
    EXPECT_TRUE(contains(out, "public String id;"));
    EXPECT_TRUE(contains(out, "public Expr maybe;"));
}

// JLS 1.0 section 8.3.1.2: "A field can be declared final, in which case its
// declarator must include a variable initializer or a compile-time error
// occurs." Blank finals are Java 1.1. javelinac enforces the 1.0 rule
// (compiler/src/compiler/sema.c), so a `final` field the constructor assigns is
// 34 compile errors, not a style preference. Class-level final is 1.0 and stays.
TEST(JavaTemplate, NoBlankFinalFields) {
    std::string out = render_fixture();
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 2, "//") == 0) continue;
        if (line.find("class ") != std::string::npos) continue;   // final class is fine
        if (line.find("static final") != std::string::npos) continue;  // initialised constants
        EXPECT_EQ(line.find(" final "), std::string::npos)
            << "blank final field (Java 1.1): " << line;
    }
}

TEST(JavaTemplate, PackageDeclarationWhenAsked) {
    EXPECT_TRUE(contains(render_fixture("com.example.peg"), "package com.example.peg;"));
    // ...and none at all when not asked, so the default output is placeable.
    EXPECT_FALSE(contains(render_fixture(), "package "));
}
