#include "generator.h"

namespace {

class CppBurgBackend : public BurgBackend {
public:
    void generate(std::ostream& out, const BurgAnalysis& a) override {
        a_ = &a;

        emit_constants(out);
        emit_user_headers(out);
        emit_macros_check(out);

        // ── Namespace open ──
        if (!a.spec->ns.empty())
            out << "namespace " << a.spec->ns << " {\n\n";

        // ── Class open ──
        out << "class BurgMatcher {\n";
        out << "public:\n";
        if (!a.spec->members.empty()) {
            std::string trimmed = trim(a.spec->members);
            if (!trimmed.empty())
                out << "    " << trimmed << "\n\n";
        }

        // ── State record (nested struct) ──
        out << "    // ── State record ──\n\n";
        emit_state_struct(out, 1);

        // ── Private section ──
        out << "private:\n";

        // Arena allocator (page-based, stable pointers)
        out << "    // ── Arena allocator ──\n\n";
        out << "    static constexpr size_t ARENA_PAGE_SIZE = 4096;\n";
        out << "    std::vector<char*> arena_pages_;\n";
        out << "    int arena_cur_ = -1;\n";
        out << "    size_t arena_pos_ = 0;\n\n";
        out << "    void* arena_alloc(size_t size) {\n";
        out << "        size = (size + 7) & ~7;\n";
        out << "        if (arena_cur_ < 0 || arena_pos_ + size > ARENA_PAGE_SIZE) {\n";
        out << "            if (++arena_cur_ >= (int)arena_pages_.size())\n";
        out << "                arena_pages_.push_back(new char[ARENA_PAGE_SIZE]);\n";
        out << "            arena_pos_ = 0;\n";
        out << "        }\n";
        out << "        void* p = arena_pages_[arena_cur_] + arena_pos_;\n";
        out << "        arena_pos_ += size;\n";
        out << "        return p;\n";
        out << "    }\n\n";
        out << "    void arena_reset() { arena_cur_ = -1; arena_pos_ = 0; }\n\n";
        out << "public:\n";
        out << "    ~BurgMatcher() { for (auto* p : arena_pages_) delete[] p; }\n\n";
        out << "private:\n";

        // Cache (for graph path)
        out << "    // ── State cache (graph path only) ──\n\n";
        out << "    std::unordered_map<void*, BurgState*> burg_state_cache;\n\n";
        out << "    BurgState* burg_cache_lookup(void* id) {\n";
        out << "        auto it = burg_state_cache.find(id);\n";
        out << "        return (it != burg_state_cache.end()) ? it->second : nullptr;\n";
        out << "    }\n\n";
        out << "    void burg_cache_store(void* id, BurgState* state) {\n";
        out << "        burg_state_cache[id] = state;\n";
        out << "    }\n\n";
        out << "    void burg_cache_clear() {\n";
        out << "        burg_state_cache.clear();\n";
        out << "    }\n\n";

        // Closures (methods, no forward declarations needed)
        emit_closures(out, 1, "", false);

        // DP switch (shared between tree and graph label)
        out << "    // ── Cost DP (shared by tree and graph label) ──\n\n";
        out << "    void burg_dp(BurgState* p, BURG_NODE_TYPE node) {\n";
        emit_dp_body(out, 2);
        out << "    }\n\n";

        // Label tree (no cache, fast path)
        out << "    // ── Label (tree path — no cache) ──\n\n";
        out << "    BurgState* burg_label_tree(BURG_NODE_TYPE node) {\n";
        emit_label_tree_body(out, 2);
        out << "    }\n\n";

        // Label graph (with cache)
        out << "    // ── Label (graph path — cached) ──\n\n";
        out << "    BurgState* burg_label(BURG_NODE_TYPE node) {\n";
        emit_label_body(out, 2);
        out << "    }\n\n";

        // Rule lookup (static method)
        out << "    // ── Rule lookup ──\n\n";
        out << "    static int burg_rule(BurgState* state, int goalnt) {\n";
        emit_rule_func_body(out, 2);
        out << "    }\n\n";

        // NT name (static method)
        out << "    // ── Nonterminal names (for debugging) ──\n\n";
        out << "    static const char* burg_nt_name(int nt) {\n";
        emit_nt_name_body(out, 2);
        out << "    }\n";

        // Reducer (method, only if actions exist)
        if (a.has_actions) {
            out << "\n    // ── Reducer (top-down pass) ──\n\n";
            out << "    void burg_reduce(BURG_NODE_TYPE node, BurgState* state, int goalnt) {\n";
            emit_reduce_body(out, 2);
            out << "    }\n";
        }

        // Rewriter (public method)
        out << "\npublic:\n";
        out << "    // ── Rewriter ──\n\n";
        out << "    void burg_rewrite(BURG_NODE_TYPE root) {\n";
        emit_rewrite_body(out, 2);
        out << "    }\n";

        // ── Class close ──
        out << "}; // class BurgMatcher\n";

        // ── Namespace close ──
        if (!a.spec->ns.empty())
            out << "\n} // namespace " << a.spec->ns << "\n";
    }
};

} // namespace

std::unique_ptr<BurgBackend> create_cpp_backend() {
    return std::make_unique<CppBurgBackend>();
}
