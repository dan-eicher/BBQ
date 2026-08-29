// opgen VcEmitter tests — the per-obligation SMT-LIB verification conditions.
// One .smt2 per obligation plus a manifest; every op appears in the manifest
// exactly once by classification (OB rows, UNVERIFIABLE with reason, or
// CLOSED), so nothing is silently absent from the verify report. A def bug
// the emitter can prove statically (an output not assigned on every path, an
// edge condition that does not parse) is a REFUSAL (nonzero rc), never a
// silently-weaker obligation.
#include "Parser.h"
#include "vcemit.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

opgen::Module* parse(const char* src) {
    Parser p;
    p.init(src, (int)std::strlen(src));
    bool ok = p.parse();
    auto f = p.furthest();
    EXPECT_TRUE(ok) << "parse failed near line " << f.line << " col " << f.col;
    return ok ? p.ast : nullptr;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Emit into a per-test scratch dir; return {rc, manifest text}.
struct VcRun { int rc; std::string manifest; std::string dir; };

VcRun vcrun(const char* spec, const char* tag) {
    VcRun r{1, "", ""};
    auto* m = parse(spec);
    if (!m) return r;
    r.dir = testing::TempDir() + "opgen_vc_" + tag;
    std::string cmd = "mkdir -p " + r.dir;
    EXPECT_EQ(std::system(cmd.c_str()), 0);
    opgen::VcEmitter vc(m, "calc");
    r.rc = vc.emit_all(r.dir.c_str());
    r.manifest = slurp(r.dir + "/calc_vc_manifest.txt");
    return r;
}

// The .smt2 for the manifest's OB row matching `mnemonic` + `kind`.
std::string ob_file(const VcRun& r, const char* mnemonic, const char* kind) {
    std::istringstream in(r.manifest);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag, id, mn, kd, expect, file;
        ls >> tag >> id >> mn >> kd >> expect >> file;
        if (tag == "OB" && mn == mnemonic && kd == kind)
            return slurp(r.dir + "/" + file);
    }
    return "";
}

#define HAS(hay, needle)  EXPECT_NE((hay).find(needle), std::string::npos) \
    << "expected to find:\n  " << (needle) << "\nin:\n" << (hay)
#define LACKS(hay, needle) EXPECT_EQ((hay).find(needle), std::string::npos) \
    << "expected NOT to find:\n  " << (needle) << "\nin:\n" << (hay)

const char* kI32 = "type i32 s4 u4 1 i\n";
const char* kI16 = "type i16 s2 u2 1 s\n";

// ── totality: signed division is partial; the guard is the assumption ──────

TEST(OpgenVc, DivisionEmitsATotalityObligation) {
    VcRun r = vcrun(std::string(kI32).append(R"(
idiv 0x0D (i32 a, i32 b -- i32 r)
    error: (. b == 0 .) -> DivByZero
    (. r = a / b; .)
)").c_str(), "div_tot");
    EXPECT_EQ(r.rc, 0);
    HAS(r.manifest, "OB ");
    HAS(r.manifest, " idiv totality unsat ");
    std::string smt = ob_file(r, "idiv", "totality");
    HAS(smt, "(set-logic QF_BV)");
    HAS(smt, "(declare-const a (_ BitVec 32))");
    HAS(smt, "(declare-const b (_ BitVec 32))");
    HAS(smt, "(check-sat)");
    HAS(smt, "(get-model)");
}

TEST(OpgenVc, TotalArithmeticEmitsNoTotalityObligation) {
    VcRun r = vcrun(std::string(kI32).append(R"(
iadd 0x02 (i32 a, i32 b -- i32 r)
    (. r = a + b; .)
)").c_str(), "add_tot");
    EXPECT_EQ(r.rc, 0);
    LACKS(r.manifest, "totality");
    HAS(r.manifest, "CLOSED iadd");
}

// ── edge: the annotation is an implication the body must satisfy ───────────

TEST(OpgenVc, EdgeAnnotationEmitsAnObligationAtTheDeclaredWidth) {
    VcRun r = vcrun(std::string(kI16).append(R"(
sdiv 0x47 (i16 a, i16 b -- i16 r)
    error: (. b == 0 .) -> DivByZero
    edge: "a == -32768 && b == -1" -> "r == -32768"
    (. r = a / b; .)
)").c_str(), "sdiv_edge");
    EXPECT_EQ(r.rc, 0);
    HAS(r.manifest, " sdiv edge unsat ");
    std::string smt = ob_file(r, "sdiv", "edge");
    HAS(smt, "(declare-const a (_ BitVec 16))");
    HAS(smt, "(declare-const b (_ BitVec 16))");
    HAS(smt, "(check-sat)");
}

// ── guard-sat: a guard nothing can fire is dead spec text ──────────────────

TEST(OpgenVc, GuardEmitsASatisfiabilityObligation) {
    VcRun r = vcrun(std::string(kI32).append(R"(
idiv 0x0D (i32 a, i32 b -- i32 r)
    error: (. b == 0 .) -> DivByZero
    (. r = a / b; .)
)").c_str(), "guard_sat");
    EXPECT_EQ(r.rc, 0);
    HAS(r.manifest, " idiv guard-sat sat ");
}

// ── classification rows: open bodies and loop bodies are metered ───────────

TEST(OpgenVc, OpenBodyGetsAnUnverifiableRowWithItsReason) {
    VcRun r = vcrun(std::string(kI32).append(R"(
istore 0x0B [ i8 idx ] (i32 v -- )
    local_value: int
    (. locals[idx] = v; .)
)").c_str(), "open");
    EXPECT_EQ(r.rc, 0);
    HAS(r.manifest, "UNVERIFIABLE istore accesses locals[]/globals[]/code[]/stack[]");
}

TEST(OpgenVc, LoopBodyIsUnverifiable) {
    VcRun r = vcrun(std::string(kI32).append(R"(
looper 0x01 (i32 a -- i32 r)
    (. r = 0; while (a > 0) { r = r + a; a = a - 1; } .)
)").c_str(), "loop");
    EXPECT_EQ(r.rc, 0);
    HAS(r.manifest, "UNVERIFIABLE looper loop in body");
}

TEST(OpgenVc, UnmodeledIntrinsicIsUnverifiable) {
    VcRun r = vcrun(std::string(kI32).append(R"(
ipop 0x01 (i32 a -- i32 r)
    (. r = popcnt(a); .)
)").c_str(), "intrin");
    EXPECT_EQ(r.rc, 0);
    HAS(r.manifest, "UNVERIFIABLE ipop intrinsic not modeled");
}

// ── refusals: statically-provable def bugs stop the emitter ────────────────

TEST(OpgenVc, OutputNotAssignedOnEveryPathIsRefused) {
    VcRun r = vcrun(std::string(kI32).append(R"(
halfout 0x01 (i32 a -- i32 r)
    (. if (a == 0) { r = 1; } .)
)").c_str(), "halfout");
    EXPECT_NE(r.rc, 0);
}

TEST(OpgenVc, UnparseableEdgeConditionIsRefused) {
    VcRun r = vcrun(std::string(kI32).append(R"(
badedge 0x01 (i32 a -- i32 r)
    edge: "a === 1" -> "r == 1"
    (. r = a; .)
)").c_str(), "badedge");
    EXPECT_NE(r.rc, 0);
}

// ── every op appears exactly once by classification ────────────────────────

TEST(OpgenVc, EveryOpHasExactlyOneClassification) {
    VcRun r = vcrun(std::string(kI32).append(R"(
iadd 0x02 (i32 a, i32 b -- i32 r)
    (. r = a + b; .)
idiv 0x0D (i32 a, i32 b -- i32 r)
    error: (. b == 0 .) -> DivByZero
    (. r = a / b; .)
mystery 0x10 (i32 a -- i32 r)
)").c_str(), "census");
    EXPECT_EQ(r.rc, 0);
    HAS(r.manifest, "CLOSED iadd");
    HAS(r.manifest, " idiv ");
    HAS(r.manifest, "UNVERIFIABLE mystery no body");
    LACKS(r.manifest, "CLOSED idiv");
}

} // namespace
