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

bool assignable(const Type& expected, const Type& value,
                 const std::map<std::string, Schema>* schemas) {
    if (expected.is_unknown() || value.is_unknown()) return true;
    if (value.kind == TypeKind::NilLit && expected.kind == TypeKind::Schema) return true;
    if (expected.kind == TypeKind::NilLit && value.kind == TypeKind::Schema) return true;
    // Structural compatibility through containers: an Unknown element
    // type matches any concrete one (used for empty list literals,
    // `[]` typed as `list<?>`, vs a concrete `list<int>`).
    if (expected.kind == TypeKind::List && value.kind == TypeKind::List) {
        if (expected.elems.empty() || value.elems.empty()) return true;
        return assignable(expected.elems[0], value.elems[0], schemas);
    }
    if (expected.kind == TypeKind::Tuple && value.kind == TypeKind::Tuple) {
        if (expected.elems.size() != value.elems.size()) return false;
        for (size_t i = 0; i < expected.elems.size(); ++i) {
            if (!assignable(expected.elems[i], value.elems[i], schemas)) return false;
        }
        return true;
    }
    if (expected == value) return true;
    // Schema constructor → parent-sum subtyping. C++ inherits the upcast
    // automatically (Imm* → Value*); this teaches the typechecker to
    // recognise it. Only checked when caller passes a schemas map.
    if (schemas &&
        expected.kind == TypeKind::Schema && value.kind == TypeKind::Schema &&
        expected.schema_import == value.schema_import) {
        auto sit = schemas->find(value.schema_import);
        if (sit != schemas->end()) {
            auto cit = sit->second.constructors.find(value.name);
            if (cit != sit->second.constructors.end() &&
                cit->second.sum_name == expected.name) {
                return true;
            }
        }
    }
    return false;
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
    Fun,            // action-language `fun` definition
    FunParam,       // bound inside a fun body
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
    // Action-language `fun` declarations. Same shape as auxes (typed
    // signature) but the body is action-language code defined in the
    // .ddcg, not opaque target code in MEMBERS.
    std::map<std::string, AuxSig> funs;
    // Per-fun parameter names (for binding inside the fun body when
    // type-checking — funs has only the types, not the names).
    std::map<std::string, std::vector<std::string>> fun_param_names;
    std::map<std::string, AuxSig> dest_variants;   // ctor_name -> arity + arg types
    std::map<std::string, std::string> dest_variant_owner;  // ctor_name -> dest name (e.g. "gamma")
    // BindPat nodes whose name resolves to an asdl-enum constructor —
    // these are constants in pattern position, not variable binds.
    // Populated by check_pattern; consulted by serialize_pattern so the
    // duplicate / overlap checks treat them as distinguishing values
    // rather than alpha-equating them all as `bind`.
    std::set<const DdcgAst::BindPat*> enum_value_bindpats;
    // The .ddcg's data_dest and ctrl_dest names (paper's δ and γ).
    // Set during collect_decls; empty string means missing.
    std::string data_dest_name;
    std::string ctrl_dest_name;
    std::string env_dest_name;  // empty when no env_dest declared

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

// Return the common asdl-sum name of two Schema types (their parent
// in the constructor → sum hierarchy), or empty if they don't share
// one. Used to widen sibling constructors in match arm joins.
std::string common_schema_sum(const Ctx& c, const Type& a, const Type& b) {
    if (a.kind != TypeKind::Schema || b.kind != TypeKind::Schema) return "";
    if (a.schema_import != b.schema_import) return "";
    auto sit = c.schemas->find(a.schema_import);
    if (sit == c.schemas->end()) return "";
    auto get_sum = [&](const std::string& n) -> std::string {
        if (sit->second.sum_names.count(n)) return n;
        auto cit = sit->second.constructors.find(n);
        if (cit != sit->second.constructors.end()) return cit->second.sum_name;
        return "";
    };
    std::string sa = get_sum(a.name);
    std::string sb = get_sum(b.name);
    return (sa == sb && !sa.empty()) ? sa : "";
}

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
    else if (field_type == "int64")      base = Type::int_();
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

        // Module-qualified schema reference: module.Constructor.
        if (!tn->module.empty()) {
            auto sit = c.schemas->find(tn->module);
            if (sit != c.schemas->end()) {
                const Schema& sch = sit->second;
                if (sch.sum_names.count(n) || sch.constructors.count(n)) {
                    return Type::schema(tn->module, n);
                }
            }
            return Type::unknown_named(tn->module + "." + n);
        }

        // `ir_root` is a placeholder that resolves to the importing
        // file's declared ir_root type — lets shared libraries write
        // signatures like `make_X : (..., ir_root) -> ir_root` without
        // baking in a specific schema/sum name. Each consumer declares
        // their own `ir_root <type>` in DESTINATIONS; the spliced lib's
        // typerefs resolve through that, no contract on naming.
        if (n == "ir_root") {
            if (c.file->ir_root) {
                return type_from_dsl_typeref(c, c.file->ir_root);
            }
            return Type::unknown_named("ir_root");
        }

        if (n == "int")        return Type::int_();
        if (n == "string")     return Type::str();
        if (n == "bool")       return Type::bool_();
        if (n == "ident" ||
            n == "identifier") return Type::ident();
        if (n == "label")      return Type::label();
        if (n == "void")       return Type::voidty();

        // Closed-sum tagged-union type? (data_dest / ctrl_dest /
        // env_dest are δ/γ/ρ; user `sum` decls are arbitrary closed
        // sums declared in the DESTINATIONS section. All four share
        // the Type::Dest representation — the name disambiguates and
        // the only thing the dispatcher signature sees specially is
        // the file-level data_dest_name / ctrl_dest_name / env_dest_
        // name strings.)
        for (auto* d : c.file->destinations) {
            if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
                if (dd->name == n) return Type::dest(dd->name);
            } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
                if (cd->name == n) return Type::dest(cd->name);
            } else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
                if (ed->name == n) return Type::dest(ed->name);
            } else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) {
                if (sd->name == n) return Type::dest(sd->name);
            }
        }
        // Schema sum/ctor — search loaded schemas. Multiple schemas
        // matching is a hard error (ambiguous); the typechecker pushes
        // an error and returns Unknown so a sensible message wins.
        const Schema* hit = nullptr;
        std::string hit_imp;
        for (const auto& [imp, schema] : *c.schemas) {
            if (schema.sum_names.count(n) || schema.constructors.count(n)) {
                if (hit) {
                    err(c, tr->loc, "typeref '" + n +
                        "' is ambiguous (defined in '" + hit_imp +
                        "' and '" + imp + "') — qualify as <module>." + n);
                    return Type::unknown_named(n);
                }
                hit = &schema; hit_imp = imp;
            }
        }
        if (hit) return Type::schema(hit_imp, n);
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
        // Bare-name constructor pattern (no module prefix) — must be
        // matching a dest tagged-union variant.
        if (cp->module.empty()) {
            auto vit = c.dest_variants.find(cp->ctor);
            if (vit == c.dest_variants.end()) {
                err(c, pat->loc, "unknown dest variant: " + cp->ctor);
                return;
            }
            // Validate dest type matches if known.
            if (expected.kind == TypeKind::Dest) {
                const std::string& owner = c.dest_variant_owner[cp->ctor];
                if (owner != expected.name) {
                    err(c, pat->loc, "variant '" + cp->ctor + "' belongs to " +
                                     owner + ", not matched subject type " +
                                     expected.display());
                }
            }
            // Walk fields. Find each named field in the dest_variant
            // declaration to recover its type.
            const DdcgAst::DestVariant* variant = nullptr;
            for (auto* d : c.file->destinations) {
                std::vector<DdcgAst::DestVariant*> vs;
                if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) vs = dd->variants;
                else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) vs = cd->variants;
                else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) vs = ed->variants;
                else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) vs = sd->variants;
                for (auto* v : vs) if (v->name == cp->ctor) { variant = v; break; }
                if (variant) break;
            }
            std::set<std::string> seen;
            for (auto* fp : cp->fields) {
                std::string fname;
                DdcgAst::Pattern* sub = nullptr;
                if (auto* nf = dynamic_cast<DdcgAst::NamedFieldPat*>(fp)) {
                    fname = nf->name; sub = nf->pat;
                } else if (auto* rf = dynamic_cast<DdcgAst::RestFieldPat*>(fp)) {
                    err(c, pat->loc, "rest pattern not supported on dest variants");
                    fname = rf->name;
                }
                const DdcgAst::DestField* field = nullptr;
                if (variant) {
                    for (auto* f : variant->fields) {
                        if (f->name == fname) { field = f; break; }
                    }
                }
                if (!field) {
                    err(c, pat->loc, "dest variant '" + cp->ctor +
                                     "' has no field '" + fname + "'");
                    continue;
                }
                if (!seen.insert(fname).second) {
                    err(c, pat->loc, "duplicate field '" + fname + "' in pattern");
                }
                Type fty = type_from_dsl_typeref(c, field->ty);
                if (sub) check_pattern(c, sub, fty);
            }
            return;
        }
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
        // Enum-value disambiguation: if the field's type is an asdl
        // enum sum and the bind name matches one of its constructors,
        // treat as a constant match (no binding) — same shape the
        // MatchExpr arm code uses for zero-arg dest variants.
        if (expected.kind == TypeKind::Schema) {
            auto sit = c.schemas->find(expected.schema_import);
            if (sit != c.schemas->end() &&
                sit->second.enum_sums.count(expected.name)) {
                auto cit = sit->second.sum_constructors.find(expected.name);
                if (cit != sit->second.sum_constructors.end()) {
                    for (const auto& cn : cit->second) {
                        if (cn == bp->name) {
                            c.enum_value_bindpats.insert(bp);
                            return;
                        }
                    }
                }
            }
        }
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

// Forward decls so MatchExpr's body-walking code can call them.
void check_stmt(Ctx& c, DdcgAst::Stmt* s, const Type& tail_expected, bool is_tail);
void check_block(Ctx& c, const std::vector<DdcgAst::Stmt*>& body,
                  const Type& tail_expected);

// Bidirectional refinement: walk expr e and re-record any
// polymorphic empty-list literals (`[]` → list<unknown>) with the
// concrete `expected` type. Recurses through Ternary and BlockExpr
// since those just forward their tail's type — the empty-list site
// is what needs the concrete element type for codegen. Stops at
// any expression that already has a concrete type or is a
// non-forwarding form (calls, lookups, etc.).
inline void refine_polymorphic(Ctx& c, DdcgAst::Expr* e, const Type& expected) {
    if (!e) return;
    if (expected.kind != TypeKind::List) return;
    if (expected.elems.empty() || expected.elems[0].is_unknown()) return;

    // Always descend into forwarding forms (ternary/block) — their
    // outer type may already be concrete from the other branch while
    // an inner ListLit is still polymorphic.
    if (auto* t = dynamic_cast<DdcgAst::TernaryExpr*>(e)) {
        auto it = c.result->expr_types.find(e);
        if (it != c.result->expr_types.end() && it->second.kind == TypeKind::List &&
            (it->second.elems.empty() || it->second.elems[0].is_unknown())) {
            it->second = expected;
        }
        refine_polymorphic(c, t->then_, expected);
        refine_polymorphic(c, t->else_, expected);
        return;
    }
    if (auto* be = dynamic_cast<DdcgAst::BlockExpr*>(e)) {
        auto it = c.result->expr_types.find(e);
        if (it != c.result->expr_types.end() && it->second.kind == TypeKind::List &&
            (it->second.elems.empty() || it->second.elems[0].is_unknown())) {
            it->second = expected;
        }
        if (!be->body.empty()) {
            if (auto* es = dynamic_cast<DdcgAst::ExprStmt*>(be->body.back()))
                refine_polymorphic(c, es->value, expected);
        }
        return;
    }
    auto it = c.result->expr_types.find(e);
    if (it == c.result->expr_types.end()) return;
    Type& cur = it->second;
    if (cur.kind != TypeKind::List) return;
    if (!cur.elems.empty() && !cur.elems[0].is_unknown()) return;
    if (dynamic_cast<DdcgAst::ListLit*>(e)) {
        cur = expected;
    }
}

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
                    if (!assignable(sig.param_types[i], arg_types[i], c.schemas)) {
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
        // Fun? (action-language function defined in the .ddcg)
        if (auto it = c.funs.find(fn_id->name); it != c.funs.end()) {
            const auto& sig = it->second;
            if ((int)arg_types.size() != sig.param_count) {
                err(c, call->loc, "fun '" + fn_id->name + "' expects " +
                                  std::to_string(sig.param_count) + " arg(s), got " +
                                  std::to_string(arg_types.size()));
            } else {
                for (size_t i = 0; i < arg_types.size(); ++i) {
                    if (!assignable(sig.param_types[i], arg_types[i], c.schemas)) {
                        err(c, call->args[i]->loc,
                            "fun '" + fn_id->name + "' arg " +
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
                    if (!assignable(sig.param_types[i], arg_types[i], c.schemas)) {
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
                    if (!assignable(sig.param_types[i], arg_types[i], c.schemas)) {
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
            // Last fallback: maybe it's an asdl enum-sum value (e.g.,
            // DtRef from `datatype = DtByte | DtShort | DtInt | DtRef`).
            // Walk all schemas; if exactly one enum sum has a matching
            // ctor, type as that sum.
            std::vector<std::pair<std::string, std::string>> matches;
            for (const auto& [imp, schema] : *c.schemas) {
                for (const auto& enum_sum : schema.enum_sums) {
                    auto cit = schema.sum_constructors.find(enum_sum);
                    if (cit == schema.sum_constructors.end()) continue;
                    for (const auto& cn : cit->second) {
                        if (cn == id->name) {
                            matches.emplace_back(imp, enum_sum);
                            break;
                        }
                    }
                }
            }
            if (matches.size() == 1) {
                ret = Type::schema(matches[0].first, matches[0].second);
            } else if (matches.size() > 1) {
                std::string msg = "ambiguous enum value '" + id->name + "': ";
                for (size_t i = 0; i < matches.size(); ++i) {
                    if (i) msg += ", ";
                    msg += matches[i].first + "." + matches[i].second;
                }
                err(c, id->loc, msg + " (qualify with module.Name)");
                ret = Type::unknown();
            } else {
                err(c, id->loc, "unknown name: " + id->name);
                ret = Type::unknown();
            }
        }
    }
    else if (auto* t = dynamic_cast<DdcgAst::TernaryExpr*>(e)) {
        Type ct = infer_expr(c, t->cond);
        if (!ct.is_unknown() && ct.kind != TypeKind::Bool) {
            err(c, t->cond->loc, "ternary condition must be bool, got " + ct.display());
        }
        Type tt = infer_expr(c, t->then_);
        Type ft = infer_expr(c, t->else_);
        // Sibling-constructor widening: two distinct constructors of
        // the same asdl sum (e.g. `ir.LoadConst` vs `ir.GetStatic`)
        // are both assignable to the parent sum (`ir.node`); the
        // ternary's result type is that parent.
        std::string common_sum = common_schema_sum(c, tt, ft);
        if (!assignable(tt, ft, c.schemas) && !assignable(ft, tt, c.schemas) &&
            common_sum.empty()) {
            err(c, e->loc, "ternary branches have incompatible types: " +
                           tt.display() + " vs " + ft.display());
        }
        if (!common_sum.empty() && tt.name != ft.name) {
            ret = Type::schema(tt.schema_import, common_sum);
        } else {
            // Pick the more-concrete branch type. NilLit and empty-elem
            // List (e.g. `[]`) are polymorphic — they widen to whichever
            // branch carries a real type.
            bool tt_concrete = !tt.is_unknown() && tt.kind != TypeKind::NilLit
                && !(tt.kind == TypeKind::List && (tt.elems.empty() ||
                                                    tt.elems[0].is_unknown()));
            ret = tt_concrete ? tt : ft;
        }
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
            if (!lt.is_unknown() && !rt.is_unknown() && !assignable(lt, rt, c.schemas) && !assignable(rt, lt, c.schemas))
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
            else if (!assignable(lt, rt, c.schemas) && !assignable(rt, lt, c.schemas))
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
        // Module-qualified enum-sum value: parser produces FieldAccExpr
        // for `module.Name`. When the base IdentExpr binds to a schema
        // import (NameKind::SchemaModule) and Name is one of that
        // schema's enum-sum constructors, resolve as that sum's value
        // (e.g. `ir.Eq` → ir.cmpop.Eq). The base is purely a namespace
        // marker, so we don't go through infer_expr on it.
        if (auto* base_id = dynamic_cast<DdcgAst::IdentExpr*>(fa->base)) {
            const auto* b = c.lookup(base_id->name);
            if (b && b->kind == NameKind::SchemaModule) {
                auto sit = c.schemas->find(base_id->name);
                if (sit != c.schemas->end()) {
                    const Schema& schema = sit->second;
                    for (const auto& enum_sum : schema.enum_sums) {
                        auto cit = schema.sum_constructors.find(enum_sum);
                        if (cit == schema.sum_constructors.end()) continue;
                        for (const auto& cn : cit->second) {
                            if (cn == fa->field) {
                                ret = Type::schema(base_id->name, enum_sum);
                                record(c, e, ret);
                                return ret;
                            }
                        }
                    }
                    err(c, fa->loc, "module '" + base_id->name +
                        "' has no enum-sum value '" + fa->field + "'");
                    ret = Type::unknown();
                    record(c, e, ret);
                    return ret;
                }
            }
        }
        Type bt = infer_expr(c, fa->base);
        if (bt.kind == TypeKind::List && fa->field == "count") {
            // List intrinsic: lists are emitted as { T* data; int count; ...}
            // structs (C) or std::vector (C++); both have a `count`/`size`
            // accessor. The backend remaps the field name as needed.
            ret = Type::int_();
            record(c, e, ret);
            return ret;
        }
        if (bt.kind == TypeKind::Dest) {
            // Field access on a destination (paper's δ/γ/ρ). All three
            // are tagged unions: a field is reachable when it appears
            // on at least one variant; if multiple variants name the
            // same field they must agree on type. Variant tag isn't
            // checked statically — runtime correctness (e.g. only
            // access γ.Lf when γ is `pair`) is the rule body's concern,
            // typically by destructuring via match.
            Type common; bool found = false; bool conflict = false;
            for (auto* d : c.file->destinations) {
                if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
                    if (dd->name != bt.name) continue;
                    for (auto* v : dd->variants)
                        for (auto* f : v->fields)
                            if (f->name == fa->field) {
                                Type t = type_from_dsl_typeref(c, f->ty);
                                if (!found) { common = t; found = true; }
                                else if (!(common == t)) conflict = true;
                            }
                    break;
                } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
                    if (cd->name != bt.name) continue;
                    for (auto* v : cd->variants)
                        for (auto* f : v->fields)
                            if (f->name == fa->field) {
                                Type t = type_from_dsl_typeref(c, f->ty);
                                if (!found) { common = t; found = true; }
                                else if (!(common == t)) conflict = true;
                            }
                    break;
                } else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
                    if (ed->name != bt.name) continue;
                    for (auto* v : ed->variants)
                        for (auto* f : v->fields)
                            if (f->name == fa->field) {
                                Type t = type_from_dsl_typeref(c, f->ty);
                                if (!found) { common = t; found = true; }
                                else if (!(common == t)) conflict = true;
                            }
                    break;
                } else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) {
                    if (sd->name != bt.name) continue;
                    for (auto* v : sd->variants)
                        for (auto* f : v->fields)
                            if (f->name == fa->field) {
                                Type t = type_from_dsl_typeref(c, f->ty);
                                if (!found) { common = t; found = true; }
                                else if (!(common == t)) conflict = true;
                            }
                    break;
                }
            }
            if (conflict) {
                err(c, fa->loc, "field '" + fa->field + "' has conflicting types "
                                "across variants of " + bt.display());
                ret = Type::unknown();
            } else if (!found) {
                err(c, fa->loc, "destination type " + bt.display() +
                                " has no variant with field '" + fa->field + "'");
                ret = Type::unknown();
            } else {
                ret = common;
            }
        }
        else if (bt.kind == TypeKind::Schema) {
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
                if (!assignable(first, t, c.schemas) && !assignable(t, first, c.schemas)) {
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
                    if (!assignable(sig.param_types[i], at, c.schemas)) {
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
        if (!st.is_unknown() && st.kind != TypeKind::Schema)
            err(c, g->subject->loc, "gen subject must be a schema-typed AST node, got " + st.display());

        // Arg-slot layout depends on whether the file declares an
        // env_dest (paper's ρ). With env_dest, layout is (ρ, δ, γ, [Lnext]);
        // without it, (δ, γ, [Lnext]). Count disambiguates.
        std::string env_name;
        for (auto* d : c.file->destinations) {
            if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
                env_name = ed->name; break;
            }
        }
        size_t n = g->args.size();
        size_t expected_min = env_name.empty() ? 2 : 3;
        size_t expected_max = expected_min + 1;
        if (n < expected_min || n > expected_max) {
            err(c, g->loc, "gen expects " + std::to_string(expected_min) +
                " or " + std::to_string(expected_max) + " arg(s) after subject" +
                (env_name.empty() ? " (δ, γ, [Lnext])"
                                  : " (ρ, δ, γ, [Lnext])") +
                ", got " + std::to_string(n));
        }
        size_t idx = 0;
        if (!env_name.empty() && n >= expected_min) {
            Type rt = infer_expr(c, g->args[idx]);
            if (!assignable(Type::dest(env_name), rt, c.schemas)) {
                err(c, g->args[idx]->loc,
                    "gen environment must be a " + env_name +
                    " value, got " + rt.display());
            }
            idx++;
        }
        if (idx < n) {
            Type dd = infer_expr(c, g->args[idx]);
            if (!c.data_dest_name.empty()) {
                Type expected_dd = Type::dest(c.data_dest_name);
                if (!assignable(expected_dd, dd, c.schemas)) {
                    err(c, g->args[idx]->loc,
                        "gen data destination must be a " + c.data_dest_name +
                        " variant, got " + dd.display());
                }
            }
            idx++;
        }
        if (idx < n) {
            Type cd = infer_expr(c, g->args[idx]);
            if (!c.ctrl_dest_name.empty()) {
                Type expected_cd = Type::dest(c.ctrl_dest_name);
                if (!assignable(expected_cd, cd, c.schemas)) {
                    err(c, g->args[idx]->loc,
                        "gen control destination must be a " + c.ctrl_dest_name +
                        " variant, got " + cd.display());
                }
            }
            idx++;
        }
        if (idx < n) {
            // Lnext slot — typecheck but don't constrain (it's an ir.node).
            infer_expr(c, g->args[idx]);
        }
        ret = type_from_dsl_typeref(c, c.file->ir_root);
    }
    else if (auto* w = dynamic_cast<DdcgAst::WithExpr*>(e)) {
        Type bt = infer_expr(c, w->base);
        Type vt = infer_expr(c, w->value);
        if (bt.kind != TypeKind::Dest) {
            err(c, w->loc, "with-update base must be an env-typed value, got " + bt.display());
            ret = Type::unknown();
        } else {
            // Find the env_dest by name; locate the field; verify value type.
            DdcgAst::EnvDest* env = nullptr;
            for (auto* d : c.file->destinations) {
                if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d); ed && ed->name == bt.name) {
                    env = ed; break;
                }
            }
            if (!env) {
                err(c, w->loc, "with-update applies only to env_dest values, not " + bt.display());
                ret = Type::unknown();
            } else {
                DdcgAst::DestField* field = nullptr;
                bool conflict = false;
                Type field_t;
                for (auto* v : env->variants) {
                    for (auto* f : v->fields) {
                        if (f->name == w->field) {
                            Type t = type_from_dsl_typeref(c, f->ty);
                            if (!field) { field = f; field_t = t; }
                            else if (!(field_t == t)) conflict = true;
                        }
                    }
                }
                if (!field) {
                    err(c, w->loc, "env_dest '" + bt.name + "' has no field '" + w->field + "'");
                    ret = bt;  // best-effort: result keeps base type
                } else if (conflict) {
                    err(c, w->loc, "with-update field '" + w->field +
                        "' has conflicting types across variants of '" + bt.name + "'");
                    ret = bt;
                } else {
                    if (!assignable(field_t, vt, c.schemas)) {
                        err(c, w->value->loc, "with-update of '" + w->field +
                            "': expected " + field_t.display() + ", got " + vt.display());
                    }
                    ret = bt;  // result is same env type as base
                }
            }
        }
    }
    else if (auto* b = dynamic_cast<DdcgAst::BuildExpr*>(e)) {
        // Resolve schema → asdl-loaded module.
        auto sit = c.schemas->find(b->schema);
        if (sit == c.schemas->end()) {
            err(c, b->loc, "build: unknown schema module '" + b->schema + "'");
            for (auto* a : b->args) infer_expr(c, a);
            ret = Type::unknown();
        } else {
            const Schema& s = sit->second;
            auto ci = s.constructors.find(b->ctor);
            if (ci == s.constructors.end()) {
                err(c, b->loc, "build: unknown constructor '" + b->schema +
                               "." + b->ctor + "'");
                for (auto* a : b->args) infer_expr(c, a);
                ret = Type::unknown();
            } else {
                const Constructor& ctor = ci->second;
                if (b->args.size() != ctor.fields.size()) {
                    err(c, b->loc, "build " + b->schema + "." + b->ctor +
                                   ": expected " +
                                   std::to_string(ctor.fields.size()) +
                                   " arg(s), got " +
                                   std::to_string(b->args.size()));
                }
                size_t n = std::min(b->args.size(), ctor.fields.size());
                for (size_t i = 0; i < n; ++i) {
                    Type at = infer_expr(c, b->args[i]);
                    Type ft = type_from_field(c, b->schema,
                                              ctor.fields[i].type_name,
                                              ctor.fields[i].flag);
                    if (!assignable(ft, at, c.schemas)) {
                        err(c, b->args[i]->loc,
                            "build " + b->schema + "." + b->ctor + " arg " +
                            std::to_string(i + 1) + " ('" +
                            ctor.fields[i].name + "'): expected " +
                            ft.display() + ", got " + at.display());
                    }
                }
                // Type-check any extra args so cached expr_types are populated.
                for (size_t i = n; i < b->args.size(); ++i) infer_expr(c, b->args[i]);
                ret = Type::schema(b->schema, b->ctor);
            }
        }
    }
    else if (auto* be = dynamic_cast<DdcgAst::BlockExpr*>(e)) {
        // `{ stmts; tail_expr; }` evaluates the stmts in sequence and
        // yields the trailing ExprStmt's value. Same shape as a
        // match arm body or a fun body. Push a scope so let-bindings
        // inside don't leak out.
        c.push_scope();
        check_block(c, be->body, /*tail_expected=*/Type::unknown());
        Type ret_t = Type::unknown();
        if (!be->body.empty()) {
            if (auto* es = dynamic_cast<DdcgAst::ExprStmt*>(be->body.back())) {
                auto it = c.result->expr_types.find(es->value);
                if (it != c.result->expr_types.end()) ret_t = it->second;
            } else {
                err(c, be->loc, "block expression must end in an expression "
                                "(found a non-tail statement)");
            }
        } else {
            err(c, be->loc, "empty block expression has no value");
        }
        c.pop_scope();
        ret = ret_t;
    }
    else if (auto* m = dynamic_cast<DdcgAst::MatchExpr*>(e)) {
        Type st = infer_expr(c, m->subject);

        // Exhaustiveness check.
        // Collect variant names matched and any wildcard/bind catch-all.
        // A BindPat whose name matches a zero-arg dest variant (or an
        // enum-sum constructor) of the subject's type is treated as a
        // constant match, not a name-bind — so users can write
        // `effect => …` (dest) or `Add => …` (asdl enum) without parens.
        bool subj_is_enum = false;
        const Schema* enum_schema = nullptr;
        std::vector<std::string> enum_ctors;
        if (st.kind == TypeKind::Schema) {
            auto sit = c.schemas->find(st.schema_import);
            if (sit != c.schemas->end() && sit->second.enum_sums.count(st.name)) {
                subj_is_enum = true;
                enum_schema = &sit->second;
                if (auto eit = sit->second.sum_constructors.find(st.name);
                    eit != sit->second.sum_constructors.end()) {
                    enum_ctors = eit->second;
                }
            }
        }

        std::set<std::string> matched_variants;
        bool has_catchall = false;
        for (auto* arm : m->arms) {
            if (dynamic_cast<DdcgAst::WildcardPat*>(arm->pat)) {
                has_catchall = true;
            } else if (auto* bp = dynamic_cast<DdcgAst::BindPat*>(arm->pat)) {
                bool is_constant = false;
                if (st.kind == TypeKind::Dest) {
                    auto vit = c.dest_variants.find(bp->name);
                    if (vit != c.dest_variants.end() &&
                        vit->second.param_count == 0 &&
                        c.dest_variant_owner[bp->name] == st.name) {
                        matched_variants.insert(bp->name);
                        is_constant = true;
                    }
                } else if (subj_is_enum) {
                    for (const auto& cn : enum_ctors) {
                        if (cn == bp->name) {
                            matched_variants.insert(bp->name);
                            is_constant = true;
                            break;
                        }
                    }
                }
                if (!is_constant) has_catchall = true;
            } else if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(arm->pat)) {
                if (cp->module.empty()) matched_variants.insert(cp->ctor);
            }
        }
        if (!has_catchall && st.kind == TypeKind::Dest) {
            // Walk this dest's variants; report any not matched.
            for (auto* d : c.file->destinations) {
                std::string dname;
                std::vector<DdcgAst::DestVariant*> variants;
                if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
                    dname = dd->name; variants = dd->variants;
                } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
                    dname = cd->name; variants = cd->variants;
                } else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
                    dname = ed->name; variants = ed->variants;
                } else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) {
                    dname = sd->name; variants = sd->variants;
                }
                if (dname != st.name) continue;
                for (auto* v : variants) {
                    if (!matched_variants.count(v->name)) {
                        err(c, m->loc, "non-exhaustive match on " + st.display() +
                                       ": variant '" + v->name + "' not covered");
                    }
                }
            }
        }
        if (!has_catchall && subj_is_enum) {
            (void)enum_schema;
            for (const auto& cn : enum_ctors) {
                if (!matched_variants.count(cn)) {
                    err(c, m->loc, "non-exhaustive match on " + st.display() +
                                   ": value '" + cn + "' not covered");
                }
            }
        }

        // Walk each arm in its own scope; the join of arm types is the
        // match's overall type.
        Type result_t = Type::unknown();
        for (auto* arm : m->arms) {
            c.push_scope();
            check_pattern(c, arm->pat, st);
            // Body's tail-expression type becomes the arm's value.
            check_block(c, arm->body, /*tail_expected=*/Type::unknown());
            // Pull the tail expression type from the cache and merge
            // it with the running join. Sibling constructors of the
            // same asdl sum widen to the sum (e.g. `target.Imm` and
            // `target.Slot` both fold up to `target.value`).
            if (!arm->body.empty()) {
                if (auto* es = dynamic_cast<DdcgAst::ExprStmt*>(arm->body.back())) {
                    auto it = c.result->expr_types.find(es->value);
                    if (it != c.result->expr_types.end()) {
                        const Type& at = it->second;
                        if (result_t.is_unknown()) {
                            result_t = at;
                        } else if (result_t.kind == TypeKind::NilLit &&
                                   at.kind != TypeKind::NilLit) {
                            // First arm yielded `nil`, this one is
                            // concrete — widen to the concrete type.
                            // (`nil` is the polymorphic bottom for
                            // schema/dest values, so the join is
                            // always the non-nil arm's type.)
                            result_t = at;
                        } else if (assignable(result_t, at, c.schemas)) {
                            // arm fits running join — keep result_t.
                        } else if (assignable(at, result_t, c.schemas)) {
                            // running join is sub-type of this arm — widen.
                            result_t = at;
                        } else if (auto sum = common_schema_sum(c, result_t, at);
                                   !sum.empty()) {
                            result_t = Type::schema(result_t.schema_import, sum);
                        } else {
                            err(c, arm->loc, "match arm type " +
                                at.display() + " differs from earlier " +
                                result_t.display());
                        }
                    }
                }
            }
            c.pop_scope();
        }
        ret = result_t;
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
                if (sb->ty.has_value() && !assignable(bt, vt, c.schemas)) {
                    err(c, let->loc, "let bind '" + sb->name + "': expected " +
                                     bt.display() + ", got " + vt.display());
                }
                // Bidirectional refinement for empty-list RHS: the
                // literal `[]` produces `list<?>`; if the bind annotates
                // a concrete list type, contextualise the RHS type so
                // the codegen has enough info to emit `std::vector<T>{}`.
                if (sb->ty.has_value()) {
                    refine_polymorphic(c, let->value, bt);
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
        if (!assignable(b->ty, vt, c.schemas)) {
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
        if (f->iter_index.has_value()) {
            if (auto* ib = dynamic_cast<DdcgAst::SimpleBind*>(*f->iter_index)) {
                c.bind(ib->name, NameKind::ForIter, Type::int_());
            }
        }
        if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(f->iter)) {
            c.bind(sb->name, NameKind::ForIter, elem_t);
        }
        check_block(c, f->body, /*tail_expected=*/Type::voidty());
        c.pop_scope();
        return;
    }
    if (auto* ifs = dynamic_cast<DdcgAst::IfStmt*>(s)) {
        Type ct = infer_expr(c, ifs->cond);
        if (!ct.is_unknown() && ct.kind != TypeKind::Bool) {
            err(c, ifs->loc, "if condition must be bool, got " + ct.display());
        }
        c.push_scope();
        check_block(c, ifs->then_body, /*tail_expected=*/Type::voidty());
        c.pop_scope();
        c.push_scope();
        check_block(c, ifs->else_body, /*tail_expected=*/Type::voidty());
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
            !assignable(tail_expected, vt, c.schemas)) {
            err(c, es->loc, "tail expression: expected " + tail_expected.display() +
                            ", got " + vt.display());
        }
        if (is_tail && tail_expected.kind != TypeKind::Void) {
            refine_polymorphic(c, es->value, tail_expected);
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

    // The DSL's `DESUGARED` block lets the .ddcg
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

    // Coverage applies only to sums the .ddcg actually dispatches on.
    // A schema may have multiple sums (e.g. expr, stmt, plus container
    // types like grammar/header that the consumer's frontend handles
    // directly); we only demand exhaustive coverage of sums whose
    // constructors appear as rule heads.
    std::set<std::string> covered_sums;
    for (const auto& [k, rules] : idx) {
        (void)rules;
        if (k.first != mod) continue;
        auto cit = sit->second.constructors.find(k.second);
        if (cit != sit->second.constructors.end()) {
            covered_sums.insert(cit->second.sum_name);
        }
    }

    for (const auto& [sum_name, ctor_list] : sit->second.sum_constructors) {
        if (covered_sums.count(sum_name) == 0) continue;
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

void serialize_pattern(const DdcgAst::Pattern* p, std::ostringstream& os,
                       const std::set<const DdcgAst::BindPat*>& enum_bps);

// Compute a structural-shape signature for a head, ignoring bind-variable
// names. Two heads with the same signature are interchangeable — only
// guards or rule order can distinguish them. Differing signatures mean
// one rule is a specialisation of the other (nested ConstructorPats,
// IntPat / StringPat / NilPat value checks, enum-value field-pats) —
// PEG ordered choice handles the disambiguation, no warning warranted.
std::string head_shape_signature(const DdcgAst::Pattern* p,
                                 const std::set<const DdcgAst::BindPat*>& enum_bps) {
    std::ostringstream os;
    serialize_pattern(p, os, enum_bps);
    return os.str();
}

void check_overlap(Ctx& c, const HeadIndex& idx) {
    for (const auto& [k, rules] : idx) {
        if (rules.size() <= 1) continue;

        // Group by head-shape signature. Within each group, every rule's
        // pattern is structurally equivalent (modulo bind names) so the
        // disambiguation is purely on guards and order.
        std::map<std::string, std::vector<const DdcgAst::Rule*>> by_shape;
        for (auto* r : rules) {
            for (auto* h : r->heads) {
                if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h)) {
                    if (cp->module == k.first && cp->ctor == k.second) {
                        by_shape[head_shape_signature(h, c.enum_value_bindpats)].push_back(r);
                        break;
                    }
                }
            }
        }

        for (const auto& [shape, group] : by_shape) {
            (void)shape;
            if (group.size() <= 1) continue;

            // The only statically-decidable bad case: an unguarded rule
            // appears before any other rule with the same shape. Under
            // PEG ordered choice the unguarded rule fires first and the
            // others can never run.
            //
            // All-guarded with same shape is unknowable — the predicates
            // could be disjoint, overlapping, or never both true. We
            // can't tell, so we don't warn; that'd just be a "please
            // check" note.
            //
            // Multiple unguarded with same shape is duplicate territory
            // and is reported by check_duplicates as an error.
            for (size_t i = 0; i + 1 < group.size(); ++i) {
                if (group[i]->guard.has_value()) continue;
                std::ostringstream msg;
                msg << "rule for head '" << k.first << "." << k.second
                    << "' is unguarded and precedes other rules with the "
                       "same shape — those later rules are unreachable";
                warn(c, group[i]->loc, msg.str());
                break;
            }
        }
    }
}

// Cheap textual signature for a guard expression — used to detect rules
// that are flat-out duplicates (same head, same guard text).
void serialize_expr(const DdcgAst::Expr* e, std::ostringstream& os);
void serialize_pattern(const DdcgAst::Pattern* p, std::ostringstream& os,
                       const std::set<const DdcgAst::BindPat*>& enum_bps);

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

void serialize_pattern(const DdcgAst::Pattern* p, std::ostringstream& os,
                       const std::set<const DdcgAst::BindPat*>& enum_bps) {
    if (auto* cp = dynamic_cast<const DdcgAst::ConstructorPat*>(p)) {
        os << cp->module << "." << cp->ctor << "(";
        for (size_t i = 0; i < cp->fields.size(); ++i) {
            if (i) os << ",";
            if (auto* nf = dynamic_cast<const DdcgAst::NamedFieldPat*>(cp->fields[i])) {
                os << nf->name << ":";
                serialize_pattern(nf->pat, os, enum_bps);
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
        // BindPats whose name resolves to an asdl-enum constructor are
        // constants — distinguish them by name. Plain value-binds still
        // alpha-equate to a single token.
        if (enum_bps.count(bp)) os << "enum(" << bp->name << ")";
        else                    os << "bind";
    } else {
        os << "?";
    }
}

void check_duplicates(Ctx& c) {
    std::map<std::string, const DdcgAst::Rule*> seen;
    for (auto* r : c.file->rules) {
        std::ostringstream sig;
        for (auto* h : r->heads) {
            serialize_pattern(h, sig, c.enum_value_bindpats);
            sig << "|";
        }
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
        } else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
            dname = ed->name; variants = ed->variants;
            if (!c.env_dest_name.empty()) {
                err(c, c.file->loc,
                    "multiple env_dest declarations: '" + c.env_dest_name +
                    "' and '" + dname + "' (ρ is a single tagged union)");
            } else {
                c.env_dest_name = dname;
            }
        } else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) {
            // User sum: arbitrary closed tagged union, not a δ/γ/ρ.
            // No uniqueness constraint — any number of sums can co-
            // exist. Just register the variants for ctor lookup +
            // match dispatch, same machinery as the dests.
            dname = sd->name; variants = sd->variants;
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
    // Action-language `fun` declarations. Register signatures up front
    // so funs can call each other (forward references, mutual recursion).
    // Bodies are type-checked separately after seed_context, so all
    // sig types are known before any body is walked.
    for (auto* f : c.file->funs) {
        AuxSig sig;
        sig.param_count = static_cast<int>(f->params.size());
        std::vector<std::string> names;
        names.reserve(f->params.size());
        for (auto* p : f->params) {
            sig.param_types.push_back(type_from_dsl_typeref(c, p->ty));
            names.push_back(p->name);
        }
        sig.return_type = type_from_dsl_typeref(c, f->ret_ty);
        c.funs[f->name] = std::move(sig);
        c.fun_param_names[f->name] = std::move(names);
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
    for (const auto& [n, sig] : c.funs)          c.bind(n, NameKind::Fun, sig.return_type);
}

// Type-check one action-language fun body. Pushes a scope with the
// fun's parameters bound, walks the body stmts requiring the trailing
// expression to match the declared return type.
// ── Where-guard purity ──────────────────────────────────────
//
// A `where <expr>` clause runs during dispatch try — once per rule
// attempt, at every site that pattern-matches the rule's head shape.
// Side effects in a guard (e.g. an AUX call that allocates a SIR
// temp, mutates ctx state, or records a try-region) fire on every
// rule-try regardless of whether the rule eventually wins, producing
// silent slot leaks, misordered allocations, or duplicate accumulator
// entries. The DSL distinguishes PREDICATES (`pred`, bool-returning,
// declared pure) from AUXILIARIES (arbitrary side-effecting); the
// purity rule below enforces that the guard expression uses only
// PRED calls + pure operators on bound variables / literals.
//
// Allowed:
//   - literals (Int/String/Bool/Nil)
//   - IdentExpr (refers to a let-bind / fun-param / pattern-bind /
//     `node` / loaded schema module — never side-effecting)
//   - BinOp / UnaryOp / TernaryExpr (operators have no side effects)
//   - CallExpr where fn is a PRED (declared pure by signature)
//   - DestPattern (constructs a δ/γ/ρ value — pure construction)
//   - FieldAccExpr / IndexAccExpr (field / index access on already-
//     bound values)
//   - TupleLit / ListLit (collecting pure values)
//
// Rejected:
//   - CallExpr to an AUX or FUN (could side-effect)
//   - GenCall (recursive cg always emits SIR — large side effect)
//   - BuildExpr (constructs SIR via arena alloc — side effect)
//   - WithExpr (functional update — semantically pure but only
//     useful in spine construction; rejecting flags as a likely bug)
//   - MatchExpr (arm bodies are stmt blocks; analysing them recursively
//     is fine in principle but rejecting as a guard form is the
//     conservative choice — guards should be flat boolean predicates)
//   - BlockExpr (contains stmts — likely side-effecting)
bool is_pure_guard_expr(Ctx& c, DdcgAst::Expr* e);

bool is_pure_guard_expr(Ctx& c, DdcgAst::Expr* e) {
    if (!e) return true;
    if (dynamic_cast<DdcgAst::IntLit*>(e)) return true;
    if (dynamic_cast<DdcgAst::StringLit*>(e)) return true;
    if (dynamic_cast<DdcgAst::BoolLit*>(e)) return true;
    if (dynamic_cast<DdcgAst::NilLit*>(e)) return true;
    if (dynamic_cast<DdcgAst::IdentExpr*>(e)) return true;
    if (auto* t = dynamic_cast<DdcgAst::TernaryExpr*>(e)) {
        return is_pure_guard_expr(c, t->cond) &&
               is_pure_guard_expr(c, t->then_) &&
               is_pure_guard_expr(c, t->else_);
    }
    if (auto* b = dynamic_cast<DdcgAst::BinOp*>(e)) {
        return is_pure_guard_expr(c, b->left) &&
               is_pure_guard_expr(c, b->right);
    }
    if (auto* u = dynamic_cast<DdcgAst::UnaryOp*>(e)) {
        return is_pure_guard_expr(c, u->operand);
    }
    if (auto* call = dynamic_cast<DdcgAst::CallExpr*>(e)) {
        // Only PRED calls are accepted. AUX / FUN calls may have
        // side effects and must not appear in a guard.
        auto* fn_id = dynamic_cast<DdcgAst::IdentExpr*>(call->fn);
        if (!fn_id) return false;
        if (c.preds.find(fn_id->name) == c.preds.end()) return false;
        for (auto* arg : call->args) {
            if (!is_pure_guard_expr(c, arg)) return false;
        }
        return true;
    }
    if (auto* fa = dynamic_cast<DdcgAst::FieldAccExpr*>(e)) {
        return is_pure_guard_expr(c, fa->base);
    }
    if (auto* ix = dynamic_cast<DdcgAst::IndexAccExpr*>(e)) {
        return is_pure_guard_expr(c, ix->base) &&
               is_pure_guard_expr(c, ix->index);
    }
    if (auto* tu = dynamic_cast<DdcgAst::TupleLit*>(e)) {
        for (auto* el : tu->elems) if (!is_pure_guard_expr(c, el)) return false;
        return true;
    }
    if (auto* l = dynamic_cast<DdcgAst::ListLit*>(e)) {
        for (auto* el : l->elems) if (!is_pure_guard_expr(c, el)) return false;
        return true;
    }
    if (auto* dp = dynamic_cast<DdcgAst::DestPattern*>(e)) {
        for (auto* arg : dp->args) if (!is_pure_guard_expr(c, arg)) return false;
        return true;
    }
    return false;
}

void check_guard_purity(Ctx& c, DdcgAst::Expr* guard) {
    if (!is_pure_guard_expr(c, guard)) {
        err(c, guard->loc,
            "where-guard expression must be pure (only PREDICATE calls, "
            "pure operators, bound idents, and literals allowed). AUX or "
            "FUN calls may have side effects that fire on every rule-try, "
            "regardless of whether the rule wins dispatch — and that's "
            "almost always a bug. Move the side-effecting work into the "
            "rule body.");
    }
}

void check_fun(Ctx& c, DdcgAst::Fun* f) {
    auto sig_it = c.funs.find(f->name);
    if (sig_it == c.funs.end()) return;  // already errored at decl time
    const AuxSig& sig = sig_it->second;
    const auto& names = c.fun_param_names[f->name];

    c.push_scope();
    for (size_t i = 0; i < f->params.size(); ++i) {
        c.bind(names[i], NameKind::FunParam, sig.param_types[i]);
    }
    check_block(c, f->body, /*tail_expected=*/sig.return_type);
    c.pop_scope();
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
                } else if (!assignable(t, it->second, c.schemas) ||
                           !assignable(it->second, t, c.schemas)) {
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
    // Bind the dispatcher's incoming destinations under their declared
    // names so rule bodies can write `gen(e, delta, gamma)` to propagate
    // the rule's own δ/γ to recursive calls (per Dybvig 1990 §3.2). The
    // names match the .ddcg's `data_dest <name>` / `ctrl_dest <name>`
    // declarations exactly. `Lnext` is the paper Figure-8 fall-through
    // hint; bound to the ir_root type and defaults to {} at the call.
    if (!c.data_dest_name.empty()) {
        c.bind(c.data_dest_name, NameKind::PatternBind,
               Type::dest(c.data_dest_name));
    }
    if (!c.ctrl_dest_name.empty()) {
        c.bind(c.ctrl_dest_name, NameKind::PatternBind,
               Type::dest(c.ctrl_dest_name));
    }
    if (!c.env_dest_name.empty()) {
        c.bind(c.env_dest_name, NameKind::PatternBind,
               Type::dest(c.env_dest_name));
    }
    if (c.file->ir_root) {
        c.bind("Lnext", NameKind::PatternBind,
               type_from_dsl_typeref(c, c.file->ir_root));
    }
    // Bind `node` to the rule head's owning sum type so rule bodies
    // can pass the matched AST node to AUXes that need it (sema
    // sidecar lookups keyed by node pointer, etc.). Multi-pattern
    // rules dispatch on a single sum, so `node`'s type is well-
    // defined; mismatched sums are flagged at the gather_head_binds
    // pass.
    {
        std::string node_module, node_sum;
        for (auto* h : r->heads) {
            if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h)) {
                auto sit = c.schemas->find(cp->module);
                if (sit == c.schemas->end()) continue;
                auto cit = sit->second.constructors.find(cp->ctor);
                if (cit == sit->second.constructors.end()) continue;
                if (node_sum.empty()) {
                    node_module = cp->module;
                    node_sum = cit->second.sum_name;
                }
            }
        }
        if (!node_sum.empty()) {
            c.bind("node", NameKind::PatternBind,
                   Type::schema(node_module, node_sum));
        }
    }
    if (r->guard.has_value()) {
        Type gt = infer_expr(c, *r->guard);
        if (!gt.is_unknown() && gt.kind != TypeKind::Bool)
            err(c, (*r->guard)->loc, "where-guard must be bool, got " + gt.display());
        check_guard_purity(c, *r->guard);
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

    for (auto* f : file->funs)  check_fun(c, f);
    for (auto* r : file->rules) check_rule(c, r);

    HeadIndex idx;
    index_heads(file, idx);
    check_coverage(c, idx);
    check_overlap(c, idx);
    check_duplicates(c);

    return result;
}

} // namespace ddcgc
