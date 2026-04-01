#include "generator.h"

namespace {

class CBurgBackend : public BurgBackend {
    std::string ctx_arg() const override { return ", ctx"; }
    std::string ctx_solo() const override { return "ctx"; }

public:
    void generate(std::ostream& out, const BurgAnalysis& a) override {
        a_ = &a;

        emit_constants(out);
        emit_user_headers(out);
        emit_macros_check(out);

        // ── BurgState (top-level struct) ──
        out << "// ── State record ──\n\n";
        emit_state_struct(out, 0);

        // ── Context struct from MEMBERS ──
        out << "// ── Context ──\n\n";
        out << "struct BurgContext {\n";
        out << "    std::unordered_map<void*, BurgState*> burg_state_cache;\n";
        if (!a.spec->members.empty()) {
            std::string trimmed = trim(a.spec->members);
            if (!trimmed.empty())
                out << "    " << trimmed << "\n";
        }
        out << "};\n\n";

        // ── Cache functions ──
        out << "// ── State cache ──\n\n";
        out << "static BurgState* burg_cache_lookup(void* id, BurgContext* ctx) {\n";
        out << "    auto it = ctx->burg_state_cache.find(id);\n";
        out << "    return (it != ctx->burg_state_cache.end()) ? it->second : nullptr;\n";
        out << "}\n\n";
        out << "static void burg_cache_store(void* id, BurgState* state, BurgContext* ctx) {\n";
        out << "    ctx->burg_state_cache[id] = state;\n";
        out << "}\n\n";
        out << "static void burg_cache_clear(BurgContext* ctx) {\n";
        out << "    ctx->burg_state_cache.clear();\n";
        out << "}\n\n";

        // Closures (static, with forward declarations)
        emit_closures(out, 0, "static ", true);

        // Label (static + ctx)
        out << "// ── Label function (bottom-up, cached) ──\n\n";
        out << "static BurgState* burg_label(BURG_NODE_TYPE node, BurgContext* ctx) {\n";
        emit_label_body(out, 1);
        out << "}\n\n";

        // Rule lookup
        out << "// ── Rule lookup ──\n\n";
        out << "static int burg_rule(BurgState* state, int goalnt) {\n";
        emit_rule_func_body(out, 1);
        out << "}\n\n";

        // NT name
        out << "// ── Nonterminal names (for debugging) ──\n\n";
        out << "static const char* burg_nt_name(int nt) {\n";
        emit_nt_name_body(out, 1);
        out << "}\n";

        // Reducer (static + ctx, only if actions)
        if (a.has_actions) {
            out << "\n// ── Reducer (top-down pass) ──\n\n";
            out << "static void burg_reduce(BURG_NODE_TYPE node, BurgState* state, int goalnt, BurgContext* ctx) {\n";
            emit_reduce_body(out, 1);
            out << "}\n";
        }

        // Rewriter (static + ctx)
        out << "\n// ── Graph rewriter (RPO walk over successor edges) ──\n\n";
        out << "static void burg_rewrite(BURG_NODE_TYPE root, BurgContext* ctx) {\n";
        emit_rewrite_body(out, 1);
        out << "}\n";
    }
};

} // namespace

std::unique_ptr<BurgBackend> create_c_backend() {
    return std::make_unique<CBurgBackend>();
}
