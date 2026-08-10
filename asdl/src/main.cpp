#include <fstream>
#include <iostream>
#include <unistd.h>

#include "generator.h"
#include "validator.h"
#include "exceptions.h"

static void print_usage(const char* program_name) {
    std::cerr << "Usage: " << program_name << " -i <input_file> [-t <template_file>] [-o <output_file>] [-lang c|c++|java] [--json <file>]" << std::endl;
    std::cerr << "  -i <input_file>   : ASDL input file" << std::endl;
    std::cerr << "  -t <template_file>: Template file for code generation (omit with --json to skip)" << std::endl;
    std::cerr << "  -o <output_file>  : Output file (optional, defaults to stdout)" << std::endl;
    std::cerr << "  -lang c|c++|java  : Target language (default: c++)" << std::endl;
    std::cerr << "  -java-package <p> : Package declaration for -lang java output" << std::endl;
    std::cerr << "  --json <file>     : Write processed module JSON to <file> for downstream tools (e.g. burgc -asdl)" << std::endl;
    std::cerr << "  -h                : Show this help message" << std::endl;
}

int main(int argc, char** argv) {
    try {
        std::string infile, templatefile, outfile, lang, jsonfile, javapackage;
        bool show_help = false;

        // Custom arg parsing: getopt doesn't support --json long option
        // alongside the existing single-letter flags.
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "-i" && i + 1 < argc) infile = argv[++i];
            else if (a == "-o" && i + 1 < argc) outfile = argv[++i];
            else if (a == "-t" && i + 1 < argc) templatefile = argv[++i];
            else if ((a == "-l" || a == "-lang") && i + 1 < argc) lang = argv[++i];
            else if (a == "-java-package" && i + 1 < argc) javapackage = argv[++i];
            else if (a == "--json" && i + 1 < argc) jsonfile = argv[++i];
            else if (a == "-h") show_help = true;
            else {
                std::cerr << "Unknown option: " << a << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        }

        if (show_help) {
            print_usage(argv[0]);
            return 0;
        }

        if (infile.empty()) {
            std::cerr << "Error: Input file (-i) is required" << std::endl;
            print_usage(argv[0]);
            return 1;
        }

        // -t is required only when generating templated output.
        if (templatefile.empty() && jsonfile.empty()) {
            std::cerr << "Error: -t <template> or --json <file> is required" << std::endl;
            print_usage(argv[0]);
            return 1;
        }

        std::cout << "Validating input files..." << std::endl;
        validate_file_exists(infile);
        if (!templatefile.empty()) validate_template_file(templatefile);
        validate_output_path(outfile);

        Generator gen;
        if (lang == "c") gen.types().set_lang_c();
        else if (lang == "java") gen.types().set_lang_java();

        if (!javapackage.empty() && lang != "java") {
            std::cerr << "Error: -java-package requires -lang java" << std::endl;
            return 1;
        }

        std::cout << "Parsing input file: " << infile << std::endl;
        auto* module_ast = gen.parse(infile);

        std::cout << "Processing AST definitions..." << std::endl;
        auto module = gen.to_json(module_ast);
        gen.process(module);
        if (!javapackage.empty()) module["java_package"] = javapackage;

        if (!jsonfile.empty()) {
            std::ofstream js(jsonfile);
            if (!js) {
                std::cerr << "Error: cannot open --json output: " << jsonfile << std::endl;
                return 1;
            }
            js << module.dump(2) << "\n";
            std::cout << "JSON sidecar written to: " << jsonfile << std::endl;
        }

        if (!templatefile.empty()) {
            std::cout << "Loading template: " << templatefile << std::endl;

            std::cout << "Generating output..." << std::endl;
            if (outfile.empty()) {
                gen.generate(module, templatefile, std::cout);
                std::cout << std::endl;
            } else {
                gen.generate(module, templatefile, outfile);
                std::cout << "Output written to: " << outfile << std::endl;
            }
        }

        std::cout << "ASDL generation completed successfully!" << std::endl;
        return 0;

    } catch (const FileException& e) {
        std::cerr << "File Error: " << e.what() << std::endl;
        return 1;
    } catch (const ParseException& e) {
        std::cerr << "Parse Error: " << e.what() << std::endl;
        return 1;
    } catch (const TemplateException& e) {
        std::cerr << "Template Error: " << e.what() << std::endl;
        return 1;
    } catch (const ASDLException& e) {
        std::cerr << "ASDL Error: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Unexpected Error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown error occurred" << std::endl;
        return 1;
    }
}
