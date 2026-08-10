// Pins for the Java backend (-lang java, JavaParserEmitter, java_emit).
//
// The generated parser has to compile under a Java 1.0 compiler, so these pins
// are as much about what is NOT emitted (goto, generics, nested classes, blank
// finals) as about what is. The behavioural pins — that ordered choice really
// backtracks, that a not-predicate really consumes nothing — live in javelina,
// where a generated parser can be compiled and run.
#include <gtest/gtest.h>
#include <string>
#include <cstring>
#include <set>

#include "Parser.h"
#include "PegAnalysis.h"
#include "JavaParserEmitter.h"

using namespace PegGrammar;
using namespace pegc;

static Grammar* parse(const char* src) {
    Parser parser;
    parser.init(src, static_cast<int>(strlen(src)));
    if (!parser.parse()) return nullptr;
    return parser.ast;
}

static std::string generate(const char* src, const char* package = "",
                            const char* runtime = "") {
    auto* g = parse(src);
    if (!g) return "";

    PegAnalysis analysis;
    auto result = analysis.analyze(g);
    if (!result.ok()) return "";

    JavaParserEmitter emitter(result, "TestParser", package, runtime);
    return emitter.emit(g);
}

static bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

static int count(const std::string& s, const std::string& sub) {
    int n = 0;
    size_t pos = 0;
    while ((pos = s.find(sub, pos)) != std::string::npos) { n++; pos += sub.size(); }
    return n;
}

// A grammar with one of everything the emitter has a case for.
static const char* kGrammar = R"PEG(
COMPILER Test

CHARACTERS
  letter = 'a'..'z' .
  digit  = '0'..'9' .

TOKENS
  ident      = letter { letter | digit } .
  string_lit = '"' { !'"' ANY } '"' .

IGNORE ' ' + '\t' + '\n'

PRODUCTIONS

Start = "begin" { Item } "end" .

Item  = ident | string_lit | "(" Start ")" .

END Test.
)PEG";

// ── Ordered choice ─────────────────────────────────────────

// PEG choice is ordered: the first alternative that matches wins, and a failing
// alternative must leave the cursor exactly where it started. Both halves have
// to be in the emitted text — a save with no restore is a silent miscompile.
TEST(JavaCodegen, OrderedChoiceSavesAndRestores) {
    std::string out = generate(kGrammar);
    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(has(out, "p.save()"));
    EXPECT_TRUE(has(out, "p.restore("));
}

// Java has no goto. Committing to a successful alternative is a forward jump
// out of the choice block, which is a labeled break.
TEST(JavaCodegen, ChoiceCommitsWithLabeledBreakNotGoto) {
    std::string out = generate(kGrammar);
    ASSERT_FALSE(out.empty());
    EXPECT_FALSE(has(out, "goto "));
    EXPECT_TRUE(has(out, "_choice_done"));
    EXPECT_TRUE(has(out, "break _choice_done"));

    // The label set and the broken-to set are equal. A choice with k
    // alternatives emits k-1 breaks to one label, so counts do not match —
    // but a break to an undeclared label does not compile, and a declared
    // label nobody breaks to is a choice that never commits.
    std::set<std::string> declared, targeted;
    for (size_t at = 0; (at = out.find("_choice_done", at)) != std::string::npos; ) {
        size_t end = out.find_first_not_of("0123456789", at + 12);
        std::string label = out.substr(at, end - at);
        bool is_break = at >= 6 && out.compare(at - 6, 6, "break ") == 0;
        (is_break ? targeted : declared).insert(label);
        at = end;
    }
    EXPECT_FALSE(declared.empty());
    EXPECT_EQ(declared, targeted);
}

// The C emitter's do{...}while(0) carries the failure action through `break`.
// Java spells the same construct do{...}while(false).
TEST(JavaCodegen, BacktrackBlockIsDoWhileFalse) {
    std::string out = generate(kGrammar);
    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(has(out, "} while (false);"));
    EXPECT_FALSE(has(out, "while(0)"));
}

// ── Predicates ─────────────────────────────────────────────

// A not-predicate restores unconditionally and then fails if the body matched:
// it never consumes, on either outcome.
TEST(JavaCodegen, NotPredicateRestoresOnBothOutcomes) {
    std::string out = generate(kGrammar);
    ASSERT_FALSE(out.empty());
    size_t at = out.find("if (_ok");
    ASSERT_NE(at, std::string::npos);
    // The restore precedes the success test — that ordering is the predicate.
    size_t restore = out.rfind("p.restore(", at);
    ASSERT_NE(restore, std::string::npos);
    EXPECT_LT(restore, at);
}

// ── Java 1.0 shape ─────────────────────────────────────────

TEST(JavaCodegen, NoConstructsPastJava10) {
    std::string out = generate(kGrammar);
    ASSERT_FALSE(out.empty());
    EXPECT_FALSE(has(out, "->"));          // lambdas (1.8)
    EXPECT_FALSE(has(out, "@Override"));   // annotations (1.5)
    EXPECT_FALSE(has(out, "java.util."));  // collections (1.2)
    EXPECT_FALSE(has(out, "StringBuilder"));

    // Nested classes are 1.1: exactly one class declaration, at column 0.
    EXPECT_EQ(count(out, "class "), 1);
    size_t at = out.find("public class TestParser {");
    ASSERT_NE(at, std::string::npos);
    EXPECT_TRUE(at == 0 || out[at - 1] == '\n');
}

// JLS 1.0 section 8.3.1.2: a final field's declarator must carry its
// initializer. The cursor field is assigned in the constructor, so it is not
// final — the same rule that shapes the ASDL Java template.
TEST(JavaCodegen, CursorFieldIsNotABlankFinal) {
    std::string out = generate(kGrammar);
    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(has(out, "private PegCursor p;"));
    EXPECT_FALSE(has(out, "private final PegCursor p;"));
}

// Java 1.0 has no function values, so the C runtime's ws_fn pointer becomes the
// tabulated form the C runtime also keeps: a 256-entry table, filled at setup.
TEST(JavaCodegen, WhitespaceIsTabulatedNotAFunctionPointer) {
    std::string out = generate(kGrammar);
    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(has(out, "private static boolean ws_predicate(char c)"));
    EXPECT_TRUE(has(out, "p.setWhitespace((char) i, ws_predicate((char) i));"));
}

// ── Structure ──────────────────────────────────────────────

TEST(JavaCodegen, CharsetsAreStaticPredicates) {
    std::string out = generate(kGrammar);
    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(has(out, "private static boolean is_letter(char c)"));
    EXPECT_TRUE(has(out, "private static boolean is_digit(char c)"));
}

TEST(JavaCodegen, ProductionsAndTokensAreInstanceMethods) {
    std::string out = generate(kGrammar);
    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(has(out, "private boolean parse_start()"));
    EXPECT_TRUE(has(out, "private boolean parse_item()"));
    EXPECT_TRUE(has(out, "private boolean ident(Span out)"));
    EXPECT_TRUE(has(out, "private boolean string_lit(Span out)"));
    // No cursor parameter: the cursor is a field.
    EXPECT_FALSE(has(out, "PegCursor p)"));
}

TEST(JavaCodegen, PackageDeclarationWhenAsked) {
    EXPECT_TRUE(has(generate(kGrammar, "com.example.parse"), "package com.example.parse;"));
    EXPECT_FALSE(has(generate(kGrammar), "package "));
}

// pegc names no runtime package of its own. PegCursor and Span are the
// backend's runtime contract — the consumer implements them and says where
// they live, exactly as a C consumer supplies peg_state.
TEST(JavaCodegen, RuntimePackageIsTheConsumersChoice) {
    std::string named = generate(kGrammar, "com.example.parse", "com.example.peg");
    EXPECT_TRUE(has(named, "import com.example.peg.PegCursor;"));
    EXPECT_TRUE(has(named, "import com.example.peg.Span;"));

    // No runtime package named: no import, and no invented default.
    std::string bare = generate(kGrammar, "com.example.parse");
    EXPECT_FALSE(has(bare, "import "));

    // Runtime shares the parser's package: importing from your own package is
    // noise, so it is not emitted.
    std::string same = generate(kGrammar, "com.example.peg", "com.example.peg");
    EXPECT_FALSE(has(same, "import "));
}

// No consumer's name appears anywhere in generated output. pegc is a generic
// tool; javelina is one of its consumers, not a dependency.
TEST(JavaCodegen, GeneratorNamesNoConsumer) {
    EXPECT_FALSE(has(generate(kGrammar, "com.example.parse", "com.example.peg"),
                     "javelina"));
}

// pegc is self-hosting and its output is byte-stable; the Java backend has to
// be too, or a regeneration step produces spurious diffs.
TEST(JavaCodegen, GenerationIsAFixpoint) {
    EXPECT_EQ(generate(kGrammar, "com.example.parse", "com.example.peg"),
              generate(kGrammar, "com.example.parse", "com.example.peg"));
}
