// ddcgc — C++ emit backend.
//
// Subclasses EmitBackend (Template Method base). Inherits language-
// agnostic state + helpers (sum_for_subject, rules_for, etc.) and
// overrides the syntax-emitting hooks. The bulk of the file is the
// per-form walks (emit_expr / emit_stmt / emit_match / emit_dispatcher)
// that produce C++ source from the .ddcg AST.
//
// Future commits may lift more of these walks into EmitBackend with
// language-specific bits exposed as virtual hooks; for now they live
// here as private members of CppBackend.

#include "cpp_backend.h"
#include "emit_backend.h"

#include <cctype>
#include <set>

namespace ddcgc {

namespace {

// asdl emitters escape C++ reserved-word field names by appending '_'
// (e.g. `default` → `default_`). ddcgc's generated field accesses must
// match the asdl-emitted AST class layout, so apply the same escaping
// when emitting `node->fieldname`. Keep this list in sync with
// asdl/src/string_utils.cpp's reserved_keywords.
bool is_cpp_keyword(const std::string& s) {
    static const std::set<std::string> kw = {
        "alignas","alignof","and","and_eq","asm","auto","bitand","bitor",
        "bool","break","case","catch","char","char8_t","char16_t","char32_t",
        "class","compl","concept","const","consteval","constexpr","const_cast",
        "continue","co_await","co_return","co_yield","decltype","default",
        "delete","do","double","dynamic_cast","else","enum","explicit","export",
        "extern","false","float","for","friend","goto","if","inline","int",
        "long","mutable","namespace","new","noexcept","not","not_eq","nullptr",
        "operator","or","or_eq","private","protected","public","register",
        "reinterpret_cast","requires","return","short","signed","sizeof",
        "static","static_assert","static_cast","struct","switch","template",
        "this","thread_local","throw","true","try","typedef","typeid",
        "typename","union","unsigned","using","virtual","void","volatile",
        "wchar_t","while","xor","xor_eq"
    };
    return kw.count(s) > 0;
}

std::string safe_field_name(const std::string& name) {
    return is_cpp_keyword(name) ? name + "_" : name;
}

// Translate an asdl schema type name to its C++ form.
// Sums and constructors become `<module>::<Name>*` (always namespace-
// qualified, so the generated dispatcher works regardless of which
// `namespace` it ends up living in — and so AST and IR types from
// different schemas can't collide on bare names like `SlotRef`).
// Primitives map to canonical C++ types; aliases (e.g. `identifier`
// → `std::string`) pass through.
std::string cpp_type_for_impl(const Schema& schema,
                              const std::string& schema_type) {
    if (auto a = schema.aliases.find(schema_type); a != schema.aliases.end()) {
        return a->second;
    }
    if (schema.sum_names.count(schema_type) ||
        schema.constructors.count(schema_type)) {
        // Products keep their snake_case type names — asdl emits them
        // as `struct switch_case { ... }` not `struct SwitchCase`.
        // Sums and their constructors are camelcased per asdl convention.
        std::string r;
        if (schema.product_names.count(schema_type)) {
            r = schema_type;
        } else {
            bool upper = true;
            for (char c : schema_type) {
                if (c == '_') { upper = true; continue; }
                r += upper ? (char)std::toupper(c) : c;
                upper = false;
            }
        }
        if (!schema.module_name.empty()) {
            r = schema.module_name + "::" + r;
        }
        // asdl enum sums are emitted as `enum class` values, not pointers.
        if (schema.enum_sums.count(schema_type)) return r;
        // Products are by-value structs in BBQ's asdl conventions; sum
        // constructors are heap-allocated, so they keep the trailing '*'.
        if (schema.product_names.count(schema_type)) return r;
        return r + "*";
    }
    if (schema_type == "string")     return "std::string";
    if (schema_type == "int")        return "int";
    if (schema_type == "int64")      return "int64_t";
    if (schema_type == "bool")       return "bool";
    if (schema_type == "identifier") return "std::string";
    return schema_type;
}

// Same as cpp_type_for_impl but always returns the bare class name (drops
// the trailing '*' if present). Used for `<Name>* node` parameter forms.
// The result is still namespace-qualified for schema types.
std::string cpp_class_name_impl(const Schema& schema,
                                const std::string& sum_or_ctor) {
    std::string t = cpp_type_for_impl(schema, sum_or_ctor);
    if (!t.empty() && t.back() == '*') t.pop_back();
    return t;
}

class CppBackend : public EmitBackend {
public:
    bool emit_body() override;

protected:
    std::string type_for_schema(const Schema& schema,
                                const std::string& name) override {
        return cpp_type_for_impl(schema, name);
    }

    // Per-form hooks — language-specific rule-body emission.
    std::string null_literal() const override { return "nullptr"; }
    std::string emit_enum_value_ref(const std::string& module,
                                    const std::string& sum,
                                    const std::string& ctor) const override {
        // <module>::<CamelSum>::<ctor>. Sum gets PascalCase; ctor
        // is already in source form (asdl ctors are PascalCase by
        // convention, no further transformation needed).
        std::string camel_sum;
        bool up = true;
        for (char ch : sum) {
            if (ch == '_') { up = true; continue; }
            camel_sum += up ? (char)std::toupper((unsigned char)ch) : ch;
            up = false;
        }
        return module + "::" + camel_sum + "::" + ctor;
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
    // Type-rendering wrappers — convenience for in-class call sites.
    std::string cpp_type_for(const Schema& s, const std::string& n) {
        return cpp_type_for_impl(s, n);
    }
    std::string cpp_class_name(const Schema& s, const std::string& n) {
        return cpp_class_name_impl(s, n);
    }
    std::string cpp_for_type(const Type& t);
    std::string cpp_for_typeref(DdcgAst::TypeRef* tr);
    std::string ternary_branch_cast(const Type& t) override {
        return cpp_for_type(t);
    }

    std::string let_decl_for(DdcgAst::Expr* rhs);
    std::string field_check_expr(DdcgAst::Pattern* sub,
                                  const std::string& field_expr,
                                  const Field* field);
    void emit_fun(DdcgAst::Fun* f);
    void emit_dispatcher_signature(const std::string& sum_name);
    void emit_dispatcher(const std::string& sum_name);
};

// ── Type translation ──────────────────────────────────────────

// Translate a Type (from the type-checker) to a C++ type expression
// suitable for declarations. Falls back to `auto` for things we
// can't render cleanly (e.g. truly Unknown).
std::string CppBackend::cpp_for_type(const Type& t) {
    switch (t.kind) {
        case TypeKind::Int:    return "int";
        case TypeKind::String: return "std::string";
        case TypeKind::Bool:   return "bool";
        case TypeKind::Ident:  return "std::string";
        case TypeKind::Label:  return "label_t";
        case TypeKind::Void:   return "void";
        case TypeKind::Schema: {
            auto sit = schemas_->find(t.schema_import);
            if (sit == schemas_->end()) return "auto";
            return cpp_type_for(sit->second, t.name);
        }
        case TypeKind::Dest:   return t.name + "_t";
        case TypeKind::List: {
            if (t.elems.empty()) return "auto";
            return "std::vector<" + cpp_for_type(t.elems[0]) + ">";
        }
        case TypeKind::Tuple: {
            std::string r = "std::tuple<";
            for (size_t i = 0; i < t.elems.size(); ++i) {
                if (i) r += ", ";
                r += cpp_for_type(t.elems[i]);
            }
            return r + ">";
        }
        case TypeKind::NilLit:
        case TypeKind::Unknown:
            return "auto";
    }
    return "auto";
}

// Lower a .ddcg TypeRef (parsed surface syntax) to a C++ type string.
// Mirrors typecheck.cpp's `type_from_dsl_typeref` but emits text.
// Used for fun parameters and return types.
std::string CppBackend::cpp_for_typeref(DdcgAst::TypeRef* tr) {
    if (auto* tn = dynamic_cast<DdcgAst::TypeName*>(tr)) {
        const std::string& n = tn->name;

        // Module-qualified schema reference.
        if (!tn->module.empty()) {
            auto sit = schemas_->find(tn->module);
            if (sit != schemas_->end()) return cpp_type_for(sit->second, n);
            return tn->module + "::" + n;
        }

        // `ir_root` placeholder — resolves to whatever the importing
        // file declared as its ir_root. Shared libraries (dybvig.ddcg)
        // use this so they don't have to know the consumer's IR's
        // module name or root sum name.
        if (n == "ir_root") {
            if (!ir_root_type_.empty()) return ir_root_type_;
            return "/*BUG: ir_root used before resolution*/auto";
        }

        if (n == "int")                          return "int";
        if (n == "int64")                        return "int64_t";
        if (n == "string")                       return "std::string";
        if (n == "bool")                         return "bool";
        if (n == "ident" || n == "identifier")   return "std::string";
        if (n == "label")                        return "label_t";
        if (n == "void")                         return "void";
        // Destination or user sum type → <name>_t (data_dest, ctrl_dest,
        // env_dest, or sum decl).
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
        // Schema sum/ctor — search loaded schemas. Multiple schemas
        // matching is ambiguous; emit a sentinel that surfaces as a
        // C++ compile error (better than silent first-wins).
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
        if (hit) return cpp_type_for(*hit, n);
        return n;  // opaque runtime-supplied
    }
    if (auto* tl = dynamic_cast<DdcgAst::TypeList*>(tr)) {
        return "std::vector<" + cpp_for_typeref(tl->elem) + ">";
    }
    if (auto* tt = dynamic_cast<DdcgAst::TypeTuple*>(tr)) {
        std::string r = "std::tuple<";
        for (size_t i = 0; i < tt->elems.size(); ++i) {
            if (i) r += ", ";
            r += cpp_for_typeref(tt->elems[i]);
        }
        return r + ">";
    }
    return "auto";
}

// ── Per-form hooks (called by EmitBackend's shared dispatch) ──

// Aux/pred/fun calls in C++ are bare method calls on `this` — no
// mangling, no ctx threading. The MEMBERS block holds the bodies
// inline; calling `cg_lit(x)` inside the dispatcher class scope
// resolves to `this->cg_lit(x)` with no further work.
void CppBackend::emit_call(DdcgAst::CallExpr* call) {
    emit_expr(call->fn);
    emit_call_args(call->args);
}

void CppBackend::emit_list_concat(DdcgAst::Expr* /*parent*/,
                                   DdcgAst::Expr* left,
                                   DdcgAst::Expr* right) {
    // std::vector has no operator+; lower to the emitted helper.
    out() << "_ddcgc_concat(";
    emit_expr(left);
    out() << ", ";
    emit_expr(right);
    out() << ")";
}

void CppBackend::emit_list_lit(DdcgAst::ListLit* l) {
    // Render with explicit element type when the type-checker has
    // one; this also makes empty-list literals well-typed (CTAD
    // can't deduce element type from `std::vector{}`).
    std::string elem_cpp;
    auto it = check_->expr_types.find(l);
    if (it != check_->expr_types.end() &&
        it->second.kind == TypeKind::List &&
        !it->second.elems.empty()) {
        elem_cpp = cpp_for_type(it->second.elems[0]);
    }
    if (!elem_cpp.empty() && elem_cpp != "auto") {
        out() << "std::vector<" << elem_cpp << ">{";
    } else {
        out() << "std::vector{";
    }
    for (size_t i = 0; i < l->elems.size(); ++i) {
        if (i > 0) out() << ", ";
        emit_expr(l->elems[i]);
    }
    out() << "}";
}

void CppBackend::emit_tuple_lit(DdcgAst::TupleLit* tu) {
    out() << "std::make_tuple(";
    for (size_t i = 0; i < tu->elems.size(); ++i) {
        if (i > 0) out() << ", ";
        emit_expr(tu->elems[i]);
    }
    out() << ")";
}

void CppBackend::emit_with_expr(DdcgAst::WithExpr* w) {
    // GNU statement-expression copy-then-mutate.
    out() << "({ auto _w = (";
    emit_expr(w->base);
    out() << "); _w." << w->field << " = (";
    emit_expr(w->value);
    out() << "); _w; })";
}

void CppBackend::emit_build(DdcgAst::BuildExpr* b) {
    auto sit = schemas_->find(b->schema);
    if (sit == schemas_->end()) {
        errors_->push_back("build: unknown schema '" + b->schema +
                            "' (typecheck should have caught this)");
        out() << "/*BUG: unknown schema*/(void)0";
        return;
    }
    // Resolve the target ctor's field list so we can specialise
    // arg emission per field shape (e.g. nil → std::nullopt for an
    // Optional schema field, not nullptr — the latter would build
    // a present-but-null std::optional<T*>, causing has_value() to
    // return true and crashing downstream consumers).
    auto ci = sit->second.constructors.find(b->ctor);
    out() << "new " << cpp_class_name(sit->second, b->ctor) << "(";
    for (size_t i = 0; i < b->args.size(); ++i) {
        if (i) out() << ", ";
        bool emitted = false;
        if (ci != sit->second.constructors.end() &&
            i < ci->second.fields.size() &&
            ci->second.fields[i].flag == FieldFlag::Optional &&
            dynamic_cast<DdcgAst::NilLit*>(b->args[i])) {
            out() << "std::nullopt";
            emitted = true;
        }
        if (!emitted) emit_expr(b->args[i]);
    }
    out() << ")";
}

void CppBackend::emit_match_expr(DdcgAst::MatchExpr* m) {
    // Match-as-expression is an IIFE lambda whose arms `return` the
    // matched value. The lambda captures by reference so consumer
    // state in MEMBERS stays accessible.
    auto camel = [](const std::string& s) {
        std::string r; bool up = true;
        for (char ch : s) {
            if (ch == '_') { up = true; continue; }
            r += up ? (char)std::toupper(ch) : ch;
            up = false;
        }
        return r;
    };

    // Subject is a dest tagged-union (switch on .tag), an asdl enum
    // sum (switch on the value directly), or a plain int (switch on
    // the value).
    std::string dest_name;
    std::string enum_cpp;
    const Schema* enum_schema = nullptr;
    std::string enum_sum_name;
    bool int_subject = false;
    // Non-enum schema sum (e.g. asdl `type_body = Product(...) | Sum(...)`)
    // — emit a dynamic_cast chain instead of a switch.
    const Schema* schema_subject = nullptr;
    std::string schema_sum_name;
    auto it = check_->expr_types.find(m->subject);
    if (it != check_->expr_types.end()) {
        const Type& st = it->second;
        if (st.kind == TypeKind::Dest) {
            dest_name = st.name;
        } else if (st.kind == TypeKind::Schema) {
            auto sit = schemas_->find(st.schema_import);
            if (sit != schemas_->end()) {
                if (sit->second.enum_sums.count(st.name)) {
                    enum_schema = &sit->second;
                    enum_sum_name = st.name;
                    enum_cpp = cpp_class_name(*enum_schema, st.name);
                } else if (sit->second.sum_names.count(st.name)) {
                    schema_subject = &sit->second;
                    schema_sum_name = st.name;
                }
            }
        } else if (st.kind == TypeKind::Int) {
            int_subject = true;
        }
    }
    if (dest_name.empty() && enum_cpp.empty() && !int_subject &&
        !schema_subject) {
        errors_->push_back("match: subject is not a dest tagged union, "
                            "asdl enum, schema sum, or int");
        out() << "({})";
        return;
    }
    // Schema-sum subject: emit `[&]() -> ret { auto _m = subj; if (auto* _n = dynamic_cast<Ctor*>(_m)) {...} ... return {}; }()`.
    if (schema_subject) {
        std::string match_ret = "auto";
        auto rti = check_->expr_types.find(m);
        if (rti != check_->expr_types.end()) {
            match_ret = cpp_for_type(rti->second);
        }
        out() << "([&]() -> " << match_ret << " {\n";
        indent_depth_++;
        indent() << "auto _m = ";
        emit_expr(m->subject);
        out() << ";\n";
        int name_ctr = 0;
        for (auto* arm : m->arms) {
            auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(arm->pat);
            if (!cp || cp->module.empty()) {
                errors_->push_back(
                    "match on schema sum: arm must be `module.Ctor(...)`");
                continue;
            }
            int blocks_open = emit_match(*schema_subject, cp, "_m", name_ctr);
            emit_stmts(arm->body, /*tail=*/true);
            while (blocks_open--) {
                indent_depth_--;
                indent() << "}\n";
            }
        }
        indent() << "return {};\n";
        indent_depth_--;
        indent() << "})()";
        return;
    }
    std::string tag_class = camel(dest_name) + "Tag";

    std::string match_ret = "auto";
    auto rti = check_->expr_types.find(m);
    if (rti != check_->expr_types.end()) {
        match_ret = cpp_for_type(rti->second);
    }
    out() << "([&]() -> " << match_ret << " {\n";
    indent_depth_++;
    indent() << "auto _m = ";
    emit_expr(m->subject);
    out() << ";\n";
    if (!enum_cpp.empty() || int_subject) {
        indent() << "switch (_m) {\n";
    } else {
        indent() << "switch (_m.tag) {\n";
    }
    auto is_zero_arg_variant_name = [this, &dest_name](const std::string& nm) {
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
            for (auto* v : vs) {
                if (v->name == nm && v->fields.empty()) return true;
            }
        }
        return false;
    };
    auto is_enum_value_name = [&enum_schema, &enum_sum_name](const std::string& nm) {
        if (!enum_schema) return false;
        auto eit = enum_schema->sum_constructors.find(enum_sum_name);
        if (eit == enum_schema->sum_constructors.end()) return false;
        for (const auto& cn : eit->second) if (cn == nm) return true;
        return false;
    };

    for (auto* arm : m->arms) {
        indent();
        if (auto* ip = dynamic_cast<DdcgAst::IntPat*>(arm->pat)) {
            out() << "case " << ip->value << ": {\n";
            indent_depth_++;
        } else if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(arm->pat);
            cp && cp->module.empty()) {
            out() << "case " << tag_class << "::" << camel(cp->ctor) << ": {\n";
            indent_depth_++;
            // Look up the variant to identify recursive fields — fields
            // whose declared type is the dest type itself (e.g. ρ's
            // `parent: rho` self-reference). Recursive fields are
            // stored as `dest_t*` in the runtime struct (the layout
            // can't recurse by value); ddcgc auto-derefs at bind time
            // so the rule body sees a `dest_t` value matching the
            // DSL-level type. Mirrors the C backend's handling.
            DdcgAst::DestVariant* variant = nullptr;
            for (auto* d : file_->destinations) {
                std::vector<DdcgAst::DestVariant*> vs;
                if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) vs = dd->variants;
                else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) vs = cd->variants;
                else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) vs = ed->variants;
                else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) vs = sd->variants;
                for (auto* v : vs) {
                    if (v->name == cp->ctor) { variant = v; break; }
                }
                if (variant) break;
            }
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
                        const char* deref = recursive ? "*" : "";
                        indent() << "auto " << bp->name
                                      << " = " << deref << "_m." << nf->name << ";\n";
                    }
                }
            }
        } else if (dynamic_cast<DdcgAst::WildcardPat*>(arm->pat)) {
            out() << "default: {\n";
            indent_depth_++;
        } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat);
                   bp && !enum_cpp.empty() && is_enum_value_name(bp->name)) {
            out() << "case " << enum_cpp << "::" << bp->name << ": {\n";
            indent_depth_++;
        } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat);
                   bp && is_zero_arg_variant_name(bp->name)) {
            out() << "case " << tag_class << "::" << camel(bp->name) << ": {\n";
            indent_depth_++;
        } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat)) {
            out() << "default: {\n";
            indent_depth_++;
            indent() << "auto " << bp->name << " = _m;\n";
        } else {
            errors_->push_back("match: unsupported arm pattern shape");
            out() << "default: {\n";
            indent_depth_++;
        }
        // Tail-position ExprStmts emit `return X;` from the lambda.
        // match_target_var_ stays empty for C++ — the lambda return
        // is the natural sink.
        emit_stmts(arm->body, /*tail=*/true);
        indent_depth_--;
        indent() << "}\n";
    }
    indent() << "}\n";
    indent() << "return {};\n";
    indent_depth_--;
    indent() << "})()";
}

void CppBackend::emit_index_acc(DdcgAst::IndexAccExpr* ix) {
    // std::vector overloads operator[].
    emit_expr(ix->base);
    out() << "[";
    emit_expr(ix->index);
    out() << "]";
}

void CppBackend::emit_gen_call(DdcgAst::GenCall* g) {
    std::string sum = sum_for_subject(g->subject);
    if (sum.empty()) {
        errors_->push_back(
            "gen-call subject's source-schema sum could not be "
            "resolved (subject type missing from expr_types). "
            "Most likely the .ddcg has a typecheck error that "
            "prevents inference.");
        out() << "/*BUG: unresolved gen*/(void)";
        emit_expr(g->subject);
        return;
    }
    out() << "compile_" << sum << "(";
    emit_expr(g->subject);
    for (auto* a : g->args) {
        out() << ", ";
        emit_expr(a);
    }
    out() << ")";
}

// ── Statement translation (per-form hooks) ────────────────────

// Pick the C++ declaration for a let-bound name. Use the type-
// checker's recorded type when available; fall back to `auto`.
std::string CppBackend::let_decl_for(DdcgAst::Expr* rhs) {
    auto it = check_->expr_types.find(rhs);
    if (it == check_->expr_types.end()) return "auto";
    std::string t = cpp_for_type(it->second);
    return t.empty() ? "auto" : t;
}

void CppBackend::emit_let_simple(DdcgAst::LetStmt* l, DdcgAst::SimpleBind* sb) {
    indent() << let_decl_for(l->value) << " " << sb->name << " = ";
    emit_expr(l->value);
    out() << ";\n";
}

void CppBackend::emit_let_destructure(DdcgAst::LetStmt* l) {
    // C++17 structured bindings — defer element-type deduction.
    indent() << "auto [";
    for (size_t i = 0; i < l->binds.size(); ++i) {
        if (i > 0) out() << ", ";
        if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(l->binds[i])) {
            out() << sb->name;
        } else {
            out() << "_unused" << i;
        }
    }
    out() << "] = ";
    emit_expr(l->value);
    out() << ";\n";
}

void CppBackend::emit_for_stmt(DdcgAst::ForStmt* f) {
    if (f->iter_index.has_value()) {
        // Indexed C++ for: walk by index so we can bind both i and elem.
        // Uses .size() (works for std::vector / span / array adapters).
        std::string idx = "_i_" + std::to_string((uintptr_t)f % 100000);
        indent() << "for (size_t " << idx << " = 0; " << idx
                 << " < (";
        emit_expr(f->seq);
        out() << ").size(); ++" << idx << ") {\n";
        indent_depth_++;
        if (auto* ib = dynamic_cast<DdcgAst::SimpleBind*>(*f->iter_index)) {
            indent() << "int " << ib->name << " = (int)" << idx << ";\n";
        }
        if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(f->iter)) {
            indent() << "auto& " << sb->name << " = (";
            emit_expr(f->seq);
            out() << ")[" << idx << "];\n";
        }
        emit_stmts(f->body, /*tail=*/false);
        indent_depth_--;
        indent() << "}\n";
        return;
    }
    indent() << "for (auto& ";
    if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(f->iter)) {
        out() << sb->name;
    } else {
        out() << "_unused";
    }
    out() << " : ";
    emit_expr(f->seq);
    out() << ") {\n";
    indent_depth_++;
    emit_stmts(f->body, /*tail=*/false);
    indent_depth_--;
    indent() << "}\n";
}

void CppBackend::emit_dest_pattern(DdcgAst::DestPattern* dp) {
    /* C++ dest constructors are member methods on the Compiler
     * class — `this` carries ctx implicitly, so we emit a bare
     * call. The user's runtime has full access to compiler state
     * via the enclosing method. */
    out() << dp->name;
    emit_call_args(dp->args);
}

void CppBackend::emit_label_stmt(DdcgAst::LabelStmt* lb) {
    indent() << "auto " << lb->name << " = fresh_label();\n";
}

// ── Pattern matching: emit dispatch + body for one rule ───────

// Emit the C++ predicate that decides whether `field_expr` matches
// the leaf sub-pattern `sub`. For BindPat / WildcardPat / RestFieldPat
// / nested ConstructorPat there is no leaf-level check — those produce
// either binds or sub-blocks; both are handled outside this function.
//
// `field` describes the asdl-level cardinality of the field. Optional
// schema-pointer fields lower to `std::optional<Foo*>`, which doesn't
// compare against `nullptr`; NilPat against such fields emits
// `!field.has_value()`.
std::string CppBackend::field_check_expr(DdcgAst::Pattern* sub,
                                          const std::string& field_expr,
                                          const Field* field) {
    bool is_optional_schema = field && field->is_schema &&
                              field->flag == FieldFlag::Optional;
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
        return field_expr + " == \"" + lit + "\"";
    }
    if (dynamic_cast<DdcgAst::NilPat*>(sub)) {
        if (is_optional_schema) return "!" + field_expr + ".has_value()";
        return field_expr + " == nullptr";
    }
    return {};
}

// Recursively match `cp` against the C++ expression `value_expr` (which
// at the call site has the matched constructor's pointer type, but we
// downcast it). Opens nested `if` blocks for dynamic_casts and a single
// combined `if` for leaf value-checks; emits binds inline. `name_ctr`
// supplies fresh `_n0`, `_n1`, … for cast-target names. Returns the
// number of opened blocks the caller must close.
int CppBackend::emit_match(const Schema& schema,
                            DdcgAst::ConstructorPat* cp,
                            const std::string& value_expr,
                            int& name_ctr) {
    int blocks_open = 0;

    // 1. dynamic_cast the value to the constructor type. Always use
    //    the namespace-qualified form (cpp_class_name handles
    //    `<module>::<PascalName>`) so two schemas can't collide on a
    //    bare ctor name like `Add`.
    auto ci = schema.constructors.find(cp->ctor);
    if (ci == schema.constructors.end()) return blocks_open;
    std::string nv = "_n" + std::to_string(name_ctr++);
    indent() << "if (auto* " << nv << " = dynamic_cast<"
                  << cpp_class_name(schema, ci->second.name) << "*>("
                  << value_expr << ")) {\n";
    indent_depth_++;
    blocks_open++;

    // 2. Walk fields. Three categories:
    //    - simple binds (BindPat, RestFieldPat) → emit `auto x = ...;`
    //    - leaf value-checks (Int/String/NilPat) → collect into one combined `if`
    //    - nested ConstructorPat → recurse
    std::vector<std::string> leaf_checks;
    std::vector<std::pair<DdcgAst::ConstructorPat*, std::string>> nested;

    for (auto* fp : cp->fields) {
        if (auto* nf = dynamic_cast<DdcgAst::NamedFieldPat*>(fp)) {
            std::string field_expr = nv + "->" + safe_field_name(nf->name);
            const Field* matched = nullptr;
            for (const auto& f : ci->second.fields) {
                if (f.name == nf->name) { matched = &f; break; }
            }
            bool is_optional_schema = matched && matched->is_schema &&
                                      matched->flag == FieldFlag::Optional;
            if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(nf->pat)) {
                // Enum-value disambiguation: if the field's asdl type is
                // an enum sum and the bind name matches one of its
                // constructors, emit a `field == Module::Type::Value`
                // leaf check rather than binding the field to a name.
                bool is_enum_value = false;
                if (matched && schema.enum_sums.count(matched->type_name)) {
                    auto eit = schema.sum_constructors.find(matched->type_name);
                    if (eit != schema.sum_constructors.end()) {
                        for (const auto& cn : eit->second) {
                            if (cn == bp->name) {
                                std::string enum_cpp =
                                    cpp_class_name(schema, matched->type_name);
                                leaf_checks.push_back(
                                    field_expr + " == " + enum_cpp +
                                    "::" + bp->name);
                                is_enum_value = true;
                                break;
                            }
                        }
                    }
                }
                if (!is_enum_value) {
                    if (is_optional_schema) {
                        // Unwrap std::optional<T*> into a bare nullable T*
                        // so subsequent rule-body uses see a pointer.
                        indent() << "auto " << bp->name << " = "
                                      << field_expr
                                      << ".has_value() ? *" << field_expr
                                      << " : nullptr;\n";
                    } else {
                        indent() << "auto " << bp->name << " = "
                                      << field_expr << ";\n";
                    }
                }
            } else if (auto* sub_cp =
                           dynamic_cast<DdcgAst::ConstructorPat*>(nf->pat)) {
                if (is_optional_schema) {
                    // Optional schema field: must have a value before we
                    // can dereference for dynamic_cast. Add a has_value
                    // leaf check evaluated in the combined-if block, then
                    // recurse against `(*field)`.
                    leaf_checks.push_back(field_expr + ".has_value()");
                    nested.emplace_back(sub_cp, "(*" + field_expr + ")");
                } else {
                    nested.emplace_back(sub_cp, field_expr);
                }
            } else {
                std::string ck = field_check_expr(nf->pat, field_expr, matched);
                if (!ck.empty()) leaf_checks.push_back(std::move(ck));
                // WildcardPat: no check, no bind.
            }
        } else if (auto* rf = dynamic_cast<DdcgAst::RestFieldPat*>(fp)) {
            indent() << "auto " << rf->name << " = "
                          << nv << "->" << rf->name << ";\n";
        }
    }

    // 3. Open the combined leaf-check block (if any).
    if (!leaf_checks.empty()) {
        std::string combined;
        for (size_t i = 0; i < leaf_checks.size(); ++i) {
            if (i) combined += " && ";
            // Parenthesise only when joining — see c_backend.cpp: a
            // lone check already sits inside the `if (...)`, and the
            // extra pair emits `if ((a == b))`, which clang reads as a
            // comparison the author meant to be an assignment
            // (-Wparentheses-equality), an error under -Werror.
            combined += leaf_checks.size() > 1
                          ? "(" + leaf_checks[i] + ")"
                          : leaf_checks[i];
        }
        indent() << "if (" << combined << ") {\n";
        indent_depth_++;
        blocks_open++;
    }

    // 4. Recurse into nested patterns. Each opens its own blocks.
    for (auto& [sub_cp, sub_expr] : nested) {
        // Look up the schema for the nested module — usually same as
        // outer; keep a single-schema lookup until cross-module nesting
        // becomes a need.
        const Schema* sub_schema = &schema;
        if (sub_cp->module != cp->module) {
            // Different module: skip (typecheck already errored if the
            // module/ctor don't exist).
            continue;
        }
        blocks_open += emit_match(*sub_schema, sub_cp, sub_expr, name_ctr);
    }

    return blocks_open;
}

// ── Top-level emission: dispatcher per source sum ─────────────

// One method per source-AST sum, named compile_<sum>, generated as a
// member of the class wrapper. Signature matches Dybvig 1990 §3.2 with
// ρ folded into the class itself:
//
//     CG : E → ρ → δ → γ → Code
//          ─────────────  ───
//           argument list  return
//
// becomes
//
//     <ir_root> compile_<sum>(<Sum>* node, <δ_t> d, <γ_t> g)
//
// The user's MEMBERS block carries everything the paper's ρ used to
// hold (var map, break label) plus whatever extra state the
// consumer needs — arena, gensym counters, diagnostics, source-loc
// tracking. Aux/pred bodies live in MEMBERS too, called as bare
// methods on `this`.
// Emit one action-language `fun` as a C++ method on the dispatcher
// class. Body is the same stmt-block grammar as a rule body — last
// ExprStmt becomes a `return …;` via emit_stmt's is_tail flag.
void CppBackend::emit_fun(DdcgAst::Fun* f) {
    out() << "\n";
    indent() << "// fun " << f->name << "\n";
    indent() << cpp_for_typeref(f->ret_ty) << " " << f->name << "(";
    for (size_t i = 0; i < f->params.size(); ++i) {
        if (i) out() << ", ";
        out() << cpp_for_typeref(f->params[i]->ty)
                 << " " << f->params[i]->name;
    }
    out() << ") {\n";
    indent_depth_++;
    for (size_t i = 0; i < f->body.size(); ++i) {
        bool is_last = (i + 1 == f->body.size());
        emit_stmt(f->body[i], is_last);
    }
    indent_depth_--;
    indent() << "}\n";
}

void CppBackend::emit_dispatcher_signature(const std::string& sum_name) {
    std::string sum_cpp = cpp_class_name(*source_schema_, sum_name);
    out() << ir_root_type_ << " compile_" << sum_name
             << "(" << sum_cpp << "* node, ";
    if (!env_dest_name_.empty()) {
        out() << env_dest_type_ << " " << env_dest_name_ << ", ";
    }
    out() << data_dest_type_ << " " << data_dest_name_ << ", "
             << ctrl_dest_type_ << " " << ctrl_dest_name_ << ", "
             << ir_root_type_ << " Lnext = {})";
}

void CppBackend::emit_dispatcher(const std::string& sum_name) {
    auto rules = rules_for(sum_name);
    if (rules.empty()) return;

    out() << "\n";
    indent() << "// Dispatcher for " << source_import_ << "."
                  << sum_name << "\n";
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
    indent() << "current_loc = node->loc;\n";

    for (auto* r : rules) {
        for (auto* h : r->heads) {
            if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h)) {
                if (cp->module != source_import_) continue;
                auto it = source_schema_->constructors.find(cp->ctor);
                if (it == source_schema_->constructors.end()) continue;
                if (it->second.sum_name != sum_name) continue;
                emit_rule_branch(*source_schema_, it->second, h, r);
            }
        }
    }

    indent() << "return {};\n";
    indent_depth_--;
    indent() << "}\n";
}

bool CppBackend::emit_body() {
    out() << "// Generated by ddcgc — do not edit\n";
    out() << "#pragma once\n";
    out() << "//\n";
    out() << "// Shape: `class Compiler` inside `namespace " << file_->compiler_name << "`.\n";
    out() << "// MEMBERS verbatim block holds the consumer's state and the\n";
    out() << "// aux/pred bodies (called as bare methods on `this`). The\n";
    out() << "// generated dispatch methods read `current_loc` / call\n";
    out() << "// `fresh_label()` if RULES use `label`; the consumer must\n";
    out() << "// declare those in MEMBERS.\n";
    out() << "//\n";
    out() << "// The two destination types `<data_dest>_t` and `<ctrl_dest>_t`\n";
    out() << "// must exist at namespace scope, with one constructor function\n";
    out() << "// per variant — those are *namespace-level*, not class members,\n";
    out() << "// because callers of `compile_<sum>` build δ/γ values themselves.\n";
    out() << "\n";
    for (auto* h : file_->headers) {
        out() << h->code << "\n";
    }
    out() << "#include <vector>\n";
    out() << "#include <tuple>\n";
    out() << "#include <string>\n\n";

    // Namespace named after the COMPILER directive so multiple
    // ddcgc-generated dispatchers can coexist in one binary.
    out() << "namespace " << file_->compiler_name << " {\n\n";

    // List concatenation helper for the `xs + ys` DSL form.
    out() << "template <class T>\n"
             "static inline std::vector<T> _ddcgc_concat(std::vector<T> a,\n"
             "                                            const std::vector<T>& b) {\n"
             "    a.insert(a.end(), b.begin(), b.end());\n"
             "    return a;\n"
             "}\n\n";

    std::vector<std::string> sums = source_sums_with_rules();
    if (sums.empty()) {
        errors_->push_back("no rules dispatch on the source AST schema");
        return false;
    }

    out() << "class Compiler {\n";
    indent_depth_ = 1;

    if (!file_->members.empty()) {
        out() << "public:\n";
        indent() << "// ─── MEMBERS (verbatim) ─────────────────────\n";
        out() << file_->members;
        if (file_->members.back() != '\n') out() << "\n";
        indent() << "// ─── End MEMBERS ────────────────────────────\n";
    }

    if (!file_->private_helpers.empty()) {
        out() << "private:\n";
        indent() << "// ─── PRIVATE (verbatim) ─────────────────────\n";
        out() << file_->private_helpers;
        if (file_->private_helpers.back() != '\n') out() << "\n";
        indent() << "// ─── End PRIVATE ────────────────────────────\n";
    }

    if (!file_->funs.empty()) {
        out() << "protected:\n";
        indent() << "// ─── action-language funs ───────────────────\n";
        indent() << "// Internal helpers; consumer (subclass) calls\n";
        indent() << "// these from rule bodies and other funs, not\n";
        indent() << "// from external code.\n";
        for (auto* f : file_->funs) emit_fun(f);
    }

    // When INTERNAL_DISPATCHERS is set, the per-sum dispatch methods are
    // implementation details — callable only from within the class
    // (e.g. by a public entry method in MEMBERS). Otherwise dispatchers
    // are the public API and consumers call them directly.
    out() << (file_->internal_dispatchers ? "private:\n" : "public:\n");
    for (const auto& s : sums) {
        emit_dispatcher(s);
    }

    indent_depth_ = 0;
    out() << "};\n";
    out() << "\n} // namespace " << file_->compiler_name << "\n";
    return true;
}

} // anonymous namespace

bool emit_cpp(const DdcgAst::File* file,
              const std::map<std::string, Schema>& schemas,
              const CheckResult& check,
              std::ostream& out,
              std::vector<std::string>& errors) {
    CppBackend backend;
    return backend.emit(file, schemas, check, out,
                        /*out_source=*/nullptr,
                        /*header_filename=*/std::string{},
                        errors);
}

std::unique_ptr<Backend> create_cpp_backend() {
    return std::make_unique<CppBackend>();
}

} // namespace ddcgc
