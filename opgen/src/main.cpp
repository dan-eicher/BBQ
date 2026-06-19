/*
 * opgen — generate the consolidated opcode VM/tables/headers from opcodes.def.
 *
 * Reads opcodes.def via the pegc-generated C++ parser, builds the Spec (derived
 * facts), and emits the VM outputs (interpreter, stencils, tables, headers).
 * A C++ tool; the emitted VM code stays C.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "Parser.h"
#include "spec.h"
#include "vmemit.h"
#include "tablesemit.h"

namespace opgen {

// RAII wrapper for a FILE* opened for writing.
class File {
public:
    File(const char* path, const char* mode) : f_(fopen(path, mode)) {}
    ~File() { if (f_) fclose(f_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    explicit operator bool() const { return f_ != nullptr; }
    FILE* get() const { return f_; }

private:
    FILE* f_;
};

// Read an entire file into a string; false (and reports via perror) on I/O error.
static bool slurp(const char* path, std::string& out) {
    File f(path, "rb");
    if (!f) { perror(path); return false; }
    fseek(f.get(), 0, SEEK_END);
    long n = ftell(f.get());
    fseek(f.get(), 0, SEEK_SET);
    std::string buf(static_cast<size_t>(n), '\0');
    if (fread(&buf[0], 1, static_cast<size_t>(n), f.get()) != static_cast<size_t>(n))
        return false;
    out = std::move(buf);
    return true;
}

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s -i <opcodes.def> -o <out_dir>\n"
        "  Emits the interpreter, JIT stencils, opcode/type tables, and runtime headers.\n",
        prog);
}

static int write_file(const char* dir, const char* name,
                      const std::function<void(FILE*)>& emit) {
    std::string path = std::string(dir) + "/" + name;
    File f(path.c_str(), "w");
    if (!f) { perror(path.c_str()); return 1; }
    emit(f.get());
    fprintf(stderr, "opgen: wrote %s\n", path.c_str());
    return 0;
}

static int run(int argc, char** argv) {
    const char* in_path = nullptr;
    const char* out_dir = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) in_path = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_dir = argv[++i];
        else { usage(argv[0]); return 2; }
    }
    if (!in_path || !out_dir) { usage(argv[0]); return 2; }

    std::string src;
    if (!slurp(in_path, src)) return 1;

    Parser p;
    p.init(src.data(), static_cast<int>(src.size()));
    if (!p.parse() || !p.ast) {
        auto fur = p.furthest();
        fprintf(stderr, "opgen: parse failed at %d:%d\n", fur.line, fur.col);
        return 1;
    }

    Module* m = p.ast;
    Spec spec(m);
    VmEmitter vm(m);
    TablesEmitter tables(spec);

    int rc = 0;
    rc |= write_file(out_dir, "opcodes.h",            [&](FILE* o){ tables.emit_opcodes_h(o); });
    rc |= write_file(out_dir, "opcode_tables.c",      [&](FILE* o){ tables.emit_opcode_tables_c(o); });
    rc |= write_file(out_dir, "runtime_api.h",        [&](FILE* o){ vm.emit_runtime_api_h(o); });
    rc |= write_file(out_dir, "gen_interp.h",         [&](FILE* o){ vm.emit_interp_h(o); });
    rc |= write_file(out_dir, "gen_interp.c",         [&](FILE* o){ vm.emit_interp_c(o); });
    rc |= write_file(out_dir, "wasm_stencils.c",      [&](FILE* o){ vm.emit_stencil_c(o); });
    rc |= write_file(out_dir, "wasm_jit_meta.h",      [&](FILE* o){ vm.emit_jit_meta(o); });
    rc |= write_file(out_dir, "wasm_valtype.h",       [&](FILE* o){ vm.emit_valtype_h(o); });
    rc |= write_file(out_dir, "wasm_type_meta.h",     [&](FILE* o){ vm.emit_type_meta(o); });
    rc |= write_file(out_dir, "wasm_jit_symbols.h",   [&](FILE* o){ vm.emit_jit_symbols(o); });

    return rc;
}

} // namespace opgen

int main(int argc, char** argv) {
    return opgen::run(argc, argv);
}
