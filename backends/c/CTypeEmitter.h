#pragma once

#include "BBQ_AST.h"
#include "Sema.h"
#include <string>
#include <sstream>
#include <unordered_map>

namespace bbqgen_c {

using bbqgen::Sema;

// CamelCase → snake_case conversion
std::string to_snake_case(const std::string& name);

class CTypeEmitter {
public:
    CTypeEmitter(Sema& sema, const std::string& prefix = "")
        : sema_(sema), prefix_(prefix.empty() ? "" : to_snake_case(prefix)) {}

    // Get the raw prefix for guard generation
    const std::string& prefix() const { return prefix_; }

    // Emit all type definitions to a string
    std::string emit(BBQ::Grammar* grammar);

    // Emit using FrameProcessor to an output stream
    bool emit_to_frame(BBQ::Grammar* grammar,
                       const std::string& frame_path,
                       std::ostream& out);

    // Type name mapping (used by CReaderEmitter and CWriterEmitter too)
    std::string c_type(BBQ::TypeExpr* type);
    std::string primitive_c_type(BBQ::PrimitiveKind* kind);
    std::string bitfield_entry_type(int64_t width);

    // Rule name → C type name (snake_case_t)
    std::string type_name(const std::string& rule_name);

    // Rule name → read/write function names
    std::string read_func(const std::string& rule_name);
    std::string write_func(const std::string& rule_name);

private:
    void emit_forward_decls(std::ostream& out);
    void emit_type_defs(std::ostream& out);

    void emit_rule_type(BBQ::Rule* rule, std::ostream& out);
    void emit_struct_type(const std::string& name, BBQ::Struct* st, std::ostream& out);
    void emit_union_type(const std::string& name, BBQ::Union* u, std::ostream& out);
    void emit_switch_type(const std::string& name, BBQ::Switch* sw, std::ostream& out);
    void emit_alternatives_type(const std::string& name, BBQ::Alternatives* alts, std::ostream& out);

    // Tagged union helper: emit tag enum + union struct
    // If enum_values is non-empty, each enum constant gets an explicit = value.
    void emit_tagged_union(const std::string& name,
                           const std::vector<std::string>& variant_types,
                           const std::vector<std::string>& variant_names,
                           std::ostream& out,
                           const std::vector<std::string>& enum_values = {});

    // Inline struct support
    void register_inline_structs(const std::string& prefix, BBQ::Struct* st);
    void emit_inline_structs(const std::string& rule_name, std::ostream& out);

    // Free function generation
    void emit_free_funcs(std::ostream& out);
    void emit_free_func(BBQ::Rule* rule, std::ostream& out);
    void emit_struct_free_body(BBQ::Struct* st, const std::string& prefix,
                               std::ostream& out, int indent);
    void emit_field_free(BBQ::TypeExpr* type, const std::string& target,
                         std::ostream& out, int indent);
    bool type_needs_free(BBQ::TypeExpr* type);

    // Prepend stored prefix to a snake_cased name
    std::string prefixed_name(const std::string& name);

    Sema& sema_;
    std::string prefix_;
    std::unordered_map<BBQ::Struct*, std::string> inline_struct_names_;
    std::vector<std::pair<std::string, BBQ::Struct*>> inline_struct_order_;
};

} // namespace bbqgen_c
