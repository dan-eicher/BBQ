#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "Parser.h"
#include "GrammarAST.h"
#include "PegAnalysis.h"
#include "ParserEmitter.h"

using namespace PegGrammar;
using namespace pegc;

static void usage() {
    fprintf(stderr,
        "Usage: pegc [options] grammar.peg\n"
        "  -o <dir>          Output directory (default: .)\n"
        "  -frames <dir>     Frame file directory\n"
        "  -prefix <name>    Generated file prefix (default: from grammar name)\n"
        "  -namespace <ns>   C++ namespace for generated code\n"
        "  -trace <flags>    Debug: A(ST) P(arser) N(analysis)\n");
}

int main(int argc, char* argv[]) {
    std::string input_file;
    std::string output_dir = ".";
    std::string frame_dir;
    std::string prefix;
    bool prefix_set = false;
    std::string ns;
    std::string trace;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            frame_dir = argv[++i];
        } else if (strcmp(argv[i], "-prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
            prefix_set = true;
        } else if (strcmp(argv[i], "-namespace") == 0 && i + 1 < argc) {
            ns = argv[++i];
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
    if (!prefix_set && prefix.empty()) {
        size_t slash = input_file.find_last_of("/\\");
        std::string base = (slash != std::string::npos)
            ? input_file.substr(slash + 1) : input_file;
        size_t dot = base.find_last_of('.');
        prefix = (dot != std::string::npos) ? base.substr(0, dot) : base;
    }

    // Default frame dir: next to the executable's source, in backend/frames/
    if (frame_dir.empty()) {
        frame_dir = "./frames";
    }

    // --- Parse ---
    std::ifstream ifs(input_file, std::ios::binary);
    if (!ifs) {
        fprintf(stderr, "pegc: cannot open %s\n", input_file.c_str());
        return 1;
    }
    std::string src((std::istreambuf_iterator<char>(ifs)), {});
    ifs.close();

    Parser parser;
    parser.init(src.data(), static_cast<int>(src.size()));
    if (!parser.parse()) {
        for (auto& e : parser.errors())
            fprintf(stderr, "%s:%d:%d: %s\n", input_file.c_str(), e.line, e.col, e.message.c_str());
        if (parser.errors().empty())
            fprintf(stderr, "pegc: parse failed\n");
        return 1;
    }

    Grammar* grammar = parser.ast;

    // Trace: AST
    if (trace.find('A') != std::string::npos) {
        fprintf(stderr, "[trace] Grammar: %s\n", grammar->name.c_str());
        fprintf(stderr, "[trace]   %zu headers, %zu charsets, %zu tokens, "
                        "%zu comments, %zu productions\n",
            grammar->headers.size(), grammar->charsets.size(),
            grammar->tokens.size(), grammar->comments.size(),
            grammar->productions.size());
        for (auto* p : grammar->productions)
            fprintf(stderr, "  production: %s (%zu params)\n",
                p->name.c_str(), p->params.size());
    }

    // --- PEG Analysis ---
    PegAnalysis analysis;
    auto result = analysis.analyze(grammar);

    // Print warnings
    for (auto& w : result.warnings) {
        fprintf(stderr, "pegc: warning: %s\n", w.c_str());
    }

    if (!result.ok()) {
        for (auto& e : result.errors) {
            fprintf(stderr, "pegc: error: %s\n", e.c_str());
        }
        return 1;
    }

    if (trace.find('N') != std::string::npos) {
        fprintf(stderr, "[trace] Analysis: %zu productions, %zu topo-ordered\n",
            result.productions.size(), result.topo_order.size());
        for (auto& [name, info] : result.productions) {
            fprintf(stderr, "  %s: first_testable=%d, %zu deps\n",
                name.c_str(), info.first_testable, info.calls.size());
        }
    }

    // --- Code Generation ---
    ParserEmitter emitter(result, ns);

    std::string parser_include = prefix + "Parser.h";

    // Emit header
    {
        std::string hdr_frame = frame_dir + "/Parser.frame";
        std::string hdr_file = output_dir + "/" + prefix + "Parser.h";
        std::ofstream out(hdr_file);
        if (!out) {
            fprintf(stderr, "pegc: cannot open %s for writing\n", hdr_file.c_str());
            return 1;
        }
        if (!emitter.emit_header_to_frame(grammar, hdr_frame, out)) {
            fprintf(stderr, "pegc: failed to process frame file %s\n", hdr_frame.c_str());
            return 1;
        }
        if (trace.find('P') != std::string::npos)
            fprintf(stderr, "[trace] Wrote %s\n", hdr_file.c_str());
    }

    // Emit implementation
    {
        std::string impl_frame = frame_dir + "/ParserImpl.frame";
        std::string impl_file = output_dir + "/" + prefix + "Parser.cpp";
        std::ofstream out(impl_file);
        if (!out) {
            fprintf(stderr, "pegc: cannot open %s for writing\n", impl_file.c_str());
            return 1;
        }
        if (!emitter.emit_impl_to_frame(grammar, impl_frame, parser_include, out)) {
            fprintf(stderr, "pegc: failed to process frame file %s\n", impl_frame.c_str());
            return 1;
        }
        if (trace.find('P') != std::string::npos)
            fprintf(stderr, "[trace] Wrote %s\n", impl_file.c_str());
    }

    fprintf(stderr, "pegc: generated %sParser.h, %sParser.cpp\n",
        prefix.c_str(), prefix.c_str());

    return 0;
}
