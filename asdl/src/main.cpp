#include <iostream>
#include <unistd.h>

#include "generator.h"
#include "validator.h"
#include "exceptions.h"

static void print_usage(const char* program_name) {
    std::cerr << "Usage: " << program_name << " -i <input_file> -t <template_file> [-o <output_file>] [-lang c|c++]" << std::endl;
    std::cerr << "  -i <input_file>   : ASDL input file" << std::endl;
    std::cerr << "  -t <template_file>: Template file for code generation" << std::endl;
    std::cerr << "  -o <output_file>  : Output file (optional, defaults to stdout)" << std::endl;
    std::cerr << "  -lang c|c++       : Target language (default: c++)" << std::endl;
    std::cerr << "  -h                : Show this help message" << std::endl;
}

int main(int argc, char** argv) {
    try {
        std::string infile, templatefile, outfile, lang;
        int opt;
        bool show_help = false;

        while ((opt = getopt(argc, argv, "i:o:t:l:h")) != -1) {
            switch (opt) {
                case 'i':
                    infile = optarg;
                    break;
                case 'o':
                    outfile = optarg;
                    break;
                case 't':
                    templatefile = optarg;
                    break;
                case 'l':
                    lang = optarg;
                    break;
                case 'h':
                    show_help = true;
                    break;
                case '?':
                    std::cerr << "Unknown option: " << static_cast<char>(optopt) << std::endl;
                    print_usage(argv[0]);
                    return 1;
                default:
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

        if (templatefile.empty()) {
            std::cerr << "Error: Template file (-t) is required" << std::endl;
            print_usage(argv[0]);
            return 1;
        }

        std::cout << "Validating input files..." << std::endl;
        validate_file_exists(infile);
        validate_template_file(templatefile);
        validate_output_path(outfile);

        Generator gen;
        if (lang == "c") gen.types().set_lang_c();

        std::cout << "Parsing input file: " << infile << std::endl;
        auto* module_ast = gen.parse(infile);

        std::cout << "Processing AST definitions..." << std::endl;
        auto module = gen.to_json(module_ast);
        gen.process(module);

        std::cout << "Loading template: " << templatefile << std::endl;

        std::cout << "Generating output..." << std::endl;
        if (outfile.empty()) {
            gen.generate(module, templatefile, std::cout);
            std::cout << std::endl;
        } else {
            gen.generate(module, templatefile, outfile);
            std::cout << "Output written to: " << outfile << std::endl;
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
