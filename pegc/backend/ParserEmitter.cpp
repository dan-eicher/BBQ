#include "ParserEmitter.h"
#include "FrameProcessor.h"

using namespace PegGrammar;

namespace pegc {

ParserEmitter::ParserEmitter(const AnalysisResult& analysis, const std::string& ns)
    : analysis_(analysis), ns_(ns) {}

// Populate the per-emission ctx with the grammar's name sets so cg_*
// functions can dispatch on charset / production / token references.
static void seed_ctx_from_grammar(cpp_emit::Ctx& ctx, Grammar* grammar) {
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

bool ParserEmitter::emit_header_to_frame(Grammar* grammar,
                                          const std::string& frame_path,
                                          std::ostream& out) {
    bbqgen::FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    seed_ctx_from_grammar(ctx_, grammar);

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
    emit_token_method_decls(grammar, out);

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

    seed_ctx_from_grammar(ctx_, grammar);

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
    emit_charset_defs(grammar, out);
    emit_skip_setup(grammar, out);

    fp.CopyFramePart("parse_implementations");
    emit_token_method_impls(grammar, out);
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
    seed_ctx_from_grammar(ctx_, grammar);
    emit_headers(grammar, ss);
    emit_public_decls(grammar, ss);
    emit_private_decls(grammar, ss);
    emit_parse_method_decls(grammar, ss);
    emit_token_method_decls(grammar, ss);
    return ss.str();
}

std::string ParserEmitter::emit_impl(Grammar* grammar) {
    std::ostringstream ss;
    seed_ctx_from_grammar(ctx_, grammar);
    emit_charset_defs(grammar, ss);
    emit_skip_setup(grammar, ss);
    emit_token_method_impls(grammar, ss);
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
    if (!grammar->members.empty()) {
        out << grammar->members;
        if (grammar->members.back() != '\n') out << "\n";
    }
}

void ParserEmitter::emit_private_decls(Grammar* grammar, std::ostream& out) {
    if (!grammar->private_helpers.empty()) {
        out << grammar->private_helpers;
        if (grammar->private_helpers.back() != '\n') out << "\n";
    }
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

void ParserEmitter::emit_charset_defs(Grammar* grammar, std::ostream& out) {
    for (auto* cd : grammar->charsets) {
        out << "static bool is_" << cd->name << "(char c) {\n";
        out << "    return ";
        cpp_emit::cg_charset_predicate(cd->body, out);
        out << ";\n";
        out << "}\n\n";
    }
}

// ── Token emission ────────────────────────────────────────────
// Tokens use the same peg_expr AST as productions; the only difference
// is emission mode (no auto-skip between atoms in lex mode).

void ParserEmitter::emit_token_method_decls(Grammar* grammar, std::ostream& out) {
    for (auto* t : grammar->tokens) {
        emit_token_decl(t, out);
    }
}

void ParserEmitter::emit_token_decl(TokenDef* tok, std::ostream& out) {
    // Capturing version + no-arg discard overload so call sites that
    // don't care about the matched span (pure-recognizer use, e.g.
    // keywords) can write `tok()` instead of constructing a throwaway.
    // The no-arg overload is defined in-class so it's implicitly inline
    // and the body is visible to call sites at any -O level.
    out << "    bool " << tok->name << "(peg::Span& out);\n";
    out << "    bool " << tok->name << "() { peg::Span _d; return "
        << tok->name << "(_d); }\n";
}

void ParserEmitter::emit_token_method_impls(Grammar* grammar, std::ostream& out) {
    for (auto* t : grammar->tokens) {
        emit_token_impl(t, out);
    }
}

void ParserEmitter::emit_token_impl(TokenDef* tok, std::ostream& out) {
    ctx_.var_counter = 0;

    out << "bool Parser::" << tok->name << "(peg::Span& out) {\n";
    out << "    const char* _start = pos();\n";

    ctx_.in_lex_mode = true;
    cpp_emit::cg_expr(&ctx_, tok->body, out, 1);
    ctx_.in_lex_mode = false;

    out << "    out.ptr = _start; out.len = static_cast<int>(pos() - _start);\n";
    out << "    return true;\n";
    out << "}\n\n";
}

void ParserEmitter::emit_skip_setup(Grammar* grammar, std::ostream& out) {
    out << "void Parser::setup_skip() {\n";

    // Whitespace function
    if (grammar->has_ignore && grammar->ignore.has_value()) {
        out << "    set_whitespace([](char c) -> bool {\n";
        out << "        return ";
        cpp_emit::cg_charset_predicate(*grammar->ignore, out);
        out << ";\n";
        out << "    });\n";
    }

    // Comment specs
    if (!grammar->comments.empty()) {
        out << "    set_comments({";
        for (size_t i = 0; i < grammar->comments.size(); i++) {
            if (i > 0) out << ", ";
            auto* c = grammar->comments[i];
            out << "{\"" << cpp_emit::escape_string(c->open) << "\", \""
                << cpp_emit::escape_string(c->close) << "\", "
                << (c->nested ? "true" : "false") << ", "
                << (c->structured ? "true" : "false") << "}";
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
    ctx_.var_counter = 0;

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
    cpp_emit::cg_expr(&ctx_, prod->body, out, 1);

    out << "    return true;\n";
    out << "}\n\n";
}

} // namespace pegc
