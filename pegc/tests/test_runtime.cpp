#include <gtest/gtest.h>
#include "PegRuntime.h"

using namespace peg;

static PegCursor make_cursor(const char* s) {
    PegCursor c;
    c.init(s, static_cast<int>(strlen(s)));
    return c;
}

// ═══════════════════════════════════════════════════════════════
// Basic Matching
// ═══════════════════════════════════════════════════════════════

TEST(PegRuntime, MatchChar) {
    auto c = make_cursor("abc");
    EXPECT_TRUE(c.match('a'));
    EXPECT_TRUE(c.match('b'));
    EXPECT_TRUE(c.match('c'));
    EXPECT_TRUE(c.at_end());
}

TEST(PegRuntime, MatchString) {
    auto c = make_cursor("hello world");
    EXPECT_TRUE(c.match("hello"));
    EXPECT_TRUE(c.match(' '));
    EXPECT_TRUE(c.match("world"));
    EXPECT_TRUE(c.at_end());
}

TEST(PegRuntime, MatchFails) {
    auto c = make_cursor("abc");
    EXPECT_FALSE(c.match('x'));
    EXPECT_FALSE(c.match("xyz"));
    EXPECT_EQ(c.peek(), 'a');  // position unchanged
}

// ═══════════════════════════════════════════════════════════════
// Save / Restore
// ═══════════════════════════════════════════════════════════════

TEST(PegRuntime, SaveRestore) {
    auto c = make_cursor("abcdef");
    auto m = c.save();
    EXPECT_TRUE(c.match("abc"));
    EXPECT_EQ(c.peek(), 'd');
    c.restore(m);
    EXPECT_EQ(c.peek(), 'a');
    EXPECT_TRUE(c.match("abcdef"));
}

// ═══════════════════════════════════════════════════════════════
// Keywords (context-sensitive)
// ═══════════════════════════════════════════════════════════════

TEST(PegRuntime, KeywordMatch) {
    auto c = make_cursor("big endian");
    EXPECT_TRUE(c.keyword("big"));
    EXPECT_TRUE(c.match(' '));
    EXPECT_TRUE(c.keyword("endian"));
}

TEST(PegRuntime, KeywordNotPrefix) {
    // "bigger" should NOT match keyword "big"
    auto c = make_cursor("bigger");
    EXPECT_FALSE(c.keyword("big"));
    EXPECT_EQ(c.peek(), 'b');  // position unchanged
}

TEST(PegRuntime, PeekKeyword) {
    auto c = make_cursor("struct Foo");
    EXPECT_TRUE(c.peek_keyword("struct"));
    EXPECT_FALSE(c.peek_keyword("str"));
    // Position unchanged after peek
    EXPECT_EQ(c.peek(), 's');
}

// ═══════════════════════════════════════════════════════════════
// Built-in Token Patterns
// ═══════════════════════════════════════════════════════════════

TEST(PegRuntime, Ident) {
    auto c = make_cursor("foo_bar 123");
    std::string id;
    EXPECT_TRUE(c.ident(id));
    EXPECT_EQ(id, "foo_bar");
    EXPECT_EQ(c.peek(), ' ');
}

TEST(PegRuntime, Integer) {
    auto c = make_cursor("42");
    int64_t v;
    EXPECT_TRUE(c.integer(v));
    EXPECT_EQ(v, 42);
}

TEST(PegRuntime, IntegerHex) {
    auto c = make_cursor("0xFF");
    int64_t v;
    EXPECT_TRUE(c.integer(v));
    EXPECT_EQ(v, 255);
}

TEST(PegRuntime, IntegerBinary) {
    auto c = make_cursor("0b1010");
    int64_t v;
    EXPECT_TRUE(c.integer(v));
    EXPECT_EQ(v, 10);
}

TEST(PegRuntime, StringLit) {
    auto c = make_cursor("\"hello\\nworld\"");
    std::string s;
    EXPECT_TRUE(c.string_lit(s));
    EXPECT_EQ(s, "hello\nworld");
}

// ═══════════════════════════════════════════════════════════════
// SPAN Optimization
// ═══════════════════════════════════════════════════════════════

TEST(PegRuntime, Span) {
    auto c = make_cursor("aaabbb");
    c.span([](char ch) { return ch == 'a'; });
    EXPECT_EQ(c.peek(), 'b');
    c.span([](char ch) { return ch == 'b'; });
    EXPECT_TRUE(c.at_end());
}

// ═══════════════════════════════════════════════════════════════
// Whitespace + Comment Skipping
// ═══════════════════════════════════════════════════════════════

TEST(PegRuntime, SkipWhitespace) {
    auto c = make_cursor("  \t\n  hello");
    c.set_whitespace([](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    });
    c.skip();
    EXPECT_TRUE(c.match("hello"));
}

TEST(PegRuntime, SkipLineComment) {
    auto c = make_cursor("// comment\nhello");
    c.set_whitespace([](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    });
    c.set_comments({CommentSpec{"//", "\n", false}});
    c.skip();
    EXPECT_TRUE(c.match("hello"));
}

TEST(PegRuntime, SkipBlockComment) {
    auto c = make_cursor("/* block */ hello");
    c.set_whitespace([](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    });
    c.set_comments({CommentSpec{"/*", "*/", false}});
    c.skip();
    EXPECT_TRUE(c.match("hello"));
}

// ═══════════════════════════════════════════════════════════════
// Line / Column Tracking
// ═══════════════════════════════════════════════════════════════

TEST(PegRuntime, LineColumnTracking) {
    auto c = make_cursor("ab\ncd\nef");
    EXPECT_EQ(c.line(), 1);
    EXPECT_EQ(c.col(), 1);
    c.match('a'); c.match('b'); c.match('\n');
    EXPECT_EQ(c.line(), 2);
    EXPECT_EQ(c.col(), 1);
    c.match('c'); c.match('d'); c.match('\n');
    EXPECT_EQ(c.line(), 3);
    EXPECT_EQ(c.col(), 1);
}

// ═══════════════════════════════════════════════════════════════
// Error Reporting
// ═══════════════════════════════════════════════════════════════

TEST(PegRuntime, ErrorReporting) {
    auto c = make_cursor("abc");
    c.match("ab");
    c.expected("'d'");
    EXPECT_TRUE(c.has_errors());
    EXPECT_EQ(c.errors().size(), 1u);
    EXPECT_EQ(c.errors()[0].line, 1);
    EXPECT_EQ(c.errors()[0].col, 3);
}

// ═══════════════════════════════════════════════════════════════
// PEG Backtracking Pattern: Choice
// ═══════════════════════════════════════════════════════════════

TEST(PegRuntime, PegChoice) {
    // Simulate: "abc" / "abd"
    auto c = make_cursor("abd");

    // Try first alternative
    auto m = c.save();
    bool ok = c.match("abc");
    if (!ok) {
        c.restore(m);
        ok = c.match("abd");
    }
    EXPECT_TRUE(ok);
    EXPECT_TRUE(c.at_end());
}

TEST(PegRuntime, PegStar) {
    // Simulate: { "a" }
    auto c = make_cursor("aaab");
    int count = 0;
    for (;;) {
        auto m = c.save();
        if (!c.match('a')) {
            c.restore(m);
            break;
        }
        count++;
    }
    EXPECT_EQ(count, 3);
    EXPECT_EQ(c.peek(), 'b');
}
