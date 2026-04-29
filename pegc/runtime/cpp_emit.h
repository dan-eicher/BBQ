// Phase-1 refactor of pegc/backend/ParserEmitter.cpp.
//
// The per-PEG-expression emit_* methods that used to live as private
// members of ParserEmitter have moved here as free functions. Names
// are chosen to match the eventual ddcgc DDCG-auxiliary signatures:
// phase 5 will replace ParserEmitter's hand-written switch dispatch
// with ddcgc-generated code that calls these same `cg_*` functions.
//
// Until ddcgc lands the structural orchestrator (ParserEmitter)
// keeps calling these via cpp_emit::cg_* — the diff at phase 1 is
// purely move + ctx-threading; no behavior changes, no regen.
#pragma once

#include <string>
#include <ostream>
#include <set>

#include "GrammarAST.h"

namespace pegc {
namespace cpp_emit {

// Per-emission ctx, threaded as the first arg of every cg_* function.
// Holds the state the per-PEG-expression emitters mutate (var_counter
// for gensym, in_lex_mode for token vs production emission) plus the
// grammar-level name sets used for dispatch decisions.
struct Ctx {
    int var_counter = 0;
    std::set<std::string> production_names;
    std::set<std::string> token_names;
    std::set<std::string> charset_names;
    bool in_lex_mode = false;
};

// ── Per-PEG-expression code generation ─────────────────────
void cg_expr     (Ctx* ctx, PegGrammar::PegExpr*  expr, std::ostream& out, int indent);
void cg_seq      (Ctx* ctx, PegGrammar::Sequence* seq,  std::ostream& out, int indent);
void cg_choice   (Ctx* ctx, PegGrammar::Choice*   ch,   std::ostream& out, int indent);
void cg_star     (Ctx* ctx, PegGrammar::Star*     star, std::ostream& out, int indent);
void cg_plus     (Ctx* ctx, PegGrammar::Plus*     plus, std::ostream& out, int indent);
void cg_optional (Ctx* ctx, PegGrammar::Optional* opt,  std::ostream& out, int indent);
void cg_and      (Ctx* ctx, PegGrammar::And*      a,    std::ostream& out, int indent);
void cg_not      (Ctx* ctx, PegGrammar::Not*      n,    std::ostream& out, int indent);
void cg_literal  (Ctx* ctx, PegGrammar::Literal*  lit,  std::ostream& out, int indent);
void cg_rule_call(Ctx* ctx, PegGrammar::RuleCall* rc,   std::ostream& out, int indent);
void cg_action   (Ctx* ctx, PegGrammar::Action*   act,  std::ostream& out, int indent);
void cg_resolver (Ctx* ctx, PegGrammar::Resolver* res,  std::ostream& out, int indent);

// ── Charset predicate (lex-level test against a charset expression) ──
void cg_charset_predicate(PegGrammar::CharsetExpr* expr, std::ostream& out);

// ── Utility ────────────────────────────────────────────────
std::string indent_str(int indent);
std::string escape_string(const std::string& s);
bool        is_keyword_literal(PegGrammar::Literal* lit);
int         next_var_id(Ctx* ctx);

} // namespace cpp_emit
} // namespace pegc
