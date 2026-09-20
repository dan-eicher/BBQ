// ipg_harness — the shared rig for the IPG conformance suite (ipg_law_test.cpp).
//
// Three ways to put a spec under a law:
//   diagnose(src)         — front end only: the sema errors/warnings a spec draws.
//   run(src, rule, bytes) — the CEK, the reference semantics every backend is
//                           generated against.
//   c_run(...)            — the generated C reader, compiled -Werror and executed,
//                           for laws where a backend could silently disagree.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Parser.h"
#include "Sema.h"
#include "CompilerCtx.h"
#include "RenderTypes.h"
#include "RenderEmit.h"
#include "bbq_compile.h"
#include "Machine.h"

namespace ipg {

// --- front end ------------------------------------------------------------

struct Diagnostics {
    bool parsed = false;
    bool ok = false;               // parsed && sema accepted
    std::string text;              // every error and warning, as printed

    bool says(const std::string& needle) const {
        return text.find(needle) != std::string::npos;
    }
};

inline Diagnostics diagnose(const std::string& src) {
    Diagnostics d;
    auto* parser = new ::Parser();
    parser->init(src.c_str(), static_cast<int>(src.size()));
    if (!parser->parse() || parser->ast == nullptr) return d;
    d.parsed = true;

    bbqgen::ErrorReporter reporter;
    bbqgen::Sema sema(reporter);
    d.ok = sema.analyze(parser->ast);
    std::ostringstream oss;
    reporter.print_all(oss);
    d.text = oss.str();
    return d;
}

// --- the CEK --------------------------------------------------------------

struct Run {
    bool compiled = false;
    bool success = false;
    std::string error;                       // sema text, or the parse failure
    bbq::zcow::parse_result meta;
    ::bbq::cek::CompiledGrammar* grammar = nullptr;
    // The parse BORROWS its input: the document's spans point into these bytes and
    // serializing blits from them. A `run(g, "R", {1, 2})` would otherwise hand the
    // document a temporary that dies at the end of the statement, and every later
    // read of a span would be a use-after-free — which is what happens, silently,
    // until something looks. The buffer is a member so it outlives the document,
    // and moving a vector keeps its heap block, so moving a Run is safe.
    std::shared_ptr<std::vector<uint8_t>> input;

    ~Run() { delete grammar; }
    Run() = default;
    Run(Run&& o) noexcept { *this = std::move(o); }
    Run& operator=(Run&& o) noexcept {
        compiled = o.compiled; success = o.success; error = std::move(o.error);
        meta = std::move(o.meta); grammar = o.grammar; o.grammar = nullptr;
        input = std::move(o.input);
        return *this;
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    const bbq::zcow::node* root() const { return meta.doc.root(); }
};

// A fixed-width blackbox parser, so specs carrying `extern("readit", …)` run.
// `seen` records what the interval handed it — the confinement law (§3.4).
struct ExternWitness {
    static std::vector<uint8_t> seen;
    static size_t consume;
};
inline std::vector<uint8_t> ExternWitness::seen;
inline size_t ExternWitness::consume = 4;

inline Run run(const std::string& src, const std::string& rule,
               const std::vector<uint8_t>& bytes) {
    Run r;
    r.input = std::make_shared<std::vector<uint8_t>>(bytes);
    auto* parser = new ::Parser();
    parser->init(src.c_str(), static_cast<int>(src.size()));
    if (!parser->parse() || parser->ast == nullptr) {
        r.error = "parse failed";
        return r;
    }
    bbqgen::ErrorReporter reporter;
    bbqgen::Sema sema(reporter);
    sema.analyze(parser->ast);
    if (reporter.has_errors()) {
        std::ostringstream oss;
        reporter.print_all(oss);
        r.error = oss.str();
        return r;
    }

    ::bbq::Compiler compiler;
    r.grammar = compiler.compile_grammar(parser->ast);
    if (!r.grammar) { r.error = "compile_grammar returned null"; return r; }
    r.compiled = true;

    ::bbq::cek::KontNode* entry = r.grammar->lookup(std::string(rule));
    if (!entry) { r.error = "rule '" + rule + "' not found"; return r; }

    ExternWitness::seen.clear();
    static ::bbq::cek::ExternalParserTable::Entry ext_entries[1];
    static ::bbq::cek::ExternalParserTable ext_table;
    ext_entries[0].name = r.grammar->strings.intern("readit");
    ext_entries[0].fn = [](const uint8_t* p, size_t length, size_t* consumed,
                           bbq::ParseArena*, void*) -> bool {
        ExternWitness::seen.assign(p, p + length);
        if (length < ExternWitness::consume) return false;
        *consumed = ExternWitness::consume;
        return true;
    };
    ext_entries[0].user_data = nullptr;
    ext_table.entries = ext_entries;
    ext_table.count = 1;

    ::bbq::cek::CEKMachine m;
    m.arena = &r.grammar->arena;
    m.builtins = &r.grammar->builtins;
    m.ext_parsers = &ext_table;
    r.meta = m.execute_from(entry, r.input->data(), r.input->size(),
                            r.grammar->default_little_endian);
    r.success = !m.failed && r.meta.success;
    if (!r.success && m.best_error_msg) r.error = m.best_error_msg;
    return r;
}

// Child lookup by name, one level down. Returns nullptr when absent.
inline const bbq::zcow::node* kid(const bbq::zcow::node* n, const char* name) {
    if (!n) return nullptr;
    for (auto& k : n->kids)
        if (k->name && std::strcmp(k->name, name) == 0) return k.get();
    return nullptr;
}

// --- the generated C reader ----------------------------------------------

// Render the spec to a C reader, compile it -Werror with a main that parses
// `bytes`, run it. `check` is a C expression over `out`, evaluated only when the
// parse is expected to succeed. Returns true iff the binary exits 0.
inline bool c_run(const std::string& spec, const std::string& type_name,
                  const std::string& fn_name, const std::vector<uint8_t>& bytes,
                  const std::string& check, bool want_parse_ok, std::string* err) {
    auto* parser = new ::Parser();
    parser->init(spec.c_str(), static_cast<int>(spec.size()));
    if (!parser->parse()) { if (err) *err = "parse failed"; return false; }
    bbqgen::ErrorReporter rep;
    bbqgen::Sema sema(rep);
    if (!sema.analyze(parser->ast)) {
        if (err) { std::ostringstream o; rep.print_all(o); *err = "sema: " + o.str(); }
        return false;
    }

    ::bbq::Compiler compiler;
    auto* g = compiler.compile_grammar(parser->ast);
    if (!g) { if (err) *err = "compile_grammar null"; return false; }

    bbq::render::CompilerCtx ctx{parser->ast, &sema, g, ""};
    std::string tmpl = std::string(SOURCE_DIR) + "/backends/render/templates";
    std::string reader = bbq::render::render_reader_c(ctx, tmpl);
    std::string decls = bbq::render::render_reader_decls(ctx);
    std::string types_h = bbq::render::render_types_c(
        ctx, std::string(SOURCE_DIR) + "/backends/render/templates/types_c.inja");

    std::string dir = "/tmp/bbq_ipg_law_" + std::to_string(getpid());
    system(("mkdir -p " + dir).c_str());
    { std::ofstream f(dir + "/testTypes.h"); f << types_h; }
    {
        std::ofstream f(dir + "/test.c");
        f << "#include \"testTypes.h\"\n#include \"bbq_runtime.h\"\n"
             "#include <stdlib.h>\n#include <string.h>\n";
        f << decls << reader;
        f << "int main(void){\n  static const uint8_t buf[] = {";
        for (size_t i = 0; i < bytes.size(); i++)
            f << static_cast<int>(bytes[i]) << (i + 1 < bytes.size() ? "," : "");
        f << "};\n  bbq_ctx_t ctx; bbq_ctx_init(&ctx, buf, sizeof buf);\n";
        f << "  " << type_name << " out; memset(&out, 0, sizeof out);\n";
        f << "  bool ok = " << fn_name << "(&ctx, &out);\n";
        f << "  if (ok != " << (want_parse_ok ? "true" : "false") << ") return 1;\n";
        if (want_parse_ok) f << "  if (!(" << check << ")) return 2;\n";
        f << "  return 0;\n}\n";
    }
    std::string rt = std::string(SOURCE_DIR) + "/backends/c/runtime";
    std::string crt = std::string(SOURCE_DIR) + "/crt";
    std::string cc = "cc -Wall -Wextra -Werror -std=c11 -I" + dir + " -I" + rt +
                     " -I" + crt + " " + dir + "/test.c " +
                     crt + "/bbq_vec.c " + crt + "/bbq_alloc.c" +
                     " -o " + dir + "/bin 2>&1";
    FILE* p = popen(cc.c_str(), "r");
    char buf[8192] = {};
    if (p) { size_t n = fread(buf, 1, sizeof buf - 1, p); (void)n; }
    int rc = p ? pclose(p) : -1;
    if (rc != 0) { if (err) *err = std::string("compile failed:\n") + buf; return false; }

    int run_rc = system((dir + "/bin").c_str());
    system(("rm -rf " + dir).c_str());
    if (run_rc != 0) { if (err) *err = "run failed rc=" + std::to_string(run_rc); return false; }
    return true;
}

}  // namespace ipg
