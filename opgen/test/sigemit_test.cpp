// opgen SigEmitter tests — the tier-2 storage-class vocabulary. classify_final
// is THE mapping (sigemit.h): everything that asks "which register class does
// this value ride" reaches a class the same way, and a value type it cannot
// answer aborts the whole emission — including the tier-1 tables a consumer may
// be running for. So the classes that are not their own spelling are pinned
// here, the way the SIMD lane views and the reference forms are pinned by the
// mapping's own comment.
#include "Parser.h"
#include "sigemit.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::string sigtab(const char* spec) {
    auto* m = parse(spec);
    if (!m) return "";
    char* buf = nullptr; size_t len = 0;
    FILE* f = open_memstream(&buf, &len);
    opgen::SigEmitter sig(m, "calc");
    sig.emit_sigtab_h(f);
    fclose(f);
    std::string out(buf, len);
    std::free(buf);
    return out;
}

#define HAS(hay, needle)  EXPECT_NE((hay).find(needle), std::string::npos) \
    << "expected to find:\n  " << (needle)

// ── sub-word integers (the JCVM spellings) ──────────────────────────────────
//
// A byte or short is held widened — the stencil hosts keep scalars in 32/64-bit
// registers — so all four sub-word spellings are held the way an i32 is, and
// the width stays in the opcode's semantics, not the carrier.

TEST(OpgenSig, SubWordIntegersAreHeldInAnI32Register) {
    using opgen::SigEmitter;
    using opgen::ValueType;
    using opgen::SClass;
    EXPECT_EQ(SigEmitter::classify_final(ValueType::TyI8,  "t", "p"), SClass::I32);
    EXPECT_EQ(SigEmitter::classify_final(ValueType::TyU8,  "t", "p"), SClass::I32);
    EXPECT_EQ(SigEmitter::classify_final(ValueType::TyI16, "t", "p"), SClass::I32);
    EXPECT_EQ(SigEmitter::classify_final(ValueType::TyU16, "t", "p"), SClass::I32);
}

// The same fact through the front door: a spec whose stack types are shorts
// (a 16-bit-slot machine, written body-less the way a tables-only consumer
// writes one) emits a sigtab, and its binop is the i32 terminal.
TEST(OpgenSig, AShortBinopIsTheI32Terminal) {
    std::string s = sigtab(
        "type i16 s2 u2 1 s\n"
        "sadd 0x41 (i16 a, i16 b -- i16 result)\n");
    HAS(s, "\"Sig_i32_i32_to_i32\"");
}

} // namespace
