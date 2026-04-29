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
// Sums and constructors become `<Name>*`; primitives map to canonical
// C++ types; aliases (e.g. `identifier` → `std::string`) pass through.
std::string cpp_type_for(const Schema& schema, const std::string& schema_type) {
    if (auto a = schema.aliases.find(schema_type); a != schema.aliases.end()) {
        return a->second;
    }
    if (schema.sum_names.count(schema_type) ||
        schema.constructors.count(schema_type)) {
        std::string r;
        bool upper = true;
        for (char c : schema_type) {
            if (c == '_') { upper = true; continue; }
            r += upper ? (char)std::toupper(c) : c;
            upper = false;
        }
        return r + "*";
    }
    if (schema_type == "string")     return "std::string";
    if (schema_type == "int")        return "int";
    if (schema_type == "bool")       return "bool";
    if (schema_type == "identifier") return "std::string";
    return schema_type;
}

// Same as cpp_type_for but always returns the bare class name (drops
// the trailing '*' if present). Used for `<Name>* node` parameter forms.
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

// ── Expression translation ────────────────────────────────────

void emit_expr(Ctx& c, DdcgAst::Expr* e);

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
        c.em.out << "->" << fa->field;
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
        c.em.out << ", ";
        emit_expr(c, g->data_dest);
        c.em.out << ", ";
        emit_expr(c, g->ctrl_dest);
        if (g->lnext.has_value()) {
            c.em.out << ", ";
            emit_expr(c, *g->lnext);
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
std::string field_check_expr(DdcgAst::Pattern* sub,
                              const std::string& field_expr) {
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

    // 1. dynamic_cast the value to the constructor type.
    auto ci = schema.constructors.find(cp->ctor);
    if (ci == schema.constructors.end()) return blocks_open;
    std::string nv = "_n" + std::to_string(name_ctr++);
    c.em.indent() << "if (auto* " << nv << " = dynamic_cast<"
                  << ci->second.name << "*>(" << value_expr << ")) {\n";
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
            std::string field_expr = nv + "->" + nf->name;
            if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(nf->pat)) {
                c.em.indent() << "auto " << bp->name << " = "
                              << field_expr << ";\n";
            } else if (auto* sub_cp =
                           dynamic_cast<DdcgAst::ConstructorPat*>(nf->pat)) {
                nested.emplace_back(sub_cp, field_expr);
            } else {
                std::string ck = field_check_expr(nf->pat, field_expr);
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
void emit_dispatcher_signature(Ctx& c, const std::string& sum_name) {
    std::string sum_cpp = cpp_class_name(*c.source_schema, sum_name);
    c.em.out << c.ir_root_cpp_type << " compile_" << sum_name
             << "(" << sum_cpp << "* node, "
             << c.data_dest_cpp_type << " d, "
             << c.ctrl_dest_cpp_type << " g)";
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
    c.em.indent() << "(void)d; (void)g;\n";
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

// Resolve the data_dest and ctrl_dest C++ type names from the .ddcg's
// DESTINATIONS block. Both are mandatory — the paper's CG signature
// has no degenerate forms. The runtime author must provide types
// named exactly `<dest>_t` for each.
struct DestNames { std::string data, ctrl; };
DestNames dest_type_names(const DdcgAst::File* file,
                            std::vector<std::string>& errors) {
    DestNames out;
    for (auto* d : file->destinations) {
        if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
            if (!out.data.empty()) {
                errors.push_back("multiple data_dest declarations");
            }
            out.data = dd->name + "_t";
        } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
            if (!out.ctrl.empty()) {
                errors.push_back("multiple ctrl_dest declarations");
            }
            out.ctrl = cd->name + "_t";
        }
    }
    if (out.data.empty()) errors.push_back("no data_dest declared (the paper's δ)");
    if (out.ctrl.empty()) errors.push_back("no ctrl_dest declared (the paper's γ)");
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
        c.ir_root_cpp_type = tn->name;
        if (tn->is_pointer) c.ir_root_cpp_type += "*";
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
    if (dn.data.empty() || dn.ctrl.empty()) return false;
    c.data_dest_cpp_type = std::move(dn.data);
    c.ctrl_dest_cpp_type = std::move(dn.ctrl);

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

    out << "public:\n";
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
