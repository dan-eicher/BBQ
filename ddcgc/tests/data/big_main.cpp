// End-to-end driver for big.ddcg. Builds a tree designed to fire every
// rule, runs the generated Compiler, then plays back the trace the
// dispatcher recorded. The trace is the test report — each rule fires
// `trace_fire("<rule-name>")`, every aux logs its arguments, and label
// placement appends a marker. If a rule body changes shape (or stops
// firing) the trace diff says exactly which feature regressed.

#include "big_runtime.h"
#include "big_compile.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

// Each entry: (rule label, sub-trace expected when that rule fires).
// Catalogues the features the .ddcg fixture is supposed to exercise.
const std::vector<std::pair<const char*, const char*>> kFeatures = {
    {"add_zero_l",  "Nested ConstructorPat in field position"},
    {"lit_zero",    "Where-guard predicate over int field"},
    {"lit",         "Tuple-destructuring let + ternary + tuple-returning aux"},
    {"add",         "Multi-rule fallback after nested specialisation"},
    {"seq",         "For-loop accumulator + list concat + Sequence field"},
    {"pair",        "Label statement + label-payload γ variant + place()"},
    {"tag_x",       "StringPat literal match"},
    {"tag_short",   "Where-guard predicate over string field"},
    {"tag",         "Generic string-field fallback"},
    {"maybe_none",  "NilPat against optional field"},
    {"maybe_some",  "BindPat against optional field carrying a value"},
    {"binop_add",   "Enum-value field-pat (OpAdd constant match)"},
    {"binop_sub",   "Enum-value field-pat (OpSub constant match)"},
    {"binop_mul",   "Enum-value field-pat (OpMul constant match)"},
    {"sel",         "match over asdl-enum subject (op_to_int fun)"},
};

// Action-language markers logged by the dest_to_value fun (match → build),
// invoked from the `pair` rule body. Each entry: (trace marker, what the
// arm exercised).
const std::vector<std::pair<const char*, const char*>> kActionMarkers = {
    {"log_value:imm:99", "fun + match `ac` arm + build target.Imm"},
    {"log_value:slot:7", "fun + match `spill(slot)` arm + build target.Slot"},
    {"rule:import_demo", "fun imported from big_imports.ddcg via `import` directive"},
};

bool trace_contains(const std::vector<std::string>& trace,
                     const std::string& needle) {
    for (const auto& s : trace) {
        if (s == needle) return true;
    }
    return false;
}

void dump_trace(const std::vector<std::string>& trace) {
    std::fprintf(stderr, "── trace (%zu entries) ──\n", trace.size());
    for (const auto& s : trace) std::fprintf(stderr, "  %s\n", s.c_str());
}

} // namespace

int main() {
    using namespace big;

    // Build a tree that hits every rule. Comments tag which rule each
    // sub-tree exercises.
    Lit l0(0), l7(7);
    Add add_zero_left(&l0, &l7);            // → add_zero_l (matches Add(Lit(0), _))

    Lit lit_zero_node(0);                    // → lit_zero  (where is_zero)
    Lit lit_50(50);                          // → lit       (regular)

    Lit l2(2), l3(3);
    Add add_general(&l2, &l3);               // → add       (fallback)

    Lit l10(10), l20(20);
    Pair pair_node(&l10, &l20);              // → pair      (label test)

    Tag tag_x_node("x");                     // → tag_x     (StringPat)
    Tag tag_short_node("ab");                // → tag_short (predicate)
    Tag tag_long_node("longname");           // → tag       (fallback)

    Maybe maybe_none_node(std::nullopt);     // → maybe_none
    Lit l5(5);
    Maybe maybe_some_node(std::optional<Expr*>{&l5});  // → maybe_some

    Lit l4(4), l6(6), l8(8), l9(9);
    BinOp binop_add_node(Op::OpAdd, &l4, &l6);  // → binop_add (4+6=10)
    BinOp binop_sub_node(Op::OpSub, &l8, &l9);  // → binop_sub (8+9=17, cg_pick is sum)
    BinOp binop_mul_node(Op::OpMul, &l2, &l3);  // → binop_mul (2+3=5)
    Sel sel_node(Op::OpMul);                    // → sel (op_to_int(OpMul)=3)

    Seq root({
        &add_zero_left,
        &lit_zero_node,
        &lit_50,
        &add_general,
        &pair_node,
        &tag_x_node,
        &tag_short_node,
        &tag_long_node,
        &maybe_none_node,
        &maybe_some_node,
        &binop_add_node,
        &binop_sub_node,
        &binop_mul_node,
        &sel_node,
    });

    Compiler compiler;
    rho_t rho{0};
    int result = compiler.compile_expr(&root, rho, ac(), fail());

    // Verify every catalogued feature fired.
    bool ok = true;
    for (const auto& [name, descr] : kFeatures) {
        std::string marker = std::string("rule:") + name;
        if (!trace_contains(compiler.trace, marker)) {
            std::fprintf(stderr, "FAIL: rule '%s' (%s) never fired\n",
                         name, descr);
            ok = false;
        }
    }
    for (const auto& [marker, descr] : kActionMarkers) {
        if (!trace_contains(compiler.trace, marker)) {
            std::fprintf(stderr, "FAIL: action-language marker '%s' (%s) never logged\n",
                         marker, descr);
            ok = false;
        }
    }

    // The expected numeric result is derived in the .ddcg's comments;
    // here we verify by spot check rather than rederiving, but log the
    // raw value either way so a numeric drift is visible.
    //
    //   add_zero_l(Lit(0), Lit(7)) → 0 + lit(7)               = 0 + 7    = 7
    //   lit_zero(0)                → 0
    //   lit(50)                    → 50
    //   add(Lit(2), Lit(3))        → lit(2) + lit(3)           = 2 + 3    = 5
    //   pair(Lit(10), Lit(20))     → cg_pick(lit(20) , 0)      = 20 + 0   = 20
    //   tag_x                      → len("x-special")          = 9
    //   tag_short("ab")            → len("ab")                 = 2
    //   tag("longname")            → len("longname")           = 8
    //   maybe_none                 → -1
    //   maybe_some(Lit(5))         → lit(5)                    = 5
    //   binop_add(Lit(4), Lit(6))  → cg_pick(lit(4) , lit(6))  = 4 + 6   = 10
    //   binop_sub(Lit(8), Lit(9))  → cg_pick(lit(8) , lit(9))  = 8 + 9   = 17
    //   binop_mul(Lit(2), Lit(3))  → cg_pick(lit(2) , lit(3))  = 2 + 3   = 5
    //   sel(OpMul)                 → cg_lit(op_to_int(OpMul))   = 3
    //                                                  Σ items = 140
    constexpr int kExpected = 140;
    if (result != kExpected) {
        std::fprintf(stderr, "FAIL: expected result=%d, got %d\n",
                     kExpected, result);
        ok = false;
    }

    if (!ok) {
        dump_trace(compiler.trace);
        return 1;
    }

    std::printf("ddcgc big.ddcg — %zu features + %zu action-lang markers:\n",
                kFeatures.size(), kActionMarkers.size());
    for (const auto& [name, descr] : kFeatures) {
        std::printf("  ✓ %-16s  %s\n", name, descr);
    }
    for (const auto& [marker, descr] : kActionMarkers) {
        std::printf("  ✓ %-16s  %s\n", marker, descr);
    }
    std::printf("result=%d, trace=%zu entries\n",
                result, compiler.trace.size());
    return 0;
}
