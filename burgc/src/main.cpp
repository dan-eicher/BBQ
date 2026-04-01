#include "generator.h"
#include <cstdio>
#include <fstream>
#include <cstring>
#include <iostream>

static void usage() {
    fprintf(stderr, "Usage: burgc -i <input.burg> [-o <output.h>] [-lang c++|c]\n");
}

int main(int argc, char** argv) {
    std::string input_file;
    std::string output_file;
    std::string lang = "c++";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-lang") == 0 && i + 1 < argc) {
            lang = argv[++i];
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

    std::unique_ptr<BurgBackend> backend;
    if (lang == "c")
        backend = create_c_backend();
    else if (lang == "c++")
        backend = create_cpp_backend();
    else {
        fprintf(stderr, "error: unknown language '%s' (use 'c++' or 'c')\n", lang.c_str());
        return 1;
    }

    if (output_file.empty()) {
        backend->generate(std::cout, gen.analysis());
    } else {
        std::ofstream ofs(output_file);
        if (!ofs) {
            fprintf(stderr, "error: cannot open output file: %s\n", output_file.c_str());
            return 1;
        }
        backend->generate(ofs, gen.analysis());
        fprintf(stderr, "burgc: generated %s\n", output_file.c_str());
    }

    return 0;
}
