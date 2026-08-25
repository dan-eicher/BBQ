#pragma once
//
// CTypeMapper — the canonical BBQ-type → C-name/type mapping.
//
// Pulled out of CTypeEmitter so it outlives that emitter's retirement and is the
// single source of C naming for the C-side consumers (the cross-backend test harness
// maps rule names through here), so they agree with the generated names by construction.
// (Same move as to_snake_case → frontend/Names.)
//
// `c_type` of an inline struct resolves through Sema's inline-struct set (a
// resolved fact computed during analyze) — this mapper only spells the C name.
//
#include "BBQ_AST.h"
#include "Sema.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace bbqgen_c {

using bbqgen::Sema;

class CTypeMapper {
public:
    CTypeMapper(Sema& sema, const std::string& prefix = "")
        : sema_(sema), prefix_(prefix.empty() ? "" : to_snake(prefix)) {}

    const std::string& prefix() const { return prefix_; }

    // Rule name → C identifiers.
    std::string prefixed_name(const std::string& name) const;
    std::string type_name(const std::string& rule_name) const;
    std::string read_func(const std::string& rule_name) const;
    std::string write_func(const std::string& rule_name) const;

    // TypeExpr / primitive → C type spelling.
    std::string c_type(BBQ::TypeExpr* type) const;
    std::string primitive_c_type(BBQ::PrimitiveKind* kind) const;
    std::string bitfield_entry_type(int64_t width, bool is_signed) const;

    // Inline structs in declaration order — delegated to Sema (the resolved fact),
    // for backends that emit their defs.
    const std::vector<std::pair<std::string, BBQ::Struct*>>& inline_structs() const {
        return sema_.inline_structs();
    }

private:
    static std::string to_snake(const std::string& name);

    Sema& sema_;
    std::string prefix_;
};

}  // namespace bbqgen_c
