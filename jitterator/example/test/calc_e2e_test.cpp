/*
 * calc_e2e_test.cpp — the REAL end-to-end test: a parsed calc SOURCE program
 * runs the full opgen-way chain.
 *
 *   pegc parse → ddcg compile → CPS/ANF IR   (calc_runner::calc_compile)
 *   IR → bytecode (the cg_jump lowering)      (calc::Lower)
 *   bytecode → result                          (opgen-generated interpreter)
 *
 * The inputs are the calc_tests regression strings; each must produce the
 * specified value through the whole pipeline.
 */
#include "calc_natives.h"      /* calc_br / calc_call / calc_ret + the VM headers */
#include "calc_runner.h"       /* calc_compile + lower (burg cg_jump) */
#include "opcodes.h"           /* OP_* — the rewrite pins read the emitted bytes */
#include <cstdio>
#include <vector>

#define TAIL __attribute__((musttail))

static const opcode_handler_t* g_table;
extern "C" void calc_next(vm_t* vm) {
    u1 op; if (!bbq_read_u8(&vm->frame.code, &op)) return;
    TAIL return g_table[op](vm);
}
extern "C" void calc_trap(vm_t* vm) { vm->trapped = 1; }

static s4 run_bytecode(const std::vector<uint8_t>& code) {
    static vm_t vm;
    g_table = gen_interp_dispatch_table();
    bbq_ctx_init(&vm.frame.code, code.data(), code.size());
    vm.frame.sp = 0; vm.depth = 0; vm.trapped = 0; vm.result.i = 0; vm.result_type = T_INT;
    calc_next(&vm);
    return vm.result.i;
}

// parse → compile → lower → run.
static bool calc_eval(const char* src, s4* out) {
    int fs = 0;
    calc_ir::Node* ir = calc_runner::calc_compile(src, &fs);
    if (!ir) return false;
    std::vector<uint8_t> code = calc_runner::lower(ir);
    *out = run_bytecode(code);
    return true;
}

// Compare an emitted sequence against a pinned one. The whole byte string is
// the assertion: counting occurrences of an opcode would have to decode
// instructions to avoid counting operand payloads (a sleb128 byte equal to
// OP_MUL is not a multiply), and the pinned sequence says more anyway — it
// says exactly what the compiler emitted.
static bool seq_is(const std::vector<uint8_t>& got,
                   const std::vector<uint8_t>& want, const char* what) {
    if (got == want) return true;
    printf("FAIL: %s\n      got: ", what);
    for (uint8_t b : got) printf("%02X ", b);
    printf("\n      want:");
    for (uint8_t b : want) printf(" %02X", b);
    printf("\n");
    return false;
}

static int fails = 0;
int main(void) {
    struct Case { const char* src; int64_t want; };
    static const Case cases[] = {
        {"5",                                       5},
        {"1+2",                                     3},
        {"2+3*4",                                  14},
        // Division: same precedence tier as `*`, left-associative, and it binds
        // tighter than `+`. The `div` opcode is the one carrying declared
        // `error:` guards, so these also prove a guarded opcode still computes.
        {"12/4",                                    3},
        {"100/5/2",                                10},
        {"2+12/4",                                  5},
        {"20/3",                                    6},   // truncating, like C
        {"-12/4",                                  -3},
        {"1==1",                                    1},
        {"2<3",                                     1},
        {"3<2",                                     0},
        {"-5 + 7",                                  2},
        {"let $a = 5; $a + 10",                    15},
        {"let $a = 3; let $b = 4; $a * $b",        12},
        {"if (1==1) 5 else 7",                      5},
        {"if (1==0) 5 else 7",                      7},
        {"let $a = 5; if ($a < 10) $a else 99",     5},
        {"if (1==1) (if (2==2) 100 else 200) else 300", 100},
        {"if (1==0) 999 else (if (3<2) 11 else 22)",    22},
        // ── the full Fig-1 grammar: sequence, while, &&/||, nesting ──
        {"3 ; 4 ; 5",                                    5},
        {"(1 < 2) && (3 < 4)",                           1},
        {"(1 < 2) && (4 < 3)",                           0},
        {"(1 < 2) || (5 < 3)",                           1},
        {"(2 < 1) || (5 < 3)",                           0},
        {"let $a = 0; while ($a < 5) $a = $a + 1; $a",  5},
        {"let $s = 0; let $i = 0; while ($i < 4) ($s = $s + $i; $i = $i + 1); $s", 6},
        {"let $n = 0; while ($n < 10) (if ($n < 3) $n = $n + 1 else $n = $n + 2); $n", 11},
        // ── function calls: tail, args, nested, and (the real test) calls in
        //    operand position, which only work because every operand is ANF'd
        //    to a temp on the spine. ──
        {"fn dbl($x) ($x + $x); dbl(21)",                                42},
        {"fn add($a, $b) ($a + $b); add(3, 4)",                          7},
        {"fn sq($x) ($x * $x); sq(sq(2))",                              16},
        {"fn g($x) ($x * 2); 1 + g(10)",                               21},
        {"fn fac($n) (if ($n < 2) 1 else $n * fac($n - 1)); fac(5)",   120},
        {"fn sum($n) (if ($n < 1) 0 else $n + sum($n - 1)); sum(5)",    15},
        {"fn fib($n) (if ($n < 2) $n else fib($n-1) + fib($n-2)); fib(10)", 55},
        {"fn ack($m, $n) (if ($m < 1) $n + 1 else (if ($n < 1) ack($m - 1, 1) else ack($m - 1, ack($m, $n - 1)))); ack(2, 3)", 9},
    };
    for (auto& c : cases) {
        s4 got = 0;
        bool ok = calc_eval(c.src, &got);
        if (!ok || got != c.want) {
            printf("FAIL: %-44s got %d want %lld%s\n", c.src, got, (long long)c.want,
                   ok ? "" : " (compile failed)");
            fails++;
        } else {
            printf("ok:   %-44s = %d\n", c.src, got);
        }
    }
    // ── The rewrite pass ────────────────────────────────────────────
    // Every case above already pins pin (1): the pass must not change what a
    // program COMPUTES. The two below pin what it changes and what it does
    // not, which the numeric results cannot distinguish.

    // (2) A pinned expression lowers to the REWRITTEN sequence. `$a * 2`
    // has no shift on this target, so `mul_two` turns it into `$a + $a`:
    // the emitted code loads the local twice and adds, with no constant
    // push and no multiply anywhere in it.
    {
        int fs = 0;
        calc_ir::Node* ir = calc_runner::calc_compile("let $a = 7; $a * 2", &fs);
        std::vector<uint8_t> code = ir ? calc_runner::lower(ir)
                                       : std::vector<uint8_t>();
        // `let $a = 7; $a * 2` = 14. The ddcg emits ANF, so the multiply's
        // operands are loads of temps; resolving each to its definition
        // within the region reassembles `7 * 2`, which folds to 14. The
        // stores stay — this pass rewrites expressions and does no DCE.
        const std::vector<uint8_t> want_rewritten = {
            OP_CONST, 0x07, OP_STORE, 0x00,
            OP_CONST, 0x07, OP_STORE, 0x01,
            OP_CONST, 0x02, OP_STORE, 0x02,
            OP_CONST, 0x0E, OP_RET                 /* 14 */
        };
        // Without the pass: the loads and the multiply are still there.
        const std::vector<uint8_t> want_plain = {
            OP_CONST, 0x07, OP_STORE, 0x00,
            OP_LOAD,  0x00, OP_STORE, 0x01,
            OP_CONST, 0x02, OP_STORE, 0x02,
            OP_LOAD,  0x01, OP_LOAD,  0x02, OP_MUL, OP_RET
        };
#ifdef CALC_EQSAT
        if (!seq_is(code, want_rewritten, "rewrite: `let $a = 7; $a * 2`")) fails++;
        else printf("ok:   rewrite: `$a * 2` lowered as a self-add\n");
#else
        // (3) the falsifier. If the plain build ALSO emits the rewritten
        // sequence, the pass was never what changed the output and pin (2)
        // proves nothing.
        if (!seq_is(code, want_plain, "falsifier: `let $a = 7; $a * 2`")) fails++;
        else printf("ok:   falsifier: without the pass, the multiply stays\n");
#endif
    }

    // Constant folding reaches the same conclusion by a different route:
    // `2 * 3 + 1` is one pushed constant once the rules have run.
    {
        int fs = 0;
        calc_ir::Node* ir = calc_runner::calc_compile("2 * 3 + 1", &fs);
        std::vector<uint8_t> code = ir ? calc_runner::lower(ir)
                                       : std::vector<uint8_t>();
        const std::vector<uint8_t> folded = { OP_CONST, 0x07, OP_RET };
        // The DDCG is destination-driven: every binop operand is spilled to a
        // temp and reloaded, so this is `2*3+1` in ANF with the let-bindings
        // realized as slots. It is also exactly why the pass cannot run here —
        // the multiply's operands are loads, not the constants.
        const std::vector<uint8_t> unfolded = {
            OP_CONST, 0x02, OP_STORE, 0x02,
            OP_CONST, 0x03, OP_STORE, 0x03,
            OP_LOAD,  0x02, OP_LOAD,  0x03, OP_MUL, OP_STORE, 0x00,
            OP_CONST, 0x01, OP_STORE, 0x01,
            OP_LOAD,  0x00, OP_LOAD,  0x01, OP_ADD, OP_RET
        };
#ifdef CALC_EQSAT
        if (!seq_is(code, folded, "rewrite: `2 * 3 + 1`")) fails++;
        else printf("ok:   rewrite: `2 * 3 + 1` folded to one constant\n");
#else
        if (!seq_is(code, unfolded, "falsifier: `2 * 3 + 1`")) fails++;
        else printf("ok:   falsifier: without the pass, the arithmetic stays\n");
#endif
    }

    if (!fails) { printf("\ncalc e2e (parse → ddcg → IR → bytecode → opgen VM): all passed\n"); return 0; }
    printf("\ncalc e2e: %d FAILED\n", fails);
    return 1;
}
