// Driver for the paper-direct DDCG end-to-end test.
//
// Builds a tiny AST (Lit(2), Lit(3), Add of those), runs the generated
// paper_consumer::Compiler against it with a top-level (δ=ac, γ=ret,
// Lnext=halt), then walks the resulting IR graph and verifies its
// shape is exactly what the paper-direct cg_helpers should produce.
//
// What this test pins:
//   * dybvig.ddcg loads through the -L search path
//   * δ × γ × Lnext threading reaches all the way through cg_deliver
//   * the L == Lnext peephole actually fires (no extra Goto nodes)
//   * `match` over γ tagged-union variants compiles + dispatches
//   * env_dest ρ threads through the dispatcher signature
//   * CEK-tree IR shape: Add carries sub-tree operands, LoadConst is
//     a leaf, no per-producer `next` field

#include "paper_consumer_runtime.h"
#include "paper_consumer_compile.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

using paper_consumer::Compiler;
using paper_consumer::ac;
using paper_consumer::effect;
using paper_consumer::ret;
using paper_consumer::jump;
using paper_consumer::pair;
using paper_consumer::rho_t;

namespace {

// Walk the IR graph from `root`, collecting reachable nodes. Producer
// nodes (Add, LoadConst, LoadLocal) carry sub-tree operand fields;
// consumer nodes (Halt, Leave, ExprEffect, StoreLocal, Branch, Goto)
// carry `next` plus optional value sub-trees.
void collect(ir::Expr* root, std::set<ir::Expr*>& seen) {
    if (!root || !seen.insert(root).second) return;
    if (auto* h = dynamic_cast<ir::Halt*>(root))            collect(h->value, seen);
    else if (auto* l = dynamic_cast<ir::Leave*>(root))      collect(l->value, seen);
    else if (auto* g = dynamic_cast<ir::Goto*>(root))       collect(g->target, seen);
    else if (auto* e = dynamic_cast<ir::ExprEffect*>(root)) { collect(e->value, seen); collect(e->next, seen); }
    else if (auto* s = dynamic_cast<ir::StoreLocal*>(root)) { collect(s->value, seen); collect(s->next, seen); }
    else if (auto* b = dynamic_cast<ir::Branch*>(root))     { collect(b->test, seen); collect(b->then_, seen); collect(b->else_, seen); }
    else if (auto* a = dynamic_cast<ir::Add*>(root))        { collect(a->left, seen); collect(a->right, seen); }
    // LoadConst / LoadLocal are leaves — no sub-trees to visit.
}

const char* kind_name(int k) {
    switch (k) {
        case 0: return "Halt";       case 1: return "Goto";
        case 2: return "LoadConst";  case 3: return "LoadLocal";
        case 4: return "StoreLocal"; case 5: return "Add";
        case 6: return "Branch";     case 7: return "Call";
        case 8: return "ExprEffect"; case 9: return "Leave";
        default: return "?";
    }
}

void dump(ir::Expr* root) {
    std::set<ir::Expr*> seen;
    collect(root, seen);
    std::fprintf(stderr, "── ir graph (%zu nodes) ──\n", seen.size());
    for (auto* n : seen) {
        std::fprintf(stderr, "  %p %s", (void*)n, kind_name(n->kind()));
        if (auto* l = dynamic_cast<ir::LoadConst*>(n))
            std::fprintf(stderr, "(value=%d)", l->value);
        else if (auto* a = dynamic_cast<ir::Add*>(n))
            std::fprintf(stderr, "(left=%p, right=%p)",
                         (void*)a->left, (void*)a->right);
        else if (auto* h = dynamic_cast<ir::Halt*>(n))
            std::fprintf(stderr, "(value=%p)", (void*)h->value);
        std::fprintf(stderr, "\n");
    }
}

} // namespace

int main() {
    // AST: Add(Lit(2), Lit(3))
    ast::Lit lit2(2);
    ast::Lit lit3(3);
    ast::Add add(&lit2, &lit3);

    Compiler compiler;
    rho_t rho = paper_consumer::frame(nullptr);

    // Top-level call: paper says ρ_init, δ=ac, γ=ret. Driver compiles
    // body with γ=jump(Lnext) where Lnext is a sentinel — the peephole
    // (L==Lnext) returns the bare value sub-tree, which the driver
    // then wraps in Halt(value).
    ir::Expr* sentinel = compiler.make_halt(nullptr);  // sentinel only — value patched out
    ir::Expr* body_value = compiler.compile_expr(&add, rho, ac(), jump(sentinel), sentinel);
    ir::Expr* root = compiler.make_halt(body_value);

    // ── Shape assertion: the IR is a CEK-tree.
    //   Halt(Add(LoadConst(2), LoadConst(3)))
    // 4 reachable nodes: Halt, Add, 2x LoadConst. No Goto (L==Lnext
    // peephole), no StoreLocal (no spill at IR level), no Branch.
    // The sentinel Halt(nullptr) is unreachable from `root` — only
    // reachable nodes count.
    std::set<ir::Expr*> reachable;
    collect(root, reachable);

    int load_const_count = 0;
    int store_local_count = 0;
    int goto_count = 0;
    int halt_count = 0;
    int branch_count = 0;
    int add_count = 0;
    for (auto* n : reachable) {
        switch (n->kind()) {
            case 0: ++halt_count;        break;
            case 1: ++goto_count;        break;
            case 2: ++load_const_count;  break;
            case 4: ++store_local_count; break;
            case 5: ++add_count;         break;
            case 6: ++branch_count;      break;
        }
    }

    bool ok = true;
    if (load_const_count != 2) {
        std::fprintf(stderr, "FAIL: expected 2 LoadConst (lit 2, lit 3), got %d\n",
                     load_const_count);
        ok = false;
    }
    if (store_local_count != 0) {
        std::fprintf(stderr, "FAIL: expected 0 StoreLocal (CEK-tree has no IR-level spill), got %d\n",
                     store_local_count);
        ok = false;
    }
    if (add_count != 1) {
        std::fprintf(stderr, "FAIL: expected 1 Add (the binop), got %d\n",
                     add_count);
        ok = false;
    }
    if (goto_count != 0) {
        std::fprintf(stderr, "FAIL: expected 0 Goto (paper L==Lnext peephole), got %d\n",
                     goto_count);
        ok = false;
    }
    if (halt_count != 1) {
        std::fprintf(stderr, "FAIL: expected 1 Halt (driver-wrapped terminator), got %d\n",
                     halt_count);
        ok = false;
    }
    if (branch_count != 0) {
        std::fprintf(stderr, "FAIL: expected 0 Branch (no test exprs), got %d\n",
                     branch_count);
        ok = false;
    }

    if (!ok) {
        dump(root);
        return 1;
    }

    std::printf("paper_consumer: %zu nodes, %d LoadConst, %d Add, %d Halt, "
                "0 Goto (L==Lnext peephole fired) — CEK-tree ✓\n",
                reachable.size(), load_const_count, add_count, halt_count);
    return 0;
}
