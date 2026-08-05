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

// ── REWRITE section ────────────────────────────────────────────────
// Directed equality rules over the SAME terminals the tiling rules use,
// for a saturating rewriter rather than the tree-DP tiler. A rule is
// data: a pattern to match, a template to build, an optional guard.
// There are no statements and no control flow — a template is a tree.

// The left-hand side. A node is either a terminal application, or a
// `$name` binder standing for whatever subtree sits at that leaf.
struct RewritePat {
    std::string name;                    // terminal name, or binder name
    bool is_binder = false;
    std::vector<RewritePat*> children;   // empty for leaves and binders
    SourceLoc loc;
};

// A foreign helper a template may call for a computed immediate, declared
// with its signature the way ddcgc's AUXILIARIES and opgen's `native` are:
//
//   AUXILIARIES
//     ctz : (int) -> int
//
// Declaring it is what lets a template call it by plain name while an
// undeclared name stays an error rather than being read as a helper.
struct AuxDecl {
    std::string name;
    std::vector<std::string> params;   // parameter type names, for arity
    std::string ret;
    SourceLoc loc;
};

// The right-hand side. A node builds a terminal, references a binder the
// pattern bound, or calls a declared auxiliary for a computed immediate.
// Which of the three a name denotes is resolved against the declarations
// during analysis, not guessed at parse time.
struct RewriteTmpl {
    enum Kind { Term, Binder, Helper };
    Kind kind = Term;
    std::string name;                    // terminal / binder / helper name
    std::vector<RewriteTmpl*> children;
    SourceLoc loc;
};

// name: Pattern => Template [where (. guard over binders .)] ;
// Direction is `=>` only; a bidirectional axiom is written as two rules,
// so the rule set never has to be read as a fixpoint of itself.
struct RewriteRule {
    std::string name;
    RewritePat*  pattern = nullptr;
    RewriteTmpl* tmpl = nullptr;
    std::string guard;                   // where-clause predicate, empty if none
    SourceLoc loc;
};

// Top-level specification
struct Spec {
    std::vector<TermDecl*> terminals;
    std::string start;                  // %start nonterminal (optional)
    // ENTRY nonterms: goals a consumer reduces to BY NAME, in addition to the
    // root's `start` goal. Goal-directed reduction is the formalism (iburg p.8
    // `rule(state, goalnt)`), so a consumer whose control nodes cannot live in
    // the grammar reduces its frontier subtrees at other goals. Declaring them
    // is what lets the liveness analysis see that demand; it has no effect on
    // the generated matcher, whose goal-directed API already exists.
    std::vector<std::string> entries;
    std::string ns;                     // NAMESPACE name (optional)
    std::string members;                // MEMBERS block (class member declarations)
    std::string private_helpers;        // PRIVATE block. C++: private section
                                        // of BurgMatcher. C: file-local in .c.
    std::vector<std::string> headers;   // (. .) header blocks (user includes/decls)
    std::vector<Rule*> rules;
    std::vector<AuxDecl*> auxiliaries;  // AUXILIARIES block, empty when absent
    std::vector<RewriteRule*> rewrites; // REWRITE section, empty when absent
    SourceLoc loc;
};

} // namespace burg_ast
