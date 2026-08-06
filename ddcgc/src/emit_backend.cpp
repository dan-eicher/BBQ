// ddcgc — emit-backend Template Method base.
//
// See emit_backend.h for the role split. This TU implements the
// language-agnostic glue: pre-emit setup (ir_root resolution, source
// schema binding, dest name extraction) and the rule-discovery
// helpers. Subclasses do the actual code emission via emit_body().

#include "emit_backend.h"

#include <set>
#include <sstream>

namespace ddcgc {

namespace {

// Resolve the data_dest, ctrl_dest, and (optional) env_dest names
// from the .ddcg's DESTINATIONS block. data and ctrl are mandatory
// per the paper's CG signature. env_dest is optional — empty
// names mean "no ρ" and dispatcher signatures stay 4-arg.
struct DestNames {
    std::string data_name, data_type;
    std::string ctrl_name, ctrl_type;
    std::string env_name, env_type;
};

DestNames extract_dest_names(const DdcgAst::File* file,
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
    if (out.data_type.empty())
        errors.push_back("no data_dest declared (the paper's δ)");
    if (out.ctrl_type.empty())
        errors.push_back("no ctrl_dest declared (the paper's γ)");
    return out;
}

} // anonymous namespace

bool EmitBackend::emit(const DdcgAst::File* file,
                       const std::map<std::string, Schema>& schemas,
                       const CheckResult& check,
                       std::ostream& out_header,
                       std::ostream* out_source,
                       const std::string& header_filename,
                       std::vector<std::string>& errors) {
    file_ = file;
    schemas_ = &schemas;
    check_ = &check;
    errors_ = &errors;
    out_header_ = &out_header;
    out_source_ = out_source;
    header_filename_ = header_filename;
    out_ = &out_header;     // backends without a split write everything here
    indent_depth_ = 0;

    // Resolve ir_root.
    if (auto* tn = dynamic_cast<DdcgAst::TypeName*>(file->ir_root)) {
        bool resolved = false;
        if (!tn->module.empty()) {
            // Module-qualified — must hit the named schema, no
            // first-wins fallback (silent mis-resolve risk when
            // multiple imports share a constructor name).
            auto sit = schemas.find(tn->module);
            if (sit == schemas.end()) {
                errors.push_back("ir_root references unknown schema '" +
                                 tn->module + "'");
                return false;
            }
            const Schema& sch = sit->second;
            if (sch.sum_names.count(tn->name) ||
                sch.constructors.count(tn->name)) {
                ir_root_type_ = type_for_schema(sch, tn->name);
                resolved = true;
            } else {
                errors.push_back("ir_root '" + tn->module + "." + tn->name +
                                 "' not found in schema '" + tn->module + "'");
                return false;
            }
        } else {
            // Unqualified — fall back to "any schema that has it"
            // when the user opted out of module qualification.
            // Primitive types (`int`) and runtime-supplied names
            // land here too.
            for (const auto& [imp, sch] : schemas) {
                (void)imp;
                if (sch.sum_names.count(tn->name) ||
                    sch.constructors.count(tn->name)) {
                    ir_root_type_ = type_for_schema(sch, tn->name);
                    resolved = true;
                    break;
                }
            }
        }
        if (!resolved) {
            ir_root_type_ = tn->name;
            if (tn->is_pointer) ir_root_type_ += "*";
        }
    } else {
        errors.push_back("ir_root must be a simple TypeName (list/tuple roots not supported)");
        return false;
    }

    // Bind source schema (first import).
    if (file->imports.empty()) {
        errors.push_back("at least one import is required (the source AST)");
        return false;
    }
    source_import_ = file->imports[0]->name;
    auto sit = schemas.find(source_import_);
    if (sit == schemas.end()) {
        errors.push_back("source import '" + source_import_ +
                         "' has no loaded schema");
        return false;
    }
    source_schema_ = &sit->second;

    // Pull dest names + types.
    DestNames dn = extract_dest_names(file, errors);
    if (dn.data_type.empty() || dn.ctrl_type.empty()) return false;
    data_dest_type_ = std::move(dn.data_type);
    ctrl_dest_type_ = std::move(dn.ctrl_type);
    env_dest_type_  = std::move(dn.env_type);
    data_dest_name_ = std::move(dn.data_name);
    ctrl_dest_name_ = std::move(dn.ctrl_name);
    env_dest_name_  = std::move(dn.env_name);

    return emit_body();
}

// ── Helpers (language-agnostic) ──────────────────────────────

std::string EmitBackend::sum_for_subject(DdcgAst::Expr* subject) const {
    auto it = check_->expr_types.find(subject);
    if (it == check_->expr_types.end()) return {};
    const Type& t = it->second;
    if (t.kind != TypeKind::Schema) return {};
    if (t.schema_import != source_import_) return {};
    auto ci = source_schema_->constructors.find(t.name);
    if (ci != source_schema_->constructors.end()) return ci->second.sum_name;
    if (source_schema_->sum_names.count(t.name)) return t.name;
    return {};
}

bool EmitBackend::is_aux(const std::string& name) const {
    for (auto* a : file_->auxiliaries) if (a->name == name) return true;
    return false;
}

bool EmitBackend::is_pred(const std::string& name) const {
    for (auto* p : file_->predicates) if (p->name == name) return true;
    return false;
}

bool EmitBackend::is_dest_variant(const std::string& name) const {
    for (auto* d : file_->destinations) {
        if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) {
            for (auto* v : dd->variants) if (v->name == name) return true;
        } else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) {
            for (auto* v : cd->variants) if (v->name == name) return true;
        } else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) {
            for (auto* v : ed->variants) if (v->name == name) return true;
        } else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) {
            for (auto* v : sd->variants) if (v->name == name) return true;
        }
    }
    return false;
}

bool EmitBackend::find_enum_value(const std::string& name,
                                   std::string* out_module,
                                   std::string* out_sum) const {
    for (const auto& [imp, schema] : *schemas_) {
        (void)imp;
        for (const auto& enum_sum : schema.enum_sums) {
            auto cit = schema.sum_constructors.find(enum_sum);
            if (cit == schema.sum_constructors.end()) continue;
            for (const auto& cn : cit->second) {
                if (cn == name) {
                    // Return the asdl module_name (e.g. "sir"), not the
                    // DSL import alias (e.g. "ir"). c_tag_value /
                    // cpp_class_name and friends key on module_name.
                    if (out_module) *out_module = schema.module_name;
                    if (out_sum)    *out_sum = enum_sum;
                    return true;
                }
            }
        }
    }
    return false;
}

bool EmitBackend::is_zero_arg_dest_variant(const std::string& name) const {
    for (auto* d : file_->destinations) {
        std::vector<DdcgAst::DestVariant*> vs;
        if (auto* dd = dynamic_cast<DdcgAst::DataDest*>(d)) vs = dd->variants;
        else if (auto* cd = dynamic_cast<DdcgAst::CtrlDest*>(d)) vs = cd->variants;
        else if (auto* ed = dynamic_cast<DdcgAst::EnvDest*>(d)) vs = ed->variants;
        else if (auto* sd = dynamic_cast<DdcgAst::Sum*>(d)) vs = sd->variants;
        for (auto* v : vs) if (v->name == name) return v->fields.empty();
    }
    return false;
}

std::vector<DdcgAst::Rule*>
EmitBackend::rules_for(const std::string& sum_name) const {
    std::vector<DdcgAst::Rule*> out;
    for (auto* r : file_->rules) {
        for (auto* h : r->heads) {
            if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h)) {
                if (cp->module != source_import_) continue;
                auto it = source_schema_->constructors.find(cp->ctor);
                if (it == source_schema_->constructors.end()) continue;
                if (it->second.sum_name != sum_name) continue;
                out.push_back(r);
                break;
            }
        }
    }
    return out;
}

std::vector<std::string>
EmitBackend::source_sums_with_rules() const {
    std::vector<std::string> sums;
    std::set<std::string> seen;
    for (auto* r : file_->rules) {
        for (auto* h : r->heads) {
            if (auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(h)) {
                if (cp->module != source_import_) continue;
                auto it = source_schema_->constructors.find(cp->ctor);
                if (it == source_schema_->constructors.end()) continue;
                const std::string& sum = it->second.sum_name;
                if (seen.insert(sum).second) sums.push_back(sum);
            }
        }
    }
    return sums;
}

// ── Shared walks (dispatch chains) ────────────────────────────

void EmitBackend::emit_call_args(const std::vector<DdcgAst::Expr*>& args) {
    out() << "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) out() << ", ";
        emit_expr(args[i]);
    }
    out() << ")";
}

std::string EmitBackend::emit_expr_to_string(DdcgAst::Expr* e) {
    std::ostringstream oss;
    std::ostream* save = out_;
    out_ = &oss;
    emit_expr(e);
    out_ = save;
    return oss.str();
}

// emit_expr dispatches on AST node type. Forms with identical
// syntax across both languages emit directly; the divergent ones
// go through virtual hooks (emit_call, emit_list_lit, ...).
void EmitBackend::emit_expr(DdcgAst::Expr* e) {
    if (auto* il = dynamic_cast<DdcgAst::IntLit*>(e)) {
        out() << il->value;
        return;
    }
    if (auto* sl = dynamic_cast<DdcgAst::StringLit*>(e)) {
        out() << "\"";
        for (char ch : sl->value) {
            switch (ch) {
                case '\\': out() << "\\\\"; break;
                case '"':  out() << "\\\""; break;
                case '\n': out() << "\\n";  break;
                case '\r': out() << "\\r";  break;
                case '\t': out() << "\\t";  break;
                default:   out() << ch;     break;
            }
        }
        out() << "\"";
        return;
    }
    if (auto* bl = dynamic_cast<DdcgAst::BoolLit*>(e)) {
        out() << (bl->value ? "true" : "false");
        return;
    }
    if (dynamic_cast<DdcgAst::NilLit*>(e)) {
        out() << null_literal();
        return;
    }
    if (auto* id = dynamic_cast<DdcgAst::IdentExpr*>(e)) {
        // Check shadowing first: if a let-bind or fun param of the
        // same name is in scope, the typechecker recorded the
        // IdentExpr's resolved type as something OTHER than a Dest.
        // Without this check, `let effect = ...` shadowed by the
        // 0-arg `effect` dest variant emits as `effect(ctx)` at the
        // use site instead of the let-bound local.
        bool shadowed_dest = false;
        {
            auto sit = check_->expr_types.find(id);
            if (sit != check_->expr_types.end() &&
                sit->second.kind != TypeKind::Dest) {
                shadowed_dest = true;
            }
        }
        if (!shadowed_dest && is_zero_arg_dest_variant(id->name)) {
            // Zero-arg dest variants written bare in DSL still emit
            // as a constructor call. Route through emit_dest_pattern
            // so C/C++ backends apply the same ctx-prepending
            // convention they use for parenthesized calls.
            DdcgAst::DestPattern dp(id->name, {});
            emit_dest_pattern(&dp);
            return;
        }
        // typecheck resolved this IdentExpr to a Schema(import, sum)
        // where `sum` is in the schema's enum_sums and the ctor list
        // contains id->name iff this is an asdl enum value reference
        // (and not a scope-bound name with coincidentally-overlapping
        // enum value name elsewhere).
        auto tit = check_->expr_types.find(id);
        if (tit != check_->expr_types.end() &&
            tit->second.kind == TypeKind::Schema) {
            auto sit = schemas_->find(tit->second.schema_import);
            if (sit != schemas_->end() &&
                sit->second.enum_sums.count(tit->second.name)) {
                auto cit = sit->second.sum_constructors.find(tit->second.name);
                if (cit != sit->second.sum_constructors.end()) {
                    for (const auto& cn : cit->second) {
                        if (cn == id->name) {
                            out() << emit_enum_value_ref(
                                sit->second.module_name,
                                tit->second.name, id->name);
                            return;
                        }
                    }
                }
            }
        }
        out() << id->name;
        return;
    }
    if (auto* t = dynamic_cast<DdcgAst::TernaryExpr*>(e)) {
        // When branches are sibling schema-ctors that widen to a
        // common parent sum (per typecheck.cpp's
        // common_schema_sum), C++ won't auto-upcast distinct pointer
        // types in a ternary — emit an explicit cast on each branch
        // to the result type so `(cond ? new A() : new B())` compiles.
        std::string result_ty;
        auto rti = check_->expr_types.find(e);
        auto thi = check_->expr_types.find(t->then_);
        auto eli = check_->expr_types.find(t->else_);
        if (rti != check_->expr_types.end() &&
            thi != check_->expr_types.end() &&
            eli != check_->expr_types.end() &&
            rti->second.kind == TypeKind::Schema &&
            !(thi->second == rti->second &&
              eli->second == rti->second)) {
            result_ty = ternary_branch_cast(rti->second);
        }
        out() << "(";
        emit_expr(t->cond);
        out() << " ? ";
        if (!result_ty.empty()) out() << "(" << result_ty << ")";
        emit_expr(t->then_);
        out() << " : ";
        if (!result_ty.empty()) out() << "(" << result_ty << ")";
        emit_expr(t->else_);
        out() << ")";
        return;
    }
    if (auto* b = dynamic_cast<DdcgAst::BinOp*>(e)) {
        std::string op = b->op;
        if (op == "and") op = "&&";
        else if (op == "or") op = "||";
        if (op == "+") {
            // List concat lowers to a per-language helper call.
            auto it = check_->expr_types.find(e);
            if (it != check_->expr_types.end() &&
                it->second.kind == TypeKind::List) {
                emit_list_concat(e, b->left, b->right);
                return;
            }
        }
        // String/ident equality. In C strings flow as `const char*`
        // and `==` is pointer-compare — must dispatch through strcmp
        // (with a null-safe pointer-equality short-circuit). In C++
        // `std::string` has the right `==` operator natively. The
        // backend tells us which language we're in via the
        // string_eq_via_strcmp() hook.
        if ((op == "==" || op == "!=") && string_eq_via_strcmp()) {
            auto lt = check_->expr_types.find(b->left);
            auto rt = check_->expr_types.find(b->right);
            bool both_strings =
                lt != check_->expr_types.end() &&
                rt != check_->expr_types.end() &&
                (lt->second.kind == TypeKind::String ||
                 lt->second.kind == TypeKind::Ident) &&
                (rt->second.kind == TypeKind::String ||
                 rt->second.kind == TypeKind::Ident);
            if (both_strings) {
                // Null-safe string compare: optional-ident fields
                // (e.g. Break.label) may be NULL. Pointer-equality
                // covers same-pointer / both-NULL; strcmp only runs
                // when both sides are non-NULL.
                std::string l = emit_expr_to_string(b->left);
                std::string r = emit_expr_to_string(b->right);
                std::string base = "(((" + l + ") == (" + r + ")) || "
                                   "((" + l + ") && (" + r + ") && "
                                   "strcmp((" + l + "), (" + r + ")) == 0))";
                out() << (op == "==" ? base : "(!" + base + ")");
                return;
            }
        }
        out() << "(";
        emit_expr(b->left);
        out() << " " << op << " ";
        emit_expr(b->right);
        out() << ")";
        return;
    }
    if (auto* u = dynamic_cast<DdcgAst::UnaryOp*>(e)) {
        std::string op = u->op;
        if (op == "not") op = "!";
        out() << "(" << op;
        emit_expr(u->operand);
        out() << ")";
        return;
    }
    if (auto* call = dynamic_cast<DdcgAst::CallExpr*>(e)) {
        emit_call(call);
        return;
    }
    if (auto* fa = dynamic_cast<DdcgAst::FieldAccExpr*>(e)) {
        // Module-qualified enum-sum value: typecheck stamps the
        // FieldAccExpr's type as the sum's schema. Detect by the base
        // IdentExpr's name being a loaded schema module (purely a
        // namespace marker, not a value). Emit via the per-language
        // enum_value_ref hook (e.g. C: SIR_EQ). The hook expects the
        // asdl `module_name` (used for tag prefixes), not the import
        // alias the consumer wrote.
        if (auto* base_id = dynamic_cast<DdcgAst::IdentExpr*>(fa->base)) {
            if (schemas_) {
                auto sit = schemas_->find(base_id->name);
                if (sit != schemas_->end()) {
                    auto self_it = check_->expr_types.find(e);
                    if (self_it != check_->expr_types.end() &&
                        self_it->second.kind == TypeKind::Schema) {
                        out() << emit_enum_value_ref(sit->second.module_name,
                                                      self_it->second.name,
                                                      fa->field);
                        return;
                    }
                }
            }
        }
        if (emit_seq_field_acc(fa)) return;
        emit_expr(fa->base);
        // Pointer-typed bases use ->; value-typed use .  Mapping is
        // language-agnostic — both backends use the same Type→sep
        // rule. (TypeKind::Schema is by-pointer; primitives, Dest,
        // and Tuple are by-value.)
        const char* sep = "->";
        bool base_is_list = false;
        auto it = check_->expr_types.find(fa->base);
        if (it != check_->expr_types.end()) {
            switch (it->second.kind) {
                case TypeKind::Dest:
                case TypeKind::Int:
                case TypeKind::String:
                case TypeKind::Bool:
                case TypeKind::Ident:
                case TypeKind::Label:
                case TypeKind::Tuple:
                case TypeKind::List:
                    sep = ".";
                    break;
                default: break;
            }
            if (it->second.kind == TypeKind::List) base_is_list = true;
        }
        // List intrinsic: `.count` is the length accessor in the
        // .ddcg DSL. C lists carry it as a struct field; std::vector
        // exposes it via .size(). Remap on the emit side.
        if (base_is_list && fa->field == "count" && !c_style_list_count()) {
            out() << sep << "size()";
        } else {
            out() << sep << fa->field;
        }
        return;
    }
    if (auto* ix = dynamic_cast<DdcgAst::IndexAccExpr*>(e)) {
        emit_index_acc(ix);
        return;
    }
    if (auto* l = dynamic_cast<DdcgAst::ListLit*>(e)) {
        emit_list_lit(l);
        return;
    }
    if (auto* tu = dynamic_cast<DdcgAst::TupleLit*>(e)) {
        emit_tuple_lit(tu);
        return;
    }
    if (auto* dp = dynamic_cast<DdcgAst::DestPattern*>(e)) {
        emit_dest_pattern(dp);
        return;
    }
    if (auto* w = dynamic_cast<DdcgAst::WithExpr*>(e)) {
        emit_with_expr(w);
        return;
    }
    if (auto* b = dynamic_cast<DdcgAst::BuildExpr*>(e)) {
        emit_build(b);
        return;
    }
    if (auto* m = dynamic_cast<DdcgAst::MatchExpr*>(e)) {
        emit_match_expr(m);
        return;
    }
    if (auto* be = dynamic_cast<DdcgAst::BlockExpr*>(e)) {
        emit_block_expr(be);
        return;
    }
    if (auto* g = dynamic_cast<DdcgAst::GenCall*>(e)) {
        emit_gen_call(g);
        return;
    }
}

void EmitBackend::emit_block_expr(DdcgAst::BlockExpr* be) {
    // GCC statement-expression `({ stmts; tail_expr; })`. Both the
    // C and C++ backends are GCC/clang only, so the extension is
    // available on both. The trailing ExprStmt's value is the
    // block's value; preceding stmts run for their side effects.
    out() << "({\n";
    indent_depth_++;
    for (size_t i = 0; i + 1 < be->body.size(); ++i) {
        emit_stmt(be->body[i], /*is_tail=*/false);
    }
    if (!be->body.empty()) {
        if (auto* es = dynamic_cast<DdcgAst::ExprStmt*>(be->body.back())) {
            indent();
            emit_expr(es->value);
            out() << ";\n";
        } else {
            // Caught by typecheck; emit a placeholder.
            indent() << "0;\n";
        }
    } else {
        indent() << "0;\n";
    }
    indent_depth_--;
    indent() << "})";
}

void EmitBackend::emit_stmt(DdcgAst::Stmt* s, bool is_tail) {
    if (auto* let = dynamic_cast<DdcgAst::LetStmt*>(s)) {
        if (let->binds.size() == 1) {
            if (auto* sb = dynamic_cast<DdcgAst::SimpleBind*>(let->binds[0])) {
                emit_let_simple(let, sb);
            } else {
                indent() << "(void)";
                emit_expr(let->value);
                out() << ";\n";
            }
            return;
        }
        emit_let_destructure(let);
        return;
    }
    if (auto* ma = dynamic_cast<DdcgAst::MutAssign*>(s)) {
        // Simple assignment — same syntax in both languages.
        indent() << ma->name << " = ";
        emit_expr(ma->value);
        out() << ";\n";
        return;
    }
    if (auto* f = dynamic_cast<DdcgAst::ForStmt*>(s)) {
        emit_for_stmt(f);
        return;
    }
    if (auto* ifs = dynamic_cast<DdcgAst::IfStmt*>(s)) {
        // Same as the rule-guard site: emit_expr already parenthesises
        // a binary operator, so adding the if's own pair would give
        // `if ((a == b))` — rejected by clang under -Werror.
        const bool self_parens =
            dynamic_cast<DdcgAst::BinOp*>(ifs->cond) != nullptr;
        indent() << "if ";
        if (!self_parens) out() << "(";
        emit_expr(ifs->cond);
        if (!self_parens) out() << ")";
        out() << " {\n";
        indent_depth_++;
        emit_stmts(ifs->then_body, /*tail=*/false);
        indent_depth_--;
        if (!ifs->else_body.empty()) {
            indent() << "} else {\n";
            indent_depth_++;
            emit_stmts(ifs->else_body, /*tail=*/false);
            indent_depth_--;
        }
        indent() << "}\n";
        return;
    }
    if (auto* lb = dynamic_cast<DdcgAst::LabelStmt*>(s)) {
        emit_label_stmt(lb);
        return;
    }
    if (auto* es = dynamic_cast<DdcgAst::ExprStmt*>(s)) {
        indent();
        if (is_tail) {
            // Match-arm bodies (with match_target_var_ set) assign
            // to the result variable; everything else returns.
            if (!match_target_var_.empty()) {
                out() << match_target_var_ << " = ";
            } else {
                out() << "return ";
            }
        }
        emit_expr(es->value);
        out() << ";\n";
        return;
    }
}

void EmitBackend::emit_stmts(const std::vector<DdcgAst::Stmt*>& body,
                              bool body_is_tail_position) {
    for (size_t i = 0; i < body.size(); ++i) {
        bool is_last = (i + 1 == body.size());
        emit_stmt(body[i], body_is_tail_position && is_last);
    }
}

void EmitBackend::emit_rule_branch(const Schema& schema,
                                    const Constructor& ctor,
                                    DdcgAst::Pattern* head,
                                    DdcgAst::Rule* rule) {
    auto* cp = dynamic_cast<DdcgAst::ConstructorPat*>(head);
    if (!cp) return;
    (void)ctor;

    int name_ctr = 0;
    int blocks_open = emit_match(schema, cp, "node", name_ctr);

    if (rule->guard.has_value()) {
        // emit_expr parenthesises a binary operator to fix precedence,
        // so a top-level one already reads `(a == b)`. Adding the if's
        // own pair gives `if ((a == b))`, which clang rejects under
        // -Werror as an equality meant to be an assignment
        // (-Wparentheses-equality). gcc has no such warning, which is
        // why the -Werror e2e targets never caught it.
        const bool self_parens =
            dynamic_cast<DdcgAst::BinOp*>(*rule->guard) != nullptr;
        indent() << "if ";
        if (!self_parens) out() << "(";
        emit_expr(*rule->guard);
        if (!self_parens) out() << ")";
        out() << " {\n";
        indent_depth_++;
        emit_stmts(rule->body, /*tail=*/true);
        if (tail_break_after_guard_match_) {
            indent() << "break;\n";
        }
        indent_depth_--;
        indent() << "}\n";
    } else {
        emit_stmts(rule->body, /*tail=*/true);
        // When the rule lives inside a switch case (c_backend) and the
        // pattern carries field constraints (enum-value matches like
        // `op: Pos`), emit_match has opened a leaf-check `if (combined)`
        // block around this body. The body sets the result via
        // match_target_var_, so without a break the next per-case rule
        // block runs and overwrites it. The break also covers the
        // unconditional-fallthrough rule: it just exits the case one
        // statement earlier than the case-end break, which is harmless.
        if (tail_break_after_guard_match_) {
            indent() << "break;\n";
        }
    }

    while (blocks_open--) {
        indent_depth_--;
        indent() << "}\n";
    }
}

} // namespace ddcgc
