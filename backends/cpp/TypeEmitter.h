#pragma once

#include "BBQ_AST.h"
#include "Sema.h"
#include <string>
#include <sstream>
#include <unordered_map>

namespace bbqgen {

class TypeEmitter {
public:
    TypeEmitter(Sema& sema, const std::string& ns = "")
        : sema_(sema), namespace_(ns) {}

    // Emit all type definitions to a string
    std::string emit(BBQ::Grammar* grammar);

    // Emit using FrameProcessor to an output stream. Returns false on frame error.
    bool emit_to_frame(BBQ::Grammar* grammar,
                       const std::string& frame_path,
                       std::ostream& out);

    // Type name mapping (used by ParserEmitter too)
    std::string cpp_type(BBQ::TypeExpr* type);
    std::string primitive_cpp_type(BBQ::PrimitiveKind* kind);
    std::string bitfield_entry_type(int64_t width);

private:
    // Emit components
    void emit_forward_decls(std::ostream& out);
    void emit_type_defs(std::ostream& out);

    // Per-rule emission
    void emit_rule_type(BBQ::Rule* rule, std::ostream& out);
    void emit_struct_type(const std::string& name, BBQ::Struct* st, std::ostream& out);
    void emit_union_type(const std::string& name, BBQ::Union* u, std::ostream& out);
    void emit_switch_type(const std::string& name, BBQ::Switch* sw, std::ostream& out);
    void emit_alternatives_type(const std::string& name, BBQ::Alternatives* alts, std::ostream& out);

    // Inline struct support
    void register_inline_structs(const std::string& prefix, BBQ::Struct* st);
    void emit_inline_structs(const std::string& rule_name, std::ostream& out);

    // Variant type string helper
    std::string variant_type_list(const std::vector<std::string>& types);

    Sema& sema_;
    std::string namespace_;

    // Registry: inline Struct* → synthesized name (e.g. "Outer_inner")
    std::unordered_map<BBQ::Struct*, std::string> inline_struct_names_;
    // Emission order for inline structs (inner-first for dependencies)
    std::vector<std::pair<std::string, BBQ::Struct*>> inline_struct_order_;
};

} // namespace bbqgen
