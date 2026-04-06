#pragma once

#include <string>
#include <ostream>
#include <sstream>
#include <set>

#include "GrammarAST.h"
#include "PegAnalysis.h"

namespace pegc {

class CParserEmitter {
public:
    CParserEmitter(const AnalysisResult& analysis, const std::string& prefix = "");

    // Emit header (declarations) to frame
    bool emit_header_to_frame(PegGrammar::Grammar* grammar,
                              const std::string& frame_path,
                              std::ostream& out);

    // Emit implementation to frame
    bool emit_impl_to_frame(PegGrammar::Grammar* grammar,
                            const std::string& frame_path,
                            const std::string& header_include,
                            std::ostream& out);

private:
    // Header emission
    void emit_headers(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_public_decls(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_parse_method_decls(PegGrammar::Grammar* grammar, std::ostream& out);

    // Implementation emission
    void emit_skip_setup(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_parse_method_impls(PegGrammar::Grammar* grammar, std::ostream& out);

    // Per-production
    void emit_production_decl(PegGrammar::Production* prod, std::ostream& out);
    void emit_production_impl(PegGrammar::Production* prod, std::ostream& out);

    // PEG expression code generation (DDCG → C)
    void emit_expr(PegGrammar::PegExpr* expr, std::ostream& out, int indent);
    void emit_sequence(PegGrammar::Sequence* seq, std::ostream& out, int indent);
    void emit_choice(PegGrammar::Choice* ch, std::ostream& out, int indent);
    void emit_star(PegGrammar::Star* star, std::ostream& out, int indent);
    void emit_plus(PegGrammar::Plus* plus, std::ostream& out, int indent);
    void emit_optional(PegGrammar::Optional* opt, std::ostream& out, int indent);
    void emit_and(PegGrammar::And* a, std::ostream& out, int indent);
    void emit_not(PegGrammar::Not* n, std::ostream& out, int indent);
    void emit_literal(PegGrammar::Literal* lit, std::ostream& out, int indent);
    void emit_rule_call(PegGrammar::RuleCall* rc, std::ostream& out, int indent);
    void emit_action(PegGrammar::Action* act, std::ostream& out, int indent);
    void emit_resolver(PegGrammar::Resolver* res, std::ostream& out, int indent);

    // Helpers
    std::string indent_str(int indent);
    std::string escape_string(const std::string& s);
    bool is_keyword_literal(PegGrammar::Literal* lit);
    void emit_charset_predicate(PegGrammar::CharsetExpr* expr, std::ostream& out);
    int next_var_id();

    const AnalysisResult& analysis_;
    std::string prefix_;
    int var_counter_ = 0;
    std::set<std::string> production_names_;

    // Tracks failure action: "return false" at production level,
    // "break" inside do { } while(0) backtracking blocks.
    std::string fail_ = "return false";
};

} // namespace pegc
