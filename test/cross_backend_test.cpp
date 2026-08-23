// cross_backend_test — the writer×reader cross-product (the Twofer net), CEK = oracle.
//
// For every rule in the fixture (the full CEK feature set): round-trip its authored
// bytes through the C backend (read → write), then judge the C WRITER's output bytes
// against the CEK two ways:
//   (1) writer fidelity — CEK(writer-output) decodes to the same value tree as
//       CEK(authored): the round-trip preserved every value (a dropped/misplaced
//       field diverges here; comparing two readers on the same output cannot see it).
//   (2) the Twofer — the C++ ZCow reader decodes the writer's bytes identically to the
//       CEK: a different reader (compiled stencils) agrees with the machine (interpreter)
//       on the C writer's output. Real bytes cross every backend boundary.
// Comparison is a generic recursive walk of the FieldCapture index, so it covers every
// field/element/arm/computed/span — not a hand-picked scalar or two.
#include <gtest/gtest.h>
#include "Parser.h"
#include "Sema.h"
#include "bbq_compile.h"
#include "Machine.h"
#include "CompilerCtx.h"
#include "RenderTypes.h"
#include "RenderEmit.h"
#include "WriterInterp.h"
#include "CTypeMapper.h"
#include "bbq_node.h"

// The generated zr:: view reader (ZcowReaderReader.h) defines its read functions
// NON-inline; render_view_parser_test owns that include. Forward-declare the entries
// we call here (resolved at link) to avoid multiply-defining them.
namespace zr {
#define ZR(R) bbq::CaptureMetadata R##_read(const uint8_t*, size_t, bbq::ParseArena&);
ZR(Flat) ZR(Nest) ZR(Arr) ZR(Pair) ZR(Outer) ZR(RArr) ZR(Bytes) ZR(Str) ZR(Wh) ZR(WhTwice)
ZR(Comp) ZR(Iv) ZR(Pos) ZR(Op) ZR(Bare) ZR(Sw) ZR(VA) ZR(VB) ZR(U) ZR(Alts)
ZR(Bf) ZR(InlineBf) ZR(Eof) ZR(Unt) ZR(Es) ZR(RsItem) ZR(Resync) ZR(Ext)
ZR(SwRange) ZR(Tern) ZR(Bstart) ZR(Rest) ZR(RestEof) ZR(Cnt) ZR(TopSw) ZR(Np) ZR(OScope)
ZR(AllPrim) ZR(Leb) ZR(LebArr) ZR(LebOpt) ZR(Mat) ZR(OptSpan)
ZR(TyByte) ZR(TyRule) ZR(OptTop) ZR(NestGrp) ZR(NestArr) ZR(FComp)
#undef ZR
}  // namespace zr

// The generated zr:: ZCow WRITER (the dual of the reader): `<Rule>_write(root, buf, zc)`
// re-emits the graph the reader built. Included here (the only consumer); defines the
// writer functions in this TU.
#include "ZcowReaderWriter.h"

#include <sys/wait.h>
#include <unordered_map>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace BBQ;
using namespace bbqgen;

namespace {

// `readit` (the C++ view extern) is defined in render_view_parser_test.cpp. The CEK
// extern mirrors it: consume 4 bytes, recording the span (the index records the consumed
// extent; the value lives in those bytes, which round-trip via the owning readit/writeit).
static bool cek_readit(const uint8_t*, size_t len, size_t* consumed, bbq::ParseArena*, void*) {
    if (len < 4) return false; *consumed = 4; return true;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// The fixture is the FULL CEK feature set, NO exclusions: if a writer or reader can't
// handle a rule, that is the truth this test must tell, never papered over by shrinking
// the denominator.
std::string fixture_for_c() {
    return read_file(std::string(SOURCE_DIR) + "/test/fixtures/zcow_reader.bbq");
}

// CEK reference (returns metadata so callers compare the index directly).
bbq::CaptureMetadata cek_meta_x(bbq::cek::CompiledGrammar* cg, const char* rule,
                                const uint8_t* buf, size_t len) {
    auto* arena = new bbq::ParseArena();
    auto* m = new bbq::cek::CEKMachine();
    m->arena = arena; m->builtins = &cg->builtins;
    auto* ext = new bbq::cek::ExternalParserTable::Entry[1];
    ext[0].name = cg->strings.intern(std::string("readit"));
    ext[0].fn = &cek_readit; ext[0].user_data = nullptr;
    auto* et = new bbq::cek::ExternalParserTable();
    et->entries = ext; et->count = 1; m->ext_parsers = et;
    return m->execute_from(cg->lookup(std::string(rule)), buf, len, cg->default_little_endian);
}

// Generate the C reader+writer for the fixture, plus a harness that — given a rule on
// argv and authored bytes on stdin — runs <rule>_read then <rule>_write and emits the
// WRITTEN bytes on stdout. One compile; run once per rule. EVERY rule is dispatched (the
// writer's uniform `const T*` signature is the dual of the reader's `T*`).
struct CRoundtrip {
    std::string bin;
    bbq::cek::CompiledGrammar* cek_cg = nullptr;
    bool ok = false;
    bool sanitized = false;   // built with a working ASan/UBSan runtime (OOB → abort)
    std::string err;
    // The writer op-list the cpp-zcow writer template was rendered from — the SAME list the
    // dynamic writer (WriterInterp) executes. Kept so the two consumers can be compared.
    nlohmann::json wops;
};

// A sanitizer-capable C compiler invocation, probed once. clang ships its own sanitizer
// runtime; gcc needs libasan installed (absent on some boxes). Falls back to an
// unsanitized build so the functional matrix still runs where no sanitizer links — the
// caller records whether the overrun tripwire is actually armed.
struct CC { std::string cmd; bool sanitized; };
const CC& sanitizer_cc() {
    static CC cc = [] {
        std::ofstream("/tmp/_bbq_san_probe.c") << "int main(void){return 0;}\n";
        const std::string san = " -std=c11 -g -fsanitize=address,undefined -fno-sanitize-recover=all";
        for (const char* comp : {"clang", "cc"}) {
            std::string probe = std::string(comp) + san +
                " /tmp/_bbq_san_probe.c -o /tmp/_bbq_san_probe 2>/dev/null";
            if (system(probe.c_str()) == 0) return CC{std::string(comp) + san, true};
        }
        return CC{"cc -std=c11", false};
    }();
    return cc;
}

CRoundtrip build_c_roundtrip(const std::vector<std::string>& rules) {
    CRoundtrip rt;
    std::string spec = fixture_for_c();

    auto* parser = new Parser();
    parser->init(spec.c_str(), (int)spec.size());
    if (!parser->parse()) { rt.err = "parse failed"; return rt; }
    auto* errs = new ErrorReporter();
    auto* sema = new Sema(*errs);
    if (!sema->analyze(parser->ast)) { rt.err = "sema failed"; return rt; }

    bbqgen_c::CTypeMapper ctypes(*sema, "");
    bbq::render::CompilerCtx ctx{parser->ast, sema, nullptr, ""};
    auto* g = new bbq::Compiler();
    auto* cg = g->compile_grammar(parser->ast);
    ctx.ir = cg;
    rt.cek_cg = cg;
    rt.wops = bbq::render::lower_writer_ops(ctx);

    std::string dir = "/tmp/bbq_cross_" + std::to_string(getpid());
    system(("mkdir -p " + dir).c_str());
    std::string tdir = std::string(SOURCE_DIR);
    std::string tmpl = tdir + "/backends/render/templates";

    { std::ofstream f(dir + "/t.h"); f << bbq::render::render_types_c(ctx, tmpl + "/types_c.inja"); }
    {
        std::ofstream f(dir + "/r.c");
        f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <stdlib.h>\n#include <string.h>\n";
        f << "bool readit(bbq_ctx_t* ctx, void* out);\n";
        f << bbq::render::render_reader_c(ctx, tmpl);
    }
    {
        std::ofstream f(dir + "/w.c");
        f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <string.h>\n";
        f << "bool writeit(bbq_write_ctx_t* ctx, const void* in);\n";
        f << bbq::render::render_writer_c(ctx, tmpl);
    }
    // Harness: read stdin, dispatch on argv[1], round-trip every rule, emit written bytes.
    {
        std::ofstream f(dir + "/h.c");
        f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <stdio.h>\n#include <stdlib.h>\n"
             "#include <string.h>\n";
        f << bbq::render::render_reader_decls(ctx);
        f << bbq::render::render_writer_decls(ctx);
        // The Ext rule's extern: read 4 bytes into the owning uint32 field, write them
        // back byte-exact (so the opaque span round-trips through the owning struct).
        f << "bool readit(bbq_ctx_t* ctx, void* out){ if(bbq_remaining(ctx)<4) return false;"
             " memcpy(out, ctx->data + bbq_pos(ctx), 4); bbq_seek(ctx, bbq_pos(ctx)+4); return true; }\n";
        f << "bool writeit(bbq_write_ctx_t* ctx, const void* in){ return bbq_write_bytes(ctx, (const uint8_t*)in, 4); }\n";
        // read_one: run the owning reader for `rule` over (b,n) → 1 accept / 0 reject. Under
        // ASan/UBSan an out-of-bounds read aborts the process (the bitcoins tripwire).
        f << "static int read_one(const char* rule, const uint8_t* b, size_t n){\n"
             "  bbq_ctx_t rc; bbq_ctx_init(&rc,b,n);\n";
        for (auto* rule : sema->sorted_rules()) {
            std::string nm = rule->name, ty = ctypes.type_name(nm), rd = ctypes.read_func(nm);
            f << "  if(!strcmp(rule,\"" << nm << "\")){ " << ty << " o; memset(&o,0,sizeof o);"
                 " return " << rd << "(&rc,&o)?1:0; }\n";
        }
        f << "  return -1;\n}\n";
        f << "int main(int argc, char** argv){\n"
             "  if(argc<2) return 9;\n"
             "  static uint8_t in[4096]; size_t n=fread(in,1,sizeof in,stdin);\n"
             "  if(argc>=3 && !strcmp(argv[2],\"neg\")){\n"
             "    for(size_t L=0; L<=n; L++) putchar(read_one(argv[1],in,L)?'1':'0');\n"
             "    static uint8_t bb[4096];\n"
             "    for(size_t p=0;p<n;p++){ uint8_t ds[3]={0x00,0xFF,(uint8_t)(in[p]+1)};\n"
             "      for(int d=0;d<3;d++){ if(ds[d]==in[p]) continue; memcpy(bb,in,n); bb[p]=ds[d];\n"
             "        putchar(read_one(argv[1],bb,n)?'1':'0'); } }\n"
             "    putchar('\\n'); return 0;\n"
             "  }\n"
             "  bbq_ctx_t rc; bbq_ctx_init(&rc,in,n);\n"
             "  bbq_write_ctx_t wc; bbq_write_ctx_init_growable(&wc,256);\n";
        for (auto* rule : sema->sorted_rules()) {
            std::string nm = rule->name;
            std::string rd = ctypes.read_func(nm);
            std::string wf = ctypes.write_func(nm);
            std::string ty = ctypes.type_name(nm);
            f << "  if(!strcmp(argv[1],\"" << nm << "\")){ " << ty << " o; memset(&o,0,sizeof o);"
                 " if(!" << rd << "(&rc,&o)) return 2;"
                 " if(!" << wf << "(&wc,&o)) return 3; }\n";
        }
        f << "  fwrite(wc.data,1,wc.pos,stdout); fflush(stdout);\n  return 0;\n}\n";
    }
    std::string rtdir = tdir + "/backends/c/runtime";
    // Sanitize the generated reader+writer where possible: the negative sweeps feed
    // adversarial bytes through this binary, and ASan/UBSan turn any out-of-bounds access
    // (the bitcoins surface) into a hard abort the test catches — not a silent corruption.
    rt.sanitized = sanitizer_cc().sanitized;
    std::string cc = sanitizer_cc().cmd + " -I" + dir + " -I" + rtdir +
                     " -I" + std::string(SOURCE_DIR) + "/crt " + dir + "/r.c " +
                     dir + "/w.c " + dir + "/h.c -o " + dir + "/bin 2>&1";
    FILE* p = popen(cc.c_str(), "r");
    char buf[8192] = {}; if (p) fread(buf, 1, sizeof buf - 1, p);
    int rc = p ? pclose(p) : -1;
    if (rc != 0) { rt.err = std::string("compile failed:\n") + buf; return rt; }
    rt.bin = dir + "/bin";
    rt.ok = true;
    return rt;
}

// Run the C round-trip for one rule, returning the C writer's output bytes.
std::vector<uint8_t> c_roundtrip(const std::string& bin, const char* rule,
                                 const std::vector<uint8_t>& in) {
    std::string cmd = "printf '";
    char hex[8];
    for (uint8_t b : in) { snprintf(hex, sizeof hex, "\\x%02x", b); cmd += hex; }
    cmd += "' | ASAN_OPTIONS=detect_leaks=0 " + bin + " " + rule;
    FILE* p = popen(cmd.c_str(), "r");
    std::vector<uint8_t> out;
    if (p) { int c; while ((c = fgetc(p)) != EOF) out.push_back((uint8_t)c); pclose(p); }
    return out;
}

// Generic recursive comparison of two FieldCapture index trees, each decoded through its
// OWN buffer (the writer may place bytes differently — e.g. interval zero-fill — so it is
// the decoded VALUES, structure, arm tags, computed values and span contents that must
// match, not the offsets). Covers every node shape: a writer that drops a field, picks the
// wrong arm, or mis-encodes a scalar diverges here.
void cmp_node(const std::string& path,
              const bbq::FieldCapture* a, const uint8_t* abuf,
              const bbq::FieldCapture* b, const uint8_t* bbuf) {
    using CT = bbq::CaptureType;
    ASSERT_NE(a, nullptr) << path; ASSERT_NE(b, nullptr) << path;
    ASSERT_EQ((int)a->type, (int)b->type) << path << " type";
    EXPECT_EQ(a->variant_tag, b->variant_tag) << path << " variant_tag";
    ASSERT_EQ(a->child_count, b->child_count) << path << " child_count";
    if (a->type == CT::Struct || a->type == CT::Array) {
        for (int i = 0; i < a->child_count; i++) {
            const char* an = a->children[i].name, *bn = b->children[i].name;
            EXPECT_EQ(std::string(an ? an : ""), std::string(bn ? bn : "")) << path << " name@" << i;
            cmp_node(path + "/" + (an ? an : std::to_string(i)),
                     &a->children[i], abuf, &b->children[i], bbuf);
        }
    } else if (a->type == CT::Computed) {
        ASSERT_NE(a->computed_value, nullptr) << path; ASSERT_NE(b->computed_value, nullptr) << path;
        ASSERT_EQ((int)a->computed_value->kind, (int)b->computed_value->kind) << path << " computed kind";
        using K = bbq::ComputedValue::Kind;
        switch (a->computed_value->kind) {
        case K::Int:    EXPECT_EQ(a->computed_value->i, b->computed_value->i) << path << " computed int"; break;
        case K::Bool:   EXPECT_EQ(a->computed_value->b, b->computed_value->b) << path << " computed bool"; break;
        case K::Float:  EXPECT_EQ(a->computed_value->f, b->computed_value->f) << path << " computed float"; break;
        case K::String: EXPECT_STREQ(a->computed_value->s, b->computed_value->s) << path << " computed string"; break;
        }
    } else if (a->type == CT::Bytes || a->type == CT::String || a->type == CT::External) {
        size_t al = a->end_offset - a->start_offset, bl = b->end_offset - b->start_offset;
        ASSERT_EQ(al, bl) << path << " span length";
        if (al) EXPECT_EQ(0, memcmp(abuf + a->start_offset, bbuf + b->start_offset, al)) << path << " span bytes";
    } else if (bbq::is_float_type(a->type)) {
        EXPECT_EQ(bbq::node_float(a, abuf), bbq::node_float(b, bbuf)) << path << " float";
    } else {
        EXPECT_EQ(bbq::node_int(a, abuf), bbq::node_int(b, bbuf)) << path << " int";
    }
}

// Run the sanitized harness in `neg` mode: it sweeps every truncation + byte-corruption
// of `in` through the owning reader and prints '1'/'0' (accept/reject) per variant. A
// clean sweep exits 0; an out-of-bounds read aborts the process (ASan) → crashed.
struct NegRun { std::string bits; bool crashed; };
NegRun run_bin_neg(const std::string& bin, const char* rule, const std::vector<uint8_t>& in) {
    std::string hexstr;
    char hex[8];
    for (uint8_t b : in) { snprintf(hex, sizeof hex, "\\x%02x", b); hexstr += hex; }
    // detect_leaks=0: the harness intentionally leaks the read structs (no frees), which
    // is not what this test is about — only an out-of-bounds access matters, and that
    // aborts with a SIGNAL under -fno-sanitize-recover (a leak merely exits non-zero).
    std::string cmd = "printf '" + hexstr + "' | ASAN_OPTIONS=detect_leaks=0 " +
                      bin + " " + rule + " neg 2>/dev/null";
    NegRun r{};
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { r.crashed = true; return r; }
    int ch;
    while ((ch = fgetc(p)) != EOF) if (ch != '\n') r.bits += (char)ch;
    int st = pclose(p);
    // With detect_leaks off, a clean sweep exits 0; a sanitizer-caught OOB aborts (gcc) or
    // exits non-zero (clang), and a raw segfault signals — all of which are "crashed".
    r.crashed = WIFSIGNALED(st) || !WIFEXITED(st) || WEXITSTATUS(st) != 0;
    return r;
}

// Byte string for a failure message — a GetPut/byte-equality mismatch is only readable
// as the two byte strings side by side.
std::string hexdump(const std::vector<uint8_t>& b) {
    std::string s;
    char hex[8];
    for (uint8_t x : b) { snprintf(hex, sizeof hex, "%02x ", x); s += hex; }
    if (!s.empty()) s.pop_back();
    return s;
}

using ViewReadFn = bbq::CaptureMetadata (*)(const uint8_t*, size_t, bbq::ParseArena&);
struct XCase { const char* rule; ViewReadFn read; std::vector<uint8_t> valid; };

// EVERY fixture rule + authored bytes (the same denominator as the view test's
// all_rule_cases — the full grammar feature set).
std::vector<XCase> xcases() {
    return {
        {"Flat", zr::Flat_read, {0x12,0x56,0x34,0x00,0x01,0x00,0x00,0x01,0x02,0xFC,0xFF,0xFF,0xFF}},
        {"Nest", zr::Nest_read, {0x07,0x09,0x00,0x0B}},
        {"Arr", zr::Arr_read, {0x03,0x10,0x00,0x20,0x00,0x30,0x00}},
        {"Pair", zr::Pair_read, {0x07,0x08}},
        {"Outer", zr::Outer_read, {0x55,0x07,0x08}},
        {"RArr", zr::RArr_read, {0x02,1,2,3,4}},
        {"Bytes", zr::Bytes_read, {0x03,0xAA,0xBB,0xCC}},
        {"Str", zr::Str_read, {0x61,0x62,0x63}},
        {"Wh", zr::Wh_read, {0x01,0x2A}},
        {"WhTwice", zr::WhTwice_read, {0x05,0x0A,0x2A}},   // lo=5, v=10 (5..13), hi=42
        {"Comp", zr::Comp_read, {0xA5}},
        {"Iv", zr::Iv_read, {0x02,0xFF,0x2A}},
        {"Pos", zr::Pos_read, {0x0A,0x0B}},
        {"Op", zr::Op_read, {0x01,0x34,0x12,0x05,0x06}},
        {"Bare", zr::Bare_read, {0x0A,0x2A}},
        {"Sw", zr::Sw_read, {0x01,0x34,0x12}},
        {"VA", zr::VA_read, {0x01,0x0A}},
        {"VB", zr::VB_read, {0x02,0x0B}},
        {"U", zr::U_read, {0x01,0x0A}},
        {"Alts", zr::Alts_read, {0x02,0x0B}},
        {"Bf", zr::Bf_read, {0xA5}},
        {"InlineBf", zr::InlineBf_read, {0x11,0xA5,0x22}},
        {"Eof", zr::Eof_read, {1,2,3,4}},
        {"Unt", zr::Unt_read, {1,2,3}},
        {"Es", zr::Es_read, {0x34,0x12,0x12,0x34}},
        {"RsItem", zr::RsItem_read, {0x05}},
        {"Resync", zr::Resync_read, {0x02,0x00,0x05,0x00,0x07}},
        {"Ext", zr::Ext_read, {0xAA,0x01,0x02,0x03,0x04,0xBB}},
        {"SwRange", zr::SwRange_read, {0x02,0x34,0x12}},
        {"Tern", zr::Tern_read, {0x05}},
        {"Bstart", zr::Bstart_read, {0x0A,0x0B}},
        {"Rest", zr::Rest_read, {0x02,0x34,0x12}},
        {"RestEof", zr::RestEof_read, {0x03,10,20,30}},
        {"Cnt", zr::Cnt_read, {0x03,10,20,30}},
        {"TopSw", zr::TopSw_read, {0x01,0x22}},
        {"Np", zr::Np_read, {0x03,0xAA,0xBB,0xCC}},
        {"OScope", zr::OScope_read, {0x03,0x03,0xFF,0x10,0x20,0x30}},
        {"AllPrim", zr::AllPrim_read, {0x12,0xFE,0x56,0x34,0x01,0x02,0xFD,0xFF,
            0x00,0x01,0x00,0x00,0x01,0x02,0x03,0x04,0xFC,0xFF,0xFF,0xFF,
            0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xFB,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0x00,0x00,0xC0,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x40,0x01}},
        {"Leb", zr::Leb_read, {0xAC,0x02,0x7B,0x42}},
        {"LebArr", zr::LebArr_read, {0x03,0x01,0xAC,0x02,0x05}},
        {"LebOpt", zr::LebOpt_read, {0x01,0xAC,0x02,0x42}},
        {"Mat", zr::Mat_read, {0x02,0x03,1,2,3,4,5,6}},
        {"OptSpan", zr::OptSpan_read, {0x01,0xAA,0xBB,0x61,0x62,0x63}},
        {"TyByte", zr::TyByte_read, {0x2A}},
        {"TyRule", zr::TyRule_read, {0x07,0x08}},
        {"OptTop", zr::OptTop_read, {0x2A}},
        {"NestGrp", zr::NestGrp_read, {0x02,0xAA,0xBB}},
        {"NestArr", zr::NestArr_read, {0x02, 0x01,0x11, 0x02,0x22,0x33}},
        {"FComp", zr::FComp_read, {0xC8}},   // x=200 → half=100.0 (float), big=true (bool)
    };
}

// rule → its ZCow writer (the dual of the per-rule read fn in xcases). Same denominator.
using ViewWriteFn = std::vector<uint8_t> (*)(const bbq::FieldCapture*, const uint8_t*, size_t, bbq::zcow*);
ViewWriteFn wfn_for(const std::string& r) {
    static const std::unordered_map<std::string, ViewWriteFn> m = {
#define ZW(R) {#R, zr::R##_write},
        ZW(Flat) ZW(Nest) ZW(Arr) ZW(Pair) ZW(Outer) ZW(RArr) ZW(Bytes) ZW(Str) ZW(Wh) ZW(WhTwice)
        ZW(Comp) ZW(Iv) ZW(Pos) ZW(Op) ZW(Bare) ZW(Sw) ZW(VA) ZW(VB) ZW(U) ZW(Alts)
        ZW(Bf) ZW(InlineBf) ZW(Eof) ZW(Unt) ZW(Es) ZW(RsItem) ZW(Resync) ZW(Ext)
        ZW(SwRange) ZW(Tern) ZW(Bstart) ZW(Rest) ZW(RestEof) ZW(Cnt) ZW(TopSw) ZW(Np) ZW(OScope)
        ZW(AllPrim) ZW(Leb) ZW(LebArr) ZW(LebOpt) ZW(Mat) ZW(OptSpan)
        ZW(TyByte) ZW(TyRule) ZW(OptTop) ZW(NestGrp) ZW(NestArr) ZW(FComp)
#undef ZW
    };
    auto it = m.find(r); return it == m.end() ? nullptr : it->second;
}

// Build the sanitized C reader+writer harness once for the whole fixture; every test
// shares it (the compile is the expensive part).
const CRoundtrip& shared_rt() {
    static CRoundtrip rt = [] {
        std::vector<std::string> rules;
        for (auto& c : xcases()) rules.push_back(c.rule);
        return build_c_roundtrip(rules);
    }();
    return rt;
}

// ── c-lite (C view) column ───────────────────────────────────────────────────
// c-lite produces an INDEX (a span tree), not round-tripped bytes — so it is judged
// against the CEK like the cpp view reader: a canonical preorder dump of {path, type,
// variant_tag, child_count, decoded value / computed / span} must match the CEK's. The
// dump schema is shared by the C harness (over bbq_field_capture) and the C++ dumper
// below (over bbq::FieldCapture) so the two faces of the same index can't drift. Run as
// an ASan/UBSan subprocess (hostile input is the headline property) like the C-owning gate.

// C++ dump of a CEK FieldCapture tree — byte-identical format to the C harness's dump_node.
void dump_cek_node(const std::string& path, const bbq::FieldCapture* nd,
                   const uint8_t* buf, std::ostream& o) {
    using CT = bbq::CaptureType;
    o << path << " t=" << (int)nd->type << " vt=" << nd->variant_tag << " ";
    if (nd->type == CT::Struct || nd->type == CT::Array) {
        o << "n=" << nd->child_count << "\n";
        for (int i = 0; i < nd->child_count; i++) {
            const bbq::FieldCapture* ch = &nd->children[i];
            // An unnamed child (array element: name null OR "") keys by index — the same
            // null/"" normalization cmp_node uses, so the C and CEK dumps agree.
            dump_cek_node(path + "/" + (ch->name && ch->name[0] ? std::string(ch->name) : std::to_string(i)),
                          ch, buf, o);
        }
    } else if (nd->type == CT::Computed) {
        const bbq::ComputedValue* cv = nd->computed_value;
        using K = bbq::ComputedValue::Kind;
        if (!cv) { o << "c?=0\n"; }
        else switch (cv->kind) {
            case K::Int:   o << "cI=" << cv->i << "\n"; break;
            case K::Bool:  o << "cB=" << (cv->b ? 1 : 0) << "\n"; break;
            case K::Float: { uint64_t b; memcpy(&b, &cv->f, 8); char h[20];
                             snprintf(h, sizeof h, "%016llx", (unsigned long long)b); o << "cF=" << h << "\n"; } break;
            case K::String: o << "cS=" << (cv->s ? cv->s : "") << "\n"; break;
        }
    } else if (nd->type == CT::Bytes || nd->type == CT::String || nd->type == CT::External) {
        o << "s=";
        char h[4];
        for (size_t i = nd->start_offset; i < nd->end_offset; i++) { snprintf(h, sizeof h, "%02x", buf[i]); o << h; }
        o << "\n";
    } else if (bbq::is_float_type(nd->type)) {
        double d = bbq::node_float(nd, buf); uint64_t b; memcpy(&b, &d, 8);
        char h[20]; snprintf(h, sizeof h, "%016llx", (unsigned long long)b); o << "f=" << h << "\n";
    } else {
        o << "i=" << bbq::node_int(nd, buf) << "\n";
    }
}
std::string dump_cek(const bbq::FieldCapture* root, const uint8_t* buf) {
    std::stringstream ss; dump_cek_node("$", root, buf, ss); return ss.str();
}

// Build the c-lite reader for the fixture + a sanitized harness: argv[1]=rule, bytes on
// stdin → normal mode emits the canonical index dump; `neg` mode sweeps every truncation +
// byte-corruption and prints accept/reject bits (the SAME sweep order as build_c_roundtrip,
// so run_bin_neg compares it to the CEK).
struct CLite { std::string bin; bool ok = false; bool sanitized = false; std::string err; };

CLite build_clite_index() {
    CLite rt;
    std::string spec = fixture_for_c();
    auto* parser = new Parser();
    parser->init(spec.c_str(), (int)spec.size());
    if (!parser->parse()) { rt.err = "parse failed"; return rt; }
    auto* errs = new ErrorReporter();
    auto* sema = new Sema(*errs);
    if (!sema->analyze(parser->ast)) { rt.err = "sema failed"; return rt; }

    bbq::render::CompilerCtx ctx{parser->ast, sema, nullptr, ""};
    auto* g = new bbq::Compiler();
    ctx.ir = g->compile_grammar(parser->ast);

    std::string dir = "/tmp/bbq_clite_" + std::to_string(getpid());
    system(("mkdir -p " + dir).c_str());
    std::string tdir = std::string(SOURCE_DIR);
    std::string tmpl = tdir + "/backends/render/templates";

    { std::ofstream f(dir + "/clr.c");
      f << "#include \"bbq_lite.h\"\n" << bbq::render::render_reader_view_c(ctx, tmpl); }

    // Per-rule read-fn dispatch (the c-lite read fn is <snake(rule)>_read, no prefix).
    std::string dispatch;
    for (auto* rule : sema->sorted_rules()) {
        std::string nm = rule->name, rd = bbqgen::to_snake_case(nm) + "_read";
        dispatch += "  if(!strcmp(rule,\"" + nm + "\")){ m = " + rd + "(b,n,&ar); }\n";
    }
    {
        std::ofstream f(dir + "/h.c");
        f << "#include \"bbq_lite.h\"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n";
        f << bbq::render::render_reader_view_c_decls(ctx);
        // The Ext rule's extern (view ABI): consume 4 bytes, like cek_readit.
        f << "bool readit(const uint8_t* d, size_t len, size_t* c){ (void)d; if(len<4) return 0; *c=4; return 1; }\n";
        f << "static int isf(bbq_capture_type t){ return t==BBQ_CT_Float32LE||t==BBQ_CT_Float32BE||t==BBQ_CT_Float64LE||t==BBQ_CT_Float64BE; }\n";
        f << "static void dump_node(const char* path, const bbq_field_capture* nd, const uint8_t* buf, FILE* o){\n"
             "  fprintf(o, \"%s t=%d vt=%d \", path, (int)nd->type, nd->variant_tag);\n"
             "  if(nd->type==BBQ_CT_Struct || nd->type==BBQ_CT_Array){\n"
             "    fprintf(o, \"n=%d\\n\", nd->child_count);\n"
             "    for(int i=0;i<nd->child_count;i++){ const bbq_field_capture* ch=&nd->children[i]; char p[1024];\n"
             "      if(ch->name && ch->name[0]) snprintf(p,sizeof p,\"%s/%s\",path,ch->name); else snprintf(p,sizeof p,\"%s/%d\",path,i);\n"
             "      dump_node(p, ch, buf, o); }\n"
             "  } else if(nd->type==BBQ_CT_Computed){ const bbq_computed_value* cv=nd->computed_value;\n"
             "    if(!cv) fprintf(o,\"c?=0\\n\");\n"
             "    else if(cv->kind==BBQ_CV_INT) fprintf(o,\"cI=%lld\\n\",(long long)cv->i);\n"
             "    else if(cv->kind==BBQ_CV_BOOL) fprintf(o,\"cB=%d\\n\",cv->b?1:0);\n"
             "    else if(cv->kind==BBQ_CV_FLOAT){ uint64_t b; memcpy(&b,&cv->f,8); fprintf(o,\"cF=%016llx\\n\",(unsigned long long)b); }\n"
             "    else fprintf(o,\"cS=%s\\n\",cv->s?cv->s:\"\");\n"
             "  } else if(nd->type==BBQ_CT_Bytes||nd->type==BBQ_CT_String||nd->type==BBQ_CT_External){\n"
             "    fprintf(o,\"s=\"); for(size_t i=nd->start_offset;i<nd->end_offset;i++) fprintf(o,\"%02x\",buf[i]); fprintf(o,\"\\n\");\n"
             "  } else if(isf(nd->type)){ double d=bbq_node_float(nd,buf); uint64_t b; memcpy(&b,&d,8); fprintf(o,\"f=%016llx\\n\",(unsigned long long)b);\n"
             "  } else { fprintf(o,\"i=%lld\\n\",(long long)bbq_node_int(nd,buf)); }\n"
             "}\n";
        f << "static int read_one(const char* rule, const uint8_t* b, size_t n){\n"
             "  bbq_arena ar; bbq_arena_init(&ar,0); bbq_capture_metadata m; m.success=0; m.root=0;\n"
          << dispatch <<
             "  int ok = m.success?1:0; bbq_arena_free(&ar); return ok;\n}\n";
        f << "int main(int argc, char** argv){\n"
             "  if(argc<2) return 9;\n"
             "  static uint8_t in[4096]; size_t n=fread(in,1,sizeof in,stdin);\n"
             "  if(argc>=3 && !strcmp(argv[2],\"neg\")){\n"
             "    for(size_t L=0; L<=n; L++) putchar(read_one(argv[1],in,L)?'1':'0');\n"
             "    static uint8_t bb[4096];\n"
             "    for(size_t p=0;p<n;p++){ uint8_t ds[3]={0x00,0xFF,(uint8_t)(in[p]+1)};\n"
             "      for(int d=0;d<3;d++){ if(ds[d]==in[p]) continue; memcpy(bb,in,n); bb[p]=ds[d];\n"
             "        putchar(read_one(argv[1],bb,n)?'1':'0'); } }\n"
             "    putchar('\\n'); return 0;\n"
             "  }\n"
             "  const char* rule = argv[1]; const uint8_t* b = in;\n"
             "  bbq_arena ar; bbq_arena_init(&ar,0); bbq_capture_metadata m; m.success=0; m.root=0;\n"
          << dispatch <<
             "  if(!m.success) return 2;\n"
             "  dump_node(\"$\", m.root, in, stdout); fflush(stdout); return 0;\n}\n";
    }
    std::string rtdir = tdir + "/backends/c/runtime";
    std::string crtdir = tdir + "/crt";
    rt.sanitized = sanitizer_cc().sanitized;
    std::string cc = sanitizer_cc().cmd + " -I" + dir + " -I" + rtdir + " -I" + crtdir + " " +
                     dir + "/clr.c " + dir + "/h.c " + rtdir + "/bbq_lite.c " +
                     crtdir + "/bbq_arena.c -o " + dir + "/bin 2>&1";
    FILE* p = popen(cc.c_str(), "r");
    char buf[8192] = {}; if (p) fread(buf, 1, sizeof buf - 1, p);
    int rc = p ? pclose(p) : -1;
    if (rc != 0) { rt.err = std::string("compile failed:\n") + buf; return rt; }
    rt.bin = dir + "/bin"; rt.ok = true;
    return rt;
}

const CLite& shared_clite() {
    static CLite rt = build_clite_index();
    return rt;
}

}  // namespace

TEST(CrossBackend, CWriterToCppReaderFullFixture) {
    const CRoundtrip& rt = shared_rt();
    ASSERT_TRUE(rt.ok) << rt.err;

    for (auto& c : xcases()) {
        SCOPED_TRACE(c.rule);
        // The authored bytes' truth, per the oracle.
        bbq::CaptureMetadata ck_auth = cek_meta_x(rt.cek_cg, c.rule, c.valid.data(), c.valid.size());
        ASSERT_TRUE(ck_auth.success) << c.rule << ": CEK rejected authored bytes";

        // Round-trip through the C backend: read authored → write from the owning struct.
        std::vector<uint8_t> cb = c_roundtrip(rt.bin, c.rule, c.valid);
        ASSERT_FALSE(cb.empty()) << c.rule << ": C round-trip produced no bytes";
        auto* buf = new uint8_t[cb.size()];
        std::copy(cb.begin(), cb.end(), buf);

        // The oracle and the C++ ZCow reader both decode the C WRITER's output.
        bbq::CaptureMetadata ck_out = cek_meta_x(rt.cek_cg, c.rule, buf, cb.size());
        ASSERT_TRUE(ck_out.success) << c.rule << ": CEK rejected C-writer bytes";
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = c.read(buf, cb.size(), arena);
        ASSERT_TRUE(vm.success) << c.rule << ": C++ reader rejected C-writer bytes";

        // (1) writer fidelity: the writer's output decodes to the authored value tree.
        // (Including `until(@remaining)` arrays: burg matched the terminator shape and
        // the writer reserves the trailing context, so even those round-trip — no skip.)
        cmp_node(std::string(c.rule) + " (writer-fidelity)",
                 ck_out.root, buf, ck_auth.root, c.valid.data());
        // (2) Twofer: the C++ reader agrees with the CEK on the writer's bytes.
        cmp_node(std::string(c.rule) + " (cpp-reader vs cek)",
                 vm.root, buf, ck_out.root, buf);

        // (3) Negative axis on the writer's OWN output: truncate every prefix and corrupt
        // every byte; the C++ ZCow reader must accept iff the CEK does, never overrun or
        // crash (run under ASan/UBSan). The writer's bytes are the realistic attack
        // surface a reader sees — this is the bitcoins net.
        for (size_t L = 0; L <= cb.size(); L++) {
            bbq::ParseArena na;
            bbq::CaptureMetadata nv = c.read(buf, L, na);
            bbq::CaptureMetadata nk = cek_meta_x(rt.cek_cg, c.rule, buf, L);
            EXPECT_EQ(nv.success, nk.success)
                << c.rule << ": truncated writer output @ " << L << "/" << cb.size()
                << " (cpp=" << nv.success << " cek=" << nk.success << ")";
        }
        for (size_t pos = 0; pos < cb.size(); pos++)
            for (uint8_t delta : {uint8_t(0x00), uint8_t(0xFF), uint8_t(cb[pos] + 1)}) {
                if (delta == cb[pos]) continue;
                std::vector<uint8_t> bad(cb);
                bad[pos] = delta;
                auto* bb = new uint8_t[bad.size()];
                std::copy(bad.begin(), bad.end(), bb);
                bbq::ParseArena na;
                bbq::CaptureMetadata nv = c.read(bb, bad.size(), na);
                bbq::CaptureMetadata nk = cek_meta_x(rt.cek_cg, c.rule, bb, bad.size());
                EXPECT_EQ(nv.success, nk.success)
                    << c.rule << ": corrupt writer output byte[" << pos << "]=0x"
                    << std::hex << (int)delta << " (cpp=" << nv.success << " cek=" << nk.success << ")";
                delete[] bb;
            }
    }
}

// ── The C++ ZCow WRITER column (the dual of the C-writer column). For every fixture rule:
// the ZCow reader builds the graph from authored bytes; the ZCow writer re-emits it; those
// bytes must decode (CEK oracle) to the SAME value tree as the authored bytes, AND the C
// owning reader must accept+round-trip them (CppWriter → CReader). The writer enforces the
// grammar over the graph; ZCow is the storage underneath. A divergence is a writer bug.
TEST(CrossBackend, CppWriterToCReaderFullFixture) {
    const CRoundtrip& rt = shared_rt();
    ASSERT_TRUE(rt.ok) << rt.err;

    for (auto& c : xcases()) {
        SCOPED_TRACE(c.rule);
        ViewWriteFn wfn = wfn_for(c.rule);
        ASSERT_NE(wfn, nullptr) << c.rule << ": no ZCow writer";

        bbq::CaptureMetadata ck_auth = cek_meta_x(rt.cek_cg, c.rule, c.valid.data(), c.valid.size());
        ASSERT_TRUE(ck_auth.success) << c.rule << ": CEK rejected authored bytes";

        // Build the graph (ZCow reader), then re-emit it (ZCow writer, unmutated).
        bbq::ParseArena arena;
        bbq::CaptureMetadata vm = c.read(c.valid.data(), c.valid.size(), arena);
        ASSERT_TRUE(vm.success) << c.rule << ": ZCow reader rejected authored bytes";
        std::vector<uint8_t> wb = wfn(vm.root, c.valid.data(), c.valid.size(), nullptr);
        ASSERT_FALSE(wb.empty()) << c.rule << ": ZCow writer produced no bytes";
        auto* wbuf = new uint8_t[wb.size()]; std::copy(wb.begin(), wb.end(), wbuf);

        // (0) GetPut — put(get(c), c) = c. With no edit the lens must return the input
        // BYTE for byte, not merely something that decodes to the same values: the whole
        // point of keeping the baseline (the Borrowed spans emit() blits) is that the
        // information `get` discarded survives `put`. cmp_node below cannot see a lost
        // constant/padding byte, so the law needs its own assertion.
        EXPECT_EQ(wb, c.valid) << c.rule << ": GetPut violated — unedited re-emit is not byte-identical\n"
                               << "  in : " << hexdump(c.valid) << "\n  out: " << hexdump(wb);

        // (0b) The DYNAMIC writer (WriterInterp) EXECUTES the same op-list these stencils
        // were RENDERED from. Byte-equality on every fixture rule, so every construct's
        // enforcement path — switch, choice, optional, resync, bitfield, intervals — is
        // held to the generated writer, not just the rules the mutate table edits.
        std::string ierr;
        std::vector<uint8_t> ib = bbq::render::run_writer(rt.wops, c.rule, vm.root,
                                                          c.valid.data(), c.valid.size(), nullptr, &ierr);
        ASSERT_FALSE(ib.empty()) << c.rule << ": dynamic writer produced no bytes: " << ierr;
        EXPECT_EQ(ib, wb) << c.rule << ": dynamic writer diverges from the generated stencils\n"
                          << "  gen: " << hexdump(wb) << "\n  dyn: " << hexdump(ib);

        // (1) Fidelity: the ZCow writer's bytes decode (oracle) to the authored value tree.
        bbq::CaptureMetadata ck_out = cek_meta_x(rt.cek_cg, c.rule, wbuf, wb.size());
        ASSERT_TRUE(ck_out.success) << c.rule << ": CEK rejected ZCow-writer bytes";
        cmp_node(std::string(c.rule) + " (zcow-writer fidelity)",
                 ck_out.root, wbuf, ck_auth.root, c.valid.data());

        // (2) CppWriter → CReader: the C owning reader reads the ZCow-writer's bytes and
        // round-trips them; the result decodes to the authored value tree too.
        std::vector<uint8_t> cb = c_roundtrip(rt.bin, c.rule, wb);
        ASSERT_FALSE(cb.empty()) << c.rule << ": C reader rejected ZCow-writer bytes";
        auto* cbuf = new uint8_t[cb.size()]; std::copy(cb.begin(), cb.end(), cbuf);
        bbq::CaptureMetadata ck_c = cek_meta_x(rt.cek_cg, c.rule, cbuf, cb.size());
        ASSERT_TRUE(ck_c.success) << c.rule << ": CEK rejected C-reader round-trip of ZCow-writer bytes";
        cmp_node(std::string(c.rule) + " (cpp-writer -> c-reader)",
                 ck_c.root, cbuf, ck_auth.root, c.valid.data());

        // (3) Negative axis (plan completion bar): truncate every prefix + corrupt every byte
        // of the ZCow-WRITER's output; the C owning reader must accept iff the CEK does, never
        // overrun/crash (run under ASan/UBSan). The C-writer column already sweeps this on ITS
        // output; this is the dual on the ZCow-writer's, the realistic surface a C reader sees.
        std::string cek_bits;   // CEK accept/reject in the SAME variant order run_bin_neg sweeps
        for (size_t L = 0; L <= wb.size(); L++)
            cek_bits += cek_meta_x(rt.cek_cg, c.rule, wbuf, L).success ? '1' : '0';
        for (size_t pos = 0; pos < wb.size(); pos++) {
            uint8_t ds[3] = {0x00, 0xFF, uint8_t(wb[pos] + 1)};
            for (int d = 0; d < 3; d++) {
                if (ds[d] == wb[pos]) continue;
                std::vector<uint8_t> bad(wb); bad[pos] = ds[d];
                cek_bits += cek_meta_x(rt.cek_cg, c.rule, bad.data(), bad.size()).success ? '1' : '0';
            }
        }
        NegRun nr = run_bin_neg(rt.bin, c.rule, wb);
        if (rt.sanitized)
            EXPECT_FALSE(nr.crashed)
                << c.rule << ": C reader overran/crashed on malformed ZCow-writer output (ASan/UBSan)";
        if (!nr.crashed)
            EXPECT_EQ(nr.bits, cek_bits)
                << c.rule << ": C reader accept/reject disagrees with CEK on ZCow-writer output\n  c="
                << nr.bits << "\n  k=" << cek_bits;
    }
}

// ── (PUT): put(A × C) ⊆ C. Foster et al. Def 3.2 types a well-behaved lens with `put`
// landing back in C, not just with GetPut/PutGet — and that typing is the one the walk
// cannot establish, because it replays the shape the PARSE recorded. Change a switch
// discriminant and the recorded arm no longer matches: the bytes are not a document this
// grammar accepts. PutGet does NOT catch it (`⊑` is vacuous when its left side is
// undefined), so the writer has to test membership itself and REFUSE.
TEST(CrossBackend, WriterRefusesWhenTheEditLeavesTheGrammar) {
    const CRoundtrip& rt = shared_rt();
    ASSERT_TRUE(rt.ok) << rt.err;

    // Sw = struct { tag: uint8, body: switch(tag) { 1: uint16le; 2: Pair; default: uint8; } }
    // Authored with tag=1 (a 2-byte body). Retag to 2 and the body must be a Pair — which
    // it still is by width, so the shape survives; retag to 9 takes the default arm, one
    // byte, leaving a trailing byte the grammar cannot place.
    const std::vector<uint8_t> v{0x01, 0x34, 0x12};
    bbq::ParseArena ar;
    bbq::CaptureMetadata vm = zr::Sw_read(v.data(), v.size(), ar);
    ASSERT_TRUE(vm.success);

    bbq::zcow zc;
    zc.set_int(bbq::node_child(vm.root, "tag"), 9);        // now the default (1-byte) arm
    std::vector<uint8_t> wb = zr::Sw_write(vm.root, v.data(), v.size(), &zc);
    EXPECT_TRUE(wb.empty())
        << "generated ZCow writer emitted a document outside the grammar: " << hexdump(wb);

    // The dynamic writer must refuse for the same reason. Its membership test is a
    // callback, since the parser differs per face — here, the CEK.
    bbq::ParseArena ar2;
    bbq::CaptureMetadata vm2 = zr::Sw_read(v.data(), v.size(), ar2);
    ASSERT_TRUE(vm2.success);
    bbq::zcow zc2;
    zc2.set_int(bbq::node_child(vm2.root, "tag"), 9);
    std::string ierr;
    const size_t tail_before = v.size() > vm2.root->end_offset ? v.size() - vm2.root->end_offset : 0;
    auto verify = [&](const uint8_t* d, size_t n, std::string* why) {
        bbq::CaptureMetadata m = cek_meta_x(rt.cek_cg, "Sw", d, n);
        if (!m.success) { if (why) *why = m.error_message ? m.error_message : "parse failed"; return false; }
        size_t tail_after = n > m.bytes_consumed ? n - m.bytes_consumed : 0;
        if (tail_after > tail_before) { if (why) *why = "trailing bytes"; return false; }
        return true;
    };
    std::vector<uint8_t> ib = bbq::render::run_writer(rt.wops, "Sw", vm2.root, v.data(), v.size(),
                                                      &zc2, &ierr, verify);
    EXPECT_TRUE(ib.empty()) << "dynamic writer emitted a document outside the grammar: " << hexdump(ib);
    EXPECT_NE(ierr.find("does not produce a document"), std::string::npos) << "unhelpful: " << ierr;

    // And a legal edit still goes through both — the refusal is not a blanket veto.
    bbq::ParseArena ar3;
    bbq::CaptureMetadata vm3 = zr::Sw_read(v.data(), v.size(), ar3);
    bbq::zcow zc3;
    zc3.set_int(&bbq::node_child(vm3.root, "body")[0], 0x1234);
    EXPECT_FALSE(zr::Sw_write(vm3.root, v.data(), v.size(), &zc3).empty())
        << "generated writer refused a legal same-shape edit";
}

// ── The MUTATE axis — the whole point of ZCow. Parse → graph → CoW-edit via the zcow
// overlay → ZCow writer re-emits → the bytes decode (CEK oracle) to the EDITED value tree,
// derived fields (counts) consistent. Value edit, array remove, array append.
TEST(CrossBackend, CppWriterMutateRoundTrip) {
    const CRoundtrip& rt = shared_rt();
    ASSERT_TRUE(rt.ok) << rt.err;
    auto reemit = [&](const char* rule, ViewReadFn rd, ViewWriteFn wr, const std::vector<uint8_t>& v,
                      std::function<void(const bbq::FieldCapture*, bbq::zcow&)> mut,
                      std::function<void(const bbq::CaptureMetadata&, const uint8_t*)> check) {
        bbq::ParseArena ar; bbq::CaptureMetadata vm = rd(v.data(), v.size(), ar);
        ASSERT_TRUE(vm.success) << rule << ": reader rejected authored";
        bbq::zcow zc; mut(vm.root, zc);
        std::vector<uint8_t> wb = wr(vm.root, v.data(), v.size(), &zc);
        ASSERT_FALSE(wb.empty()) << rule << ": writer produced no bytes";

        // The dynamic writer, given the SAME edit from an independent parse + overlay (the
        // first writer already wrote its recomputed counts into `zc`, so reusing it would
        // hand the interpreter an answer instead of asking for one).
        bbq::ParseArena ar2; bbq::CaptureMetadata vm2 = rd(v.data(), v.size(), ar2);
        ASSERT_TRUE(vm2.success) << rule << ": reader rejected authored (dynamic pass)";
        bbq::zcow zc2; mut(vm2.root, zc2);
        std::string ierr;
        std::vector<uint8_t> ib = bbq::render::run_writer(rt.wops, rule, vm2.root,
                                                          v.data(), v.size(), &zc2, &ierr);
        ASSERT_FALSE(ib.empty()) << rule << ": dynamic writer produced no bytes: " << ierr;
        EXPECT_EQ(ib, wb) << rule << ": dynamic writer diverges from the generated stencils\n"
                          << "  gen: " << hexdump(wb) << "\n  dyn: " << hexdump(ib);

        auto* b = new uint8_t[wb.size()]; std::copy(wb.begin(), wb.end(), b);
        bbq::CaptureMetadata ck = cek_meta_x(rt.cek_cg, rule, b, wb.size());
        ASSERT_TRUE(ck.success) << rule << ": CEK rejected the re-emitted (edited) bytes";
        check(ck, b);
    };

    // (1) same-width scalar value edit: Flat.u8 → 0x99.
    { SCOPED_TRACE("Flat.u8 = 0x99");
      reemit("Flat", zr::Flat_read, zr::Flat_write,
             {0x12,0x56,0x34,0x00,0x01,0x00,0x00,0x01,0x02,0xFC,0xFF,0xFF,0xFF},
             [](const bbq::FieldCapture* r, bbq::zcow& zc){ zc.set_int(bbq::node_child(r,"u8"), 0x99); },
             [](const bbq::CaptureMetadata& ck, const uint8_t* b){
                 EXPECT_EQ(bbq::node_int(bbq::node_child(ck.root,"u8"), b), 0x99);
                 EXPECT_EQ(bbq::node_int(bbq::node_child(ck.root,"u16"), b), 0x3456); }); }

    // (2) array element value edit: Arr.xs[1] → 0x0099 (structure unchanged).
    { SCOPED_TRACE("Arr.xs[1] = 0x99");
      reemit("Arr", zr::Arr_read, zr::Arr_write, {0x03,0x10,0x00,0x20,0x00,0x30,0x00},
             [](const bbq::FieldCapture* r, bbq::zcow& zc){
                 const bbq::FieldCapture* xs = bbq::node_child(r,"xs"); zc.set_int(&xs->children[1], 0x0099); },
             [](const bbq::CaptureMetadata& ck, const uint8_t* b){
                 const bbq::FieldCapture* xs = bbq::node_child(ck.root,"xs");
                 ASSERT_EQ(xs->child_count, 3); EXPECT_EQ(bbq::node_int(&xs->children[1], b), 0x0099); }); }

    // (3) array REMOVE: Arr.xs remove[0] → n=2, xs=[0x20,0x30] (count must update).
    { SCOPED_TRACE("Arr.xs remove[0]");
      reemit("Arr", zr::Arr_read, zr::Arr_write, {0x03,0x10,0x00,0x20,0x00,0x30,0x00},
             [](const bbq::FieldCapture* r, bbq::zcow& zc){ zc.remove_index(bbq::node_child(r,"xs"), 0); },
             [](const bbq::CaptureMetadata& ck, const uint8_t* b){
                 EXPECT_EQ(bbq::node_int(bbq::node_child(ck.root,"n"), b), 2);
                 const bbq::FieldCapture* xs = bbq::node_child(ck.root,"xs");
                 ASSERT_EQ(xs->child_count, 2);
                 EXPECT_EQ(bbq::node_int(&xs->children[0], b), 0x0020);
                 EXPECT_EQ(bbq::node_int(&xs->children[1], b), 0x0030); }); }

    // (4) array APPEND: Arr.xs += 0x0040 → n=4 (count + the new element).
    { SCOPED_TRACE("Arr.xs append 0x40");
      reemit("Arr", zr::Arr_read, zr::Arr_write, {0x03,0x10,0x00,0x20,0x00,0x30,0x00},
             [](const bbq::FieldCapture* r, bbq::zcow& zc){
                 auto e = std::make_unique<bbq::znode>();
                 e->kind = bbq::znode::Kind::Scalar; e->type = bbq::CaptureType::UInt16LE; e->ival = 0x0040;
                 zc.append(bbq::node_child(r,"xs"), std::move(e)); },
             [](const bbq::CaptureMetadata& ck, const uint8_t* b){
                 EXPECT_EQ(bbq::node_int(bbq::node_child(ck.root,"n"), b), 4);
                 const bbq::FieldCapture* xs = bbq::node_child(ck.root,"xs");
                 ASSERT_EQ(xs->child_count, 4);
                 EXPECT_EQ(bbq::node_int(&xs->children[3], b), 0x0040); }); }

    // (5) PATH count `[h.n]`: Np.xs append 0xDD → h.n recomputed to 4 (the count field lives
    // in a sub-struct; mark_count records it in `h`'s scope, the array sets it). Dual of the
    // C writer's PathCountRecomputedOnModify.
    { SCOPED_TRACE("Np.xs append 0xDD (path count h.n)");
      reemit("Np", zr::Np_read, zr::Np_write, {0x03,0xAA,0xBB,0xCC},
             [](const bbq::FieldCapture* r, bbq::zcow& zc){
                 auto e = std::make_unique<bbq::znode>();
                 e->kind = bbq::znode::Kind::Scalar; e->type = bbq::CaptureType::UInt8; e->ival = 0xDD;
                 zc.append(bbq::node_child(r,"xs"), std::move(e)); },
             [](const bbq::CaptureMetadata& ck, const uint8_t* b){
                 EXPECT_EQ(bbq::node_int(bbq::node_child(bbq::node_child(ck.root,"h"),"n"), b), 4);
                 const bbq::FieldCapture* xs = bbq::node_child(ck.root,"xs");
                 ASSERT_EQ(xs->child_count, 4);
                 EXPECT_EQ(bbq::node_int(&xs->children[3], b), 0xDD); }); }

    // (6) @rest recompute: RestEof.xs += 40 → sz must become 4 (the window's new byte length),
    // NOT the stored 3. The window holds an eof array, so the edit changes its length; the
    // writer recomputes the @rest size from the edited content. Dual of RestSizeRecomputedOnModify.
    { SCOPED_TRACE("RestEof.xs append 40 (@rest size)");
      reemit("RestEof", zr::RestEof_read, zr::RestEof_write, {0x03,10,20,30},
             [](const bbq::FieldCapture* r, bbq::zcow& zc){
                 auto e = std::make_unique<bbq::znode>();
                 e->kind = bbq::znode::Kind::Scalar; e->type = bbq::CaptureType::UInt8; e->ival = 40;
                 zc.append(bbq::node_child(r,"xs"), std::move(e)); },
             [](const bbq::CaptureMetadata& ck, const uint8_t* b){
                 EXPECT_EQ(bbq::node_int(bbq::node_child(ck.root,"sz"), b), 4);
                 const bbq::FieldCapture* xs = bbq::node_child(ck.root,"xs");
                 ASSERT_EQ(xs->child_count, 4);
                 EXPECT_EQ(bbq::node_int(&xs->children[3], b), 40); }); }

    // (7) NESTED per-element count: NestArr.gs[0].xs += 0x99 → gs[0].n recomputed to 2 (the
    // inner array's count, NOT the outer gs count which stays 2). Proves the walk DESCENDS into
    // element bodies — emit() alone can't fix an inner count it's blind to.
    { SCOPED_TRACE("NestArr.gs[0].xs append 0x99 (nested count)");
      reemit("NestArr", zr::NestArr_read, zr::NestArr_write, {0x02, 0x01,0x11, 0x02,0x22,0x33},
             [](const bbq::FieldCapture* r, bbq::zcow& zc){
                 const bbq::FieldCapture* gs = bbq::node_child(r,"gs");
                 const bbq::FieldCapture* xs0 = bbq::node_child(&gs->children[0],"xs");
                 auto e = std::make_unique<bbq::znode>();
                 e->kind = bbq::znode::Kind::Scalar; e->type = bbq::CaptureType::UInt8; e->ival = 0x99;
                 zc.append(xs0, std::move(e)); },
             [](const bbq::CaptureMetadata& ck, const uint8_t* b){
                 EXPECT_EQ(bbq::node_int(bbq::node_child(ck.root,"m"), b), 2);   // outer count unchanged
                 const bbq::FieldCapture* gs = bbq::node_child(ck.root,"gs");
                 ASSERT_EQ(gs->child_count, 2);
                 EXPECT_EQ(bbq::node_int(bbq::node_child(&gs->children[0],"n"), b), 2);   // inner count fixed
                 const bbq::FieldCapture* xs0 = bbq::node_child(&gs->children[0],"xs");
                 ASSERT_EQ(xs0->child_count, 2);
                 EXPECT_EQ(bbq::node_int(&xs0->children[1], b), 0x99);
                 EXPECT_EQ(bbq::node_int(bbq::node_child(&gs->children[1],"n"), b), 2); }); }   // sibling intact
}

// Meta-check: every rule in the fixture (i.e. every grammar feature it exercises) must
// have a cross-backend case. Adding a rule to the fixture without a test makes THIS test
// fail — so "no test for it" is a red build, not a silent skip. Every column (C writer,
// C++ ZCow writer, C-owning reader, AND the c-lite view reader) iterates this same
// xcases() denominator, so a covered rule is covered on every column — including c-lite.
TEST(CrossBackend, EveryFixtureRuleHasACase) {
    std::string spec = fixture_for_c();
    auto* parser = new Parser();
    parser->init(spec.c_str(), (int)spec.size());
    ASSERT_TRUE(parser->parse());
    auto* errs = new ErrorReporter();
    auto* sema = new Sema(*errs);
    ASSERT_TRUE(sema->analyze(parser->ast));

    std::set<std::string> covered;
    for (auto& c : xcases()) covered.insert(c.rule);

    std::string missing;
    for (auto* r : sema->sorted_rules())
        if (!covered.count(r->name)) missing += std::string(missing.empty() ? "" : ", ") + r->name;
    EXPECT_TRUE(missing.empty())
        << "fixture rules with NO cross-backend case (untested ≠ optional): " << missing;
}

// ── c-lite POSITIVE: the C view reader's index == the CEK's, per fixture rule. The
// canonical preorder dump (shared schema) of the c-lite index must equal the CEK's —
// every field/element/arm/computed/span/decoded value, the same generic compare the
// other columns use, just dump-vs-dump (c-lite produces an index, not bytes).
TEST(CrossBackend, ViewCReaderMatchesCek) {
    const CLite& cl = shared_clite();
    ASSERT_TRUE(cl.ok) << cl.err;
    const CRoundtrip& rt = shared_rt();   // the CEK oracle grammar
    ASSERT_TRUE(rt.ok) << rt.err;

    for (auto& c : xcases()) {
        SCOPED_TRACE(c.rule);
        bbq::CaptureMetadata ck = cek_meta_x(rt.cek_cg, c.rule, c.valid.data(), c.valid.size());
        ASSERT_TRUE(ck.success) << c.rule << ": CEK rejected authored bytes";
        std::string cek_d = dump_cek(ck.root, c.valid.data());

        std::vector<uint8_t> out = c_roundtrip(cl.bin, c.rule, c.valid);   // normal mode → index dump
        std::string cl_d(out.begin(), out.end());
        EXPECT_EQ(cl_d, cek_d) << c.rule << ": c-lite index dump diverges from the CEK\n"
                               << "  c-lite:\n" << cl_d << "  cek:\n" << cek_d;
    }
}

// ── c-lite: a field named k times in one predicate is SEARCHED ONCE. ─────────────
// The c-lite field_ref is bbq_view_i64(ctx, "<name>") — a by-name search of the capture
// index, not a struct member. Spelling it per occurrence makes a k-term predicate pay k
// searches for an answer that cannot differ between them, and the index-vs-CEK gates above
// stay green either way: the cost is invisible to behaviour. WhTwice names `v` twice and
// `lo` twice, so its stencil must hold one search apiece, with the predicate reading the
// locals. Rules that name a field once keep the direct call — nothing to hoist.
TEST(CrossBackend, ViewCReaderSearchesARepeatedFieldOnce) {
    std::string spec = fixture_for_c();
    auto* parser = new Parser();
    parser->init(spec.c_str(), (int)spec.size());
    ASSERT_TRUE(parser->parse());
    auto* errs = new ErrorReporter();
    auto* sema = new Sema(*errs);
    ASSERT_TRUE(sema->analyze(parser->ast));
    bbq::render::CompilerCtx ctx{parser->ast, sema, nullptr, ""};
    auto* g = new bbq::Compiler();
    ctx.ir = g->compile_grammar(parser->ast);
    std::string src = bbq::render::render_reader_view_c(
        ctx, std::string(SOURCE_DIR) + "/backends/render/templates");

    auto count = [&src](const std::string& needle) {
        size_t n = 0;
        for (size_t p = src.find(needle); p != std::string::npos; p = src.find(needle, p + 1)) n++;
        return n;
    };
    // Exactly one search apiece, and it is the hoist. (A whole-file count of the CALL would
    // be answering a different question: other rules have their own `v`, named once, which
    // correctly keeps its direct call.)
    EXPECT_EQ(count("const int64_t _fr_v = bbq_view_i64(ctx, \"v\");"), 1u);
    EXPECT_EQ(count("const int64_t _fr_lo = bbq_view_i64(ctx, \"lo\");"), 1u);

    // ...and WhTwice's predicate itself searches for nothing: every occurrence reads a local.
    size_t at = src.find("WhTwice.constraint failed");
    ASSERT_NE(at, std::string::npos) << "fixture rule WhTwice lost its predicate";
    size_t bol = src.rfind('\n', at) + 1;
    std::string stmt = src.substr(bol, src.find('\n', at) - bol);
    EXPECT_EQ(stmt.find("bbq_view_i64"), std::string::npos)
        << "WhTwice's predicate still searches the index:\n" << stmt;
    EXPECT_NE(stmt.find("_fr_v"), std::string::npos) << stmt;
    EXPECT_NE(stmt.find("_fr_lo"), std::string::npos) << stmt;
}

// ── c-lite NEGATIVE (the gatekeeper, ASan/UBSan): for every rule, every truncation +
// byte-corruption, the sanitized c-lite reader accepts iff the CEK does — and never reads
// out of bounds. A zero-copy span index over hostile input is the bitcoins surface; this
// proves it stays in bounds and agrees with the oracle, systematically.
TEST(CrossBackend, ViewCReaderRejectsMalformedLikeCek) {
    const CLite& cl = shared_clite();
    ASSERT_TRUE(cl.ok) << cl.err;
    const CRoundtrip& rt = shared_rt();
    ASSERT_TRUE(rt.ok) << rt.err;

    for (auto& c : xcases()) {
        SCOPED_TRACE(c.rule);
        std::string cek_bits;   // CEK accept/reject in the SAME order run_bin_neg sweeps
        for (size_t L = 0; L <= c.valid.size(); L++)
            cek_bits += cek_meta_x(rt.cek_cg, c.rule, c.valid.data(), L).success ? '1' : '0';
        for (size_t pos = 0; pos < c.valid.size(); pos++) {
            uint8_t ds[3] = {0x00, 0xFF, uint8_t(c.valid[pos] + 1)};
            for (int d = 0; d < 3; d++) {
                if (ds[d] == c.valid[pos]) continue;
                std::vector<uint8_t> b(c.valid); b[pos] = ds[d];
                cek_bits += cek_meta_x(rt.cek_cg, c.rule, b.data(), b.size()).success ? '1' : '0';
            }
        }
        NegRun nr = run_bin_neg(cl.bin, c.rule, c.valid);
        if (cl.sanitized)
            EXPECT_FALSE(nr.crashed)
                << c.rule << ": c-lite reader overran/crashed on malformed input (ASan/UBSan)";
        if (!nr.crashed)
            EXPECT_EQ(nr.bits, cek_bits)
                << c.rule << ": c-lite accept/reject disagrees with CEK\n  c=" << nr.bits
                << "\n  k=" << cek_bits;
    }
}

// Negative axis for the GATEKEEPER (the C-owning reader): for every rule, every
// truncation + byte-corruption of valid input, the sanitized reader must accept iff the
// CEK does — and never read out of bounds (ASan/UBSan abort → test failure). The owning
// model is only safe because this reader rejects malformed bytes before a bad struct
// reaches the writer; this proves it does, systematically (not case-by-case).
TEST(CrossBackend, COwningReaderRejectsMalformedLikeCek) {
    const CRoundtrip& rt = shared_rt();
    ASSERT_TRUE(rt.ok) << rt.err;

    for (auto& c : xcases()) {
        SCOPED_TRACE(c.rule);
        // CEK's accept/reject string, in the SAME variant order the harness sweeps.
        std::string cek_bits;
        for (size_t L = 0; L <= c.valid.size(); L++)
            cek_bits += cek_meta_x(rt.cek_cg, c.rule, c.valid.data(), L).success ? '1' : '0';
        for (size_t pos = 0; pos < c.valid.size(); pos++) {
            uint8_t ds[3] = {0x00, 0xFF, uint8_t(c.valid[pos] + 1)};
            for (int d = 0; d < 3; d++) {
                if (ds[d] == c.valid[pos]) continue;
                std::vector<uint8_t> b(c.valid);
                b[pos] = ds[d];
                cek_bits += cek_meta_x(rt.cek_cg, c.rule, b.data(), b.size()).success ? '1' : '0';
            }
        }
        NegRun nr = run_bin_neg(rt.bin, c.rule, c.valid);
        // The overrun tripwire only fires a verdict when the binary is actually sanitized
        // (else a crash and a clean run are indistinguishable from a non-zero exit).
        if (rt.sanitized)
            EXPECT_FALSE(nr.crashed)
                << c.rule << ": C-owning reader overran/crashed on malformed input (ASan/UBSan)";
        if (!nr.crashed)
            EXPECT_EQ(nr.bits, cek_bits)
                << c.rule << ": C reader accept/reject disagrees with CEK\n  c=" << nr.bits
                << "\n  k=" << cek_bits;
    }
}

// @rest size is computed from content, never the stored field. A clean read→write can't
// tell trust-vs-compute apart (a valid stored size always equals the content length), so
// this MODIFIES the body (appends an element, changing the rest's byte length) and proves
// the writer emits a corrected size — the owning-model behavior the old AST-walker had.
TEST(CrossBackend, RestSizeRecomputedOnModify) {
    const char* spec = "@endian little\n"
                       "R = struct { sz: uleb128 @rest, xs: array<uint8>(none, eof) }\n";
    auto* parser = new Parser();
    parser->init(spec, (int)strlen(spec));
    ASSERT_TRUE(parser->parse());
    auto* errs = new ErrorReporter();
    auto* sema = new Sema(*errs);
    ASSERT_TRUE(sema->analyze(parser->ast));
    bbq::render::CompilerCtx ctx{parser->ast, sema, nullptr, ""};
    auto* g = new bbq::Compiler();
    ctx.ir = g->compile_grammar(parser->ast);

    std::string dir = "/tmp/bbq_rest_" + std::to_string(getpid());
    system(("mkdir -p " + dir).c_str());
    std::string tmpl = std::string(SOURCE_DIR) + "/backends/render/templates";
    { std::ofstream f(dir + "/t.h"); f << bbq::render::render_types_c(ctx, tmpl + "/types_c.inja"); }
    { std::ofstream f(dir + "/r.c");
      f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <stdlib.h>\n#include <string.h>\n"
        << bbq::render::render_reader_c(ctx, tmpl); }
    { std::ofstream f(dir + "/w.c");
      f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <string.h>\n"
        << bbq::render::render_writer_c(ctx, tmpl); }
    { std::ofstream f(dir + "/h.c");
      f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n";
      f << bbq::render::render_reader_decls(ctx);
      f << bbq::render::render_writer_decls(ctx);
      f << "int main(void){ static uint8_t in[256]; size_t n=fread(in,1,sizeof in,stdin);\n"
           "  bbq_ctx_t rc; bbq_ctx_init(&rc,in,n); r_t o; memset(&o,0,sizeof o);\n"
           "  if(!r_read(&rc,&o)) return 2;\n"
           "  o.xs.items=realloc(o.xs.items,(o.xs.count+1)*sizeof(*o.xs.items));\n"
           "  o.xs.items[o.xs.count]=40; o.xs.count++;\n"
           "  bbq_write_ctx_t wc; bbq_write_ctx_init_growable(&wc,64);\n"
           "  if(!r_write(&wc,&o)) return 3;\n"
           "  fwrite(wc.data,1,wc.pos,stdout); fflush(stdout); return 0; }\n";
    }
    std::string rtdir = std::string(SOURCE_DIR) + "/backends/c/runtime";
    std::string cc = sanitizer_cc().cmd + " -I" + dir + " -I" + rtdir +
                     " -I" + std::string(SOURCE_DIR) + "/crt " + dir + "/r.c " +
                     dir + "/w.c " + dir + "/h.c -o " + dir + "/bin 2>&1";
    FILE* cp = popen(cc.c_str(), "r");
    char cb[4096] = {}; if (cp) fread(cb, 1, sizeof cb - 1, cp);
    ASSERT_EQ(cp ? pclose(cp) : -1, 0) << "compile failed:\n" << cb;

    // sz=3, xs=[10,20,30] → append 40 → the writer must emit sz=4 and the 4 elements.
    std::vector<uint8_t> out = c_roundtrip(dir + "/bin", "", {0x03, 10, 20, 30});
    std::vector<uint8_t> want = {0x04, 10, 20, 30, 40};
    EXPECT_EQ(out, want) << "@rest size not recomputed from the (modified) content";
}

// A count-prefix `[n]` array must serialize the ACTUAL element count, not a stored field
// that can go stale — the writer's job is a valid binary. Append an element and assert the
// emitted prefix tracks the data (red until the writer derives the count from the vec).
TEST(CrossBackend, CountPrefixRecomputedOnModify) {
    const char* spec = "@endian little\n"
                       "A = struct { n: uint8, xs: array<uint16le>[n] }\n";
    auto* parser = new Parser();
    parser->init(spec, (int)strlen(spec));
    ASSERT_TRUE(parser->parse());
    auto* errs = new ErrorReporter();
    auto* sema = new Sema(*errs);
    ASSERT_TRUE(sema->analyze(parser->ast));
    bbq::render::CompilerCtx ctx{parser->ast, sema, nullptr, ""};
    auto* g = new bbq::Compiler();
    ctx.ir = g->compile_grammar(parser->ast);

    std::string dir = "/tmp/bbq_cnt_" + std::to_string(getpid());
    system(("mkdir -p " + dir).c_str());
    std::string tmpl = std::string(SOURCE_DIR) + "/backends/render/templates";
    { std::ofstream f(dir + "/t.h"); f << bbq::render::render_types_c(ctx, tmpl + "/types_c.inja"); }
    { std::ofstream f(dir + "/r.c");
      f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <stdlib.h>\n#include <string.h>\n"
        << bbq::render::render_reader_c(ctx, tmpl); }
    { std::ofstream f(dir + "/w.c");
      f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <string.h>\n"
        << bbq::render::render_writer_c(ctx, tmpl); }
    { std::ofstream f(dir + "/h.c");
      f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n";
      f << bbq::render::render_reader_decls(ctx);
      f << bbq::render::render_writer_decls(ctx);
      f << "int main(void){ static uint8_t in[256]; size_t n=fread(in,1,sizeof in,stdin);\n"
           "  bbq_ctx_t rc; bbq_ctx_init(&rc,in,n); a_t o; memset(&o,0,sizeof o);\n"
           "  if(!a_read(&rc,&o)) return 2;\n"
           "  o.xs.items=realloc(o.xs.items,(o.xs.count+1)*sizeof(*o.xs.items));\n"
           "  o.xs.items[o.xs.count]=0x0033; o.xs.count++;\n"
           "  bbq_write_ctx_t wc; bbq_write_ctx_init_growable(&wc,64);\n"
           "  if(!a_write(&wc,&o)) return 3;\n"
           "  fwrite(wc.data,1,wc.pos,stdout); fflush(stdout); return 0; }\n";
    }
    std::string rtdir = std::string(SOURCE_DIR) + "/backends/c/runtime";
    std::string cc = sanitizer_cc().cmd + " -I" + dir + " -I" + rtdir +
                     " -I" + std::string(SOURCE_DIR) + "/crt " + dir + "/r.c " +
                     dir + "/w.c " + dir + "/h.c -o " + dir + "/bin 2>&1";
    FILE* cp = popen(cc.c_str(), "r");
    char cb[4096] = {}; if (cp) fread(cb, 1, sizeof cb - 1, cp);
    ASSERT_EQ(cp ? pclose(cp) : -1, 0) << "compile failed:\n" << cb;

    // n=2, xs=[0x11,0x22] → append 0x33 → a valid binary has n=3 and the three elements.
    std::vector<uint8_t> out = c_roundtrip(dir + "/bin", "", {0x02, 0x11, 0x00, 0x22, 0x00});
    std::vector<uint8_t> want = {0x03, 0x11, 0x00, 0x22, 0x00, 0x33, 0x00};
    EXPECT_EQ(out, want) << "count prefix not derived from the (modified) array length";
}

// A PATH count prefix `[h.n]` (count field inside a sub-struct) must also derive from the
// array length on modify — the same guarantee as a plain `[n]`, across nesting.
TEST(CrossBackend, PathCountRecomputedOnModify) {
    const char* spec = "@endian little\n"
                       "R = struct { h: struct { n: uint8 }, xs: array<uint8>[h.n] }\n";
    auto* parser = new Parser();
    parser->init(spec, (int)strlen(spec));
    ASSERT_TRUE(parser->parse());
    auto* errs = new ErrorReporter();
    auto* sema = new Sema(*errs);
    ASSERT_TRUE(sema->analyze(parser->ast));
    bbq::render::CompilerCtx ctx{parser->ast, sema, nullptr, ""};
    auto* g = new bbq::Compiler();
    ctx.ir = g->compile_grammar(parser->ast);

    std::string dir = "/tmp/bbq_pcnt_" + std::to_string(getpid());
    system(("mkdir -p " + dir).c_str());
    std::string tmpl = std::string(SOURCE_DIR) + "/backends/render/templates";
    { std::ofstream f(dir + "/t.h"); f << bbq::render::render_types_c(ctx, tmpl + "/types_c.inja"); }
    { std::ofstream f(dir + "/r.c");
      f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <stdlib.h>\n#include <string.h>\n"
        << bbq::render::render_reader_c(ctx, tmpl); }
    { std::ofstream f(dir + "/w.c");
      f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <string.h>\n"
        << bbq::render::render_writer_c(ctx, tmpl); }
    { std::ofstream f(dir + "/h.c");
      f << "#include \"t.h\"\n#include \"bbq_runtime.h\"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n";
      f << bbq::render::render_reader_decls(ctx);
      f << bbq::render::render_writer_decls(ctx);
      f << "int main(void){ static uint8_t in[256]; size_t n=fread(in,1,sizeof in,stdin);\n"
           "  bbq_ctx_t rc; bbq_ctx_init(&rc,in,n); r_t o; memset(&o,0,sizeof o);\n"
           "  if(!r_read(&rc,&o)) return 2;\n"
           "  o.xs.items=realloc(o.xs.items,(o.xs.count+1)*sizeof(*o.xs.items));\n"
           "  o.xs.items[o.xs.count]=0xDD; o.xs.count++;  /* note: h.n left stale on purpose */\n"
           "  bbq_write_ctx_t wc; bbq_write_ctx_init_growable(&wc,64);\n"
           "  if(!r_write(&wc,&o)) return 3;\n"
           "  fwrite(wc.data,1,wc.pos,stdout); fflush(stdout); return 0; }\n";
    }
    std::string rtdir = std::string(SOURCE_DIR) + "/backends/c/runtime";
    std::string cc = sanitizer_cc().cmd + " -I" + dir + " -I" + rtdir +
                     " -I" + std::string(SOURCE_DIR) + "/crt " + dir + "/r.c " +
                     dir + "/w.c " + dir + "/h.c -o " + dir + "/bin 2>&1";
    FILE* cp = popen(cc.c_str(), "r");
    char cb[4096] = {}; if (cp) fread(cb, 1, sizeof cb - 1, cp);
    ASSERT_EQ(cp ? pclose(cp) : -1, 0) << "compile failed:\n" << cb;

    std::vector<uint8_t> out = c_roundtrip(dir + "/bin", "", {0x03, 0xAA, 0xBB, 0xCC});
    std::vector<uint8_t> want = {0x04, 0xAA, 0xBB, 0xCC, 0xDD};
    EXPECT_EQ(out, want) << "path count h.n not derived from the (modified) array length";
}
