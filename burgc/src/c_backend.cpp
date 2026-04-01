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

        // ── Namespace open ──
        if (!a.spec->ns.empty())
            out << "namespace " << a.spec->ns << " {\n\n";

        // ── BurgState (top-level struct) ──
        out << "// ── State record ──\n\n";
        emit_state_struct(out, 0);

        // ── Context struct from MEMBERS ──
        out << "// ── Context ──\n\n";
        out << "static constexpr size_t ARENA_PAGE_SIZE = 4096;\n\n";
        out << "struct BurgContext {\n";
        out << "    std::vector<char*> arena_pages;\n";
        out << "    int arena_cur = -1;\n";
        out << "    size_t arena_pos = 0;\n";
        out << "    std::unordered_map<void*, BurgState*> burg_state_cache;\n";
        if (!a.spec->members.empty()) {
            std::string trimmed = trim(a.spec->members);
            if (!trimmed.empty())
                out << "    " << trimmed << "\n";
        }
        out << "    ~BurgContext() { for (auto* p : arena_pages) delete[] p; }\n";
        out << "};\n\n";

        // ── Arena functions ──
        out << "// ── Arena allocator (page-based, stable pointers) ──\n\n";
        out << "static void* arena_alloc(size_t size, BurgContext* ctx) {\n";
        out << "    size = (size + 7) & ~7;\n";
        out << "    if (ctx->arena_cur < 0 || ctx->arena_pos + size > ARENA_PAGE_SIZE) {\n";
        out << "        if (++ctx->arena_cur >= (int)ctx->arena_pages.size())\n";
        out << "            ctx->arena_pages.push_back(new char[ARENA_PAGE_SIZE]);\n";
        out << "        ctx->arena_pos = 0;\n";
        out << "    }\n";
        out << "    void* p = ctx->arena_pages[ctx->arena_cur] + ctx->arena_pos;\n";
        out << "    ctx->arena_pos += size;\n";
        out << "    return p;\n";
        out << "}\n\n";
        out << "static void arena_reset(BurgContext* ctx) { ctx->arena_cur = -1; ctx->arena_pos = 0; }\n\n";

        // ── Cache functions ──
        out << "// ── State cache (graph path only) ──\n\n";
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

        // DP (static + ctx)
        out << "// ── Cost DP (shared by tree and graph label) ──\n\n";
        out << "static void burg_dp(BurgState* p, BURG_NODE_TYPE node, BurgContext* ctx) {\n";
        emit_dp_body(out, 1);
        out << "}\n\n";

        // Label tree (static + ctx, no cache)
        out << "// ── Label (tree path — no cache) ──\n\n";
        out << "static BurgState* burg_label_tree(BURG_NODE_TYPE node, BurgContext* ctx) {\n";
        emit_label_tree_body(out, 1);
        out << "}\n\n";

        // Label graph (static + ctx, with cache)
        out << "// ── Label (graph path — cached) ──\n\n";
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
        out << "\n// ── Rewriter ──\n\n";
        out << "static void burg_rewrite(BURG_NODE_TYPE root, BurgContext* ctx) {\n";
        emit_rewrite_body(out, 1);
        out << "}\n";

        // ── Namespace close ──
        if (!a.spec->ns.empty())
            out << "\n} // namespace " << a.spec->ns << "\n";
    }
};

} // namespace

std::unique_ptr<BurgBackend> create_c_backend() {
    return std::make_unique<CBurgBackend>();
}
