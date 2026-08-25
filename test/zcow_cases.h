#pragma once
//
// The one input table for the zcow_reader fixture.
//
// Every C++-side suite reads from here, so a case added for one is a case every other
// gets, and the coverage gates in cross_backend_test (EveryFixtureRuleHasACase,
// EveryVariantArmIsExercised, EveryOptionalIsExercisedBothWays) speak for all of them.
// Four hand-maintained copies of this table is what let arms and optional-absent paths
// go untested while each suite believed its own list was the coverage claim.
//
// A rule appears once per ARM it can take — see the gates for what makes that complete.
//
// The generated zr:: readers define their entry points NON-inline, so exactly one
// translation unit per binary may include ZcowReaderReader.h. This header only
// DECLARES them; whichever TU includes the generated header supplies the bodies at
// link. (bbq_tests: render_view_parser_test.cpp. cpp_tests: cpp_backend_test.cpp.)
#include "CaptureCow.h"
#include "ParseArena.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zr {
#define ZR(R) bbq::zcow::parse_result R##_read(const uint8_t*, size_t, bbq::ParseArena&);
ZR(Flat) ZR(Nest) ZR(Arr) ZR(Pair) ZR(Outer) ZR(RArr) ZR(Bytes) ZR(Str) ZR(Wh) ZR(WhTwice)
ZR(Comp) ZR(Iv) ZR(Pos) ZR(Op) ZR(Bare) ZR(Sw) ZR(VA) ZR(VB) ZR(U) ZR(Alts)
ZR(Bf) ZR(SBf) ZR(InlineBf) ZR(Eof) ZR(Unt) ZR(Es) ZR(RsItem) ZR(Resync) ZR(Ext)
ZR(SwRange) ZR(Tern) ZR(Bstart) ZR(Rest) ZR(RestEof) ZR(Cnt) ZR(TopSw) ZR(Np) ZR(OScope)
ZR(AllPrim) ZR(Leb) ZR(LebArr) ZR(LebOpt) ZR(Mat) ZR(OptSpan)
ZR(TyByte) ZR(TyRule) ZR(OptTop) ZR(NestGrp) ZR(NestArr) ZR(FComp)
#undef ZR
}  // namespace zr

namespace zcow_fixture {

using ReadFn = bbq::zcow::parse_result (*)(const uint8_t*, size_t, bbq::ParseArena&);

struct Case {
    const char* rule;
    ReadFn read;
    std::vector<uint8_t> bytes;
};

inline const std::vector<Case>& cases() {
    static const std::vector<Case> c = {
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
        {"Op", zr::Op_read, {0x01,0x34,0x12,0x05,0x06}},   // f==1: both optionals present
        {"Op", zr::Op_read, {0x00}},                       // f!=1: both skipped
        {"Bare", zr::Bare_read, {0x0A,0x2A}},              // the bare optional taken
        {"Bare", zr::Bare_read, {0x0A}},                   // ...and not taken (input ends)
        // A rule appears once per ARM it can take. Every axis below — the producers
        // agreeing, the round trips, the truncation and corruption sweeps — then runs
        // on each arm, because which arm was taken decides how the bytes after it are
        // read. EveryVariantArmIsExercised is what says this list is complete.
        {"Sw", zr::Sw_read, {0x01,0x34,0x12}},          // arm 0: case 1 → uint16le
        {"Sw", zr::Sw_read, {0x02,0x05,0x06}},          // arm 1: case 2 → Pair
        {"Sw", zr::Sw_read, {0x09,0x2A}},               // arm 2: default → uint8
        {"VA", zr::VA_read, {0x01,0x0A}},
        {"VB", zr::VB_read, {0x02,0x0B}},
        {"U", zr::U_read, {0x01,0x0A}},                 // arm 0: asA (VA, t==1)
        {"U", zr::U_read, {0x02,0x0B}},                 // arm 1: asB (VB, t==2)
        {"Alts", zr::Alts_read, {0x01,0x0A}},           // arm 0: VA
        {"Alts", zr::Alts_read, {0x02,0x0B}},           // arm 1: VB
        {"Bf", zr::Bf_read, {0xA5}},
        {"SBf", zr::SBf_read, {0x2D}},   // imm: signed 4 = -3, tag: unsigned 4 = 2
        {"InlineBf", zr::InlineBf_read, {0x11,0xA5,0x22}},
        {"Eof", zr::Eof_read, {1,2,3,4}},
        {"Unt", zr::Unt_read, {1,2,3}},
        {"Es", zr::Es_read, {0x34,0x12,0x12,0x34}},
        {"RsItem", zr::RsItem_read, {0x05}},
        {"Resync", zr::Resync_read, {0x02,0x00,0x05,0x00,0x07}},
        {"Ext", zr::Ext_read, {0xAA,0x01,0x02,0x03,0x04,0xBB}},
        {"SwRange", zr::SwRange_read, {0x02,0x34,0x12}},   // arm 0: the 1..3 range case
        {"SwRange", zr::SwRange_read, {0x09,0x2A}},        // arm 1: default → uint8
        {"Tern", zr::Tern_read, {0x05}},
        {"Bstart", zr::Bstart_read, {0x0A,0x0B}},
        {"Rest", zr::Rest_read, {0x02,0x34,0x12}},
        {"RestEof", zr::RestEof_read, {0x03,10,20,30}},
        {"Cnt", zr::Cnt_read, {0x03,10,20,30}},
        {"TopSw", zr::TopSw_read, {0x01,0x22}},            // arm 0: peek()==1 → Pair
        {"TopSw", zr::TopSw_read, {0x05}},                 // arm 1: default → uint8
        {"Np", zr::Np_read, {0x03,0xAA,0xBB,0xCC}},
        {"OScope", zr::OScope_read, {0x03,0x03,0xFF,0x10,0x20,0x30}},
        {"AllPrim", zr::AllPrim_read, {0x12,0xFE,0x56,0x34,0x01,0x02,0xFD,0xFF,
            0x00,0x01,0x00,0x00,0x01,0x02,0x03,0x04,0xFC,0xFF,0xFF,0xFF,
            0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xFB,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0x00,0x00,0xC0,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x40,0x01}},
        {"Leb", zr::Leb_read, {0xAC,0x02,0x7B,0x42}},
        {"LebArr", zr::LebArr_read, {0x03,0x01,0xAC,0x02,0x05}},
        {"LebOpt", zr::LebOpt_read, {0x01,0xAC,0x02,0x42}},   // f==1: the varint present
        {"LebOpt", zr::LebOpt_read, {0x00,0x42}},             // f!=1: skipped, tail moves up
        {"Mat", zr::Mat_read, {0x02,0x03,1,2,3,4,5,6}},
        {"OptSpan", zr::OptSpan_read, {0x01,0xAA,0xBB,0x61,0x62,0x63}},  // n==1: both spans
        {"OptSpan", zr::OptSpan_read, {0x00}},                           // n!=1: both skipped
        {"TyByte", zr::TyByte_read, {0x2A}},
        {"TyRule", zr::TyRule_read, {0x07,0x08}},
        {"OptTop", zr::OptTop_read, {0x2A}},
        {"NestGrp", zr::NestGrp_read, {0x02,0xAA,0xBB}},
        {"NestArr", zr::NestArr_read, {0x02, 0x01,0x11, 0x02,0x22,0x33}},
        {"FComp", zr::FComp_read, {0xC8}},   // x=200 → half=100.0 (float), big=true (bool)
    };
    return c;
}

}  // namespace zcow_fixture
