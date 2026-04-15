#include "generator.h"
#include "FrameProcessor.h"
#include <sstream>

namespace {

class CBurgBackend : public BurgBackend {
    std::string ctx_arg() const override { return ", ctx"; }
    std::string ctx_solo() const override { return "ctx"; }
    std::string state_type() const override { return "burg_state_t"; }

    void emit_label_body(std::ostream& out, int indent) override;
    void emit_rpo_dfs(std::ostream& out, int indent) override;
    void emit_rewrite_body(std::ostream& out, int indent) override;

    std::string ns_prefix_;
    std::string ctx_type_;

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

    // Emit terminal/nonterminal #defines (no includes — those are in the frame)
    void emit_c_constants(std::ostream& out) {
        out << "/* ── Terminal symbols ── */\n\n";
        for (auto* t : a_->spec->terminals) {
            out << "#define BURG_" << t->name << " ";
            if (!t->symbol.empty()) out << t->symbol;
            else                    out << t->number;
            out << "\n";
        }

        out << "\n/* ── Nonterminal indices ── */\n\n";
        for (size_t i = 0; i < a_->nonterms.size(); i++)
            out << "#define " << a_->nonterms[i] << "_NT " << (i + 1) << "\n";
        out << "#define BURG_MAX_NT " << a_->nonterms.size() << "\n";
        out << "#define BURG_MAX_COST SHRT_MAX\n";
    }

    void emit_context_struct(std::ostream& out) {
        out << "typedef struct " << ctx_type_ << " {\n";
        out << "    bbq_arena arena;\n";
        out << "    bbq_htree* state_cache;\n";
        if (!a_->spec->members.empty()) {
            std::string trimmed = trim(a_->spec->members);
            if (!trimmed.empty())
                out << "    " << trimmed << "\n";
        }
        out << "} " << ctx_type_ << ";\n";
    }

    void emit_public_api(std::ostream& out) {
        out << "void " << ns_prefix_ << "burg_ctx_init(" << ctx_type_ << "* ctx);\n";
        out << "void " << ns_prefix_ << "burg_ctx_free(" << ctx_type_ << "* ctx);\n";
        out << "void " << ns_prefix_ << "burg_rewrite(BURG_NODE_TYPE root, " << ctx_type_ << "* ctx);\n";
        out << "int "  << ns_prefix_ << "burg_rule(burg_state_t* state, int goalnt);\n";
        out << "const char* " << ns_prefix_ << "burg_nt_name(int nt);\n";
        if (a_->has_actions) {
            out << "void " << ns_prefix_ << "burg_reduce(BURG_NODE_TYPE node, burg_state_t* state, int goalnt, " << ctx_type_ << "* ctx);\n";
        }
    }

    void emit_ctx_lifecycle(std::ostream& out) {
        out << "void " << ns_prefix_ << "burg_ctx_init(" << ctx_type_ << "* ctx) {\n";
        out << "    bbq_arena_init(&ctx->arena, 4096);\n";
        out << "    ctx->state_cache = bbq_htree_create();\n";
        out << "}\n\n";

        out << "void " << ns_prefix_ << "burg_ctx_free(" << ctx_type_ << "* ctx) {\n";
        out << "    bbq_arena_free(&ctx->arena);\n";
        out << "    bbq_htree_destroy(ctx->state_cache);\n";
        out << "    ctx->state_cache = NULL;\n";
        out << "}\n";
    }

    void emit_internal_helpers(std::ostream& out) {
        out << "static BURG_UNUSED void* arena_alloc(size_t size, " << ctx_type_ << "* ctx) {\n";
        out << "    return bbq_arena_alloc(&ctx->arena, size);\n";
        out << "}\n\n";
        out << "static BURG_UNUSED void arena_reset(" << ctx_type_ << "* ctx) {\n";
        out << "    bbq_arena_reset(&ctx->arena);\n";
        out << "}\n\n";

        out << "static BURG_UNUSED burg_state_t* burg_cache_lookup(uint32_t id, " << ctx_type_ << "* ctx) {\n";
        out << "    return (burg_state_t*)bbq_htree_search(ctx->state_cache, id);\n";
        out << "}\n\n";
        out << "static BURG_UNUSED void burg_cache_store(uint32_t id, burg_state_t* state, " << ctx_type_ << "* ctx) {\n";
        out << "    bbq_htree_insert(ctx->state_cache, id, state);\n";
        out << "}\n\n";
        out << "static BURG_UNUSED void burg_cache_clear(" << ctx_type_ << "* ctx) {\n";
        out << "    bbq_htree_clear(ctx->state_cache);\n";
        out << "}\n";
    }

    void emit_functions(std::ostream& out) {
        // DP
        out << "static void burg_dp(burg_state_t* p, BURG_NODE_TYPE node, " << ctx_type_ << "* ctx) {\n";
        emit_dp_body(out, 1);
        out << "}\n\n";

        // Label tree
        out << "static burg_state_t* burg_label_tree(BURG_NODE_TYPE node, " << ctx_type_ << "* ctx) {\n";
        emit_label_tree_body(out, 1);
        out << "}\n\n";

        // Label graph
        out << "static burg_state_t* burg_label(BURG_NODE_TYPE node, " << ctx_type_ << "* ctx) {\n";
        emit_label_body(out, 1);
        out << "}\n\n";

        // Rule lookup (public)
        out << "int " << ns_prefix_ << "burg_rule(burg_state_t* state, int goalnt) {\n";
        emit_rule_func_body(out, 1);
        out << "}\n\n";

        // NT name (public)
        out << "const char* " << ns_prefix_ << "burg_nt_name(int nt) {\n";
        emit_nt_name_body(out, 1);
        out << "}\n\n";

        // Reducer (public, only if actions)
        if (a_->has_actions) {
            out << "void " << ns_prefix_ << "burg_reduce(BURG_NODE_TYPE node, burg_state_t* state, int goalnt, " << ctx_type_ << "* ctx) {\n";
            emit_reduce_body(out, 1);
            out << "}\n\n";
        }

        // Rewriter (public)
        out << "void " << ns_prefix_ << "burg_rewrite(BURG_NODE_TYPE root, " << ctx_type_ << "* ctx) {\n";
        emit_rewrite_body(out, 1);
        out << "}\n";
    }

public:
    bool needs_impl_file() const override { return true; }

    // Single-file not supported for C — requires -o for .h/.c split
    void generate(std::ostream& out, const BurgAnalysis& a,
                  const std::string& frame_dir) override {
        // Delegate to two-file with a dummy impl stream
        std::ostringstream dummy;
        generate(out, dummy, a, "burg_generated.h", frame_dir);
        out << "\n";
        out << dummy.str();
    }

    // Two-file generation with frame templates
    void generate(std::ostream& hdr, std::ostream& impl,
                  const BurgAnalysis& a, const std::string& hdr_name,
                  const std::string& frame_dir) override {
        setup(a);
        std::string guard = make_guard(hdr_name);

        // ═══ HEADER (via frame) ═══

        bbqgen::FrameProcessor hfp(frame_dir + "/CBurg.frame", hdr);
        if (!hfp.ok()) {
            // Fallback: emit without frame — inline everything into header
            hdr << "#ifndef " << guard << "\n#define " << guard << "\n\n";
            hdr << "#include <limits.h>\n#include <stdint.h>\n#include <stdbool.h>\n";
            hdr << "#include \"bbq_arena.h\"\n#include \"bbq_htree.h\"\n\n";
            emit_c_constants(hdr);
            hdr << "\n";
            emit_context_struct(hdr);
            hdr << "\n";
            emit_public_api(hdr);
            hdr << "\n#endif /* " << guard << " */\n";
            return;
        }

        hfp.CopyFramePart("guard_open");
        hdr << "#ifndef " << guard << "\n";
        hdr << "#define " << guard << "\n";

        hfp.CopyFramePart("constants");
        emit_c_constants(hdr);

        hfp.CopyFramePart("user_headers");
        emit_user_headers(hdr);

        hfp.CopyFramePart("macros_check");
        emit_macros_check(hdr);

        hfp.CopyFramePart("state_struct");
        emit_state_struct(hdr, 0);

        hfp.CopyFramePart("context_struct");
        emit_context_struct(hdr);

        hfp.CopyFramePart("public_api");
        emit_public_api(hdr);

        hfp.CopyFramePart("guard_close");
        hdr << "#endif /* " << guard << " */\n";

        // ═══ IMPLEMENTATION (via frame) ═══

        bbqgen::FrameProcessor ifp(frame_dir + "/CBurgImpl.frame", impl);
        if (!ifp.ok()) return;

        ifp.CopyFramePart("impl_include");
        impl << "#include \"" << hdr_name << "\"\n";

        ifp.CopyFramePart("ctx_lifecycle");
        emit_ctx_lifecycle(impl);

        ifp.CopyFramePart("internal_helpers");
        emit_internal_helpers(impl);

        ifp.CopyFramePart("closures");
        emit_closures(impl, 0, "static ", true);

        ifp.CopyFramePart("functions");
        emit_functions(impl);
    }
};

// ── C-specific overrides ─────────────────────────────────

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
