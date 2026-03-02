#pragma once

#include "BBQ_AST.h"
#include "Sema.h"
#include "TypeEmitter.h"
#include <string>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace bbqgen {

// Scope entry for nested struct code generation
struct EmitScope {
    std::string prefix;                     // e.g. "out.", "out.header."
    std::unordered_set<std::string> fields; // field names visible in this scope
};

class ParserEmitter {
public:
    ParserEmitter(Sema& sema, TypeEmitter& types, const std::string& ns = "")
        : sema_(sema), types_(types), namespace_(ns) {}

    // Emit all parse function declarations (for header)
    std::string emit_decls(BBQ::Grammar* grammar);

    // Emit all parse function implementations
    std::string emit_impls(BBQ::Grammar* grammar);

    // Frame-based emission. Returns false on frame error.
    bool emit_header_to_frame(BBQ::Grammar* grammar,
                              const std::string& frame_path,
                              const std::string& types_include,
                              std::ostream& out);
    bool emit_impl_to_frame(BBQ::Grammar* grammar,
                            const std::string& frame_path,
                            const std::string& parser_include,
                            std::ostream& out);

private:
    void emit_decl(BBQ::Rule* rule, std::ostream& out);
    void emit_impl(BBQ::Rule* rule, std::ostream& out);

    // Code generation per construct
    void emit_struct_body(const std::string& rule_name, BBQ::Struct* st, std::ostream& out, int indent);
    void emit_union_body(const std::string& rule_name, BBQ::Union* u, const std::string& target, std::ostream& out, int indent);
    void emit_switch_body(const std::string& rule_name, BBQ::Switch* sw, const std::string& disc_expr, const std::string& target, std::ostream& out, int indent);
    void emit_alternatives_body(const std::string& rule_name, BBQ::Alternatives* alts, const std::string& target, std::ostream& out, int indent);
    void emit_array_body(const std::string& rule_name, BBQ::Array* arr, const std::string& target, std::ostream& out, int indent);
    void emit_optional_body(const std::string& rule_name, BBQ::Optional* opt, const std::string& target, std::ostream& out, int indent);

    // Field-level code generation
    void emit_field_read(const std::string& rule_name, BBQ::Field* field, std::ostream& out, int indent);
    void emit_read_call(const std::string& target, BBQ::TypeExpr* type, const std::string& ctx, std::ostream& out, int indent, const std::string& on_fail = "");

    // Bitfield code generation
    void emit_bitfield_read(const std::string& ctx_name, BBQ::Bitfield* bf,
                             const std::string& target, std::ostream& out, int indent);

    // Expression compilation
    std::string compile_expr(BBQ::Expr* expr);
    std::string compile_ref_path(BBQ::RefPath* path);

    // Primitive read method name
    std::string read_method(BBQ::PrimitiveKind* kind);

    // Backtracking try-parse helpers
    void emit_try_parse(BBQ::TypeExpr* type, const std::string& target, std::ostream& out);
    void emit_try_parse_block(BBQ::TypeExpr* type, const std::string& target, const std::string& rule_name, std::ostream& out, int indent);

    // String switch detection
    bool has_string_cases(BBQ::Switch* sw);

    // Helpers
    std::string ind(int n);
    std::string cpp_type(BBQ::TypeExpr* type) { return types_.cpp_type(type); }

    Sema& sema_;
    TypeEmitter& types_;
    std::string namespace_;

    // Scope chain for nested struct code generation.
    // Innermost scope is at the back. Initialized per-rule in emit_impl().
    std::vector<EmitScope> scopes_;

    // Cross-rule scope resolution state
    std::string current_rule_name_;          // Current rule being emitted
    int array_nesting_depth_ = 0;            // >0 means local 'i' var exists

    struct ParentContext {
        std::string rule_name;                // C++ type for static_cast
        std::unordered_set<std::string> fields;  // fields visible in this parent
    };
    std::vector<ParentContext> parent_contexts_;
};

} // namespace bbqgen
