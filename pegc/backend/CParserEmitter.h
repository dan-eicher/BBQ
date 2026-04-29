#pragma once

#include <string>
#include <ostream>

#include "GrammarAST.h"
#include "PegAnalysis.h"
#include "c_emit.h"

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
    void emit_charset_defs(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_skip_setup(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_parse_method_impls(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_token_method_impls(PegGrammar::Grammar* grammar, std::ostream& out);
    void emit_token_forward_decls(PegGrammar::Grammar* grammar, std::ostream& out);

    // Per-production / per-token (orchestrators that reset gensym + lex mode
    // and dispatch into the cg_* free functions for the body)
    void emit_production_decl(PegGrammar::Production* prod, std::ostream& out);
    void emit_production_impl(PegGrammar::Production* prod, std::ostream& out);
    void emit_token_forward_decl(PegGrammar::TokenDef* tok, std::ostream& out);
    void emit_token_impl(PegGrammar::TokenDef* tok, std::ostream& out);

    // Per-PEG-expression code generation lives in pegc/runtime/c_emit.{h,cpp}
    // as `pegc::c_emit::cg_*` free functions, threaded through `ctx_`.
    // Phase 6 will replace this class's switch dispatch with ddcgc-generated
    // code that calls the same `cg_*` functions.

    const AnalysisResult& analysis_;
    c_emit::Ctx ctx_;
};

} // namespace pegc
