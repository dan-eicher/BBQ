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

        // Cache (member variable + methods)
        out << "    // ── State cache ──\n\n";
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

        // Label (method)
        out << "    // ── Label function (bottom-up, cached) ──\n\n";
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
        out << "    // ── Graph rewriter (RPO walk over successor edges) ──\n\n";
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
