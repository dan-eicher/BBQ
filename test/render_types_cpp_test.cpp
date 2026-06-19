// render_types_cpp_test — the C++ typed-view generator (Phase 2).
//
// Renders the handle classes for a spec and g++ -fsyntax-only compiles them
// against the shared index runtime (bbq_node.h + backends/cek). The conformance
// driver is the real ELF64 grammar. Unsupported constructs must throw (the
// Sema-style feature gate), not silently degrade.
#include <gtest/gtest.h>
#include "Parser.h"
#include "Sema.h"
#include "RenderTypes.h"
#include "CompilerCtx.h"
#include <fstream>
#include <sstream>
#include <string>
#include <cstdio>

using namespace BBQ;

namespace {

std::string render(const std::string& spec, const std::string& ns = "") {
    Parser parser;
    parser.init(spec.c_str(), (int)spec.size());
    EXPECT_TRUE(parser.parse()) << "parse failed";
    bbqgen::ErrorReporter rep;
    bbqgen::Sema sema(rep);
    EXPECT_TRUE(sema.analyze(parser.ast)) << "sema failed";
    std::string tmpl = std::string(SOURCE_DIR) + "/backends/render/templates/types_cpp.inja";
    bbq::render::CompilerCtx ctx{parser.ast, &sema, nullptr, ""};
    return bbq::render::render_types_cpp(ctx, tmpl, ns);
}

// Render `spec`, then g++ -fsyntax-only the generated header. Returns true iff
// it compiles; on failure fills `err` with the compiler output.
bool compiles(const std::string& spec, std::string* err, const std::string& ns = "") {
    std::string header;
    try {
        header = render(spec, ns);
    } catch (const std::exception& e) {
        if (err) *err = std::string("render threw: ") + e.what();
        return false;
    }
    std::string dir = "/tmp/bbq_cpp_types_" + std::to_string(getpid());
    system(("mkdir -p " + dir).c_str());
    { std::ofstream f(dir + "/testTypes.hpp"); f << header; }
    { std::ofstream f(dir + "/main.cpp"); f << "#include \"testTypes.hpp\"\n"; }

    std::string src = std::string(SOURCE_DIR);
    std::string cmd = "g++ -std=c++17 -fsyntax-only -Wall -Wextra"
                      " -I" + dir +
                      " -I" + src + "/backends/cpp/runtime " +
                      dir + "/main.cpp 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    char buf[8192] = {}; if (p) fread(buf, 1, sizeof buf - 1, p);
    int rc = p ? pclose(p) : -1;
    system(("rm -rf " + dir).c_str());
    if (rc != 0) { if (err) *err = buf; return false; }
    return true;
}

}  // namespace

TEST(RenderTypesCpp, FlatScalarStruct) {
    std::string err;
    EXPECT_TRUE(compiles("@endian little\nP = struct { x: uint8, y: uint16, z: int32 }", &err)) << err;
}

TEST(RenderTypesCpp, BytesAndRuleRef) {
    std::string err;
    EXPECT_TRUE(compiles(
        "Inner = struct { a: uint8, pad: bytes[3] }\n"
        "Outer = struct { tag: uint8, inner: Inner }", &err)) << err;
}

TEST(RenderTypesCpp, ComputeField) {
    std::string err;
    EXPECT_TRUE(compiles(
        "S = struct { info: uint8, hi: compute((info >> 4) & 0x0F : uint8) }", &err)) << err;
}

TEST(RenderTypesCpp, ArrayOfRule) {
    std::string err;
    EXPECT_TRUE(compiles(
        "Item = struct { v: uint8 }\n"
        "List = struct { n: uint8, items: array<Item>[n] }", &err)) << err;
}

TEST(RenderTypesCpp, Namespaced) {
    std::string err;
    EXPECT_TRUE(compiles("P = struct { x: uint32 }", &err, "demo")) << err;
}

TEST(RenderTypesCpp, Elf64Conformance) {
    std::ifstream f(std::string(SOURCE_DIR) + "/jitterator/extract/elf64.bbq");
    std::stringstream ss; ss << f.rdbuf();
    std::string err;
    EXPECT_TRUE(compiles(ss.str(), &err)) << err;
}

// The C++ ZCow type model is fully general over the neutral type nodes — there are
// no feature gates. Every grammar feature reads through a decode-on-access accessor;
// correctness is proven by CEK-parity in render_types_cpp_parity_test.

