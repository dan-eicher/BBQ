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
#include "CTypeEmitter.h"
#include "CReaderEmitter.h"
#include "CWriterEmitter.h"

using namespace BBQ;
using namespace bbqgen;

static void usage() {
    fprintf(stderr,
        "Usage: bbqc [options] format.bbq\n"
        "  -o <dir>          Output directory (default: .)\n"
        "  -frames <dir>     Frame file directory (default: ./frames)\n"
        "  -prefix <name>    Generated file prefix (default: from input)\n"
        "  -namespace <ns>   C++ namespace for generated code\n"
        "  --lang <c|cpp>    Output language (default: cpp)\n"
        "  -trace <flags>    Debug: A(ST) T(ypes) P(arser) S(ema)\n");
}

int main(int argc, char* argv[]) {
    std::string input_file;
    std::string output_dir = ".";
    std::string frame_dir = "";
    std::string prefix;
    std::string ns;
    std::string trace;
    std::string lang = "cpp";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            frame_dir = argv[++i];
        } else if (strcmp(argv[i], "-prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
        } else if (strcmp(argv[i], "-namespace") == 0 && i + 1 < argc) {
            ns = argv[++i];
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            lang = argv[++i];
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

    // --- C backend ---
    if (lang == "c") {
        bbqgen_c::CTypeEmitter ctypes(sema);

        std::string types_name = bbqgen_c::to_snake_case(prefix + "Types");
        std::string reader_name = bbqgen_c::to_snake_case(prefix + "Reader");
        std::string writer_name = bbqgen_c::to_snake_case(prefix + "Writer");

        // Types
        {
            std::string types_frame = frame_dir + "/Types.frame";
            std::string types_file = output_dir + "/" + types_name + ".h";
            std::ofstream out(types_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", types_file.c_str()); return 1; }
            if (!ctypes.emit_to_frame(grammar, types_frame, out)) {
                fprintf(stderr, "bbqc: failed to process frame %s\n", types_frame.c_str());
                return 1;
            }
            if (trace.find('T') != std::string::npos)
                fprintf(stderr, "[trace] Wrote %s\n", types_file.c_str());
        }

        std::string types_include = types_name + ".h";

        // Reader
        bbqgen_c::CReaderEmitter reader(sema, ctypes);
        {
            std::string hdr_frame = frame_dir + "/ReaderHeader.frame";
            std::string hdr_file = output_dir + "/" + reader_name + ".h";
            std::ofstream out(hdr_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", hdr_file.c_str()); return 1; }
            if (!reader.emit_header_to_frame(grammar, hdr_frame, types_include, out)) {
                fprintf(stderr, "bbqc: failed to process frame %s\n", hdr_frame.c_str());
                return 1;
            }
        }
        {
            std::string impl_frame = frame_dir + "/ReaderImpl.frame";
            std::string impl_file = output_dir + "/" + reader_name + ".c";
            std::ofstream out(impl_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", impl_file.c_str()); return 1; }
            std::string reader_include = reader_name + ".h";
            if (!reader.emit_impl_to_frame(grammar, impl_frame, reader_include, out)) {
                fprintf(stderr, "bbqc: failed to process frame %s\n", impl_frame.c_str());
                return 1;
            }
        }

        // Writer
        bbqgen_c::CWriterEmitter writer(sema, ctypes);
        {
            std::string hdr_frame = frame_dir + "/WriterHeader.frame";
            std::string hdr_file = output_dir + "/" + writer_name + ".h";
            std::ofstream out(hdr_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", hdr_file.c_str()); return 1; }
            if (!writer.emit_header_to_frame(grammar, hdr_frame, types_include, out)) {
                fprintf(stderr, "bbqc: failed to process frame %s\n", hdr_frame.c_str());
                return 1;
            }
        }
        {
            std::string impl_frame = frame_dir + "/WriterImpl.frame";
            std::string impl_file = output_dir + "/" + writer_name + ".c";
            std::ofstream out(impl_file);
            if (!out) { fprintf(stderr, "bbqc: cannot open %s\n", impl_file.c_str()); return 1; }
            std::string writer_include = writer_name + ".h";
            if (!writer.emit_impl_to_frame(grammar, impl_frame, writer_include, out)) {
                fprintf(stderr, "bbqc: failed to process frame %s\n", impl_frame.c_str());
                return 1;
            }
        }

        fprintf(stderr, "bbqc: generated %s.h, %s.h/.c, %s.h/.c\n",
            types_name.c_str(), reader_name.c_str(), writer_name.c_str());
        return 0;
    }

    // --- C++ backend (default) ---
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
