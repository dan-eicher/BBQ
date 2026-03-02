#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "Parser.h"
#include "BBQ_AST.h"
#include "Sema.h"
#include "TypeEmitter.h"
#include "ParserEmitter.h"

using namespace BBQ;
using namespace bbqgen;

static void usage() {
    fprintf(stderr,
        "Usage: bbqc [options] format.bbq\n"
        "  -o <dir>          Output directory (default: .)\n"
        "  -frames <dir>     Frame file directory (default: ./frames)\n"
        "  -prefix <name>    Generated file prefix (default: from input)\n"
        "  -namespace <ns>   C++ namespace for generated code\n"
        "  -trace <flags>    Debug: A(ST) T(ypes) P(arser) S(ema)\n");
}

int main(int argc, char* argv[]) {
    std::string input_file;
    std::string output_dir = ".";
    std::string frame_dir = "";
    std::string prefix;
    std::string ns;
    std::string trace;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            frame_dir = argv[++i];
        } else if (strcmp(argv[i], "-prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
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
    if (prefix.empty()) {
        size_t slash = input_file.find_last_of("/\\");
        std::string base = (slash != std::string::npos)
            ? input_file.substr(slash + 1) : input_file;
        size_t dot = base.find_last_of('.');
        prefix = (dot != std::string::npos) ? base.substr(0, dot) : base;
    }

    // Default frame dir: next to the executable, or ./frames
    if (frame_dir.empty()) {
        frame_dir = "./frames";
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

    // Trace: AST
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
    // Print warnings even on success
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

    // --- Type Generation ---
    TypeEmitter types(sema, ns);
    {
        std::string types_frame = frame_dir + "/Types.frame";
        std::string types_file = output_dir + "/" + prefix + "Types.h";
        std::ofstream out(types_file);
        if (!out) {
            fprintf(stderr, "bbqc: cannot open %s for writing\n", types_file.c_str());
            return 1;
        }
        if (!types.emit_to_frame(grammar, types_frame, out)) {
            fprintf(stderr, "bbqc: failed to process frame file %s\n", types_frame.c_str());
            return 1;
        }
        if (trace.find('T') != std::string::npos)
            fprintf(stderr, "[trace] Wrote %s\n", types_file.c_str());
    }

    // --- Parser Generation ---
    ParserEmitter pemit(sema, types, ns);

    std::string types_include = prefix + "Types.h";
    std::string parser_include = prefix + "Parser.h";

    {
        std::string hdr_frame = frame_dir + "/ParserHeader.frame";
        std::string hdr_file = output_dir + "/" + prefix + "Parser.h";
        std::ofstream out(hdr_file);
        if (!out) {
            fprintf(stderr, "bbqc: cannot open %s for writing\n", hdr_file.c_str());
            return 1;
        }
        if (!pemit.emit_header_to_frame(grammar, hdr_frame, types_include, out)) {
            fprintf(stderr, "bbqc: failed to process frame file %s\n", hdr_frame.c_str());
            return 1;
        }
        if (trace.find('P') != std::string::npos)
            fprintf(stderr, "[trace] Wrote %s\n", hdr_file.c_str());
    }
    {
        std::string impl_frame = frame_dir + "/ParserImpl.frame";
        std::string impl_file = output_dir + "/" + prefix + "Parser.cpp";
        std::ofstream out(impl_file);
        if (!out) {
            fprintf(stderr, "bbqc: cannot open %s for writing\n", impl_file.c_str());
            return 1;
        }
        if (!pemit.emit_impl_to_frame(grammar, impl_frame, parser_include, out)) {
            fprintf(stderr, "bbqc: failed to process frame file %s\n", impl_frame.c_str());
            return 1;
        }
        if (trace.find('P') != std::string::npos)
            fprintf(stderr, "[trace] Wrote %s\n", impl_file.c_str());
    }

    fprintf(stderr, "bbqc: generated %sTypes.h, %sParser.h, %sParser.cpp\n",
        prefix.c_str(), prefix.c_str(), prefix.c_str());

    return 0;
}
