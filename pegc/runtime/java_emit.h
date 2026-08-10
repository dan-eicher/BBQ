// Java-output sibling of c_emit.h / cpp_emit.h. Same per-PEG-expression cg_*
// names, different bodies.
//
// The target is the Java 1.0 language (JLS 1st edition), which shapes three
// things:
//
//   - There is no `goto`, so the C emitter's `goto _choice_doneN` becomes a
//     labeled block plus `break _choice_doneN` — the one Java construct that
//     jumps forward out of a statement.
//   - `do { ... } while (false)` with `break` is legal Java and carries the
//     failure action exactly as it does in C, so ctx.fail is either
//     "return false" (production level) or "break" (inside a backtrack block).
//   - There are no out-parameters. A production's declared params are emitted
//     verbatim and `is_ref` is ignored: a grammar targeting Java returns values
//     through an object or a one-element array, not through a reference.
//
// The generated parser holds a PegCursor in a field named `p`, so every cursor
// call reads `p.method(...)` and parse methods take no cursor parameter — the
// C signature's leading `peg_state* p` has no Java analogue. PegCursor and Span
// are the runtime contract a consumer implements; which package they live in is
// the consumer's choice (see JavaParserEmitter's runtime_package).
#pragma once

#include <string>
#include <ostream>
#include <set>

#include "GrammarAST.h"

namespace pegc {
namespace java_emit {

// Per-emission ctx. Mirrors c_emit::Ctx; `prefix` is unused for symbol
// mangling (Java scopes by class) but kept so the three emitters share a shape.
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

} // namespace java_emit
} // namespace pegc
