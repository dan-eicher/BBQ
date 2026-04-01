#pragma once

#include "BBQ_AST.h"
#include "Sema.h"
#include "CTypeEmitter.h"
#include <string>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace bbqgen_c {

using bbqgen::Sema;

// Scope entry for nested struct code generation
struct CEmitScope {
    std::string prefix;                     // e.g. "out->", "out->header."
    std::unordered_set<std::string> fields; // field names visible in this scope
};

class CReaderEmitter {
public:
    CReaderEmitter(Sema& sema, CTypeEmitter& types)
        : sema_(sema), types_(types) {}

    // Emit all read function declarations (for header)
    std::string emit_decls(BBQ::Grammar* grammar);

    // Emit all read function implementations
    std::string emit_impls(BBQ::Grammar* grammar);

    // Frame-based emission
    bool emit_header_to_frame(BBQ::Grammar* grammar,
                              const std::string& frame_path,
                              const std::string& types_include,
                              std::ostream& out);
    bool emit_impl_to_frame(BBQ::Grammar* grammar,
                            const std::string& frame_path,
                            const std::string& reader_include,
                            std::ostream& out);

private:
    void emit_decl(BBQ::Rule* rule, std::ostream& out);
    void emit_impl(BBQ::Rule* rule, std::ostream& out);

    // Per-construct code generation
    void emit_struct_body(const std::string& rule_name, BBQ::Struct* st, std::ostream& out, int indent);
    void emit_union_body(const std::string& rule_name, BBQ::Union* u, const std::string& target, std::ostream& out, int indent);
    void emit_switch_body(const std::string& rule_name, BBQ::Switch* sw, const std::string& disc_expr, const std::string& target, std::ostream& out, int indent);
    void emit_alternatives_body(const std::string& rule_name, BBQ::Alternatives* alts, const std::string& target, std::ostream& out, int indent);
    void emit_array_body(const std::string& rule_name, BBQ::Array* arr, const std::string& target, std::ostream& out, int indent);
    void emit_optional_body(const std::string& rule_name, BBQ::Optional* opt, const std::string& target, std::ostream& out, int indent);

    // Field-level code generation
    void emit_field_read(const std::string& rule_name, BBQ::Field* field, std::ostream& out, int indent);
    void emit_read_call(const std::string& target, BBQ::TypeExpr* type, const std::string& ctx, std::ostream& out, int indent);

    // Bitfield
    void emit_bitfield_read(const std::string& ctx_name, BBQ::Bitfield* bf,
                             const std::string& target, std::ostream& out, int indent);

    // Expression compilation (C output)
    std::string compile_expr(BBQ::Expr* expr);
    std::string compile_ref_path(BBQ::RefPath* path);

    // Primitive read function name (e.g. "bbq_read_u16be")
    std::string read_func_name(BBQ::PrimitiveKind* kind);

    // Try-parse helpers
    void emit_try_parse(BBQ::TypeExpr* type, const std::string& target, std::ostream& out);
    void emit_try_parse_block(BBQ::TypeExpr* type, const std::string& target, const std::string& rule_name, std::ostream& out, int indent);

    // String switch detection
    bool has_string_cases(BBQ::Switch* sw);

    // Helpers
    std::string ind(int n);
    std::string c_type(BBQ::TypeExpr* type) { return types_.c_type(type); }

    Sema& sema_;
    CTypeEmitter& types_;

    // Scope chain for nested struct code generation
    std::vector<CEmitScope> scopes_;
    std::string current_rule_name_;
    int array_nesting_depth_ = 0;

    struct ParentContext {
        std::string type_name;
        std::unordered_set<std::string> fields;
    };
    std::vector<ParentContext> parent_contexts_;
};

} // namespace bbqgen_c
