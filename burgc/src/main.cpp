#include "generator.h"
#include <cstdio>
#include <fstream>
#include <cstring>
#include <iostream>

static void usage() {
    fprintf(stderr,
        "Usage: burgc -i <input.burg> [-o <output.h>] [-lang c++|c] [-frames <dir>]\n"
        "  -i <file>      Input .burg file\n"
        "  -o <file>      Output file (.h for C++, .h+.c for C)\n"
        "  -lang c++|c    Target language (default: c++)\n"
        "  -frames <dir>  Frame template directory\n");
}

int main(int argc, char** argv) {
    std::string input_file;
    std::string output_file;
    std::string lang = "c++";
    std::string frame_dir;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-lang") == 0 && i + 1 < argc) {
            lang = argv[++i];
        } else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            frame_dir = argv[++i];
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

    for (auto& w : gen.warnings())
        fprintf(stderr, "warning: %s\n", w.c_str());

    std::unique_ptr<BurgBackend> backend;
    if (lang == "c")
        backend = create_c_backend();
    else if (lang == "c++")
        backend = create_cpp_backend();
    else {
        fprintf(stderr, "error: unknown language '%s' (use 'c++' or 'c')\n", lang.c_str());
        return 1;
    }

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
