#include "generator.h"
#include "completeness.h"
#include <cstdio>
#include <fstream>
#include <cstring>
#include <iostream>

static void usage() {
    fprintf(stderr,
        "Usage: burgc -i <input.burg> [-o <output.h>] [-lang c++|c] [-frames <dir>]\n"
        "             [-asdl <file.json>] [--strict] [--coverage]\n"
        "  -i <file>          Input .burg file\n"
        "  -o <file>          Output file (.h for C++, .h+.c for C)\n"
        "  -lang c++|c        Target language (default: c++)\n"
        "  -frames <dir>      Frame template directory\n"
        "  --emit-rule-table  Also export the labeled rule set (nonterminal, pattern,\n"
        "                     cost, guard) as data, plus burg_rule_guard() to evaluate\n"
        "                     a rule's where-clause. Lets a consumer search the rules\n"
        "                     itself and check the labeler's chosen cover against an\n"
        "                     independent minimum. Off by default — nothing inside the\n"
        "                     matcher reads it.\n"
        "  -asdl <file.json>  ASDL JSON sidecar from `asdl --json`. When supplied,\n"
        "                     completeness analysis uses precise per-position\n"
        "                     demand from the IR's constructor schema instead of\n"
        "                     the heuristic derived from rule patterns alone.\n"
        "  --strict           Treat warnings as errors (CI gating).\n"
        "  --coverage         Run the full tree-automaton analysis: dead-rule\n"
        "                     warnings plus shape-enumeration warnings for\n"
        "                     uncovered tree shapes. Off by default — automaton\n"
        "                     construction is |states|^arity per round and\n"
        "                     dominates burgc runtime on large grammars.\n"
        "                     Reserve for a final CI gate; routine regenerations\n"
        "                     run codegen-only (duplicate-rule detection still\n"
        "                     runs without the automaton).\n");
}

int main(int argc, char** argv) {
    std::string input_file;
    std::string output_file;
    std::string lang = "c++";
    std::string frame_dir;
    std::string asdl_file;
    bool strict = false;
    bool coverage = false;
    bool rule_table = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-lang") == 0 && i + 1 < argc) {
            lang = argv[++i];
        } else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            frame_dir = argv[++i];
        } else if (strcmp(argv[i], "-asdl") == 0 && i + 1 < argc) {
            asdl_file = argv[++i];
        } else if (strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else if (strcmp(argv[i], "--coverage") == 0) {
            coverage = true;
        } else if (strcmp(argv[i], "--emit-rule-table") == 0) {
            rule_table = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    if (input_file.empty()) {
        usage();
        return 1;
    }

    BurgGenerator gen;
    AnalysisConfig acfg;
    acfg.emit_coverage_warnings = coverage;
    acfg.skip_dead_rule_analysis = !coverage;

    AsdlSchema schema;
    if (!asdl_file.empty()) {
        std::string err;
        if (!load_asdl_schema(asdl_file, schema, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        acfg.asdl = &schema;
    }
    gen.set_completeness_config(acfg);

    auto* spec = gen.parse(input_file);
    if (!spec) {
        for (auto& e : gen.errors())
            fprintf(stderr, "error: %s\n", e.c_str());
        return 1;
    }

    if (!gen.analyze(spec)) {
        for (auto& e : gen.errors())
            fprintf(stderr, "error: %s\n", e.c_str());
        return 1;
    }

    // Surface warnings; --strict treats them as fatal so CI can gate
    // on completeness regressions.
    const char* prefix = strict ? "error" : "warning";
    for (auto& w : gen.warnings())
        fprintf(stderr, "%s: %s\n", prefix, w.c_str());
    if (strict && !gen.warnings().empty())
        return 1;

    // Analysis-only mode: no codegen requested (no -o and no
    // -frames). Useful for `burgc -i foo.burg --coverage` style
    // checks where the user just wants the warnings.
    if (output_file.empty() && frame_dir.empty())
        return 0;

    std::unique_ptr<BurgBackend> backend;
    if (lang == "c")
        backend = create_c_backend();
    else if (lang == "c++")
        backend = create_cpp_backend();
    else {
        fprintf(stderr, "error: unknown language '%s' (use 'c++' or 'c')\n", lang.c_str());
        return 1;
    }
    backend->set_emit_rule_table(rule_table);

    if (backend->needs_impl_file()) {
        if (output_file.empty()) {
            fprintf(stderr, "error: -o required for C output\n");
            return 1;
        }
        std::string hdr_file = output_file;
        std::string impl_file = hdr_file;
        if (impl_file.size() > 2 && impl_file.substr(impl_file.size() - 2) == ".h")
            impl_file = impl_file.substr(0, impl_file.size() - 2) + ".c";
        else
            impl_file += ".c";

        std::string hdr_basename = hdr_file;
        size_t slash = hdr_basename.find_last_of("/\\");
        if (slash != std::string::npos) hdr_basename = hdr_basename.substr(slash + 1);

        std::ofstream hdr(hdr_file);
        std::ofstream imp(impl_file);
        if (!hdr || !imp) {
            fprintf(stderr, "error: cannot open output files\n");
            return 1;
        }
        backend->generate(hdr, imp, gen.analysis(), hdr_basename, frame_dir);
        fprintf(stderr, "burgc: generated %s, %s\n", hdr_file.c_str(), impl_file.c_str());
    } else if (output_file.empty()) {
        backend->generate(std::cout, gen.analysis(), frame_dir);
    } else {
        std::ofstream ofs(output_file);
        if (!ofs) {
            fprintf(stderr, "error: cannot open output file: %s\n", output_file.c_str());
            return 1;
        }
        backend->generate(ofs, gen.analysis(), frame_dir);
        fprintf(stderr, "burgc: generated %s\n", output_file.c_str());
    }

    return 0;
}
