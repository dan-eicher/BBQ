#include "generator.h"

namespace {

class CBurgBackend : public BurgBackend {
    std::string ctx_arg() const override { return ", ctx"; }
    std::string ctx_solo() const override { return "ctx"; }
    std::string state_type() const override { return "burg_state_t"; }

    void emit_constants(std::ostream& out) override;
    void emit_label_body(std::ostream& out, int indent) override;
    void emit_rpo_dfs(std::ostream& out, int indent) override;
    void emit_rewrite_body(std::ostream& out, int indent) override;

public:
    void generate(std::ostream& out, const BurgAnalysis& a) override {
        a_ = &a;

        // In C, NAMESPACE becomes a symbol prefix (e.g., "myns_")
        std::string ns_prefix;
        if (!a.spec->ns.empty()) {
            ns_prefix = a.spec->ns + "_";
        }

        emit_constants(out);
        emit_user_headers(out);
        emit_macros_check(out);

        // ── State record ──
        out << "/* ── State record ── */\n\n";
        emit_state_struct(out, 0);

        // ── Context struct ──
        out << "/* ── Context ── */\n\n";
        std::string ctx_type = ns_prefix + "burg_ctx_t";

        out << "typedef struct " << ctx_type << " {\n";
        out << "    bbq_arena arena;\n";
        out << "    bbq_htree* state_cache;\n";
        if (!a.spec->members.empty()) {
            std::string trimmed = trim(a.spec->members);
            if (!trimmed.empty())
                out << "    " << trimmed << "\n";
        }
        out << "} " << ctx_type << ";\n\n";

        // ── Context lifecycle ──
        out << "static void " << ns_prefix << "burg_ctx_init(" << ctx_type << "* ctx) {\n";
        out << "    bbq_arena_init(&ctx->arena, 4096);\n";
        out << "    ctx->state_cache = bbq_htree_create();\n";
        out << "}\n\n";

        out << "static void " << ns_prefix << "burg_ctx_free(" << ctx_type << "* ctx) {\n";
        out << "    bbq_arena_free(&ctx->arena);\n";
        out << "    bbq_htree_destroy(ctx->state_cache);\n";
        out << "    ctx->state_cache = NULL;\n";
        out << "}\n\n";

        // ── Arena helper (delegates to CRT) ──
        out << "static void* arena_alloc(size_t size, " << ctx_type << "* ctx) {\n";
        out << "    return bbq_arena_alloc(&ctx->arena, size);\n";
        out << "}\n\n";
        out << "static void arena_reset(" << ctx_type << "* ctx) {\n";
        out << "    bbq_arena_reset(&ctx->arena);\n";
        out << "}\n\n";

        // ── Cache functions (delegate to htree) ──
        out << "/* ── State cache (graph path only) ── */\n\n";
        out << "static burg_state_t* burg_cache_lookup(uint32_t id, " << ctx_type << "* ctx) {\n";
        out << "    return (burg_state_t*)bbq_htree_search(ctx->state_cache, id);\n";
        out << "}\n\n";
        out << "static void burg_cache_store(uint32_t id, burg_state_t* state, " << ctx_type << "* ctx) {\n";
        out << "    bbq_htree_insert(ctx->state_cache, id, state);\n";
        out << "}\n\n";
        out << "static void burg_cache_clear(" << ctx_type << "* ctx) {\n";
        out << "    bbq_htree_clear(ctx->state_cache);\n";
        out << "}\n\n";

        // Closures (static, with forward declarations)
        emit_closures(out, 0, "static ", true);

        // DP (static + ctx)
        out << "/* ── Cost DP (shared by tree and graph label) ── */\n\n";
        out << "static void burg_dp(burg_state_t* p, BURG_NODE_TYPE node, " << ctx_type << "* ctx) {\n";
        emit_dp_body(out, 1);
        out << "}\n\n";

        // Label tree (static + ctx, no cache)
        out << "/* ── Label (tree path - no cache) ── */\n\n";
        out << "static burg_state_t* burg_label_tree(BURG_NODE_TYPE node, " << ctx_type << "* ctx) {\n";
        emit_label_tree_body(out, 1);
        out << "}\n\n";

        // Label graph (static + ctx, with cache)
        out << "/* ── Label (graph path - cached) ── */\n\n";
        out << "static burg_state_t* burg_label(BURG_NODE_TYPE node, " << ctx_type << "* ctx) {\n";
        emit_label_body(out, 1);
        out << "}\n\n";

        // Rule lookup
        out << "/* ── Rule lookup ── */\n\n";
        out << "static BURG_UNUSED int " << ns_prefix << "burg_rule(burg_state_t* state, int goalnt) {\n";
        emit_rule_func_body(out, 1);
        out << "}\n\n";

        // NT name
        out << "/* ── Nonterminal names (for debugging) ── */\n\n";
        out << "static BURG_UNUSED const char* " << ns_prefix << "burg_nt_name(int nt) {\n";
        emit_nt_name_body(out, 1);
        out << "}\n";

        // Reducer (static + ctx, only if actions)
        if (a.has_actions) {
            out << "\n/* ── Reducer (top-down pass) ── */\n\n";
            out << "static void burg_reduce(BURG_NODE_TYPE node, burg_state_t* state, int goalnt, " << ctx_type << "* ctx) {\n";
            emit_reduce_body(out, 1);
            out << "}\n";
        }

        // Rewriter (public entry point — prefixed)
        out << "\n/* ── Rewriter ── */\n\n";
        out << "static void " << ns_prefix << "burg_rewrite(BURG_NODE_TYPE root, " << ctx_type << "* ctx) {\n";
        emit_rewrite_body(out, 1);
        out << "}\n";

        out << "\n#endif /* BURG_GENERATED_H */\n";
    }
};

// ── C-specific preamble (C11 includes + #define constants) ──

void CBurgBackend::emit_constants(std::ostream& out) {
    out << "/* Generated by burgc -- do not edit */\n";
    out << "#ifndef BURG_GENERATED_H\n";
    out << "#define BURG_GENERATED_H\n\n";
    out << "#include <stdint.h>\n";
    out << "#include <stddef.h>\n";
    out << "#include <limits.h>\n";
    out << "#include <stdbool.h>\n\n";
    out << "#include \"bbq_arena.h\"\n";
    out << "#include \"bbq_htree.h\"\n";
    out << "#include \"bbq_vec.h\"\n\n";
    out << "#ifdef __GNUC__\n";
    out << "#define BURG_UNUSED __attribute__((unused))\n";
    out << "#else\n";
    out << "#define BURG_UNUSED\n";
    out << "#endif\n\n";

    out << "/* ── Terminal symbols ── */\n\n";
    for (auto* t : a_->spec->terminals)
        out << "#define BURG_" << t->name << " " << t->number << "\n";

    out << "\n/* ── Nonterminal indices ── */\n\n";
    for (size_t i = 0; i < a_->nonterms.size(); i++)
        out << "#define " << a_->nonterms[i] << "_NT " << (i + 1) << "\n";
    out << "#define BURG_MAX_NT " << a_->nonterms.size() << "\n";
    out << "#define BURG_MAX_COST SHRT_MAX\n\n";
}

// ── C-specific label body (uint32_t cache key) ──────────

void CBurgBackend::emit_label_body(std::ostream& out, int indent) {
    pad(out, indent); out << "uint32_t id = (uint32_t)(uintptr_t)BURG_NODE_ID(node);\n";
    pad(out, indent); out << "burg_state_t* cached = burg_cache_lookup(id, ctx);\n";
    pad(out, indent); out << "if (cached) return cached;\n\n";

    emit_label_alloc(out, indent);
    out << "\n";

    pad(out, indent); out << "/* Cache BEFORE DP (back-edge cut-point safety) */\n";
    pad(out, indent); out << "burg_cache_store(id, p, ctx);\n\n";

    pad(out, indent); out << "for (int i = 0; i < arity; i++)\n";
    pad(out, indent + 1); out << "p->children[i] = burg_label(BURG_NODE_CHILD(node, i), ctx);\n\n";

    pad(out, indent); out << "burg_dp(p, node, ctx);\n";
    pad(out, indent); out << "return p;\n";
}

// ── C-specific RPO DFS (uses bbq_vec + bbq_htree) ───────

void CBurgBackend::emit_rpo_dfs(std::ostream& out, int indent) {
    pad(out, indent); out << "BURG_NODE_TYPE* rpo = NULL;\n";
    pad(out, indent); out << "{\n";
    pad(out, indent + 1); out << "bbq_htree* visited = bbq_htree_create();\n";
    pad(out, indent + 1); out << "typedef struct { BURG_NODE_TYPE node; int succ; } Frame;\n";
    pad(out, indent + 1); out << "Frame* stack = NULL;\n";
    pad(out, indent + 1); out << "Frame f0;\n";
    pad(out, indent + 1); out << "f0.node = root; f0.succ = 0;\n";
    pad(out, indent + 1); out << "bbq_vec_push(stack, f0);\n";
    pad(out, indent + 1); out << "bbq_htree_insert(visited, (uint32_t)(uintptr_t)BURG_NODE_ID(root), (void*)(uintptr_t)1);\n\n";

    pad(out, indent + 1); out << "while (bbq_vec_len(stack) > 0) {\n";
    pad(out, indent + 2); out << "Frame* f = &stack[bbq_vec_len(stack) - 1];\n";
    pad(out, indent + 2); out << "int sc = BURG_NODE_SUCC_COUNT(f->node);\n";
    pad(out, indent + 2); out << "if (f->succ < sc) {\n";
    pad(out, indent + 3); out << "BURG_NODE_TYPE s = BURG_NODE_SUCC(f->node, f->succ);\n";
    pad(out, indent + 3); out << "f->succ++;\n";
    pad(out, indent + 3); out << "if (s && !bbq_htree_contains(visited, (uint32_t)(uintptr_t)BURG_NODE_ID(s))) {\n";
    pad(out, indent + 4); out << "bbq_htree_insert(visited, (uint32_t)(uintptr_t)BURG_NODE_ID(s), (void*)(uintptr_t)1);\n";
    pad(out, indent + 4); out << "Frame fn;\n";
    pad(out, indent + 4); out << "fn.node = s; fn.succ = 0;\n";
    pad(out, indent + 4); out << "bbq_vec_push(stack, fn);\n";
    pad(out, indent + 3); out << "}\n";
    pad(out, indent + 2); out << "} else {\n";
    pad(out, indent + 3); out << "bbq_vec_push(rpo, f->node);\n";
    pad(out, indent + 3); out << "bbq__vec_hdr(stack)->len--;\n";
    pad(out, indent + 2); out << "}\n";
    pad(out, indent + 1); out << "}\n";
    pad(out, indent + 1); out << "bbq_vec_reverse(rpo);\n";
    pad(out, indent + 1); out << "bbq_vec_free(stack);\n";
    pad(out, indent + 1); out << "bbq_htree_destroy(visited);\n";
    pad(out, indent); out << "}\n\n";
}

// ── C-specific rewrite body (C loops instead of range-for) ──

void CBurgBackend::emit_rewrite_body(std::ostream& out, int indent) {
    int64_t start_idx = a_->nonterm_index.at(a_->start_nonterm);

    pad(out, indent); out << "arena_reset(ctx);\n\n";

    // Fast path: tree (no successor edges -> skip RPO + cache)
    pad(out, indent); out << "if (BURG_NODE_SUCC_COUNT(root) == 0) {\n";
    pad(out, indent + 1); out << "burg_state_t* state = burg_label_tree(root, ctx);\n";
    if (a_->has_actions) {
        pad(out, indent + 1); out << "if (state->rule[" << start_idx << "])\n";
        pad(out, indent + 2); out << "burg_reduce(root, state, " << start_idx << ", ctx);\n";
    }
    pad(out, indent + 1); out << "return;\n";
    pad(out, indent); out << "}\n\n";

    // Slow path: graph (RPO + cache)
    pad(out, indent); out << "burg_cache_clear(ctx);\n\n";

    pad(out, indent); out << "/* Compute reverse postorder via iterative DFS */\n";
    emit_rpo_dfs(out, indent);

    pad(out, indent); out << "/* Label every node (data-flow children labeled recursively) */\n";
    pad(out, indent); out << "{\n";
    pad(out, indent + 1); out << "int _i, _n = bbq_vec_len(rpo);\n";
    pad(out, indent + 1); out << "for (_i = 0; _i < _n; _i++)\n";
    pad(out, indent + 2); out << "burg_label(rpo[_i], ctx);\n";
    pad(out, indent); out << "}\n\n";

    if (a_->has_actions) {
        pad(out, indent); out << "/* Reduce every node with the start nonterminal */\n";
        pad(out, indent); out << "{\n";
        pad(out, indent + 1); out << "int _i, _n = bbq_vec_len(rpo);\n";
        pad(out, indent + 1); out << "for (_i = 0; _i < _n; _i++) {\n";
        pad(out, indent + 2); out << "burg_state_t* s = burg_cache_lookup((uint32_t)(uintptr_t)BURG_NODE_ID(rpo[_i]), ctx);\n";
        pad(out, indent + 2); out << "if (s && s->rule[" << start_idx << "])\n";
        pad(out, indent + 3); out << "burg_reduce(rpo[_i], s, " << start_idx << ", ctx);\n";
        pad(out, indent + 1); out << "}\n";
        pad(out, indent); out << "}\n";
    }

    pad(out, indent); out << "bbq_vec_free(rpo);\n";
}

} // namespace

std::unique_ptr<BurgBackend> create_c_backend() {
    return std::make_unique<CBurgBackend>();
}
