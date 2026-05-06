#include "calc_runner.h"

#include "CalcMatcher.h"
#include "calc_compile.h"
#include "Parser.h"

#include <cassert>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace calc_runner {

namespace {

// Pre-DDCG AST pass: implements paper page 12 boolean-context rewrites
// and accumulates the max referenced $N slot. Walks the AST as a
// `calc::ASTVisitor` (asdl-emitted) carrying a `Context` that mirrors
// the (δ, γ) shape categories DDCG's gen() will receive — so each
// boolean producer can apply the matching page-12 rule.
//
// Page 12 rules implemented (priority order — most specific first):
//   2.  And(E1, E2)         in Effect+SingleLabel  ⇒  If(E1, E2, IntLit(0))
//   3.  Or(E1, E2)          in Effect+SingleLabel  ⇒  If(Not(E1), E2, IntLit(0))
//   4.  Cmp(E1, E2)         in Effect+SingleLabel  ⇒  Sequence(E1, E2)
//   5.  Not(E)              in Effect+SingleLabel  ⇒  E
//   1.  Ebool                in any non-Effect ctx ⇒  If(Ebool, IntLit(1), IntLit(0))
//
// Also fills missing else_ branches with IntLit(0) (paper figure 5
// one-arm if requires γ=L; calc desugars else-less Ifs to two-arm so
// the value-context default is well-defined).

enum class Dest { Effect, AcOrLoc };
enum class Ctrl { Pair, SingleLabel, Ret };
struct Context { Dest dest; Ctrl ctrl; };

class Normalizer : public calc::ASTVisitor {
public:
    // Per-function frame_size, populated by run_function() / run_main().
    // The driver consults this to populate the Compiler's fn_table.
    std::unordered_map<std::string, int> function_frame_sizes;
    int main_frame_size = 0;

    calc::Expr* run(calc::Expr* e, Context c) {
        if (!e) return nullptr;
        Context saved = ctx_;
        ctx_ = c;
        result_ = e;
        e->accept(*this);
        calc::Expr* out = result_;
        ctx_ = saved;
        return out;
    }

    // Normalize a function body with a fresh scope chain. params are
    // bound to local indices [0, n_params); let-bindings extend from
    // n_params upward. Returns the rewritten body; records the
    // function's frame_size = next_local in `function_frame_sizes`.
    calc::Expr* run_function(calc::Function* fn) {
        scope_.clear();
        scope_.push_back({});
        next_local_ = 0;
        for (auto& p : fn->params) {
            scope_.back()[p] = next_local_++;
        }
        calc::Expr* body = run(fn->body, {Dest::AcOrLoc, Ctrl::SingleLabel});
        function_frame_sizes[fn->name] = next_local_;
        return body;
    }

    // Normalize the top-level main expression with its own fresh frame.
    // Main has no params; let-bindings start at local 0.
    calc::Expr* run_main(calc::Expr* body) {
        scope_.clear();
        scope_.push_back({});
        next_local_ = 0;
        calc::Expr* out = run(body, {Dest::AcOrLoc, Ctrl::SingleLabel});
        main_frame_size = next_local_;
        return out;
    }

    // Program is never reached via run() (caller passes program->body
    // directly). The default no-op override from ASTVisitor is fine.
    void visit(calc::IntLit* x) override { result_ = x; }
    void visit(calc::BinOp* x) override {
        x->left  = run(x->left,  {Dest::AcOrLoc, Ctrl::SingleLabel});
        x->right = run(x->right, {Dest::AcOrLoc, Ctrl::SingleLabel});
        result_ = x;
    }
    void visit(calc::NegOp* x) override {
        x->operand = run(x->operand, {Dest::AcOrLoc, Ctrl::SingleLabel});
        result_ = x;
    }
    void visit(calc::Sequence* x) override {
        x->first = run(x->first, {Dest::Effect, Ctrl::SingleLabel});
        x->rest  = run(x->rest, ctx_);  // inherits enclosing context
        result_ = x;
    }
    void visit(calc::If* x) override {
        x->test  = run(x->test, {Dest::Effect, Ctrl::Pair});
        x->then_ = run(x->then_, ctx_);
        calc::Expr* el = x->else_.has_value() ? *x->else_ : nullptr;
        calc::Expr* el2 = el ? run(el, ctx_) : new calc::IntLit(0);
        x->else_ = std::optional<calc::Expr*>(el2);
        result_ = x;
    }
    void visit(calc::While* x) override {
        // Paper page 10: while Etest Ebody ⇒ loop (if Etest Ebody break).
        // Normalize the inner pieces first, then wrap.
        calc::Expr* nT = run(x->test, {Dest::Effect, Ctrl::Pair});
        calc::Expr* nB = run(x->body, {Dest::Effect, Ctrl::SingleLabel});
        calc::Expr* inner_if = new calc::If(
            nT, nB,
            std::optional<calc::Expr*>(new calc::Break()));
        result_ = new calc::Loop(inner_if);
    }
    void visit(calc::Loop* x) override {
        x->body = run(x->body, {Dest::Effect, Ctrl::SingleLabel});
        result_ = x;
    }
    void visit(calc::Break* x) override { result_ = x; }
    void visit(calc::Return* x) override {
        x->value = run(x->value, {Dest::AcOrLoc, Ctrl::Ret});
        result_ = x;
    }

    // Comparisons — figure 7 in test position; page 12 elsewhere.
    void visit(calc::CmpEq*  x) override { handle_cmp(x); }
    void visit(calc::CmpNeq* x) override { handle_cmp(x); }
    void visit(calc::CmpLt*  x) override { handle_cmp(x); }
    void visit(calc::CmpLe*  x) override { handle_cmp(x); }
    void visit(calc::CmpGt*  x) override { handle_cmp(x); }
    void visit(calc::CmpGe*  x) override { handle_cmp(x); }

    void visit(calc::And* x) override {
        if (in_effect_label()) {
            // Page 12 rule 2: and E1 E2 ⇒ if E1 E2.
            calc::Expr* nE1 = run(x->left,  {Dest::Effect, Ctrl::Pair});
            calc::Expr* nE2 = run(x->right, ctx_);
            result_ = new calc::If(
                nE1, nE2,
                std::optional<calc::Expr*>(new calc::IntLit(0)));
            return;
        }
        // Either Effect+Pair (figure 7) or AcOrLoc (page 12 rule 1).
        x->left  = run(x->left,  {Dest::Effect, Ctrl::Pair});
        x->right = run(x->right, {Dest::Effect, Ctrl::Pair});
        result_ = wrap_if_value_context(x);
    }
    void visit(calc::Or* x) override {
        if (in_effect_label()) {
            // Page 12 rule 3: or E1 E2 ⇒ if (not E1) E2.
            calc::Expr* nE1 = run(x->left,  {Dest::Effect, Ctrl::Pair});
            calc::Expr* nE2 = run(x->right, ctx_);
            result_ = new calc::If(
                new calc::Not(nE1), nE2,
                std::optional<calc::Expr*>(new calc::IntLit(0)));
            return;
        }
        x->left  = run(x->left,  {Dest::Effect, Ctrl::Pair});
        x->right = run(x->right, {Dest::Effect, Ctrl::Pair});
        result_ = wrap_if_value_context(x);
    }
    void visit(calc::Not* x) override {
        if (in_effect_label()) {
            // Page 12 rule 5: not E ⇒ E.
            result_ = run(x->operand, ctx_);
            return;
        }
        x->operand = run(x->operand, {Dest::Effect, Ctrl::Pair});
        result_ = wrap_if_value_context(x);
    }

    // Phase 9 — variable scope (paper ρ.m). Var/VarAssign/LetDecl are
    // unresolved surface forms; the scope chain resolves them to
    // LocalRef/LocalAssign with frame-relative indices.
    void visit(calc::Var* x) override {
        result_ = new calc::LocalRef(lookup(x->name));
    }
    void visit(calc::VarAssign* x) override {
        int idx = lookup(x->name);
        calc::Expr* nv = run(x->value, {Dest::AcOrLoc, Ctrl::SingleLabel});
        result_ = new calc::LocalAssign(idx, nv);
    }
    void visit(calc::LetDecl* x) override {
        // Allocate the binding before normalizing the value so a let
        // can refer to itself (`let $a = $a + 1` reads the current $a
        // if one's in outer scope, then rebinds — typical lexical-let
        // semantics). For Phase 9c that means: assign the index first,
        // then normalize value with the binding visible.
        int idx = next_local_++;
        scope_.back()[x->name] = idx;
        calc::Expr* nv = run(x->value, {Dest::AcOrLoc, Ctrl::SingleLabel});
        result_ = new calc::LocalAssign(idx, nv);
    }
    void visit(calc::Call* x) override {
        // Args needs values; recurse with AcOrLoc/SingleLabel. The
        // function name resolution happens at DDCG time via the lib
        // call_op rule's lookup_function_* AUX.
        for (auto& a : x->args) {
            a = run(a, {Dest::AcOrLoc, Ctrl::SingleLabel});
        }
        result_ = x;
    }
    void visit(calc::LocalRef* x) override { result_ = x; }
    void visit(calc::LocalAssign* x) override {
        x->value = run(x->value, {Dest::AcOrLoc, Ctrl::SingleLabel});
        result_ = x;
    }

private:
    Context ctx_{Dest::AcOrLoc, Ctrl::SingleLabel};
    calc::Expr* result_ = nullptr;
    std::vector<std::unordered_map<std::string, int>> scope_;
    int next_local_ = 0;

    int lookup(const std::string& name) {
        for (auto it = scope_.rbegin(); it != scope_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        assert(false && "Normalizer::lookup: unbound variable");
        return -1;
    }

    bool in_effect_label() const {
        return ctx_.dest == Dest::Effect && ctx_.ctrl == Ctrl::SingleLabel;
    }

    // Wrap a boolean producer in `if Ebool 1 0` when the surrounding
    // context wants its value (paper page 12 rule 1). Pass-through if
    // we're in Effect+Pair (figure 7 handles it in DDCG).
    calc::Expr* wrap_if_value_context(calc::Expr* boolprod) {
        if (ctx_.dest == Dest::Effect) return boolprod;
        return new calc::If(
            boolprod, new calc::IntLit(1),
            std::optional<calc::Expr*>(new calc::IntLit(0)));
    }

    template <class CmpT> void handle_cmp(CmpT* x) {
        if (in_effect_label()) {
            // Page 12 rule 4: relop E1 E2 ⇒ sequence E1 E2 (both for effect).
            calc::Expr* L = run(x->left,  {Dest::Effect, Ctrl::SingleLabel});
            calc::Expr* R = run(x->right, {Dest::Effect, Ctrl::SingleLabel});
            result_ = new calc::Sequence(L, R);
            return;
        }
        // Operands compute values for the comparison.
        x->left  = run(x->left,  {Dest::AcOrLoc, Ctrl::SingleLabel});
        x->right = run(x->right, {Dest::AcOrLoc, Ctrl::SingleLabel});
        result_ = wrap_if_value_context(x);
    }
};

calc::Program* parse_program(const char* input) {
    Parser parser;
    parser.init(input, (int)std::strlen(input));
    if (!parser.parse() || !parser.ast) return nullptr;
    return parser.ast;
}

} // namespace

calc_ir::Node* calc_compile(const char* input, int* main_frame_size) {
    auto* program = parse_program(input);
    if (!program) return nullptr;

    Normalizer norm;
    // Normalize each function body in its own scope; record frame_size
    // for the call-site lookup. Body remains attached to fn->body.
    for (auto* fn : program->functions) {
        fn->body = norm.run_function(fn);
    }
    // Normalize main body in its own (top-level) scope; record main's
    // frame_size for any case-4 spills we use locally there.
    program->body = norm.run_main(program->body);

    calc::Compiler compiler;

    // Pre-allocate Nop placeholders for each function's entry — lets
    // mutual recursion resolve forward refs through the function table
    // before any body is compiled.
    std::unordered_map<std::string, calc_ir::Node*> entry_nops;
    for (auto* fn : program->functions) {
        calc_ir::Node* entry_nop = compiler.fresh_kont();
        entry_nops[fn->name] = entry_nop;
        int fs = norm.function_frame_sizes[fn->name];
        compiler.fn_table[fn->name] = {entry_nop, fs};
    }

    // Compile each function body. Reset the compiler's per-function
    // local counter to (n_params + n_lets); case-4 spills bump it
    // further during compile.
    //
    // γ=ret: cg_deliver wraps the body's value in make_leave(value).
    // The body might be a pure value sub-tree, a Call sub-tree, or a
    // spine-rooted Loop — all handled uniformly by cg_deliver.
    for (auto* fn : program->functions) {
        compiler.reset_next_local(norm.function_frame_sizes[fn->name]);
        calc::rho_t fn_rho = calc::frame(nullptr);
        calc_ir::Node* fn_root = compiler.compile_expr(
            fn->body, fn_rho, calc::ac(), calc::ret(), nullptr);
        compiler.patch_kont(entry_nops[fn->name], fn_root);
        compiler.fn_table[fn->name].second = compiler.current_next_local();
    }

    // Main: same scheme. cg_deliver(value, ac, ret, _) emits Leave
    // (which is stencil-identical to Halt — both `ret`); program
    // terminates and ctx.ac holds the result for the C-side reader.
    compiler.reset_next_local(norm.main_frame_size);
    calc::rho_t rho = calc::frame(nullptr);
    calc_ir::Node* root = compiler.compile_expr(
        program->body, rho, calc::ac(), calc::ret(), nullptr);
    // Final next_local includes any case-4 spills allocated during
    // compile — that's main's true frame size for the runtime sp init.
    if (main_frame_size) *main_frame_size = compiler.current_next_local();
    return root;
}

int64_t calc_run(const char* input, bool* ok) {
    int main_frame_size = 0;
    calc_ir::Node* ir = calc_compile(input, &main_frame_size);
    if (!ir) {
        if (ok) *ok = false;
        return 0;
    }

    JitContext jctx;
    jit_ctx = &jctx;
    jctx.emit_entry();
    BurgMatcher matcher;

    // Lower main first so entry → emitted[0] lands in main's first
    // stencil. Function bodies (independent IR roots reached via
    // Call.target) get lowered after; backpatch_all resolves each
    // Call's _HOLE_target via first_in_tree(target).
    matcher.burg_rewrite(ir);
    {
        std::unordered_set<calc_ir::Node*> seen;
        std::unordered_set<calc_ir::Node*> fn_entries_seen;
        std::vector<calc_ir::Node*> stack{ir};
        std::vector<calc_ir::Node*> fn_entries;
        while (!stack.empty()) {
            auto* n = stack.back(); stack.pop_back();
            if (!n || !seen.insert(n).second) continue;
            if (n->tag == calc_ir::NodeTag::Call) {
                auto* tgt = static_cast<calc_ir::Call*>(n)->target;
                if (fn_entries_seen.insert(tgt).second) {
                    fn_entries.push_back(tgt);
                }
            }
            int oc = operand_count(n);
            for (int i = 0; i < oc; i++) stack.push_back(operand_producer(n, i));
            int sc = successor_count(n);
            for (int i = 0; i < sc; i++) stack.push_back(successor(n, i));
        }
        for (auto* fn_entry : fn_entries) {
            matcher.burg_rewrite(fn_entry);
        }
    }
    jctx.backpatch_all();

    auto* fn = (void(*)(Ctx*))jctx.buf.finalize();
    Ctx ctx{};
    // Initialize sp past main's locals so eval-stack temps don't
    // clobber them. (Function calls handle this internally via the
    // call stencil's sp arithmetic; main has no caller to do it.)
    ctx.sp = main_frame_size;
    fn(&ctx);

    if (ok) *ok = true;
    return ctx.ac;
}

int count_node_kind(calc_ir::Node* ir, calc_ir::NodeTag kind) {
    int count = 0;
    std::vector<calc_ir::Node*> stack{ir};
    std::unordered_set<calc_ir::Node*> seen;
    while (!stack.empty()) {
        auto* n = stack.back(); stack.pop_back();
        if (!n || !seen.insert(n).second) continue;
        if (n->tag == kind) count++;
        // CEK-tree IR: walk both spine successors AND operand sub-trees
        // — value-producers (Add/LoadLocal/etc.) live only as operands.
        int sc = successor_count(n);
        for (int i = 0; i < sc; i++) stack.push_back(successor(n, i));
        int oc = operand_count(n);
        for (int i = 0; i < oc; i++) stack.push_back(operand_producer(n, i));
    }
    return count;
}

} // namespace calc_runner
