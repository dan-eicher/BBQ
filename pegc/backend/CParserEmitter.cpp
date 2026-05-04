#include "CParserEmitter.h"
#include "FrameProcessor.h"

using namespace PegGrammar;

namespace pegc {

CParserEmitter::CParserEmitter(const AnalysisResult& analysis, const std::string& prefix)
    : analysis_(analysis) {
    ctx_.prefix = prefix;
}

// Populate the per-emission ctx with the grammar's name sets so cg_*
// functions can dispatch on charset / production / token references.
static void seed_ctx_from_grammar(c_emit::Ctx& ctx, Grammar* grammar) {
    for (auto* p : grammar->productions)
        ctx.production_names.insert(p->name);
    for (auto* t : grammar->tokens)
        ctx.token_names.insert(t->name);
    for (auto* c : grammar->charsets)
        ctx.charset_names.insert(c->name);
}

// ═══════════════════════════════════════════════════════════════
// Frame-based emission
// ═══════════════════════════════════════════════════════════════

bool CParserEmitter::emit_header_to_frame(Grammar* grammar,
                                           const std::string& frame_path,
                                           std::ostream& out) {
    bbqgen::FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    seed_ctx_from_grammar(ctx_, grammar);

    // Compute guard name from prefix + grammar name
    std::string guard = ctx_.prefix;
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
    out << "void " << ctx_.prefix << "parser_init(peg_state* p, const char* input, int length);\n";
    out << "bool " << ctx_.prefix << "parser_parse(peg_state* p);\n";

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

    seed_ctx_from_grammar(ctx_, grammar);

    fp.CopyFramePart("parser_include");
    out << "#include \"" << header_include << "\"\n\n";

    fp.CopyFramePart("runtime");

    fp.CopyFramePart("skip_setup");
    emit_charset_defs(grammar, out);
    emit_skip_setup(grammar, out);
    emit_token_forward_decls(grammar, out);

    // Forward declarations for all parse functions (before init/parse use them)
    for (auto* prod : grammar->productions) {
        emit_production_decl(prod, out);
    }
    out << "\n";

    fp.CopyFramePart("init_impl");
    // init function
    out << "void " << ctx_.prefix << "parser_init(peg_state* p, const char* input, int length) {\n";
    out << "    peg_init(p, input, length);\n";
    out << "    " << ctx_.prefix << "setup_skip(p);\n";
    out << "}\n\n";

    // parse function
    out << "bool " << ctx_.prefix << "parser_parse(peg_state* p) {\n";
    out << "    peg_skip(p);\n";
    if (!grammar->productions.empty()) {
        auto* start = grammar->productions[0];
        if (start->params.empty()) {
            out << "    return " << ctx_.prefix << "parse_"
                << c_emit::to_snake_case(start->name) << "(p);\n";
        } else {
            for (auto* param : start->params) {
                out << "    " << param->type_code << " _" << param->name;
                if (param->type_code.find('*') != std::string::npos)
                    out << " = NULL";
                else
                    out << " = {0}";
                out << ";\n";
            }
            out << "    return " << ctx_.prefix << "parse_"
                << c_emit::to_snake_case(start->name) << "(p";
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
    emit_token_method_impls(grammar, out);
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
    out << "static bool " << ctx_.prefix << "parse_"
        << c_emit::to_snake_case(prod->name) << "(peg_state* p";
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

void CParserEmitter::emit_charset_defs(Grammar* grammar, std::ostream& out) {
    for (auto* cd : grammar->charsets) {
        out << "static bool is_" << cd->name << "(char c) {\n";
        out << "    return ";
        c_emit::cg_charset_predicate(cd->body, out);
        out << ";\n";
        out << "}\n\n";
    }
}

// ── Token emission ────────────────────────────────────────────
// Tokens use the same peg_expr AST as productions; the only difference
// is emission mode (no auto-skip between atoms in lex mode).

void CParserEmitter::emit_token_forward_decls(Grammar* grammar, std::ostream& out) {
    for (auto* t : grammar->tokens) {
        emit_token_forward_decl(t, out);
    }
}

void CParserEmitter::emit_token_forward_decl(TokenDef* tok, std::ostream& out) {
    out << "static bool " << ctx_.prefix << tok->name
        << "(peg_state* p, peg_span* out);\n";
}

void CParserEmitter::emit_token_method_impls(Grammar* grammar, std::ostream& out) {
    for (auto* t : grammar->tokens) {
        emit_token_impl(t, out);
    }
}

void CParserEmitter::emit_token_impl(TokenDef* tok, std::ostream& out) {
    ctx_.var_counter = 0;

    out << "static bool " << ctx_.prefix << tok->name
        << "(peg_state* p, peg_span* out) {\n";
    out << "    const char* _start = peg_pos(p);\n";

    ctx_.in_lex_mode = true;
    c_emit::cg_expr(&ctx_, tok->body, out, 1);
    ctx_.in_lex_mode = false;

    // out is optional — pure-recognizer tokens (keywords, punctuation)
    // don't need the captured span at the call site.
    out << "    if (out) { out->ptr = _start; out->len = (int)(peg_pos(p) - _start); }\n";
    out << "    return true;\n";
    out << "}\n\n";
}

void CParserEmitter::emit_skip_setup(Grammar* grammar, std::ostream& out) {
    // Whitespace predicate as a named function
    if (grammar->has_ignore && grammar->ignore.has_value()) {
        out << "static bool " << ctx_.prefix << "ws_predicate(char c) {\n";
        out << "    return ";
        c_emit::cg_charset_predicate(*grammar->ignore, out);
        out << ";\n";
        out << "}\n\n";
    }

    out << "static void " << ctx_.prefix << "setup_skip(peg_state* p) {\n";
    if (grammar->has_ignore && grammar->ignore.has_value()) {
        out << "    peg_set_whitespace(p, " << ctx_.prefix << "ws_predicate);\n";
    }
    for (auto* c : grammar->comments) {
        out << "    peg_add_comment(p, \"" << c_emit::escape_string(c->open)
            << "\", \"" << c_emit::escape_string(c->close) << "\", "
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
    ctx_.var_counter = 0;

    out << "static bool " << ctx_.prefix << "parse_"
        << c_emit::to_snake_case(prod->name) << "(peg_state* p";
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

    c_emit::cg_expr(&ctx_, prod->body, out, 1);

    out << "    return true;\n";
    out << "}\n\n";
}

} // namespace pegc
