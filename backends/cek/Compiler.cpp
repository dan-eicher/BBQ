#include "Compiler.h"
#include "BBQ_AST.h"
#include "Sema.h"

#include <algorithm>
#include <cstring>

namespace bbq::cek {

// --- Helpers ---

const char* CEKCompiler::intern(const std::string& s) {
    return strings_->intern(s);
}

KontNode* CEKCompiler::wrap_constraint(BBQ::Expr* constraint, KontNode* next) {
    auto* node = arena_->alloc<EvalConstraintNode>();
    node->constraint = compile_expr(constraint);
    node->next = next;
    node->on_false = nullptr;  // Hard fail on constraint violation
    return node;
}

// --- PrimitiveInfo conversion ---

static PrimWidth map_width(BBQ::Width w) {
    switch (w) {
        case BBQ::Width::W8:  return PrimWidth::W8;
        case BBQ::Width::W16: return PrimWidth::W16;
        case BBQ::Width::W32: return PrimWidth::W32;
        case BBQ::Width::W64: return PrimWidth::W64;
    }
    return PrimWidth::W8;
}

static PrimSign map_sign(BBQ::Signedness s) {
    return (s == BBQ::Signedness::Signed) ? PrimSign::Signed : PrimSign::Unsigned;
}

static PrimEndian map_endian(BBQ::Endianness e) {
    switch (e) {
        case BBQ::Endianness::Little: return PrimEndian::Little;
        case BBQ::Endianness::Big:    return PrimEndian::Big;
        case BBQ::Endianness::Default: return PrimEndian::Native;
    }
    return PrimEndian::Native;
}

PrimitiveInfo CEKCompiler::to_prim_info(BBQ::PrimitiveKind* kind) {
    if (auto* ik = dynamic_cast<BBQ::IntegerKind*>(kind)) {
        return {PrimKind::Integer, map_width(ik->width), map_sign(ik->sign),
                map_endian(ik->endian)};
    }
    if (auto* fk = dynamic_cast<BBQ::FloatKind*>(kind)) {
        return {PrimKind::Float, map_width(fk->width), PrimSign::Unsigned,
                map_endian(fk->endian)};
    }
    // BoolKind
    return {PrimKind::Bool, PrimWidth::W8, PrimSign::Unsigned, PrimEndian::Native};
}

// --- Binop/Unaryop mapping ---

static ExprOp map_binop(BBQ::Binop op) {
    switch (op) {
        case BBQ::Binop::Add:    return ExprOp::Add;
        case BBQ::Binop::Sub:    return ExprOp::Sub;
        case BBQ::Binop::Mul:    return ExprOp::Mul;
        case BBQ::Binop::Div:    return ExprOp::Div;
        case BBQ::Binop::Mod:    return ExprOp::Mod;
        case BBQ::Binop::BitAnd: return ExprOp::BitAnd;
        case BBQ::Binop::BitOr:  return ExprOp::BitOr;
        case BBQ::Binop::BitXor: return ExprOp::BitXor;
        case BBQ::Binop::Shl:    return ExprOp::Shl;
        case BBQ::Binop::Shr:    return ExprOp::Shr;
        case BBQ::Binop::And:    return ExprOp::And;
        case BBQ::Binop::Or:     return ExprOp::Or;
        case BBQ::Binop::Eq:     return ExprOp::Eq;
        case BBQ::Binop::Ne:     return ExprOp::Ne;
        case BBQ::Binop::Lt:     return ExprOp::Lt;
        case BBQ::Binop::Le:     return ExprOp::Le;
        case BBQ::Binop::Gt:     return ExprOp::Gt;
        case BBQ::Binop::Ge:     return ExprOp::Ge;
    }
    return ExprOp::Add;
}

static ExprOp map_unop(BBQ::Unaryop op) {
    switch (op) {
        case BBQ::Unaryop::Neg:    return ExprOp::Neg;
        case BBQ::Unaryop::BitNot: return ExprOp::BitNot;
        case BBQ::Unaryop::Not:    return ExprOp::Not;
        case BBQ::Unaryop::Abs:    return ExprOp::Call; // Handled specially
    }
    return ExprOp::Neg;
}

// --- Expression compilation ---

CompiledExpr* CEKCompiler::compile_expr(BBQ::Expr* expr) {
    if (auto* il = dynamic_cast<BBQ::IntLit*>(expr)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::IntLit;
        e->int_val = il->value;
        return e;
    }
    if (auto* fl = dynamic_cast<BBQ::FloatLit*>(expr)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::FloatLit;
        e->float_val = static_cast<double>(fl->value);
        return e;
    }
    if (auto* sl = dynamic_cast<BBQ::StrLit*>(expr)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::StrLit;
        e->str_val = intern(sl->value);
        return e;
    }
    if (auto* bl = dynamic_cast<BBQ::BoolLit*>(expr)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::BoolLit;
        e->bool_val = bl->value;
        return e;
    }
    if (auto* el = dynamic_cast<BBQ::EndianLit*>(expr)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::IntLit;
        e->int_val = (el->value == BBQ::Endianness::Little) ? 1 : 0;
        return e;
    }
    if (dynamic_cast<BBQ::EOI*>(expr)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::EOI;
        return e;
    }
    if (auto* ref = dynamic_cast<BBQ::Ref*>(expr)) {
        return compile_ref_path(ref->path);
    }
    if (auto* bin = dynamic_cast<BBQ::BinOp*>(expr)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = map_binop(bin->op);
        e->binary.left = compile_expr(bin->left);
        e->binary.right = compile_expr(bin->right);
        return e;
    }
    if (auto* un = dynamic_cast<BBQ::UnaryOp*>(expr)) {
        if (un->op == BBQ::Unaryop::Abs) {
            // Compile as call to "abs" function
            auto* e = arena_->alloc<CompiledExpr>();
            e->op = ExprOp::Call;
            e->call.func_name = intern("abs");
            e->call.arg_count = 1;
            auto** args = static_cast<CompiledExpr**>(
                arena_->allocate(sizeof(CompiledExpr*), alignof(CompiledExpr*)));
            args[0] = compile_expr(un->operand);
            e->call.args = args;
            return e;
        }
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = map_unop(un->op);
        e->unary.operand = compile_expr(un->operand);
        return e;
    }
    if (auto* tern = dynamic_cast<BBQ::Ternary*>(expr)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::Ternary;
        e->ternary.cond = compile_expr(tern->cond);
        e->ternary.then_ = compile_expr(tern->then_branch);
        e->ternary.else_ = compile_expr(tern->else_branch);
        return e;
    }
    if (auto* call = dynamic_cast<BBQ::Call*>(expr)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::Call;
        e->call.func_name = intern(call->func);
        e->call.arg_count = static_cast<int>(call->args.size());
        if (e->call.arg_count > 0) {
            auto** args = static_cast<CompiledExpr**>(
                arena_->allocate(
                    static_cast<size_t>(e->call.arg_count) * sizeof(CompiledExpr*),
                    alignof(CompiledExpr*)));
            for (int i = 0; i < e->call.arg_count; i++) {
                args[i] = compile_expr(call->args[static_cast<size_t>(i)]);
            }
            e->call.args = args;
        } else {
            e->call.args = nullptr;
        }
        return e;
    }
    // Fallback — should not happen on validated AST
    auto* e = arena_->alloc<CompiledExpr>();
    e->op = ExprOp::IntLit;
    e->int_val = 0;
    return e;
}

CompiledExpr* CEKCompiler::compile_ref_path(BBQ::RefPath* path) {
    if (auto* simple = dynamic_cast<BBQ::Simple*>(path)) {
        // Check for special names
        if (simple->name == "i") {
            auto* e = arena_->alloc<CompiledExpr>();
            e->op = ExprOp::LoopVar;
            return e;
        }
        if (simple->name == "pos") {
            auto* e = arena_->alloc<CompiledExpr>();
            e->op = ExprOp::Pos;
            return e;
        }
        if (simple->name == "remaining") {
            auto* e = arena_->alloc<CompiledExpr>();
            e->op = ExprOp::Remaining;
            return e;
        }
        if (simple->name == "at_end") {
            auto* e = arena_->alloc<CompiledExpr>();
            e->op = ExprOp::AtEnd;
            return e;
        }
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::Ref;
        e->ref_name = intern(simple->name);
        return e;
    }
    if (auto* fa = dynamic_cast<BBQ::FieldAcc*>(path)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::FieldAccess;
        e->field_access.base = compile_ref_path(fa->base);
        e->field_access.field = intern(fa->field);
        return e;
    }
    if (auto* ia = dynamic_cast<BBQ::IndexAcc*>(path)) {
        auto* e = arena_->alloc<CompiledExpr>();
        e->op = ExprOp::IndexAccess;
        e->index_access.base = compile_ref_path(ia->base);
        e->index_access.index = compile_expr(ia->index);
        return e;
    }
    // Fallback
    auto* e = arena_->alloc<CompiledExpr>();
    e->op = ExprOp::IntLit;
    e->int_val = 0;
    return e;
}

// --- Type expression dispatch ---

KontNode* CEKCompiler::compile_type_expr(BBQ::TypeExpr* type,
                                          const char* field_name,
                                          KontNode* next) {
    if (auto* st = dynamic_cast<BBQ::Struct*>(type))
        return compile_struct(st, field_name, next);
    if (auto* alts = dynamic_cast<BBQ::Alternatives*>(type))
        return compile_alternatives(alts, next);
    if (auto* un = dynamic_cast<BBQ::Union*>(type))
        return compile_union(un, next);
    if (auto* arr = dynamic_cast<BBQ::Array*>(type))
        return compile_array(arr, field_name, next);
    if (auto* opt = dynamic_cast<BBQ::Optional*>(type))
        return compile_optional(opt, field_name, next);
    if (auto* sw = dynamic_cast<BBQ::Switch*>(type))
        return compile_switch(sw, next);
    if (auto* prim = dynamic_cast<BBQ::Primitive*>(type))
        return compile_primitive(prim, field_name, next);
    if (auto* rr = dynamic_cast<BBQ::RuleRef*>(type))
        return compile_rule_ref(rr, field_name, next);
    if (auto* ext = dynamic_cast<BBQ::Extern*>(type))
        return compile_extern(ext, field_name, next);
    if (auto* bf = dynamic_cast<BBQ::Bitfield*>(type))
        return compile_bitfield(bf, next);
    if (auto* es = dynamic_cast<BBQ::EndianSwitch*>(type))
        return compile_endian_switch(es, next);
    // Compute is handled at the field level, not here.
    // Should not reach here on validated AST.
    return next;
}

// --- Rule compilation ---

KontNode* CEKCompiler::compile_rule(BBQ::Rule* rule) {
    auto* halt = arena_->alloc<HaltNode>();
    auto* ret = arena_->alloc<ReturnRuleNode>();
    ret->next = halt;

    // If the body is a struct, compile fields directly without
    // BeginStruct/EndStruct. InvokeRuleNode already provides the struct
    // scope for nested calls; for top-level execution, fields go into
    // the root capture created by finish().
    if (auto* st = dynamic_cast<BBQ::Struct*>(rule->body)) {
        KontNode* chain = ret;
        for (int i = static_cast<int>(st->fields.size()) - 1; i >= 0; i--) {
            chain = compile_field(st->fields[static_cast<size_t>(i)], chain);
        }
        return chain;
    }

    return compile_type_expr(rule->body, nullptr, ret);
}

// --- Struct compilation ---

KontNode* CEKCompiler::compile_struct(BBQ::Struct* st, const char* name,
                                       KontNode* next) {
    auto* end = arena_->alloc<EndStructNode>();
    end->next = next;

    // Chain fields in reverse
    KontNode* chain = end;
    for (int i = static_cast<int>(st->fields.size()) - 1; i >= 0; i--) {
        chain = compile_field(st->fields[static_cast<size_t>(i)], chain);
    }

    auto* begin = arena_->alloc<BeginStructNode>();
    begin->struct_name = name;
    begin->next = chain;
    return begin;
}

// --- Field compilation ---

KontNode* CEKCompiler::compile_field(BBQ::Field* field, KontNode* next) {
    const char* name = intern(field->name);

    // EndianSwitch → SetEndianNode (not a real field)
    if (auto* es = dynamic_cast<BBQ::EndianSwitch*>(field->body)) {
        return compile_endian_switch(es, next);
    }

    // Compute → BindComputeNode
    if (auto* comp = dynamic_cast<BBQ::Compute*>(field->body)) {
        auto* node = arena_->alloc<BindComputeNode>();
        node->field_name = name;
        node->expr = compile_expr(comp->expression);
        node->next = next;
        return node;
    }

    KontNode* target = next;

    // Field-level constraint (where clause)
    if (field->constraint) {
        target = wrap_constraint(*field->constraint, next);
    }

    return compile_type_expr(field->body, name, target);
}

// --- Alternatives compilation ---

KontNode* CEKCompiler::compile_alternatives(BBQ::Alternatives* alts,
                                             KontNode* next) {
    int n = static_cast<int>(alts->alts.size());
    if (n == 0) return next;
    if (n == 1) return compile_type_expr(alts->alts[0], nullptr, next);

    auto* commit = arena_->alloc<CommitChoiceNode>();
    commit->next = next;

    // Build alternative chains
    // Last alt chains directly to next (no commit)
    auto** alt_entries = static_cast<KontNode**>(
        arena_->allocate(static_cast<size_t>(n - 1) * sizeof(KontNode*),
                         alignof(KontNode*)));

    for (int i = 1; i < n; i++) {
        KontNode* target = (i == n - 1) ? next : commit;
        alt_entries[i - 1] = compile_type_expr(alts->alts[static_cast<size_t>(i)],
                                                nullptr, target);
    }

    // First alt chains through commit
    KontNode* first = compile_type_expr(alts->alts[0], nullptr, commit);

    auto* begin = arena_->alloc<BeginChoiceNode>();
    begin->alternatives = alt_entries;
    begin->alt_count = n - 1;
    begin->next = first;
    return begin;
}

// --- Union compilation ---

KontNode* CEKCompiler::compile_union(BBQ::Union* un, KontNode* next) {
    int n = static_cast<int>(un->variants.size());
    if (n == 0) return next;
    if (n == 1)
        return compile_type_expr(un->variants[0]->body, nullptr, next);

    auto* commit = arena_->alloc<CommitChoiceNode>();
    commit->next = next;

    auto** alt_entries = static_cast<KontNode**>(
        arena_->allocate(static_cast<size_t>(n - 1) * sizeof(KontNode*),
                         alignof(KontNode*)));

    for (int i = 1; i < n; i++) {
        KontNode* target = (i == n - 1) ? next : commit;
        alt_entries[i - 1] = compile_type_expr(
            un->variants[static_cast<size_t>(i)]->body, nullptr, target);
    }

    KontNode* first = compile_type_expr(un->variants[0]->body, nullptr, commit);

    auto* begin = arena_->alloc<BeginChoiceNode>();
    begin->alternatives = alt_entries;
    begin->alt_count = n - 1;
    begin->next = first;
    return begin;
}

// --- Array compilation ---

KontNode* CEKCompiler::compile_array(BBQ::Array* arr, const char* name,
                                      KontNode* next) {
    auto* end = arena_->alloc<EndArrayNode>();
    end->next = next;

    ArrayMode mode = ArrayMode::FixedCount;
    CompiledExpr* count_expr = nullptr;
    CompiledExpr* until_expr = nullptr;
    BBQ::Separator* sep = nullptr;

    if (auto* fc = dynamic_cast<BBQ::FixedCount*>(arr->spec)) {
        mode = ArrayMode::FixedCount;
        count_expr = compile_expr(fc->count);
    } else if (auto* st = dynamic_cast<BBQ::SepTerm*>(arr->spec)) {
        sep = st->sep;
        if (dynamic_cast<BBQ::EofTerm*>(st->term)) {
            mode = ArrayMode::EOF_;
        } else if (auto* ct = dynamic_cast<BBQ::CountTerm*>(st->term)) {
            mode = ArrayMode::Count;
            count_expr = compile_expr(ct->count);
        } else if (auto* ut = dynamic_cast<BBQ::UntilTerm*>(st->term)) {
            mode = ArrayMode::Until;
            until_expr = compile_expr(ut->condition);
        }
        // TypeTerm deferred
    }

    auto* arr_next = arena_->alloc<ArrayNextNode>();
    arr_next->end_node = end;
    arr_next->mode = mode;
    arr_next->count_expr = count_expr;
    arr_next->until_expr = until_expr;

    // Compile element body, chaining to arr_next
    KontNode* body_entry = compile_type_expr(arr->element, nullptr, arr_next);

    // If separator exists (TypeSep), compile sep chain before body
    if (sep) {
        if (auto* ts = dynamic_cast<BBQ::TypeSep*>(sep)) {
            KontNode* sep_chain = compile_type_expr(ts->sep_type, nullptr, body_entry);
            arr_next->next = sep_chain;
        } else {
            arr_next->next = body_entry;
        }
    } else {
        arr_next->next = body_entry;
    }

    auto* begin = arena_->alloc<BeginArrayNode>();
    begin->array_name = name;
    begin->mode = mode;
    begin->count_expr = count_expr;
    begin->end_node = end;
    begin->next = body_entry;
    return begin;
}

// --- Optional compilation ---

KontNode* CEKCompiler::compile_optional(BBQ::Optional* opt, const char* name,
                                         KontNode* next) {
    if (opt->constraint) {
        // Conditional presence: if constraint true, parse inner; else skip
        KontNode* inner = compile_type_expr(opt->element, name, next);
        auto* eval = arena_->alloc<EvalConstraintNode>();
        eval->constraint = compile_expr(*opt->constraint);
        eval->next = inner;
        eval->on_false = next;
        return eval;
    }

    // Try-parse optional
    auto* end_opt = arena_->alloc<EndOptionalNode>();
    end_opt->next = next;
    KontNode* inner = compile_type_expr(opt->element, name, end_opt);
    auto* begin = arena_->alloc<BeginOptionalNode>();
    begin->inner = inner;
    begin->next = next;  // Skip point
    return begin;
}

// --- Switch compilation ---

KontNode* CEKCompiler::compile_switch(BBQ::Switch* sw, KontNode* next) {
    auto* node = arena_->alloc<SwitchDispatchNode>();
    node->discriminator = compile_expr(sw->discriminator);

    int n = static_cast<int>(sw->cases.size());
    node->case_count = n;
    if (n > 0) {
        node->cases = static_cast<SwitchDispatchNode::Case*>(
            arena_->allocate(static_cast<size_t>(n) * sizeof(SwitchDispatchNode::Case),
                             alignof(SwitchDispatchNode::Case)));
        for (int i = 0; i < n; i++) {
            auto* sc = sw->cases[static_cast<size_t>(i)];
            KontNode* target = compile_type_expr(sc->target, nullptr, next);

            int64_t val = 0;
            const char* str = nullptr;
            if (auto* iv = dynamic_cast<BBQ::IntValue*>(sc->value)) {
                val = iv->value;
            } else if (auto* sv = dynamic_cast<BBQ::StrValue*>(sc->value)) {
                str = intern(sv->value);
                val = static_cast<int64_t>(reinterpret_cast<intptr_t>(str));
            } else if (auto* idv = dynamic_cast<BBQ::IdentValue*>(sc->value)) {
                str = intern(idv->name);
                val = static_cast<int64_t>(reinterpret_cast<intptr_t>(str));
            }

            node->cases[i] = {val, str, target};
        }
    } else {
        node->cases = nullptr;
    }

    if (sw->default_) {
        node->default_target = compile_type_expr(
            (*sw->default_)->target, nullptr, next);
    } else {
        node->default_target = nullptr;
    }

    return node;
}

// --- Primitive compilation ---

KontNode* CEKCompiler::compile_primitive(BBQ::Primitive* prim, const char* name,
                                          KontNode* next) {
    // bytes/string → MatchBytesNode
    if (dynamic_cast<BBQ::BytesKind*>(prim->kind) ||
        dynamic_cast<BBQ::StringKind*>(prim->kind)) {
        auto* node = arena_->alloc<MatchBytesNode>();
        node->field_name = name;
        node->is_string = (dynamic_cast<BBQ::StringKind*>(prim->kind) != nullptr);

        // Length comes from the interval on the primitive
        if (prim->interval) {
            if (auto* len = dynamic_cast<BBQ::Length*>(*prim->interval)) {
                node->length_expr = compile_expr(len->length);
            } else if (auto* se = dynamic_cast<BBQ::StartEnd*>(*prim->interval)) {
                // length = end - start
                auto* sub = arena_->alloc<CompiledExpr>();
                sub->op = ExprOp::Sub;
                sub->binary.left = compile_expr(se->end);
                sub->binary.right = compile_expr(se->start);
                node->length_expr = sub;
            }
        } else {
            // No interval — zero length
            auto* zero = arena_->alloc<CompiledExpr>();
            zero->op = ExprOp::IntLit;
            zero->int_val = 0;
            node->length_expr = zero;
        }

        node->next = prim->constraint ? wrap_constraint(*prim->constraint, next) : next;
        return node;
    }

    // Regular primitive → MatchPrimitiveNode
    auto* node = arena_->alloc<MatchPrimitiveNode>();
    node->field_name = name;
    node->prim = to_prim_info(prim->kind);
    node->next = prim->constraint ? wrap_constraint(*prim->constraint, next) : next;
    return node;
}

// --- Rule reference compilation ---

KontNode* CEKCompiler::compile_rule_ref(BBQ::RuleRef* ref, const char* name,
                                         KontNode* next) {
    auto* node = arena_->alloc<InvokeRuleNode>();
    node->rule_name = name ? name : intern(ref->name);
    auto it = rule_entries_.find(ref->name);
    node->entry = (it != rule_entries_.end()) ? it->second : nullptr;
    node->next = ref->constraint ? wrap_constraint(*ref->constraint, next) : next;
    return node;
}

// --- Extern compilation ---

KontNode* CEKCompiler::compile_extern(BBQ::Extern* ext, const char* name,
                                       KontNode* next) {
    auto* node = arena_->alloc<ExternalCallNode>();
    node->func_name = intern(ext->func_name);
    node->field_name = name;
    node->next = ext->constraint ? wrap_constraint(*ext->constraint, next) : next;
    return node;
}

// --- Bitfield compilation ---

KontNode* CEKCompiler::compile_bitfield(BBQ::Bitfield* bf, KontNode* next) {
    auto* node = arena_->alloc<BitfieldReadNode>();
    node->container = to_prim_info(bf->container);

    int n = static_cast<int>(bf->entries.size());
    node->entry_count = n;
    if (n > 0) {
        node->entries = static_cast<BitfieldReadNode::Entry*>(
            arena_->allocate(static_cast<size_t>(n) * sizeof(BitfieldReadNode::Entry),
                             alignof(BitfieldReadNode::Entry)));
        for (int i = 0; i < n; i++) {
            node->entries[i] = {intern(bf->entries[static_cast<size_t>(i)]->name),
                                static_cast<int>(bf->entries[static_cast<size_t>(i)]->width)};
        }
    } else {
        node->entries = nullptr;
    }

    node->next = next;
    return node;
}

// --- EndianSwitch compilation ---

KontNode* CEKCompiler::compile_endian_switch(BBQ::EndianSwitch* es,
                                              KontNode* next) {
    auto* node = arena_->alloc<SetEndianNode>();
    node->expr = compile_expr(es->endian_expr);
    node->next = next;
    return node;
}

// --- Top-level compilation ---

CompiledGrammar* CEKCompiler::compile(BBQ::Grammar* grammar,
                                       const bbqgen::Sema& sema) {
    auto* result = new CompiledGrammar();
    arena_ = &result->arena;
    strings_ = &result->strings;
    rule_entries_.clear();

    // Default endianness
    result->default_little_endian =
        (sema.default_endian() != BBQ::Endianness::Big);

    // Compile rules in topo order
    const auto& sorted = sema.sorted_rules();
    int count = static_cast<int>(sorted.size());
    result->rule_count = count;

    if (count > 0) {
        result->rules = static_cast<CompiledGrammar::RuleEntry*>(
            arena_->allocate(static_cast<size_t>(count) * sizeof(CompiledGrammar::RuleEntry),
                             alignof(CompiledGrammar::RuleEntry)));
    }

    for (int i = 0; i < count; i++) {
        BBQ::Rule* rule = sorted[static_cast<size_t>(i)];
        const char* name = intern(rule->name);
        KontNode* entry = compile_rule(rule);
        rule_entries_[rule->name] = entry;
        result->rules[i] = {name, entry};
    }

    // Clean up compiler state
    arena_ = nullptr;
    strings_ = nullptr;
    rule_entries_.clear();

    return result;
}

} // namespace bbq::cek
