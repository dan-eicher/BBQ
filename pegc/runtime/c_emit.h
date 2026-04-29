// C-output sibling of cpp_emit.h. Same per-PEG-expression cg_* names,
// different bodies (do{}while(0) instead of lambdas, peg_* C runtime
// API, fail-action tracking via ctx->fail). Phase 5/6 will replace
// CParserEmitter's hand-written switch dispatch with ddcgc-generated
// code that calls these `cg_*` functions.
//
// Until ddcgc lands the structural orchestrator (CParserEmitter)
// keeps calling these via c_emit::cg_* — the diff at phase 1 is
// purely move + ctx-threading; no behavior changes, no regen.
#pragma once

#include <string>
#include <ostream>
#include <set>

#include "GrammarAST.h"

namespace pegc {
namespace c_emit {

// Per-emission ctx. Mirrors cpp_emit::Ctx with the C-specific extras:
// `prefix` for symbol mangling and `fail` carrying the active failure
// action ("return false" at production level, "break" inside a
// do{}while(0) backtrack block). cg_* functions that introduce a
// backtrack block save/restore `fail` around recursive cg_expr calls.
struct Ctx {
    int var_counter = 0;
    std::set<std::string> production_names;
    std::set<std::string> token_names;
    std::set<std::string> charset_names;
    bool in_lex_mode = false;
    std::string prefix;
    std::string fail = "return false";
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
std::string to_snake_case(const std::string& name);

} // namespace c_emit
} // namespace pegc
