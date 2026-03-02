#include <gtest/gtest.h>
#include <sstream>
#include <fstream>
#include "generator.h"
#include "type_registry.h"
#include "exceptions.h"

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

static std::string source_path(const char* relative) {
    return std::string(SOURCE_DIR) + "/" + relative;
}

// --- TypeRegistry tests ---

TEST(TypeRegistry, HasBuiltinTypes) {
    TypeRegistry types;
    EXPECT_TRUE(types.has_type("int"));
    EXPECT_TRUE(types.has_type("bool"));
    EXPECT_TRUE(types.has_type("float"));
    EXPECT_TRUE(types.has_type("string"));
    EXPECT_TRUE(types.has_type("identifier"));
}

TEST(TypeRegistry, MapsBuiltinTypesCorrectly) {
    TypeRegistry types;
    EXPECT_EQ(types.get_type("int"), "int");
    EXPECT_EQ(types.get_type("bool"), "bool");
    EXPECT_EQ(types.get_type("float"), "float");
    EXPECT_EQ(types.get_type("string"), "std::string");
    EXPECT_EQ(types.get_type("identifier"), "std::string");
}

TEST(TypeRegistry, IdentifiesBaseTypes) {
    TypeRegistry types;
    EXPECT_TRUE(types.is_base_type("int"));
    EXPECT_TRUE(types.is_base_type("bool"));
    EXPECT_TRUE(types.is_base_type("string"));
}

TEST(TypeRegistry, RegistersCustomTypes) {
    TypeRegistry types;
    types.register_type("expr", "Expr*");
    EXPECT_TRUE(types.has_type("expr"));
    EXPECT_EQ(types.get_type("expr"), "Expr*");
    EXPECT_FALSE(types.is_base_type("expr"));
}

TEST(TypeRegistry, ThrowsOnUnknownType) {
    TypeRegistry types;
    EXPECT_THROW(types.get_type("nonexistent"), TemplateException);
}

// --- Parser tests ---

TEST(Generator, ParsesExampleASDL) {
    Generator gen;
    auto* ast = gen.parse(source_path("examples/ast.asdl"));
    ASSERT_NE(ast, nullptr);
    EXPECT_EQ(ast->name, "ast");
    EXPECT_FALSE(ast->definitions.empty());
}

TEST(Generator, ParsesOwnASDL) {
    Generator gen;
    auto* ast = gen.parse(source_path("grammar/asdl.asdl"));
    ASSERT_NE(ast, nullptr);
    EXPECT_EQ(ast->name, "asdl_ast");
    EXPECT_EQ(ast->definitions.size(), 7u);  // flag, mod, alias, definition, type_body, constructor, field
}

TEST(Generator, ToJsonProducesValidStructure) {
    Generator gen;
    auto* ast = gen.parse(source_path("examples/ast.asdl"));
    auto module = gen.to_json(ast);
    EXPECT_TRUE(module.contains("name"));
    EXPECT_TRUE(module.contains("definitions"));
    EXPECT_TRUE(module["definitions"].is_array());
}

// --- End-to-end tests ---

TEST(Generator, EndToEndGeneration) {
    Generator gen;
    auto* ast = gen.parse(source_path("examples/ast.asdl"));
    auto module = gen.to_json(ast);
    gen.process(module);

    std::stringstream output;
    gen.generate(module, source_path("templates/cpp.inja"), output);

    std::string result = output.str();
    EXPECT_NE(result.find("struct"), std::string::npos);
    EXPECT_NE(result.find("ASTNode"), std::string::npos);
}
