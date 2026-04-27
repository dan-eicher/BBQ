#include "CParserEmitter.h"
#include "FrameProcessor.h"

using namespace PegGrammar;

namespace pegc {

static std::string to_snake_case(const std::string& name) {
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

CParserEmitter::CParserEmitter(const AnalysisResult& analysis, const std::string& prefix)
    : analysis_(analysis), prefix_(prefix) {}

// ═══════════════════════════════════════════════════════════════
// Frame-based emission
// ═══════════════════════════════════════════════════════════════

bool CParserEmitter::emit_header_to_frame(Grammar* grammar,
                                           const std::string& frame_path,
                                           std::ostream& out) {
    bbqgen::FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    for (auto* p : grammar->productions)
        production_names_.insert(p->name);

    // Compute guard name from prefix + grammar name
    std::string guard = prefix_;
    guard += grammar->name;
    guard += "_PARSER_H";
    for (auto& c : guard) c = (char)toupper(c);

    fp.CopyFramePart("guard_open");
    out << "#ifndef " << guard << "\n";
    out << "#define " << guard << "\n";

    fp.CopyFramePart("headers");
    emit_headers(grammar, out);

    fp.CopyFramePart("public_decls");
    emit_public_decls(grammar, out);

    fp.CopyFramePart("init_decls");
    out << "void " << prefix_ << "parser_init(peg_state* p, const char* input, int length);\n";
    out << "bool " << prefix_ << "parser_parse(peg_state* p);\n";

    fp.CopyFramePart("parse_methods");
    emit_parse_method_decls(grammar, out);

    fp.CopyFramePart("guard_close");
    out << "#endif /* " << guard << " */\n";

    return true;
}

bool CParserEmitter::emit_impl_to_frame(Grammar* grammar,
                                         const std::string& frame_path,
                                         const std::string& header_include,
                                         std::ostream& out) {
    bbqgen::FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    for (auto* p : grammar->productions)
        production_names_.insert(p->name);

    fp.CopyFramePart("parser_include");
    out << "#include \"" << header_include << "\"\n\n";

    fp.CopyFramePart("skip_setup");
    emit_skip_setup(grammar, out);

    // Forward declarations for all parse functions (before init/parse use them)
    for (auto* prod : grammar->productions) {
        emit_production_decl(prod, out);
    }
    out << "\n";

    fp.CopyFramePart("init_impl");
    // init function
    out << "void " << prefix_ << "parser_init(peg_state* p, const char* input, int length) {\n";
    out << "    peg_init(p, input, length);\n";
    out << "    " << prefix_ << "setup_skip(p);\n";
    out << "}\n\n";

    // parse function
    out << "bool " << prefix_ << "parser_parse(peg_state* p) {\n";
    out << "    peg_skip(p);\n";
    if (!grammar->productions.empty()) {
        auto* start = grammar->productions[0];
        if (start->params.empty()) {
            out << "    return " << prefix_ << "parse_" << to_snake_case(start->name) << "(p);\n";
        } else {
            for (auto* param : start->params) {
                out << "    " << param->type_code << " _" << param->name;
                if (param->type_code.find('*') != std::string::npos)
                    out << " = NULL";
                else
                    out << " = {0}";
                out << ";\n";
            }
            out << "    return " << prefix_ << "parse_" << to_snake_case(start->name) << "(p";
            for (auto* param : start->params) {
                out << ", ";
                if (!param->is_ref) out << "&";
                out << "_" << param->name;
            }
            out << ");\n";
        }
    } else {
        out << "    return true;\n";
    }
    out << "}\n\n";

    fp.CopyFramePart("parse_implementations");
    emit_parse_method_impls(grammar, out);

    return true;
}

// ═══════════════════════════════════════════════════════════════
// Header emission
// ═══════════════════════════════════════════════════════════════

void CParserEmitter::emit_headers(Grammar* grammar, std::ostream& out) {
    for (auto* h : grammar->headers) {
        out << h->code << "\n";
    }
}

void CParserEmitter::emit_public_decls(Grammar* grammar, std::ostream& out) {
    (void)grammar;
}

void CParserEmitter::emit_parse_method_decls(Grammar* grammar, std::ostream& out) {
    // Production functions are static — don't declare them in the header.
    // Forward declarations go into the .c file (see emit_parse_method_impls).
    (void)grammar;
    (void)out;
}

void CParserEmitter::emit_production_decl(Production* prod, std::ostream& out) {
    out << "static bool " << prefix_ << "parse_" << to_snake_case(prod->name) << "(peg_state* p";
    for (auto* param : prod->params) {
        out << ", " << param->type_code;
        if (param->is_ref) out << "*";
        out << " " << param->name;
    }
    out << ");\n";
}

// ═══════════════════════════════════════════════════════════════
// Implementation emission
// ═══════════════════════════════════════════════════════════════

void CParserEmitter::emit_skip_setup(Grammar* grammar, std::ostream& out) {
    // Whitespace predicate as a named function
    if (grammar->has_ignore && grammar->ignore.has_value()) {
        out << "static bool " << prefix_ << "ws_predicate(char c) {\n";
        out << "    return ";
        emit_charset_predicate(*grammar->ignore, out);
        out << ";\n";
        out << "}\n\n";
    }

    out << "static void " << prefix_ << "setup_skip(peg_state* p) {\n";
    if (grammar->has_ignore && grammar->ignore.has_value()) {
        out << "    peg_set_whitespace(p, " << prefix_ << "ws_predicate);\n";
    }
    for (auto* c : grammar->comments) {
        out << "    peg_add_comment(p, \"" << escape_string(c->open)
            << "\", \"" << escape_string(c->close) << "\", "
            << (c->nested ? "true" : "false") << ");\n";
    }
    out << "}\n\n";
}

void CParserEmitter::emit_parse_method_impls(Grammar* grammar, std::ostream& out) {
    for (auto* p : grammar->productions) {
        emit_production_impl(p, out);
    }
}

void CParserEmitter::emit_production_impl(Production* prod, std::ostream& out) {
    var_counter_ = 0;

    out << "static bool " << prefix_ << "parse_" << to_snake_case(prod->name) << "(peg_state* p";
    for (auto* param : prod->params) {
        out << ", " << param->type_code;
        if (param->is_ref) out << "*";
        out << " " << param->name;
    }
    out << ") {\n";

    // Local variables
    if (prod->locals_code.has_value() && !prod->locals_code->empty()) {
        out << "    " << *prod->locals_code << "\n";
    }

    emit_expr(prod->body, out, 1);

    out << "    return true;\n";
    out << "}\n\n";
}

// ═══════════════════════════════════════════════════════════════
// PEG Expression Code Generation (DDCG → C)
//
// Key difference from C++ emitter: lambdas are replaced with
// do { ... } while(0) blocks where break = "return false".
// A bool _ok variable tracks success.
// ═══════════════════════════════════════════════════════════════

void CParserEmitter::emit_expr(PegExpr* expr, std::ostream& out, int indent) {
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
        out << indent_str(indent) << "if (peg_at_end(p)) " << fail_ << ";\n";
        out << indent_str(indent) << "peg_advance(p);\n";
    }
}

// ── Sequence: e1 e2 ────────────────────────────────────────
void CParserEmitter::emit_sequence(Sequence* seq, std::ostream& out, int indent) {
    for (auto* elem : seq->elements) {
        emit_expr(elem, out, indent);
    }
}

// ── Helpers ────────────────────────────────────────────────

static PegExpr* first_element(PegExpr* alt) {
    if (auto* s = dynamic_cast<Sequence*>(alt))
        if (!s->elements.empty()) return s->elements[0];
    return alt;
}

static Literal* first_literal(PegExpr* alt) {
    return dynamic_cast<Literal*>(first_element(alt));
}

// ── Choice: e1 / e2 / e3 ──────────────────────────────────
//
// C pattern: do { body; _ok = true; } while(0);
// On failure inside body: break exits the do-while.
//
void CParserEmitter::emit_choice(Choice* ch, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int n = static_cast<int>(ch->alternatives.size());

    if (n == 0) return;
    if (n == 1) {
        emit_expr(ch->alternatives[0], out, indent);
        return;
    }

    // Check for head-fail optimization
    bool all_literal_starts = true;
    for (auto* alt : ch->alternatives) {
        if (!first_literal(alt)) {
            all_literal_starts = false;
            break;
        }
    }

    if (all_literal_starts && n <= 8) {
        out << ind << "peg_skip(p);\n";
        for (int i = 0; i < n; i++) {
            auto* alt = ch->alternatives[i];
            auto* lit = first_literal(alt);
            bool is_last = (i == n - 1);

            if (!is_last) {
                if (is_keyword_literal(lit)) {
                    out << ind << "if (peg_peek_keyword(p, \"" << escape_string(lit->value) << "\")) {\n";
                } else {
                    out << ind << "if (peg_peek_at(p, \"" << escape_string(lit->value) << "\")) {\n";
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

    // General choice with do/while(0) per alternative
    int mid = next_var_id();
    out << ind << "{\n";
    out << ind << "    peg_mark _m" << mid << " = peg_save(p);\n";

    for (int i = 0; i < n; i++) {
        bool is_last = (i == n - 1);

        if (!is_last) {
            int vid = next_var_id();
            out << ind << "    {\n";
            out << ind << "        bool _ok" << vid << " = false;\n";
            out << ind << "        do {\n";
            { auto saved = fail_; fail_ = "break";
              emit_expr(ch->alternatives[i], out, indent + 3);
              fail_ = saved; }
            out << ind << "            _ok" << vid << " = true;\n";
            out << ind << "        } while(0);\n";
            out << ind << "        if (!_ok" << vid << ") {\n";
            out << ind << "            peg_restore(p, _m" << mid << ");\n";
            out << ind << "        } else goto _choice_done" << mid << ";\n";
            out << ind << "    }\n";
        } else {
            // Last alternative: failures propagate
            emit_expr(ch->alternatives[i], out, indent + 1);
        }
    }

    out << ind << "_choice_done" << mid << ":;\n";
    out << ind << "}\n";
}

// ── Star: e* ───────────────────────────────────────────────
void CParserEmitter::emit_star(Star* star, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id();
    out << ind << "for (;;) {\n";
    out << ind << "    peg_mark _m" << vid << " = peg_save(p);\n";
    out << ind << "    bool _ok" << vid << " = false;\n";
    out << ind << "    do {\n";
    { auto saved = fail_; fail_ = "break";
      emit_expr(star->body, out, indent + 2);
      fail_ = saved; }
    out << ind << "        _ok" << vid << " = true;\n";
    out << ind << "    } while(0);\n";
    out << ind << "    if (!_ok" << vid << ") { peg_restore(p, _m" << vid << "); break; }\n";
    out << ind << "}\n";
}

// ── Plus: e+ ───────────────────────────────────────────────
void CParserEmitter::emit_plus(Plus* plus, std::ostream& out, int indent) {
    emit_expr(plus->body, out, indent);
    auto star_node = Star(plus->body);
    emit_star(&star_node, out, indent);
}

// ── Optional: e? ───────────────────────────────────────────
void CParserEmitter::emit_optional(Optional* opt, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id();
    out << ind << "{\n";
    out << ind << "    peg_mark _m" << vid << " = peg_save(p);\n";
    out << ind << "    bool _ok" << vid << " = false;\n";
    out << ind << "    do {\n";
    { auto saved = fail_; fail_ = "break";
      emit_expr(opt->body, out, indent + 2);
      fail_ = saved; }
    out << ind << "        _ok" << vid << " = true;\n";
    out << ind << "    } while(0);\n";
    out << ind << "    if (!_ok" << vid << ") peg_restore(p, _m" << vid << ");\n";
    out << ind << "}\n";
}

// ── And predicate: &e ──────────────────────────────────────
void CParserEmitter::emit_and(And* a, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id();
    out << ind << "{\n";
    out << ind << "    peg_mark _m" << vid << " = peg_save(p);\n";
    out << ind << "    bool _ok" << vid << " = false;\n";
    out << ind << "    do {\n";
    { auto saved = fail_; fail_ = "break";
      emit_expr(a->body, out, indent + 2);
      fail_ = saved; }
    out << ind << "        _ok" << vid << " = true;\n";
    out << ind << "    } while(0);\n";
    out << ind << "    peg_restore(p, _m" << vid << ");\n";
    out << ind << "    if (!_ok" << vid << ") " << fail_ << ";\n";
    out << ind << "}\n";
}

// ── Not predicate: !e ──────────────────────────────────────
void CParserEmitter::emit_not(Not* n, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    int vid = next_var_id();
    out << ind << "{\n";
    out << ind << "    peg_mark _m" << vid << " = peg_save(p);\n";
    out << ind << "    bool _ok" << vid << " = false;\n";
    out << ind << "    do {\n";
    { auto saved = fail_; fail_ = "break";
      emit_expr(n->body, out, indent + 2);
      fail_ = saved; }
    out << ind << "        _ok" << vid << " = true;\n";
    out << ind << "    } while(0);\n";
    out << ind << "    peg_restore(p, _m" << vid << ");\n";
    out << ind << "    if (_ok" << vid << ") " << fail_ << ";\n";
    out << ind << "}\n";
}

// ── Literal: "foo" ─────────────────────────────────────────
void CParserEmitter::emit_literal(Literal* lit, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    out << ind << "peg_skip(p);\n";
    if (is_keyword_literal(lit)) {
        out << ind << "if (!peg_keyword(p, \"" << escape_string(lit->value) << "\")) " << fail_ << ";\n";
    } else {
        out << ind << "if (!peg_match(p, \"" << escape_string(lit->value) << "\")) " << fail_ << ";\n";
    }
}

// ── Rule call: parse_Foo(p, args) ──────────────────────────
//
// Args are an opaque raw string — whatever the user wrote between
// `<...>` (or `<. ... .>`) flows verbatim into the call. This means
// the .peg author is responsible for matching the C signature
// (including `&` prefix on pointer-output params for built-ins like
// `peg_ident`); pegc no longer auto-prefixes.
void CParserEmitter::emit_rule_call(RuleCall* rc, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    std::string fn_name = production_names_.count(rc->name)
        ? prefix_ + "parse_" + to_snake_case(rc->name)
        : "peg_" + rc->name;

    out << ind << "peg_skip(p);\n";
    out << ind << "if (!" << fn_name << "(p";
    if (!rc->args.empty()) out << ", " << rc->args;
    out << ")) " << fail_ << ";\n";
}

// ── Action: (. code .) ─────────────────────────────────────
void CParserEmitter::emit_action(Action* act, std::ostream& out, int indent) {
    out << indent_str(indent) << act->code << "\n";
}

// ── Resolver: IF(cond) body ────────────────────────────────
void CParserEmitter::emit_resolver(Resolver* res, std::ostream& out, int indent) {
    std::string ind = indent_str(indent);
    out << ind << "if (" << res->code << ") {\n";
    emit_expr(res->body, out, indent + 1);
    out << ind << "}\n";
}

// ═══════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════

std::string CParserEmitter::indent_str(int indent) {
    return std::string(indent * 4, ' ');
}

std::string CParserEmitter::escape_string(const std::string& s) {
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

bool CParserEmitter::is_keyword_literal(Literal* lit) {
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

int CParserEmitter::next_var_id() {
    return var_counter_++;
}

// ── Charset predicate emission ─────────────────────────────

namespace {
void emit_charset_pred_c(CharsetExpr* expr, std::ostream& out) {
    if (auto* r = dynamic_cast<Range*>(expr)) {
        out << "(c >= " << r->from << " && c <= " << r->to << ")";
    } else if (auto* s = dynamic_cast<Single*>(expr)) {
        out << "(c == " << s->ch << ")";
    } else if (auto* u = dynamic_cast<Union*>(expr)) {
        out << "(";
        emit_charset_pred_c(u->left, out);
        out << " || ";
        emit_charset_pred_c(u->right, out);
        out << ")";
    } else if (auto* d = dynamic_cast<Diff*>(expr)) {
        out << "(";
        emit_charset_pred_c(d->base, out);
        out << " && !(";
        emit_charset_pred_c(d->minus, out);
        out << "))";
    } else if (dynamic_cast<Any*>(expr)) {
        out << "true";
    } else if (auto* nr = dynamic_cast<NameRef*>(expr)) {
        out << "is_" << nr->name << "(c)";
    }
}
} // anonymous namespace

void CParserEmitter::emit_charset_predicate(CharsetExpr* expr, std::ostream& out) {
    emit_charset_pred_c(expr, out);
}

} // namespace pegc
