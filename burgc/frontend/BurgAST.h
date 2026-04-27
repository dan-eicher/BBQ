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

// A terminal declaration: TERM LoadInt = 1   /* numeric form */
//                         TERM LoadInt = SIR_LOADINT  /* symbolic form */
// Emitted into the generated matcher as the value of `BURG_<name>` —
// numeric form emits the integer literal; symbolic form emits the
// identifier verbatim so the C preprocessor substitutes it at
// compile time (letting an external enum drive terminal IDs without
// the .burg file having to restate them).
struct TermDecl {
    std::string name;
    int64_t number;         // set when symbol is empty
    std::string symbol;     // non-empty → emit as identifier
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
    std::string ns;                     // NAMESPACE name (optional)
    std::string members;                // MEMBERS block (class member declarations)
    std::string private_helpers;        // PRIVATE block. C++: private section
                                        // of BurgMatcher. C: file-local in .c.
    std::vector<std::string> headers;   // (. .) header blocks (user includes/decls)
    std::vector<Rule*> rules;
    SourceLoc loc;
};

} // namespace burg_ast
