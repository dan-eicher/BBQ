#include "java_emit.h"

using namespace PegGrammar;

namespace pegc {
namespace java_emit {

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
// PEG Expression Code Generation (DDCG → Java 1.0)
//
// Backtracking is `int _mN = p.save()` / `p.restore(_mN)`: a mark is a plain
// input index, so a save costs nothing and allocates nothing on the hot path.
// Line and column are recovered from an index only when a diagnostic asks.
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
        out << indent_str(indent) << "if (p.atEnd()) " << ctx->fail << ";\n";
        out << indent_str(indent) << "p.advance();\n";
    } else if (auto* cr = dynamic_cast<CharRange*>(expr)) {
        out << indent_str(indent) << "if (p.atEnd() || p.peekChar() < "
            << cr->from << " || p.peekChar() > " << cr->to << ") "
            << ctx->fail << ";\n";
        out << indent_str(indent) << "p.advance();\n";
    } else if (auto* csr = dynamic_cast<CharsetRef*>(expr)) {
        out << indent_str(indent) << "if (p.atEnd() || !is_" << csr->name
            << "(p.peekChar())) " << ctx->fail << ";\n";
        out << indent_str(indent) << "p.advance();\n";
    }
}

// ── Sequence: e1 e2 ────────────────────────────────────────
void cg_seq(Ctx* ctx, Sequence* seq, std::ostream& out, int indent) {
    for (auto* elem : seq->elements) {
        cg_expr(ctx, elem, out, indent);
    }
}

// ── Choice: e1 / e2 / e3 ──────────────────────────────────
//
// Java pattern: do { body; _ok = true; } while (false);
// On failure inside body: break exits the do-while.
//
// Committing to a successful alternative is a forward jump out of the
// enclosing block. Java has no goto, so the block is labeled and the jump is
// `break _choice_doneN` — the same control flow the C emitter's goto expresses.
//
// The labeled block does not capture the unlabeled `break` that carries a
// failure action: JLS 14.14 targets an unlabeled break at the innermost
// enclosing switch/while/do/for, and a labeled block is none of those. So a
// choice nested inside a star's `do { } while (false)` still fails the star
// iteration, which is what PEG semantics require.
//
void cg_choice(Ctx* ctx, Choice* ch, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int n = static_cast<int>(ch->alternatives.size());

    if (n == 0) return;
    if (n == 1) {
        cg_expr(ctx, ch->alternatives[0], out, indent);
        return;
    }

    // Check for head-fail optimization
    bool all_literal_starts = true;
    for (auto* alt : ch->alternatives) {
        auto* lit = first_literal(alt);
        if (!lit) {
            all_literal_starts = false;
            break;
        }
        if (!ctx->in_lex_mode && is_keyword_literal(lit)) {
            all_literal_starts = false;
            break;
        }
    }

    // Predictive dispatch is sound only when the heads are pairwise
    // prefix-disjoint: if one head is a prefix of another, peeking cannot tell
    // them apart and would commit to the earlier alternative, which PEG
    // ordered choice forbids once that alternative fails deeper. Same rule and
    // same reasoning as the C emitter.
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
        if (!ctx->in_lex_mode) out << ind << "p.skip();\n";
        for (int i = 0; i < n; i++) {
            auto* alt = ch->alternatives[i];
            auto* lit = first_literal(alt);
            bool is_last = (i == n - 1);

            if (!is_last) {
                out << ind << "if (p.peekAt(\"" << escape_string(lit->value)
                    << "\")) {\n";
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

    // General choice: one labeled block, one mark, a do/while(false) per
    // non-final alternative.
    int mid = next_var_id(ctx);
    out << ind << "_choice_done" << mid << ": {\n";
    out << ind << "    int _m" << mid << " = p.save();\n";

    for (int i = 0; i < n; i++) {
        bool is_last = (i == n - 1);

        if (!is_last) {
            int vid = next_var_id(ctx);
            out << ind << "    {\n";
            out << ind << "        boolean _ok" << vid << " = false;\n";
            out << ind << "        do {\n";
            { auto saved = ctx->fail; ctx->fail = "break";
              cg_expr(ctx, ch->alternatives[i], out, indent + 3);
              ctx->fail = saved; }
            out << ind << "            _ok" << vid << " = true;\n";
            out << ind << "        } while (false);\n";
            out << ind << "        if (!_ok" << vid << ") {\n";
            out << ind << "            p.restore(_m" << mid << ");\n";
            out << ind << "        } else break _choice_done" << mid << ";\n";
            out << ind << "    }\n";
        } else {
            // Last alternative: failures propagate
            cg_expr(ctx, ch->alternatives[i], out, indent + 1);
        }
    }

    out << ind << "}\n";
}

// ── Star: e* ───────────────────────────────────────────────
void cg_star(Ctx* ctx, Star* star, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);

    // Charset predicate tests don't consume on fail, so the save/restore
    // around them is dead work — collapse to a tight while loop.
    if (auto* csr = dynamic_cast<CharsetRef*>(star->body)) {
        if (ctx->charset_names.count(csr->name)) {
            out << ind << "while (!p.atEnd() && is_" << csr->name
                << "(p.peekChar())) p.advance();\n";
            return;
        }
    }

    int vid = next_var_id(ctx);
    out << ind << "for (;;) {\n";
    out << ind << "    int _m" << vid << " = p.save();\n";
    out << ind << "    boolean _ok" << vid << " = false;\n";
    out << ind << "    do {\n";
    { auto saved = ctx->fail; ctx->fail = "break";
      cg_expr(ctx, star->body, out, indent + 2);
      ctx->fail = saved; }
    out << ind << "        _ok" << vid << " = true;\n";
    out << ind << "    } while (false);\n";
    out << ind << "    if (!_ok" << vid << ") { p.restore(_m" << vid << "); break; }\n";
    out << ind << "}\n";
}

// ── Plus: e+ ───────────────────────────────────────────────
void cg_plus(Ctx* ctx, Plus* plus, std::ostream& out, int indent) {
    cg_expr(ctx, plus->body, out, indent);
    auto star_node = Star(plus->body);
    cg_star(ctx, &star_node, out, indent);
}

// ── Optional: e? ───────────────────────────────────────────
void cg_optional(Ctx* ctx, Optional* opt, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id(ctx);
    out << ind << "{\n";
    out << ind << "    int _m" << vid << " = p.save();\n";
    out << ind << "    boolean _ok" << vid << " = false;\n";
    out << ind << "    do {\n";
    { auto saved = ctx->fail; ctx->fail = "break";
      cg_expr(ctx, opt->body, out, indent + 2);
      ctx->fail = saved; }
    out << ind << "        _ok" << vid << " = true;\n";
    out << ind << "    } while (false);\n";
    out << ind << "    if (!_ok" << vid << ") p.restore(_m" << vid << ");\n";
    out << ind << "}\n";
}

// ── And predicate: &e ──────────────────────────────────────
void cg_and(Ctx* ctx, And* a, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id(ctx);
    out << ind << "{\n";
    out << ind << "    int _m" << vid << " = p.save();\n";
    out << ind << "    boolean _ok" << vid << " = false;\n";
    out << ind << "    do {\n";
    { auto saved = ctx->fail; ctx->fail = "break";
      cg_expr(ctx, a->body, out, indent + 2);
      ctx->fail = saved; }
    out << ind << "        _ok" << vid << " = true;\n";
    out << ind << "    } while (false);\n";
    out << ind << "    p.restore(_m" << vid << ");\n";
    out << ind << "    if (!_ok" << vid << ") " << ctx->fail << ";\n";
    out << ind << "}\n";
}

// ── Not predicate: !e ──────────────────────────────────────
void cg_not(Ctx* ctx, Not* n, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id(ctx);
    out << ind << "{\n";
    out << ind << "    int _m" << vid << " = p.save();\n";
    out << ind << "    boolean _ok" << vid << " = false;\n";
    out << ind << "    do {\n";
    { auto saved = ctx->fail; ctx->fail = "break";
      cg_expr(ctx, n->body, out, indent + 2);
      ctx->fail = saved; }
    out << ind << "        _ok" << vid << " = true;\n";
    out << ind << "    } while (false);\n";
    out << ind << "    p.restore(_m" << vid << ");\n";
    out << ind << "    if (_ok" << vid << ") " << ctx->fail << ";\n";
    out << ind << "}\n";
}

// ── Literal: "foo" ─────────────────────────────────────────
void cg_literal(Ctx* ctx, Literal* lit, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    if (!ctx->in_lex_mode) out << ind << "p.skip();\n";
    out << ind << "if (!p.match(\"" << escape_string(lit->value)
        << "\")) " << ctx->fail << ";\n";
}

// ── Rule call: parse_foo(args) ─────────────────────────────
//
// Args are an opaque raw string — whatever the user wrote between `<...>` (or
// `<. ... .>`) flows verbatim into the call, so a grammar targeting Java writes
// Java argument syntax there. There is no `&` for an output parameter: a Java
// grammar returns through an object or a one-element array.
void cg_rule_call(Ctx* ctx, RuleCall* rc, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);

    // Charset reference: predicate-test-and-consume-one-char. Charsets are
    // single-char (lex-level) predicates — never auto-skip, even in production
    // mode, so `!ident_continue` and similar boundary checks stay put.
    if (ctx->charset_names.count(rc->name)) {
        out << ind << "if (p.atEnd() || !is_" << rc->name
            << "(p.peekChar())) " << ctx->fail << ";\n";
        out << ind << "p.advance();\n";
        return;
    }

    std::string fn_name;
    if (ctx->production_names.count(rc->name)) {
        fn_name = "parse_" + to_snake_case(rc->name);
    } else {
        // Must be a TOKEN — analysis rejects anything else.
        fn_name = rc->name;
    }

    if (!ctx->in_lex_mode) out << ind << "p.skip();\n";
    out << ind << "if (!" << fn_name << "(";
    if (!rc->args.empty()) {
        out << rc->args;
    } else if (ctx->token_names.count(rc->name)) {
        // Tokens always take a Span out param; auto-pass null when the call
        // site doesn't care about the captured span (pure-recognizer use).
        out << "null";
    }
    out << ")) " << ctx->fail << ";\n";
}

// ── Action: (. code .) ─────────────────────────────────────
void cg_action(Ctx* ctx, Action* act, std::ostream& out, int indent) {
    out << indent_str(indent) << act->code << "\n";
}

// ── Resolver: IF(cond) body ────────────────────────────────
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
void emit_charset_pred_java(CharsetExpr* expr, std::ostream& out) {
    if (auto* r = dynamic_cast<Range*>(expr)) {
        out << "(c >= " << r->from << " && c <= " << r->to << ")";
    } else if (auto* s = dynamic_cast<Single*>(expr)) {
        out << "(c == " << s->ch << ")";
    } else if (auto* u = dynamic_cast<Union*>(expr)) {
        out << "(";
        emit_charset_pred_java(u->left, out);
        out << " || ";
        emit_charset_pred_java(u->right, out);
        out << ")";
    } else if (auto* d = dynamic_cast<Diff*>(expr)) {
        out << "(";
        emit_charset_pred_java(d->base, out);
        out << " && !(";
        emit_charset_pred_java(d->minus, out);
        out << "))";
    } else if (dynamic_cast<Any*>(expr)) {
        out << "true";
    } else if (auto* nr = dynamic_cast<NameRef*>(expr)) {
        out << "is_" << nr->name << "(c)";
    }
}
} // anonymous namespace

void cg_charset_predicate(CharsetExpr* expr, std::ostream& out) {
    emit_charset_pred_java(expr, out);
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

std::string to_snake_case(const std::string& name) {
    std::string result;
    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            if (i > 0) {
                char prev = name[i-1];
                if ((prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9'))
                    result += '_';
                else if ((prev >= 'A' && prev <= 'Z') && i + 1 < name.size() &&
                         (name[i+1] >= 'a' && name[i+1] <= 'z'))
                    result += '_';
            }
            result += (char)(c - 'A' + 'a');
        } else {
            result += c;
        }
    }
    return result;
}

} // namespace java_emit
} // namespace pegc
