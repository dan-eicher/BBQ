#pragma once

#include <string>
#include <ostream>

#include "GrammarAST.h"
#include "PegAnalysis.h"
#include "java_emit.h"

namespace pegc {

// Java sibling of CParserEmitter. Java has no header/implementation split, so
// this emitter has one entry point and pegc writes one `.java` file from one
// frame (JavaParser.frame) rather than the C/C++ pair.
//
// Everything the C backend puts at file scope becomes a member of the parser
// class: charset predicates are private static methods, tokens and productions
// are private instance methods, and the cursor lives in a field named `p` so
// generated bodies never thread it through a parameter.
//
// The generated parser depends on two runtime classes, PegCursor and Span —
// the backend's runtime contract, as peg_state and peg_span are the C
// backend's. The consumer supplies them and says where they live; pegc names
// no package of its own.
class JavaParserEmitter {
public:
    // `package` is the generated parser's own package; `runtime_package` is
    // where the consumer keeps PegCursor and Span. Both default to empty: the
    // default package, and no import.
    JavaParserEmitter(const AnalysisResult& analysis,
                      const std::string& class_name,
                      const std::string& package = "",
                      const std::string& runtime_package = "");

    bool emit_to_frame(PegGrammar::Grammar* grammar,
                       const std::string& frame_path,
                       std::ostream& out);

    // Emit to a string (for testing)
    std::string emit(PegGrammar::Grammar* grammar);

private:
    void emit_headers(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_charset_defs(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_skip_setup(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_entry_points(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_token_impls(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_production_impls(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_production_impl(PegGrammar::Production* prod, std::ostream& out);
    void emit_token_impl(PegGrammar::TokenDef* tok, std::ostream& out);

    const AnalysisResult& analysis_;
    std::string class_name_;
    std::string package_;
    std::string runtime_package_;
    java_emit::Ctx ctx_;
};

} // namespace pegc
