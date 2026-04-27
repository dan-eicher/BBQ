#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "Parser.h"
#include "GrammarAST.h"
#include "PegAnalysis.h"
#include "ParserEmitter.h"
#include "CParserEmitter.h"

using namespace PegGrammar;
using namespace pegc;

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

static void usage() {
    fprintf(stderr,
        "Usage: pegc [options] grammar.peg\n"
        "  -o <dir>          Output directory (default: .)\n"
        "  -frames <dir>     Frame file directory\n"
        "  -prefix <name>    Generated file prefix (default: from grammar name)\n"
        "  -namespace <ns>   C++ namespace for generated code\n"
        "  -lang c|c++       Target language (default: c++)\n"
        "  -trace <flags>    Debug: A(ST) P(arser) N(analysis)\n"
        "  --strict          Treat analysis warnings as errors (CI gating)\n"
        "  --coverage        Enable shape-enumeration warnings (ordered-choice\n"
        "                    masking, Star/Plus + suffix conflicts). Off by\n"
        "                    default — these fire on legitimate shared-prefix\n"
        "                    alternatives in real grammars.\n");
}

int main(int argc, char* argv[]) {
    std::string input_file;
    std::string output_dir = ".";
    std::string frame_dir;
    std::string prefix;
    bool prefix_set = false;
    std::string ns;
    std::string lang = "c++";
    std::string trace;
    bool strict = false;
    bool coverage = false;

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
        } else if (strcmp(argv[i], "-lang") == 0 && i + 1 < argc) {
            lang = argv[++i];
        } else if (strcmp(argv[i], "-trace") == 0 && i + 1 < argc) {
            trace = argv[++i];
        } else if (strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else if (strcmp(argv[i], "--coverage") == 0) {
            coverage = true;
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
    PegAnalysisConfig acfg;
    acfg.emit_coverage_warnings = coverage;
    auto result = analysis.analyze(grammar, acfg);

    // Print warnings; --strict promotes them to errors so CI can gate
    // on them.
    const char* warn_prefix = strict ? "error" : "warning";
    for (auto& w : result.warnings) {
        fprintf(stderr, "pegc: %s: %s\n", warn_prefix, w.c_str());
    }

    if (!result.ok()) {
        for (auto& e : result.errors) {
            fprintf(stderr, "pegc: error: %s\n", e.c_str());
        }
        return 1;
    }
    if (strict && !result.warnings.empty()) {
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
    bool c_mode = (lang == "c");

    // In C mode, snake_case the prefix for filenames and function names
    std::string c_prefix;
    if (c_mode) {
        c_prefix = to_snake_case(prefix);
        if (!c_prefix.empty() && c_prefix.back() != '_')
            c_prefix += '_';
    }

    std::string impl_ext = c_mode ? ".c" : ".cpp";
    std::string hdr_frame_name = c_mode ? "CParser.frame" : "Parser.frame";
    std::string impl_frame_name = c_mode ? "CParserImpl.frame" : "ParserImpl.frame";

    std::string file_prefix = c_mode ? c_prefix + "parser" : prefix + "Parser";
    std::string parser_include = file_prefix + ".h";

    // Emit header
    {
        std::string hdr_frame = frame_dir + "/" + hdr_frame_name;
        std::string hdr_file = output_dir + "/" + file_prefix + ".h";
        std::ofstream out(hdr_file);
        if (!out) {
            fprintf(stderr, "pegc: cannot open %s for writing\n", hdr_file.c_str());
            return 1;
        }
        bool ok;
        if (c_mode) {
            CParserEmitter emitter(result, c_prefix);
            ok = emitter.emit_header_to_frame(grammar, hdr_frame, out);
        } else {
            ParserEmitter emitter(result, ns);
            ok = emitter.emit_header_to_frame(grammar, hdr_frame, out);
        }
        if (!ok) {
            fprintf(stderr, "pegc: failed to process frame file %s\n", hdr_frame.c_str());
            return 1;
        }
        if (trace.find('P') != std::string::npos)
            fprintf(stderr, "[trace] Wrote %s\n", hdr_file.c_str());
    }

    // Emit implementation
    {
        std::string impl_frame = frame_dir + "/" + impl_frame_name;
        std::string impl_file = output_dir + "/" + file_prefix + impl_ext;
        std::ofstream out(impl_file);
        if (!out) {
            fprintf(stderr, "pegc: cannot open %s for writing\n", impl_file.c_str());
            return 1;
        }
        bool ok;
        if (c_mode) {
            CParserEmitter emitter(result, c_prefix);
            ok = emitter.emit_impl_to_frame(grammar, impl_frame, parser_include, out);
        } else {
            ParserEmitter emitter(result, ns);
            ok = emitter.emit_impl_to_frame(grammar, impl_frame, parser_include, out);
        }
        if (!ok) {
            fprintf(stderr, "pegc: failed to process frame file %s\n", impl_frame.c_str());
            return 1;
        }
        if (trace.find('P') != std::string::npos)
            fprintf(stderr, "[trace] Wrote %s\n", impl_file.c_str());
    }

    fprintf(stderr, "pegc: generated %s.h, %s%s\n",
        file_prefix.c_str(), file_prefix.c_str(), impl_ext.c_str());

    return 0;
}
