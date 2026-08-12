// The tier-2 storage-class signature vocabulary: one walk over Module.opcodes
// emitting the signature table (<prefix>_sigtab.h), the tree IR schema
// (<prefix>_ttree.asdl) and the tiling grammar (<prefix>_tile.burg).
//
// The terminals a tree matcher can see are not the opcodes: 499 opcodes share
// far fewer STORAGE-CLASS signatures, and a tiling that names opcodes would
// carry 499 near-identical rule families for the handful of distinct shapes
// underneath. What a rule has to fix is the class of every operand and of the
// result — `(i32)->(i32)` cannot stand for both i32.eqz and i32.trunc_f64_s —
// so the signature IS the terminal.
#include "sigemit.h"
#include "spec.h"     /* Spec::forwarded_name — which stack slots the spec says are one value */

#include <cstdlib>
#include <cstring>
#include <set>

namespace opgen {

// ── the class vocabulary ────────────────────────────────────

static const char* const kClassName[] = {
    "i32", "i64", "f32", "f64", "v128", "ref", "stk", "addr", "poly"
};
static const char* const kClassEnum[] = {
    "JSC_I32", "JSC_I64", "JSC_F32", "JSC_F64", "JSC_V128", "JSC_REF",
    "JSC_STK", "JSC_ADDR", "JSC_POLY"
};
static const char* cname(SClass c) { return kClassName[(unsigned)c]; }

bool Sig::is_final() const {
    for (auto c : params)  if ((unsigned)c >= SCLASS_FINAL) return false;
    for (auto c : results) if ((unsigned)c >= SCLASS_FINAL) return false;
    return true;
}

// The variadic group is one signature slot and no tree child: opgen leaves those
// operands on the value stack for the body to read in place, so nothing below
// them is a subtree the matcher can tile.
int Sig::nkids() const {
    int n = 0;
    for (auto c : params) n += (c != SClass::Stk);
    return n;
}

std::string Sig::name() const {
    if (kind == SigKind::Carried) return std::string("Carried_") + cname(results[0]);
    std::string s = "Sig";
    for (auto c : params) { s += '_'; s += cname(c); }
    s += "_to";
    if (results.empty()) s += "_void";
    for (auto c : results) { s += '_'; s += cname(c); }
    return s;
}

// ── classification ──────────────────────────────────────────

// The class a value type is HELD in. The SIMD lane views are views of one v128
// (opgen.asdl: "lane views of a v128 (storage = v128)"), and every reference
// form is one 8-byte managed handle — the tiling cares where a value lives, not
// what the type system calls it.
SClass SigEmitter::classify_final(ValueType ty, const char* mnemonic, const char* param) {
    switch (ty) {
    case ValueType::TyI32:   return SClass::I32;
    case ValueType::TyI64:   return SClass::I64;
    case ValueType::TyF32:   return SClass::F32;
    case ValueType::TyF64:   return SClass::F64;
    case ValueType::TyV128:
    case ValueType::TyI8x16: case ValueType::TyI16x8: case ValueType::TyI32x4:
    case ValueType::TyI64x2: case ValueType::TyF32x4: case ValueType::TyF64x2:
        return SClass::V128;
    case ValueType::TyRef:
    case ValueType::TyFuncRef:
    case ValueType::TyExternRef:
    case ValueType::TyI31Ref: return SClass::Ref;
    case ValueType::TyAddr:   return SClass::Addr;
    // A `word`/`any` stack slot is the spec declining to name a type it does not
    // need: tier-1 moves whole slots. A register does need one.
    case ValueType::TyWord:
    case ValueType::TyDword:
    case ValueType::TyAny:    return SClass::Poly;
    default: break;
    }
    fprintf(stderr, "opgen: %s: stack param `%s` has no storage class\n",
            mnemonic, param);
    std::exit(1);
}

// The addrtype sources one `addr(<expr>)` names. The spec writes them as
// `<pred>(<operand>)` conjunctions and nothing else; an unrecognised shape is a
// spec form this walk cannot answer, so it stops rather than resolve it wrong.
void SigEmitter::collect_atoms(const SemExpr* e, std::vector<Atom>& out,
                               const Opcode* op) {
    if (auto* b = as<const SBinOp>(e)) {
        if (std::strcmp(b->op, "&&") == 0) {
            collect_atoms(b->left, out, op);
            collect_atoms(b->right, out, op);
            return;
        }
    } else if (auto* c = as<const SCall>(e)) {
        if (c->args.size() == 1) {
            if (auto* id = as<const SIdent>(c->args[0])) {
                int idx = -1;
                for (size_t i = 0; i < op->operands.size(); i++)
                    if (std::strcmp(op->operands[i]->name, id->name) == 0) idx = (int)i;
                // `memidx` is not an operand: a memarg decodes it out of its own
                // align-flag bit (bit 6), which is where vmemit binds it too.
                if (idx < 0 && std::strcmp(id->name, "memidx") == 0)
                    for (size_t i = 0; i < op->operands.size(); i++)
                        if (op->operands[i]->ty == ValueType::TyMemarg) idx = (int)i;
                if (idx < 0) {
                    fprintf(stderr, "opgen: %s: addr() names `%s`, which is neither an "
                                    "operand nor a memarg's memidx\n",
                            op->mnemonic, id->name);
                    std::exit(1);
                }
                for (auto& a : out)
                    if (a.pred == c->name && a.ident == id->name) return;
                out.push_back({c->name, id->name, idx});
                return;
            }
        }
    }
    fprintf(stderr, "opgen: %s: addr() source is not a conjunction of "
                    "<pred>(<operand>) forms\n", op->mnemonic);
    std::exit(1);
}

unsigned SigEmitter::atom_mask(const SemExpr* e, const std::vector<Atom>& atoms,
                               const Opcode* op) {
    if (auto* b = as<const SBinOp>(e))
        if (std::strcmp(b->op, "&&") == 0)
            return atom_mask(b->left, atoms, op) | atom_mask(b->right, atoms, op);
    (void)op;
    auto* c = as<const SCall>(e);
    auto* id = c ? as<const SIdent>(c->args[0]) : nullptr;
    for (size_t i = 0; i < atoms.size(); i++)
        if (atoms[i].pred == c->name && atoms[i].ident == id->name)
            return 1u << i;
    return 0;
}

// Does `e` merely FORWARD a value, or does it compute one? A bare name and a
// ternary between names carry a value through unchanged, so whatever class it
// had it still has; anything else — a call, a cast, an operator, an index — is
// a new value of a class this expression does not name. Purely structural: the
// tag vocabulary a body assigns (T_GCREF, T_LONG, …) is the BACKEND's, and
// decoding it here would put a target's spellings in the generator.
static bool forwards(const SemExpr* e, std::vector<const char*>& names) {
    if (auto* id = as<const SIdent>(e)) { names.push_back(id->name); return true; }
    if (auto* t = as<const STernary>(e))
        return forwards(t->cond, names) && forwards(t->then_, names) &&
               forwards(t->else_, names);
    return false;
}

// `name` or `name_wt` — the value and its runtime tag are the same slot, and
// either assignment says the same thing about where the class came from.
static bool slot_named(const char* ref, const char* slot) {
    size_t n = std::strlen(slot);
    if (std::strncmp(ref, slot, n) != 0) return false;
    return ref[n] == '\0' || std::strcmp(ref + n, "_wt") == 0;
}

static void forwarding_ties(const SemStmt* s, const Opcode* op,
                            const std::vector<int>& poly_slot,
                            std::vector<int>& uf) {
    if (!s) return;
    if (auto* blk = as<const SBlock>(s)) {
        for (auto* st : blk->stmts) forwarding_ties(st, op, poly_slot, uf);
        return;
    }
    auto* asn = as<const SAssign>(s);
    if (!asn) return;
    auto* tgt = as<const SIdent>(asn->target);
    if (!tgt) return;
    std::vector<const char*> names;
    if (!forwards(asn->value, names)) return;

    size_t nin = op->stack_in.size();
    auto slot_of = [&](const char* ref) -> int {
        for (size_t i = 0; i < nin; i++)
            if (slot_named(ref, op->stack_in[i]->name)) return (int)i;
        for (size_t k = 0; k < op->stack_out.size(); k++)
            if (slot_named(ref, op->stack_out[k]->name)) return (int)(nin + k);
        return -1;
    };
    auto find = [&](int x) { while (uf[x] != x) x = uf[x] = uf[uf[x]]; return x; };

    int dst = slot_of(tgt->name);
    if (dst < 0 || poly_slot[dst] < 0) return;
    for (const char* n : names) {
        int src = slot_of(n);
        if (src < 0 || poly_slot[src] < 0) continue;
        uf[find(src)] = find(dst);
    }
}

SigEmitter::OpInfo SigEmitter::describe(const Opcode* op) const {
    OpInfo oi;
    oi.op = op;
    auto add = [&](const StackParam* sp, bool is_param) {
        SClass c;
        if (sp->count.has_value()) {
            c = SClass::Stk;
            if (is_param) oi.variadic_param = (int)oi.decl.params.size();
        } else if (sp->at_src.has_value()) {
            c = SClass::Addr;
        } else {
            c = classify_final(sp->ty, op->mnemonic, sp->name);
        }
        (is_param ? oi.decl.params : oi.decl.results).push_back(c);
    };
    for (auto* sp : op->stack_in)  add(sp, true);
    for (auto* sp : op->stack_out) add(sp, false);

    // ADDR: one atom per distinct module entry, one mask per slot.
    for (auto* sp : op->stack_in)
        if (sp->at_src.has_value()) collect_atoms(*sp->at_src, oi.atoms, op);
    oi.param_atoms.assign(oi.decl.params.size(), 0);
    for (size_t i = 0; i < op->stack_in.size(); i++)
        if (op->stack_in[i]->at_src.has_value())
            oi.param_atoms[i] = atom_mask(*op->stack_in[i]->at_src, oi.atoms, op);

    // POLY: one tie group per independently-typed slot. Slots the spec forwards
    // between share a class — `select`'s three `word`s are §3.4.1's one `t`, and
    // its body says so (`result = c ? v1 : v2`) — so enumerating them
    // independently would mint signatures like (i32 f64 i32)->(v128) that no
    // instruction has, which is exactly the IR-impossible shape that makes a
    // coverage claim meaningless.
    size_t nin = op->stack_in.size(), nslots = oi.decl.params.size() + oi.decl.results.size();
    oi.poly_group.assign(nslots, -1);
    std::vector<int> poly_slot(nslots, -1), uf(nslots);
    for (size_t i = 0; i < nslots; i++) {
        uf[i] = (int)i;
        poly_slot[i] = ((i < nin ? oi.decl.params[i] : oi.decl.results[i - nin])
                            == SClass::Poly) ? 1 : -1;
    }
    // The declared forward: a stack_out written `= "<in>"` IS that input value.
    auto find = [&](int x) { while (uf[x] != x) x = uf[x] = uf[uf[x]]; return x; };
    for (size_t k = 0; k < op->stack_out.size(); k++) {
        std::string src = Spec::forwarded_name(op->stack_out[k]);
        if (src.empty()) continue;
        size_t slot = nin + k;
        if (poly_slot[slot] < 0) continue;
        for (size_t i = 0; i < nin; i++)
            if (poly_slot[i] > 0 && src == op->stack_in[i]->name)
                uf[find((int)i)] = find((int)slot);
    }
    for (auto* s : op->sem_body) forwarding_ties(s, op, poly_slot, uf);

    std::map<int, int> group_of;
    for (size_t i = 0; i < nslots; i++) {
        if (poly_slot[i] < 0) continue;
        int root = find((int)i);
        auto it = group_of.find(root);
        if (it == group_of.end()) it = group_of.emplace(root, oi.npoly_groups++).first;
        oi.poly_group[i] = it->second;
    }
    return oi;
}

int SigEmitter::intern(const Sig& s) {
    auto it = sig_ids_.find(s);
    if (it != sig_ids_.end()) return it->second;
    int id = (int)sigs_.size();
    sigs_.push_back(s);
    sig_ids_[s] = id;
    return id;
}

// Every signature this opcode can become: each addrtype atom is i32 or i64
// independently, and each POLY tie group ranges over the register classes.
void SigEmitter::resolve(OpInfo& oi) {
    std::set<int> got;
    size_t nin = oi.decl.params.size();
    for (unsigned bits = 0; bits < (1u << oi.atoms.size()); bits++) {
        long combos = 1;
        for (int g = 0; g < oi.npoly_groups; g++) combos *= SCLASS_FINAL - 1;
        for (long k = 0; k < combos; k++) {
            Sig s = oi.decl;
            long rest = k;
            std::vector<SClass> pick((size_t)oi.npoly_groups);
            for (int g = 0; g < oi.npoly_groups; g++) {
                pick[g] = (SClass)(rest % (SCLASS_FINAL - 1));
                rest /= (SCLASS_FINAL - 1);
            }
            for (size_t i = 0; i < s.params.size(); i++) {
                if (s.params[i] == SClass::Addr)
                    s.params[i] = ((bits & oi.param_atoms[i]) == oi.param_atoms[i])
                                      ? SClass::I64 : SClass::I32;
                else if (s.params[i] == SClass::Poly)
                    s.params[i] = pick[oi.poly_group[i]];
            }
            for (size_t i = 0; i < s.results.size(); i++)
                if (s.results[i] == SClass::Poly)
                    s.results[i] = pick[oi.poly_group[nin + i]];
            got.insert(intern(s));
        }
    }
    oi.resolved.assign(got.begin(), got.end());
}

SigEmitter::SigEmitter(const Module* mod, std::string prefix)
    : mod_(mod), prefix_(std::move(prefix)) {
    for (char c : prefix_) uprefix_ += (char)toupper((unsigned char)c);
    build();
}

void SigEmitter::build() {
    for (auto* op : mod_->opcodes) {
        OpInfo oi = describe(op);
        intern(oi.decl);          // the declared form keeps an id of its own
        resolve(oi);
        ops_.push_back(std::move(oi));
    }
    // One canonical-state leaf per register class: the value a previous region
    // left on the memory stack, which the next region's first consumer needs a
    // child for. A leaf, so no params; its own terminal, so the stitcher can
    // tell "already there" from "materialise a constant".
    for (unsigned c = 0; c < SCLASS_FINAL; c++) {
        if ((SClass)c == SClass::Stk) continue;   // the variadic group is not a value
        Sig s; s.kind = SigKind::Carried; s.results.push_back((SClass)c);
        carried_ids_.push_back(intern(s));
    }
    for (size_t i = 0; i < sigs_.size(); i++)
        if (sigs_[i].is_final()) final_ids_.push_back((int)i);
}

// ── <prefix>_sigtab.h ───────────────────────────────────────

void SigEmitter::put_guard(FILE* o, const char* what) const {
    fprintf(o, "/* AUTO-GENERATED by opgen — do not edit. %s */\n", what);
}

void SigEmitter::emit_sigtab_h(FILE* o) {
    int maxp = 0, maxr = 0, maxkids = 0;
    for (auto& s : sigs_) {
        if ((int)s.params.size() > maxp)  maxp = (int)s.params.size();
        if ((int)s.results.size() > maxr) maxr = (int)s.results.size();
        if (s.is_final() && s.nkids() > maxkids) maxkids = s.nkids();
    }
    size_t max_atoms = 0, max_res = 0;
    for (auto& oi : ops_) {
        if (oi.atoms.size() > max_atoms) max_atoms = oi.atoms.size();
        if (oi.resolved.size() > max_res) max_res = oi.resolved.size();
    }

    put_guard(o, "Tier-2 storage-class signatures.");
    fprintf(o,
        "#ifndef %s_SIGTAB_H\n#define %s_SIGTAB_H\n"
        "#include <stddef.h>\n#include <stdint.h>\n#include \"%s_valtype.h\"\n\n"
        "/* What a value has to be HELD in. The operand stack is one uniform slot per\n"
        " * value, so a stack-resident value needs no class; a REGISTER-resident one\n"
        " * does, which is why tier-1 never had to answer this and tier-2 does. */\n"
        "typedef enum {\n"
        "    JSC_I32 = 0, JSC_I64, JSC_F32, JSC_F64, JSC_V128, JSC_REF,\n"
        "    JSC_STK,    /* the variadic operand group: one signature slot, no tree child */\n"
        "    JSC_ADDR,   /* non-final: i32|i64, the addressed entry's declared addrtype (§2.3.11) */\n"
        "    JSC_POLY,   /* non-final: the declared local/global/field/element/operand type */\n"
        "    JSC_COUNT\n} %s_sclass_t;\n"
        "#define %s_SCLASS_FINAL   %u   /* JSC_I32..JSC_STK need no resolution */\n\n",
        uprefix_.c_str(), uprefix_.c_str(), prefix_.c_str(), prefix_.c_str(),
        uprefix_.c_str(), SCLASS_FINAL);
    fprintf(o,
        "#define %s_SIG_COUNT        %zu\n"
        "#define %s_SIG_MAX_PARAMS   %d\n"
        "#define %s_SIG_MAX_RESULTS  %d\n"
        "#define %s_SIG_MAX_KIDS     %d   /* the burg arity ceiling */\n"
        "#define %s_SIG_MAX_RESOLVE  %zu\n\n",
        uprefix_.c_str(), sigs_.size(), uprefix_.c_str(), maxp, uprefix_.c_str(), maxr,
        uprefix_.c_str(), maxkids, uprefix_.c_str(), max_res);

    fprintf(o,
        "/* One signature. `final` ones are the burg terminals and the %s_ttree.asdl\n"
        " * constructors; the rest are the declared form an opcode carries, and\n"
        " * `resolves_to` lists every terminal it can become. */\n"
        "typedef struct {\n"
        "    const char*     name;\n"
        "    uint8_t         nparams;    /* signature slots, JSC_STK included */\n"
        "    uint8_t         nkids;      /* burg arity: the slots that are tree children */\n"
        "    uint8_t         nresults;\n"
        "    uint8_t         final;\n"
        "    uint8_t         params[%s_SIG_MAX_PARAMS];\n"
        "    uint8_t         results[%s_SIG_MAX_RESULTS];\n"
        "    uint16_t        nresolve;\n"
        "    const uint16_t* resolves_to;\n"
        "} %s_sig_t;\n\n",
        prefix_.c_str(), uprefix_.c_str(), uprefix_.c_str(), prefix_.c_str());

    // Per-signature resolution lists, unioned over the opcodes that declare it.
    std::vector<std::set<int>> res(sigs_.size());
    for (auto& oi : ops_) {
        int id = sig_ids_.find(oi.decl)->second;
        res[id].insert(oi.resolved.begin(), oi.resolved.end());
    }
    for (size_t i = 0; i < sigs_.size(); i++) {
        if (res[i].empty()) continue;
        fprintf(o, "static const uint16_t %s_sigres_%zu[] = {", prefix_.c_str(), i);
        for (int r : res[i]) fprintf(o, " %d,", r);
        fputs(" };\n", o);
    }

    fprintf(o, "\nstatic const %s_sig_t %s_sigtab[%s_SIG_COUNT] = {\n",
            prefix_.c_str(), prefix_.c_str(), uprefix_.c_str());
    for (size_t i = 0; i < sigs_.size(); i++) {
        const Sig& s = sigs_[i];
        fprintf(o, "    [%zu] = { \"%s\", %zu, %d, %zu, %d, {", i, s.name().c_str(),
                s.params.size(), s.nkids(), s.results.size(), s.is_final() ? 1 : 0);
        for (auto c : s.params) fprintf(o, " %s,", kClassEnum[(unsigned)c]);
        fputs(" }, {", o);
        for (auto c : s.results) fprintf(o, " %s,", kClassEnum[(unsigned)c]);
        if (res[i].empty()) fputs(" }, 0, NULL },\n", o);
        else fprintf(o, " }, %zu, %s_sigres_%zu },\n", res[i].size(), prefix_.c_str(), i);
    }
    fputs("};\n\n", o);

    // opcode -> declared signature id, and the ADDR resolution inputs.
    fprintf(o,
        "/* The addrtype resolution inputs (§2.3.11). Each ATOM is a module entry an\n"
        " * ADDR slot reads, named by the predicate the spec wrote and the operand that\n"
        " * indexes it; a slot is i64 when EVERY atom it names is — memory.copy's length\n"
        " * is 64-bit only when both memories are. */\n"
        "typedef struct { const char* pred; const char* ident; uint8_t operand; } %s_addr_atom_t;\n"
        "typedef struct {\n"
        "    uint16_t                sig;        /* declared signature id */\n"
        "    uint8_t                 natoms;\n"
        "    const %s_addr_atom_t*   atoms;\n"
        "    uint8_t                 param_atoms[%s_SIG_MAX_PARAMS];  /* atom bitmask per param */\n"
        "    /* Slots the spec types alike, params then results: same group, same class.\n"
        "     * A POLY result in a group an input also belongs to takes that input's\n"
        "     * class — which is how select's three §3.4.1 `t` slots stay one type. -1\n"
        "     * for a slot that is not polymorphic. */\n"
        "    int8_t                  poly_group[%s_SIG_MAX_PARAMS + %s_SIG_MAX_RESULTS];\n"
        "    int8_t                  variadic;   /* param index of the variadic group, -1 if none */\n"
        "    uint8_t                 present;\n"
        "} %s_opcode_sig_t;\n\n",
        prefix_.c_str(), prefix_.c_str(), uprefix_.c_str(),
        uprefix_.c_str(), uprefix_.c_str(), prefix_.c_str());

    for (auto& oi : ops_) {
        if (oi.atoms.empty()) continue;
        fprintf(o, "static const %s_addr_atom_t %s_atoms_%s[] = {",
                prefix_.c_str(), prefix_.c_str(), oi.op->mnemonic);
        for (auto& a : oi.atoms)
            fprintf(o, " {\"%s\", \"%s\", %d},", a.pred.c_str(), a.ident.c_str(), a.operand);
        fputs(" };\n", o);
    }

    auto row = [&](const OpInfo& oi) {
        fprintf(o, "{ %d, %zu, ", sig_ids_.find(oi.decl)->second, oi.atoms.size());
        if (oi.atoms.empty()) fputs("NULL, {", o);
        else fprintf(o, "%s_atoms_%s, {", prefix_.c_str(), oi.op->mnemonic);
        for (unsigned m : oi.param_atoms) fprintf(o, " %u,", m);
        fputs(" }, {", o);
        for (int g : oi.poly_group) fprintf(o, " %d,", g);
        fprintf(o, " }, %d, 1 },\n", oi.variadic_param);
    };

    int prefixes[8]; int npfx = 0;
    for (auto& oi : ops_) {
        if (oi.op->subop < 0) continue;
        int seen = 0;
        for (int p = 0; p < npfx; p++) if (prefixes[p] == oi.op->opcode_val) seen = 1;
        if (!seen && npfx < 8) prefixes[npfx++] = oi.op->opcode_val;
    }
    for (int p = 0; p < npfx; p++) {
        int pv = prefixes[p], maxsub = 0;
        for (auto& oi : ops_)
            if (oi.op->subop > maxsub && oi.op->opcode_val == pv) maxsub = oi.op->subop;
        fprintf(o, "\nstatic const %s_opcode_sig_t %s_opcode_sig_sub_%02x[%d] = {\n",
                prefix_.c_str(), prefix_.c_str(), pv, maxsub + 1);
        for (auto& oi : ops_) {
            if (oi.op->subop < 0 || oi.op->opcode_val != pv) continue;
            fprintf(o, "    [%d] = ", oi.op->subop); row(oi);
        }
        fputs("};\n", o);
    }
    fprintf(o, "\nstatic const %s_opcode_sig_t %s_opcode_sig[256] = {\n",
            prefix_.c_str(), prefix_.c_str());
    for (auto& oi : ops_) {
        if (oi.op->subop >= 0) continue;
        fprintf(o, "    [0x%02x] = ", oi.op->opcode_val); row(oi);
    }
    fputs("};\n", o);
    fprintf(o, "\nstatic const %s_opcode_sig_t* const %s_opcode_sig_sub[256] = {\n",
            prefix_.c_str(), prefix_.c_str());
    for (int p = 0; p < npfx; p++)
        fprintf(o, "    [0x%02x] = %s_opcode_sig_sub_%02x,\n",
                prefixes[p], prefix_.c_str(), prefixes[p]);
    fputs("};\n", o);
    // A sub-table is exactly as long as its highest subopcode; walking it needs
    // the length, not a sentinel — an unfilled slot inside the range is zeroed
    // and `present` tells it apart, but past the end there is nothing to read.
    fprintf(o, "\nstatic const uint16_t %s_opcode_sig_sub_len[256] = {\n", prefix_.c_str());
    for (int p = 0; p < npfx; p++) {
        int pv = prefixes[p], maxsub = 0;
        for (auto& oi : ops_)
            if (oi.op->subop > maxsub && oi.op->opcode_val == pv) maxsub = oi.op->subop;
        fprintf(o, "    [0x%02x] = %d,\n", pv, maxsub + 1);
    }
    fputs("};\n", o);

    // §3.3's canonical-state leaf, per register class.
    fprintf(o,
        "\n/* The value a previous region left on the memory stack, per storage class:\n"
        " * the leaf the next region's first consumer takes as its child. */\n"
        "static const uint16_t %s_carried_sig[%u] = {", prefix_.c_str(), SCLASS_FINAL - 1);
    for (int id : carried_ids_) fprintf(o, " %d,", id);
    fputs(" };\n", o);

    // valtype -> storage class, in the one place that owns the class vocabulary.
    // A consumer projecting a module's types into the tree builder's view reads
    // it here rather than writing the switch again.
    fprintf(o,
        "\n/* The class a §7.6 value type is HELD in. WVT_BOT has none — it is the\n"
        " * validator's \"any type\" and no register answers it. */\n"
        "static inline uint8_t %s_sclass_of_valtype(%s_valtype_t t) {\n"
        "    switch (t) {\n"
        "    case WVT_I32:  return JSC_I32;\n"
        "    case WVT_I64:  return JSC_I64;\n"
        "    case WVT_F32:  return JSC_F32;\n"
        "    case WVT_F64:  return JSC_F64;\n"
        "    case WVT_V128: return JSC_V128;\n"
        "    case WVT_REF: case WVT_REF_NN: return JSC_REF;\n"
        "    default:       return JSC_COUNT;\n"
        "    }\n}\n", prefix_.c_str(), prefix_.c_str());

    // The census the pin reads back, so the numbers live where they are derived.
    size_t nmulti = 0;
    for (int id : final_ids_) if (sigs_[id].results.size() > 1) nmulti++;
    fprintf(o,
        "\n/* The census this vocabulary is pinned on. */\n"
        "#define %s_SIG_OPCODES       %zu\n"
        "#define %s_SIG_TERMINALS     %zu\n"
        "#define %s_SIG_MULTI_RESULT  %zu\n"
        "#define %s_SIG_MAX_ARITY     %d\n",
        uprefix_.c_str(), ops_.size(), uprefix_.c_str(), final_ids_.size(),
        uprefix_.c_str(), nmulti, uprefix_.c_str(), maxkids);
    fprintf(o, "\n#endif /* %s_SIGTAB_H */\n", uprefix_.c_str());
    fprintf(stderr, "opgen: %zu opcodes, %zu signatures (%zu terminals), "
                    "max arity %d, %zu multi-result\n",
            ops_.size(), sigs_.size(), final_ids_.size(), maxkids, nmulti);
}

// ── <prefix>_ttree.asdl ─────────────────────────────────────

// One sum type per RESULT class, one constructor per terminal, one `node` field
// per tree child — the AST-shaped form burgc's -asdl coverage needs. A slot-based
// schema (children as indices) makes the node-field count disagree with the burg
// arity, and burgc then falls back to its heuristic filter: the coverage gate
// stays green while checking nothing.
void SigEmitter::emit_ttree_asdl(FILE* o) {
    fprintf(o, "// AUTO-GENERATED by opgen — do not edit.\n"
               "// The tier-2 tree IR: a node per storage-class signature, typed by the\n"
               "// class it PRODUCES, with one field per operand that is a tree child.\n\n"
               "module %s_ttree {\n", prefix_.c_str());
    std::vector<std::vector<int>> by_result((size_t)SCLASS_FINAL + 1);
    const size_t kVoid = SCLASS_FINAL;
    for (int id : final_ids_) {
        const Sig& s = sigs_[id];
        by_result[s.results.empty() ? kVoid : (size_t)s.results[0]].push_back(id);
    }
    for (size_t r = 0; r <= kVoid; r++) {
        if (by_result[r].empty()) continue;
        fprintf(o, "\n    %s =", r == kVoid ? "eff" : kClassName[r]);
        bool first = true;
        for (int id : by_result[r]) {
            const Sig& s = sigs_[id];
            fprintf(o, "%s %s", first ? "" : "\n            |", s.name().c_str());
            first = false;
            if (s.nkids() == 0) continue;
            fputc('(', o);
            int k = 0;
            for (size_t i = 0; i < s.params.size(); i++) {
                if (s.params[i] == SClass::Stk) continue;
                fprintf(o, "%s%s a%d", k ? ", " : "", cname(s.params[i]), k);
                k++;
            }
            fputc(')', o);
        }
        fputc('\n', o);
    }
    fputs("}\n", o);
}

// ── <prefix>_tile.burg ──────────────────────────────────────

// The terminals, and the rules for the one location assignment that exists
// before the variant family does: every operand and every result on the memory
// stack. That is what the JIT emits today, so the grammar is complete by
// construction, and the cached assignments are added on top of it.
void SigEmitter::emit_tile_burg(FILE* o) {
    fprintf(o, "// AUTO-GENERATED by opgen — do not edit.\n"
               "// Tier-2 tiling: terminals are storage-class signatures, nonterminals are\n"
               "// cache states (storage class x location).\n\n"
               "(. #include \"%s_ttree.h\" .)\n\n", prefix_.c_str());
    for (int id : final_ids_)
        fprintf(o, "TERM %s = %d\n", sigs_[id].name().c_str(), id);

    fputs("\nSTART stmt\n\nRULES\n\n"
          "// A region root: whatever the region computed, left where the next region\n"
          "// can find it — the memory stack.\n", o);
    for (unsigned c = 0; c < SCLASS_FINAL; c++) {
        bool used = false;
        for (int id : final_ids_)
            if (!sigs_[id].results.empty() && (unsigned)sigs_[id].results[0] == c) used = true;
        if (used) fprintf(o, "stmt: %s_mem = 0;\n", kClassName[c]);
    }
    fputs("\n// A value the previous region left behind: already where it needs to be,\n"
          "// so the leaf costs nothing to \"produce\".\n", o);
    for (int id : final_ids_)
        if (sigs_[id].kind == SigKind::Carried)
            fprintf(o, "%s_mem: %s = 0;\n", cname(sigs_[id].results[0]),
                    sigs_[id].name().c_str());

    fputs("\n// One rule per terminal, all operands and the result on the memory stack.\n", o);
    for (int id : final_ids_) {
        const Sig& s = sigs_[id];
        if (s.kind == SigKind::Carried) continue;
        const char* lhs = s.results.empty() ? "stmt" : nullptr;
        std::string lhs_s = lhs ? lhs : (std::string(cname(s.results[0])) + "_mem");
        fprintf(o, "%s: %s", lhs_s.c_str(), s.name().c_str());
        if (s.nkids()) {
            fputc('(', o);
            int k = 0;
            for (size_t i = 0; i < s.params.size(); i++) {
                if (s.params[i] == SClass::Stk) continue;
                fprintf(o, "%s%s_mem", k ? ", " : "", cname(s.params[i]));
                k++;
            }
            fputc(')', o);
        }
        fputs(" = 1;\n", o);
    }
}

} // namespace opgen
