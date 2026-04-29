// ddcgc — DDCG compiler-compiler — CLI driver.
//
// Pipeline:
//   1. Parse the .ddcg via the pegc-generated parser
//   2. Load asdl JSON sidecars for source AST + target IR
//   3. Type-check (typecheck.cpp)
//   4. If clean, emit C++ via cpp_backend.cpp
//
// Mirrors the burgc/src/main.cpp arg-parsing style.

#include "Parser.h"
#include "schema.h"
#include "typecheck.h"
#include "backend.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static void usage() {
    std::fprintf(stderr,
        "Usage: ddcgc -i <input.ddcg> -ast <ast.json> -ir <ir.json>\n"
        "             [-o <output.cpp>]\n"
        "  -i <file>          Input .ddcg file\n"
        "  -ast <file.json>   asdl JSON for the source AST (matched against\n"
        "                     the .ddcg's source-import name)\n"
        "  -ir  <file.json>   asdl JSON for the target IR (matched against\n"
        "                     the .ddcg's target-import name)\n"
        "  -o   <file>        Output .cpp file (default: stdout)\n"
        "  -h                 Show this help message\n");
}

static bool read_file(const std::string& path, std::string& out_contents,
                       std::string& err_out) {
    std::ifstream f(path);
    if (!f) {
        err_out = "cannot open: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out_contents = ss.str();
    return true;
}

int main(int argc, char** argv) {
    std::string input_file;
    std::string ast_json_file;
    std::string ir_json_file;
    std::string output_file;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (std::strcmp(argv[i], "-ast") == 0 && i + 1 < argc) {
            ast_json_file = argv[++i];
        } else if (std::strcmp(argv[i], "-ir") == 0 && i + 1 < argc) {
            ir_json_file = argv[++i];
        } else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    if (input_file.empty() || ast_json_file.empty() || ir_json_file.empty()) {
        usage();
        return 1;
    }

    // 1. Read + parse .ddcg
    std::string src;
    {
        std::string err;
        if (!read_file(input_file, src, err)) {
            std::fprintf(stderr, "ddcgc: %s\n", err.c_str());
            return 1;
        }
    }
    Parser parser;
    parser.init(src.data(), static_cast<int>(src.size()));
    if (!parser.parse() || !parser.ast) {
        std::fprintf(stderr, "ddcgc: parse error in %s\n", input_file.c_str());
        return 1;
    }
    DdcgAst::File* file = parser.ast;

    // 2. Load schemas, keyed by the .ddcg's IMPORTS names.
    //    The .ddcg's first import is treated as the source AST, second as
    //    the target IR — ddcgc CLI matches them positionally to -ast/-ir.
    std::map<std::string, ddcgc::Schema> schemas;
    if (file->imports.size() < 2) {
        std::fprintf(stderr, "ddcgc: %s must declare at least two imports "
                     "(source AST + target IR)\n", input_file.c_str());
        return 1;
    }
    {
        std::string err;
        auto s = ddcgc::load_asdl_schema(ast_json_file, err);
        if (!s) { std::fprintf(stderr, "ddcgc: %s\n", err.c_str()); return 1; }
        schemas[file->imports[0]->name] = std::move(*s);
    }
    {
        std::string err;
        auto s = ddcgc::load_asdl_schema(ir_json_file, err);
        if (!s) { std::fprintf(stderr, "ddcgc: %s\n", err.c_str()); return 1; }
        schemas[file->imports[1]->name] = std::move(*s);
    }

    // 3. Type-check
    auto check = ddcgc::check_file(file, schemas);
    for (const auto& w : check.warnings) {
        std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    }
    for (const auto& e : check.errors) {
        std::fprintf(stderr, "error: %s\n", e.message.c_str());
    }
    if (!check.ok()) return 1;

    // 4. Emit C++
    std::ostream* out = &std::cout;
    std::ofstream out_file;
    if (!output_file.empty()) {
        out_file.open(output_file);
        if (!out_file) {
            std::fprintf(stderr, "ddcgc: cannot write %s\n", output_file.c_str());
            return 1;
        }
        out = &out_file;
    }
    auto backend = ddcgc::create_cpp_backend();
    std::vector<std::string> emit_errs;
    if (!backend->emit(file, schemas, check, *out, emit_errs)) {
        for (const auto& e : emit_errs) std::fprintf(stderr, "emit error: %s\n", e.c_str());
        return 1;
    }
    return 0;
}
