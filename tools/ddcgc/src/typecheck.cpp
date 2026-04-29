#include "typecheck.h"

#include <set>
#include <sstream>

namespace ddcgc {

// ── Type plumbing ─────────────────────────────────────────────

bool Type::operator==(const Type& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case TypeKind::Schema:
            return schema_import == o.schema_import && name == o.name;
        case TypeKind::Dest:
            return name == o.name;
        case TypeKind::List:
        case TypeKind::Tuple:
            return elems == o.elems;
        case TypeKind::Unknown:
            return name == o.name;  // both unknown but same hint
        default:
            return true;  // primitives compare by kind alone
    }
}

std::string Type::display() const {
    switch (kind) {
        case TypeKind::Unknown:  return name.empty() ? "?" : name + "?";
        case TypeKind::Void:     return "void";
        case TypeKind::Int:      return "int";
        case TypeKind::String:   return "string";
        case TypeKind::Bool:     return "bool";
        case TypeKind::Ident:    return "ident";
        case TypeKind::Label:    return "label";
        case TypeKind::NilLit:   return "nil";
        case TypeKind::Schema:
            return schema_import.empty() ? name + "*" : schema_import + "." + name + "*";
        case TypeKind::Dest:     return name;
        case TypeKind::List: {
            std::ostringstream os;
            os << "list<" << (elems.empty() ? "?" : elems[0].display()) << ">";
            return os.str();
        }
        case TypeKind::Tuple: {
            std::ostringstream os;
            os << "tuple<";
            for (size_t i = 0; i < elems.size(); ++i) {
                if (i) os << ", ";
                os << elems[i].display();
            }
            os << ">";
            return os.str();
        }
    }
    return "?";
}

bool assignable(const Type& expected, const Type& value) {
    if (expected.is_unknown() || value.is_unknown()) return true;
    if (value.kind == TypeKind::NilLit && expected.kind == TypeKind::Schema) return true;
    if (expected.kind == TypeKind::NilLit && value.kind == TypeKind::Schema) return true;
    // Structural compatibility through containers: an Unknown element
    // type matches any concrete one (used for empty list literals,
    // `[]` typed as `list<?>`, vs a concrete `list<int>`).
    if (expected.kind == TypeKind::List && value.kind == TypeKind::List) {
        if (expected.elems.empty() || value.elems.empty()) return true;
        return assignable(expected.elems[0], value.elems[0]);
    }
    if (expected.kind == TypeKind::Tuple && value.kind == TypeKind::Tuple) {
        if (expected.elems.size() != value.elems.size()) return false;
        for (size_t i = 0; i < expected.elems.size(); ++i) {
            if (!assignable(expected.elems[i], value.elems[i])) return false;
        }
        return true;
    }
    return expected == value;
}

// ── Ctx ──────────────────────────────────────────────────────

namespace {

enum class NameKind {
    PatternBind,
    LetBind,
    ForIter,
    Label,
    DestVariant,
    Auxiliary,
    Predicate,
    SchemaModule,
};

struct AuxSig {
    int param_count = 0;
    std::vector<Type> param_types;
    Type return_type;
};

struct Ctx {
    const DdcgAst::File* file = nullptr;
    const std::map<std::string, Schema>* schemas = nullptr;
    CheckResult* result = nullptr;

    std::set<std::string> import_names;
    std::map<std::string, AuxSig> auxes;
    std::map<std::string, AuxSig> preds;
    std::map<std::string, AuxSig> dest_variants;   // ctor_name -> arity + arg types
    std::map<std::string, std::string> dest_variant_owner;  // ctor_name -> dest name (e.g. "gamma")
    // The .ddcg's data_dest and ctrl_dest names (paper's δ and γ).
    // Set during collect_decls; empty string means missing.
    std::string data_dest_name;
    std::string ctrl_dest_name;

    // Stack of name -> (kind, type) bindings.
    struct Binding { NameKind kind; Type ty; };
    std::vector<std::map<std::string, Binding>> stack;

    void push_scope() { stack.emplace_back(); }
    void pop_scope()  { stack.pop_back(); }

    void bind(const std::string& n, NameKind k, Type t = Type::unknown()) {
        if (stack.empty()) stack.emplace_back();
        stack.back()[n] = Binding{k, std::move(t)};
    }

    const Binding* lookup(const std::string& n) const {
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }
};

void err(Ctx& c, DdcgAst::SourceLoc loc, std::string msg) {
    c.result->errors.push_back({loc, std::move(msg)});
}

void warn(Ctx& c, DdcgAst::SourceLoc loc, std::string msg) {
    c.result->warnings.push_back({loc, std::move(msg)});
}

// ── Type translation: schema field -> Type ───────────────────

Type type_from_dsl_typeref(Ctx& c, DdcgAst::TypeRef* tr);

Type type_from_field(Ctx& c, const std::string& schema_import,
                      const std::string& field_type, FieldFlag flag) {
    Type base;
    if      (field_type == "int")        base = Type::int_();
    else if (field_type == "string")     base = Type::str();
    else if (field_type == "bool")       base = Type::bool_();
    else if (field_type == "identifier") base = Type::ident();
    else {
        auto sit = c.schemas->find(schema_import);
        if (sit != c.schemas->end()) {
            const Schema& s = sit->second;
            if (s.sum_names.count(field_type) || s.constructors.count(field_type)) {
                base = Type::schema(schema_import, field_type);
            } else if (auto a = s.aliases.find(field_type); a != s.aliases.end()) {
                base = Type::unknown_named(a->second);
            } else {
                base = Type::unknown_named(field_type);
            }
        } else {
            base = Type::unknown_named(field_type);
        }
    }
    if (flag == FieldFlag::Sequence) return Type::list_(base);
    // Optional fields keep base type; a nil value is compatible per assignable().
    return base;
}

Type type_from_dsl_typeref(Ctx& c, DdcgAst::TypeRef* tr) {
    if (auto* tn = dynamic_cast<DdcgAst::TypeName*>(tr)) {
        const std::string& n = tn->name;
        if (n == "int")        return Type::int_();
        if (n == "string")     return Type::str();
        if (n == "bool")       return Type::bool_();
        if (n == "ident" ||
            n == "identifier") return Type::ident();
        if (n == "label")      return Type::label();
        if (n == "void")       return Type::voidty();

        // Destination type?
        for (auto* d : c.file->destinations) {
            if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
                if (dd->name == n) return Type::dest(dd->name);
            } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
                if (cd->name == n) return Type::dest(cd->name);
            }
        }
        // Schema sum/ctor — search loaded schemas
        for (const auto& [imp, schema] : *c.schemas) {
            if (schema.sum_names.count(n) || schema.constructors.count(n)) {
                return Type::schema(imp, n);
            }
        }
        // Unknown / runtime-supplied: keep the name so display is useful
        return Type::unknown_named(n);
    }
    if (auto* tl = dynamic_cast<DdcgAst::TypeList*>(tr)) {
        return Type::list_(type_from_dsl_typeref(c, tl->elem));
    }
    if (auto* tt = dynamic_cast<DdcgAst::TypeTuple*>(tr)) {
        std::vector<Type> es;
        for (auto* e : tt->elems) es.push_back(type_from_dsl_typeref(c, e));
        return Type::tuple_(std::move(es));
    }
    return Type::unknown();
}

// ── Pattern checking + binding ───────────────────────────────

// Walk a pattern, emit errors for unknown ctors/fields, and bind
// every named variable into the current scope with its schema-declared type.
void check_pattern(Ctx& c, DdcgAst::Pattern* pat, const Type& expected) {
    if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(pat)) {
        auto sit = c.schemas->find(cp->module);
        if (sit == c.schemas->end()) {
            err(c, pat->loc, "unknown schema module: " + cp->module);
            return;
        }
        const Schema& s = sit->second;
        auto ci = s.constructors.find(cp->ctor);
        if (ci == s.constructors.end()) {
            err(c, pat->loc, "unknown constructor: " + cp->module + "." + cp->ctor);
            return;
        }
        const Constructor& ctor = ci->second;
        std::set<std::string> seen;
        for (auto* fp : cp->fields) {
            std::string fname;
            DdcgAst::Pattern* sub = nullptr;
            bool is_rest = false;
            if (auto* nf = dynamic_cast<DdcgAst::NamedFieldPat*>(fp)) {
                fname = nf->name; sub = nf->pat;
            } else if (auto* rf = dynamic_cast<DdcgAst::RestFieldPat*>(fp)) {
                fname = rf->name; is_rest = true;
            }
            const Field* matched = nullptr;
            for (const auto& f : ctor.fields) {
                if (f.name == fname) { matched = &f; break; }
            }
            if (!matched) {
                err(c, pat->loc, "constructor '" + cp->module + "." + cp->ctor +
                                 "' has no field '" + fname + "'");
                continue;
            }
            if (!seen.insert(fname).second) {
                err(c, pat->loc, "duplicate field '" + fname + "' in pattern for '" +
                                 cp->module + "." + cp->ctor + "'");
            }
            Type fty = type_from_field(c, cp->module, matched->type_name, matched->flag);
            if (is_rest) {
                // ...rest binds the whole field (which is sequence-typed)
                c.bind(fname, NameKind::PatternBind, fty);
            } else if (sub) {
                check_pattern(c, sub, fty);
            }
        }
        (void)expected;  // we don't yet validate against `expected` here
        return;
    }
    if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(pat)) {
        c.bind(bp->name, NameKind::PatternBind, expected);
        return;
    }
    if (dynamic_cast<DdcgAst::WildcardPat*>(pat)) return;
    if (dynamic_cast<DdcgAst::NilPat*>(pat)) {
        if (!expected.is_unknown() &&
            expected.kind != TypeKind::Schema &&
            expected.kind != TypeKind::List) {
            err(c, pat->loc, "nil pattern matched against non-nullable field type "
                            + expected.display());
        }
        return;
    }
    if (dynamic_cast<DdcgAst::IntPat*>(pat)) {
        if (!expected.is_unknown() && expected.kind != TypeKind::Int) {
            err(c, pat->loc, "integer literal pattern matched against "
                            + expected.display() + "-typed field");
        }
        return;
    }
    if (dynamic_cast<DdcgAst::StringPat*>(pat)) {
        if (!expected.is_unknown() &&
            expected.kind != TypeKind::String &&
            expected.kind != TypeKind::Ident) {
            err(c, pat->loc, "string literal pattern matched against "
                            + expected.display() + "-typed field");
        }
        return;
    }
}

// ── Expression type inference ────────────────────────────────

Type infer_expr(Ctx& c, DdcgAst::Expr* e);

void record(Ctx& c, DdcgAst::Expr* e, Type t) {
    c.result->expr_types[e] = t;
}

Type infer_call(Ctx& c, DdcgAst::CallExpr* call) {
    // Type-check arguments unconditionally.
    std::vector<Type> arg_types;
    for (auto* a : call->args) arg_types.push_back(infer_expr(c, a));

    auto* fn_id = dynamic_cast<DdcgAst::IdentExpr*>(call->fn);
    if (fn_id) {
        // Aux?
        if (auto it = c.auxes.find(fn_id->name); it != c.auxes.end()) {
            const auto& sig = it->second;
            if ((int)arg_types.size() != sig.param_count) {
                err(c, call->loc, "auxiliary '" + fn_id->name + "' expects " +
                                  std::to_string(sig.param_count) + " arg(s), got " +
                                  std::to_string(arg_types.size()));
            } else {
                for (size_t i = 0; i < arg_types.size(); ++i) {
                    if (!assignable(sig.param_types[i], arg_types[i])) {
                        err(c, call->args[i]->loc,
                            "auxiliary '" + fn_id->name + "' arg " +
                            std::to_string(i + 1) + ": expected " +
                            sig.param_types[i].display() + ", got " +
                            arg_types[i].display());
                    }
                }
            }
            return sig.return_type;
        }
        // Predicate?
        if (auto it = c.preds.find(fn_id->name); it != c.preds.end()) {
            const auto& sig = it->second;
            if ((int)arg_types.size() != sig.param_count) {
                err(c, call->loc, "predicate '" + fn_id->name + "' expects " +
                                  std::to_string(sig.param_count) + " arg(s), got " +
                                  std::to_string(arg_types.size()));
            } else {
                for (size_t i = 0; i < arg_types.size(); ++i) {
                    if (!assignable(sig.param_types[i], arg_types[i])) {
                        err(c, call->args[i]->loc,
                            "predicate '" + fn_id->name + "' arg " +
                            std::to_string(i + 1) + ": expected " +
                            sig.param_types[i].display() + ", got " +
                            arg_types[i].display());
                    }
                }
            }
            return Type::bool_();
        }
        // Dest variant?
        if (auto it = c.dest_variants.find(fn_id->name); it != c.dest_variants.end()) {
            const auto& sig = it->second;
            if ((int)arg_types.size() != sig.param_count) {
                err(c, call->loc, "destination constructor '" + fn_id->name +
                                  "' expects " + std::to_string(sig.param_count) +
                                  " arg(s), got " + std::to_string(arg_types.size()));
            } else {
                for (size_t i = 0; i < arg_types.size(); ++i) {
                    if (!assignable(sig.param_types[i], arg_types[i])) {
                        err(c, call->args[i]->loc,
                            "destination constructor '" + fn_id->name + "' arg " +
                            std::to_string(i + 1) + ": expected " +
                            sig.param_types[i].display() + ", got " +
                            arg_types[i].display());
                    }
                }
            }
            return Type::dest(c.dest_variant_owner[fn_id->name]);
        }
    }
    // Generic call: emit a name lookup error if the callee isn't bound,
    // else opaque (we don't track function types beyond aux/pred/dest).
    if (fn_id && !c.lookup(fn_id->name)) {
        err(c, fn_id->loc, "unknown name: " + fn_id->name);
    }
    return Type::unknown();
}

Type infer_expr(Ctx& c, DdcgAst::Expr* e) {
    if (!e) return Type::unknown();

    Type ret;
    if (dynamic_cast<DdcgAst::IntLit*>(e))     ret = Type::int_();
    else if (dynamic_cast<DdcgAst::StringLit*>(e)) ret = Type::str();
    else if (dynamic_cast<DdcgAst::BoolLit*>(e))   ret = Type::bool_();
    else if (dynamic_cast<DdcgAst::NilLit*>(e))    ret = Type::nil_();
    else if (auto* id = dynamic_cast<DdcgAst::IdentExpr*>(e)) {
        if (auto* b = c.lookup(id->name)) {
            ret = b->ty;
        } else if (auto dv = c.dest_variants.find(id->name);
                   dv != c.dest_variants.end()) {
            // Dest variant referenced bare, but it has payload params —
            // tell the user explicitly instead of "unknown name".
            err(c, id->loc, "destination constructor '" + id->name +
                "' expects " + std::to_string(dv->second.param_count) +
                " arg(s), got 0 (use call form, e.g. " + id->name + "(...))");
            ret = Type::unknown();
        } else {
            err(c, id->loc, "unknown name: " + id->name);
            ret = Type::unknown();
        }
    }
    else if (auto* t = dynamic_cast<DdcgAst::TernaryExpr*>(e)) {
        Type ct = infer_expr(c, t->cond);
        if (!ct.is_unknown() && ct.kind != TypeKind::Bool) {
            err(c, t->cond->loc, "ternary condition must be bool, got " + ct.display());
        }
        Type tt = infer_expr(c, t->then_);
        Type ft = infer_expr(c, t->else_);
        if (!assignable(tt, ft) && !assignable(ft, tt)) {
            err(c, e->loc, "ternary branches have incompatible types: " +
                           tt.display() + " vs " + ft.display());
        }
        ret = tt.is_unknown() ? ft : tt;
    }
    else if (auto* b = dynamic_cast<DdcgAst::BinOp*>(e)) {
        Type lt = infer_expr(c, b->left);
        Type rt = infer_expr(c, b->right);
        const std::string& op = b->op;
        if (op == "and" || op == "or") {
            if (!lt.is_unknown() && lt.kind != TypeKind::Bool)
                err(c, b->left->loc, "operator '" + op + "' expects bool, got " + lt.display());
            if (!rt.is_unknown() && rt.kind != TypeKind::Bool)
                err(c, b->right->loc, "operator '" + op + "' expects bool, got " + rt.display());
            ret = Type::bool_();
        } else if (op == "==" || op == "!=") {
            if (!lt.is_unknown() && !rt.is_unknown() && !assignable(lt, rt) && !assignable(rt, lt))
                err(c, e->loc, "operator '" + op + "' on incompatible types: " +
                               lt.display() + " vs " + rt.display());
            ret = Type::bool_();
        } else if (op == "<" || op == ">" || op == "<=" || op == ">=") {
            if (!lt.is_unknown() && lt.kind != TypeKind::Int)
                err(c, b->left->loc, "operator '" + op + "' expects int, got " + lt.display());
            if (!rt.is_unknown() && rt.kind != TypeKind::Int)
                err(c, b->right->loc, "operator '" + op + "' expects int, got " + rt.display());
            ret = Type::bool_();
        } else if (op == "+" &&
                   (lt.kind == TypeKind::List || rt.kind == TypeKind::List)) {
            // List concatenation: both sides must be list, element types
            // must be compatible (assignable in either direction).
            if (lt.kind != TypeKind::List)
                err(c, b->left->loc, "list concat '+' expects list on left, got " + lt.display());
            else if (rt.kind != TypeKind::List)
                err(c, b->right->loc, "list concat '+' expects list on right, got " + rt.display());
            else if (!assignable(lt, rt) && !assignable(rt, lt))
                err(c, e->loc, "list concat '+' element types differ: " +
                               lt.display() + " vs " + rt.display());
            // Pick the more concrete element type if available.
            ret = (!lt.elems.empty() && !lt.elems[0].is_unknown()) ? lt : rt;
        } else if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            if (!lt.is_unknown() && lt.kind != TypeKind::Int)
                err(c, b->left->loc, "operator '" + op + "' expects int, got " + lt.display());
            if (!rt.is_unknown() && rt.kind != TypeKind::Int)
                err(c, b->right->loc, "operator '" + op + "' expects int, got " + rt.display());
            ret = Type::int_();
        } else {
            ret = Type::unknown();
        }
    }
    else if (auto* u = dynamic_cast<DdcgAst::UnaryOp*>(e)) {
        Type ot = infer_expr(c, u->operand);
        if (u->op == "not") {
            if (!ot.is_unknown() && ot.kind != TypeKind::Bool)
                err(c, u->operand->loc, "operator 'not' expects bool, got " + ot.display());
            ret = Type::bool_();
        } else if (u->op == "-") {
            if (!ot.is_unknown() && ot.kind != TypeKind::Int)
                err(c, u->operand->loc, "unary '-' expects int, got " + ot.display());
            ret = Type::int_();
        } else {
            ret = Type::unknown();
        }
    }
    else if (auto* call = dynamic_cast<DdcgAst::CallExpr*>(e)) {
        ret = infer_call(c, call);
    }
    else if (auto* fa = dynamic_cast<DdcgAst::FieldAccExpr*>(e)) {
        Type bt = infer_expr(c, fa->base);
        if (bt.kind == TypeKind::Schema) {
            // Field lookup only resolves when the base is a specific
            // constructor — a sum-typed value would require dispatching
            // first to know which constructor's fields are visible.
            auto sit = c.schemas->find(bt.schema_import);
            if (sit != c.schemas->end()) {
                const Schema& s = sit->second;
                if (auto cit = s.constructors.find(bt.name); cit != s.constructors.end()) {
                    const Field* f = nullptr;
                    for (const auto& fld : cit->second.fields) {
                        if (fld.name == fa->field) { f = &fld; break; }
                    }
                    if (f) ret = type_from_field(c, bt.schema_import, f->type_name, f->flag);
                    else {
                        err(c, fa->loc, "type " + bt.display() +
                                        " has no field '" + fa->field + "'");
                        ret = Type::unknown();
                    }
                } else if (s.sum_names.count(bt.name)) {
                    // Sum-typed access — only allowed if every constructor
                    // has the field and they all agree on type.
                    Type common;
                    bool first = true; bool all_match = true;
                    if (auto cl = s.sum_constructors.find(bt.name); cl != s.sum_constructors.end()) {
                        for (const auto& cname : cl->second) {
                            const Constructor& ctor = s.constructors.at(cname);
                            const Field* f = nullptr;
                            for (const auto& fld : ctor.fields) {
                                if (fld.name == fa->field) { f = &fld; break; }
                            }
                            if (!f) { all_match = false; break; }
                            Type t = type_from_field(c, bt.schema_import, f->type_name, f->flag);
                            if (first) { common = t; first = false; }
                            else if (!(common == t)) { all_match = false; break; }
                        }
                    }
                    if (!all_match || first) {
                        err(c, fa->loc, "field '" + fa->field +
                                        "' is not common to all constructors of sum " + bt.display());
                        ret = Type::unknown();
                    } else {
                        ret = common;
                    }
                } else {
                    ret = Type::unknown();
                }
            } else {
                ret = Type::unknown();
            }
        } else if (bt.is_unknown()) {
            ret = Type::unknown();
        } else {
            err(c, fa->loc, "field access requires schema-typed base, got " + bt.display());
            ret = Type::unknown();
        }
    }
    else if (auto* ix = dynamic_cast<DdcgAst::IndexAccExpr*>(e)) {
        Type bt = infer_expr(c, ix->base);
        Type it = infer_expr(c, ix->index);
        if (!it.is_unknown() && it.kind != TypeKind::Int)
            err(c, ix->index->loc, "index must be int, got " + it.display());
        if (bt.kind == TypeKind::List)       ret = bt.elems.empty() ? Type::unknown() : bt.elems[0];
        else if (bt.is_unknown())            ret = Type::unknown();
        else {
            err(c, ix->loc, "index access requires list, got " + bt.display());
            ret = Type::unknown();
        }
    }
    else if (auto* l = dynamic_cast<DdcgAst::ListLit*>(e)) {
        if (l->elems.empty()) {
            ret = Type::list_(Type::unknown());  // contextualised later
        } else {
            Type first = infer_expr(c, l->elems[0]);
            for (size_t i = 1; i < l->elems.size(); ++i) {
                Type t = infer_expr(c, l->elems[i]);
                if (!assignable(first, t) && !assignable(t, first)) {
                    err(c, l->elems[i]->loc,
                        "list element type mismatch: expected " + first.display() +
                        ", got " + t.display());
                }
            }
            ret = Type::list_(first);
        }
    }
    else if (auto* tu = dynamic_cast<DdcgAst::TupleLit*>(e)) {
        std::vector<Type> es;
        for (auto* x : tu->elems) es.push_back(infer_expr(c, x));
        ret = Type::tuple_(std::move(es));
    }
    else if (auto* dp = dynamic_cast<DdcgAst::DestPattern*>(e)) {
        // Treat as a destination-variant call.
        auto it = c.dest_variants.find(dp->name);
        if (it == c.dest_variants.end()) {
            err(c, dp->loc, "unknown destination constructor: " + dp->name);
            ret = Type::unknown();
        } else {
            const auto& sig = it->second;
            if ((int)dp->args.size() != sig.param_count) {
                err(c, dp->loc, "destination constructor '" + dp->name + "' expects " +
                                std::to_string(sig.param_count) + " arg(s), got " +
                                std::to_string(dp->args.size()));
            } else {
                for (size_t i = 0; i < dp->args.size(); ++i) {
                    Type at = infer_expr(c, dp->args[i]);
                    if (!assignable(sig.param_types[i], at)) {
                        err(c, dp->args[i]->loc,
                            "destination constructor '" + dp->name + "' arg " +
                            std::to_string(i + 1) + ": expected " +
                            sig.param_types[i].display() + ", got " + at.display());
                    }
                }
            }
            ret = Type::dest(c.dest_variant_owner[dp->name]);
        }
    }
    else if (auto* g = dynamic_cast<DdcgAst::GenCall*>(e)) {
        Type st = infer_expr(c, g->subject);
        Type dd = infer_expr(c, g->data_dest);
        Type cd = infer_expr(c, g->ctrl_dest);
        if (!st.is_unknown() && st.kind != TypeKind::Schema)
            err(c, g->subject->loc, "gen subject must be a schema-typed AST node, got " + st.display());
        // δ must belong to the declared data_dest type (paper's δ slot).
        if (!c.data_dest_name.empty()) {
            Type expected_dd = Type::dest(c.data_dest_name);
            if (!assignable(expected_dd, dd)) {
                err(c, g->data_dest->loc,
                    "gen data destination must be a " + c.data_dest_name +
                    " variant, got " + dd.display());
            }
        }
        // γ must belong to the declared ctrl_dest type.
        if (!c.ctrl_dest_name.empty()) {
            Type expected_cd = Type::dest(c.ctrl_dest_name);
            if (!assignable(expected_cd, cd)) {
                err(c, g->ctrl_dest->loc,
                    "gen control destination must be a " + c.ctrl_dest_name +
                    " variant, got " + cd.display());
            }
        }
        if (g->lnext.has_value()) infer_expr(c, *g->lnext);
        // Result is the ir_root type
        ret = type_from_dsl_typeref(c, c.file->ir_root);
    }
    else {
        ret = Type::unknown();
    }

    record(c, e, ret);
    return ret;
}

// ── Statement checking ───────────────────────────────────────

void check_stmt(Ctx& c, DdcgAst::Stmt* s, const Type& tail_expected, bool is_tail);

void check_block(Ctx& c, const std::vector<DdcgAst::Stmt*>& body,
                  const Type& tail_expected) {
    for (size_t i = 0; i < body.size(); ++i) {
        bool is_last = (i + 1 == body.size());
        check_stmt(c, body[i], tail_expected, is_last);
    }
}

void check_stmt(Ctx& c, DdcgAst::Stmt* s,
                 const Type& tail_expected, bool is_tail) {
    if (auto* let = dynamic_cast<DdcgAst::LetStmt*>(s)) {
        Type vt = infer_expr(c, let->value);
        if (let->binds.size() == 1) {
            if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(let->binds[0])) {
                Type bt = sb->ty.has_value() ? type_from_dsl_typeref(c, *sb->ty) : vt;
                if (sb->ty.has_value() && !assignable(bt, vt)) {
                    err(c, let->loc, "let bind '" + sb->name + "': expected " +
                                     bt.display() + ", got " + vt.display());
                }
                // Bidirectional refinement for empty-list RHS: the
                // literal `[]` produces `list<?>`; if the bind annotates
                // a concrete list type, contextualise the RHS type so
                // the codegen has enough info to emit `std::vector<T>{}`.
                if (sb->ty.has_value() && bt.kind == TypeKind::List &&
                    vt.kind == TypeKind::List &&
                    !vt.elems.empty() && vt.elems[0].is_unknown()) {
                    c.result->expr_types[let->value] = bt;
                }
                c.bind(sb->name, NameKind::LetBind, sb->ty.has_value() ? bt : vt);
            }
        } else {
            // Tuple destructuring: value type must be Tuple of matching size.
            if (vt.kind != TypeKind::Tuple) {
                if (!vt.is_unknown())
                    err(c, let->loc, "tuple destructure of non-tuple value (" + vt.display() + ")");
                for (auto* b : let->binds) {
                    if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(b))
                        c.bind(sb->name, NameKind::LetBind, Type::unknown());
                }
            } else if (vt.elems.size() != let->binds.size()) {
                err(c, let->loc, "tuple destructure size mismatch: " +
                                 std::to_string(vt.elems.size()) + " elems, " +
                                 std::to_string(let->binds.size()) + " binds");
                for (auto* b : let->binds) {
                    if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(b))
                        c.bind(sb->name, NameKind::LetBind, Type::unknown());
                }
            } else {
                for (size_t i = 0; i < let->binds.size(); ++i) {
                    if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(let->binds[i]))
                        c.bind(sb->name, NameKind::LetBind, vt.elems[i]);
                }
            }
        }
        return;
    }
    if (auto* ma = dynamic_cast<DdcgAst::MutAssign*>(s)) {
        const Ctx::Binding* b = c.lookup(ma->name);
        if (!b) {
            err(c, ma->loc, "mutable rebind of unbound name: " + ma->name +
                            " (must be `let`-bound first)");
            infer_expr(c, ma->value);
            return;
        }
        Type vt = infer_expr(c, ma->value);
        if (!assignable(b->ty, vt)) {
            err(c, ma->loc, "rebind of '" + ma->name + "': expected " +
                            b->ty.display() + ", got " + vt.display());
        }
        return;
    }
    if (auto* f = dynamic_cast<DdcgAst::ForStmt*>(s)) {
        Type st = infer_expr(c, f->seq);
        Type elem_t = Type::unknown();
        if (st.kind == TypeKind::List) {
            if (!st.elems.empty()) elem_t = st.elems[0];
        } else if (!st.is_unknown()) {
            err(c, f->loc, "for-loop sequence must be a list, got " + st.display());
        }
        c.push_scope();
        if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(f->iter)) {
            c.bind(sb->name, NameKind::ForIter, elem_t);
        }
        check_block(c, f->body, /*tail_expected=*/Type::voidty());
        c.pop_scope();
        return;
    }
    if (auto* lb = dynamic_cast<DdcgAst::LabelStmt*>(s)) {
        c.bind(lb->name, NameKind::Label, Type::label());
        return;
    }
    if (auto* es = dynamic_cast<DdcgAst::ExprStmt*>(s)) {
        Type vt = infer_expr(c, es->value);
        if (is_tail && tail_expected.kind != TypeKind::Void &&
            !assignable(tail_expected, vt)) {
            err(c, es->loc, "tail expression: expected " + tail_expected.display() +
                            ", got " + vt.display());
        }
        return;
    }
}

// ── Coverage + overlap + duplicate-rule ──────────────────────

using HeadIndex = std::map<std::pair<std::string, std::string>,
                            std::vector<const DdcgAst::Rule*>>;

void index_heads(const DdcgAst::File* file, HeadIndex& idx) {
    for (auto* r : file->rules) {
        for (auto* p : r->heads) {
            if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(p)) {
                idx[{cp->module, cp->ctor}].push_back(r);
            }
        }
    }
}

void check_coverage(Ctx& c, const HeadIndex& idx) {
    if (c.file->imports.empty()) return;
    const std::string& mod = c.file->imports[0]->name;
    auto sit = c.schemas->find(mod);
    if (sit == c.schemas->end()) return;

    // The DSL's `DESUGARED_BY_NORMALIZATION` block lets the .ddcg
    // author exempt constructors that the consumer's earlier
    // normalization pass has already eliminated. Validate every name
    // refers to a real constructor in the source schema, then skip
    // them in the coverage walk.
    std::set<std::string> exempt;
    for (const auto& name : c.file->desugared_by_normalization) {
        if (sit->second.constructors.count(name) == 0) {
            err(c, c.file->loc,
                "desugared_by_normalization: '" + name +
                "' is not a constructor in source import '" + mod + "'");
            continue;
        }
        exempt.insert(name);
    }

    for (const auto& [sum_name, ctor_list] : sit->second.sum_constructors) {
        (void)sum_name;
        for (const auto& cname : ctor_list) {
            if (exempt.count(cname)) continue;
            auto k = std::make_pair(mod, cname);
            if (idx.find(k) == idx.end()) {
                err(c, c.file->loc,
                    "no rule covers constructor '" + mod + "." + cname +
                    "' (declared in source import '" + mod + "')");
            }
        }
    }
}

void check_overlap(Ctx& c, const HeadIndex& idx) {
    for (const auto& [k, rules] : idx) {
        if (rules.size() <= 1) continue;
        bool any_unguarded = false;
        for (auto* r : rules) if (!r->guard.has_value()) any_unguarded = true;
        if (!any_unguarded) continue;
        std::ostringstream msg;
        msg << "multiple rules share head '" << k.first << "." << k.second
            << "' — ensure their `where` guards are disjoint";
        for (size_t i = 1; i < rules.size(); ++i) {
            warn(c, rules[i]->loc, msg.str());
        }
    }
}

// Cheap textual signature for a guard expression — used to detect rules
// that are flat-out duplicates (same head, same guard text).
void serialize_expr(const DdcgAst::Expr* e, std::ostringstream& os);
void serialize_pattern(const DdcgAst::Pattern* p, std::ostringstream& os);

void serialize_expr(const DdcgAst::Expr* e, std::ostringstream& os) {
    if (!e) { os << "_"; return; }
    if (auto* il = dynamic_cast<const DdcgAst::IntLit*>(e)) { os << "I(" << il->value << ")"; return; }
    if (auto* sl = dynamic_cast<const DdcgAst::StringLit*>(e)) { os << "S(" << sl->value << ")"; return; }
    if (auto* bl = dynamic_cast<const DdcgAst::BoolLit*>(e)) { os << "B(" << bl->value << ")"; return; }
    if (dynamic_cast<const DdcgAst::NilLit*>(e)) { os << "nil"; return; }
    if (auto* id = dynamic_cast<const DdcgAst::IdentExpr*>(e)) { os << "id(" << id->name << ")"; return; }
    if (auto* b = dynamic_cast<const DdcgAst::BinOp*>(e)) {
        os << "BO(" << b->op << ",";
        serialize_expr(b->left, os); os << ",";
        serialize_expr(b->right, os); os << ")";
        return;
    }
    if (auto* u = dynamic_cast<const DdcgAst::UnaryOp*>(e)) {
        os << "UO(" << u->op << ",";
        serialize_expr(u->operand, os); os << ")";
        return;
    }
    if (auto* call = dynamic_cast<const DdcgAst::CallExpr*>(e)) {
        os << "C("; serialize_expr(call->fn, os);
        for (auto* a : call->args) { os << ","; serialize_expr(a, os); }
        os << ")";
        return;
    }
    os << "?";  // good enough — duplicate detection is a hint
}

void serialize_pattern(const DdcgAst::Pattern* p, std::ostringstream& os) {
    if (auto* cp = dynamic_cast<const DdcgAst::ConstructorPat*>(p)) {
        os << cp->module << "." << cp->ctor << "(";
        for (size_t i = 0; i < cp->fields.size(); ++i) {
            if (i) os << ",";
            if (auto* nf = dynamic_cast<const DdcgAst::NamedFieldPat*>(cp->fields[i])) {
                os << nf->name << ":";
                serialize_pattern(nf->pat, os);
            } else if (auto* rf = dynamic_cast<const DdcgAst::RestFieldPat*>(cp->fields[i])) {
                os << "..." << rf->name;
            }
        }
        os << ")";
    } else if (dynamic_cast<const DdcgAst::WildcardPat*>(p)) {
        os << "_";
    } else if (dynamic_cast<const DdcgAst::NilPat*>(p)) {
        os << "nil";
    } else if (auto* ip = dynamic_cast<const DdcgAst::IntPat*>(p)) {
        os << "int(" << ip->value << ")";
    } else if (auto* sp = dynamic_cast<const DdcgAst::StringPat*>(p)) {
        os << "str(" << sp->value << ")";
    } else if (auto* bp = dynamic_cast<const DdcgAst::BindPat*>(p)) {
        // BindPats with different names are still the same rule — we
        // alpha-equate them via a fixed token.
        (void)bp;
        os << "bind";
    } else {
        os << "?";
    }
}

void check_duplicates(Ctx& c) {
    std::map<std::string, const DdcgAst::Rule*> seen;
    for (auto* r : c.file->rules) {
        std::ostringstream sig;
        for (auto* h : r->heads) { serialize_pattern(h, sig); sig << "|"; }
        sig << "@";
        if (r->guard.has_value()) serialize_expr(*r->guard, sig);
        else sig << "_";
        std::string key = sig.str();
        if (seen.count(key)) {
            err(c, r->loc, "duplicate rule (same heads + same guard) — earlier "
                            "definition at line " +
                            std::to_string(seen[key]->loc.line));
        } else {
            seen[key] = r;
        }
    }
}

// ── Seed ─────────────────────────────────────────────────────

void seed_context(Ctx& c) {
    for (auto* imp : c.file->imports) c.import_names.insert(imp->name);

    for (auto* d : c.file->destinations) {
        std::string dname;
        std::vector<DdcgAst::DestVariant*> variants;
        bool is_data = false;
        if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
            dname = dd->name; variants = dd->variants; is_data = true;
            if (!c.data_dest_name.empty()) {
                err(c, c.file->loc,
                    "multiple data_dest declarations: '" + c.data_dest_name +
                    "' and '" + dname + "' (the paper's δ is a single tagged "
                    "union; declare alternatives as variants of one data_dest)");
            } else {
                c.data_dest_name = dname;
            }
        } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
            dname = cd->name; variants = cd->variants;
            if (!c.ctrl_dest_name.empty()) {
                err(c, c.file->loc,
                    "multiple ctrl_dest declarations: '" + c.ctrl_dest_name +
                    "' and '" + dname + "' (γ is a single tagged union)");
            } else {
                c.ctrl_dest_name = dname;
            }
        }
        (void)is_data;
        for (auto* v : variants) {
            AuxSig sig;
            sig.param_count = static_cast<int>(v->fields.size());
            for (auto* f : v->fields) sig.param_types.push_back(type_from_dsl_typeref(c, f->ty));
            sig.return_type = Type::dest(dname);
            c.dest_variants[v->name] = std::move(sig);
            c.dest_variant_owner[v->name] = dname;
        }
    }
    // Both destinations are mandatory — the paper's CG signature
    // (E → ρ → δ → γ → Code) has no degenerate forms.
    if (c.data_dest_name.empty()) {
        err(c, c.file->loc,
            "DESTINATIONS must declare exactly one data_dest "
            "(the paper's δ; e.g. `data_dest delta { effect }`)");
    }
    if (c.ctrl_dest_name.empty()) {
        err(c, c.file->loc,
            "DESTINATIONS must declare exactly one ctrl_dest "
            "(the paper's γ; e.g. `ctrl_dest gamma { fail }`)");
    }

    for (auto* a : c.file->auxiliaries) {
        AuxSig sig;
        sig.param_count = static_cast<int>(a->param_types.size());
        for (auto* tr : a->param_types) sig.param_types.push_back(type_from_dsl_typeref(c, tr));
        sig.return_type = type_from_dsl_typeref(c, a->return_ty);
        c.auxes[a->name] = std::move(sig);
    }
    for (auto* p : c.file->predicates) {
        AuxSig sig;
        sig.param_count = static_cast<int>(p->param_types.size());
        for (auto* tr : p->param_types) sig.param_types.push_back(type_from_dsl_typeref(c, tr));
        sig.return_type = Type::bool_();
        c.preds[p->name] = std::move(sig);
    }

    c.push_scope();
    for (const auto& n : c.import_names)        c.bind(n, NameKind::SchemaModule, Type::unknown());
    // Only zero-payload dest variants are values you can name bare —
    // `effect`, `ac`, `fail`. Variants with payload fields exist only
    // as constructors (`restore(0)`, `stack(slot)`); naming one bare
    // is an arity error, not a value reference.
    for (const auto& [n, sig] : c.dest_variants) {
        if (sig.param_count == 0) c.bind(n, NameKind::DestVariant, sig.return_type);
    }
    for (const auto& [n, sig] : c.auxes)         c.bind(n, NameKind::Auxiliary, sig.return_type);
    for (const auto& [n, sig] : c.preds)         c.bind(n, NameKind::Predicate, Type::bool_());
}

// Walk each head in its own throwaway scope so we can compare the
// bind-sets across heads of a multi-pattern rule. Returns the captured
// bindings (var name → type) per head, in source order.
std::vector<std::map<std::string, Type>>
gather_head_binds(Ctx& c, const DdcgAst::Rule* r) {
    std::vector<std::map<std::string, Type>> out;
    for (auto* p : r->heads) {
        c.push_scope();
        check_pattern(c, p, Type::unknown());
        std::map<std::string, Type> binds;
        for (const auto& [n, b] : c.stack.back()) {
            if (b.kind == NameKind::PatternBind) binds[n] = b.ty;
        }
        c.pop_scope();
        out.push_back(std::move(binds));
    }
    return out;
}

void check_rule(Ctx& c, const DdcgAst::Rule* r) {
    auto head_binds = gather_head_binds(c, r);

    if (head_binds.size() > 1) {
        const auto& first = head_binds.front();
        for (size_t i = 1; i < head_binds.size(); ++i) {
            const auto& other = head_binds[i];
            if (first.size() != other.size()) {
                err(c, r->heads[i]->loc,
                    "multi-pattern rule heads must bind the same variables; "
                    "head 1 binds " + std::to_string(first.size()) +
                    ", head " + std::to_string(i + 1) + " binds " +
                    std::to_string(other.size()));
                continue;
            }
            for (const auto& [n, t] : first) {
                auto it = other.find(n);
                if (it == other.end()) {
                    err(c, r->heads[i]->loc,
                        "multi-pattern head " + std::to_string(i + 1) +
                        " is missing variable '" + n + "' bound by head 1");
                } else if (!assignable(t, it->second) ||
                           !assignable(it->second, t)) {
                    err(c, r->heads[i]->loc,
                        "multi-pattern head " + std::to_string(i + 1) +
                        " binds '" + n + "' as " + it->second.display() +
                        " but head 1 binds it as " + t.display());
                }
            }
        }
    }

    c.push_scope();
    if (!head_binds.empty()) {
        for (const auto& [n, t] : head_binds.front()) {
            c.bind(n, NameKind::PatternBind, t);
        }
    }
    if (r->guard.has_value()) {
        Type gt = infer_expr(c, *r->guard);
        if (!gt.is_unknown() && gt.kind != TypeKind::Bool)
            err(c, (*r->guard)->loc, "where-guard must be bool, got " + gt.display());
    }
    Type ir_root_t = type_from_dsl_typeref(c, c.file->ir_root);
    check_block(c, r->body, ir_root_t);
    c.pop_scope();
}

} // namespace

CheckResult check_file(const DdcgAst::File* file,
                        const std::map<std::string, Schema>& schemas) {
    CheckResult result;
    Ctx c;
    c.file = file;
    c.schemas = &schemas;
    c.result = &result;

    seed_context(c);

    for (auto* imp : file->imports) {
        if (schemas.find(imp->name) == schemas.end()) {
            err(c, imp->loc, "import '" + imp->name + "' declared but no schema "
                              "supplied for it");
        }
    }

    for (auto* r : file->rules) check_rule(c, r);

    HeadIndex idx;
    index_heads(file, idx);
    check_coverage(c, idx);
    check_overlap(c, idx);
    check_duplicates(c);

    return result;
}

} // namespace ddcgc
