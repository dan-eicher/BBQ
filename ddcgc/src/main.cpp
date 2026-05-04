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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static void usage() {
    std::fprintf(stderr,
        "Usage: ddcgc -i <input.ddcg> -ast <ast.json> -ir <ir.json>\n"
        "             [-L <libdir>]... [-o <output.cpp>]\n"
        "  -i <file>          Input .ddcg file\n"
        "  -ast <file.json>   asdl JSON for the source AST (matched against\n"
        "                     the .ddcg's source-import name)\n"
        "  -ir  <file.json>   asdl JSON for the target IR (matched against\n"
        "                     the .ddcg's target-import name)\n"
        "  -L   <dir>         Add a library search path for `import \"...ddcg\"`.\n"
        "                     Repeatable; searched in order after the importing\n"
        "                     file's own directory.\n"
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

// Canonical string form of a TypeRef, used for signature equality
// when comparing aux / pred declarations across files. Mirrors the
// surface syntax (`int`, `list<T>`, `tuple<A, B>`, `module.Ctor*`).
static std::string typeref_to_string(DdcgAst::TypeRef* t) {
    if (auto* tn = dynamic_cast<DdcgAst::TypeName*>(t)) {
        std::string r;
        if (!tn->module.empty()) r = tn->module + ".";
        r += tn->name;
        if (tn->is_pointer) r += "*";
        return r;
    }
    if (auto* tl = dynamic_cast<DdcgAst::TypeList*>(t)) {
        return "list<" + typeref_to_string(tl->elem) + ">";
    }
    if (auto* tt = dynamic_cast<DdcgAst::TypeTuple*>(t)) {
        std::string r = "tuple<";
        for (size_t i = 0; i < tt->elems.size(); ++i) {
            if (i) r += ", ";
            r += typeref_to_string(tt->elems[i]);
        }
        return r + ">";
    }
    return "?";
}

static std::string aux_signature(DdcgAst::Aux* a) {
    std::string r = "(";
    for (size_t i = 0; i < a->param_types.size(); ++i) {
        if (i) r += ", ";
        r += typeref_to_string(a->param_types[i]);
    }
    r += ") -> " + typeref_to_string(a->return_ty);
    return r;
}

static std::string pred_signature(DdcgAst::Pred* p) {
    std::string r = "(";
    for (size_t i = 0; i < p->param_types.size(); ++i) {
        if (i) r += ", ";
        r += typeref_to_string(p->param_types[i]);
    }
    r += ") -> bool";
    return r;
}

// Canonical form of a destination declaration (kind + name + ordered
// variant list with per-variant field shapes), for cross-file shape
// equality on imports. Variants are order-sensitive: `delta { a | b }`
// and `delta { b | a }` would emit different switch tables, so we
// treat them as different.
static std::string dest_signature(DdcgAst::DestDecl* d) {
    std::string r;
    std::vector<DdcgAst::DestVariant*> variants;
    if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
        r = "data_dest " + dd->name + " {";
        variants = dd->variants;
    } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
        r = "ctrl_dest " + cd->name + " {";
        variants = cd->variants;
    } else {
        return "?";
    }
    for (size_t i = 0; i < variants.size(); ++i) {
        if (i) r += " |";
        r += " " + variants[i]->name;
        if (!variants[i]->fields.empty()) {
            r += "(";
            for (size_t j = 0; j < variants[i]->fields.size(); ++j) {
                if (j) r += ", ";
                r += variants[i]->fields[j]->name + ": " +
                     typeref_to_string(variants[i]->fields[j]->ty);
            }
            r += ")";
        }
    }
    r += " }";
    return r;
}

static std::string dest_decl_name(DdcgAst::DestDecl* d) {
    if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) return dd->name;
    if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) return cd->name;
    return "";
}

// Resolve a library import path: try the importing file's directory
// first, then each entry in the search-path list. Returns the canonical
// path on success, or empty on failure (caller reports the error with
// the original `requested` name + tried paths).
static std::filesystem::path resolve_library(
        const std::string& import_path,
        const std::filesystem::path& importing_dir,
        const std::vector<std::filesystem::path>& search_paths,
        std::vector<std::string>& tried) {
    auto try_one = [&](const std::filesystem::path& base) {
        std::filesystem::path candidate = base / import_path;
        std::error_code ec;
        auto c = std::filesystem::canonical(candidate, ec);
        if (!ec) return c;
        tried.push_back(candidate.string());
        return std::filesystem::path{};
    };
    if (auto r = try_one(importing_dir); !r.empty()) return r;
    for (const auto& sp : search_paths) {
        if (auto r = try_one(sp); !r.empty()) return r;
    }
    return {};
}

// Recursively load `import "..."` library files, splicing their fun /
// auxiliary / predicate / destination declarations into the primary
// file. Cycles error out (with the chain printed); diamond imports
// are tolerated (each lib loaded exactly once).
//
// `loaded` tracks canonical paths fully processed so we skip them on
// repeated visits. `loading` is the chain currently being expanded —
// re-encountering one of its entries means a cycle.
static bool load_one_library(DdcgAst::File* primary,
                              const std::string& import_path,
                              const std::filesystem::path& importing_dir,
                              const std::vector<std::filesystem::path>& search_paths,
                              std::set<std::string>& loaded,
                              std::vector<std::string>& loading) {
    std::vector<std::string> tried;
    auto canonical = resolve_library(import_path, importing_dir,
                                      search_paths, tried);
    if (canonical.empty()) {
        std::fprintf(stderr, "ddcgc: library not found: %s\n",
                     import_path.c_str());
        for (const auto& t : tried) {
            std::fprintf(stderr, "  tried: %s\n", t.c_str());
        }
        return false;
    }
    std::string canonical_str = canonical.string();

    if (loaded.count(canonical_str)) return true;
    for (const auto& on_stack : loading) {
        if (on_stack == canonical_str) {
            std::fprintf(stderr, "ddcgc: library import cycle:\n");
            for (const auto& step : loading) {
                std::fprintf(stderr, "  %s\n", step.c_str());
            }
            std::fprintf(stderr, "  %s  (cycle closes here)\n",
                         canonical_str.c_str());
            return false;
        }
    }

    loading.push_back(canonical_str);

    std::string src, err;
    if (!read_file(canonical_str, src, err)) {
        std::fprintf(stderr, "ddcgc: %s\n", err.c_str());
        return false;
    }
    Parser parser;
    parser.init(src.data(), static_cast<int>(src.size()));
    if (!parser.parse() || !parser.ast) {
        auto fur = parser.furthest();
        std::fprintf(stderr,
                     "ddcgc: parse error in library %s near line %d, col %d\n",
                     canonical_str.c_str(), fur.line, fur.col);
        return false;
    }
    DdcgAst::File* lib = parser.ast;

    // Recurse into the lib's own imports first so transitively-loaded
    // funs come into the primary in dependency order.
    auto lib_dir = canonical.parent_path();
    for (auto* sub : lib->libraries) {
        if (!load_one_library(primary, sub->path, lib_dir, search_paths,
                              loaded, loading)) {
            return false;
        }
    }

    // Splice DESTINATIONS from the library. Same-name + same-shape
    // (variants, fields, types, order) merges silently. Different
    // shape = hard error.
    for (auto* d : lib->destinations) {
        const std::string dname = dest_decl_name(d);
        bool merged = false;
        for (auto* existing : primary->destinations) {
            if (dest_decl_name(existing) != dname) continue;
            std::string lhs = dest_signature(existing);
            std::string rhs = dest_signature(d);
            if (lhs == rhs) { merged = true; break; }
            std::fprintf(stderr,
                         "ddcgc: destination '%s' from library %s is %s, "
                         "incompatible with earlier %s\n",
                         dname.c_str(), canonical_str.c_str(),
                         rhs.c_str(), lhs.c_str());
            return false;
        }
        if (!merged) primary->destinations.push_back(d);
    }

    // Splice AUXILIARIES from the library. Same-name + same-signature
    // is silent (diamond / re-export); same-name + different-signature
    // is a hard error.
    for (auto* a : lib->auxiliaries) {
        bool merged = false;
        for (auto* existing : primary->auxiliaries) {
            if (existing->name != a->name) continue;
            std::string lhs = aux_signature(existing);
            std::string rhs = aux_signature(a);
            if (lhs == rhs) { merged = true; break; }
            std::fprintf(stderr,
                         "ddcgc: aux '%s' from library %s has signature %s, "
                         "incompatible with earlier %s\n",
                         a->name.c_str(), canonical_str.c_str(),
                         rhs.c_str(), lhs.c_str());
            return false;
        }
        if (!merged) primary->auxiliaries.push_back(a);
    }

    // Splice PREDICATES from the library — same rule as AUXILIARIES.
    for (auto* p : lib->predicates) {
        bool merged = false;
        for (auto* existing : primary->predicates) {
            if (existing->name != p->name) continue;
            std::string lhs = pred_signature(existing);
            std::string rhs = pred_signature(p);
            if (lhs == rhs) { merged = true; break; }
            std::fprintf(stderr,
                         "ddcgc: predicate '%s' from library %s has signature "
                         "%s, incompatible with earlier %s\n",
                         p->name.c_str(), canonical_str.c_str(),
                         rhs.c_str(), lhs.c_str());
            return false;
        }
        if (!merged) primary->predicates.push_back(p);
    }

    // Splice fun declarations into the primary file. Collision = error;
    // the user is responsible for naming hygiene.
    for (auto* f : lib->funs) {
        for (auto* existing : primary->funs) {
            if (existing->name == f->name) {
                std::fprintf(stderr,
                             "ddcgc: fun '%s' from library %s collides "
                             "with an existing definition\n",
                             f->name.c_str(), canonical_str.c_str());
                return false;
            }
        }
        primary->funs.push_back(f);
    }

    loaded.insert(canonical_str);
    loading.pop_back();
    return true;
}

static bool load_libraries(DdcgAst::File* primary,
                            const std::filesystem::path& primary_dir,
                            const std::vector<std::filesystem::path>& search_paths) {
    std::set<std::string> loaded;
    std::vector<std::string> loading;
    for (auto* lib : primary->libraries) {
        if (!load_one_library(primary, lib->path, primary_dir, search_paths,
                               loaded, loading)) {
            return false;
        }
    }
    return true;
}

int main(int argc, char** argv) {
    std::string input_file;
    std::string ast_json_file;
    std::string ir_json_file;
    std::string output_file;
    std::vector<std::filesystem::path> lib_search_paths;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (std::strcmp(argv[i], "-ast") == 0 && i + 1 < argc) {
            ast_json_file = argv[++i];
        } else if (std::strcmp(argv[i], "-ir") == 0 && i + 1 < argc) {
            ir_json_file = argv[++i];
        } else if (std::strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            lib_search_paths.emplace_back(argv[++i]);
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
        auto fur = parser.furthest();
        std::fprintf(stderr, "ddcgc: parse error in %s near line %d, col %d\n",
                     input_file.c_str(), fur.line, fur.col);
        if (parser.has_errors()) {
            for (const auto& e : parser.errors()) {
                std::fprintf(stderr, "  line %d, col %d: %s\n",
                             e.line, e.col, e.message.c_str());
            }
        }
        return 1;
    }
    DdcgAst::File* file = parser.ast;

    // 1.5. Recursively load `import "..."` libraries. Their funs are
    //      spliced into `file->funs` so the existing typecheck + emit
    //      pipeline picks them up unchanged.
    {
        std::error_code ec;
        auto primary_path = std::filesystem::canonical(input_file, ec);
        if (ec) {
            std::fprintf(stderr, "ddcgc: cannot resolve %s: %s\n",
                         input_file.c_str(), ec.message().c_str());
            return 1;
        }
        if (!load_libraries(file, primary_path.parent_path(),
                            lib_search_paths)) return 1;
    }

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
