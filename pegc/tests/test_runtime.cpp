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

// ═══════════════════════════════════════════════════════════════
// Span and unescape helpers (token payload decoding)
// ═══════════════════════════════════════════════════════════════

static Span span_of(const char* s) {
    return Span{s, static_cast<int>(strlen(s))};
}

TEST(PegRuntime, SpanToString) {
    EXPECT_EQ(span_of("hello").to_string(), std::string("hello"));
    Span partial{"abc", 2};
    EXPECT_EQ(partial.to_string(), std::string("ab"));
    EXPECT_EQ(Span{}.to_string(), std::string(""));
}

TEST(PegRuntime, UnescapeStringLitStripsQuotes) {
    EXPECT_EQ(unescape_string_lit(span_of("\"hello\"")), std::string("hello"));
    EXPECT_EQ(unescape_string_lit(span_of("\"\"")), std::string(""));
}

TEST(PegRuntime, UnescapeStringLitWithoutQuotes) {
    // Caller already stripped quotes — still works.
    EXPECT_EQ(unescape_string_lit(span_of("hello")), std::string("hello"));
}

TEST(PegRuntime, UnescapeStringLitEscapes) {
    EXPECT_EQ(unescape_string_lit(span_of("\"a\\nb\"")), std::string("a\nb"));
    EXPECT_EQ(unescape_string_lit(span_of("\"a\\tb\"")), std::string("a\tb"));
    EXPECT_EQ(unescape_string_lit(span_of("\"a\\\\b\"")), std::string("a\\b"));
    EXPECT_EQ(unescape_string_lit(span_of("\"a\\\"b\"")), std::string("a\"b"));
}

TEST(PegRuntime, UnescapeCharLitStripsQuotes) {
    EXPECT_EQ(unescape_char_lit(span_of("'a'")), 'a');
    EXPECT_EQ(unescape_char_lit(span_of("'Z'")), 'Z');
}

TEST(PegRuntime, UnescapeCharLitEscapes) {
    EXPECT_EQ(unescape_char_lit(span_of("'\\n'")), '\n');
    EXPECT_EQ(unescape_char_lit(span_of("'\\t'")), '\t');
    EXPECT_EQ(unescape_char_lit(span_of("'\\\\'")), '\\');
    EXPECT_EQ(unescape_char_lit(span_of("'\\''")), '\'');
}

TEST(PegRuntime, UnescapeCharLitWithoutQuotes) {
    EXPECT_EQ(unescape_char_lit(span_of("a")), 'a');
    EXPECT_EQ(unescape_char_lit(span_of("\\n")), '\n');
}
