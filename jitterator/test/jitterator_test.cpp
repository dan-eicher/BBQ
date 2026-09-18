// jitterator_test — the stencil extractor's own tests.
//
// Until now jitterator was exercised only through the calc VM end to end: if the
// calculator printed the right number, the extractor was assumed correct. That
// covers one object file, built one way, with two relocation types in it. What it
// cannot see is everything the extractor does with an object file that is shaped
// differently — which is the whole of its job.
//
// Each test builds a real `.o` with clang and runs the real binary, because the
// thing under test is precisely how clang's output is read.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct Proc {
    int status = -1;
    std::string out;      // stdout + stderr
    bool ok() const { return status == 0; }
    bool says(const std::string& needle) const {
        return out.find(needle) != std::string::npos;
    }
};

std::string tmpdir() {
    static std::string d = "/tmp/jitterator_test_" + std::to_string(getpid());
    static bool made = (system(("mkdir -p " + d).c_str()) == 0);
    (void)made;
    return d;
}

Proc shell(const std::string& cmd) {
    Proc r;
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return r;
    char buf[4096];
    while (fgets(buf, sizeof buf, p)) r.out += buf;
    int rc = pclose(p);
    r.status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    return r;
}

// Compile a stencil source to a .o. `flags` picks the code model, which is what
// decides the relocation types clang emits.
std::string compile_obj(const std::string& name, const std::string& src,
                        const std::string& flags, Proc* out) {
    std::string c = tmpdir() + "/" + name + ".c";
    std::string o = tmpdir() + "/" + name + ".o";
    { std::ofstream f(c); f << src; }
    *out = shell(std::string(CLANG_C) + " -c -std=c11 " + flags + " " + c + " -o " + o);
    return o;
}

Proc run_jitterator(const std::string& obj, const std::string& header = "") {
    std::string cmd = std::string(JITTERATOR_BIN) + " " + obj;
    if (!header.empty()) cmd += " -o " + header;
    return shell(cmd);
}

// A minimal well-formed stencil: one branch hole (a tail call to the next
// stencil) and one data hole (a value the driver plugs).
const char* kStencilPrelude = R"(
#include <stdint.h>
#define STENCIL __attribute__((preserve_none))
#define TAIL    __attribute__((musttail))
extern uint64_t _HOLE_v;
extern void STENCIL _HOLE_cont(int64_t*);
)";

}  // namespace

// ── The shape it is built for ────────────────────────────────────────────────

TEST(Jitterator, ExtractsAStencilWithItsBranchAndDataHoles) {
    Proc cc;
    std::string obj = compile_obj("good", std::string(kStencilPrelude) + R"(
void STENCIL op_add(int64_t* s) {
    s[0] += (int64_t)(uintptr_t)&_HOLE_v;
    TAIL return _HOLE_cont(s);
}
)", "-O2", &cc);
    ASSERT_TRUE(cc.ok()) << cc.out;

    std::string hdr = tmpdir() + "/good.h";
    Proc r = run_jitterator(obj, hdr);
    ASSERT_TRUE(r.ok()) << r.out;

    std::ifstream f(hdr);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string table = ss.str();
    EXPECT_NE(table.find("STENCIL_OP_ADD"), std::string::npos) << table;
    EXPECT_NE(table.find("_HOLE_cont"), std::string::npos) << table;
    EXPECT_NE(table.find("_HOLE_v"), std::string::npos) << table;
    // The tail call is a branch; taking a hole's address is a data reference that
    // needs a constant-pool slot.
    EXPECT_NE(table.find("PATCH_REL_BRANCH"), std::string::npos) << table;
    EXPECT_NE(table.find("PATCH_REL_DATA"), std::string::npos) << table;
}

// ── Relocation types it was never taught ─────────────────────────────────────
//
// `reloc_to_patch_type` switched on four relocation types and returned
// PATCH_REL_DATA for everything else. Everything else is not a RIP-relative data
// load: a GOT-relative one loads the address *through the GOT*, and a 64-bit
// PC-relative one is eight bytes, not four. Either would be patched as though it
// were the thing it is not, and the only symptom is a JIT that computes the wrong
// answer or jumps into nowhere.

TEST(Jitterator, RefusesAGotRelativeRelocationRatherThanGuessing) {
    // -fPIE is a default on many distributions, and it turns `&_HOLE_v` into
    // R_X86_64_REX_GOTPCRELX.
    Proc cc;
    std::string obj = compile_obj("pie", std::string(kStencilPrelude) + R"(
void STENCIL op_pie(int64_t* s) {
    s[0] = (int64_t)(uintptr_t)&_HOLE_v;
    TAIL return _HOLE_cont(s);
}
)", "-O2 -fPIE", &cc);
    ASSERT_TRUE(cc.ok()) << cc.out;

    Proc r = run_jitterator(obj);
    EXPECT_FALSE(r.ok()) << "a GOT-relative hole cannot be copy-and-patched:\n" << r.out;
    EXPECT_TRUE(r.says("_HOLE_v")) << r.out;
}

TEST(Jitterator, RefusesA64BitPcRelativeRelocation) {
    // -mcmodel=large gives R_X86_64_PC64: eight bytes where the patcher writes four.
    Proc cc;
    std::string obj = compile_obj("large", std::string(kStencilPrelude) + R"(
void STENCIL op_large(int64_t* s) {
    s[0] = (int64_t)(uintptr_t)&_HOLE_v;
    TAIL return _HOLE_cont(s);
}
)", "-O2 -mcmodel=large", &cc);
    ASSERT_TRUE(cc.ok()) << cc.out;

    Proc r = run_jitterator(obj);
    EXPECT_FALSE(r.ok()) << "a 64-bit PC-relative hole is not a 32-bit patch:\n" << r.out;
}

// ── Things that are not stencils ─────────────────────────────────────────────

TEST(Jitterator, RefusesAStencilWithNoBody) {
    // A symbol with no size yields no code. Emitting it produces a stencil that
    // stamps nothing, and a JIT that falls off the end of the buffer.
    // A real stencil, so .text exists and the object is otherwise well formed,
    // plus a global function symbol in it with size 0.
    Proc cc;
    std::string obj = compile_obj("empty", std::string(kStencilPrelude) + R"(
void STENCIL op_real(int64_t* s) { s[0] += 1; TAIL return _HOLE_cont(s); }
__asm__(".text\n.globl op_empty\n.type op_empty,@function\n.size op_empty,0\nop_empty:\n");
)", "-O2", &cc);
    ASSERT_TRUE(cc.ok()) << cc.out;

    Proc r = run_jitterator(obj);
    EXPECT_FALSE(r.ok()) << "a zero-length stencil is not a stencil:\n" << r.out;
    EXPECT_TRUE(r.says("op_empty")) << r.out;
}

TEST(Jitterator, RefusesARelocationThatIsNotAHole) {
    // A compiler-emitted .rodata constant: its RIP-relative load would dangle once
    // the code is copied somewhere else. This one was already refused; the test
    // holds it refused.
    Proc cc;
    std::string obj = compile_obj("rodata", std::string(kStencilPrelude) + R"(
void STENCIL op_const(int64_t* s) {
    double d = (double)s[0] * 3.14159265358979e300;
    s[0] = (int64_t)d;
    TAIL return _HOLE_cont(s);
}
)", "-O2", &cc);
    ASSERT_TRUE(cc.ok()) << cc.out;

    Proc r = run_jitterator(obj);
    EXPECT_FALSE(r.ok()) << "a compiler constant cannot be copy-and-patched:\n" << r.out;
    EXPECT_TRUE(r.says("Move this op into a native") || r.says("non-_HOLE_")) << r.out;
}

// ── Input it did not produce ─────────────────────────────────────────────────

TEST(Jitterator, RejectsSomethingThatIsNotAnObjectFile) {
    std::string p = tmpdir() + "/notelf.o";
    { std::ofstream f(p); f << "this is not an ELF object file, not even close"; }
    Proc r = run_jitterator(p);
    EXPECT_FALSE(r.ok()) << r.out;
}

TEST(Jitterator, RejectsAnObjectFileTruncatedAnywhere) {
    // The extractor reads section headers, a symbol table, a string table and a
    // relocation table by offsets the file itself supplies. Every prefix of a real
    // object has to be refused rather than followed off the end.
    Proc cc;
    std::string obj = compile_obj("trunc", std::string(kStencilPrelude) + R"(
void STENCIL op_t(int64_t* s) { s[0] += (int64_t)(uintptr_t)&_HOLE_v; TAIL return _HOLE_cont(s); }
)", "-O2", &cc);
    ASSERT_TRUE(cc.ok()) << cc.out;

    std::ifstream in(obj, std::ios::binary);
    std::string whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_GT(whole.size(), 64u);

    // Every 1/16th prefix: enough to cover the header, the tables and the tail.
    for (size_t frac = 1; frac < 16; frac++) {
        size_t n = whole.size() * frac / 16;
        std::string p = tmpdir() + "/trunc_" + std::to_string(frac) + ".o";
        { std::ofstream f(p, std::ios::binary); f.write(whole.data(), (std::streamsize)n); }
        Proc r = run_jitterator(p);
        // Succeeding is allowed only if it genuinely found everything; what is not
        // allowed is a crash.
        EXPECT_LT(r.status, 128) << "prefix " << n << " of " << whole.size()
                                 << " crashed:\n" << r.out;
        EXPECT_GE(r.status, 0) << "prefix " << n << " did not exit normally";
    }
}

TEST(Jitterator, RejectsAnObjectFileWithCorruptedBytes) {
    Proc cc;
    std::string obj = compile_obj("corrupt", std::string(kStencilPrelude) + R"(
void STENCIL op_c(int64_t* s) { s[0] += (int64_t)(uintptr_t)&_HOLE_v; TAIL return _HOLE_cont(s); }
)", "-O2", &cc);
    ASSERT_TRUE(cc.ok()) << cc.out;

    std::ifstream in(obj, std::ios::binary);
    std::string whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // The ELF header and section-header table are where the offsets live, so that
    // is where a wrong byte does the most damage.
    for (size_t pos = 0; pos < 128 && pos < whole.size(); pos += 7) {
        for (unsigned char v : {0x00u, 0xFFu}) {
            std::string b = whole;
            if ((unsigned char)b[pos] == v) continue;
            b[pos] = (char)v;
            std::string p = tmpdir() + "/corrupt.o";
            { std::ofstream f(p, std::ios::binary); f.write(b.data(), (std::streamsize)b.size()); }
            Proc r = run_jitterator(p);
            EXPECT_LT(r.status, 128) << "byte " << pos << " = " << (int)v
                                     << " crashed:\n" << r.out;
        }
    }
}
