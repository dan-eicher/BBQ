#include "JavaParserEmitter.h"
#include "FrameProcessor.h"

#include <sstream>

using namespace PegGrammar;

namespace pegc {

JavaParserEmitter::JavaParserEmitter(const AnalysisResult& analysis,
                                     const std::string& class_name,
                                     const std::string& package,
                                     const std::string& runtime_package)
    : analysis_(analysis), class_name_(class_name), package_(package),
      runtime_package_(runtime_package) {
}

// Populate the per-emission ctx with the grammar's name sets so cg_* functions
// can dispatch on charset / production / token references.
static void seed_ctx_from_grammar(java_emit::Ctx& ctx, Grammar* grammar) {
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

bool JavaParserEmitter::emit_to_frame(Grammar* grammar,
                                      const std::string& frame_path,
                                      std::ostream& out) {
    bbqgen::FrameProcessor fp(frame_path, out);
    if (!fp.ok()) return false;

    seed_ctx_from_grammar(ctx_, grammar);

    fp.CopyFramePart("package");
    if (!package_.empty()) out << "package " << package_ << ";\n";

    fp.CopyFramePart("headers");
    emit_headers(grammar, out);

    fp.CopyFramePart("class_open");
    out << "public class " << class_name_ << " {\n";

    fp.CopyFramePart("entry_points");
    emit_entry_points(grammar, out);

    fp.CopyFramePart("charsets");
    emit_charset_defs(grammar, out);
    emit_skip_setup(grammar, out);

    fp.CopyFramePart("parse_implementations");
    emit_token_impls(grammar, out);
    emit_production_impls(grammar, out);

    fp.CopyFramePart("class_close");
    out << "}\n";

    return true;
}

std::string JavaParserEmitter::emit(Grammar* grammar) {
    std::ostringstream out;
    seed_ctx_from_grammar(ctx_, grammar);

    if (!package_.empty()) out << "package " << package_ << ";\n\n";
    emit_headers(grammar, out);
    out << "public class " << class_name_ << " {\n";
    emit_entry_points(grammar, out);
    emit_charset_defs(grammar, out);
    emit_skip_setup(grammar, out);
    emit_token_impls(grammar, out);
    emit_production_impls(grammar, out);
    out << "}\n";
    return out.str();
}

// ═══════════════════════════════════════════════════════════════
// Emission
// ═══════════════════════════════════════════════════════════════

void JavaParserEmitter::emit_headers(Grammar* grammar, std::ostream& out) {
    // Every generated parser names PegCursor and Span — that pair is the
    // backend's runtime contract, the way peg_state and peg_span are the C
    // backend's. Which package supplies them is the consumer's choice, so the
    // import is emitted only when one was named, and never when the runtime
    // sits in the same package as the parser.
    if (!runtime_package_.empty() && runtime_package_ != package_) {
        out << "import " << runtime_package_ << ".PegCursor;\n";
        out << "import " << runtime_package_ << ".Span;\n\n";
    }

    // A grammar targeting Java puts its own imports in the header block, the
    // same place a C grammar puts its #includes.
    for (auto* h : grammar->headers) {
        out << h->code << "\n";
    }
}

// The cursor field, the constructor, and the start-production entry.
void JavaParserEmitter::emit_entry_points(Grammar* grammar, std::ostream& out) {
    // JLS 1.0 section 8.3.1.2 forbids a blank final, so the cursor field is a
    // plain field assigned in the constructor.
    out << "    private PegCursor p;\n\n";

    out << "    public " << class_name_ << "(char[] input) {\n";
    out << "        p = new PegCursor(input);\n";
    out << "        setupSkip();\n";
    out << "    }\n\n";

    out << "    public PegCursor cursor() {\n";
    out << "        return p;\n";
    out << "    }\n\n";

    out << "    public boolean parse() {\n";
    out << "        p.skip();\n";
    if (!grammar->productions.empty()) {
        auto* start = grammar->productions[0];
        if (start->params.empty()) {
            out << "        return parse_" << java_emit::to_snake_case(start->name) << "();\n";
        } else {
            // A parameterised start production cannot be called without values
            // the grammar author has to supply, so `parse()` is not the entry
            // point for it — call the production directly.
            out << "        return false;   // start production takes parameters:"
                << " call parse_" << java_emit::to_snake_case(start->name)
                << " directly\n";
        }
    } else {
        out << "        return true;\n";
    }
    out << "    }\n\n";
}

void JavaParserEmitter::emit_charset_defs(Grammar* grammar, std::ostream& out) {
    for (auto* cd : grammar->charsets) {
        out << "    private static boolean is_" << cd->name << "(char c) {\n";
        out << "        return ";
        java_emit::cg_charset_predicate(cd->body, out);
        out << ";\n";
        out << "    }\n\n";
    }
}

void JavaParserEmitter::emit_skip_setup(Grammar* grammar, std::ostream& out) {
    if (grammar->has_ignore && grammar->ignore.has_value()) {
        out << "    private static boolean ws_predicate(char c) {\n";
        out << "        return ";
        java_emit::cg_charset_predicate(*grammar->ignore, out);
        out << ";\n";
        out << "    }\n\n";
    }

    out << "    private void setupSkip() {\n";
    if (grammar->has_ignore && grammar->ignore.has_value()) {
        // Java 1.0 has no function values, so the C runtime's ws_fn pointer
        // becomes the tabulated form directly: the predicate is pure, so the
        // 256-entry table is exact and the skip loop stays a table lookup.
        out << "        for (int i = 0; i < 256; i++) {\n";
        out << "            p.setWhitespace((char) i, ws_predicate((char) i));\n";
        out << "        }\n";
    }
    for (auto* c : grammar->comments) {
        out << "        p.addComment(\"" << java_emit::escape_string(c->open)
            << "\", \"" << java_emit::escape_string(c->close) << "\", "
            << (c->nested ? "true" : "false") << ", "
            << (c->structured ? "true" : "false") << ");\n";
    }
    out << "    }\n\n";
}

// ── Token emission ────────────────────────────────────────────
// Tokens use the same peg_expr AST as productions; the only difference is
// emission mode (no auto-skip between atoms in lex mode).

void JavaParserEmitter::emit_token_impls(Grammar* grammar, std::ostream& out) {
    for (auto* t : grammar->tokens) {
        emit_token_impl(t, out);
    }
}

void JavaParserEmitter::emit_token_impl(TokenDef* tok, std::ostream& out) {
    ctx_.var_counter = 0;

    out << "    private boolean " << tok->name << "(Span out) {\n";
    out << "        int _start = p.save();\n";

    ctx_.in_lex_mode = true;
    java_emit::cg_expr(&ctx_, tok->body, out, 2);
    ctx_.in_lex_mode = false;

    // `out` is optional — pure-recognizer tokens (keywords, punctuation) don't
    // need the captured span at the call site.
    out << "        if (out != null) out.set(_start, p.save() - _start);\n";
    out << "        return true;\n";
    out << "    }\n\n";
}

void JavaParserEmitter::emit_production_impls(Grammar* grammar, std::ostream& out) {
    for (auto* p : grammar->productions) {
        emit_production_impl(p, out);
    }
}

void JavaParserEmitter::emit_production_impl(Production* prod, std::ostream& out) {
    ctx_.var_counter = 0;

    out << "    private boolean parse_" << java_emit::to_snake_case(prod->name) << "(";
    bool first = true;
    for (auto* param : prod->params) {
        if (!first) out << ", ";
        first = false;
        // `is_ref` is a C pointer concern with no Java analogue: a grammar
        // targeting Java returns through an object or a one-element array, so
        // the declared type is emitted exactly as written.
        out << param->type_code << " " << param->name;
    }
    out << ") {\n";

    if (prod->locals_code.has_value() && !prod->locals_code->empty()) {
        out << "        " << *prod->locals_code << "\n";
    }

    java_emit::cg_expr(&ctx_, prod->body, out, 2);

    out << "        return true;\n";
    out << "    }\n\n";
}

} // namespace pegc
