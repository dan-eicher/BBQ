#include "generator.h"
#include "FrameProcessor.h"

namespace {

class CppBurgBackend : public BurgBackend {
    // C++ uses exceptions instead of polling: burg_set_error() throws
    // BurgError, so the codegen's `if (burg_has_error()) return;` and
    // `burg_clear_error()` would be dead code.
    bool needs_error_polling() const override { return false; }

public:
    void generate(std::ostream& out, const BurgAnalysis& a,
                  const std::string& frame_dir = "") override {
        a_ = &a;

        bbqgen::FrameProcessor fp(frame_dir + "/CppBurg.frame", out);
        if (!fp.ok()) {
            fprintf(stderr, "burgc: cannot open frame file %s/CppBurg.frame\n",
                    frame_dir.c_str());
            return;
        }

        fp.CopyFramePart("user_headers");
        emit_user_headers(out);

        fp.CopyFramePart("constants");
        emit_defines(out);

        fp.CopyFramePart("macros_check");
        emit_macros_check(out);

        fp.CopyFramePart("namespace_open");
        if (!a.spec->ns.empty())
            out << "namespace " << a.spec->ns << " {\n\n";

        fp.CopyFramePart("members");
        if (!a.spec->members.empty()) {
            std::string trimmed = trim(a.spec->members);
            if (!trimmed.empty())
                out << "    " << trimmed << "\n\n";
        }

        fp.CopyFramePart("state_struct");
        emit_state_struct(out, 1);

        fp.CopyFramePart("private_helpers");
        if (!a.spec->private_helpers.empty()) {
            std::string trimmed = trim(a.spec->private_helpers);
            if (!trimmed.empty())
                out << "    " << trimmed << "\n\n";
        }

        fp.CopyFramePart("closures");
        emit_closures(out, 1, "", false);

        fp.CopyFramePart("dp");
        out << "    void burg_dp(" << state_type() << "* p, BURG_NODE_TYPE node) {\n";
        emit_dp_body(out, 2);
        out << "    }\n";

        fp.CopyFramePart("label_tree");
        out << "    " << state_type() << "* burg_label_tree(BURG_NODE_TYPE node) {\n";
        emit_label_tree_body(out, 2);
        out << "    }\n";

        fp.CopyFramePart("label_graph");
        out << "    " << state_type() << "* burg_label(BURG_NODE_TYPE node) {\n";
        emit_label_body(out, 2);
        out << "    }\n";

        fp.CopyFramePart("rule_lookup");
        out << "    static int burg_rule(" << state_type() << "* state, int goalnt) {\n";
        emit_rule_func_body(out, 2);
        out << "    }\n\n";
        out << "    static int burg_cost(" << state_type() << "* state, int goalnt) {\n";
        emit_cost_func_body(out, 2);
        out << "    }\n";

        fp.CopyFramePart("nt_name");
        out << "    static const char* burg_nt_name(int nt) {\n";
        emit_nt_name_body(out, 2);
        out << "    }\n";

        fp.CopyFramePart("reducer");
        if (a.has_actions) {
            out << "    void burg_reduce(BURG_NODE_TYPE node, " << state_type() << "* state, int goalnt) {\n";
            emit_reduce_body(out, 2);
            out << "    }\n";
        }

        fp.CopyFramePart("rewriter");
        out << "    // Goal-directed entry: label a tree, then read its rule/cost vectors and\n";
        out << "    // drive the reducer toward a goal of your own choosing. The state lives in\n";
        out << "    // the matcher arena — valid until the next labeling or rewrite call.\n";
        out << "    " << state_type() << "* burg_label_root(BURG_NODE_TYPE root) {\n";
        emit_label_root_body(out, 2);
        out << "    }\n\n";
        out << "    void burg_rewrite(BURG_NODE_TYPE root) {\n";
        emit_rewrite_body(out, 2);
        out << "    }\n";

        if (emit_rule_table_) {
            out << "\n";
            emit_rule_table_types(out, 1);
            out << "\n";
            emit_rule_table_data(out, 1, "inline static ");
            out << "    /* Evaluate rule `rule`'s where-clause at `node`; an unguarded rule\n";
            out << "       yields 1, so a caller never has to special-case one. */\n";
            out << "    static int burg_rule_guard(int rule, BURG_NODE_TYPE node) {\n";
            emit_rule_guard_body(out, 2);
            out << "    }\n";
        }

        fp.CopyFramePart("namespace_close");
        if (!a.spec->ns.empty())
            out << "\n} // namespace " << a.spec->ns << "\n";
    }
};

} // namespace

std::unique_ptr<BurgBackend> create_cpp_backend() {
    return std::make_unique<CppBurgBackend>();
}
