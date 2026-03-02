#include "ParserEmitter.h"
#include "FrameProcessor.h"

using namespace PegGrammar;

namespace pegc {

ParserEmitter::ParserEmitter(const AnalysisResult& analysis, const std::string& ns)
    : analysis_(analysis), ns_(ns) {}

// ═══════════════════════════════════════════════════════════════
// Frame-based emission
// ═══════════════════════════════════════════════════════════════

bool ParserEmitter::emit_header_to_frame(Grammar* grammar,
                                          const std::string& frame_path,
                                          std::ostream& out) {
    bbqgen::FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    // Collect production names
    for (auto* p : grammar->productions)
        production_names_.insert(p->name);

    fp.CopyFramePart("headers");
    emit_headers(grammar, out);

    fp.CopyFramePart("namespace_open");
    if (!ns_.empty()) out << "namespace " << ns_ << " {\n\n";

    fp.CopyFramePart("public_decls");
    emit_public_decls(grammar, out);

    fp.CopyFramePart("private_decls");
    emit_private_decls(grammar, out);

    fp.CopyFramePart("parse_methods");
    emit_parse_method_decls(grammar, out);

    fp.CopyFramePart("namespace_close");
    if (!ns_.empty()) out << "} // namespace " << ns_ << "\n";

    return true;
}

bool ParserEmitter::emit_impl_to_frame(Grammar* grammar,
                                        const std::string& frame_path,
                                        const std::string& header_include,
                                        std::ostream& out) {
    bbqgen::FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    for (auto* p : grammar->productions)
        production_names_.insert(p->name);

    fp.CopyFramePart("parser_include");
    out << "#include \"" << header_include << "\"\n\n";

    fp.CopyFramePart("namespace_open");
    if (!ns_.empty()) out << "namespace " << ns_ << " {\n\n";

    fp.CopyFramePart("entry_call");
    // Entry call: if the start production has parameters, declare locals
    if (!grammar->productions.empty()) {
        auto* start = grammar->productions[0];
        if (start->params.empty()) {
            out << "    return parse_" << start->name << "();\n";
        } else {
            for (auto* p : start->params) {
                out << "    " << p->type_code << " _" << p->name << "{};\n";
            }
            out << "    return parse_" << start->name << "(";
            for (size_t i = 0; i < start->params.size(); i++) {
                if (i > 0) out << ", ";
                out << "_" << start->params[i]->name;
            }
            out << ");\n";
        }
    } else {
        out << "    return true;\n";
    }

    fp.CopyFramePart("skip_setup");
    emit_skip_setup(grammar, out);

    fp.CopyFramePart("parse_implementations");
    emit_parse_method_impls(grammar, out);

    fp.CopyFramePart("namespace_close");
    if (!ns_.empty()) out << "} // namespace " << ns_ << "\n";

    return true;
}

// ═══════════════════════════════════════════════════════════════
// String-based emission (for testing)
// ═══════════════════════════════════════════════════════════════

std::string ParserEmitter::emit_header(Grammar* grammar) {
    std::ostringstream ss;
    for (auto* p : grammar->productions)
        production_names_.insert(p->name);
    emit_headers(grammar, ss);
    emit_public_decls(grammar, ss);
    emit_private_decls(grammar, ss);
    emit_parse_method_decls(grammar, ss);
    return ss.str();
}

std::string ParserEmitter::emit_impl(Grammar* grammar) {
    std::ostringstream ss;
    for (auto* p : grammar->productions)
        production_names_.insert(p->name);
    emit_skip_setup(grammar, ss);
    emit_parse_method_impls(grammar, ss);
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// Header emission details
// ═══════════════════════════════════════════════════════════════

void ParserEmitter::emit_headers(Grammar* grammar, std::ostream& out) {
    for (auto* h : grammar->headers) {
        out << h->code << "\n";
    }
}

void ParserEmitter::emit_public_decls(Grammar* grammar, std::ostream& out) {
    // Nothing extra for now
}

void ParserEmitter::emit_private_decls(Grammar* grammar, std::ostream& out) {
    // Nothing extra for now
}

void ParserEmitter::emit_parse_method_decls(Grammar* grammar, std::ostream& out) {
    for (auto* p : grammar->productions) {
        emit_production_decl(p, out);
    }
}

void ParserEmitter::emit_production_decl(Production* prod, std::ostream& out) {
    out << "    bool parse_" << prod->name << "(";
    for (size_t i = 0; i < prod->params.size(); i++) {
        if (i > 0) out << ", ";
        out << prod->params[i]->type_code;
        if (prod->params[i]->is_ref) out << "&";
        out << " " << prod->params[i]->name;
    }
    out << ");\n";
}

// ═══════════════════════════════════════════════════════════════
// Implementation emission
// ═══════════════════════════════════════════════════════════════

void ParserEmitter::emit_skip_setup(Grammar* grammar, std::ostream& out) {
    out << "void Parser::setup_skip() {\n";

    // Whitespace function
    if (grammar->has_ignore && grammar->ignore.has_value()) {
        out << "    set_whitespace([](char c) -> bool {\n";
        out << "        return ";
        emit_charset_predicate(*grammar->ignore, out);
        out << ";\n";
        out << "    });\n";
    }

    // Comment specs
    if (!grammar->comments.empty()) {
        out << "    set_comments({";
        for (size_t i = 0; i < grammar->comments.size(); i++) {
            if (i > 0) out << ", ";
            auto* c = grammar->comments[i];
            out << "{\"" << escape_string(c->open) << "\", \""
                << escape_string(c->close) << "\", "
                << (c->nested ? "true" : "false") << "}";
        }
        out << "});\n";
    }

    out << "}\n\n";
}

void ParserEmitter::emit_parse_method_impls(Grammar* grammar, std::ostream& out) {
    for (auto* p : grammar->productions) {
        emit_production_impl(p, out);
    }
}

void ParserEmitter::emit_production_impl(Production* prod, std::ostream& out) {
    var_counter_ = 0;

    out << "bool Parser::parse_" << prod->name << "(";
    for (size_t i = 0; i < prod->params.size(); i++) {
        if (i > 0) out << ", ";
        out << prod->params[i]->type_code;
        if (prod->params[i]->is_ref) out << "&";
        out << " " << prod->params[i]->name;
    }
    out << ") {\n";

    // Local variables
    if (prod->locals_code.has_value() && !prod->locals_code->empty()) {
        out << "    " << *prod->locals_code << "\n";
    }

    // Body — at production level, return false = production fails
    emit_expr(prod->body, out, 1);

    out << "    return true;\n";
    out << "}\n\n";
}

// ═══════════════════════════════════════════════════════════════
// PEG Expression Code Generation (DDCG → C++)
//
// Key invariant: emit_expr emits code where "return false" means
// the expression failed. At production level, this exits the method.
// Inside a lambda (star/choice/optional/and/not), it returns from
// the lambda, allowing the enclosing operator to handle recovery.
// ═══════════════════════════════════════════════════════════════

void ParserEmitter::emit_expr(PegExpr* expr, std::ostream& out, int indent) {
    if (auto* seq = dynamic_cast<Sequence*>(expr)) {
        emit_sequence(seq, out, indent);
    } else if (auto* ch = dynamic_cast<Choice*>(expr)) {
        emit_choice(ch, out, indent);
    } else if (auto* star = dynamic_cast<Star*>(expr)) {
        emit_star(star, out, indent);
    } else if (auto* plus = dynamic_cast<Plus*>(expr)) {
        emit_plus(plus, out, indent);
    } else if (auto* opt = dynamic_cast<Optional*>(expr)) {
        emit_optional(opt, out, indent);
    } else if (auto* a = dynamic_cast<And*>(expr)) {
        emit_and(a, out, indent);
    } else if (auto* n = dynamic_cast<Not*>(expr)) {
        emit_not(n, out, indent);
    } else if (auto* lit = dynamic_cast<Literal*>(expr)) {
        emit_literal(lit, out, indent);
    } else if (auto* rc = dynamic_cast<RuleCall*>(expr)) {
        emit_rule_call(rc, out, indent);
    } else if (auto* act = dynamic_cast<Action*>(expr)) {
        emit_action(act, out, indent);
    } else if (auto* res = dynamic_cast<Resolver*>(expr)) {
        emit_resolver(res, out, indent);
    } else if (auto* grp = dynamic_cast<Group*>(expr)) {
        emit_expr(grp->body, out, indent);
    } else if (dynamic_cast<AnyExpr*>(expr)) {
        out << indent_str(indent) << "if (at_end()) return false;\n";
        out << indent_str(indent) << "advance();\n";
    }
}

// ── Sequence (DDCG §6): e1 e2 ──────────────────────────────
// CG[e1] effect L1;  L1: CG[e2] effect γ
// → emit each element in order; return false propagates from any
void ParserEmitter::emit_sequence(Sequence* seq, std::ostream& out, int indent) {
    for (auto* elem : seq->elements) {
        emit_expr(elem, out, indent);
    }
}

// ── Helpers to extract first literal from an alternative ─────

static PegExpr* first_element(PegExpr* alt) {
    if (auto* s = dynamic_cast<Sequence*>(alt))
        if (!s->elements.empty()) return s->elements[0];
    return alt;
}

static Literal* first_literal(PegExpr* alt) {
    return dynamic_cast<Literal*>(first_element(alt));
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
void ParserEmitter::emit_choice(Choice* ch, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int n = static_cast<int>(ch->alternatives.size());

    if (n == 0) return;
    if (n == 1) {
        emit_expr(ch->alternatives[0], out, indent);
        return;
    }

    // Check for head-fail optimization: all alternatives start with a literal
    bool all_literal_starts = true;
    for (auto* alt : ch->alternatives) {
        if (!first_literal(alt)) {
            all_literal_starts = false;
            break;
        }
    }

    if (all_literal_starts && n <= 8) {
        // Head-fail optimization: peek tests determine which alternative
        // to commit to. No save/restore needed.
        // Skip whitespace before peeking so we test the actual next token.
        out << ind << "skip();\n";
        for (int i = 0; i < n; i++) {
            auto* alt = ch->alternatives[i];
            auto* lit = first_literal(alt);
            bool is_last = (i == n - 1);

            if (!is_last) {
                if (is_keyword_literal(lit)) {
                    out << ind << "if (peek_keyword(\"" << escape_string(lit->value) << "\")) {\n";
                } else {
                    out << ind << "if (peek_at(\"" << escape_string(lit->value) << "\")) {\n";
                }
                emit_expr(alt, out, indent + 1);
                out << ind << "} else ";
            } else {
                out << "{\n";
                emit_expr(alt, out, indent + 1);
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
    int mid = next_var_id();
    out << ind << "{\n";
    out << ind << "    auto _m" << mid << " = save();\n";

    for (int i = 0; i < n; i++) {
        bool is_last = (i == n - 1);

        if (!is_last) {
            out << ind << "    if ([&]() -> bool {\n";
            emit_expr(ch->alternatives[i], out, indent + 2);
            out << ind << "        return true;\n";
            out << ind << "    }()) {} else {\n";
            out << ind << "    restore(_m" << mid << ");\n";
        } else {
            // Last alternative: failures propagate to enclosing scope
            emit_expr(ch->alternatives[i], out, indent + 1);
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
void ParserEmitter::emit_star(Star* star, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id();
    out << ind << "for (;;) {\n";
    out << ind << "    auto _m" << vid << " = save();\n";
    out << ind << "    if (![&]() -> bool {\n";
    emit_expr(star->body, out, indent + 2);
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
void ParserEmitter::emit_plus(Plus* plus, std::ostream& out, int indent) {
    // First occurrence: emitted directly, return false = production fails
    emit_expr(plus->body, out, indent);
    // Then star loop
    auto star_node = Star(plus->body);
    emit_star(&star_node, out, indent);
}

// ── Optional (DDCG §10): e? ────────────────────────────────
//
// DDCG: OP_CHOICE L1; CG[e]; OP_COMMIT L2; L1: (always succeeds)
// C++:  Lambda body. If fails, restore. Always succeeds overall.
//
void ParserEmitter::emit_optional(Optional* opt, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id();
    out << ind << "{\n";
    out << ind << "    auto _m" << vid << " = save();\n";
    out << ind << "    if (![&]() -> bool {\n";
    emit_expr(opt->body, out, indent + 2);
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
void ParserEmitter::emit_and(And* a, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id();
    out << ind << "{\n";
    out << ind << "    auto _m" << vid << " = save();\n";
    out << ind << "    bool _ok" << vid << " = [&]() -> bool {\n";
    emit_expr(a->body, out, indent + 2);
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
void ParserEmitter::emit_not(Not* n, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id();
    out << ind << "{\n";
    out << ind << "    auto _m" << vid << " = save();\n";
    out << ind << "    bool _ok" << vid << " = [&]() -> bool {\n";
    emit_expr(n->body, out, indent + 2);
    out << ind << "        return true;\n";
    out << ind << "    }();\n";
    out << ind << "    restore(_m" << vid << ");\n";
    out << ind << "    if (_ok" << vid << ") return false;\n";
    out << ind << "}\n";
}

// ── Literal (DDCG §2): "foo" ───────────────────────────────
// OP_STRING "foo"  /  OP_TESTSTRING "foo" Lfalse
// → skip(); if(!match("foo")) return false;
// Identifier-like literals use keyword() to prevent prefix matching.
void ParserEmitter::emit_literal(Literal* lit, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    out << ind << "skip();\n";
    if (is_keyword_literal(lit)) {
        out << ind << "if (!keyword(\"" << escape_string(lit->value) << "\")) return false;\n";
    } else {
        out << ind << "if (!match(\"" << escape_string(lit->value) << "\")) return false;\n";
    }
}

// ── RuleCall (DDCG §16): parse_Foo(args) ────────────────────
// OP_CALL A / OP_JUMP A (tail call)
// → skip(); if(!parse_Foo(args)) return false;
void ParserEmitter::emit_rule_call(RuleCall* rc, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);

    if (production_names_.count(rc->name)) {
        out << ind << "skip();\n";
        out << ind << "if (!parse_" << rc->name << "(";
        for (size_t i = 0; i < rc->args.size(); i++) {
            if (i > 0) out << ", ";
            out << rc->args[i]->expr;
        }
        out << ")) return false;\n";
    } else {
        // Unknown reference — built-in token function (ident, integer, etc.)
        out << ind << "skip();\n";
        out << ind << "if (!" << rc->name << "(";
        for (size_t i = 0; i < rc->args.size(); i++) {
            if (i > 0) out << ", ";
            out << rc->args[i]->expr;
        }
        out << ")) return false;\n";
    }
}

// ── Action: (. code .) ──────────────────────────────────────
void ParserEmitter::emit_action(Action* act, std::ostream& out, int indent) {
    out << indent_str(indent) << act->code << "\n";
}

// ── Resolver: IF(cond) body ─────────────────────────────────
void ParserEmitter::emit_resolver(Resolver* res, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    out << ind << "if (" << res->code << ") {\n";
    emit_expr(res->body, out, indent + 1);
    out << ind << "}\n";
}

// ═══════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════

std::string ParserEmitter::indent_str(int indent) {
    return std::string(indent * 4, ' ');
}

std::string ParserEmitter::escape_string(const std::string& s) {
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

bool ParserEmitter::is_keyword_literal(Literal* lit) {
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

int ParserEmitter::next_var_id() {
    return var_counter_++;
}

// ── Charset predicate emission ──────────────────────────────

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

void ParserEmitter::emit_charset_predicate(CharsetExpr* expr, std::ostream& out) {
    emit_charset_pred(expr, out);
}

} // namespace pegc
