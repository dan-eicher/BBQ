#include "cpp_backend.h"
#include "backend.h"

#include <cctype>
#include <set>

namespace ddcgc {

namespace {

struct Emit {
    std::ostream& out;
    int indent_depth = 0;

    std::ostream& indent() {
        for (int i = 0; i < indent_depth; ++i) out << "    ";
        return out;
    }
};

// Translate an asdl schema type name to its C++ form.
// Sums and constructors become `<module>::<Name>*` (always namespace-
// qualified, so the generated dispatcher works regardless of which
// `namespace` it ends up living in — and so AST and IR types from
// different schemas can't collide on bare names like `SlotRef`).
// Primitives map to canonical C++ types; aliases (e.g. `identifier`
// → `std::string`) pass through.
// asdl emitters escape C++ reserved-word field names by appending '_'
// (e.g. `default` → `default_`). ddcgc's generated field accesses must
// match the asdl-emitted AST class layout, so apply the same escaping
// when emitting `node->fieldname`. Keep this list in sync with
// asdl/src/string_utils.cpp's reserved_keywords.
static bool is_cpp_keyword(const std::string& s) {
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

static std::string safe_field_name(const std::string& name) {
    return is_cpp_keyword(name) ? name + "_" : name;
}

std::string cpp_type_for(const Schema& schema, const std::string& schema_type) {
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

// Same as cpp_type_for but always returns the bare class name (drops
// the trailing '*' if present). Used for `<Name>* node` parameter forms.
// The result is still namespace-qualified for schema types.
std::string cpp_class_name(const Schema& schema, const std::string& sum_or_ctor) {
    std::string t = cpp_type_for(schema, sum_or_ctor);
    if (!t.empty() && t.back() == '*') t.pop_back();
    return t;
}

// Top-level emission state.
struct Ctx {
    const DdcgAst::File* file = nullptr;
    const std::map<std::string, Schema>* schemas = nullptr;
    const CheckResult* check = nullptr;
    std::vector<std::string>* errors = nullptr;
    Emit em;

    // Source-AST schema (the .ddcg's first import).
    std::string source_import;
    const Schema* source_schema = nullptr;

    // The two destination C++ type names. Per BBQ convention, the
    // runtime author must `typedef` (or define a struct named) these
    // exactly: `<data_dest_name>_t` and `<ctrl_dest_name>_t`.
    std::string data_dest_cpp_type;
    std::string ctrl_dest_cpp_type;
    // The DSL-level names (without the _t suffix) the user chose in
    // the .ddcg's `data_dest <name>` / `ctrl_dest <name>` declarations.
    // The dispatcher parameter names are exactly these — so a rule body
    // that writes `gen(e, delta, gamma)` references the dispatcher's
    // own incoming destinations by their declared names.
    std::string data_dest_name;
    std::string ctrl_dest_name;
    // Empty when no env_dest declared — dispatcher and gen-call stay
    // 4-arg (without ρ).
    std::string env_dest_cpp_type;
    std::string env_dest_name;

    // ir_root C++ type — the return type of every dispatcher.
    std::string ir_root_cpp_type;

    Ctx(std::ostream& o) : em{o} {}
};

// ── Type lookup helpers (keyed on the type-checker side-table) ─

// Resolve the schema sum_name that a gen-call subject ought to dispatch
// to, by consulting expr_types. Returns "" if the subject isn't a Schema
// type (e.g. nil literal, unknown) — caller falls back to a runtime void*
// dispatch or emits an error.
std::string sum_for_subject(const Ctx& c, DdcgAst::Expr* subject) {
    auto it = c.check->expr_types.find(subject);
    if (it == c.check->expr_types.end()) return {};
    const Type& t = it->second;
    if (t.kind != TypeKind::Schema) return {};
    if (t.schema_import != c.source_import) return {};
    // If t.name is a constructor, look up its sum_name in the schema.
    auto ci = c.source_schema->constructors.find(t.name);
    if (ci != c.source_schema->constructors.end()) return ci->second.sum_name;
    if (c.source_schema->sum_names.count(t.name)) return t.name;
    return {};
}

// Translate a Type (from the type-checker) to a C++ type expression
// suitable for declarations. Falls back to `auto` for things we
// can't render cleanly (e.g. truly Unknown).
std::string cpp_for_type(const Ctx& c, const Type& t) {
    switch (t.kind) {
        case TypeKind::Int:    return "int";
        case TypeKind::String: return "std::string";
        case TypeKind::Bool:   return "bool";
        case TypeKind::Ident:  return "std::string";
        case TypeKind::Label:  return "label_t";
        case TypeKind::Void:   return "void";
        case TypeKind::Schema: {
            auto sit = c.schemas->find(t.schema_import);
            if (sit == c.schemas->end()) return "auto";
            return cpp_type_for(sit->second, t.name);
        }
        case TypeKind::Dest:   return t.name + "_t";
        case TypeKind::List: {
            if (t.elems.empty()) return "auto";
            return "std::vector<" + cpp_for_type(c, t.elems[0]) + ">";
        }
        case TypeKind::Tuple: {
            std::string r = "std::tuple<";
            for (size_t i = 0; i < t.elems.size(); ++i) {
                if (i) r += ", ";
                r += cpp_for_type(c, t.elems[i]);
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
std::string cpp_for_typeref(Ctx& c, DdcgAst::TypeRef* tr) {
    if (auto* tn = dynamic_cast<DdcgAst::TypeName*>(tr)) {
        const std::string& n = tn->name;

        // Module-qualified schema reference.
        if (!tn->module.empty()) {
            auto sit = c.schemas->find(tn->module);
            if (sit != c.schemas->end()) return cpp_type_for(sit->second, n);
            return tn->module + "::" + n;
        }

        // `ir_root` placeholder — resolves to whatever the importing
        // file declared as its ir_root. Shared libraries (dybvig.ddcg)
        // use this so they don't have to know the consumer's IR's
        // module name or root sum name.
        if (n == "ir_root") {
            if (!c.ir_root_cpp_type.empty()) return c.ir_root_cpp_type;
            return "/*BUG: ir_root used before resolution*/auto";
        }

        if (n == "int")                          return "int";
        if (n == "int64")                        return "int64_t";
        if (n == "string")                       return "std::string";
        if (n == "bool")                         return "bool";
        if (n == "ident" || n == "identifier")   return "std::string";
        if (n == "label")                        return "label_t";
        if (n == "void")                         return "void";
        // Destination type → <name>_t (data_dest, ctrl_dest, or env_dest)
        for (auto* d : c.file->destinations) {
            if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
                if (dd->name == n) return n + "_t";
            } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
                if (cd->name == n) return n + "_t";
            } else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
                if (ed->name == n) return n + "_t";
            }
        }
        // Schema sum/ctor — search loaded schemas. Multiple schemas
        // matching is ambiguous; emit a sentinel that surfaces as a
        // C++ compile error (better than silent first-wins).
        const Schema* hit = nullptr;
        std::string hit_imp;
        for (const auto& [imp, schema] : *c.schemas) {
            if (schema.sum_names.count(n) || schema.constructors.count(n)) {
                if (hit) {
                    c.errors->push_back("typeref '" + n +
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
        return "std::vector<" + cpp_for_typeref(c, tl->elem) + ">";
    }
    if (auto* tt = dynamic_cast<DdcgAst::TypeTuple*>(tr)) {
        std::string r = "std::tuple<";
        for (size_t i = 0; i < tt->elems.size(); ++i) {
            if (i) r += ", ";
            r += cpp_for_typeref(c, tt->elems[i]);
        }
        return r + ">";
    }
    return "auto";
}

// ── Expression translation ────────────────────────────────────

void emit_expr(Ctx& c, DdcgAst::Expr* e);
// Forward decl so MatchExpr's per-arm body emission can call it.
void emit_stmt(Ctx& c, DdcgAst::Stmt* s, bool is_tail);

bool is_aux(Ctx& c, const std::string& name) {
    for (auto* a : c.file->auxiliaries) if (a->name == name) return true;
    return false;
}

bool is_pred(Ctx& c, const std::string& name) {
    for (auto* p : c.file->predicates) if (p->name == name) return true;
    return false;
}

bool is_dest_variant(Ctx& c, const std::string& name) {
    for (auto* d : c.file->destinations) {
        if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
            for (auto* v : dd->variants) if (v->name == name) return true;
        } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
            for (auto* v : cd->variants) if (v->name == name) return true;
        }
    }
    return false;
}

// True when `name` is a dest variant declared with zero payload fields.
// In the .ddcg these can appear as bare idents (`effect`, `ac`, `fail`)
// rather than calls — but at the C++ level the runtime exposes them as
// no-arg constructor functions, so the emitter has to add `()`.
bool is_zero_arg_dest_variant(Ctx& c, const std::string& name) {
    for (auto* d : c.file->destinations) {
        std::vector<DdcgAst::DestVariant*> vs;
        if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) vs = dd->variants;
        else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) vs = cd->variants;
        for (auto* v : vs) if (v->name == name) return v->fields.empty();
    }
    return false;
}

void emit_call_args(Ctx& c, const std::vector<DdcgAst::Expr*>& args) {
    c.em.out << "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) c.em.out << ", ";
        emit_expr(c, args[i]);
    }
    c.em.out << ")";
}

void emit_expr(Ctx& c, DdcgAst::Expr* e) {
    if (auto* il = dynamic_cast<DdcgAst::IntLit*>(e)) {
        c.em.out << il->value;
        return;
    }
    if (auto* sl = dynamic_cast<DdcgAst::StringLit*>(e)) {
        c.em.out << "\"";
        for (char ch : sl->value) {
            switch (ch) {
                case '\\': c.em.out << "\\\\"; break;
                case '"':  c.em.out << "\\\""; break;
                case '\n': c.em.out << "\\n";  break;
                case '\r': c.em.out << "\\r";  break;
                case '\t': c.em.out << "\\t";  break;
                default:   c.em.out << ch;     break;
            }
        }
        c.em.out << "\"";
        return;
    }
    if (auto* bl = dynamic_cast<DdcgAst::BoolLit*>(e)) {
        c.em.out << (bl->value ? "true" : "false");
        return;
    }
    if (dynamic_cast<DdcgAst::NilLit*>(e)) {
        c.em.out << "nullptr";
        return;
    }
    if (auto* id = dynamic_cast<DdcgAst::IdentExpr*>(e)) {
        if (is_zero_arg_dest_variant(c, id->name)) {
            c.em.out << id->name << "()";
        } else {
            c.em.out << id->name;
        }
        return;
    }
    if (auto* t = dynamic_cast<DdcgAst::TernaryExpr*>(e)) {
        c.em.out << "(";
        emit_expr(c, t->cond);
        c.em.out << " ? ";
        emit_expr(c, t->then_);
        c.em.out << " : ";
        emit_expr(c, t->else_);
        c.em.out << ")";
        return;
    }
    if (auto* b = dynamic_cast<DdcgAst::BinOp*>(e)) {
        std::string op = b->op;
        if (op == "and") op = "&&";
        else if (op == "or") op = "||";

        // List concat: if `+` was type-checked as list-typed, lower
        // it via the emitted `_ddcgc_concat` helper rather than the
        // C++ `+` operator (std::vector has no `operator+`).
        if (op == "+") {
            auto it = c.check->expr_types.find(e);
            if (it != c.check->expr_types.end() &&
                it->second.kind == TypeKind::List) {
                c.em.out << "_ddcgc_concat(";
                emit_expr(c, b->left);
                c.em.out << ", ";
                emit_expr(c, b->right);
                c.em.out << ")";
                return;
            }
        }

        c.em.out << "(";
        emit_expr(c, b->left);
        c.em.out << " " << op << " ";
        emit_expr(c, b->right);
        c.em.out << ")";
        return;
    }
    if (auto* u = dynamic_cast<DdcgAst::UnaryOp*>(e)) {
        std::string op = u->op;
        if (op == "not") op = "!";
        c.em.out << "(" << op;
        emit_expr(c, u->operand);
        c.em.out << ")";
        return;
    }
    if (auto* call = dynamic_cast<DdcgAst::CallExpr*>(e)) {
        // Aux, pred, and dest-variant calls all emit identically — bare
        // function-call syntax. Auxes/preds resolve to the user's MEMBERS
        // methods on `this` (no qualification needed inside class scope);
        // dest variants resolve to free functions in the same namespace.
        if (auto* fn_id = dynamic_cast<DdcgAst::IdentExpr*>(call->fn)) {
            (void)fn_id;
        }
        emit_expr(c, call->fn);
        emit_call_args(c, call->args);
        return;
    }
    if (auto* fa = dynamic_cast<DdcgAst::FieldAccExpr*>(e)) {
        emit_expr(c, fa->base);
        // Schema sums/ctors emit as pointer types (`<Name>*`); accessing
        // their fields uses `->`. Dest tagged unions and primitive value
        // types use `.`. Default to `->` when type info is missing.
        const char* sep = "->";
        auto it = c.check->expr_types.find(fa->base);
        if (it != c.check->expr_types.end()) {
            switch (it->second.kind) {
                case TypeKind::Dest:
                case TypeKind::Int:
                case TypeKind::String:
                case TypeKind::Bool:
                case TypeKind::Ident:
                case TypeKind::Label:
                case TypeKind::Tuple:
                    sep = ".";
                    break;
                default: break;
            }
        }
        c.em.out << sep << fa->field;
        return;
    }
    if (auto* ix = dynamic_cast<DdcgAst::IndexAccExpr*>(e)) {
        emit_expr(c, ix->base);
        c.em.out << "[";
        emit_expr(c, ix->index);
        c.em.out << "]";
        return;
    }
    if (auto* l = dynamic_cast<DdcgAst::ListLit*>(e)) {
        // Try to render the element type from the list's inferred type;
        // if the type checker computed it, use it for an explicit
        // std::vector<T>{...} (this also makes empty list literals
        // type-correct, which CTAD can't do alone).
        std::string elem_cpp;
        auto it = c.check->expr_types.find(e);
        if (it != c.check->expr_types.end() &&
            it->second.kind == TypeKind::List &&
            !it->second.elems.empty()) {
            elem_cpp = cpp_for_type(c, it->second.elems[0]);
        }
        if (!elem_cpp.empty() && elem_cpp != "auto") {
            c.em.out << "std::vector<" << elem_cpp << ">{";
        } else {
            c.em.out << "std::vector{";
        }
        for (size_t i = 0; i < l->elems.size(); ++i) {
            if (i > 0) c.em.out << ", ";
            emit_expr(c, l->elems[i]);
        }
        c.em.out << "}";
        return;
    }
    if (auto* tu = dynamic_cast<DdcgAst::TupleLit*>(e)) {
        c.em.out << "std::make_tuple(";
        for (size_t i = 0; i < tu->elems.size(); ++i) {
            if (i > 0) c.em.out << ", ";
            emit_expr(c, tu->elems[i]);
        }
        c.em.out << ")";
        return;
    }
    if (auto* dp = dynamic_cast<DdcgAst::DestPattern*>(e)) {
        c.em.out << dp->name;
        emit_call_args(c, dp->args);
        return;
    }
    if (auto* w = dynamic_cast<DdcgAst::WithExpr*>(e)) {
        // Functional record-update via GNU statement-expression: copy
        // the base, mutate the named field on the copy, return it.
        c.em.out << "({ auto _w = (";
        emit_expr(c, w->base);
        c.em.out << "); _w." << w->field << " = (";
        emit_expr(c, w->value);
        c.em.out << "); _w; })";
        return;
    }
    if (auto* b = dynamic_cast<DdcgAst::BuildExpr*>(e)) {
        // Lower `build Schema.Ctor(args)` to `new Namespace::Ctor(args)`.
        // cpp_class_name handles snake_case → PascalCase + namespace.
        auto sit = c.schemas->find(b->schema);
        if (sit == c.schemas->end()) {
            c.errors->push_back("build: unknown schema '" + b->schema +
                                "' (typecheck should have caught this)");
            c.em.out << "/*BUG: unknown schema*/(void)0";
            return;
        }
        c.em.out << "new " << cpp_class_name(sit->second, b->ctor) << "(";
        for (size_t i = 0; i < b->args.size(); ++i) {
            if (i) c.em.out << ", ";
            emit_expr(c, b->args[i]);
        }
        c.em.out << ")";
        return;
    }
    if (auto* m = dynamic_cast<DdcgAst::MatchExpr*>(e)) {
        // CamelCase the dest name → its tag enum (e.g. "gamma" → "GammaTag").
        // Per BBQ runtime convention; matches what tiny_runtime.h /
        // calc.ddcg expose.
        auto camel = [](const std::string& s) {
            std::string r; bool up = true;
            for (char ch : s) {
                if (ch == '_') { up = true; continue; }
                r += up ? (char)std::toupper(ch) : ch;
                up = false;
            }
            return r;
        };

        // Determine the subject's static type. Two supported shapes:
        //   1. Dest tagged-union  → switch on `_m.tag` against `<Dest>Tag::X`
        //   2. asdl enum sum      → switch on `_m` directly against
        //                            `<module>::<Enum>::X`
        std::string dest_name;
        std::string enum_cpp;     // namespace-qualified class name when subj is enum
        const Schema* enum_schema = nullptr;
        std::string enum_sum_name;
        auto it = c.check->expr_types.find(m->subject);
        if (it != c.check->expr_types.end()) {
            const Type& st = it->second;
            if (st.kind == TypeKind::Dest) {
                dest_name = st.name;
            } else if (st.kind == TypeKind::Schema) {
                auto sit = c.schemas->find(st.schema_import);
                if (sit != c.schemas->end() &&
                    sit->second.enum_sums.count(st.name)) {
                    enum_schema = &sit->second;
                    enum_sum_name = st.name;
                    enum_cpp = cpp_class_name(*enum_schema, st.name);
                }
            }
        }
        if (dest_name.empty() && enum_cpp.empty()) {
            c.errors->push_back("match: subject is not a dest tagged union "
                                "or asdl enum");
            c.em.out << "({})";
            return;
        }
        std::string tag_class = camel(dest_name) + "Tag";

        // IIFE wrapping the switch — match-as-expression. Capture-by-ref.
        // Pull the recorded result type to give the lambda an explicit
        // return so arms with related-but-different constructor types
        // don't trip auto-deduction.
        std::string match_ret = "auto";
        auto rti = c.check->expr_types.find(e);
        if (rti != c.check->expr_types.end()) {
            match_ret = cpp_for_type(c, rti->second);
        }
        c.em.out << "([&]() -> " << match_ret << " {\n";
        c.em.indent_depth++;
        c.em.indent() << "auto _m = ";
        emit_expr(c, m->subject);
        c.em.out << ";\n";
        if (!enum_cpp.empty()) {
            c.em.indent() << "switch (_m) {\n";
        } else {
            c.em.indent() << "switch (_m.tag) {\n";
        }
        // Helper: BindPat with a name matching a zero-arg variant of
        // this dest is treated as a variant match (`case Tag::X:`).
        auto is_zero_arg_variant_name = [&](const std::string& nm) {
            for (auto* d : c.file->destinations) {
                std::string dn;
                std::vector<DdcgAst::DestVariant*> vs;
                if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
                    dn = dd->name; vs = dd->variants;
                } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
                    dn = cd->name; vs = cd->variants;
                }
                if (dn != dest_name) continue;
                for (auto* v : vs) {
                    if (v->name == nm && v->fields.empty()) return true;
                }
            }
            return false;
        };
        // Helper: BindPat name matching one of the enum sum's
        // constructors → emit `case Module::Enum::X:`.
        auto is_enum_value_name = [&](const std::string& nm) {
            if (!enum_schema) return false;
            auto eit = enum_schema->sum_constructors.find(enum_sum_name);
            if (eit == enum_schema->sum_constructors.end()) return false;
            for (const auto& cn : eit->second) {
                if (cn == nm) return true;
            }
            return false;
        };

        for (auto* arm : m->arms) {
            c.em.indent();
            if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(arm->pat);
                cp && cp->module.empty()) {
                c.em.out << "case " << tag_class << "::" << camel(cp->ctor) << ": {\n";
                c.em.indent_depth++;
                // Bind each NamedFieldPat to the subject's field.
                for (auto* fp : cp->fields) {
                    if (auto* nf = dynamic_cast<DdcgAst::NamedFieldPat*>(fp)) {
                        if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(nf->pat)) {
                            c.em.indent() << "auto " << bp->name
                                          << " = _m." << nf->name << ";\n";
                        } else if (dynamic_cast<DdcgAst::WildcardPat*>(nf->pat)) {
                            // No binding for wildcard.
                        }
                    }
                }
            } else if (dynamic_cast<DdcgAst::WildcardPat*>(arm->pat)) {
                c.em.out << "default: {\n";
                c.em.indent_depth++;
            } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat);
                       bp && !enum_cpp.empty() && is_enum_value_name(bp->name)) {
                c.em.out << "case " << enum_cpp << "::" << bp->name << ": {\n";
                c.em.indent_depth++;
            } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat);
                       bp && is_zero_arg_variant_name(bp->name)) {
                c.em.out << "case " << tag_class << "::" << camel(bp->name) << ": {\n";
                c.em.indent_depth++;
            } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat)) {
                c.em.out << "default: {\n";
                c.em.indent_depth++;
                c.em.indent() << "auto " << bp->name << " = _m;\n";
            } else {
                c.errors->push_back("match: unsupported arm pattern shape");
                c.em.out << "default: {\n";
                c.em.indent_depth++;
            }
            for (size_t i = 0; i < arm->body.size(); ++i) {
                bool is_last = (i + 1 == arm->body.size());
                emit_stmt(c, arm->body[i], is_last);
            }
            c.em.indent_depth--;
            c.em.indent() << "}\n";
        }
        c.em.indent() << "}\n";
        // Unreachable for exhaustive matches; keeps the compiler happy.
        c.em.indent() << "return {};\n";
        c.em.indent_depth--;
        c.em.indent() << "})()";
        return;
    }
    if (auto* g = dynamic_cast<DdcgAst::GenCall*>(e)) {
        std::string sum = sum_for_subject(c, g->subject);
        if (sum.empty()) {
            // No silent fallback: if we can't pick a sum the .ddcg
            // is broken (typically the subject's type wasn't inferred
            // because of an upstream error). Surface it.
            c.errors->push_back(
                "gen-call subject's source-schema sum could not be "
                "resolved (subject type missing from expr_types). "
                "Most likely the .ddcg has a typecheck error that "
                "prevents inference.");
            c.em.out << "/*BUG: unresolved gen*/(void)";
            emit_expr(c, g->subject);
            return;
        }
        c.em.out << "compile_" << sum << "(";
        emit_expr(c, g->subject);
        for (auto* a : g->args) {
            c.em.out << ", ";
            emit_expr(c, a);
        }
        c.em.out << ")";
        return;
    }
}

// ── Statement translation ────────────────────────────────────

void emit_stmt(Ctx& c, DdcgAst::Stmt* s, bool is_tail);

void emit_stmts(Ctx& c, const std::vector<DdcgAst::Stmt*>& body,
                 bool body_is_tail_position) {
    for (size_t i = 0; i < body.size(); ++i) {
        bool is_last = (i + 1 == body.size());
        emit_stmt(c, body[i], body_is_tail_position && is_last);
    }
}

// Pick the C++ declaration for a let-bound name. If the type-checker
// recorded a Type for the RHS, render it; otherwise fall back to `auto`.
std::string let_decl_for(Ctx& c, DdcgAst::Expr* rhs) {
    auto it = c.check->expr_types.find(rhs);
    if (it == c.check->expr_types.end()) return "auto";
    std::string t = cpp_for_type(c, it->second);
    return t.empty() ? "auto" : t;
}

void emit_stmt(Ctx& c, DdcgAst::Stmt* s, bool is_tail) {
    if (auto* let = dynamic_cast<DdcgAst::LetStmt*>(s)) {
        c.em.indent();
        if (let->binds.size() == 1) {
            if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(let->binds[0])) {
                c.em.out << let_decl_for(c, let->value) << " " << sb->name
                         << " = ";
                emit_expr(c, let->value);
                c.em.out << ";\n";
            } else {
                c.em.out << "(void) ";
                emit_expr(c, let->value);
                c.em.out << ";\n";
            }
        } else {
            // Tuple-destructuring let: leave the deduction to C++17
            // structured bindings — we already type-checked the shape.
            c.em.out << "auto [";
            for (size_t i = 0; i < let->binds.size(); ++i) {
                if (i > 0) c.em.out << ", ";
                if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(let->binds[i])) {
                    c.em.out << sb->name;
                } else {
                    c.em.out << "_unused" << i;
                }
            }
            c.em.out << "] = ";
            emit_expr(c, let->value);
            c.em.out << ";\n";
        }
        return;
    }
    if (auto* ma = dynamic_cast<DdcgAst::MutAssign*>(s)) {
        c.em.indent() << ma->name << " = ";
        emit_expr(c, ma->value);
        c.em.out << ";\n";
        return;
    }
    if (auto* f = dynamic_cast<DdcgAst::ForStmt*>(s)) {
        c.em.indent() << "for (auto& ";
        if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(f->iter)) {
            c.em.out << sb->name;
        } else {
            c.em.out << "_unused";
        }
        c.em.out << " : ";
        emit_expr(c, f->seq);
        c.em.out << ") {\n";
        c.em.indent_depth++;
        emit_stmts(c, f->body, /*tail=*/false);
        c.em.indent_depth--;
        c.em.indent() << "}\n";
        return;
    }
    if (auto* lb = dynamic_cast<DdcgAst::LabelStmt*>(s)) {
        c.em.indent() << "auto " << lb->name
                      << " = fresh_label();\n";
        return;
    }
    if (auto* es = dynamic_cast<DdcgAst::ExprStmt*>(s)) {
        c.em.indent();
        if (is_tail) c.em.out << "return ";
        emit_expr(c, es->value);
        c.em.out << ";\n";
        return;
    }
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
std::string field_check_expr(DdcgAst::Pattern* sub,
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
int emit_match(Ctx& c, const Schema& schema,
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
    c.em.indent() << "if (auto* " << nv << " = dynamic_cast<"
                  << cpp_class_name(schema, ci->second.name) << "*>("
                  << value_expr << ")) {\n";
    c.em.indent_depth++;
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
                        c.em.indent() << "auto " << bp->name << " = "
                                      << field_expr
                                      << ".has_value() ? *" << field_expr
                                      << " : nullptr;\n";
                    } else {
                        c.em.indent() << "auto " << bp->name << " = "
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
            c.em.indent() << "auto " << rf->name << " = "
                          << nv << "->" << rf->name << ";\n";
        }
    }

    // 3. Open the combined leaf-check block (if any).
    if (!leaf_checks.empty()) {
        std::string combined;
        for (size_t i = 0; i < leaf_checks.size(); ++i) {
            if (i) combined += " && ";
            combined += "(" + leaf_checks[i] + ")";
        }
        c.em.indent() << "if (" << combined << ") {\n";
        c.em.indent_depth++;
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
        blocks_open += emit_match(c, *sub_schema, sub_cp, sub_expr, name_ctr);
    }

    return blocks_open;
}

void emit_rule_branch(Ctx& c, const Schema& schema,
                       const Constructor& ctor,
                       DdcgAst::Pattern* head,
                       DdcgAst::Rule* rule) {
    auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(head);
    if (!cp) return;
    (void)ctor;  // matched via cp->ctor lookup inside emit_match

    int name_ctr = 0;
    int blocks_open = emit_match(c, schema, cp, "node", name_ctr);

    if (rule->guard.has_value()) {
        c.em.indent() << "if (";
        emit_expr(c, *rule->guard);
        c.em.out << ") {\n";
        c.em.indent_depth++;
        emit_stmts(c, rule->body, /*tail=*/true);
        c.em.indent_depth--;
        c.em.indent() << "}\n";
    } else {
        emit_stmts(c, rule->body, /*tail=*/true);
    }

    while (blocks_open--) {
        c.em.indent_depth--;
        c.em.indent() << "}\n";
    }
}

// ── Top-level emission: dispatcher per source sum ─────────────

// Collect the rules that match against a given (module, sum_name).
std::vector<DdcgAst::Rule*> rules_for(const Ctx& c,
                                       const std::string& sum_name) {
    std::vector<DdcgAst::Rule*> out;
    for (auto* r : c.file->rules) {
        for (auto* h : r->heads) {
            if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h)) {
                if (cp->module != c.source_import) continue;
                auto it = c.source_schema->constructors.find(cp->ctor);
                if (it == c.source_schema->constructors.end()) continue;
                if (it->second.sum_name != sum_name) continue;
                out.push_back(r);
                break;
            }
        }
    }
    return out;
}

// Discover, in source order of first appearance, every sum within the
// source schema that has at least one rule.
std::vector<std::string> source_sums_with_rules(const Ctx& c) {
    std::vector<std::string> sums;
    std::set<std::string> seen;
    for (auto* r : c.file->rules) {
        for (auto* h : r->heads) {
            if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h)) {
                if (cp->module != c.source_import) continue;
                auto it = c.source_schema->constructors.find(cp->ctor);
                if (it == c.source_schema->constructors.end()) continue;
                const std::string& sum = it->second.sum_name;
                if (seen.insert(sum).second) sums.push_back(sum);
            }
        }
    }
    return sums;
}

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
void emit_fun(Ctx& c, DdcgAst::Fun* f) {
    c.em.out << "\n";
    c.em.indent() << "// fun " << f->name << "\n";
    c.em.indent() << cpp_for_typeref(c, f->ret_ty) << " " << f->name << "(";
    for (size_t i = 0; i < f->params.size(); ++i) {
        if (i) c.em.out << ", ";
        c.em.out << cpp_for_typeref(c, f->params[i]->ty)
                 << " " << f->params[i]->name;
    }
    c.em.out << ") {\n";
    c.em.indent_depth++;
    for (size_t i = 0; i < f->body.size(); ++i) {
        bool is_last = (i + 1 == f->body.size());
        emit_stmt(c, f->body[i], is_last);
    }
    c.em.indent_depth--;
    c.em.indent() << "}\n";
}

void emit_dispatcher_signature(Ctx& c, const std::string& sum_name) {
    std::string sum_cpp = cpp_class_name(*c.source_schema, sum_name);
    c.em.out << c.ir_root_cpp_type << " compile_" << sum_name
             << "(" << sum_cpp << "* node, ";
    if (!c.env_dest_name.empty()) {
        c.em.out << c.env_dest_cpp_type << " " << c.env_dest_name << ", ";
    }
    c.em.out << c.data_dest_cpp_type << " " << c.data_dest_name << ", "
             << c.ctrl_dest_cpp_type << " " << c.ctrl_dest_name << ", "
             << c.ir_root_cpp_type << " Lnext = {})";
}

void emit_dispatcher(Ctx& c, const std::string& sum_name) {
    auto rules = rules_for(c, sum_name);
    if (rules.empty()) return;

    c.em.out << "\n";
    c.em.indent() << "// Dispatcher for " << c.source_import << "."
                  << sum_name << "\n";
    c.em.indent();
    emit_dispatcher_signature(c, sum_name);
    c.em.out << " {\n";
    c.em.indent_depth++;
    c.em.indent() << "(void)" << c.data_dest_name << "; "
                  << "(void)" << c.ctrl_dest_name << "; "
                  << "(void)Lnext;\n";
    if (!c.env_dest_name.empty()) {
        c.em.indent() << "(void)" << c.env_dest_name << ";\n";
    }
    c.em.indent() << "current_loc = node->loc;\n";

    for (auto* r : rules) {
        for (auto* h : r->heads) {
            if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h)) {
                if (cp->module != c.source_import) continue;
                auto it = c.source_schema->constructors.find(cp->ctor);
                if (it == c.source_schema->constructors.end()) continue;
                if (it->second.sum_name != sum_name) continue;
                emit_rule_branch(c, *c.source_schema, it->second, h, r);
            }
        }
    }

    c.em.indent() << "return {};\n";
    c.em.indent_depth--;
    c.em.indent() << "}\n";
}

// Resolve the data_dest and ctrl_dest names from the .ddcg's
// DESTINATIONS block. Both are mandatory — the paper's CG signature
// has no degenerate forms. The runtime author must provide types
// named exactly `<dest>_t` for each, and the dispatcher binds the
// declared name itself (e.g. `delta`, `gamma`) as the parameter
// identifier so rule bodies can reference them.
struct DestNames {
    std::string data_name, data_type;
    std::string ctrl_name, ctrl_type;
    // env_dest is optional: empty names mean "no ρ" — dispatcher
    // signature stays 4-arg, gen-call layout stays (δ, γ, [Lnext]).
    std::string env_name, env_type;
};
DestNames dest_type_names(const DdcgAst::File* file,
                            std::vector<std::string>& errors) {
    DestNames out;
    for (auto* d : file->destinations) {
        if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
            if (!out.data_type.empty()) {
                errors.push_back("multiple data_dest declarations");
            }
            out.data_name = dd->name;
            out.data_type = dd->name + "_t";
        } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
            if (!out.ctrl_type.empty()) {
                errors.push_back("multiple ctrl_dest declarations");
            }
            out.ctrl_name = cd->name;
            out.ctrl_type = cd->name + "_t";
        } else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
            if (!out.env_type.empty()) {
                errors.push_back("multiple env_dest declarations");
            }
            out.env_name = ed->name;
            out.env_type = ed->name + "_t";
        }
    }
    if (out.data_type.empty()) errors.push_back("no data_dest declared (the paper's δ)");
    if (out.ctrl_type.empty()) errors.push_back("no ctrl_dest declared (the paper's γ)");
    return out;
}
} // namespace

bool emit_cpp(const DdcgAst::File* file,
              const std::map<std::string, Schema>& schemas,
              const CheckResult& check,
              std::ostream& out,
              std::vector<std::string>& errors) {
    Ctx c(out);
    c.file = file;
    c.schemas = &schemas;
    c.check = &check;
    c.errors = &errors;

    if (auto* tn = dynamic_cast<DdcgAst::TypeName*>(file->ir_root)) {
        // Module-qualified (`ir.expr`) lookups MUST go through the
        // named schema only — picking the first schema that happens
        // to have a sum named `expr` would silently mis-resolve when
        // multiple imports share a constructor name.
        bool resolved = false;
        if (!tn->module.empty()) {
            auto sit = schemas.find(tn->module);
            if (sit == schemas.end()) {
                errors.push_back("ir_root references unknown schema '" +
                                 tn->module + "'");
                return false;
            }
            const Schema& sch = sit->second;
            if (sch.sum_names.count(tn->name) ||
                sch.constructors.count(tn->name)) {
                c.ir_root_cpp_type = cpp_type_for(sch, tn->name);
                resolved = true;
            } else {
                errors.push_back("ir_root '" + tn->module + "." + tn->name +
                                 "' not found in schema '" + tn->module + "'");
                return false;
            }
        } else {
            // Unqualified — fall back to "any schema that has it" only
            // when the user opted out of module qualification. Primitive
            // types (`int`) and runtime-supplied names land here too.
            for (const auto& [imp, sch] : schemas) {
                (void)imp;
                if (sch.sum_names.count(tn->name) ||
                    sch.constructors.count(tn->name)) {
                    c.ir_root_cpp_type = cpp_type_for(sch, tn->name);
                    resolved = true;
                    break;
                }
            }
        }
        if (!resolved) {
            c.ir_root_cpp_type = tn->name;
            if (tn->is_pointer) c.ir_root_cpp_type += "*";
        }
    } else {
        errors.push_back("ir_root must be a simple TypeName (list/tuple roots not supported)");
        return false;
    }

    if (file->imports.empty()) {
        errors.push_back("at least one import is required (the source AST)");
        return false;
    }
    c.source_import = file->imports[0]->name;
    auto sit = schemas.find(c.source_import);
    if (sit == schemas.end()) {
        errors.push_back("source import '" + c.source_import +
                         "' has no loaded schema");
        return false;
    }
    c.source_schema = &sit->second;

    DestNames dn = dest_type_names(file, errors);
    if (dn.data_type.empty() || dn.ctrl_type.empty()) return false;
    c.data_dest_cpp_type = std::move(dn.data_type);
    c.ctrl_dest_cpp_type = std::move(dn.ctrl_type);
    c.data_dest_name = std::move(dn.data_name);
    c.ctrl_dest_name = std::move(dn.ctrl_name);
    c.env_dest_cpp_type = std::move(dn.env_type);
    c.env_dest_name = std::move(dn.env_name);

    out << "// Generated by ddcgc — do not edit\n";
    out << "#pragma once\n";
    out << "//\n";
    out << "// Shape: `class Compiler` inside `namespace " << file->compiler_name << "`.\n";
    out << "// MEMBERS verbatim block holds the consumer's state and the\n";
    out << "// aux/pred bodies (called as bare methods on `this`). The\n";
    out << "// generated dispatch methods read `current_loc` / call\n";
    out << "// `fresh_label()` if RULES use `label`; the consumer must\n";
    out << "// declare those in MEMBERS.\n";
    out << "//\n";
    out << "// The two destination types `<data_dest>_t` and `<ctrl_dest>_t`\n";
    out << "// must exist at namespace scope, with one constructor function\n";
    out << "// per variant — those are *namespace-level*, not class members,\n";
    out << "// because callers of `compile_<sum>` build δ/γ values themselves.\n";
    out << "\n";
    for (auto* h : file->headers) {
        out << h->code << "\n";
    }
    out << "#include <vector>\n";
    out << "#include <tuple>\n";
    out << "#include <string>\n\n";

    // Namespace named after the COMPILER directive so multiple
    // ddcgc-generated dispatchers can coexist in one binary.
    out << "namespace " << file->compiler_name << " {\n\n";

    // List concatenation helper for the `xs + ys` DSL form.
    out << "template <class T>\n"
           "static inline std::vector<T> _ddcgc_concat(std::vector<T> a,\n"
           "                                            const std::vector<T>& b) {\n"
           "    a.insert(a.end(), b.begin(), b.end());\n"
           "    return a;\n"
           "}\n\n";

    std::vector<std::string> sums = source_sums_with_rules(c);
    if (sums.empty()) {
        errors.push_back("no rules dispatch on the source AST schema");
        return false;
    }

    out << "class Compiler {\n";
    c.em.indent_depth = 1;

    if (!file->members.empty()) {
        out << "public:\n";
        c.em.indent() << "// ─── MEMBERS (verbatim) ─────────────────────\n";
        out << file->members;
        if (file->members.back() != '\n') out << "\n";
        c.em.indent() << "// ─── End MEMBERS ────────────────────────────\n";
    }

    if (!file->private_helpers.empty()) {
        out << "private:\n";
        c.em.indent() << "// ─── PRIVATE (verbatim) ─────────────────────\n";
        out << file->private_helpers;
        if (file->private_helpers.back() != '\n') out << "\n";
        c.em.indent() << "// ─── End PRIVATE ────────────────────────────\n";
    }

    if (!file->funs.empty()) {
        out << "protected:\n";
        c.em.indent() << "// ─── action-language funs ───────────────────\n";
        c.em.indent() << "// Internal helpers; consumer (subclass) calls\n";
        c.em.indent() << "// these from rule bodies and other funs, not\n";
        c.em.indent() << "// from external code.\n";
        for (auto* f : file->funs) emit_fun(c, f);
    }

    // When INTERNAL_DISPATCHERS is set, the per-sum dispatch methods are
    // implementation details — callable only from within the class
    // (e.g. by a public entry method in MEMBERS). Otherwise dispatchers
    // are the public API and consumers call them directly.
    out << (file->internal_dispatchers ? "private:\n" : "public:\n");
    for (const auto& s : sums) {
        emit_dispatcher(c, s);
    }

    c.em.indent_depth = 0;
    out << "};\n";
    out << "\n} // namespace " << file->compiler_name << "\n";
    return true;
}

namespace {
class CppBackend : public Backend {
public:
    bool emit(const DdcgAst::File* file,
              const std::map<std::string, Schema>& schemas,
              const CheckResult& check,
              std::ostream& out,
              std::vector<std::string>& errors) override {
        return emit_cpp(file, schemas, check, out, errors);
    }
};
} // namespace

std::unique_ptr<Backend> create_cpp_backend() {
    return std::make_unique<CppBackend>();
}

} // namespace ddcgc
