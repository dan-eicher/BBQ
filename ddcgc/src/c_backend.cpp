// ddcgc — C emit backend.
//
// Subclasses EmitBackend (Template Method base). Differences from the
// C++ backend:
//
//   - Dispatch is `switch (node->tag)` over the asdl-c tag enum, not
//     a chain of dynamic_cast.
//   - Field access on tagged-union sums goes through the variant arm:
//     `node->variant_name.field` (asdl-c convention).
//   - Aux/pred calls become `<prefix>_<name>(ctx, args)` — free
//     functions, ctx threaded explicitly (no method calls).
//   - Constructors call asdl-c factories: `<module>_<snake>(arena, ...)`.
//   - No `auto`, `std::vector`, `std::string`, `std::tuple`, `nullptr`.
//     Forms that have no clean C analog (lists, tuples, sequence-for,
//     match-as-expr) emit a `/*TODO C: ...*/` marker so the gap is
//     visible at compile time rather than silently producing bad code.
//
// See c_backend.h for the user contract.

#include "c_backend.h"
#include "emit_backend.h"

#include <cctype>
#include <map>
#include <set>

namespace ddcgc {

namespace {

bool is_c_keyword(const std::string& s) {
    // Matches asdl's reserved_keywords (C + C++) so the variant arm
    // names this function returns line up with the asdl-c-emitted
    // tagged-union member names. Adding C-only keywords here without
    // the corresponding asdl entry causes downstream generated-code
    // mismatch.
    static const std::set<std::string> kw = {
        "alignas","alignof","and","and_eq","asm","auto","bitand","bitor",
        "bool","break","case","catch","char","char8_t","char16_t","char32_t",
        "class","compl","concept","const","consteval","constexpr","const_cast",
        "continue","co_await","co_return","co_yield","decltype","default","delete",
        "do","double","dynamic_cast","else","enum","explicit","export","extern",
        "false","float","for","friend","goto","if","inline","int","long",
        "mutable","namespace","new","noexcept","not","not_eq","nullptr","operator",
        "or","or_eq","private","protected","public","register","reinterpret_cast",
        "requires","return","short","signed","sizeof","static","static_assert",
        "static_cast","struct","switch","template","this","thread_local","throw",
        "true","try","typedef","typeid","typename","union","unsigned","using",
        "virtual","void","volatile","wchar_t","while","xor","xor_eq",
        "restrict","_Alignas","_Alignof","_Atomic","_Bool","_Complex","_Generic",
        "_Imaginary","_Noreturn","_Static_assert","_Thread_local","NULL"
    };
    return kw.count(s) > 0;
}

std::string safe_field_name(const std::string& name) {
    return is_c_keyword(name) ? name + "_" : name;
}

std::string to_upper(const std::string& s) {
    std::string r;
    for (char c : s) r += (char)std::toupper((unsigned char)c);
    return r;
}

// Mirror asdl's SnakeCase: insert `_` between lowercase→uppercase and
// digit→letter boundaries (e.g., S2I → s2_i, FieldAccess → field_access).
// Must match asdl/src/string_utils.cpp:SnakeCase exactly so the
// emitted factory names line up with asdl-c's output.
std::string to_snake(const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        bool add_underscore = false;
        if (i > 0) {
            if (std::isupper((unsigned char)c) &&
                std::islower((unsigned char)name[i - 1])) {
                add_underscore = true;
            } else if (std::isalpha((unsigned char)c) &&
                       std::isdigit((unsigned char)name[i - 1])) {
                add_underscore = true;
            }
        }
        if (add_underscore) out += '_';
        out += (char)std::tolower((unsigned char)c);
    }
    return out;
}

// asdl-c's tag-enum constant: <MODULE_UPPER>_<CTOR_UPPER>. asdl-c does
// NOT split Pascal-cased ctor parts, so `SwitchCase` → `SWITCHCASE`.
std::string c_tag_value(const std::string& module, const std::string& ctor) {
    return to_upper(module) + "_" + to_upper(ctor);
}

// asdl-c's variant-union arm name: snake_case(ctor), keyword-safe.
std::string c_variant_arm(const std::string& ctor) {
    return safe_field_name(to_snake(ctor));
}

// Schema type → C type spelling.
std::string c_type_for_impl(const Schema& schema,
                            const std::string& schema_type) {
    if (auto a = schema.aliases.find(schema_type); a != schema.aliases.end()) {
        return a->second;
    }
    auto ci = schema.constructors.find(schema_type);
    if (ci != schema.constructors.end()) {
        return c_type_for_impl(schema, ci->second.sum_name);
    }
    if (schema.sum_names.count(schema_type) ||
        schema.product_names.count(schema_type)) {
        std::string snake = to_snake(schema_type);
        std::string r = schema.module_name.empty()
            ? snake : schema.module_name + "_" + snake;
        r += "_t";
        if (schema.enum_sums.count(schema_type)) return r;
        if (schema.product_names.count(schema_type)) return r;
        return r + "*";
    }
    if (schema_type == "string")     return "const char*";
    if (schema_type == "int")        return "int";
    if (schema_type == "int64")      return "int64_t";
    if (schema_type == "bool")       return "bool";
    if (schema_type == "identifier") return "const char*";
    return schema_type;
}

// asdl-c's per-ctor factory: <module>_<snake_ctor>(arena, ...).
std::string c_ctor_factory(const Schema& schema, const std::string& ctor) {
    std::string m = schema.module_name;
    std::string c = to_snake(ctor);
    return m.empty() ? c : (m + "_" + c);
}

class CBackend : public EmitBackend {
public:
    bool emit_body() override;

protected:
    std::string type_for_schema(const Schema& schema,
                                const std::string& name) override {
        return c_type_for_impl(schema, name);
    }

protected:
    // Per-form hooks (override of EmitBackend's abstract surface).
    std::string null_literal() const override { return "NULL"; }
    std::string emit_enum_value_ref(const std::string& module,
                                    const std::string& /*sum*/,
                                    const std::string& ctor) const override {
        return c_tag_value(module, ctor);
    }
    void emit_call(DdcgAst::CallExpr* c) override;
    void emit_list_concat(DdcgAst::Expr* parent,
                          DdcgAst::Expr* left,
                          DdcgAst::Expr* right) override;
    void emit_list_lit(DdcgAst::ListLit* l) override;
    void emit_tuple_lit(DdcgAst::TupleLit* t) override;
    void emit_with_expr(DdcgAst::WithExpr* w) override;
    void emit_build(DdcgAst::BuildExpr* b) override;
    void emit_match_expr(DdcgAst::MatchExpr* m) override;
    void emit_gen_call(DdcgAst::GenCall* g) override;
    void emit_index_acc(DdcgAst::IndexAccExpr* ix) override;
    bool emit_seq_field_acc(DdcgAst::FieldAccExpr* fa) override;
    void emit_let_simple(DdcgAst::LetStmt* l,
                          DdcgAst::SimpleBind* sb) override;
    void emit_let_destructure(DdcgAst::LetStmt* l) override;
    void emit_for_stmt(DdcgAst::ForStmt* f) override;
    void emit_label_stmt(DdcgAst::LabelStmt* lb) override;
    void emit_dest_pattern(DdcgAst::DestPattern* dp) override;
    int emit_match(const Schema& schema,
                   DdcgAst::ConstructorPat* cp,
                   const std::string& value_expr,
                   int& name_ctr) override;

private:
    std::string compiler_prefix() const { return file_->compiler_name; }
    std::string ctx_type() const { return compiler_prefix() + "_ctx_t"; }

    std::string c_type_for(const Schema& s, const std::string& n) {
        return c_type_for_impl(s, n);
    }

    // Type-checker Type → C type spelling. Empty when caller should
    // fall back to `__typeof__`.
    std::string c_for_type(const Type& t);

    // DSL TypeRef → C type spelling. Used for aux/pred/fun signatures.
    std::string c_for_typeref(DdcgAst::TypeRef* tr);

    // Per-Field: render the C type of a local bind for that field.
    std::string c_for_field(const Schema& schema, const Field& f);
    // Same but ignoring the FieldFlag::Sequence wrapper — returns
    // the raw element type, used by sequence-bind emission.
    std::string c_for_field_elem(const Schema& schema, const Field& f);

    // Default-zero literal for a C type — used to fill the implicit
    // Lnext in gen-calls (C has no default arguments).
    std::string c_zero_for(const std::string& c_ty);

    // ── List/tuple/seq type registry ──────────────────────────
    // Discovery pass walks the file once and populates these so the
    // helper block at the top of the generated TU can emit the
    // necessary typedefs and inline helpers BEFORE any code refers
    // to them.

    // Distinct C-rendered list/sequence element types (e.g. "int",
    // "big_expr_t*"). One typedef + helpers per entry.
    std::set<std::string> list_elems_;

    // Distinct tuple shapes, in source order of first appearance.
    // Each shape is the per-component C type names; a single struct
    // typedef is emitted per shape.
    std::vector<std::vector<std::string>> tuple_shapes_;
    std::set<std::vector<std::string>> tuple_shapes_seen_;

    std::string sanitize_for_suffix(const std::string& c_ty);
    std::string list_type_name(const std::string& elem_c);
    std::string tuple_type_name(const std::vector<std::string>& shape);

    void register_type(const Type& t);
    void register_typeref(DdcgAst::TypeRef* tr);
    void register_pattern(DdcgAst::Pattern* p);
    void register_stmts(const std::vector<DdcgAst::Stmt*>& body);
    void register_stmt(DdcgAst::Stmt* s);
    void register_expr(DdcgAst::Expr* e);
    void run_discovery();
    void emit_helpers_block();

    std::string let_decl_for(DdcgAst::Expr* rhs);

    std::string field_check_expr(DdcgAst::Pattern* sub,
                                  const std::string& field_expr,
                                  const Field* field);
    // Fun emit is split: the signature goes in the header (extern
    // forward decl) and the body in the source.
    void emit_fun_signature(DdcgAst::Fun* f);
    void emit_fun(DdcgAst::Fun* f);

    void emit_file_preamble();
    void emit_ctx_struct();
    void emit_private_block();
    void emit_aux_forward_decls();
    void emit_dispatcher_signature(const std::string& sum_name);
    void emit_dispatcher(const std::string& sum_name);

    // Source-side and forward-decl helpers introduced by the .h/.c
    // split. emit_source_preamble writes the .c's `#include` of the
    // companion header.
    void emit_source_preamble();
    void emit_fun_forward_decls();
    void emit_dispatcher_forward_decls();
};

// ── Type translation ──────────────────────────────────────────

std::string CBackend::c_for_type(const Type& t) {
    switch (t.kind) {
        case TypeKind::Int:    return "int";
        case TypeKind::String: return "const char*";
        case TypeKind::Bool:   return "bool";
        case TypeKind::Ident:  return "const char*";
        case TypeKind::Label:  return "label_t";
        case TypeKind::Void:   return "void";
        case TypeKind::Schema: {
            auto sit = schemas_->find(t.schema_import);
            if (sit == schemas_->end()) return "";
            return c_type_for(sit->second, t.name);
        }
        case TypeKind::Dest:   return t.name + "_t";
        case TypeKind::List: {
            if (t.elems.empty()) return "";
            return list_type_name(c_for_type(t.elems[0]));
        }
        case TypeKind::Tuple: {
            std::vector<std::string> shape;
            for (const auto& e : t.elems) shape.push_back(c_for_type(e));
            return tuple_type_name(shape);
        }
        case TypeKind::NilLit:
        case TypeKind::Unknown:
            return "";
    }
    return "";
}

std::string CBackend::c_for_typeref(DdcgAst::TypeRef* tr) {
    if (auto* tn = dynamic_cast<DdcgAst::TypeName*>(tr)) {
        const std::string& n = tn->name;
        if (!tn->module.empty()) {
            auto sit = schemas_->find(tn->module);
            if (sit != schemas_->end()) return c_type_for(sit->second, n);
            return tn->module + "_" + n;
        }
        if (n == "ir_root") {
            if (!ir_root_type_.empty()) return ir_root_type_;
            return "/*BUG: ir_root used before resolution*/ int";
        }
        if (n == "int")                        return "int";
        if (n == "int64")                      return "int64_t";
        if (n == "string")                     return "const char*";
        if (n == "bool")                       return "bool";
        if (n == "ident" || n == "identifier") return "const char*";
        if (n == "label")                      return "label_t";
        if (n == "void")                       return "void";
        for (auto* d : file_->destinations) {
            if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
                if (dd->name == n) return n + "_t";
            } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
                if (cd->name == n) return n + "_t";
            } else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
                if (ed->name == n) return n + "_t";
            } else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) {
                if (sd->name == n) return n + "_t";
            }
        }
        const Schema* hit = nullptr;
        std::string hit_imp;
        for (const auto& [imp, schema] : *schemas_) {
            if (schema.sum_names.count(n) || schema.constructors.count(n)) {
                if (hit) {
                    errors_->push_back("typeref '" + n +
                        "' is ambiguous (defined in '" + hit_imp +
                        "' and '" + imp + "') — qualify as <module>." + n);
                    return "/*AMBIGUOUS*/" + n;
                }
                hit = &schema; hit_imp = imp;
            }
        }
        if (hit) return c_type_for(*hit, n);
        return n;
    }
    if (auto* tl = dynamic_cast<DdcgAst::TypeList*>(tr)) {
        return list_type_name(c_for_typeref(tl->elem));
    }
    if (auto* tt = dynamic_cast<DdcgAst::TypeTuple*>(tr)) {
        std::vector<std::string> shape;
        for (auto* e : tt->elems) shape.push_back(c_for_typeref(e));
        return tuple_type_name(shape);
    }
    return "void";
}

std::string CBackend::c_for_field_elem(const Schema& schema, const Field& f) {
    if (f.is_schema) {
        return c_type_for(schema, f.type_name);
    }
    if (f.type_name == "int")        return "int";
    if (f.type_name == "int8")       return "int8_t";
    if (f.type_name == "int16")      return "int16_t";
    if (f.type_name == "int32")      return "int32_t";
    if (f.type_name == "int64")      return "int64_t";
    if (f.type_name == "uint8")      return "uint8_t";
    if (f.type_name == "uint16")     return "uint16_t";
    if (f.type_name == "uint32")     return "uint32_t";
    if (f.type_name == "uint64")     return "uint64_t";
    if (f.type_name == "string")     return "const char*";
    if (f.type_name == "bool")       return "bool";
    if (f.type_name == "identifier") return "const char*";
    return f.type_name;
}

std::string CBackend::c_for_field(const Schema& schema, const Field& f) {
    std::string elem = c_for_field_elem(schema, f);
    if (f.flag == FieldFlag::Sequence) {
        return list_type_name(elem);
    }
    return elem;
}

std::string CBackend::c_zero_for(const std::string& c_ty) {
    if (!c_ty.empty() && c_ty.back() == '*') return "NULL";
    if (c_ty == "int" || c_ty == "int64_t" || c_ty == "label_t") return "0";
    if (c_ty == "bool") return "false";
    if (c_ty == "const char*") return "NULL";
    if (c_ty == "void") return "(void)0";
    return "(" + c_ty + "){0}";
}

// ── Type-name suffix sanitisation ────────────────────────────
//
// Compose stable C identifiers from arbitrary C type spellings so the
// emitted helper structs/functions have unique names per shape.
//   int               → int
//   const char*       → const_char_ptr
//   big_expr_t*       → big_expr_t_ptr
std::string CBackend::sanitize_for_suffix(const std::string& c_ty) {
    std::string r;
    bool prev_break = false;
    for (char ch : c_ty) {
        if (std::isalnum((unsigned char)ch) || ch == '_') {
            r += ch;
            prev_break = false;
        } else if (ch == '*') {
            if (!r.empty() && r.back() != '_') r += '_';
            r += "ptr";
            prev_break = false;
        } else {
            if (!prev_break && !r.empty()) {
                r += '_';
                prev_break = true;
            }
        }
    }
    if (!r.empty() && r.back() == '_') r.pop_back();
    return r;
}

std::string CBackend::list_type_name(const std::string& elem_c) {
    return compiler_prefix() + "_list_" + sanitize_for_suffix(elem_c) + "_t";
}

std::string CBackend::tuple_type_name(const std::vector<std::string>& shape) {
    std::string r = compiler_prefix() + "_tup";
    for (const auto& c : shape) {
        r += "_";
        r += sanitize_for_suffix(c);
    }
    return r + "_t";
}

// ── Discovery pass ───────────────────────────────────────────
//
// Walks the file's AST + the type-checker's expr_types side-table
// and records every distinct list-element c-type and tuple shape.
// The helper block at the top of the generated TU emits one typedef
// + helper bundle per entry, BEFORE any other code references them.

void CBackend::register_type(const Type& t) {
    if (t.kind == TypeKind::List) {
        if (!t.elems.empty()) {
            std::string elem_c = c_for_type(t.elems[0]);
            // Skip when the element type didn't resolve (NilLit-typed
            // empty literals, Unknown placeholders) — registering "" as
            // a list elem emits a broken `ycdg_list__t` helper. The
            // typed concrete uses register their elem separately.
            if (!elem_c.empty()) list_elems_.insert(elem_c);
            register_type(t.elems[0]);
        }
    } else if (t.kind == TypeKind::Tuple) {
        std::vector<std::string> shape;
        for (const auto& e : t.elems) {
            shape.push_back(c_for_type(e));
            register_type(e);
        }
        if (tuple_shapes_seen_.insert(shape).second) {
            tuple_shapes_.push_back(shape);
        }
    }
}

void CBackend::register_typeref(DdcgAst::TypeRef* tr) {
    if (auto* tl = dynamic_cast<DdcgAst::TypeList*>(tr)) {
        std::string elem_c = c_for_typeref(tl->elem);
        if (!elem_c.empty()) list_elems_.insert(elem_c);
        register_typeref(tl->elem);
    } else if (auto* tt = dynamic_cast<DdcgAst::TypeTuple*>(tr)) {
        std::vector<std::string> shape;
        for (auto* e : tt->elems) {
            shape.push_back(c_for_typeref(e));
            register_typeref(e);
        }
        if (tuple_shapes_seen_.insert(shape).second) {
            tuple_shapes_.push_back(shape);
        }
    }
}

void CBackend::register_pattern(DdcgAst::Pattern* p) {
    if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(p)) {
        for (auto* fp : cp->fields) {
            if (auto* nf = dynamic_cast<DdcgAst::NamedFieldPat*>(fp)) {
                register_pattern(nf->pat);
            }
        }
    }
}

void CBackend::register_expr(DdcgAst::Expr* e) {
    if (!e) return;
    auto it = check_->expr_types.find(e);
    if (it != check_->expr_types.end()) register_type(it->second);

    if (auto* t = dynamic_cast<DdcgAst::TernaryExpr*>(e)) {
        register_expr(t->cond);
        register_expr(t->then_);
        register_expr(t->else_);
    } else if (auto* b = dynamic_cast<DdcgAst::BinOp*>(e)) {
        register_expr(b->left);
        register_expr(b->right);
    } else if (auto* u = dynamic_cast<DdcgAst::UnaryOp*>(e)) {
        register_expr(u->operand);
    } else if (auto* call = dynamic_cast<DdcgAst::CallExpr*>(e)) {
        register_expr(call->fn);
        for (auto* a : call->args) register_expr(a);
    } else if (auto* fa = dynamic_cast<DdcgAst::FieldAccExpr*>(e)) {
        register_expr(fa->base);
    } else if (auto* ix = dynamic_cast<DdcgAst::IndexAccExpr*>(e)) {
        register_expr(ix->base);
        register_expr(ix->index);
    } else if (auto* l = dynamic_cast<DdcgAst::ListLit*>(e)) {
        for (auto* x : l->elems) register_expr(x);
    } else if (auto* tu = dynamic_cast<DdcgAst::TupleLit*>(e)) {
        for (auto* x : tu->elems) register_expr(x);
    } else if (auto* dp = dynamic_cast<DdcgAst::DestPattern*>(e)) {
        for (auto* a : dp->args) register_expr(a);
    } else if (auto* w = dynamic_cast<DdcgAst::WithExpr*>(e)) {
        register_expr(w->base);
        register_expr(w->value);
    } else if (auto* be = dynamic_cast<DdcgAst::BuildExpr*>(e)) {
        for (auto* a : be->args) register_expr(a);
    } else if (auto* m = dynamic_cast<DdcgAst::MatchExpr*>(e)) {
        register_expr(m->subject);
        for (auto* arm : m->arms) {
            register_pattern(arm->pat);
            register_stmts(arm->body);
        }
    } else if (auto* g = dynamic_cast<DdcgAst::GenCall*>(e)) {
        register_expr(g->subject);
        for (auto* a : g->args) register_expr(a);
    }
}

void CBackend::register_stmt(DdcgAst::Stmt* s) {
    if (auto* let = dynamic_cast<DdcgAst::LetStmt*>(s)) {
        register_expr(let->value);
    } else if (auto* ma = dynamic_cast<DdcgAst::MutAssign*>(s)) {
        register_expr(ma->value);
    } else if (auto* f = dynamic_cast<DdcgAst::ForStmt*>(s)) {
        register_expr(f->seq);
        register_stmts(f->body);
    } else if (auto* ifs = dynamic_cast<DdcgAst::IfStmt*>(s)) {
        register_expr(ifs->cond);
        register_stmts(ifs->then_body);
        register_stmts(ifs->else_body);
    } else if (auto* es = dynamic_cast<DdcgAst::ExprStmt*>(s)) {
        register_expr(es->value);
    }
}

void CBackend::register_stmts(const std::vector<DdcgAst::Stmt*>& body) {
    for (auto* s : body) register_stmt(s);
}

void CBackend::run_discovery() {
    // 1. expr_types side-table covers every typed expression we'll
    //    walk; this catches list/tuple types inferred by the
    //    type-checker that may not appear in TypeRef positions
    //    (e.g. ListLit's contextual element type).
    for (const auto& [_, t] : check_->expr_types) register_type(t);

    // 2. Auxiliary / predicate / fun signatures contribute list+tuple
    //    types via TypeRefs (e.g. `cg_seq : (list<int>) -> int`).
    for (auto* a : file_->auxiliaries) {
        register_typeref(a->return_ty);
        for (auto* p : a->param_types) register_typeref(p);
    }
    for (auto* p : file_->predicates) {
        for (auto* pt : p->param_types) register_typeref(pt);
    }
    for (auto* f : file_->funs) {
        register_typeref(f->ret_ty);
        for (auto* p : f->params) register_typeref(p->ty);
        register_stmts(f->body);
    }

    // 3. Source-schema sequence fields lower to list types — the
    //    BindPat against them needs the matching list helper
    //    (alloc-and-copy from the asdl-c (T**, count) pair).
    if (source_schema_) {
        for (const auto& [_, ctor] : source_schema_->constructors) {
            for (const auto& f : ctor.fields) {
                if (f.flag == FieldFlag::Sequence) {
                    list_elems_.insert(c_for_field_elem(*source_schema_, f));
                }
            }
        }
    }

    // 4. Rule heads (their pattern field types) and rule bodies.
    for (auto* r : file_->rules) {
        for (auto* h : r->heads) register_pattern(h);
        if (r->guard.has_value()) register_expr(*r->guard);
        register_stmts(r->body);
    }
}

void CBackend::emit_helpers_block() {
    if (list_elems_.empty() && tuple_shapes_.empty()) return;

    out() << "/* ─── ddcgc-emitted list/tuple helpers ─── */\n";
    out() << "#include <stdlib.h>\n";
    out() << "#include <string.h>\n\n";

    // Per-element-type list: dynamic vector + alloc-and-copy from a
    // borrowed C array (used by Sequence-field bind).
    for (const auto& elem : list_elems_) {
        std::string t = list_type_name(elem);
        std::string suffix = sanitize_for_suffix(elem);
        std::string fn_prefix = compiler_prefix() + "_list_" + suffix;
        out() << "typedef struct {\n"
              << "    " << elem << "* data;\n"
              << "    int count;\n"
              << "    int capacity;\n"
              << "} " << t << ";\n\n";
        out() << "static inline " << t << " " << fn_prefix << "_make(void) {\n"
              << "    " << t << " r = {0}; return r;\n"
              << "}\n\n";
        out() << "static inline " << t << " " << fn_prefix
              << "_push(" << t << " a, " << elem << " v) {\n"
              << "    if (a.count >= a.capacity) {\n"
              << "        int nc = a.capacity ? a.capacity * 2 : 4;\n"
              << "        a.data = (" << elem << "*)realloc(a.data, sizeof(*a.data) * (size_t)nc);\n"
              << "        a.capacity = nc;\n"
              << "    }\n"
              << "    a.data[a.count++] = v;\n"
              << "    return a;\n"
              << "}\n\n";
        out() << "static inline " << t << " " << fn_prefix
              << "_concat(" << t << " a, " << t << " b) {\n"
              << "    for (int i = 0; i < b.count; ++i) a = " << fn_prefix
              << "_push(a, b.data[i]);\n"
              << "    return a;\n"
              << "}\n\n";
        out() << "static inline " << t << " " << fn_prefix
              << "_from_arr(" << elem << "* arr, int n) {\n"
              << "    " << t << " r = {0};\n"
              << "    if (n <= 0) return r;\n"
              << "    r.data = (" << elem << "*)malloc(sizeof(*r.data) * (size_t)n);\n"
              << "    memcpy(r.data, arr, sizeof(*r.data) * (size_t)n);\n"
              << "    r.count = n;\n"
              << "    r.capacity = n;\n"
              << "    return r;\n"
              << "}\n\n";
    }

    // Per-shape tuple struct.
    for (const auto& shape : tuple_shapes_) {
        std::string t = tuple_type_name(shape);
        out() << "typedef struct {\n";
        for (size_t i = 0; i < shape.size(); ++i) {
            out() << "    " << shape[i] << " _" << i << ";\n";
        }
        out() << "} " << t << ";\n\n";
    }
}

// ── Per-form hooks (called by EmitBackend's shared dispatch) ──

// Aux/pred/fun calls all become mangled free fns with ctx threaded
// first. Other CallExprs (dest-variant ctors, in-scope identifiers)
// emit as written — they resolve to consumer-supplied free fns from
// the runtime header.
void CBackend::emit_call(DdcgAst::CallExpr* call) {
    if (auto* fn_id = dynamic_cast<DdcgAst::IdentExpr*>(call->fn)) {
        const std::string& nm = fn_id->name;
        bool is_fun = false;
        for (auto* fn : file_->funs) {
            if (fn->name == nm) { is_fun = true; break; }
        }
        if (is_aux(nm) || is_pred(nm) || is_fun) {
            out() << compiler_prefix() << "_" << nm << "(ctx";
            for (auto* a : call->args) {
                out() << ", ";
                emit_expr(a);
            }
            out() << ")";
            return;
        }
        // Dest-variant constructor (effect/loc/single/pair/ret/...).
        // ddcgc prepends `ctx` so the user's runtime constructor has
        // access to ctx state (arena for recursive variants, etc.) —
        // matching the AUX/pred/fun convention. The constructor is
        // user-supplied via the runtime header, no compiler prefix.
        bool is_dest_variant = false;
        for (auto* d : file_->destinations) {
            std::vector<DdcgAst::DestVariant*> vs;
            if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) vs = dd->variants;
            else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) vs = cd->variants;
            else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) vs = ed->variants;
            else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) vs = sd->variants;
            for (auto* v : vs) {
                if (v->name == nm) { is_dest_variant = true; break; }
            }
            if (is_dest_variant) break;
        }
        if (is_dest_variant) {
            out() << nm << "(ctx";
            for (auto* a : call->args) {
                out() << ", ";
                emit_expr(a);
            }
            out() << ")";
            return;
        }
    }
    emit_expr(call->fn);
    emit_call_args(call->args);
}

void CBackend::emit_list_concat(DdcgAst::Expr* parent,
                                 DdcgAst::Expr* left,
                                 DdcgAst::Expr* right) {
    auto it = check_->expr_types.find(parent);
    if (it == check_->expr_types.end() ||
        it->second.kind != TypeKind::List ||
        it->second.elems.empty()) {
        out() << "/*BUG: list concat without inferable element type*/ ";
        emit_expr(left);
        return;
    }
    std::string elem = c_for_type(it->second.elems[0]);
    out() << compiler_prefix() << "_list_"
          << sanitize_for_suffix(elem) << "_concat(";
    emit_expr(left);
    out() << ", ";
    emit_expr(right);
    out() << ")";
}

void CBackend::emit_list_lit(DdcgAst::ListLit* l) {
    // Stmt-expression that builds the list via per-element pushes
    // onto a freshly-made empty list. Element type comes from the
    // type-checker.
    auto it = check_->expr_types.find(l);
    if (it == check_->expr_types.end() ||
        it->second.kind != TypeKind::List ||
        it->second.elems.empty()) {
        out() << "/*BUG: list literal without inferable element type*/ ((void)0)";
        return;
    }
    std::string elem = c_for_type(it->second.elems[0]);
    std::string fn_pfx = compiler_prefix() + "_list_" +
                         sanitize_for_suffix(elem);
    out() << "({ " << list_type_name(elem) << " _l = "
          << fn_pfx << "_make();";
    for (auto* x : l->elems) {
        out() << " _l = " << fn_pfx << "_push(_l, ";
        emit_expr(x);
        out() << ");";
    }
    out() << " _l; })";
}

void CBackend::emit_tuple_lit(DdcgAst::TupleLit* tu) {
    auto it = check_->expr_types.find(tu);
    if (it == check_->expr_types.end() ||
        it->second.kind != TypeKind::Tuple) {
        out() << "/*BUG: tuple literal without inferred type*/ ((void)0)";
        return;
    }
    std::vector<std::string> shape;
    for (const auto& te : it->second.elems) shape.push_back(c_for_type(te));
    out() << "(" << tuple_type_name(shape) << "){";
    for (size_t i = 0; i < tu->elems.size(); ++i) {
        if (i) out() << ", ";
        emit_expr(tu->elems[i]);
    }
    out() << "}";
}

void CBackend::emit_with_expr(DdcgAst::WithExpr* w) {
    // GNU statement-expression copy-then-mutate. __typeof__ avoids
    // spelling the type — gcc/clang only (BBQ-generated C target).
    std::string base = emit_expr_to_string(w->base);
    out() << "({ __typeof__((" << base << ")) _w = (" << base
          << "); _w." << w->field << " = (";
    emit_expr(w->value);
    out() << "); _w; })";
}

void CBackend::emit_build(DdcgAst::BuildExpr* b) {
    auto sit = schemas_->find(b->schema);
    if (sit == schemas_->end()) {
        errors_->push_back("build: unknown schema '" + b->schema + "'");
        out() << "/*BUG: unknown schema*/((void)0)";
        return;
    }
    // asdl-c factories take `*` (sequence) fields as a (T**, int) pair.
    // List-typed DSL args are emitted as ycdg_list_T_t structs; unpack
    // to .data + .count when the constructor's matching field is a
    // sequence so the factory call lines up.
    const auto& schema = sit->second;
    const Constructor* ctor = nullptr;
    auto cit = schema.constructors.find(b->ctor);
    if (cit != schema.constructors.end()) ctor = &cit->second;
    out() << c_ctor_factory(schema, b->ctor) << "(ctx->arena";
    for (size_t i = 0; i < b->args.size(); ++i) {
        out() << ", ";
        const Field* matched = nullptr;
        if (ctor && i < ctor->fields.size()) matched = &ctor->fields[i];
        bool seq_unpack = false;
        if (matched && matched->flag == FieldFlag::Sequence) {
            auto et = check_->expr_types.find(b->args[i]);
            if (et != check_->expr_types.end() &&
                et->second.kind == TypeKind::List) {
                seq_unpack = true;
            }
        }
        if (seq_unpack) {
            out() << "(";
            emit_expr(b->args[i]);
            out() << ").data, (";
            emit_expr(b->args[i]);
            out() << ").count";
        } else {
            emit_expr(b->args[i]);
        }
    }
    out() << ")";
}

void CBackend::emit_match_expr(DdcgAst::MatchExpr* m) {
    // Match-as-expression in C: GNU statement-expression with a
    // result variable assigned per arm. Subject is either a dest
    // tagged-union (switch on `.tag`) or an asdl enum sum (switch on
    // the value directly).
    auto camel_upper = [](const std::string& s) {
        std::string r;
        for (char ch : s) r += (char)std::toupper((unsigned char)ch);
        return r;
    };

    std::string dest_name;
    const Schema* enum_schema = nullptr;
    std::string enum_sum_name;
    std::string enum_module;
    const Schema* sum_schema = nullptr;     // non-enum schema sum
    std::string sum_name;
    std::string sum_import;
    bool int_subject = false;
    auto sit = check_->expr_types.find(m->subject);
    if (sit != check_->expr_types.end()) {
        const Type& st = sit->second;
        if (st.kind == TypeKind::Dest) {
            dest_name = st.name;
        } else if (st.kind == TypeKind::Schema) {
            auto sk = schemas_->find(st.schema_import);
            if (sk != schemas_->end()) {
                if (sk->second.enum_sums.count(st.name)) {
                    enum_schema = &sk->second;
                    enum_sum_name = st.name;
                    enum_module = sk->second.module_name;
                } else if (sk->second.sum_names.count(st.name)) {
                    sum_schema = &sk->second;
                    sum_name = st.name;
                    sum_import = st.schema_import;
                }
            }
        } else if (st.kind == TypeKind::Int) {
            int_subject = true;
        }
    }
    if (dest_name.empty() && !enum_schema && !sum_schema && !int_subject) {
        errors_->push_back("match: subject is not a dest tagged union, "
                            "asdl enum, schema sum, or int");
        out() << "((void)0)";
        return;
    }

    std::string result_ty;
    auto rti = check_->expr_types.find(m);
    if (rti != check_->expr_types.end()) {
        result_ty = c_for_type(rti->second);
    }
    if (result_ty.empty()) result_ty = "int";

    // Pick a unique local-name suffix from the address of the node
    // so nested matches don't collide on `_m` / `_r`.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "_%p", (const void*)m);
    std::string m_var = std::string("_m") + (buf + 2);
    std::string r_var = std::string("_r") + (buf + 2);

    std::string saved_target = match_target_var_;
    match_target_var_ = r_var;

    out() << "({ ";
    if (!dest_name.empty()) {
        out() << dest_name + "_t " << m_var << " = ";
    } else if (enum_schema) {
        out() << c_type_for(*enum_schema, enum_sum_name) << " " << m_var << " = ";
    } else if (sum_schema) {
        // Schema sum: subject is a tagged-union pointer (e.g. ast_expr_t*).
        out() << c_type_for(*sum_schema, sum_name) << " " << m_var << " = ";
    } else {
        out() << "int " << m_var << " = ";
    }
    emit_expr(m->subject);
    out() << "; " << result_ty << " " << r_var << " = "
          << c_zero_for(result_ty) << ";\n";

    if (!dest_name.empty()) {
        indent() << "    switch (" << m_var << ".tag) {\n";
    } else if (sum_schema) {
        // Schema sum: pointer-deref to read .tag.
        indent() << "    switch (" << m_var << "->tag) {\n";
    } else {
        // asdl enum or int: switch on the value directly.
        indent() << "    switch (" << m_var << ") {\n";
    }
    indent_depth_ += 2;

    std::string dest_upper = camel_upper(dest_name);
    auto find_dest_variant = [this, &dest_name](const std::string& nm)
        -> DdcgAst::DestVariant* {
        for (auto* d : file_->destinations) {
            std::string dn;
            std::vector<DdcgAst::DestVariant*> vs;
            if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
                dn = dd->name; vs = dd->variants;
            } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
                dn = cd->name; vs = cd->variants;
            } else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
                dn = ed->name; vs = ed->variants;
            } else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) {
                dn = sd->name; vs = sd->variants;
            }
            if (dn != dest_name) continue;
            for (auto* v : vs) if (v->name == nm) return v;
        }
        return nullptr;
    };
    auto is_enum_value_name =
        [&enum_schema, &enum_sum_name](const std::string& nm) {
        if (!enum_schema) return false;
        auto eit = enum_schema->sum_constructors.find(enum_sum_name);
        if (eit == enum_schema->sum_constructors.end()) return false;
        for (const auto& cn : eit->second) if (cn == nm) return true;
        return false;
    };

    // Track whether the user supplied a default arm — if so, skip the
    // auto-emitted `default: break;` after the loop (gcc errors on
    // "multiple default labels in one switch").
    bool user_default_arm = false;

    for (auto* arm : m->arms) {
        indent();
        if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(arm->pat);
            cp && cp->module.empty()) {
            out() << "case " << dest_upper << "_"
                  << camel_upper(cp->ctor) << ": {\n";
            indent_depth_++;
            // Look up the variant to identify recursive fields — fields
            // whose declared type is the dest type itself (e.g. ρ's
            // `parent: rho`). Recursive fields are stored as `dest_t*`
            // in the runtime struct (C structs can't recurse by value);
            // ddcgc auto-derefs at bind time so the rule body sees a
            // dest_t value matching the DSL-level type.
            auto* variant = find_dest_variant(cp->ctor);
            auto is_recursive_field = [variant, &dest_name](const std::string& fname) {
                if (!variant) return false;
                for (auto* f : variant->fields) {
                    if (f->name != fname) continue;
                    if (auto* tn = dynamic_cast<DdcgAst::TypeName*>(f->ty)) {
                        return tn->module.empty() && tn->name == dest_name;
                    }
                    return false;
                }
                return false;
            };
            for (auto* fp : cp->fields) {
                if (auto* nf = dynamic_cast<DdcgAst::NamedFieldPat*>(fp)) {
                    if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(nf->pat)) {
                        bool recursive = is_recursive_field(nf->name);
                        std::string field_expr = std::string(m_var) + "." + nf->name;
                        std::string rhs = recursive ? "*" + field_expr : field_expr;
                        indent() << "__typeof__(" << rhs << ") " << bp->name
                                 << " = " << rhs << ";\n";
                        // Suppress -Werror=unused-variable when the
                        // arm body doesn't reference the bind.
                        indent() << "(void)" << bp->name << ";\n";
                    }
                }
            }
        } else if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(arm->pat);
                   cp && sum_schema && cp->module == sum_import) {
            // Schema sum match arm: case AST_X: { bind fields via
            // _m->variant.field; ... }
            out() << "case " << c_tag_value(sum_schema->module_name, cp->ctor)
                  << ": {\n";
            indent_depth_++;
            std::string variant = c_variant_arm(cp->ctor);
            for (auto* fp : cp->fields) {
                if (auto* nf = dynamic_cast<DdcgAst::NamedFieldPat*>(fp)) {
                    if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(nf->pat)) {
                        std::string field_expr = std::string(m_var) +
                            "->" + variant + "." + safe_field_name(nf->name);
                        indent() << "__typeof__(" << field_expr << ") "
                                 << bp->name << " = " << field_expr << ";\n";
                        indent() << "(void)" << bp->name << ";\n";
                    }
                }
            }
        } else if (auto* ip = dynamic_cast<DdcgAst::IntPat*>(arm->pat)) {
            out() << "case " << ip->value << ": {\n";
            indent_depth_++;
        } else if (dynamic_cast<DdcgAst::WildcardPat*>(arm->pat)) {
            out() << "default: {\n";
            indent_depth_++;
            user_default_arm = true;
        } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat);
                   bp && enum_schema && is_enum_value_name(bp->name)) {
            out() << "case " << c_tag_value(enum_module, bp->name)
                  << ": {\n";
            indent_depth_++;
        } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat);
                   bp && find_dest_variant(bp->name) &&
                   find_dest_variant(bp->name)->fields.empty()) {
            out() << "case " << dest_upper << "_"
                  << camel_upper(bp->name) << ": {\n";
            indent_depth_++;
        } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat)) {
            out() << "default: {\n";
            indent_depth_++;
            indent() << "__typeof__(" << m_var << ") " << bp->name
                     << " = " << m_var << ";\n";
            indent() << "(void)" << bp->name << ";\n";
            user_default_arm = true;
        } else {
            errors_->push_back("match: unsupported arm pattern shape");
            out() << "default: {\n";
            indent_depth_++;
            user_default_arm = true;
        }
        // Arm body — tail-position emit_stmt sees match_target_var_
        // and emits `<r_var> = <expr>;` instead of `return <expr>;`.
        emit_stmts(arm->body, /*tail=*/true);
        indent() << "break;\n";
        indent_depth_--;
        indent() << "}\n";
    }
    if (!user_default_arm) {
        indent() << "default: break;\n";
    }
    indent_depth_ -= 2;
    indent() << "    }\n";
    indent() << "    " << r_var << "; })";

    match_target_var_ = saved_target;
}

// FieldAccExpr on a sequence-typed schema field: AST stores it as
// a flat (T**, int) pair; bind sites use _list_T_from_arr to wrap
// the pair into a list_t struct. Same wrapping is needed at every
// other access site (passing as fun arg, indexing, etc.) so the
// expression has the list_t shape its inferred type promises.
//
// Returns false (let the default `base.field` emit run) when the
// field isn't a sequence on a recognised schema base.
bool CBackend::emit_seq_field_acc(DdcgAst::FieldAccExpr* fa) {
    auto bt_it = check_->expr_types.find(fa->base);
    if (bt_it == check_->expr_types.end()) return false;
    if (bt_it->second.kind != TypeKind::Schema) return false;
    auto sit = schemas_->find(bt_it->second.schema_import);
    if (sit == schemas_->end()) return false;
    const Schema& schema = sit->second;
    const std::string& sname = bt_it->second.name;

    // The schema name may be a constructor (Product) or a sum. For
    // sums, resolve to the first/only constructor that has the named
    // sequence field — this matches the asdl-c flattening for
    // single-constructor sums and product types.
    const Field* matched = nullptr;
    auto cit = schema.constructors.find(sname);
    if (cit != schema.constructors.end()) {
        for (const auto& f : cit->second.fields) {
            if (f.name == fa->field) { matched = &f; break; }
        }
    } else {
        auto sci = schema.sum_constructors.find(sname);
        if (sci == schema.sum_constructors.end()) return false;
        for (const auto& cn : sci->second) {
            auto cit2 = schema.constructors.find(cn);
            if (cit2 == schema.constructors.end()) continue;
            for (const auto& f : cit2->second.fields) {
                if (f.name == fa->field) { matched = &f; break; }
            }
            if (matched) break;
        }
    }
    if (!matched || matched->flag != FieldFlag::Sequence) return false;

    std::string elem_c = c_for_field_elem(schema, *matched);
    std::string fn = compiler_prefix() + "_list_" +
                      sanitize_for_suffix(elem_c) + "_from_arr";
    out() << fn << "(";
    emit_expr(fa->base);
    out() << "->" << fa->field << ", ";
    emit_expr(fa->base);
    out() << "->" << fa->field << "_count)";
    return true;
}

void CBackend::emit_index_acc(DdcgAst::IndexAccExpr* ix) {
    // C list types are structs (data ptr + count); emit `.data[i]`
    // when the base is list-typed. Fall back to plain operator[]
    // for anything else (raw arrays, pointers).
    auto it = check_->expr_types.find(ix->base);
    bool is_list = (it != check_->expr_types.end() &&
                    it->second.kind == TypeKind::List);
    out() << "(";
    emit_expr(ix->base);
    out() << ")";
    if (is_list) out() << ".data";
    out() << "[";
    emit_expr(ix->index);
    out() << "]";
}

void CBackend::emit_gen_call(DdcgAst::GenCall* g) {
    std::string sum = sum_for_subject(g->subject);
    if (sum.empty()) {
        errors_->push_back(
            "gen-call subject's source-schema sum could not be "
            "resolved (subject type missing from expr_types)");
        out() << "/*BUG: unresolved gen*/ 0";
        return;
    }
    out() << compiler_prefix() << "_compile_" << sum << "(ctx, ";
    emit_expr(g->subject);
    for (auto* a : g->args) {
        out() << ", ";
        emit_expr(a);
    }
    // The dispatcher signature is: ctx, node, [env_dest,] data_dest,
    // ctrl_dest, Lnext. The DSL gen-call typically omits Lnext (the
    // paper's default). C has no default args, so when the call shape
    // is one short, fill in a zero literal.
    size_t expected = (env_dest_name_.empty() ? 0u : 1u) + 2u + 1u;
    size_t given = g->args.size();
    if (given + 1 == expected) {
        out() << ", " << c_zero_for(ir_root_type_);
    }
    out() << ")";
}


// ── Statement-form hooks ─────────────────────────────────────

std::string CBackend::let_decl_for(DdcgAst::Expr* rhs) {
    auto it = check_->expr_types.find(rhs);
    if (it == check_->expr_types.end()) return "";
    return c_for_type(it->second);
}

void CBackend::emit_let_simple(DdcgAst::LetStmt* l, DdcgAst::SimpleBind* sb) {
    std::string ty = let_decl_for(l->value);
    indent();
    if (ty.empty()) {
        // Fall back to __typeof__ for shapes the type-checker
        // couldn't render (gcc/clang only).
        std::string rhs = emit_expr_to_string(l->value);
        out() << "__typeof__((" << rhs << ")) " << sb->name
              << " = (" << rhs << ");\n";
        return;
    }
    out() << ty << " " << sb->name << " = ";
    emit_expr(l->value);
    out() << ";\n";
}

void CBackend::emit_let_destructure(DdcgAst::LetStmt* l) {
    // Tuple-destructuring: bind a temp tuple, copy each component
    // out by index. Element types come from the type-checker.
    auto vit = check_->expr_types.find(l->value);
    if (vit == check_->expr_types.end() ||
        vit->second.kind != TypeKind::Tuple) {
        errors_->push_back("destructuring let: RHS isn't tuple-typed");
        indent() << "/*BUG: destructuring let on non-tuple*/\n";
        return;
    }
    std::vector<std::string> shape;
    for (const auto& te : vit->second.elems) shape.push_back(c_for_type(te));
    std::string tup_t = tuple_type_name(shape);
    std::string tmp = "_tup_" + std::to_string((uintptr_t)l % 100000);
    indent() << tup_t << " " << tmp << " = ";
    emit_expr(l->value);
    out() << ";\n";
    for (size_t i = 0; i < l->binds.size(); ++i) {
        std::string lhs_ty = i < shape.size() ? shape[i] : std::string("int");
        if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(l->binds[i])) {
            indent() << lhs_ty << " " << sb->name << " = "
                     << tmp << "._" << i << ";\n";
        }
        // WildcardBind: drop the component.
    }
}

void CBackend::emit_for_stmt(DdcgAst::ForStmt* f) {
    // Iterate a list/sequence-bound-as-list. The seq's inferred
    // type gives the element type, so we know how to spell the bind.
    auto sit = check_->expr_types.find(f->seq);
    if (sit == check_->expr_types.end() ||
        sit->second.kind != TypeKind::List ||
        sit->second.elems.empty()) {
        errors_->push_back("for: iterand is not list-typed");
        indent() << "/*BUG: for-stmt over non-list*/\n";
        return;
    }
    std::string elem_ty = c_for_type(sit->second.elems[0]);
    std::string seq_str = emit_expr_to_string(f->seq);
    std::string idx = "_i_" + std::to_string((uintptr_t)f % 100000);
    indent() << "for (int " << idx << " = 0; " << idx
             << " < (" << seq_str << ").count; ++" << idx << ") {\n";
    indent_depth_++;
    if (f->iter_index.has_value()) {
        if (auto* ib = dynamic_cast<DdcgAst::SimpleBind*>(*f->iter_index)) {
            indent() << "int " << ib->name << " = " << idx << ";\n";
        }
    }
    if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(f->iter)) {
        indent() << elem_ty << " " << sb->name
                 << " = (" << seq_str << ").data[" << idx << "];\n";
    }
    emit_stmts(f->body, /*tail=*/false);
    indent_depth_--;
    indent() << "}\n";
}

void CBackend::emit_dest_pattern(DdcgAst::DestPattern* dp) {
    /* C-side dest constructor — `ctx` is prepended so the user
     * runtime has access to compiler state (arena, etc.) wherever
     * a constructor is called from a rule body. Recursive variants
     * use it for arena-allocating their parent indirection; non-
     * recursive constructors get an unused ctx arg. */
    out() << dp->name << "(ctx";
    for (auto* a : dp->args) {
        out() << ", ";
        emit_expr(a);
    }
    out() << ")";
}

void CBackend::emit_label_stmt(DdcgAst::LabelStmt* lb) {
    // Consumer must provide <prefix>_fresh_label(ctx). The forward
    // decl is emitted by emit_aux_forward_decls.
    indent() << "label_t " << lb->name << " = "
             << compiler_prefix() << "_fresh_label(ctx);\n";
}

// ── Pattern matching ──────────────────────────────────────────

std::string CBackend::field_check_expr(DdcgAst::Pattern* sub,
                                        const std::string& field_expr,
                                        const Field* field) {
    (void)field;
    if (auto* il = dynamic_cast<DdcgAst::IntPat*>(sub)) {
        return field_expr + " == " + std::to_string(il->value);
    }
    if (auto* sl = dynamic_cast<DdcgAst::StringPat*>(sub)) {
        std::string lit;
        for (char ch : sl->value) {
            switch (ch) {
                case '\\': lit += "\\\\"; break;
                case '"':  lit += "\\\""; break;
                case '\n': lit += "\\n";  break;
                case '\r': lit += "\\r";  break;
                case '\t': lit += "\\t";  break;
                default:   lit += ch;     break;
            }
        }
        // C has no operator== on strings; use strcmp via <string.h>.
        return "(strcmp(" + field_expr + ", \"" + lit + "\") == 0)";
    }
    if (dynamic_cast<DdcgAst::NilPat*>(sub)) {
        return field_expr + " == NULL";
    }
    return {};
}

int CBackend::emit_match(const Schema& schema,
                          DdcgAst::ConstructorPat* cp,
                          const std::string& value_expr,
                          int& name_ctr) {
    int blocks_open = 0;
    auto ci = schema.constructors.find(cp->ctor);
    if (ci == schema.constructors.end()) return blocks_open;

    // For nested patterns we open a real `if` (asdl-c has no
    // dynamic_cast — the discriminator IS the tag). At the top level
    // (called from emit_dispatcher), the outer switch already gated
    // on the tag, so emit only the field binds + leaf checks; detect
    // that by `value_expr == "node"` + cp->ctor matches the case we're
    // in (caller responsibility — we don't re-check here).
    bool is_top = (value_expr == "node");

    std::string nv;
    if (is_top) {
        // No fresh local; field accesses go directly off `node`.
        nv = "node";
    } else {
        nv = "_n" + std::to_string(name_ctr++);
        std::string sum_t = c_type_for(schema, ci->second.sum_name);
        indent() << sum_t << " " << nv << " = " << value_expr << ";\n";
        indent() << "if (" << nv << " != NULL && " << nv << "->tag == "
                 << c_tag_value(schema.module_name, cp->ctor) << ") {\n";
        indent_depth_++;
        blocks_open++;
    }

    std::string variant = c_variant_arm(cp->ctor);

    std::vector<std::string> leaf_checks;
    std::vector<std::pair<DdcgAst::ConstructorPat*, std::string>> nested;

    for (auto* fp : cp->fields) {
        if (auto* nf = dynamic_cast<DdcgAst::NamedFieldPat*>(fp)) {
            std::string field_expr = nv + "->" + variant + "." +
                                      safe_field_name(nf->name);
            const Field* matched = nullptr;
            for (const auto& f : ci->second.fields) {
                if (f.name == nf->name) { matched = &f; break; }
            }
            if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(nf->pat)) {
                bool is_enum_value = false;
                if (matched && schema.enum_sums.count(matched->type_name)) {
                    auto eit = schema.sum_constructors.find(matched->type_name);
                    if (eit != schema.sum_constructors.end()) {
                        for (const auto& cn : eit->second) {
                            if (cn == bp->name) {
                                leaf_checks.push_back(
                                    field_expr + " == " +
                                    c_tag_value(schema.module_name, bp->name));
                                is_enum_value = true;
                                break;
                            }
                        }
                    }
                }
                if (!is_enum_value) {
                    std::string ty = matched ? c_for_field(schema, *matched)
                                              : std::string("int");
                    if (matched && matched->flag == FieldFlag::Sequence) {
                        // asdl-c stores sequences as (T* arr; int arr_count).
                        // Bind the pair into a list helper so the body can
                        // iterate / concat it like any other list.
                        std::string elem_c = c_for_field_elem(schema, *matched);
                        std::string fn = compiler_prefix() + "_list_" +
                                          sanitize_for_suffix(elem_c) + "_from_arr";
                        indent() << ty << " " << bp->name << " = " << fn
                                 << "(" << field_expr << ", "
                                 << field_expr << "_count);\n";
                    } else {
                        indent() << ty << " " << bp->name << " = "
                                 << field_expr << ";\n";
                    }
                    indent() << "(void)" << bp->name << ";\n";
                }
            } else if (auto* sub_cp =
                           dynamic_cast<DdcgAst::ConstructorPat*>(nf->pat)) {
                nested.emplace_back(sub_cp, field_expr);
            } else {
                std::string ck = field_check_expr(nf->pat, field_expr, matched);
                if (!ck.empty()) leaf_checks.push_back(std::move(ck));
            }
        } else if (auto* rf = dynamic_cast<DdcgAst::RestFieldPat*>(fp)) {
            // RestFieldPat (`...name`) binds the whole field by name.
            // The grammar permits any field but the convention is
            // sequence-typed; for sequence fields we emit the same
            // _from_arr(arr, count) bind that NamedFieldPat+BindPat
            // uses on a sequence field. For non-sequence fields, fall
            // back to a direct bind.
            const Field* matched = nullptr;
            for (const auto& f : ci->second.fields) {
                if (f.name == rf->name) { matched = &f; break; }
            }
            std::string field_expr = nv + "->" + variant + "." +
                                      safe_field_name(rf->name);
            if (matched && matched->flag == FieldFlag::Sequence) {
                std::string elem_c = c_for_field_elem(schema, *matched);
                std::string fn = compiler_prefix() + "_list_" +
                                  sanitize_for_suffix(elem_c) + "_from_arr";
                indent() << c_for_field(schema, *matched) << " "
                         << rf->name << " = " << fn << "("
                         << field_expr << ", " << field_expr
                         << "_count);\n";
            } else {
                std::string ty = matched ? c_for_field(schema, *matched)
                                          : std::string("int");
                indent() << ty << " " << rf->name << " = "
                         << field_expr << ";\n";
            }
            indent() << "(void)" << rf->name << ";\n";
        }
    }

    if (!leaf_checks.empty()) {
        std::string combined;
        for (size_t i = 0; i < leaf_checks.size(); ++i) {
            if (i) combined += " && ";
            combined += "(" + leaf_checks[i] + ")";
        }
        indent() << "if (" << combined << ") {\n";
        indent_depth_++;
        blocks_open++;
    }

    for (auto& [sub_cp, sub_expr] : nested) {
        if (sub_cp->module != cp->module) continue;
        blocks_open += emit_match(schema, sub_cp, sub_expr, name_ctr);
    }

    return blocks_open;
}

// ── Action-language funs ──────────────────────────────────────

void CBackend::emit_fun_signature(DdcgAst::Fun* f) {
    out() << c_for_typeref(f->ret_ty) << " "
          << compiler_prefix() << "_" << f->name
          << "(" << ctx_type() << "* ctx";
    for (size_t i = 0; i < f->params.size(); ++i) {
        out() << ", " << c_for_typeref(f->params[i]->ty)
              << " " << f->params[i]->name;
    }
    out() << ")";
}

void CBackend::emit_fun(DdcgAst::Fun* f) {
    out() << "\n";
    indent() << "/* fun " << f->name << " */\n";
    indent();
    emit_fun_signature(f);
    out() << " {\n";
    indent_depth_++;
    indent() << "(void)ctx;\n";
    // Suppress unused-parameter warnings: every fun param gets a (void)
    // cast at the top, since DSL bodies may legitimately ignore some.
    for (auto* p : f->params) {
        indent() << "(void)" << p->name << ";\n";
    }
    for (size_t i = 0; i < f->body.size(); ++i) {
        bool is_last = (i + 1 == f->body.size());
        emit_stmt(f->body[i], is_last);
    }
    indent_depth_--;
    indent() << "}\n";
}

// ── File-level emission ──────────────────────────────────────

void CBackend::emit_file_preamble() {
    out() << "/* Generated by ddcgc — do not edit */\n";
    out() << "#pragma once\n";
    out() << "/*\n";
    out() << " * Shape: free functions + a `" << ctx_type() << "` struct.\n";
    out() << " * MEMBERS hold consumer state (data fields ONLY — function defs\n";
    out() << " * fail the C compiler). Aux/pred functions are caller-supplied\n";
    out() << " * with `" << ctx_type() << "*` first parameter; ddcgc emits\n";
    out() << " * forward decls. Dispatchers and funs are defined in the\n";
    out() << " * companion .c file; this header has only declarations.\n";
    out() << " */\n\n";

    for (auto* h : file_->headers) {
        out() << h->code << "\n";
    }
    out() << "#include <stdint.h>\n";
    out() << "#include <stddef.h>\n";
    out() << "#include <stdbool.h>\n";
    out() << "#include <string.h>\n\n";
}

void CBackend::emit_source_preamble() {
    out() << "/* Generated by ddcgc — do not edit */\n";
    out() << "#include \"" << header_filename_ << "\"\n";
    out() << "#include <stdlib.h>\n";
    out() << "#include <string.h>\n\n";
}

void CBackend::emit_fun_forward_decls() {
    if (file_->funs.empty()) return;
    out() << "/* ─── fun forward decls (definitions in .c) ─── */\n";
    for (auto* f : file_->funs) {
        emit_fun_signature(f);
        out() << ";\n";
    }
    out() << "\n";
}

void CBackend::emit_dispatcher_forward_decls() {
    auto sums = source_sums_with_rules();
    if (sums.empty()) return;
    out() << "/* ─── Dispatcher forward decls (definitions in .c) ─── */\n";
    for (const auto& s : sums) {
        emit_dispatcher_signature(s);
        out() << ";\n";
    }
    out() << "\n";
}

void CBackend::emit_ctx_struct() {
    out() << "/* ─── Compiler context (MEMBERS verbatim) ─── */\n";
    out() << "typedef struct " << compiler_prefix() << "_ctx {\n";
    if (!file_->members.empty()) {
        out() << file_->members;
        if (file_->members.back() != '\n') out() << "\n";
    }
    out() << "} " << ctx_type() << ";\n\n";
}

void CBackend::emit_private_block() {
    if (file_->private_helpers.empty()) return;
    out() << "/* ─── PRIVATE (file-scope helpers) ─── */\n";
    out() << file_->private_helpers;
    if (file_->private_helpers.back() != '\n') out() << "\n";
    out() << "\n";
}

void CBackend::emit_aux_forward_decls() {
    out() << "/* ─── Auxiliaries / predicates (caller supplies) ─── */\n";
    // Always-on: the DSL `label NAME;` statement lowers to a call to
    // <prefix>_fresh_label(ctx). Forward-declare it here so consumers
    // that use labels can supply the body without a separate header.
    out() << "label_t " << compiler_prefix() << "_fresh_label("
          << ctx_type() << "* ctx);\n";
    for (auto* a : file_->auxiliaries) {
        out() << c_for_typeref(a->return_ty) << " "
              << compiler_prefix() << "_" << a->name
              << "(" << ctx_type() << "* ctx";
        for (size_t i = 0; i < a->param_types.size(); ++i) {
            out() << ", " << c_for_typeref(a->param_types[i])
                  << " _arg" << i;
        }
        out() << ");\n";
    }
    for (auto* p : file_->predicates) {
        out() << "bool " << compiler_prefix() << "_" << p->name
              << "(" << ctx_type() << "* ctx";
        for (size_t i = 0; i < p->param_types.size(); ++i) {
            out() << ", " << c_for_typeref(p->param_types[i])
                  << " _arg" << i;
        }
        out() << ");\n";
    }
    out() << "\n";
}

void CBackend::emit_dispatcher_signature(const std::string& sum_name) {
    std::string sum_t = c_type_for(*source_schema_, sum_name);
    out() << ir_root_type_ << " " << compiler_prefix() << "_compile_"
          << sum_name << "(" << ctx_type() << "* ctx, " << sum_t << " node";
    if (!env_dest_name_.empty()) {
        out() << ", " << env_dest_type_ << " " << env_dest_name_;
    }
    out() << ", " << data_dest_type_ << " " << data_dest_name_
          << ", " << ctrl_dest_type_ << " " << ctrl_dest_name_
          << ", " << ir_root_type_ << " Lnext)";
}

void CBackend::emit_dispatcher(const std::string& sum_name) {
    auto rules = rules_for(sum_name);
    if (rules.empty()) return;

    out() << "\n";
    indent() << "/* Dispatcher for " << source_import_ << "."
             << sum_name << " */\n";
    indent();
    emit_dispatcher_signature(sum_name);
    out() << " {\n";
    indent_depth_++;
    indent() << "(void)" << data_dest_name_ << "; "
             << "(void)" << ctrl_dest_name_ << "; "
             << "(void)Lnext;\n";
    if (!env_dest_name_.empty()) {
        indent() << "(void)" << env_dest_name_ << ";\n";
    }
    indent() << "ctx->current_loc = node->loc;\n";

    // Group rules by constructor (= tag), preserving source order of
    // first appearance. Each case dispatches all rules for its tag in
    // the order they were declared.
    std::map<std::string, std::vector<DdcgAst::Rule*>> by_ctor;
    std::vector<std::string> ctor_order;
    for (auto* r : rules) {
        for (auto* h : r->heads) {
            auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h);
            if (!cp) continue;
            if (cp->module != source_import_) continue;
            auto it = source_schema_->constructors.find(cp->ctor);
            if (it == source_schema_->constructors.end()) continue;
            if (it->second.sum_name != sum_name) continue;
            if (by_ctor.find(cp->ctor) == by_ctor.end()) {
                ctor_order.push_back(cp->ctor);
            }
            by_ctor[cp->ctor].push_back(r);
        }
    }

    indent() << "switch (node->tag) {\n";
    for (const auto& ctor : ctor_order) {
        indent() << "case "
                 << c_tag_value(source_schema_->module_name, ctor)
                 << ": {\n";
        indent_depth_++;
        for (auto* r : by_ctor[ctor]) {
            for (auto* h : r->heads) {
                auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h);
                if (!cp || cp->ctor != ctor) continue;
                auto it = source_schema_->constructors.find(cp->ctor);
                if (it == source_schema_->constructors.end()) continue;
                // Each rule's binds + body live in their own block —
                // multiple rules under one tag would otherwise re-
                // declare the same field names (e.g. `r` in both
                // `add_zero_l` and `add` rules under Add).
                indent() << "{\n";
                indent_depth_++;
                emit_rule_branch(*source_schema_, it->second, h, r);
                indent_depth_--;
                indent() << "}\n";
                break;
            }
        }
        indent() << "break;\n";
        indent_depth_--;
        indent() << "}\n";
    }
    indent() << "default: break;\n";
    indent() << "}\n";
    indent() << "return " << c_zero_for(ir_root_type_) << ";\n";
    indent_depth_--;
    indent() << "}\n";
}

bool CBackend::emit_body() {
    if (!out_source_) {
        errors_->push_back("C backend requires a separate source-output stream "
                           "(use main's two-file -o convention)");
        return false;
    }

    // Walk the file once up-front so the helper block can emit every
    // list/tuple typedef + helper bundle BEFORE anything references
    // them. Aux/pred fwd decls and the ctx struct may both mention
    // these types via signature TypeRefs.
    run_discovery();

    auto sums = source_sums_with_rules();
    if (sums.empty()) {
        errors_->push_back("no rules dispatch on the source AST schema");
        return false;
    }

    // ── HEADER ─────────────────────────────────────────────────
    out_ = out_header_;
    emit_file_preamble();
    emit_helpers_block();
    emit_ctx_struct();
    emit_private_block();
    emit_aux_forward_decls();
    emit_fun_forward_decls();
    emit_dispatcher_forward_decls();

    // ── SOURCE ─────────────────────────────────────────────────
    out_ = out_source_;
    emit_source_preamble();

    if (!file_->funs.empty()) {
        out() << "/* ─── action-language funs ─── */\n";
        for (auto* f : file_->funs) emit_fun(f);
        out() << "\n";
    }

    out() << "/* ─── Dispatchers ─── */\n";
    for (const auto& s : sums) emit_dispatcher(s);

    return true;
}

} // anonymous namespace

bool emit_c(const DdcgAst::File* file,
            const std::map<std::string, Schema>& schemas,
            const CheckResult& check,
            std::ostream& out_header,
            std::ostream* out_source,
            const std::string& header_filename,
            std::vector<std::string>& errors) {
    CBackend backend;
    return backend.emit(file, schemas, check,
                        out_header, out_source, header_filename, errors);
}

std::unique_ptr<Backend> create_c_backend() {
    return std::make_unique<CBackend>();
}

} // namespace ddcgc
