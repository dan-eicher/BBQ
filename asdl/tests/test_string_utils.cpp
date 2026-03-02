#include <gtest/gtest.h>
#include "string_utils.h"

TEST(CamelCase, ConvertsSnakeCaseToCamelCase) {
    EXPECT_EQ(CamelCase("foo_bar"), "FooBar");
    EXPECT_EQ(CamelCase("hello_world"), "HelloWorld");
    EXPECT_EQ(CamelCase("one_two_three"), "OneTwoThree");
}

TEST(CamelCase, CapitalizesFirstLetter) {
    EXPECT_EQ(CamelCase("simple"), "Simple");
    EXPECT_EQ(CamelCase("word"), "Word");
}

TEST(CamelCase, EmptyString) {
    EXPECT_EQ(CamelCase(""), "");
}

TEST(CamelCase, SingleCharacter) {
    EXPECT_EQ(CamelCase("a"), "A");
}

TEST(CamelCase, AlreadyCapitalized) {
    EXPECT_EQ(CamelCase("Already"), "Already");
}

TEST(SnakeCase, ConvertsCamelCaseToSnakeCase) {
    EXPECT_EQ(SnakeCase("FooBar"), "foo_bar");
    EXPECT_EQ(SnakeCase("HelloWorld"), "hello_world");
}

TEST(SnakeCase, SimpleLowercaseWords) {
    EXPECT_EQ(SnakeCase("simple"), "simple");
}

TEST(SnakeCase, EmptyString) {
    EXPECT_EQ(SnakeCase(""), "");
}

TEST(SnakeCase, SingleCharacter) {
    EXPECT_EQ(SnakeCase("A"), "a");
    EXPECT_EQ(SnakeCase("a"), "a");
}

TEST(SnakeCase, Numbers) {
    EXPECT_EQ(SnakeCase("Value1"), "value1");
}

TEST(SafeName, EscapesCppKeywords) {
    EXPECT_EQ(get_safe_name("class"), "class_");
    EXPECT_EQ(get_safe_name("return"), "return_");
    EXPECT_EQ(get_safe_name("int"), "int_");
    EXPECT_EQ(get_safe_name("void"), "void_");
    EXPECT_EQ(get_safe_name("if"), "if_");
    EXPECT_EQ(get_safe_name("else"), "else_");
}

TEST(SafeName, PassesThroughNonKeywords) {
    EXPECT_EQ(get_safe_name("name"), "name");
    EXPECT_EQ(get_safe_name("value"), "value");
    EXPECT_EQ(get_safe_name("foo"), "foo");
}
