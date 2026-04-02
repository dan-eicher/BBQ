#include "generator.h"
#include <sstream>

namespace {

class CBurgBackend : public BurgBackend {
    std::string ctx_arg() const override { return ", ctx"; }
    std::string ctx_solo() const override { return "ctx"; }
    std::string state_type() const override { return "burg_state_t"; }

    void emit_constants(std::ostream& out) override;
    void emit_label_body(std::ostream& out, int indent) override;
    void emit_rpo_dfs(std::ostream& out, int indent) override;
    void emit_rewrite_body(std::ostream& out, int indent) override;

    std::string ns_prefix_;
    std::string ctx_type_;
    std::string guard_;
    std::string impl_include_;

    void setup(const BurgAnalysis& a) {
        a_ = &a;
        ns_prefix_ = a.spec->ns.empty() ? "" : a.spec->ns + "_";
        ctx_type_ = ns_prefix_ + "burg_ctx_t";
    }

    static std::string make_guard(const std::string& hdr_name) {
        std::string g;
        for (char c : hdr_name) {
            if (c == '.' || c == '-' || c == '/') g += '_';
            else g += (char)toupper(c);
        }
        return g;
    }

public:
    bool needs_impl_file() const override { return true; }

    // Single-file fallback
    void generate(std::ostream& out, const BurgAnalysis& a) override {
        std::ostringstream dummy;
        generate(out, dummy, a, "burg_generated.h");
    }

    // Two-file generation: header + implementation
    void generate(std::ostream& hdr, std::ostream& impl,
                  const BurgAnalysis& a, const std::string& hdr_name) override {
        setup(a);
        guard_ = make_guard(hdr_name);
        impl_include_ = hdr_name;

        // ═══ HEADER ═══

        emit_constants(hdr);
        emit_user_headers(hdr);
        emit_macros_check(hdr);

        hdr << "/* ── State record ── */\n\n";
        emit_state_struct(hdr, 0);

        hdr << "/* ── Context ── */\n\n";
        hdr << "typedef struct " << ctx_type_ << " {\n";
        hdr << "    bbq_arena arena;\n";
        hdr << "    bbq_htree* state_cache;\n";
        if (!a.spec->members.empty()) {
            std::string trimmed = trim(a.spec->members);
            if (!trimmed.empty())
                hdr << "    " << trimmed << "\n";
        }
        hdr << "} " << ctx_type_ << ";\n\n";

        // Public function declarations
        hdr << "/* ── Public API ── */\n\n";
        hdr << "void " << ns_prefix_ << "burg_ctx_init(" << ctx_type_ << "* ctx);\n";
        hdr << "void " << ns_prefix_ << "burg_ctx_free(" << ctx_type_ << "* ctx);\n";
        hdr << "void " << ns_prefix_ << "burg_rewrite(BURG_NODE_TYPE root, " << ctx_type_ << "* ctx);\n";
        hdr << "int "  << ns_prefix_ << "burg_rule(burg_state_t* state, int goalnt);\n";
        hdr << "const char* " << ns_prefix_ << "burg_nt_name(int nt);\n";
        if (a.has_actions) {
            hdr << "void " << ns_prefix_ << "burg_reduce(BURG_NODE_TYPE node, burg_state_t* state, int goalnt, " << ctx_type_ << "* ctx);\n";
        }

        hdr << "\n#endif /* " << guard_ << " */\n";

        // ═══ IMPLEMENTATION ═══

        impl << "#include \"" << impl_include_ << "\"\n\n";

        // Context lifecycle
        impl << "void " << ns_prefix_ << "burg_ctx_init(" << ctx_type_ << "* ctx) {\n";
        impl << "    bbq_arena_init(&ctx->arena, 4096);\n";
        impl << "    ctx->state_cache = bbq_htree_create();\n";
        impl << "}\n\n";

        impl << "void " << ns_prefix_ << "burg_ctx_free(" << ctx_type_ << "* ctx) {\n";
        impl << "    bbq_arena_free(&ctx->arena);\n";
        impl << "    bbq_htree_destroy(ctx->state_cache);\n";
        impl << "    ctx->state_cache = NULL;\n";
        impl << "}\n\n";

        // Internal helpers (static, may be unused if tree-only)
        impl << "static BURG_UNUSED void* arena_alloc(size_t size, " << ctx_type_ << "* ctx) {\n";
        impl << "    return bbq_arena_alloc(&ctx->arena, size);\n";
        impl << "}\n\n";
        impl << "static BURG_UNUSED void arena_reset(" << ctx_type_ << "* ctx) {\n";
        impl << "    bbq_arena_reset(&ctx->arena);\n";
        impl << "}\n\n";

        impl << "static BURG_UNUSED burg_state_t* burg_cache_lookup(uint32_t id, " << ctx_type_ << "* ctx) {\n";
        impl << "    return (burg_state_t*)bbq_htree_search(ctx->state_cache, id);\n";
        impl << "}\n\n";
        impl << "static BURG_UNUSED void burg_cache_store(uint32_t id, burg_state_t* state, " << ctx_type_ << "* ctx) {\n";
        impl << "    bbq_htree_insert(ctx->state_cache, id, state);\n";
        impl << "}\n\n";
        impl << "static BURG_UNUSED void burg_cache_clear(" << ctx_type_ << "* ctx) {\n";
        impl << "    bbq_htree_clear(ctx->state_cache);\n";
        impl << "}\n\n";

        // Closures
        emit_closures(impl, 0, "static ", true);

        // DP
        impl << "static void burg_dp(burg_state_t* p, BURG_NODE_TYPE node, " << ctx_type_ << "* ctx) {\n";
        emit_dp_body(impl, 1);
        impl << "}\n\n";

        // Label tree
        impl << "static burg_state_t* burg_label_tree(BURG_NODE_TYPE node, " << ctx_type_ << "* ctx) {\n";
        emit_label_tree_body(impl, 1);
        impl << "}\n\n";

        // Label graph
        impl << "static burg_state_t* burg_label(BURG_NODE_TYPE node, " << ctx_type_ << "* ctx) {\n";
        emit_label_body(impl, 1);
        impl << "}\n\n";

        // Rule lookup (public)
        impl << "int " << ns_prefix_ << "burg_rule(burg_state_t* state, int goalnt) {\n";
        emit_rule_func_body(impl, 1);
        impl << "}\n\n";

        // NT name (public)
        impl << "const char* " << ns_prefix_ << "burg_nt_name(int nt) {\n";
        emit_nt_name_body(impl, 1);
        impl << "}\n\n";

        // Reducer (public, only if actions)
        if (a.has_actions) {
            impl << "void " << ns_prefix_ << "burg_reduce(BURG_NODE_TYPE node, burg_state_t* state, int goalnt, " << ctx_type_ << "* ctx) {\n";
            emit_reduce_body(impl, 1);
            impl << "}\n\n";
        }

        // Rewriter (public)
        impl << "void " << ns_prefix_ << "burg_rewrite(BURG_NODE_TYPE root, " << ctx_type_ << "* ctx) {\n";
        emit_rewrite_body(impl, 1);
        impl << "}\n";
    }

};

// ── C-specific preamble (C11 includes + #define constants) ──

void CBurgBackend::emit_constants(std::ostream& out) {
    out << "/* Generated by burgc -- do not edit */\n";
    out << "#ifndef " << guard_ << "\n";
    out << "#define " << guard_ << "\n\n";
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
    out << "/* BURG_NODE_TYPE et al. must be defined before this point.\n";
    out << "   Define BURG_CONFIG_HEADER to auto-include your definitions, e.g.:\n";
    out << "     #define BURG_CONFIG_HEADER \"my_node_defs.h\"  */\n";
    out << "#ifdef BURG_CONFIG_HEADER\n";
    out << "#include BURG_CONFIG_HEADER\n";
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

    pad(out, indent); out << "if (BURG_NODE_SUCC_COUNT(root) == 0) {\n";
    pad(out, indent + 1); out << "burg_state_t* state = burg_label_tree(root, ctx);\n";
    if (a_->has_actions) {
        pad(out, indent + 1); out << "if (state->rule[" << start_idx << "])\n";
        pad(out, indent + 2); out << ns_prefix_ << "burg_reduce(root, state, " << start_idx << ", ctx);\n";
    }
    pad(out, indent + 1); out << "return;\n";
    pad(out, indent); out << "}\n\n";

    pad(out, indent); out << "burg_cache_clear(ctx);\n\n";
    pad(out, indent); out << "/* Compute reverse postorder via iterative DFS */\n";
    emit_rpo_dfs(out, indent);

    pad(out, indent); out << "/* Label every node */\n";
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
        pad(out, indent + 3); out << ns_prefix_ << "burg_reduce(rpo[_i], s, " << start_idx << ", ctx);\n";
        pad(out, indent + 1); out << "}\n";
        pad(out, indent); out << "}\n";
    }

    pad(out, indent); out << "bbq_vec_free(rpo);\n";
}

} // namespace

std::unique_ptr<BurgBackend> create_c_backend() {
    return std::make_unique<CBurgBackend>();
}
