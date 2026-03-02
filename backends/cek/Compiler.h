#pragma once

#include <string>
#include <unordered_map>

#include "ParseArena.h"
#include "StringPool.h"
#include "KontNode.h"
#include "Expr.h"

// Forward declarations for BBQ AST (avoid full include)
namespace BBQ {
    struct Grammar;
    struct Rule;
    struct TypeExpr;
    struct Struct;
    struct Field;
    struct Alternatives;
    struct Union;
    struct Array;
    struct Optional;
    struct Switch;
    struct Compute;
    struct Primitive;
    struct RuleRef;
    struct Extern;
    struct Bitfield;
    struct EndianSwitch;
    struct PrimitiveKind;
    struct Expr;
    struct RefPath;
}

namespace bbqgen { class Sema; }

namespace bbq::cek {

// --- CompiledGrammar ---
// Owns all allocated memory for a compiled grammar.
// Stays alive across multiple parses.

struct CompiledGrammar {
    struct RuleEntry {
        const char* name;       // Interned
        KontNode* entry;        // Rule's entry continuation
    };

    RuleEntry* rules = nullptr;
    int rule_count = 0;
    bool default_little_endian = true;

    ParseArena arena;           // Owns all nodes + compiled exprs
    StringPool strings;         // Owns all interned strings

    KontNode* lookup(const char* name) const {
        for (int i = 0; i < rule_count; i++) {
            if (rules[i].name == name) return rules[i].entry;
        }
        return nullptr;
    }

    KontNode* lookup(const std::string& name) const {
        for (int i = 0; i < rule_count; i++) {
            if (std::strcmp(rules[i].name, name.c_str()) == 0)
                return rules[i].entry;
        }
        return nullptr;
    }
};

// --- CEKCompiler ---
// Transforms a validated, topo-sorted BBQ AST into a continuation graph.

class CEKCompiler {
public:
    // Main entry point. Takes validated, topo-sorted AST from Sema.
    // Returns heap-allocated CompiledGrammar (caller owns).
    CompiledGrammar* compile(BBQ::Grammar* grammar,
                              const bbqgen::Sema& sema);

private:
    ParseArena* arena_ = nullptr;
    StringPool* strings_ = nullptr;

    // Rule entries built during compilation (for forward refs)
    std::unordered_map<std::string, KontNode*> rule_entries_;

    // --- Type expression compilation (DDCG) ---
    KontNode* compile_rule(BBQ::Rule* rule);
    KontNode* compile_type_expr(BBQ::TypeExpr* type,
                                 const char* field_name,
                                 KontNode* next);
    KontNode* compile_struct(BBQ::Struct* st, const char* name,
                              KontNode* next);
    KontNode* compile_field(BBQ::Field* field, KontNode* next);
    KontNode* compile_alternatives(BBQ::Alternatives* alts,
                                    KontNode* next);
    KontNode* compile_union(BBQ::Union* un, KontNode* next);
    KontNode* compile_array(BBQ::Array* arr, const char* name,
                             KontNode* next);
    KontNode* compile_optional(BBQ::Optional* opt, const char* name,
                                KontNode* next);
    KontNode* compile_switch(BBQ::Switch* sw, KontNode* next);
    KontNode* compile_primitive(BBQ::Primitive* prim, const char* name,
                                 KontNode* next);
    KontNode* compile_rule_ref(BBQ::RuleRef* ref, const char* name,
                                KontNode* next);
    KontNode* compile_extern(BBQ::Extern* ext, const char* name,
                              KontNode* next);
    KontNode* compile_bitfield(BBQ::Bitfield* bf, KontNode* next);
    KontNode* compile_endian_switch(BBQ::EndianSwitch* es,
                                     KontNode* next);

    // --- Expression compilation ---
    CompiledExpr* compile_expr(BBQ::Expr* expr);
    CompiledExpr* compile_ref_path(BBQ::RefPath* path);

    // --- Helpers ---
    PrimitiveInfo to_prim_info(BBQ::PrimitiveKind* kind);
    const char* intern(const std::string& s);
    KontNode* wrap_constraint(BBQ::Expr* constraint, KontNode* next);
};

} // namespace bbq::cek
