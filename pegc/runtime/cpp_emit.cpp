#include "cpp_emit.h"

using namespace PegGrammar;

namespace pegc {
namespace cpp_emit {

// ── Helpers to extract first literal from an alternative ─────

static PegExpr* first_element(PegExpr* alt) {
    if (auto* s = dynamic_cast<Sequence*>(alt))
        if (!s->elements.empty()) return s->elements[0];
    return alt;
}

static Literal* first_literal(PegExpr* alt) {
    return dynamic_cast<Literal*>(first_element(alt));
}

// ═══════════════════════════════════════════════════════════════
// PEG Expression Code Generation (DDCG → C++)
//
// Key invariant: cg_expr emits code where "return false" means
// the expression failed. At production level, this exits the method.
// Inside a lambda (star/choice/optional/and/not), it returns from
// the lambda, allowing the enclosing operator to handle recovery.
// ═══════════════════════════════════════════════════════════════

void cg_expr(Ctx* ctx, PegExpr* expr, std::ostream& out, int indent) {
    if (auto* seq = dynamic_cast<Sequence*>(expr)) {
        cg_seq(ctx, seq, out, indent);
    } else if (auto* ch = dynamic_cast<Choice*>(expr)) {
        cg_choice(ctx, ch, out, indent);
    } else if (auto* star = dynamic_cast<Star*>(expr)) {
        cg_star(ctx, star, out, indent);
    } else if (auto* plus = dynamic_cast<Plus*>(expr)) {
        cg_plus(ctx, plus, out, indent);
    } else if (auto* opt = dynamic_cast<Optional*>(expr)) {
        cg_optional(ctx, opt, out, indent);
    } else if (auto* a = dynamic_cast<And*>(expr)) {
        cg_and(ctx, a, out, indent);
    } else if (auto* n = dynamic_cast<Not*>(expr)) {
        cg_not(ctx, n, out, indent);
    } else if (auto* lit = dynamic_cast<Literal*>(expr)) {
        cg_literal(ctx, lit, out, indent);
    } else if (auto* rc = dynamic_cast<RuleCall*>(expr)) {
        cg_rule_call(ctx, rc, out, indent);
    } else if (auto* act = dynamic_cast<Action*>(expr)) {
        cg_action(ctx, act, out, indent);
    } else if (auto* res = dynamic_cast<Resolver*>(expr)) {
        cg_resolver(ctx, res, out, indent);
    } else if (auto* grp = dynamic_cast<Group*>(expr)) {
        cg_expr(ctx, grp->body, out, indent);
    } else if (dynamic_cast<AnyExpr*>(expr)) {
        out << indent_str(indent) << "if (at_end()) return false;\n";
        out << indent_str(indent) << "advance();\n";
    } else if (auto* cr = dynamic_cast<CharRange*>(expr)) {
        out << indent_str(indent) << "if (at_end() || peek() < " << cr->from
            << " || peek() > " << cr->to << ") return false;\n";
        out << indent_str(indent) << "advance();\n";
    } else if (auto* csr = dynamic_cast<CharsetRef*>(expr)) {
        out << indent_str(indent) << "if (at_end() || !is_" << csr->name
            << "(peek())) return false;\n";
        out << indent_str(indent) << "advance();\n";
    }
}

// ── Sequence (DDCG §6): e1 e2 ──────────────────────────────
// CG[e1] effect L1;  L1: CG[e2] effect γ
// → emit each element in order; return false propagates from any
void cg_seq(Ctx* ctx, Sequence* seq, std::ostream& out, int indent) {
    for (auto* elem : seq->elements) {
        cg_expr(ctx, elem, out, indent);
    }
}

// ── Choice (DDCG §7): e1 / e2 / e3 ─────────────────────────
//
// General pattern (OP_CHOICE/OP_COMMIT):
//   For each non-last alternative, wrap in a lambda so that
//   "return false" triggers recovery (restore + try next alt).
//   Last alternative's failures propagate to the enclosing scope.
//
// Head-fail optimization (OP_TEST*/OP_COMMIT):
//   When all alternatives start with a distinct literal, use
//   peek_at/peek_keyword to test without consuming. Once committed,
//   failures in the rest of the alternative propagate (no backtrack).
//
void cg_choice(Ctx* ctx, Choice* ch, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int n = static_cast<int>(ch->alternatives.size());

    if (n == 0) return;
    if (n == 1) {
        cg_expr(ctx, ch->alternatives[0], out, indent);
        return;
    }

    // Check for head-fail optimization: all alternatives start with a literal
    bool all_literal_starts = true;
    for (auto* alt : ch->alternatives) {
        auto* lit = first_literal(alt);
        if (!lit) {
            all_literal_starts = false;
            break;
        }
        // Keyword-shaped literals require ident-continue boundary checks
        // that head-fail's peek_at can't express; fall back to general
        // choice in that case.
        if (!ctx->in_lex_mode && is_keyword_literal(lit)) {
            all_literal_starts = false;
            break;
        }
    }

    // Predictive dispatch is sound ONLY when the alternative heads are pairwise
    // prefix-disjoint: peek_at(L) is true whenever the input starts with L, so if
    // one head is a prefix of another (e.g. two alternatives both starting "(") the
    // peek cannot tell them apart and would wrongly commit to the earlier one — when
    // it then fails deeper, PEG ordered-choice requires falling through to the rest,
    // which predictive dispatch cannot do. Otherwise use the general backtracking choice.
    bool prefix_disjoint = all_literal_starts && n <= 8;
    for (int i = 0; i < n && prefix_disjoint; i++) {
        const std::string& a = first_literal(ch->alternatives[i])->value;
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            const std::string& b = first_literal(ch->alternatives[j])->value;
            if (a.size() <= b.size() && b.compare(0, a.size(), a) == 0) {
                prefix_disjoint = false;
                break;
            }
        }
    }

    if (prefix_disjoint) {
        // Head-fail optimization: peek tests determine which alternative
        // to commit to. No save/restore needed.
        // Skip whitespace before peeking so we test the actual next token.
        if (!ctx->in_lex_mode) out << ind << "skip();\n";
        for (int i = 0; i < n; i++) {
            auto* alt = ch->alternatives[i];
            auto* lit = first_literal(alt);
            bool is_last = (i == n - 1);

            if (!is_last) {
                out << ind << "if (peek_at(\"" << escape_string(lit->value) << "\")) {\n";
                cg_expr(ctx, alt, out, indent + 1);
                out << ind << "} else ";
            } else {
                out << "{\n";
                cg_expr(ctx, alt, out, indent + 1);
                out << ind << "}\n";
            }
        }
        return;
    }

    // General choice: lambda per non-last alternative for backtracking.
    //
    // DDCG: OP_CHOICE L1; CG[e1]; OP_COMMIT L2; L1: CG[e2]; L2:
    // C++:  save; if(try e1) {commit} else {restore; e2}
    //
    // Lambda maps to CHOICE/COMMIT: return false = fail to recovery,
    // return true = commit (discard choice point, continue).
    int mid = next_var_id(ctx);
    out << ind << "{\n";
    out << ind << "    auto _m" << mid << " = save();\n";

    for (int i = 0; i < n; i++) {
        bool is_last = (i == n - 1);

        if (!is_last) {
            out << ind << "    if ([&]() -> bool {\n";
            cg_expr(ctx, ch->alternatives[i], out, indent + 2);
            out << ind << "        return true;\n";
            out << ind << "    }()) {} else {\n";
            out << ind << "    restore(_m" << mid << ");\n";
        } else {
            // Last alternative: failures propagate to enclosing scope
            cg_expr(ctx, ch->alternatives[i], out, indent + 1);
        }
    }

    // Close else blocks
    for (int i = 0; i < n - 1; i++) {
        out << ind << "    }\n";
    }
    out << ind << "}\n";
}

// ── Star (DDCG §8): e* ─────────────────────────────────────
//
// DDCG: OP_CHOICE L2; L1: CG[e]; OP_PARTIAL_COMMIT L1; L2:
// C++:  Loop with lambda body. Lambda return false = body failed,
//       restore and break (OP_CHOICE recovery to L2).
//       Lambda return true = body succeeded, loop back (PARTIAL_COMMIT).
//       Always succeeds (star matches zero or more).
//
void cg_star(Ctx* ctx, Star* star, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);

    // Charset predicate tests don't consume on fail, so the save/restore
    // around them is dead work — collapse to a tight while loop.
    if (auto* csr = dynamic_cast<CharsetRef*>(star->body)) {
        if (ctx->charset_names.count(csr->name)) {
            out << ind << "while (!at_end() && is_" << csr->name
                << "(peek())) advance();\n";
            return;
        }
    }

    int vid = next_var_id(ctx);
    out << ind << "for (;;) {\n";
    out << ind << "    auto _m" << vid << " = save();\n";
    out << ind << "    if (![&]() -> bool {\n";
    cg_expr(ctx, star->body, out, indent + 2);
    out << ind << "        return true;\n";
    out << ind << "    }()) { restore(_m" << vid << "); break; }\n";
    out << ind << "}\n";
}

// ── Plus (DDCG §9): e+ ─────────────────────────────────────
//
// DDCG: CG[e] effect L4; L4: (star loop)
// First occurrence must succeed (failures propagate).
// Then zero-or-more loop.
//
void cg_plus(Ctx* ctx, Plus* plus, std::ostream& out, int indent) {
    // First occurrence: emitted directly, return false = production fails
    cg_expr(ctx, plus->body, out, indent);
    // Then star loop
    auto star_node = Star(plus->body);
    cg_star(ctx, &star_node, out, indent);
}

// ── Optional (DDCG §10): e? ────────────────────────────────
//
// DDCG: OP_CHOICE L1; CG[e]; OP_COMMIT L2; L1: (always succeeds)
// C++:  Lambda body. If fails, restore. Always succeeds overall.
//
void cg_optional(Ctx* ctx, Optional* opt, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id(ctx);
    out << ind << "{\n";
    out << ind << "    auto _m" << vid << " = save();\n";
    out << ind << "    if (![&]() -> bool {\n";
    cg_expr(ctx, opt->body, out, indent + 2);
    out << ind << "        return true;\n";
    out << ind << "    }()) restore(_m" << vid << ");\n";
    out << ind << "}\n";
}

// ── And predicate (DDCG §11): &e ───────────────────────────
//
// DDCG: OP_CHOICE L1; CG[e]; OP_BACK_COMMIT L3;
//       L1: OP_FAIL;  L3: (continue)
// C++:  Lambda body. Restore position regardless. If body
//       failed, &e fails. If body succeeded, &e succeeds
//       (but position is restored — zero-width assertion).
//
void cg_and(Ctx* ctx, And* a, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id(ctx);
    out << ind << "{\n";
    out << ind << "    auto _m" << vid << " = save();\n";
    out << ind << "    bool _ok" << vid << " = [&]() -> bool {\n";
    cg_expr(ctx, a->body, out, indent + 2);
    out << ind << "        return true;\n";
    out << ind << "    }();\n";
    out << ind << "    restore(_m" << vid << ");\n";
    out << ind << "    if (!_ok" << vid << ") return false;\n";
    out << ind << "}\n";
}

// ── Not predicate (DDCG §12): !e ───────────────────────────
//
// DDCG: OP_CHOICE L1; CG[e]; OP_FAILTWICE;
//       L1: (continue — e failed, so !e succeeds)
// C++:  Lambda body. Restore position regardless. If body
//       succeeded, !e fails (FAILTWICE). If body failed,
//       !e succeeds (recovery to L1).
//
void cg_not(Ctx* ctx, Not* n, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id(ctx);
    out << ind << "{\n";
    out << ind << "    auto _m" << vid << " = save();\n";
    out << ind << "    bool _ok" << vid << " = [&]() -> bool {\n";
    cg_expr(ctx, n->body, out, indent + 2);
    out << ind << "        return true;\n";
    out << ind << "    }();\n";
    out << ind << "    restore(_m" << vid << ");\n";
    out << ind << "    if (_ok" << vid << ") return false;\n";
    out << ind << "}\n";
}

// ── Literal (DDCG §2): "foo" ───────────────────────────────
// OP_STRING "foo"  /  OP_TESTSTRING "foo" Lfalse
// → skip(); if(!match("foo")) return false;
void cg_literal(Ctx* ctx, Literal* lit, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    if (!ctx->in_lex_mode) out << ind << "skip();\n";
    out << ind << "if (!match(\"" << escape_string(lit->value) << "\")) return false;\n";
}

// ── RuleCall (DDCG §16): parse_Foo(args) ────────────────────
// OP_CALL A / OP_JUMP A (tail call)
// → skip(); if(!parse_Foo(args)) return false;
//
// Args are now an opaque raw string — whatever the user wrote between
// `<` and `>` (or `<.` and `.>`) gets emitted verbatim into the call.
// pegc no longer parses or validates the contents; nonsense surfaces
// as a C++ compile error instead.
void cg_rule_call(Ctx* ctx, RuleCall* rc, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);

    // Charset reference: predicate-test-and-consume-one-char. Charsets
    // are single-char (lex-level) predicates — never auto-skip, even in
    // production mode. Required so `!ident_continue` and similar
    // boundary checks operate at the current position.
    if (ctx->charset_names.count(rc->name)) {
        out << ind << "if (at_end() || !is_" << rc->name << "(peek())) return false;\n";
        out << ind << "advance();\n";
        return;
    }

    const char* fn_prefix = ctx->production_names.count(rc->name) ? "parse_" : "";

    if (!ctx->in_lex_mode) out << ind << "skip();\n";
    out << ind << "if (!" << fn_prefix << rc->name << "(" << rc->args << ")) return false;\n";
}

// ── Action: (. code .) ──────────────────────────────────────
void cg_action(Ctx* ctx, Action* act, std::ostream& out, int indent) {
    out << indent_str(indent) << act->code << "\n";
}

// ── Resolver: IF(cond) body ─────────────────────────────────
void cg_resolver(Ctx* ctx, Resolver* res, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    out << ind << "if (" << res->code << ") {\n";
    cg_expr(ctx, res->body, out, indent + 1);
    out << ind << "}\n";
}

// ═══════════════════════════════════════════════════════════════
// Charset predicate emission
// ═══════════════════════════════════════════════════════════════

namespace {
void emit_charset_pred(CharsetExpr* expr, std::ostream& out) {
    if (auto* r = dynamic_cast<Range*>(expr)) {
        out << "(c >= " << r->from << " && c <= " << r->to << ")";
    } else if (auto* s = dynamic_cast<Single*>(expr)) {
        out << "(c == " << s->ch << ")";
    } else if (auto* u = dynamic_cast<Union*>(expr)) {
        out << "(";
        emit_charset_pred(u->left, out);
        out << " || ";
        emit_charset_pred(u->right, out);
        out << ")";
    } else if (auto* d = dynamic_cast<Diff*>(expr)) {
        out << "(";
        emit_charset_pred(d->base, out);
        out << " && !(";
        emit_charset_pred(d->minus, out);
        out << "))";
    } else if (dynamic_cast<Any*>(expr)) {
        out << "true";
    } else if (auto* nr = dynamic_cast<NameRef*>(expr)) {
        out << "is_" << nr->name << "(c)";
    }
}
} // anonymous namespace

void cg_charset_predicate(CharsetExpr* expr, std::ostream& out) {
    emit_charset_pred(expr, out);
}

// ═══════════════════════════════════════════════════════════════
// Utility
// ═══════════════════════════════════════════════════════════════

std::string indent_str(int indent) {
    return std::string(indent * 4, ' ');
}

std::string escape_string(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

bool is_keyword_literal(Literal* lit) {
    if (lit->value.empty()) return false;
    char first = lit->value[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_'))
        return false;
    for (char c : lit->value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

int next_var_id(Ctx* ctx) {
    return ctx->var_counter++;
}

} // namespace cpp_emit
} // namespace pegc
