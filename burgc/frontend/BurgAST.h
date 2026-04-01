// BurgAST.h — AST nodes for parsed .burg specifications
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace burg_ast {

struct SourceLoc {
    int line = 0;
    int col = 0;
};

// A terminal declaration: %term LoadInt=1 LoadFloat=2
struct TermDecl {
    std::string name;
    int64_t number;     // external symbol number
    SourceLoc loc;
};

// A tree pattern: term(child, child) or just a nonterm/term leaf
struct TreePattern {
    std::string name;                       // operator or nonterm name
    std::vector<TreePattern*> children;     // empty for leaves
    SourceLoc loc;

    bool is_leaf() const { return children.empty(); }
};

// A rule: nonterm: pattern [where (. guard .)] = rule_number [cost] [(. action .)];
struct Rule {
    std::string nonterm;        // left-hand side nonterminal
    TreePattern* pattern;       // tree pattern (right-hand side)
    int64_t rule_number;        // external rule number
    int64_t cost;               // cost (default 0)
    std::string guard;          // where-clause predicate (empty if none)
    std::string action;         // semantic action code (empty if none)
    SourceLoc loc;
};

// Top-level specification
struct Spec {
    std::vector<TermDecl*> terminals;
    std::string start;                  // %start nonterminal (optional)
    std::string members;                // MEMBERS block (class member declarations)
    std::vector<std::string> headers;   // (. .) header blocks (user includes/decls)
    std::vector<Rule*> rules;
    SourceLoc loc;
};

} // namespace burg_ast
