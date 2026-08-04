#include "spec.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace opgen {

int Spec::type_bytes(ValueType ty) {
    switch (ty) {
        case ValueType::TyI8:
        case ValueType::TyU8:    return 1;
        case ValueType::TyI16:
        case ValueType::TyU16:
        case ValueType::TyRef:
        case ValueType::TyWord:  return 2;
        case ValueType::TyI32:
        case ValueType::TyDword: return 4;
        case ValueType::TyF32:   return 4;
        case ValueType::TyI64:
        case ValueType::TyF64:   return 8;
        case ValueType::TyAny:   return 8;   // runtime-typed; max width for sizing
        case ValueType::TyAddr:  return 8;   // addrtype i32/i64 — size as the max width
        case ValueType::TyV128:
        case ValueType::TyI8x16: case ValueType::TyI16x8: case ValueType::TyI32x4:
        case ValueType::TyI64x2: case ValueType::TyF32x4: case ValueType::TyF64x2:
            return 16;   // v128 + its lane views
        case ValueType::TyFuncRef:
        case ValueType::TyExternRef:
        case ValueType::TyI31Ref: return 8;
        // LEB operands are variable-length in the stream — this is the DECODED
        // carrier width, not an encoded size; the LEB reader advances the cursor.
        case ValueType::TyUleb32:
        case ValueType::TySleb32: return 4;
        case ValueType::TyUleb64:
        case ValueType::TySleb64: return 8;
        case ValueType::TyMemarg: return 8;   // decoded offset carrier (u64)
    }
    return 0;
}

int Spec::stack_slots(ValueType ty) const {
    // Prefer the spec's `type` declarations so the slot model follows the
    // target (JVM int=1 slot, long/double=2); else the JCVM 16-bit default.
    if (mod_) {
        for (auto* td : mod_->types)
            if (td->ty == ty) return td->slots;
    }
    switch (ty) {
        case ValueType::TyI8:
        case ValueType::TyU8:
        case ValueType::TyI16:
        case ValueType::TyU16:
        case ValueType::TyRef:
        case ValueType::TyWord:  return 1;
        case ValueType::TyI32:
        case ValueType::TyDword: return 2;
        case ValueType::TyF32:   return 1;   // JVM 32-bit slot model
        case ValueType::TyI64:
        case ValueType::TyF64:   return 2;
        case ValueType::TyAny:   return 1;   // runtime-typed; 1/2 from the stack tag
        case ValueType::TyAddr:  return 1;   // addrtype: always ONE slot (i32 or i64) in the single-slot model
        case ValueType::TyV128:
        case ValueType::TyI8x16: case ValueType::TyI16x8: case ValueType::TyI32x4:
        case ValueType::TyI64x2: case ValueType::TyF32x4: case ValueType::TyF64x2:
        case ValueType::TyFuncRef:
        case ValueType::TyExternRef:
        case ValueType::TyI31Ref: return 1;
        case ValueType::TyUleb32:
        case ValueType::TyUleb64:
        case ValueType::TySleb32:
        case ValueType::TySleb64:
        case ValueType::TyMemarg: return 1;   // operand carriers, never stacked
    }
    return 0;
}

// ── Flag / mnemonic predicates ──────────────────────────────

static bool has_flag(const Opcode* op, const char* name) {
    for (auto* f : op->flags)
        if (std::strcmp(f, name) == 0) return true;
    return false;
}

static bool any_flag_starts_with(const Opcode* op, const char* prefix) {
    size_t plen = std::strlen(prefix);
    for (auto* f : op->flags)
        if (std::strncmp(f, prefix, plen) == 0) return true;
    return false;
}

static bool starts_with(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

// True if `e` is a direct `locals[...]` slot access.
static bool is_local_slot(SemExpr* e) {
    auto* idx = as<SIndex>(e);
    if (!idx) return false;
    auto* base = as<SIdent>(idx->base);
    return base && std::strcmp(base->name, "locals") == 0;
}

// OR into `f` the local-access bits implied by one statement's actual effect:
// `locals[i] = ..` is a STORE (incl. the read-modify-write of sinc/iinc);
// `result = locals[i]` — a stack value read straight from a slot — is a LOAD.
// Recurses into blocks.
static void local_access_flags(SemStmt* s, unsigned& f) {
    if (!s) return;
    if (auto* asn = as<SAssign>(s)) {
        if (is_local_slot(asn->target))     f |= OPCF_LOCAL_STORE;
        else if (is_local_slot(asn->value)) f |= OPCF_LOCAL_LOAD;
    } else if (auto* blk = as<SBlock>(s)) {
        for (auto* st : blk->stmts) local_access_flags(st, f);
    }
}

// ── Per-opcode derivations ──────────────────────────────────

int Spec::compute_length(const Opcode* op) const {
    if (has_flag(op, "special_switch")) return OPCL_VARIABLE;
    int total = 1;  // opcode byte
    for (auto* o : op->operands) total += type_bytes(o->ty);
    return total;
}

// §3b: an op with a variadic stack param (`count * ty name`) pops a runtime-determined number of operands.
static bool has_variadic_stack(const Opcode* op) {
    for (auto* s : op->stack_in)  if (s->count.has_value()) return true;
    for (auto* s : op->stack_out) if (s->count.has_value()) return true;
    return false;
}

int Spec::compute_sp_delta(const Opcode* op) const {
    if (any_flag_starts_with(op, "special_") || has_variadic_stack(op)) return OPCD_VARIABLE;
    int in = 0, out = 0;
    for (auto* s : op->stack_in)  in  += stack_slots(s->ty);
    for (auto* s : op->stack_out) out += stack_slots(s->ty);
    return out - in;
}

int Spec::compute_sp_pops(const Opcode* op) const {
    if (any_flag_starts_with(op, "special_") || has_variadic_stack(op)) return OPCD_VARIABLE;
    int in = 0;
    for (auto* s : op->stack_in) in += stack_slots(s->ty);
    return in;
}

unsigned Spec::compute_flags(const Opcode* op) const {
    unsigned f = 0;
    if (op->br.has_value() || has_flag(op, "branch")) f |= OPCF_BRANCH;
    if (std::strcmp(op->mnemonic, "sreturn") == 0 ||
        std::strcmp(op->mnemonic, "ireturn") == 0 ||
        std::strcmp(op->mnemonic, "areturn") == 0 ||
        std::strcmp(op->mnemonic, "return")  == 0) f |= OPCF_RETURN;
    if (starts_with(op->mnemonic, "invoke")) f |= OPCF_INVOKE;
    if (has_flag(op, "special_switch"))      f |= OPCF_SWITCH;
    for (auto* o : op->operands)
        if (o->cp_ref) { f |= OPCF_HAS_CP_REF; break; }
    // ENDS_BB: anything that doesn't fall through to the next sequential op.
    if ((f & (OPCF_BRANCH | OPCF_RETURN | OPCF_SWITCH)) ||
        std::strcmp(op->mnemonic, "athrow") == 0 ||
        has_flag(op, "special_jsr") ||
        has_flag(op, "special_ret")) {
        f |= OPCF_ENDS_BB;
    }
    // Local access is read from the body's actual effect, not the
    // `local_value` annotation (the `_this` field ops carry `local_value`
    // for the implicit `this` slot but neither load nor store a local).
    for (auto* s : op->sem_body) local_access_flags(s, f);
    // A spec whose semantics live in a hand-written interpreter declares its
    // opcodes without bodies, so there is no effect to read. The declaration
    // then says it: a local_value opcode that pushes is a load, one that pops
    // is a store, and one with no stack effect that falls through is a
    // read-modify-write. `ret` reads a slot but ends the block, so it is
    // none of the three and excludes itself through the ENDS_BB test above.
    if (op->sem_body.empty() && op->local_value.has_value()) {
        if (!op->stack_out.empty())     f |= OPCF_LOCAL_LOAD;
        else if (!op->stack_in.empty()) f |= OPCF_LOCAL_STORE;
        else if (!(f & OPCF_ENDS_BB))   f |= OPCF_LOCAL_STORE;
    }
    return f;
}

int Spec::compute_cp_ref_offset(const Opcode* op) const {
    // Byte offset within the instruction: opcode at 0, operands follow.
    int off = 1;  // skip opcode byte
    for (auto* o : op->operands) {
        if (o->cp_ref) return off;
        off += type_bytes(o->ty);
    }
    return 0;
}

Spec::Spec(const Module* m) : mod_(m) {
    // A fact must name a declared vocabulary and one of its members. opgen
    // does not know what the names mean, but it does know whether the spec
    // declared them — an undeclared one is a typo that would otherwise reach
    // a consumer as a fact that is simply never true.
    for (auto* op : m->opcodes) {
        for (auto* f : op->facts) {
            const FactDecl* decl = nullptr;
            for (auto* d : m->fact_decls)
                if (std::strcmp(d->name, f->name) == 0) { decl = d; break; }
            if (!decl) {
                fprintf(stderr, "opgen: %s: no `facts %s` declaration\n",
                        op->mnemonic, f->name);
                std::exit(1);
            }
            bool member = false;
            for (auto* mem : decl->members)
                if (std::strcmp(mem, f->value) == 0) { member = true; break; }
            if (!member) {
                fprintf(stderr, "opgen: %s: `%s` is not a declared member of `facts %s`\n",
                        op->mnemonic, f->value, f->name);
                std::exit(1);
            }
        }
    }
    for (auto* op : m->opcodes) {
        unsigned idx = static_cast<unsigned>(op->opcode_val) & 0xff;
        Derived& d = derived_[idx];
        d.present       = true;
        d.ast           = op;
        d.length        = compute_length(op);
        d.sp_delta      = compute_sp_delta(op);
        d.flags         = compute_flags(op);
        d.cp_ref_offset = compute_cp_ref_offset(op);
        d.sp_pops       = compute_sp_pops(op);
        d.local_index   = op->local_index.has_value()
                              ? (*op->local_index)->index : -1;
    }
    // narrow_form names another opcode by mnemonic, so it resolves only once
    // every opcode is in the table.
    for (auto* op : m->opcodes) {
        if (!op->narrow_form.has_value()) continue;
        const char* want = (*op->narrow_form)->mnemonic;
        for (auto* other : m->opcodes) {
            if (std::strcmp(other->mnemonic, want) == 0) {
                derived_[static_cast<unsigned>(op->opcode_val) & 0xff].narrow_form =
                    other->opcode_val;
                break;
            }
        }
    }
}

} // namespace opgen
