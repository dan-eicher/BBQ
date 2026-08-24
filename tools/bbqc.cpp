#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "Parser.h"
#include "BBQ_AST.h"
#include "Sema.h"
#include "Names.h"
#include "RenderTypes.h"
#include "CompilerCtx.h"
#include "RenderEmit.h"
#include "bbq_compile.h"
#include <stdexcept>

using namespace BBQ;
using namespace bbqgen;

static void usage() {
    fprintf(stderr,
        "Usage: bbqc [options] format.bbq\n"
        "  -o <dir>          Output directory (default: .)\n"
        "  -frames <dir>     Frame file directory (default: ./frames)\n"
        "  -templates <dir>  Render-template directory (default: ./templates)\n"
        "  -prefix <name>    Generated file prefix (default: from input)\n"
        "  -namespace <ns>   C++ namespace for generated code\n"
        "  -backend <c|cpp|c-lite>  Output backend (default: cpp)\n"
        "                    c = owning C (types+reader+writer); cpp = C++ ZCow\n"
        "                    (types+reader+writer); c-lite = read-only C view reader\n"
        "  -trace <flags>    Debug: A(ST) T(ypes) P(arser) S(ema)\n");
}

int main(int argc, char* argv[]) {
    std::string input_file;
    std::string output_dir = ".";
    std::string frame_dir = "";
    std::string templates_dir = "";
    std::string prefix;
    std::string ns;
    std::string trace;
    std::string backend = "cpp";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            frame_dir = argv[++i];
        } else if (strcmp(argv[i], "-templates") == 0 && i + 1 < argc) {
            templates_dir = argv[++i];
        } else if (strcmp(argv[i], "-prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
        } else if (strcmp(argv[i], "-namespace") == 0 && i + 1 < argc) {
            ns = argv[++i];
        } else if (strcmp(argv[i], "-backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (strcmp(argv[i], "-trace") == 0 && i + 1 < argc) {
            trace = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage();
            return 1;
        } else {
            input_file = argv[i];
        }
    }

    if (input_file.empty()) {
        usage();
        return 1;
    }

    // Derive prefix from input filename if not specified
    if (prefix.empty()) {
        size_t slash = input_file.find_last_of("/\\");
        std::string base = (slash != std::string::npos)
            ? input_file.substr(slash + 1) : input_file;
        size_t dot = base.find_last_of('.');
        prefix = (dot != std::string::npos) ? base.substr(0, dot) : base;
    }

    // Default frame dir
    if (frame_dir.empty()) {
        frame_dir = "./frames";
    }
    // Default render-template dir (the C types header is rendered, not framed).
    if (templates_dir.empty()) {
        templates_dir = "./templates";
    }

    // --- Parse ---
    std::ifstream file(input_file);
    if (!file) {
        fprintf(stderr, "bbqc: cannot open %s\n", input_file.c_str());
        return 1;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    Parser parser;
    parser.init(content.data(), static_cast<int>(content.size()));
    if (!parser.parse()) {
        fprintf(stderr, "bbqc: parse error(s)\n");
        for (auto& e : parser.errors())
            fprintf(stderr, "  line %d, col %d: %s\n", e.line, e.col, e.message.c_str());
        return 1;
    }

    Grammar* grammar = parser.ast;

    if (trace.find('A') != std::string::npos) {
        fprintf(stderr, "[trace] AST: %zu directives, %zu rules\n",
            grammar->directives.size(), grammar->rules.size());
        for (auto* r : grammar->rules)
            fprintf(stderr, "  rule: %s\n", r->name.c_str());
    }

    // --- Semantic Analysis ---
    ErrorReporter errors;
    Sema sema(errors);
    if (!sema.analyze(grammar)) {
        errors.print_all(std::cerr);
        return 1;
    }
    for (auto& e : errors.errors()) {
        if (e.is_warning) {
            std::cerr << "warning";
            if (e.loc.line > 0)
                std::cerr << " (line " << e.loc.line << ", col " << e.loc.column << ")";
            std::cerr << ": " << e.message << "\n";
        }
    }

    if (trace.find('S') != std::string::npos) {
        fprintf(stderr, "[trace] Sema: %zu rules validated, default endian=%s\n",
            sema.sorted_rules().size(),
            sema.default_endian() == Endianness::Big ? "big" : "little");
    }

    // --- c-lite backend: read-only C view reader (zero-copy span index) ---
    if (backend == "c-lite") {
        bbq::render::CompilerCtx ctx{grammar, &sema, nullptr, prefix};
        ::bbq::Compiler cek_compiler;
        auto* cek_g = cek_compiler.compile_grammar(grammar);
        if (!cek_g) { fprintf(stderr, "bbqc: failed to compile kont graph\n"); return 1; }
        ctx.ir = cek_g;

        std::string reader_name = bbqgen::to_snake_case(prefix + "Reader");
        std::string guard = reader_name + "_H";
        for (auto& c : guard) c = (char)toupper((unsigned char)c);
        {
            std::string hdr_file = output_dir + "/" + reader_name + ".h";
            std::ofstream out(hdr_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", hdr_file.c_str()); return 1; }
            out << "/* Generated by BBQ (c-lite backend) — do not edit */\n";
            out << "#ifndef " << guard << "\n#define " << guard << "\n\n";
            out << "#include \"bbq_lite.h\"\n\n";
            out << bbq::render::render_reader_view_c_decls(ctx);
            out << "\n#endif /* " << guard << " */\n";
        }
        {
            std::string impl_file = output_dir + "/" + reader_name + ".c";
            std::ofstream out(impl_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", impl_file.c_str()); return 1; }
            out << "/* Generated by BBQ (c-lite backend) — do not edit */\n";
            out << "#include \"" << reader_name << ".h\"\n\n";
            out << bbq::render::render_reader_view_c(ctx, templates_dir);
        }
        fprintf(stderr, "bbqc: generated %s.h/.c (c-lite view reader)\n", reader_name.c_str());
        return 0;
    }

    // --- C backend ---
    if (backend == "c") {
        // The compiler context — the shared, accumulating home the backend phases
        // consume (ir is filled in once the kont graph is compiled, below).
        bbq::render::CompilerCtx ctx{grammar, &sema, nullptr, prefix};

        std::string types_name = bbqgen::to_snake_case(prefix + "Types");
        std::string reader_name = bbqgen::to_snake_case(prefix + "Reader");
        std::string writer_name = bbqgen::to_snake_case(prefix + "Writer");

        // Types — destination-driven render backend (flat type model -> inja).
        {
            std::string types_tmpl = templates_dir + "/types_c.inja";
            std::string types_file = output_dir + "/" + types_name + ".h";
            std::ofstream out(types_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", types_file.c_str()); return 1; }
            out << bbq::render::render_types_c(ctx, types_tmpl);
            if (trace.find('T') != std::string::npos)
                fprintf(stderr, "[trace] Wrote %s\n", types_file.c_str());
        }

        std::string types_include = types_name + ".h";

        // Reader — destination-driven render backend (kont graph -> op-list -> inja).
        ::bbq::Compiler cek_compiler;
        auto* cek_g = cek_compiler.compile_grammar(grammar);
        if (!cek_g) { fprintf(stderr, "bbqc: failed to compile kont graph\n"); return 1; }
        ctx.ir = cek_g;  // accumulate: the IR is now available to the reader phases
        std::string reader_guard = reader_name + "_H";
        for (auto& c : reader_guard) c = (char)toupper((unsigned char)c);
        {
            std::string hdr_file = output_dir + "/" + reader_name + ".h";
            std::ofstream out(hdr_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", hdr_file.c_str()); return 1; }
            out << "/* Generated by BBQ (C backend) — do not edit */\n";
            out << "#ifndef " << reader_guard << "\n#define " << reader_guard << "\n\n";
            out << "#include \"" << types_include << "\"\n#include \"bbq_runtime.h\"\n\n";
            out << bbq::render::render_reader_decls(ctx);
            out << "\n#endif /* " << reader_guard << " */\n";
        }
        {
            std::string impl_file = output_dir + "/" + reader_name + ".c";
            std::ofstream out(impl_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", impl_file.c_str()); return 1; }
            out << "/* Generated by BBQ (C backend) — do not edit */\n";
            out << "#include \"" << reader_name << ".h\"\n#include <stdlib.h>\n#include <string.h>\n\n";
            out << bbq::render::render_reader_c(ctx, templates_dir);
        }

        // Writer — the burg-driven OwningCWriter, dual of the reader over the same
        // lowering (no frames, no AST walk): header is the per-rule decls, impl is the
        // write stencils (+ any @writer user blocks appended by the emitter).
        std::string writer_guard = writer_name + "_H";
        for (auto& c : writer_guard) c = (char)toupper((unsigned char)c);
        {
            std::string hdr_file = output_dir + "/" + writer_name + ".h";
            std::ofstream out(hdr_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", hdr_file.c_str()); return 1; }
            out << "/* Generated by BBQ (C backend) — do not edit */\n";
            out << "#ifndef " << writer_guard << "\n#define " << writer_guard << "\n\n";
            out << "#include \"" << types_include << "\"\n#include \"bbq_runtime.h\"\n\n";
            out << bbq::render::render_writer_decls(ctx);
            out << "\n#endif /* " << writer_guard << " */\n";
        }
        {
            std::string impl_file = output_dir + "/" + writer_name + ".c";
            std::ofstream out(impl_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", impl_file.c_str()); return 1; }
            out << "/* Generated by BBQ (C backend) — do not edit */\n";
            out << "#include \"" << writer_name << ".h\"\n#include <string.h>\n\n";
            out << bbq::render::render_writer_c(ctx, templates_dir);
        }

        fprintf(stderr, "bbqc: generated %s.h, %s.h/.c, %s.h/.c\n",
            types_name.c_str(), reader_name.c_str(), writer_name.c_str());
        return 0;
    }

    if (backend != "cpp") {
        fprintf(stderr, "bbqc: unknown -backend '%s' (expected c|cpp|c-lite)\n", backend.c_str());
        return 1;
    }

    // --- C++ backend (default) — destination-driven render backend ---
    // Zero-copy typed handles over the shared index runtime (Types.h), plus the
    // cpp-zcow view reader (Reader.h) — the burg ZCow profile over the kont IR, the
    // sibling of the C reader. Unsupported constructs are rejected by the Sema-style
    // gate (a thrown error), never silently dropped.
    {
        std::string types_tmpl   = templates_dir + "/types_cpp.inja";
        std::string types_name   = prefix + "Types.h";
        try {
            // C++ types need no IR (they read the index shape); CamelCase, no prefix.
            bbq::render::CompilerCtx tctx{grammar, &sema, nullptr, ""};
            std::ofstream out(output_dir + "/" + types_name);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", types_name.c_str()); return 1; }
            out << bbq::render::render_types_cpp(tctx, types_tmpl, ns);
        } catch (const std::exception& e) {
            fprintf(stderr, "bbqc: %s\n", e.what());
            return 1;
        }
        fprintf(stderr, "bbqc: generated %s\n", types_name.c_str());
        // The cpp-zcow (view) parser — the index-builder, the burg ZCow profile over
        // the kont IR, sibling of the C reader. Incomplete features surface as a thrown
        // gate error, never hidden behind a fallback.
        std::string reader_name = prefix + "Reader.h";
        try {
            ::bbq::Compiler cek_compiler;
            auto* cek_g = cek_compiler.compile_grammar(grammar);
            if (!cek_g) { fprintf(stderr, "bbqc: compile_grammar failed\n"); return 1; }
            bbq::render::CompilerCtx ctx{grammar, &sema, cek_g, ""};
            std::ofstream rout(output_dir + "/" + reader_name);
            if (!rout) { fprintf(stderr, "bbqc: cannot open %s\n", reader_name.c_str()); return 1; }
            rout << bbq::render::render_reader_view(ctx, templates_dir, ns);
        } catch (const std::exception& e) {
            fprintf(stderr, "bbqc: %s\n", e.what());
            return 1;
        }
        fprintf(stderr, "bbqc: generated %s\n", reader_name.c_str());
        // No writer is generated for this backend. The reader builds a bbq::zcow
        // document; editing and serializing are the document's own, dependent fields
        // included — the parser records what determines them, so nothing downstream
        // has to re-derive the grammar to write it back.
        return 0;
    }
}
