#include "Sema.h"
#include <algorithm>
#include <queue>

using namespace BBQ;

namespace {

// Check if an expression tree contains a reference to loop index 'i'
bool contains_loop_var(Expr* expr) {
    if (auto* ref = dynamic_cast<Ref*>(expr)) {
        if (auto* simple = dynamic_cast<Simple*>(ref->path))
            return simple->name == "i";
        // FieldAcc/IndexAcc: check base recursively
        if (auto* ia = dynamic_cast<IndexAcc*>(ref->path))
            return contains_loop_var(ia->index);
        return false;
    }
    if (auto* bin = dynamic_cast<BinOp*>(expr))
        return contains_loop_var(bin->left) || contains_loop_var(bin->right);
    if (auto* un = dynamic_cast<UnaryOp*>(expr))
        return contains_loop_var(un->operand);
    if (auto* tern = dynamic_cast<Ternary*>(expr))
        return contains_loop_var(tern->cond) ||
               contains_loop_var(tern->then_branch) ||
               contains_loop_var(tern->else_branch);
    if (auto* call = dynamic_cast<Call*>(expr)) {
        for (auto* arg : call->args)
            if (contains_loop_var(arg)) return true;
        return false;
    }
    return false;
}

} // anonymous namespace

namespace bbqgen {

bool Sema::analyze(Grammar* grammar) {
    collect_rules(grammar);
    if (errors_.has_errors()) return false;

    resolve_endian(grammar);

    for (auto* rule : grammar->rules) {
        validate_rule(rule);
    }

    if (!errors_.has_errors()) {
        if (!topological_sort()) return false;
    }

    for (auto* rule : grammar->rules) {
        check_forward_refs(rule);
    }

    if (!errors_.has_errors()) {
        for (auto* rule : grammar->rules) {
            check_types(rule);
        }
    }

    return !errors_.has_errors();
}

// --- Phase 1: collect rule names ---

void Sema::collect_rules(Grammar* grammar) {
    for (auto* rule : grammar->rules) {
        auto [it, inserted] = rule_map_.emplace(rule->name, rule);
        if (!inserted) {
            errors_.error(rule->loc, "duplicate rule '%s'", rule->name.c_str());
        }
        deps_[rule->name]; // ensure entry exists
    }
}

// --- Phase 2: resolve @endian directive ---

void Sema::resolve_endian(Grammar* grammar) {
    for (auto* dir : grammar->directives) {
        default_endian_ = dir->endian;
    }
    // Rewrite Default endianness in all primitives is done during codegen,
    // not here — the AST keeps the original Default tag and emitters
    // resolve it using default_endian().
}

// --- Phase 3: validate references, build dependency graph ---

void Sema::validate_rule(Rule* rule) {
    validate_type(rule->body, rule->name);
}

void Sema::validate_type(TypeExpr* type, const std::string& ctx) {
    if (auto* st = dynamic_cast<Struct*>(type)) {
        for (auto* field : st->fields) {
            validate_type(field->body, ctx);
            if (field->constraint)
                validate_expr(*field->constraint, ctx);
        }
    } else if (auto* u = dynamic_cast<Union*>(type)) {
        for (auto* variant : u->variants) {
            validate_type(variant->body, ctx);
            if (variant->constraint)
                validate_expr(*variant->constraint, ctx);
        }
    } else if (auto* arr = dynamic_cast<Array*>(type)) {
        validate_type(arr->element, ctx);
        if (auto* fc = dynamic_cast<FixedCount*>(arr->spec)) {
            validate_expr(fc->count, ctx);
        } else if (auto* st = dynamic_cast<SepTerm*>(arr->spec)) {
            if (auto* ts = dynamic_cast<TypeSep*>(st->sep))
                validate_type(ts->sep_type, ctx);
            if (auto* ct = dynamic_cast<CountTerm*>(st->term))
                validate_expr(ct->count, ctx);
            else if (auto* ut = dynamic_cast<UntilTerm*>(st->term))
                validate_expr(ut->condition, ctx);
            else if (auto* tt = dynamic_cast<BBQ::TypeTerm*>(st->term))
                validate_type(tt->term_type, ctx);
        }
        if (arr->constraint)
            validate_expr(*arr->constraint, ctx);
        if (arr->element_interval) {
            auto* eiv = *arr->element_interval;
            if (auto* se = dynamic_cast<StartEnd*>(eiv)) {
                validate_expr(se->start, ctx);
                validate_expr(se->end, ctx);
            }
            // element_interval only valid on counted arrays
            bool is_counted = dynamic_cast<FixedCount*>(arr->spec) != nullptr;
            if (!is_counted) {
                if (auto* st2 = dynamic_cast<SepTerm*>(arr->spec)) {
                    is_counted = dynamic_cast<CountTerm*>(st2->term) != nullptr;
                }
            }
            if (!is_counted) {
                errors_.error(eiv->loc, "per-element interval requires a counted array (FixedCount or CountTerm)");
            }
            // Warn if element interval doesn't reference loop index 'i'
            bool uses_i = false;
            if (auto* se = dynamic_cast<StartEnd*>(eiv)) {
                uses_i = contains_loop_var(se->start) || contains_loop_var(se->end);
            }
            if (!uses_i) {
                errors_.warning(eiv->loc,
                    "per-element interval does not reference loop index 'i'; "
                    "all elements will use the same bounds");
            }
        }
    } else if (auto* opt = dynamic_cast<Optional*>(type)) {
        validate_type(opt->element, ctx);
        if (opt->constraint)
            validate_expr(*opt->constraint, ctx);
    } else if (auto* sw = dynamic_cast<Switch*>(type)) {
        validate_expr(sw->discriminator, ctx);
        for (auto* c : sw->cases) {
            validate_type(c->target, ctx);
        }
        if (sw->default_)
            validate_type((*sw->default_)->target, ctx);
        validate_switch(sw, ctx);
    } else if (auto* comp = dynamic_cast<Compute*>(type)) {
        validate_expr(comp->expression, ctx);
    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        if (rule_map_.find(rr->name) == rule_map_.end()) {
            auto suggestion = find_closest_rule(rr->name);
            if (!suggestion.empty())
                errors_.error(rr->loc, "undefined rule '%s'; did you mean '%s'?",
                              rr->name.c_str(), suggestion.c_str());
            else
                errors_.error(rr->loc, "undefined rule '%s'", rr->name.c_str());
        } else {
            deps_[ctx].insert(rr->name);
        }
        if (rr->constraint)
            validate_expr(*rr->constraint, ctx);
    } else if (auto* alts = dynamic_cast<Alternatives*>(type)) {
        for (auto* alt : alts->alts) {
            validate_type(alt, ctx);
        }
    } else if (auto* prim = dynamic_cast<Primitive*>(type)) {
        if (prim->constraint)
            validate_expr(*prim->constraint, ctx);
    } else if (auto* ext = dynamic_cast<Extern*>(type)) {
        if (ext->func_name.empty())
            errors_.error(ext->loc, "extern function name must not be empty");
        else if (!is_valid_cpp_ident(ext->func_name))
            errors_.error(ext->loc, "extern function name '%s' is not a valid identifier",
                          ext->func_name.c_str());
        if (ext->cpp_type.empty())
            errors_.error(ext->loc, "extern type must not be empty");
        if (ext->constraint)
            validate_expr(*ext->constraint, ctx);
    } else if (auto* es = dynamic_cast<EndianSwitch*>(type)) {
        validate_expr(es->endian_expr, ctx);
    } else if (auto* bf = dynamic_cast<Bitfield*>(type)) {
        auto* ik = dynamic_cast<IntegerKind*>(bf->container);
        if (!ik) {
            errors_.error(bf->loc, "bitfield container must be an integer type");
            return;
        }
        int expected = 0;
        switch (ik->width) {
            case Width::W8:  expected = 8; break;
            case Width::W16: expected = 16; break;
            case Width::W32: expected = 32; break;
            case Width::W64: expected = 64; break;
        }
        int total_bits = 0;
        std::unordered_set<std::string> names;
        for (auto* entry : bf->entries) {
            if (entry->width <= 0)
                errors_.error(entry->loc, "bitfield entry '%s' width must be > 0",
                              entry->name.c_str());
            if (!names.insert(entry->name).second)
                errors_.error(entry->loc, "duplicate bitfield entry '%s'",
                              entry->name.c_str());
            total_bits += entry->width;
        }
        if (total_bits != expected)
            errors_.error(bf->loc, "bitfield entries sum to %d bits, container is %d bits",
                          total_bits, expected);
    }
}

void Sema::validate_expr(Expr* expr, const std::string& ctx) {
    if (auto* bin = dynamic_cast<BinOp*>(expr)) {
        validate_expr(bin->left, ctx);
        validate_expr(bin->right, ctx);
    } else if (auto* un = dynamic_cast<UnaryOp*>(expr)) {
        validate_expr(un->operand, ctx);
    } else if (auto* tern = dynamic_cast<Ternary*>(expr)) {
        validate_expr(tern->cond, ctx);
        validate_expr(tern->then_branch, ctx);
        validate_expr(tern->else_branch, ctx);
    } else if (auto* ref = dynamic_cast<Ref*>(expr)) {
        validate_ref_path(ref->path, ctx);
    } else if (auto* call = dynamic_cast<Call*>(expr)) {
        if (call->func == "peek" && !call->args.empty())
            errors_.error(call->loc, "peek() takes no arguments");
        for (auto* arg : call->args) {
            validate_expr(arg, ctx);
        }
    }
    // Literals and EOI: nothing to validate
}

void Sema::validate_ref_path(RefPath* path, const std::string& ctx) {
    if (auto* fa = dynamic_cast<FieldAcc*>(path)) {
        validate_ref_path(fa->base, ctx);
    } else if (auto* ia = dynamic_cast<IndexAcc*>(path)) {
        validate_ref_path(ia->base, ctx);
        validate_expr(ia->index, ctx);
    }
    // Simple: validated during forward ref check
}

// --- Phase 4: topological sort + cycle detection ---

bool Sema::topological_sort() {
    // Kahn's algorithm
    // in_degree[A] = number of rules A depends on (must come before A)
    std::unordered_map<std::string, int> in_degree;
    for (auto& [name, _] : rule_map_) {
        in_degree[name] = 0;
    }
    for (auto& [name, dep_set] : deps_) {
        in_degree[name] = static_cast<int>(dep_set.size());
    }

    std::queue<std::string> queue;
    for (auto& [name, deg] : in_degree) {
        if (deg == 0) queue.push(name);
    }

    sorted_.clear();
    while (!queue.empty()) {
        auto name = queue.front();
        queue.pop();
        sorted_.push_back(rule_map_[name]);

        // For each rule that depends on `name`, decrement its in-degree
        for (auto& [other, dep_set] : deps_) {
            if (dep_set.count(name)) {
                in_degree[other]--;
                if (in_degree[other] == 0) {
                    queue.push(other);
                }
            }
        }
    }

    if (sorted_.size() != rule_map_.size()) {
        // Find rules involved in cycles
        for (auto& [name, deg] : in_degree) {
            if (deg > 0) {
                errors_.error(rule_map_[name]->loc,
                    "circular dependency involving rule '%s'", name.c_str());
            }
        }
        return false;
    }
    return true;
}

// --- Phase 5: forward reference checks ---

void Sema::check_forward_refs(Rule* rule) {
    auto* st = dynamic_cast<Struct*>(rule->body);
    if (!st) return;

    check_forward_refs_struct(st, {}, rule->name);
}

void Sema::check_forward_refs_struct(Struct* st,
                                      std::unordered_set<std::string> inherited,
                                      const std::string& ctx,
                                      std::unordered_set<std::string> all_ancestor_fields) {
    std::unordered_set<std::string> defined = std::move(inherited);

    // Build set of ALL local field names — used to distinguish forward refs
    // (later field in same struct → error) from cross-rule refs (name not in
    // this struct at all → allowed, resolved at runtime).
    std::unordered_set<std::string> all_local_fields;
    for (auto* f : st->fields)
        if (!dynamic_cast<EndianSwitch*>(f->body))
            all_local_fields.insert(f->name);

    // Combined set: local + ancestor. If a name is in here but not in
    // `defined`, it's a forward reference (error). If not in here, it's
    // a cross-rule reference (allowed).
    std::unordered_set<std::string> all_known_fields = all_local_fields;
    all_known_fields.insert(all_ancestor_fields.begin(), all_ancestor_fields.end());

    for (auto* field : st->fields) {
        // EndianSwitch pseudo-field: check expression refs, don't add to defined
        if (auto* es = dynamic_cast<EndianSwitch*>(field->body)) {
            std::unordered_set<std::string> refs;
            collect_expr_refs(es->endian_expr, refs);
            for (auto& ref : refs) {
                if (defined.find(ref) == defined.end() && all_known_fields.count(ref)) {
                    errors_.error(field->loc,
                        "forward reference to '%s' in @endian directive",
                        ref.c_str());
                }
            }
            continue;
        }

        // Helper to check a constraint expression for forward refs
        auto check_constraint = [&](Expr* constr) {
            std::unordered_set<std::string> refs;
            collect_expr_refs(constr, refs);
            for (auto& ref : refs) {
                if (defined.find(ref) == defined.end() && ref != field->name
                    && all_known_fields.count(ref)) {
                    errors_.error(field->loc,
                        "forward reference to '%s' in constraint of field '%s'",
                        ref.c_str(), field->name.c_str());
                }
            }
        };

        // Check constraint on the Field itself
        if (field->constraint)
            check_constraint(*field->constraint);

        // Check constraint on the type node (Primitive, RuleRef, or Extern)
        if (auto* prim = dynamic_cast<Primitive*>(field->body)) {
            if (prim->constraint)
                check_constraint(*prim->constraint);
        } else if (auto* rr = dynamic_cast<RuleRef*>(field->body)) {
            if (rr->constraint)
                check_constraint(*rr->constraint);
        } else if (auto* ext = dynamic_cast<Extern*>(field->body)) {
            if (ext->constraint)
                check_constraint(*ext->constraint);
        }

        // Check intervals on the Field
        if (field->interval.has_value()) {
            if (auto* se = dynamic_cast<StartEnd*>(*field->interval)) {
                check_constraint(se->start);
                check_constraint(se->end);
            } else if (auto* len = dynamic_cast<Length*>(*field->interval)) {
                check_constraint(len->length);
            }
        }
        // Check intervals on body type (RuleRef, Primitive)
        Interval* body_interval = nullptr;
        if (auto* rr = dynamic_cast<RuleRef*>(field->body)) {
            if (rr->interval.has_value()) body_interval = *rr->interval;
        } else if (auto* prim = dynamic_cast<Primitive*>(field->body)) {
            if (prim->interval.has_value()) body_interval = *prim->interval;
        } else if (auto* ext = dynamic_cast<Extern*>(field->body)) {
            if (ext->interval.has_value()) body_interval = *ext->interval;
        }
        if (body_interval) {
            if (auto* se = dynamic_cast<StartEnd*>(body_interval)) {
                check_constraint(se->start);
                check_constraint(se->end);
            } else if (auto* len = dynamic_cast<Length*>(body_interval)) {
                check_constraint(len->length);
            }
        }

        // Compute fields: expression must reference only earlier fields (or cross-rule)
        if (auto* comp = dynamic_cast<Compute*>(field->body)) {
            std::unordered_set<std::string> refs;
            collect_expr_refs(comp->expression, refs);
            for (auto& ref : refs) {
                if (defined.find(ref) == defined.end() && all_known_fields.count(ref)) {
                    errors_.error(field->loc,
                        "forward reference to '%s' in compute field '%s'",
                        ref.c_str(), field->name.c_str());
                }
            }
        }

        // Switch discriminator: must reference only earlier fields (item 11)
        if (auto* sw = dynamic_cast<Switch*>(field->body)) {
            std::unordered_set<std::string> refs;
            collect_expr_refs(sw->discriminator, refs);
            for (auto& ref : refs) {
                if (defined.find(ref) == defined.end() && ref != field->name
                    && all_known_fields.count(ref)) {
                    errors_.error(field->loc,
                        "forward reference to '%s' in switch discriminator of field '%s'",
                        ref.c_str(), field->name.c_str());
                }
            }
        }

        // Array spec: count/until must reference only earlier fields
        if (auto* arr = dynamic_cast<Array*>(field->body)) {
            auto check_arr = [&](Expr* e) {
                std::unordered_set<std::string> refs;
                collect_expr_refs(e, refs);
                for (auto& ref : refs) {
                    if (defined.find(ref) == defined.end() && ref != field->name
                        && all_local_fields.count(ref)) {
                        errors_.error(field->loc,
                            "forward reference to '%s' in array spec of field '%s'",
                            ref.c_str(), field->name.c_str());
                    }
                }
            };
            if (auto* fc = dynamic_cast<FixedCount*>(arr->spec))
                check_arr(fc->count);
            else if (auto* st2 = dynamic_cast<SepTerm*>(arr->spec)) {
                if (auto* ct = dynamic_cast<CountTerm*>(st2->term))
                    check_arr(ct->count);
                else if (auto* ut = dynamic_cast<UntilTerm*>(st2->term))
                    check_arr(ut->condition);
            }
        }

        // Check for loop index 'i' used outside element interval context.
        // Compute expressions are exempt — 'i' may come from a parent's
        // array loop via ctx.loop_index() at runtime.
        {
            bool has_element_interval = false;
            if (auto* arr = dynamic_cast<Array*>(field->body))
                has_element_interval = arr->element_interval.has_value();

            if (!has_element_interval) {
                // Collect all expressions to check for 'i'
                std::vector<Expr*> exprs_to_check;
                if (field->constraint)
                    exprs_to_check.push_back(*field->constraint);
                if (auto* prim = dynamic_cast<Primitive*>(field->body)) {
                    if (prim->constraint) exprs_to_check.push_back(*prim->constraint);
                } else if (auto* rr = dynamic_cast<RuleRef*>(field->body)) {
                    if (rr->constraint) exprs_to_check.push_back(*rr->constraint);
                } else if (auto* ext = dynamic_cast<Extern*>(field->body)) {
                    if (ext->constraint) exprs_to_check.push_back(*ext->constraint);
                }
                // Note: Compute expressions are NOT checked — 'i' is valid
                // in cross-rule compute fields (resolved via ctx.loop_index())
                for (auto* e : exprs_to_check) {
                    if (contains_loop_var(e)) {
                        errors_.error(field->loc,
                            "loop index 'i' referenced outside per-element interval context in field '%s'",
                            field->name.c_str());
                        break;
                    }
                }
            }
        }

        // Nested inline struct: recurse with current scope
        if (auto* nested_st = dynamic_cast<Struct*>(field->body)) {
            check_forward_refs_struct(nested_st, defined,
                                       ctx + "." + field->name,
                                       all_known_fields);
        }

        defined.insert(field->name);
    }
}

void Sema::collect_expr_refs(Expr* expr, std::unordered_set<std::string>& refs) {
    if (auto* bin = dynamic_cast<BinOp*>(expr)) {
        collect_expr_refs(bin->left, refs);
        collect_expr_refs(bin->right, refs);
    } else if (auto* un = dynamic_cast<UnaryOp*>(expr)) {
        collect_expr_refs(un->operand, refs);
    } else if (auto* tern = dynamic_cast<Ternary*>(expr)) {
        collect_expr_refs(tern->cond, refs);
        collect_expr_refs(tern->then_branch, refs);
        collect_expr_refs(tern->else_branch, refs);
    } else if (auto* ref = dynamic_cast<Ref*>(expr)) {
        collect_refpath_refs(ref->path, refs);
    } else if (auto* call = dynamic_cast<Call*>(expr)) {
        for (auto* arg : call->args) {
            collect_expr_refs(arg, refs);
        }
    }
}

void Sema::collect_refpath_refs(RefPath* path, std::unordered_set<std::string>& refs) {
    if (auto* simple = dynamic_cast<Simple*>(path)) {
        // Built-in context properties — not field references
        if (simple->name == "remaining" || simple->name == "pos" || simple->name == "at_end" || simple->name == "i")
            return;
        refs.insert(simple->name);
    } else if (auto* fa = dynamic_cast<FieldAcc*>(path)) {
        collect_refpath_refs(fa->base, refs);
    } else if (auto* ia = dynamic_cast<IndexAcc*>(path)) {
        collect_refpath_refs(ia->base, refs);
        collect_expr_refs(ia->index, refs);
    }
}

// --- Phase 6: switch validation (renumbered from original Phase 6) ---

void Sema::validate_switch(Switch* sw, const std::string& ctx) {
    std::unordered_set<int> int_values;
    std::unordered_set<std::string> str_values;
    bool has_str = false, has_int = false;

    for (auto* c : sw->cases) {
        if (auto* iv = dynamic_cast<IntValue*>(c->value)) {
            has_int = true;
            if (!int_values.insert(iv->value).second) {
                errors_.error(c->loc,
                    "duplicate switch case value %d in %s",
                    iv->value, ctx.c_str());
            }
        } else if (auto* sv = dynamic_cast<StrValue*>(c->value)) {
            has_str = true;
            if (!str_values.insert(sv->value).second) {
                errors_.error(c->loc,
                    "duplicate switch case value '%s' in %s",
                    sv->value.c_str(), ctx.c_str());
            }
        } else {
            // IdentValue, RefValue — treated as non-string for this check
            has_int = true;
        }
    }

    if (has_str && has_int) {
        errors_.error(sw->loc,
            "switch in %s mixes string and integer/identifier case values",
            ctx.c_str());
    }

    if (!sw->default_.has_value()) {
        errors_.warning(sw->loc, "switch in %s has no default case", ctx.c_str());
    }
}

// --- Phase 7: expression type checking ---

// Type compatibility helpers

ExprType Sema::join(ExprType a, ExprType b) {
    if (a == ExprType::Unknown || b == ExprType::Unknown) return ExprType::Unknown;
    if (a == b) return a;
    // Integer + Float → Numeric
    if ((a == ExprType::Integer || a == ExprType::Float || a == ExprType::Numeric) &&
        (b == ExprType::Integer || b == ExprType::Float || b == ExprType::Numeric))
        return ExprType::Numeric;
    // Endian + non-Endian → Unknown (will trigger type error in ternary)
    return ExprType::Unknown;
}

bool Sema::is_numeric(ExprType t) {
    return t == ExprType::Integer || t == ExprType::Float ||
           t == ExprType::Numeric || t == ExprType::Unknown;
}

bool Sema::is_integer(ExprType t) {
    return t == ExprType::Integer || t == ExprType::Unknown;
}

bool Sema::is_boolean_compatible(ExprType t) {
    return t == ExprType::Bool || t == ExprType::Integer || t == ExprType::Unknown;
}

bool Sema::types_compatible(ExprType expr, ExprType decl) {
    if (expr == ExprType::Unknown || decl == ExprType::Unknown) return true;
    if (expr == decl) return true;
    // Numeric widening: Integer ↔ Float ↔ Numeric are all compatible
    if (is_numeric(expr) && is_numeric(decl)) return true;
    return false;
}

bool Sema::is_valid_cpp_ident(const std::string& s) {
    if (s.empty()) return false;
    // First char: alpha or underscore
    if (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_')
        return false;
    for (size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            continue;
        // Allow :: for namespace-qualified names
        if (c == ':' && i + 1 < s.size() && s[i + 1] == ':') {
            ++i; // skip second ':'
            continue;
        }
        return false;
    }
    return true;
}

int Sema::edit_distance(const std::string& a, const std::string& b) {
    size_t m = a.size(), n = b.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1));
    for (size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1,
                                 dp[i][j - 1] + 1,
                                 dp[i - 1][j - 1] + cost});
        }
    }
    return dp[m][n];
}

std::string Sema::find_closest_rule(const std::string& name) const {
    std::string best;
    int best_dist = 4; // threshold: only suggest if distance <= 3
    for (auto& [rule_name, _] : rule_map_) {
        int d = edit_distance(name, rule_name);
        if (d < best_dist) {
            best_dist = d;
            best = rule_name;
        }
    }
    return best;
}

std::string Sema::find_closest_field(const std::string& name,
                                     const std::vector<Field*>& fields) {
    std::string best;
    int best_dist = 4;
    for (auto* f : fields) {
        int d = edit_distance(name, f->name);
        if (d < best_dist) {
            best_dist = d;
            best = f->name;
        }
    }
    return best;
}

bool Sema::body_has_constraint(TypeExpr* body) {
    if (auto* prim = dynamic_cast<Primitive*>(body))
        return prim->constraint.has_value();
    if (auto* rr = dynamic_cast<RuleRef*>(body))
        return rr->constraint.has_value();
    if (auto* ext = dynamic_cast<Extern*>(body))
        return ext->constraint.has_value();
    return false;
}

// Type inference from PrimitiveKind

ExprType Sema::type_of_primitive_kind(PrimitiveKind* kind) {
    if (dynamic_cast<IntegerKind*>(kind)) return ExprType::Integer;
    if (dynamic_cast<FloatKind*>(kind))   return ExprType::Float;
    if (dynamic_cast<BoolKind*>(kind))    return ExprType::Bool;
    if (dynamic_cast<StringKind*>(kind))  return ExprType::String;
    if (dynamic_cast<BytesKind*>(kind))   return ExprType::Bytes;
    return ExprType::Unknown;
}

// Type inference from TypeExpr (a field's body type)

ExprType Sema::type_of_type_expr(TypeExpr* type) {
    if (auto* prim = dynamic_cast<Primitive*>(type))
        return type_of_primitive_kind(prim->kind);
    if (auto* comp = dynamic_cast<Compute*>(type))
        return comp->result_type ? type_of_primitive_kind(comp->result_type) : ExprType::Unknown;
    if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        auto* rule = lookup_rule(rr->name);
        if (rule) return type_of_type_expr(rule->body);
        return ExprType::Unknown;
    }
    // Array, Struct, Union, Switch, Alternatives, Bitfield, Optional, Extern → Unknown
    return ExprType::Unknown;
}

// Structural resolution: follow RuleRef chains to find a Struct*

Struct* Sema::resolve_to_struct(TypeExpr* type) {
    if (auto* st = dynamic_cast<Struct*>(type)) return st;
    if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        auto* rule = lookup_rule(rr->name);
        if (rule) return resolve_to_struct(rule->body);
    }
    return nullptr;
}

// Structural resolution: follow to get array element type

TypeExpr* Sema::resolve_to_array_element(TypeExpr* type) {
    if (auto* arr = dynamic_cast<Array*>(type)) return arr->element;
    if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        auto* rule = lookup_rule(rr->name);
        if (rule) return resolve_to_array_element(rule->body);
    }
    return nullptr;
}

// Core expression type inference

ExprType Sema::type_of_expr(Expr* expr, const TypeCheckScope& scope) {
    if (dynamic_cast<IntLit*>(expr))    return ExprType::Integer;
    if (dynamic_cast<FloatLit*>(expr)) return ExprType::Float;
    if (dynamic_cast<StrLit*>(expr))   return ExprType::String;
    if (dynamic_cast<BoolLit*>(expr))  return ExprType::Bool;
    if (dynamic_cast<EndianLit*>(expr)) return ExprType::Endian;
    if (dynamic_cast<EOI*>(expr))      return ExprType::Integer;

    if (auto* ref = dynamic_cast<Ref*>(expr))
        return type_of_ref_path(ref->path, scope);

    if (auto* bin = dynamic_cast<BinOp*>(expr)) {
        auto lt = type_of_expr(bin->left, scope);
        auto rt = type_of_expr(bin->right, scope);

        switch (bin->op) {
            // Arithmetic: both numeric
            case Binop::Add: case Binop::Sub:
            case Binop::Mul: case Binop::Div: case Binop::Mod:
                if (!is_numeric(lt))
                    errors_.error(bin->left->loc, "arithmetic operand must be numeric");
                if (!is_numeric(rt))
                    errors_.error(bin->right->loc, "arithmetic operand must be numeric");
                return join(lt, rt);

            // Bitwise: both integer
            case Binop::BitAnd: case Binop::BitOr:
            case Binop::BitXor: case Binop::Shl: case Binop::Shr:
                if (!is_integer(lt))
                    errors_.error(bin->left->loc, "bitwise operand must be integer");
                if (!is_integer(rt))
                    errors_.error(bin->right->loc, "bitwise operand must be integer");
                return ExprType::Integer;

            // Comparison: both compatible, result Bool
            case Binop::Eq: case Binop::Ne:
            case Binop::Lt: case Binop::Le:
            case Binop::Gt: case Binop::Ge:
                // Permissive: any comparable pair, including Unknown
                return ExprType::Bool;

            // Logical: both boolean-compatible, result Bool
            case Binop::And: case Binop::Or:
                if (!is_boolean_compatible(lt))
                    errors_.error(bin->left->loc, "logical operand must be boolean");
                if (!is_boolean_compatible(rt))
                    errors_.error(bin->right->loc, "logical operand must be boolean");
                return ExprType::Bool;
        }
        return ExprType::Unknown;
    }

    if (auto* un = dynamic_cast<UnaryOp*>(expr)) {
        auto ot = type_of_expr(un->operand, scope);
        switch (un->op) {
            case Unaryop::Neg: case Unaryop::Abs:
                if (!is_numeric(ot))
                    errors_.error(un->operand->loc, "operand must be numeric");
                return ot == ExprType::Unknown ? ExprType::Unknown : ot;
            case Unaryop::BitNot:
                if (!is_integer(ot))
                    errors_.error(un->operand->loc, "bitwise not operand must be integer");
                return ExprType::Integer;
            case Unaryop::Not:
                if (!is_boolean_compatible(ot))
                    errors_.error(un->operand->loc, "logical not operand must be boolean");
                return ExprType::Bool;
        }
        return ExprType::Unknown;
    }

    if (auto* tern = dynamic_cast<Ternary*>(expr)) {
        auto ct = type_of_expr(tern->cond, scope);
        if (!is_boolean_compatible(ct))
            errors_.error(tern->cond->loc, "ternary condition must be boolean");
        auto tt = type_of_expr(tern->then_branch, scope);
        auto et = type_of_expr(tern->else_branch, scope);
        return join(tt, et);
    }

    if (auto* call = dynamic_cast<Call*>(expr)) {
        // Type-check arguments
        for (auto* arg : call->args)
            type_of_expr(arg, scope);
        // Known built-in functions
        if (call->func == "crc32" || call->func == "checksum_xor" || call->func == "peek")
            return ExprType::Integer;
        return ExprType::Unknown;
    }

    return ExprType::Unknown;
}

// Ref path type resolution

ExprType Sema::type_of_ref_path(RefPath* path, const TypeCheckScope& scope) {
    if (auto* simple = dynamic_cast<Simple*>(path)) {
        if (simple->name == "remaining" || simple->name == "pos" || simple->name == "i")
            return ExprType::Integer;
        if (simple->name == "at_end")
            return ExprType::Bool;
        auto it = scope.types.find(simple->name);
        if (it != scope.types.end()) return it->second;
        return ExprType::Unknown;
    }

    if (auto* fa = dynamic_cast<FieldAcc*>(path)) {
        // Resolve base to its TypeExpr body, then find the field
        // First, get the root name
        auto* base_simple = dynamic_cast<Simple*>(fa->base);
        TypeExpr* base_body = nullptr;
        if (base_simple) {
            auto it = scope.bodies.find(base_simple->name);
            if (it != scope.bodies.end()) base_body = it->second;
        }

        if (base_body) {
            // Walk any intermediate FieldAcc chain (for a.b.c, recurse)
            // For simple base, resolve directly
            auto* st = resolve_to_struct(base_body);
            if (st) {
                for (auto* f : st->fields) {
                    if (f->name == fa->field)
                        return type_of_type_expr(f->body);
                }
                auto suggestion = find_closest_field(fa->field, st->fields);
                if (!suggestion.empty())
                    errors_.error(fa->loc, "no field '%s' in struct; did you mean '%s'?",
                                  fa->field.c_str(), suggestion.c_str());
                else
                    errors_.error(fa->loc, "no field '%s' in struct", fa->field.c_str());
                return ExprType::Unknown;
            }
        }

        // For deeper paths (a.b.c), resolve step by step
        if (!base_simple) {
            auto base_type = type_of_ref_path(fa->base, scope);
            // Can't resolve further without structural info — permissive
            (void)base_type;
        }
        return ExprType::Unknown;
    }

    if (auto* ia = dynamic_cast<IndexAcc*>(path)) {
        auto idx_type = type_of_expr(ia->index, scope);
        if (!is_integer(idx_type))
            errors_.error(ia->index->loc, "array index must be integer");

        // Resolve base to array element type
        auto* base_simple = dynamic_cast<Simple*>(ia->base);
        if (base_simple) {
            auto it = scope.bodies.find(base_simple->name);
            if (it != scope.bodies.end()) {
                auto* elem = resolve_to_array_element(it->second);
                if (elem) return type_of_type_expr(elem);
            }
        }
        return ExprType::Unknown;
    }

    return ExprType::Unknown;
}

// Check interval types and static bounds

void Sema::check_interval_types(Interval* iv, const TypeCheckScope& scope,
                                const std::string& ctx) {
    // Helper: extract a static integer value from an expression (IntLit or -IntLit)
    auto static_int = [](Expr* e) -> std::optional<int64_t> {
        if (auto* lit = dynamic_cast<IntLit*>(e))
            return lit->value;
        if (auto* un = dynamic_cast<UnaryOp*>(e)) {
            if (un->op == Unaryop::Neg) {
                if (auto* lit = dynamic_cast<IntLit*>(un->operand))
                    return -lit->value;
            }
        }
        return std::nullopt;
    };

    if (auto* se = dynamic_cast<StartEnd*>(iv)) {
        auto st = type_of_expr(se->start, scope);
        auto et = type_of_expr(se->end, scope);
        if (!is_integer(st))
            errors_.error(se->start->loc, "interval start must be integer");
        if (!is_integer(et))
            errors_.error(se->end->loc, "interval end must be integer");

        // Static literal bounds checks
        auto sval = static_int(se->start);
        auto eval = static_int(se->end);
        if (sval && *sval < 0)
            errors_.error(se->start->loc, "interval start must be non-negative");
        if (eval && *eval < 0)
            errors_.error(se->end->loc, "interval end must be non-negative");
        if (sval && eval && *sval >= *eval)
            errors_.error(se->loc, "interval start must be less than end");
    } else if (auto* len = dynamic_cast<Length*>(iv)) {
        auto lt = type_of_expr(len->length, scope);
        if (!is_integer(lt))
            errors_.error(len->length->loc, "interval length must be integer");
        auto lval = static_int(len->length);
        if (lval && *lval < 0)
            errors_.error(len->length->loc, "interval length must be non-negative");
    }
}

// Check types for a single field

void Sema::check_field_types(Field* field, const TypeCheckScope& scope,
                             const std::string& ctx) {
    // Check Field-level constraint
    if (field->constraint) {
        auto ct = type_of_expr(*field->constraint, scope);
        if (!is_boolean_compatible(ct))
            errors_.error((*field->constraint)->loc,
                "constraint must be boolean (got non-boolean expression)");
    }

    // Check body-level constraint (Primitive, RuleRef, Extern)
    Expr* body_constraint = nullptr;
    if (auto* prim = dynamic_cast<Primitive*>(field->body)) {
        if (prim->constraint) body_constraint = *prim->constraint;
    } else if (auto* rr = dynamic_cast<RuleRef*>(field->body)) {
        if (rr->constraint) body_constraint = *rr->constraint;
    } else if (auto* ext = dynamic_cast<Extern*>(field->body)) {
        if (ext->constraint) body_constraint = *ext->constraint;
    }
    if (body_constraint) {
        auto ct = type_of_expr(body_constraint, scope);
        if (!is_boolean_compatible(ct))
            errors_.error(body_constraint->loc,
                "constraint must be boolean (got non-boolean expression)");
    }

    // Item 16: Conflicting constraint detection
    if (field->constraint && body_has_constraint(field->body)) {
        errors_.warning(field->loc,
            "field '%s' has constraints on both field and type; "
            "both will be checked but this may be unintentional",
            field->name.c_str());
    }

    // Self-referential constraint tautology detection
    auto check_tautology = [&](Expr* constr) {
        auto* bin = dynamic_cast<BinOp*>(constr);
        if (!bin) return;
        if (bin->op != Binop::Eq && bin->op != Binop::Ne) return;
        auto* lref = dynamic_cast<Ref*>(bin->left);
        auto* rref = dynamic_cast<Ref*>(bin->right);
        if (!lref || !rref) return;
        auto* lsimple = dynamic_cast<Simple*>(lref->path);
        auto* rsimple = dynamic_cast<Simple*>(rref->path);
        if (!lsimple || !rsimple) return;
        if (lsimple->name == rsimple->name) {
            const char* kind = (bin->op == Binop::Eq) ? "always true" : "always false";
            errors_.warning(constr->loc,
                "constraint on field '%s' compares field to itself (%s)",
                lsimple->name.c_str(), kind);
        }
    };
    if (field->constraint)
        check_tautology(*field->constraint);
    if (body_constraint)
        check_tautology(body_constraint);

    // Item 14: Compute result type matching
    if (auto* comp = dynamic_cast<Compute*>(field->body)) {
        if (!comp->result_type) {
            errors_.error(comp->loc, "compute field missing result type");
        } else {
            auto expr_t = type_of_expr(comp->expression, scope);
            auto decl_t = type_of_primitive_kind(comp->result_type);
            if (!types_compatible(expr_t, decl_t))
                errors_.error(comp->loc,
                    "compute expression type incompatible with declared result type");
        }
    }

    // Check Field-level interval
    if (field->interval.has_value())
        check_interval_types(*field->interval, scope, ctx);

    // Check body-level interval
    Interval* body_interval = nullptr;
    if (auto* rr = dynamic_cast<RuleRef*>(field->body)) {
        if (rr->interval.has_value()) body_interval = *rr->interval;
    } else if (auto* prim = dynamic_cast<Primitive*>(field->body)) {
        if (prim->interval.has_value()) body_interval = *prim->interval;
    } else if (auto* ext = dynamic_cast<Extern*>(field->body)) {
        if (ext->interval.has_value()) body_interval = *ext->interval;
    }
    if (body_interval)
        check_interval_types(body_interval, scope, ctx);

    // Check array spec types (item 12)
    if (auto* arr = dynamic_cast<Array*>(field->body)) {
        if (auto* fc = dynamic_cast<FixedCount*>(arr->spec)) {
            auto ct = type_of_expr(fc->count, scope);
            if (!is_integer(ct))
                errors_.error(fc->count->loc, "array count must be integer");
        } else if (auto* st = dynamic_cast<SepTerm*>(arr->spec)) {
            if (auto* ct = dynamic_cast<CountTerm*>(st->term)) {
                auto t = type_of_expr(ct->count, scope);
                if (!is_integer(t))
                    errors_.error(ct->count->loc, "array count must be integer");
            } else if (auto* ut = dynamic_cast<UntilTerm*>(st->term)) {
                auto t = type_of_expr(ut->condition, scope);
                if (!is_boolean_compatible(t))
                    errors_.error(ut->condition->loc,
                        "until condition must be boolean");
            }
        }
        // Check array constraint
        if (arr->constraint) {
            auto ct = type_of_expr(*arr->constraint, scope);
            if (!is_boolean_compatible(ct))
                errors_.error((*arr->constraint)->loc,
                    "constraint must be boolean (got non-boolean expression)");
        }
        // Check array interval
        if (arr->interval.has_value())
            check_interval_types(*arr->interval, scope, ctx);
        // Check element interval
        if (arr->element_interval.has_value())
            check_interval_types(*arr->element_interval, scope, ctx);
    }

    // Check switch discriminator type (inline switch in a field)
    if (auto* sw = dynamic_cast<Switch*>(field->body)) {
        // Just type-check the discriminator expression
        type_of_expr(sw->discriminator, scope);
    }
}

// Main type checking entry point per rule

void Sema::check_types(Rule* rule) {
    auto* st = dynamic_cast<Struct*>(rule->body);
    if (!st) {
        // Non-struct rules: check with empty scope
        TypeCheckScope empty_scope;

        // Top-level switch
        if (auto* sw = dynamic_cast<Switch*>(rule->body))
            type_of_expr(sw->discriminator, empty_scope);

        // Top-level array
        if (auto* arr = dynamic_cast<Array*>(rule->body)) {
            if (auto* fc = dynamic_cast<FixedCount*>(arr->spec)) {
                auto ct = type_of_expr(fc->count, empty_scope);
                if (!is_integer(ct))
                    errors_.error(fc->count->loc, "array count must be integer");
            } else if (auto* st2 = dynamic_cast<SepTerm*>(arr->spec)) {
                if (auto* ct = dynamic_cast<CountTerm*>(st2->term)) {
                    auto t = type_of_expr(ct->count, empty_scope);
                    if (!is_integer(t))
                        errors_.error(ct->count->loc, "array count must be integer");
                } else if (auto* ut = dynamic_cast<UntilTerm*>(st2->term)) {
                    auto t = type_of_expr(ut->condition, empty_scope);
                    if (!is_boolean_compatible(t))
                        errors_.error(ut->condition->loc,
                            "until condition must be boolean");
                }
            }
            if (arr->constraint) {
                auto ct = type_of_expr(*arr->constraint, empty_scope);
                if (!is_boolean_compatible(ct))
                    errors_.error((*arr->constraint)->loc,
                        "constraint must be boolean (got non-boolean expression)");
            }
            if (arr->interval.has_value())
                check_interval_types(*arr->interval, empty_scope, rule->name);
            if (arr->element_interval.has_value())
                check_interval_types(*arr->element_interval, empty_scope, rule->name);
        }

        return;
    }

    // Struct rule: delegate to recursive helper with empty inherited scope
    check_types_struct(st, {}, rule->name);
}

void Sema::check_types_struct(Struct* st, TypeCheckScope inherited,
                               const std::string& ctx) {
    TypeCheckScope scope = std::move(inherited);
    for (auto* field : st->fields) {
        // EndianSwitch: type-check the expression, require Endian type
        if (auto* es = dynamic_cast<EndianSwitch*>(field->body)) {
            auto et = type_of_expr(es->endian_expr, scope);
            if (et != ExprType::Endian && et != ExprType::Unknown)
                errors_.error(es->loc,
                    "@endian expression must be endian-typed (use 'little' or 'big')");
            continue;
        }

        // Add field to scope BEFORE checking (self-reference is valid)
        scope.types[field->name] = type_of_type_expr(field->body);
        scope.bodies[field->name] = field->body;

        check_field_types(field, scope, ctx);

        // Recurse into nested inline struct
        if (auto* nested_st = dynamic_cast<Struct*>(field->body)) {
            check_types_struct(nested_st, scope, ctx + "." + field->name);
        }
    }
}

} // namespace bbqgen
