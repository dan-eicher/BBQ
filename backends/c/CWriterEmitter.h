#pragma once

#include "BBQ_AST.h"
#include "Sema.h"
#include "CTypeEmitter.h"
#include <string>
#include <sstream>

namespace bbqgen_c {

using bbqgen::Sema;

class CWriterEmitter {
public:
    CWriterEmitter(Sema& sema, CTypeEmitter& types)
        : sema_(sema), types_(types) {}

    std::string emit_decls(BBQ::Grammar* grammar);
    std::string emit_impls(BBQ::Grammar* grammar);

    bool emit_header_to_frame(BBQ::Grammar* grammar,
                              const std::string& frame_path,
                              const std::string& types_include,
                              std::ostream& out);
    bool emit_impl_to_frame(BBQ::Grammar* grammar,
                            const std::string& frame_path,
                            const std::string& writer_include,
                            std::ostream& out);

private:
    void emit_decl(BBQ::Rule* rule, std::ostream& out);
    void emit_impl(BBQ::Rule* rule, std::ostream& out);

    void emit_struct_write(const std::string& rule_name, BBQ::Struct* st,
                           const std::string& src, std::ostream& out, int indent);
    void emit_field_write(const std::string& rule_name, BBQ::Field* field,
                          const std::string& src, std::ostream& out, int indent);
    void emit_write_call(const std::string& src, BBQ::TypeExpr* type,
                         const std::string& ctx, std::ostream& out, int indent);
    void emit_bitfield_write(const std::string& ctx_name, BBQ::Bitfield* bf,
                              const std::string& src, std::ostream& out, int indent);

    std::string write_func_name(BBQ::PrimitiveKind* kind);
    std::string ind(int n);

    Sema& sema_;
    CTypeEmitter& types_;
};

} // namespace bbqgen_c
