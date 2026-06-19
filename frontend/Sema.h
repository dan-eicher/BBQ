#pragma once

#include "BBQ_AST.h"
#include "Errors.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bbqgen {

// Expression type classification for type checking
enum class ExprType {
    Integer,   // int/uint primitives, int literals, EOI, remaining, pos, i
    Float,     // float primitives, float literals
    Numeric,   // join of Integer+Float (mixed arithmetic result)
    Bool,      // bool primitive, bool literals, comparisons, at_end
    String,    // string primitive, string literals
    Bytes,     // bytes primitive
    Endian,    // endian literals (little, big)
    Unknown    // extern, optional, unresolvable — permissive, never errors
};

// Resolved discriminator label for one switch case (shape side-data). The
// CaseValue AST classification lives once here; backends render it differently
// (the enum value vs the C `case` lines, where a Range expands to many).
struct SwitchCaseLabel {
    enum class Kind { Int, Range, Ident, Ref, Positional } kind = Kind::Positional;
    int64_t lo = 0, hi = 0;   // Int: value in lo; Range: [lo, hi]
    std::string name;         // Ident: the constant; Ref: the joined path
    int index = 0;            // case ordinal (the positional fallback)
};

// Scope for type checking within a struct
struct TypeCheckScope {
    std::unordered_map<std::string, ExprType> types;       // field name → ExprType
    std::unordered_map<std::string, BBQ::TypeExpr*> bodies; // field name → TypeExpr*
};

class Sema {
public:
    explicit Sema(ErrorReporter& errors) : errors_(errors) {}

    // Run all semantic checks on the grammar. Returns true if no errors.
    bool analyze(BBQ::Grammar* grammar);

    // Access the topological ordering of rules (after analyze).
    const std::vector<BBQ::Rule*>& sorted_rules() const { return sorted_; }

    // Resolved default endianness from @endian directive.
    BBQ::Endianness default_endian() const { return default_endian_; }

    // Look up a rule by name. Returns nullptr if not found.
    BBQ::Rule* lookup_rule(const std::string& name) const {
        auto it = rule_map_.find(name);
        return (it != rule_map_.end()) ? it->second : nullptr;
    }

    // Resolved fact (shape side-data): does this type own heap memory that needs
    // a _free? Derived from the AST + rule table, memoized so backends read it
    // instead of re-deriving ownership per render. AST-only — no CEK dependency.
    bool type_needs_free(BBQ::TypeExpr* type);

    // Resolved fact (shape side-data): a field's classified type, computed during
    // check_types and ATTACHED here instead of discarded. Backends (and the
    // c-view/cpp-zcow decode-on-access profiles) read it instead of re-deriving.
    // Returns ExprType::Unknown for a field never type-checked.
    ExprType field_type(BBQ::Field* field) const {
        auto it = field_types_.find(field);
        return it != field_types_.end() ? it->second : ExprType::Unknown;
    }

    // Resolved fact (shape side-data): the inline (anonymous nested) structs, in
    // declaration order (innermost first), with their synthesized base name
    // (`Rule_field` path). Computed once during analyze. The C namer spells these;
    // backends that emit inline defs iterate them. Replaces CTypeMapper's per-
    // instance walk so the set is computed once, not per backend.
    const std::vector<std::pair<std::string, BBQ::Struct*>>& inline_structs() const {
        return inline_structs_;
    }
    std::string inline_struct_name(BBQ::Struct* st) const {
        auto it = inline_struct_name_.find(st);
        return it != inline_struct_name_.end() ? it->second : "";
    }

    // Resolved fact (shape side-data): the discriminator label per switch case,
    // in order. The CaseValue classification is done once here; backends render
    // it. Memoized per Switch. AST-only — no CEK dependency.
    const std::vector<SwitchCaseLabel>& switch_case_labels(BBQ::Switch* sw);


private:
    // Phase 1: collect rule names, detect duplicates
    void collect_rules(BBQ::Grammar* grammar);

    // Phase 2: resolve @endian, rewrite Default endianness
    void resolve_endian(BBQ::Grammar* grammar);

    // Phase 3: validate references, build dependency graph
    void validate_rule(BBQ::Rule* rule);
    // `guarded` = true when this type sits under an Array.element or
    // Optional.element, where the recursion is finite at runtime
    // (count=0 / absence terminates). RuleRefs reached while guarded
    // do NOT count as topo-sort dependencies — the cycle is legal and
    // the compiler resolves the forward ref via patch_kont.
    void validate_type(BBQ::TypeExpr* type, const std::string& ctx,
                        bool guarded = false);
    void validate_expr(BBQ::Expr* expr, const std::string& ctx);
    void validate_ref_path(BBQ::RefPath* path, const std::string& ctx);

    // Phase 4: cycle detection + topological sort
    bool topological_sort();

    // Phase 5: per-struct forward reference checks (+ switch discriminator)
    void check_forward_refs(BBQ::Rule* rule);
    void check_forward_refs_struct(BBQ::Struct* st,
                                    std::unordered_set<std::string> inherited,
                                    const std::string& ctx,
                                    std::unordered_set<std::string> all_ancestor_fields = {});
    void collect_expr_refs(BBQ::Expr* expr, std::unordered_set<std::string>& refs);
    void collect_refpath_refs(BBQ::RefPath* path, std::unordered_set<std::string>& refs);

    // Phase 6: switch validation
    void validate_switch(BBQ::Switch* sw, const std::string& ctx);

    // Discriminator range analysis — used by validate_switch to skip
    // the no-default warning when cases provably cover the full range.
    std::optional<std::pair<int64_t, int64_t>>
        compute_value_range(BBQ::Expr* e);

    // Phase 7: expression type checking
    void check_types(BBQ::Rule* rule);
    void check_types_struct(BBQ::Struct* st, TypeCheckScope inherited,
                            const std::string& ctx);
    void check_field_types(BBQ::Field* field, const TypeCheckScope& scope,
                           const std::string& ctx);
    void check_interval_types(BBQ::Interval* iv, const TypeCheckScope& scope,
                              const std::string& ctx);

    // Type inference
    ExprType type_of_primitive_kind(BBQ::PrimitiveKind* kind);
    ExprType type_of_type_expr(BBQ::TypeExpr* type);
    ExprType type_of_expr(BBQ::Expr* expr, const TypeCheckScope& scope);
    ExprType type_of_ref_path(BBQ::RefPath* path, const TypeCheckScope& scope);

    // Structural resolution for field/array access
    BBQ::Struct* resolve_to_struct(BBQ::TypeExpr* type);
    BBQ::TypeExpr* resolve_to_array_element(BBQ::TypeExpr* type);

    // Type compatibility helpers
    static ExprType join(ExprType a, ExprType b);
    static bool is_numeric(ExprType t);
    static bool is_integer(ExprType t);
    static bool is_boolean_compatible(ExprType t);
    static bool types_compatible(ExprType expr, ExprType decl);
    static bool is_valid_cpp_ident(const std::string& s);

    // Did-you-mean suggestion helpers
    std::string find_closest_rule(const std::string& name) const;
    static std::string find_closest_field(const std::string& name,
                                          const std::vector<BBQ::Field*>& fields);
    static int edit_distance(const std::string& a, const std::string& b);

    // Constraint helpers
    static bool body_has_constraint(BBQ::TypeExpr* body);

    // Inline-struct registration (shape side-data): synthesize nested-struct
    // names in declaration order, innermost first. `base` is the accumulated
    // Rule_field path (NOT the CLI prefix — the C namer applies that).
    void register_inline_structs(const std::string& base, BBQ::Struct* st);

    // Phase 8: resolve cross-rule references
    // Populates Simple::resolved_path for refs that aren't local fields
    void resolve_cross_refs();
    void resolve_cross_refs_in_expr(BBQ::Expr* expr, const std::string& rule_name,
                                     const std::unordered_set<std::string>& local_fields);
    void resolve_cross_refs_in_type(BBQ::TypeExpr* type, const std::string& rule_name,
                                     const std::unordered_set<std::string>& local_fields);
    bool find_field_recursive(BBQ::Struct* st, const std::string& name,
                               std::string& path);

    ErrorReporter& errors_;
    std::unordered_map<std::string, BBQ::Rule*> rule_map_;
    std::unordered_map<BBQ::TypeExpr*, bool> needs_free_;     // ownership memo (shape side-data)
    std::unordered_map<BBQ::Field*, ExprType> field_types_;   // resolved field types (shape side-data)
    std::vector<std::pair<std::string, BBQ::Struct*>> inline_structs_;     // order (shape side-data)
    std::unordered_map<BBQ::Struct*, std::string> inline_struct_name_;     // Struct* -> base name
    std::unordered_map<BBQ::Switch*, std::vector<SwitchCaseLabel>> switch_labels_;  // case labels (shape side-data)
    std::unordered_map<std::string, std::unordered_set<std::string>> deps_;
    std::vector<BBQ::Rule*> sorted_;
    BBQ::Endianness default_endian_ = BBQ::Endianness::Little;
};

} // namespace bbqgen
