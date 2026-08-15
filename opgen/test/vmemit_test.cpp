// opgen VmEmitter tests — the emitters ABOVE the body lowering: operand decode,
// stack marshaling, guards, the value model, the stencil hole/extern tables, and the
// JIT metadata. semlower_test.cpp covers a body in isolation; these cover the frame
// opgen builds around one, which is where the two tiers are made to agree.
//
// Assertions are substrings, not whole-file equality: the outputs are thousands of
// lines and an exact match would break on every unrelated edit. Where ORDER is the
// property under test (a variadic pop before vs. after the body) the test compares
// find() positions, which is the property stated directly.
#include "Parser.h"
#include "vmemit.h"
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

using Emitter = void (opgen::VmEmitter::*)(FILE*);

std::string emit(const opgen::Module* m, Emitter which) {
    char* buf = nullptr; size_t len = 0;
    FILE* f = open_memstream(&buf, &len);
    opgen::VmEmitter vm(m, "calc");
    (vm.*which)(f);
    fclose(f);
    std::string out(buf, len);
    std::free(buf);
    return out;
}

std::string interp(const char* spec)  { auto* m = parse(spec); return m ? emit(m, &opgen::VmEmitter::emit_interp_c) : ""; }
std::string stencil(const char* spec) { auto* m = parse(spec); return m ? emit(m, &opgen::VmEmitter::emit_stencil_c) : ""; }
std::string jitmeta(const char* spec) { auto* m = parse(spec); return m ? emit(m, &opgen::VmEmitter::emit_jit_meta) : ""; }
std::string rtapi(const char* spec)   { auto* m = parse(spec); return m ? emit(m, &opgen::VmEmitter::emit_runtime_api_h) : ""; }

#define HAS(hay, needle)  EXPECT_NE((hay).find(needle), std::string::npos) \
    << "expected to find:\n  " << (needle)
#define LACKS(hay, needle) EXPECT_EQ((hay).find(needle), std::string::npos) \
    << "expected NOT to find:\n  " << (needle)

// Assert `first` occurs before `second`, both present.
void expect_order(const std::string& s, const char* first, const char* second) {
    auto a = s.find(first), b = s.find(second);
    ASSERT_NE(a, std::string::npos) << "missing: " << first;
    ASSERT_NE(b, std::string::npos) << "missing: " << second;
    EXPECT_LT(a, b) << first << "\nmust precede\n" << second;
}

// Type rows use opgen's OWN scalar spellings (s4/u4/s8/u8/f4/f8 — the typedefs the
// value model emits). scalar_bits() and the float tests key on those names, so a row
// written `int64_t` would be silently sized as 32 bits.
const char* kI32 = "type i32 s4 u4 1 i\n";
const char* kI64 = "type i64 s8 u8 1 l\n";
const char* kF64 = "type f64 f8 f8 1 d\n";

// ── addrtype (§2.3.11): the width is DECLARED, never sniffed ────────────────
//
// WASM makes addrtype part of each memory's/table's type, so every typing rule reads
// it from the module and §4.6.8 executes it as an assertion ("Due to validation ...
// Pop the value (at.const i)"). Testing the operand's runtime tag instead is a
// divergence in MECHANISM — a tested fact can disagree with the declared type, an
// asserted one cannot — and it did disagree: an i64 address reconstructed out of a GC
// aggregate carries that aggregate's tag, so the width collapsed to 32 bits and an
// out-of-bounds memory64 access read an in-bounds location instead of trapping.

TEST(OpgenEmit, AddrPopTakesItsWidthFromTheDeclaredSource) {
    std::string s = interp(std::string(kI32).append(kI64).append(R"(
inline native int mem_is64(int m);
load 0x10 [ u8 m ] (addr(mem_is64(m)) a -- i32 r)
    (. r = (int)a; .)
)").c_str());
    HAS(s, "GPOP_ADDR(a, (mem_is64(NATIVE_ARGS, m)));");
    // The macro takes the width as an ARGUMENT; it must not re-derive it from the tag.
    HAS(s, "#define GPOP_ADDR(name, is64)");
    LACKS(s, "#define GPOP_ADDR(name) u8 name; do { if (JV_STKT");
}

// ── brtable: the count is opgen's to decode, the labels are opgen's to skip ──

const char* kBrTable =
    "type i32 s4 u4 1 i\n"
    "sidetable\n"
    "native void transfer();\n"
    "br_table 0x0e [ brtable labels ] (i32 key -- )\n"
    "(.  stp = stp + (((uint)key > labels_count) ? labels_count : (uint)key);  transfer();  .)\n";

TEST(OpgenEmit, BrTableCountDecodedInInterpAndBakedInStencil) {
    std::string i = interp(kBrTable);
    HAS(i, "u4 labels_count = 0; (void)bbq_read_uleb128_u32(&f->code, &labels_count);");
    // count+1 labels: one per case, then the default.
    HAS(i, "for (u4 _bt_i = 0; _bt_i <= labels_count; _bt_i++)");

    std::string st = stencil(kBrTable);
    HAS(st, "u4 labels_count = (u4)_HOLE_labels_count;");
    // The hole rides a DERIVED name, so the extern block has to derive it too — the
    // stencil would otherwise reference a symbol nothing declares.
    HAS(st, "extern uint64_t _HOLE_labels_count;");

    // The JIT compile-walk reads the count anyway to skip the labels, so it is a fixed
    // operand with its own kind; only the label vector stays tail-skipped.
    std::string jm = jitmeta(kBrTable);
    HAS(jm, "{\"_HOLE_labels_count\", JOP_BRTABLE_COUNT, 0}");
    HAS(jm, "JTAIL_BRTABLE");
}

// A body that never names the count gets no decode and no patch point: an unread
// immediate would be a wasted hole, and the tail walk already skips the whole vector.
TEST(OpgenEmit, BrTableCountNotExposedWhenTheBodyIgnoresIt) {
    std::string s = interp(
        "type i32 s4 u4 1 i\n"
        "native void go(int k);\n"
        "br_table 0x0e [ brtable labels ] (i32 key -- )\n"
        "(.  go(key);  .)\n");
    LACKS(s, "labels_count");
}

// ── §3(b) variadic operands: WHEN sp drops is a GC question ─────────────────

const char* kVariadicBody =
    "type i32 s4 u4 1 i\n"
    "native int nparams(int fn);\n"
    "native void invoke(int fn);\n";

TEST(OpgenEmit, VariadicDropsSpAfterTheBodyByDefault) {
    std::string s = interp(std::string(kVariadicBody).append(
        "call 0x10 [ uleb32 fn ] ( [nparams(fn)] i32 args -- )\n"
        "(.  invoke(fn);  .)\n").c_str());
    // The operands stay rooted across the body — what a constructor (struct.new,
    // array.new_fixed) needs, because its body allocates and sp bounds the root scan.
    expect_order(s, "invoke(NATIVE_ARGS, fn)", "JV_SP -= _vc_args;");
}

TEST(OpgenEmit, PopsFirstDropsSpBeforeTheBody) {
    std::string s = interp(std::string(kVariadicBody).append(
        "call 0x10 [ uleb32 fn ] ( [nparams(fn)] i32 args -- )\n"
        "    flag: pops_first\n"
        "(.  invoke(fn);  .)\n").c_str());
    // The call family carves the callee frame AT sp, so the params must already be
    // popped when the body hands off.
    expect_order(s, "JV_SP -= _vc_args;", "invoke(NATIVE_ARGS, fn)");
    // ...and exactly once: the after-the-body drop must be suppressed, not duplicated.
    EXPECT_EQ(s.find("JV_SP -= _vc_args;"), s.rfind("JV_SP -= _vc_args;"));
}

// ── §3(b) variadic RESULTS: exposing is a sp raise, not a push ──────────────

TEST(OpgenEmit, VariadicResultRaisesSpAndDeclaresNothing) {
    std::string s = interp(
        "type i32 s4 u4 1 i\n"
        "native void produce(int n);\n"
        "expose 0x10 [ u8 n ] ( -- [n] i32 rets )\n"
        "(.  produce(n);  .)\n");
    // The values are already at the frame top (a callee left them there); exposing
    // them IS the marshaling.
    HAS(s, "JV_SP += _vr_rets;");
    // A variadic result names a COUNT, not a value, so there is nothing to declare
    // and nothing to push.
    LACKS(s, "s4 rets;");
    LACKS(s, "GPUSH_INT(rets)");
}

// ── The runtime-typed carrier must not truncate a v128 ──────────────────────
//
// any_t carries a value whose type tag is known only at run time. Shaped for a
// 64-bit payload it silently dropped lanes 2/3 of every v128 crossing it — a
// struct/array field, a variadic operand read in place, a `pop`.

const char* kV128 =
    "type i32 s4 u4 1 i\n"
    "type i64 s8 u8 1 l\n"
    "type v128 v128_t v128_t 1 v\n"
    "native void keep(any v);\n"
    "native any  take();\n"
    "box   0x10 (any v -- )     (.  keep(v);  .)\n"
    "unbox 0x11 ( -- any r )    (.  r = take();  .)\n";

TEST(OpgenEmit, AnyCarrierMovesBothHalvesWhenTheModelDeclaresV128) {
    std::string s = interp(kV128);
    // Both 64-bit lanes of the slot, under the member letter the spec's OWN type
    // table gives v128 — never a hardcoded name.
    HAS(s, "#define GPOP_ANY(name) any_t name; do { JV_SP--; name.bits = JV_STK[JV_SP].v.i64[0];"
           " name.hi = JV_STK[JV_SP].v.i64[1];");
    HAS(s, "JV_STK[JV_SP].v.i64[0] = _a.bits; JV_STK[JV_SP].v.i64[1] = _a.hi;");
}

TEST(OpgenEmit, AnyCarrierStaysSingleWordWithoutAV128Row) {
    std::string s = interp(
        "type i32 s4 u4 1 i\n"
        "type i64 s8 u8 1 l\n"
        "native void keep(any v);\n"
        "box 0x10 (any v -- )  (.  keep(v);  .)\n");
    HAS(s, "name.bits = JV_STK[JV_SP].l; name.kind = JV_STKT[JV_SP];");
    LACKS(s, "i64[1]");
}

TEST(OpgenEmit, AnyStructCarriesTheHighHalf) {
    std::string s = rtapi(kV128);
    HAS(s, "typedef struct { s8 bits; s8 hi; u1 kind; } any_t;");
}

// A variadic operand read in place is the same carrier by another route — a v128
// field of a struct.new must not lose its top lanes on the way to the native.
TEST(OpgenEmit, VariadicOperandReadCarriesBothHalves) {
    std::string s = interp(
        "type i32 s4 u4 1 i\n"
        "type i64 s8 u8 1 l\n"
        "type v128 v128_t v128_t 1 v\n"
        "native int nfields(int t);\n"
        "native void set_field(int i, any v);\n"
        "struct_new 0x10 [ uleb32 t ] ( [nfields(t)] any fields -- )\n"
        "(.  set_field(0, fields[0]);  .)\n");
    HAS(s, "_vb_fields[0].v.i64[0]");
    HAS(s, ".v.i64[1], .kind = _vt_fields[");
}

// ── `word`: a whole slot, so a SCALAR source needs the field selector ───────

TEST(OpgenEmit, WordResultFromAScalarGoesThroughTheFieldSelector) {
    // memory.size's shape: the result WIDTH is decided at run time, so the signature
    // says `word` and the body sets the tag itself via <name>_wt.
    std::string s = interp(
        "type i32 s4 u4 1 i\n"
        "type i64 s8 u8 1 l\n"
        "native int pages(int m);\n"
        "native int is64(int m);\n"
        "msize 0x10 [ u8 m ] ( -- word result )\n"
        "(.  result = pages(m);  result_wt = is64(m) ? T_LONG : T_INT;  .)\n");
    HAS(s, "result.l = (s8)((pages(NATIVE_ARGS, m)));");
}

TEST(OpgenEmit, WordResultFromASlotIsAWholeSlotMove) {
    // select's shape: both arms are slots, so this is a move, not a field write.
    std::string s = interp(
        "type i32 s4 u4 1 i\n"
        "type i64 s8 u8 1 l\n"
        "select 0x1b (word v1, word v2, i32 c -- word result)\n"
        "(.  result = c ? v1 : v2;  result_wt = c ? v1_wt : v2_wt;  .)\n");
    HAS(s, "result = ((c ? v1 : v2));");
    LACKS(s, "result.l = ");
}

// ── Float constants inside a guard ──────────────────────────────────────────
//
// A float literal lowers to a C literal, which clang materializes from the .rodata
// constant pool — a relocation a copy-and-patch stencil cannot carry. A BODY constant
// already gets a named hole (a literal-init local); a CONDITION has no local to hang
// a name on, so the literal needs a synthesized one keyed by its VALUE.

const char* kTrunc =
    "type i32 s4 u4 1 i\n"
    "type f64 f8 f8 1 d\n"
    "trunc 0x10 (f64 a -- i32 r)\n"
    "    error: (. a != a .) -> NaN\n"
    "    error: (. !(a >= -2147483648.0 && a < 2147483648.0) .) -> Overflow\n"
    "(.  r = (int)a;  .)\n";

TEST(OpgenEmit, GuardFloatConstantsRideNamedHoles) {
    std::string s = stencil(kTrunc);
    // -2147483648.0 and 2147483648.0 — same magnitude, different sign bit.
    HAS(s, "_HOLE_kc1e0000000000000");
    HAS(s, "_HOLE_k41e0000000000000");
    HAS(s, "extern uint64_t _HOLE_kc1e0000000000000;");
    // The negated literal is ONE constant, not a negation of one: folding the sign in
    // keeps it off the sign-mask path, whose hole is only declared for a runtime negate.
    LACKS(s, "_HOLE_fsignmask");

    std::string jm = jitmeta(kTrunc);
    HAS(jm, "{\"_HOLE_kc1e0000000000000\", JOP_CONST, 0xc1e0000000000000ULL}");
}

// The INTERP tier has no holes — it spells the constants, which is what the JIT tier
// has to agree with. Pinned so the hole path cannot drift into the interpreter.
TEST(OpgenEmit, GuardFloatConstantsStayLiteralInTheInterp) {
    std::string s = interp(kTrunc);
    LACKS(s, "_HOLE_k");
    HAS(s, "2147483648");
}

TEST(OpgenEmit, GuardWithARuntimeFloatNegateDeclaresTheSignMask) {
    const char* spec =
        "type i32 s4 u4 1 i\n"
        "type f64 f8 f8 1 d\n"
        "chk 0x10 (f64 a, f64 b -- f64 r)\n"
        "    error: (. -a > b .) -> NegOverflow\n"
        "(.  r = a;  .)\n";
    std::string s = stencil(spec);
    // The body scan cannot cover this: it keys on the RESULT type, and a float
    // COMPARE has an i32 result, so the width has to come from the condition. Left
    // undeclared, jitterator waves the hole through on its name prefix and the driver
    // bakes 0 — the sign flip silently vanishes in JIT code only.
    HAS(s, "_HOLE_fsignmask64");
    HAS(s, "extern uint64_t _HOLE_fsignmask64;");
    std::string jm = jitmeta(spec);
    HAS(jm, "{\"_HOLE_fsignmask64\", JOP_CONST, 0x8000000000000000ULL}");
}

// ── Stencil externs: EVERY expression the stencil lowers must be scanned ────

TEST(OpgenEmit, StencilDeclaresANativeNamedOnlyByAGuard) {
    // Guards are emitted into the stencil too, so a native reachable only from a
    // condition still rides a _HOLE_ patch point. Scanning the body alone left the
    // hole undeclared and the stencil failed to compile.
    std::string s = stencil(
        "type i32 s4 u4 1 i\n"
        "native int is_bad(int a);\n"
        "chk 0x10 (i32 a -- i32 r)\n"
        "    error: (. is_bad(a) != 0 .) -> Bad\n"
        "(.  r = a;  .)\n");
    HAS(s, "extern uint64_t _HOLE_is_bad;");
}

// A native taking an addrtype value must be able to SAY so. The action language accepts
// uint/ulong; the prototype emitter used to fall through to s4 for both, silently
// narrowing the declared signature of every native an addrtype was handed to.
TEST(OpgenEmit, NativePrototypesKeepUnsignedWidths) {
    std::string s = rtapi(std::string(kI32).append(kI64).append(
        "native int in_bounds(int m, ulong a, uint n);\n"
        "op 0x10 (i32 x -- i32 r)  (.  r = in_bounds(0, 0, 0);  .)\n").c_str());
    HAS(s, "in_bounds(vm_t*, heap_t*, s4, u8, u4);");
}

TEST(OpgenEmit, StencilDeclaresANativeNamedOnlyByAnAddrtypeSource) {
    // The addrtype source is lowered into the same operand-pop preamble. An op set
    // whose source happens to be an `inline native` never notices — an inline native
    // is a direct call with no hole — so this is the case that has to be pinned.
    std::string s = stencil(std::string(kI32).append(kI64).append(
        "native int mem_is64(int m);\n"
        "load 0x10 [ u8 m ] (addr(mem_is64(m)) a -- i32 r)\n"
        "(.  r = (int)a;  .)\n").c_str());
    HAS(s, "extern uint64_t _HOLE_mem_is64;");
}

TEST(OpgenEmit, StencilDeclaresANativeNamedOnlyByAVariadicCount) {
    // The pop-count expression is lowered ahead of the body, so a native named ONLY
    // there rides a patch point on the same terms.
    std::string s = stencil(
        "type i32 s4 u4 1 i\n"
        "native int type_nparams(int t);\n"
        "native void go();\n"
        "call_ref 0x10 [ uleb32 t ] ( [type_nparams(t)] i32 args -- )\n"
        "(.  go();  .)\n");
    HAS(s, "extern uint64_t _HOLE_type_nparams;");
}

// ── Running off the end of a function ───────────────────────────────────────

TEST(OpgenEmit, HaltStencilStashesNothing) {
    std::string s = stencil(std::string(kI32).append(
        "add 0x10 (i32 a, i32 b -- i32 r)  (.  r = a + b;  .)\n").c_str());
    /* Every stencil shares the ONE musttail signature (CACHE_ARGS), halt
     * included — a per-stencil signature is rejected by clang's musttail,
     * which is the constraint that made the signature uniform. The claim
     * this test holds is the body: empty but for silencing the parameter. */
    HAS(s, "void STENCIL gen_st_halt(CACHE_ARGS) { (void)vm; }");
    // `result`/`result_type` were the BACKEND's field names in one consumer's vm_t —
    // a baked-in target identifier in a generator that promises to have none. The
    // results are the top of the frame stack, where the caller reads them.
    LACKS(s, "vm->result");
}

// ── The `sidetable` directive ───────────────────────────────────────────────

const char* kNoSidetable =
    "type i32 s4 u4 1 i\n"
    "native void transfer();\n"
    "go 0x10 ( -- )  (.  transfer();  .)\n";

TEST(OpgenEmit, SidetableDirectiveEmitsTheEntryTypeAndTheWalk) {
    std::string s = rtapi(std::string("sidetable\n").append(kNoSidetable).c_str());
    HAS(s, "} opgen_st_entry_t;");
    HAS(s, "static inline void opgen_do_transfer(frame_t* f)");
    // The walk moves slots through the value model's own primitive rather than
    // hand-rolling f->stack[...] = f->stack[...].
    HAS(s, "JV_NMOVE(f,");
}

TEST(OpgenEmit, WithoutTheDirectiveNoWalkIsEmitted) {
    // Keyed on the DECLARATION, not on a built-in's name: opgen bakes in no target
    // identifiers, and a substrate with no side table must not be handed a walk that
    // reads frame fields it does not have. Here `transfer` is declared and called.
    std::string s = rtapi(kNoSidetable);
    LACKS(s, "opgen_st_entry_t");
    LACKS(s, "opgen_do_transfer");
    // The cursor accessors are unconditional: a body may name `stp` either way.
    HAS(s, "#define OPGEN_ST_PTR(f)");
}

TEST(OpgenEmit, BodyStpGoesThroughTheCursorSeam) {
    std::string s = interp(
        "type i32 s4 u4 1 i\n"
        "sidetable\n"
        "native void transfer();\n"
        "br 0x10 [ uleb32 d ] ( -- )  (.  stp = stp + d;  transfer();  .)\n");
    HAS(s, "OPGEN_ST_PTR(f) = (u4)((OPGEN_ST_PTR(f) + d));");
    LACKS(s, "f->stp");
}

// ── Hard errors ────────────────────────────────────────────────────────────
//
// opgen used to print a diagnostic and emit NOTHING for these, so the generated
// handler compiled and silently did the wrong thing. Each is now fatal.

using OpgenEmitDeathTest = ::testing::Test;

// Lower a spec to /dev/null purely for the side effect of dying.
void lower_to_null(const char* spec) {
    auto* m = parse(spec);
    if (!m) std::exit(2);
    FILE* devnull = std::fopen("/dev/null", "w");
    opgen::VmEmitter vm(m, "calc");
    vm.emit_interp_c(devnull);
    std::fclose(devnull);
}

// The constant-hole table is a STENCIL concern, so this one drives that emitter.
void stencil_to_null(const char* spec) {
    auto* m = parse(spec);
    if (!m) std::exit(2);
    FILE* devnull = std::fopen("/dev/null", "w");
    opgen::VmEmitter vm(m, "calc");
    vm.emit_stencil_c(devnull);
    std::fclose(devnull);
}

TEST(OpgenEmitDeathTest, UndeclaredBuiltinInValuePosition) {
    EXPECT_EXIT(lower_to_null(std::string(kI32).append(
        "op 0x10 (i32 a -- i32 r)  (.  r = nope(a);  .)\n").c_str()),
        ::testing::ExitedWithCode(1), "undeclared built-in 'nope'");
}

TEST(OpgenEmitDeathTest, UndeclaredBuiltinInStatementPosition) {
    EXPECT_EXIT(lower_to_null(std::string(kI32).append(
        "op 0x10 (i32 a -- )  (.  nope(a);  .)\n").c_str()),
        ::testing::ExitedWithCode(1), "undeclared built-in 'nope'");
}

TEST(OpgenEmitDeathTest, IndexingSomethingOtherThanLocalsOrCode) {
    EXPECT_EXIT(lower_to_null(std::string(kI32).append(
        "op 0x10 (i32 a -- i32 r)  (.  r = mystery[a];  .)\n").c_str()),
        ::testing::ExitedWithCode(1), "only locals\\[\\]/code\\[\\] indexing");
}

TEST(OpgenEmitDeathTest, SlotStoreFromANonSlotSource) {
    EXPECT_EXIT(lower_to_null(std::string(kI32).append(
        "op 0x10 (i32 a -- )  (.  stack[0] = a;  .)\n").c_str()),
        ::testing::ExitedWithCode(1), "slot store needs a slot source");
}

TEST(OpgenEmitDeathTest, UnsupportedAssignmentTarget) {
    EXPECT_EXIT(lower_to_null(std::string(kI32).append(
        "native int f(int x);\n"
        "op 0x10 (i32 a -- )  (.  f(1) = a;  .)\n").c_str()),
        ::testing::ExitedWithCode(1), "unsupported assignment target");
}

TEST(OpgenEmitDeathTest, ExpressionStatementThatIsNotACall) {
    EXPECT_EXIT(lower_to_null(std::string(kI32).append(
        "op 0x10 (i32 a -- )  (.  a + 1;  .)\n").c_str()),
        ::testing::ExitedWithCode(1), "must be a built-in call");
}

TEST(OpgenEmitDeathTest, AddrOperandWithNoDeclaredSource) {
    // There is no correct default: falling back to the operand's runtime tag is
    // precisely the divergence the `addr(...)` form exists to remove.
    EXPECT_EXIT(lower_to_null(std::string(kI32).append(kI64).append(
        "load 0x10 (addr a -- i32 r)  (.  r = (int)a;  .)\n").c_str()),
        ::testing::ExitedWithCode(1), "no addrtype source");
}

TEST(OpgenEmitDeathTest, TooManyConstantHolesIsFatal) {
    // A dropped hole is not a missing patch, it is a WRONG one: the stencil still
    // names the hole and the driver bakes 0 into it.
    EXPECT_EXIT(stencil_to_null(std::string(kI32).append(
        "op 0x10 ( -- i32 r )\n"
        "(.  int a = 1; int b = 2; int c = 3; int d = 4; int e = 5;\n"
        "    int g = 6; int h = 7; int i = 8; int j = 9; r = a;  .)\n").c_str()),
        ::testing::ExitedWithCode(1), "constant holes");
}

}  // namespace
