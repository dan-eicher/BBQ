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
#include <cstdio>
#include <vector>

#define TAIL __attribute__((musttail))

static const opcode_handler_t* g_table;
extern "C" void wasm_next(vm_t* vm) {
    u1 op; if (!bbq_read_u8(&vm->frame.code, &op)) return;
    TAIL return g_table[op](vm);
}
extern "C" void wasm_trap(vm_t* vm) { vm->trapped = 1; }

static s4 run_bytecode(const std::vector<uint8_t>& code) {
    static vm_t vm;
    g_table = gen_interp_dispatch_table();
    bbq_ctx_init(&vm.frame.code, code.data(), code.size());
    vm.frame.sp = 0; vm.depth = 0; vm.trapped = 0; vm.result.i = 0; vm.result_type = T_INT;
    wasm_next(&vm);
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

static int fails = 0;
int main(void) {
    struct Case { const char* src; int64_t want; };
    static const Case cases[] = {
        {"5",                                       5},
        {"1+2",                                     3},
        {"2+3*4",                                  14},
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
    if (!fails) { printf("\ncalc e2e (parse → ddcg → IR → bytecode → opgen VM): all passed\n"); return 0; }
    printf("\ncalc e2e: %d FAILED\n", fails);
    return 1;
}
