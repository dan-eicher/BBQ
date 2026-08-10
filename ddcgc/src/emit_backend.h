// ddcgc — emit-backend Template Method base.
//
// Both the C++ and (future) C backends inherit from EmitBackend. The
// base owns:
//
//   - the language-agnostic Ctx state (file pointer, schemas map,
//     check-result side-table, error sink, source-import resolution,
//     destination names + types)
//   - the language-agnostic pre-emit setup (resolving ir_root,
//     binding source schema, pulling DESTINATIONS names+types)
//   - rule-discovery helpers that don't reference the target language
//     (rules_for, source_sums_with_rules, sum_for_subject,
//     is_aux / is_pred / is_dest_variant)
//
// Subclasses implement the syntax-emitting hooks (type names, dispatch
// shape, file preamble, container literals, etc.) and the per-walk
// emit_expr / emit_stmt / emit_match logic that wraps the hooks.
//
// This is purely a refactor target; the existing emit_cpp() free
// function continues to work and is currently the canonical
// implementation. Future commits will move walk methods into the
// base as `virtual` with default impls, with CppBackend overriding
// the diverging forms.

#pragma once

#include "backend.h"
#include "ddcg_ast.h"
#include "schema.h"
#include "typecheck.h"

#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace ddcgc {

class EmitBackend : public Backend {
public:
    bool emit(const DdcgAst::File* file,
              const std::map<std::string, Schema>& schemas,
              const CheckResult& check,
              std::ostream& out_header,
              std::ostream* out_source,
              const std::string& header_filename,
              std::vector<std::string>& errors) override;

protected:
    // ── Walk state (populated by emit() before delegating to hooks) ──

    const DdcgAst::File* file_ = nullptr;
    const std::map<std::string, Schema>* schemas_ = nullptr;
    const CheckResult* check_ = nullptr;
    std::vector<std::string>* errors_ = nullptr;
    // out_ is the *currently active* output stream — emit code calls
    // out() / indent() and gets whichever stream is active. CBackend
    // swaps between header_ and source_ across emit_body sections.
    std::ostream* out_ = nullptr;
    std::ostream* out_header_ = nullptr;
    std::ostream* out_source_ = nullptr;
    std::string header_filename_;
    int indent_depth_ = 0;

    // Source-AST schema (the .ddcg's first import).
    std::string source_import_;
    const Schema* source_schema_ = nullptr;

    // Destination type names, lowered to the target language.
    // Subclasses populate the `*_type_` fields via `dest_type()`
    // (the suffix differs: `_t` typedef in C++ today, `<name>_t`
    // in C as well). The DSL-level names (`*_name_`) are the
    // dispatcher parameter identifiers — those are language-
    // agnostic.
    std::string data_dest_type_;
    std::string ctrl_dest_type_;
    std::string env_dest_type_;       // empty when no env_dest declared
    std::string data_dest_name_;
    std::string ctrl_dest_name_;
    std::string env_dest_name_;

    // ir_root type — the return type of every dispatcher.
    std::string ir_root_type_;

    // ── Output helpers ───────────────────────────────────────────
    std::ostream& out() { return *out_; }
    std::ostream& indent() {
        for (int i = 0; i < indent_depth_; ++i) *out_ << "    ";
        return *out_;
    }

    // ── Match-as-expr threading ──────────────────────────────────
    // Set during MatchExpr emission. emit_stmt's tail-position branch
    // checks this: if non-empty, the tail expression assigns to the
    // named variable instead of returning from the enclosing function.
    std::string match_target_var_;

    // Suffix supply for the locals a match-expression declares. Nested
    // matches must not collide, which is all these names have to
    // achieve, and numbering them keeps the emitted file identical
    // across runs over the same input — reproducible output is what
    // lets a regeneration be diffed against a toolchain change, and
    // what makes byte-identical output usable as a falsifier.
    int match_var_ctr_ = 0;

    // Set by c_backend's per-case dispatcher loop: when a rule's guard
    // matches, the body's tail-position assigns to match_target_var_,
    // and then needs to `break` out of the enclosing switch case so the
    // following per-case rule blocks (including the no-guard fallthrough)
    // don't overwrite the result. cpp_backend leaves this false because
    // its tail emits `return X;` which already exits the function.
    bool tail_break_after_guard_match_ = false;

    // ── Shared walks (dispatch chains) ───────────────────────────
    // The per-AST-type dispatch chains live here so all backends
    // share the visitor logic. Each branch either emits directly
    // (forms with identical syntax across languages) or delegates to
    // a virtual hook (forms whose output text diverges).
    void emit_expr(DdcgAst::Expr* e);
    void emit_stmt(DdcgAst::Stmt* s, bool is_tail);
    void emit_stmts(const std::vector<DdcgAst::Stmt*>& body,
                    bool body_is_tail_position);
    void emit_block_expr(DdcgAst::BlockExpr* be);

    // Per-language hook for dest-variant constructor calls. The C
    // backend prepends `ctx` so recursive variants (parent: rho)
    // can arena-allocate; the C++ backend treats constructors as
    // bare free-fns (state lives on `this`).
    virtual void emit_dest_pattern(DdcgAst::DestPattern* dp) = 0;
    void emit_call_args(const std::vector<DdcgAst::Expr*>& args);
    std::string emit_expr_to_string(DdcgAst::Expr* e);

    // emit_rule_branch is the same in both backends — open match
    // blocks via the virtual emit_match hook, optionally wrap body
    // in `if (guard)`, emit body, close blocks. Lives here.
    void emit_rule_branch(const Schema& schema,
                          const Constructor& ctor,
                          DdcgAst::Pattern* head,
                          DdcgAst::Rule* rule);

    // ── Hooks (language-specific) ────────────────────────────────

    // Translate a schema sum/constructor name to the target-language
    // type expression suitable for declarations (e.g. `tiny::Lit*`
    // in C++, `tiny_lit_t*` in C).
    virtual std::string type_for_schema(const Schema& schema,
                                        const std::string& name) = 0;

    // Run the actual code emission. Called by emit() once setup is
    // complete. Subclass orchestrates file-level layout (preamble,
    // ctx struct, fwd decls, dispatchers); the per-form hooks below
    // do the rule-body work.
    virtual bool emit_body() = 0;

    // ── Per-form hooks called from the shared dispatch chains ────
    //
    // null literal — `nullptr` (C++) vs `NULL` (C).
    virtual std::string null_literal() const = 0;

    // Whether string equality should be emitted as `strcmp` (C, where
    // strings flow as `const char*`) or via the language's overloaded
    // `==` operator (C++ `std::string`). Default false → use `==`;
    // override true in the C backend.
    virtual bool string_eq_via_strcmp() const { return false; }

    // Whether `list.count` should be emitted as a struct-field access
    // (C: `xs.count`) or the standard-library size accessor (C++:
    // `xs.size()`). Default false → C++; override true in the C
    // backend.
    virtual bool c_style_list_count() const { return false; }

    // The cast expression used to coerce one ternary arm to the
    // widened parent-sum type when the arms are sibling schema-ctors.
    // C++: `cpp::Foo*` (just a pointer cast). C: the typedef name
    // resolved to. Hook so each backend formats its own.
    virtual std::string ternary_branch_cast(const Type& result_ty) = 0;

    // asdl enum-value reference (e.g. DSL `DtRef` from
    // `datatype = DtByte | DtShort | DtInt | DtRef`). C emits the
    // tag-constant macro `<MODULE>_<CTOR>`; C++ emits the
    // `module::Type::Ctor` enum-class form.
    virtual std::string emit_enum_value_ref(const std::string& module,
                                            const std::string& sum_name,
                                            const std::string& ctor) const = 0;

    // Aux/pred/fun call — mangling differs (method on `this` vs
    // `<prefix>_<name>(ctx, ...)` free function).
    virtual void emit_call(DdcgAst::CallExpr* c) = 0;

    // List concat (`a + b` where both sides are list-typed) — C++
    // uses a shared `_ddcgc_concat<T>` helper; C uses one helper per
    // element type emitted by the discovery pass.
    virtual void emit_list_concat(DdcgAst::Expr* parent,
                                  DdcgAst::Expr* left,
                                  DdcgAst::Expr* right) = 0;

    // List / tuple literals — `std::vector{}` vs stmt-expr; vs
    // `std::make_tuple` vs compound literal.
    virtual void emit_list_lit(DdcgAst::ListLit* l) = 0;
    virtual void emit_tuple_lit(DdcgAst::TupleLit* t) = 0;

    // Functional record-update (`r with field = v`).
    virtual void emit_with_expr(DdcgAst::WithExpr* w) = 0;

    // `build Schema.Ctor(args)` — `new` (C++) vs asdl-c arena
    // factory (C).
    virtual void emit_build(DdcgAst::BuildExpr* b) = 0;

    // Match-as-expression — IIFE lambda (C++) vs stmt-expr +
    // result variable (C).
    virtual void emit_match_expr(DdcgAst::MatchExpr* m) = 0;

    // Recursive gen() — method on `this` (C++) vs free function
    // with explicit ctx + Lnext fill-in (C).
    virtual void emit_gen_call(DdcgAst::GenCall* g) = 0;

    // Indexing into a list: C++ uses operator[] on std::vector; C uses
    // `.data[i]` because the emitted list type is a struct holding the
    // data pointer.
    virtual void emit_index_acc(DdcgAst::IndexAccExpr* ix) = 0;

    // FieldAccExpr on a sequence-typed schema field. Default returns
    // false → fall through to the generic `base.field` emission. C
    // overrides to wrap the (T**, count) pair into a list_t helper
    // so AST sequence fields are interchangeable with bind-list values.
    virtual bool emit_seq_field_acc(DdcgAst::FieldAccExpr*) { return false; }

    // Per-Stmt hooks — let-simple, let-destructure, for, label
    // each have substantially different code shapes per language.
    virtual void emit_let_simple(DdcgAst::LetStmt* l,
                                  DdcgAst::SimpleBind* sb) = 0;
    virtual void emit_let_destructure(DdcgAst::LetStmt* l) = 0;
    virtual void emit_for_stmt(DdcgAst::ForStmt* f) = 0;
    virtual void emit_label_stmt(DdcgAst::LabelStmt* lb) = 0;

    // emit_match: walks a ConstructorPat against a value expression,
    // opening blocks for tag/dynamic_cast checks + nested patterns.
    // Returns the count of opened blocks the caller must close.
    // The scaffolding shape (dynamic_cast chain vs switch-on-tag)
    // is structural enough that this stays a per-language hook.
    virtual int emit_match(const Schema& schema,
                            DdcgAst::ConstructorPat* cp,
                            const std::string& value_expr,
                            int& name_ctr) = 0;

    // ── Helpers (language-agnostic) ──────────────────────────────

    // Resolve the schema sum_name that a gen-call subject ought to
    // dispatch to, by consulting expr_types. Returns "" if the
    // subject isn't a schema type (caller falls back to runtime
    // dispatch or emits an error).
    std::string sum_for_subject(DdcgAst::Expr* subject) const;

    // Membership predicates over the file's declarations.
    bool is_aux(const std::string& name) const;
    bool is_pred(const std::string& name) const;
    bool is_dest_variant(const std::string& name) const;
    bool is_zero_arg_dest_variant(const std::string& name) const;

    // Resolve a bare ident to an asdl enum value across all imported
    // schemas. Returns `false` if not found or ambiguous; the
    // ambiguity case is caught earlier in typecheck. On success
    // populates {module, sum_name}.
    bool find_enum_value(const std::string& name,
                         std::string* out_module,
                         std::string* out_sum) const;

    // Collect the rules that match against the source schema's
    // (module, sum_name).
    std::vector<DdcgAst::Rule*> rules_for(const std::string& sum_name) const;

    // Discover, in source order of first appearance, every sum
    // within the source schema that has at least one rule.
    std::vector<std::string> source_sums_with_rules() const;
};

} // namespace ddcgc
