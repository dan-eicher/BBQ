#pragma once

#include <cstring>
#include <string>

#include "ParseArena.h"
#include "StringPool.h"
#include "KontNode.h"

namespace bbq::cek {

// CompiledGrammar — owns all memory for a compiled grammar.
// Stays alive across multiple parses. Built by bbq::Compiler::
// compile_grammar (see bbq_compile.h, generated from bbq.ddcg).
struct CompiledGrammar {
    struct RuleEntry {
        const char* name;       // Interned
        KontNode* entry;        // Rule's entry continuation
    };

    RuleEntry* rules = nullptr;
    int rule_count = 0;
    bool default_little_endian = true;

    ParseArena arena;
    StringPool strings;

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

} // namespace bbq::cek
