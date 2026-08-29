// The VC emitter: closed opcode bodies lowered to per-obligation SMT-LIB2.
//
// The term builder mirrors what SemLowerer's C emission actually computes —
// C integer promotion at every operator, the THREADED result type for shift
// masks and `>>>`/int_min widths, conversion to the declared type at every
// assignment — because the obligations are about the code the other emitters
// generate, not about an idealized reading of the body. Where the mirror and
// the C could drift, the seeded-liar fixtures and the reference-kernel
// differential leg are the check on the mirror itself.
#include "vcemit.h"
#include "refemit.h"
#include "Parser.h"
#include <cinttypes>
#include <cstring>
#include <string>
#include <vector>

namespace opgen {

template <class T>
static T* opt_ptr(const std::optional<T*>& o) { return o ? *o : nullptr; }

namespace {

// ── Terms ───────────────────────────────────────────────────
//
// A term carries its SMT text, its C-typing width in bits, and its C-typing
// signedness — the two facts C's usual-arithmetic-conversions read.

struct Term {
    std::string s;
    int  w   = 32;
    bool sgn = true;
};

std::string bvlit(uint64_t v, int w) {
    uint64_t mask = (w >= 64) ? ~0ULL : ((1ULL << w) - 1);
    char buf[48];
    snprintf(buf, sizeof buf, "(_ bv%" PRIu64 " %d)", v & mask, w);
    return buf;
}

Term ext(const Term& t, int W) {
    if (t.w == W) return t;
    char buf[32];
    if (t.w < W) {
        snprintf(buf, sizeof buf, "((_ %s %d) ",
                 t.sgn ? "sign_extend" : "zero_extend", W - t.w);
        return Term{std::string(buf) + t.s + ")", W, t.sgn};
    }
    snprintf(buf, sizeof buf, "((_ extract %d 0) ", W - 1);
    return Term{std::string(buf) + t.s + ")", W, t.sgn};
}

// C conversion to a (width, signedness) type: widen by the SOURCE's
// signedness (that is what C value-converts), then adopt the target's.
Term cvt(const Term& t, int W, bool sgn) {
    Term r = ext(t, W);
    r.sgn = sgn;
    return r;
}

// C integer promotion: anything narrower than int becomes int (32, signed —
// int represents every s1/u1/s2/u2 value).
Term promote(const Term& t) {
    if (t.w >= 32) return t;
    return cvt(t, 32, true);
}

// C usual arithmetic conversions over the two promoted operands.
void usual(Term& l, Term& r) {
    l = promote(l);
    r = promote(r);
    int W = l.w > r.w ? l.w : r.w;
    bool sgn;
    if (l.w == r.w) sgn = l.sgn && r.sgn;
    else            sgn = (l.w > r.w) ? l.sgn : r.sgn;   // s8 absorbs u4; u8 absorbs s4
    l = cvt(l, W, sgn);
    r = cvt(r, W, sgn);
}

std::string tobool(const Term& t) {
    return "(not (= " + t.s + " " + bvlit(0, t.w) + "))";
}

Term boolterm(const std::string& b) {
    return Term{"(ite " + b + " " + bvlit(1, 32) + " " + bvlit(0, 32) + ")", 32, true};
}

// The intrinsics the translation models. The rest (clz/ctz/popcnt/rotl/rotr/
// reinterpret) classify the op UNVERIFIABLE — metered, never silently wrong.
bool modeled_call(const char* n) { return !strcmp(n, "int_min"); }

// ── The mirror walker + symbolic executor ───────────────────

class VcLower {
public:
    VcLower(const SemLowerer& low, const Opcode* op) : low_(low), op_(op) {
        for (auto* p : op->stack_in)  declare(p->name, p->ty);
        for (auto* p : op->operands)  declare(p->name, p->ty);
        for (auto* p : op->stack_out) {
            if (find(p->name)) continue;   // pass-through out: already a param
            int w; bool s; scalar_of(p->ty, &w, &s);
            env_.push_back({p->name, Term{bvlit(0, w), w, s}, false, w, s, false});
        }
    }

    // Every symbolic input, as `(declare-const <name> (_ BitVec W))` lines.
    std::string decls() const {
        std::string out;
        for (const auto& b : env_)
            if (b.is_input)
                out += "(declare-const " + b.name + " (_ BitVec " +
                       std::to_string(b.declw) + "))\n";
        return out;
    }

    const std::vector<std::string>& undefs() const { return undef_; }

    // Execute the closed body; false (with *err set) on a def bug.
    bool exec_body(std::string* err) {
        for (auto* s : op_->sem_body)
            if (!exec_stmt(s, err)) return false;
        for (auto* p : op_->stack_out) {
            const Binding* b = find(p->name);
            if (b && !b->assigned) {
                *err = std::string("output '") + p->name +
                       "' is not assigned on every path";
                return false;
            }
        }
        return true;
    }

    // Translate one expression in the CURRENT environment, mirroring
    // lower_expr's result-type threading.
    Term xexpr(const SemExpr* e, ValueType rt) {
        switch (e->tag) {
        case SemExprTag::SInt: {
            int64_t v = static_cast<const SInt*>(e)->value;
            int w = (v > 2147483647LL || v < -2147483648LL) ? 64 : 32;
            return Term{bvlit((uint64_t)v, w), w, true};
        }
        case SemExprTag::SIdent: {
            const Binding* b = find(static_cast<const SIdent*>(e)->name);
            return b ? b->t : Term{bvlit(0, 32), 32, true};
        }
        case SemExprTag::SCall: {
            // int_min(): the most negative value of rt — mirrors the
            // lower_intrinsic emission ((T)((uT)1 << (W-1))).
            int W = low_.scalar_bits(low_.c_scalar(rt));
            return Term{bvlit(1ULL << (W - 1), W), W, true};
        }
        case SemExprTag::SUnary: {
            auto* u = static_cast<const SUnary*>(e);
            Term t = xexpr(u->operand, rt);
            if (!strcmp(u->op, "!")) return boolterm("(not " + tobool(t) + ")");
            Term p = promote(t);
            const char* fn = !strcmp(u->op, "-") ? "bvneg" : "bvnot";
            return Term{std::string("(") + fn + " " + p.s + ")", p.w, p.sgn};
        }
        case SemExprTag::STernary: {
            auto* tn = static_cast<const STernary*>(e);
            Term c = xexpr(tn->cond, rt);
            Term l = xexpr(tn->then_, rt);
            Term r = xexpr(tn->else_, rt);
            usual(l, r);
            return Term{"(ite " + tobool(c) + " " + l.s + " " + r.s + ")", l.w, l.sgn};
        }
        case SemExprTag::SCast: {
            auto* ca = static_cast<const SCast*>(e);
            if (!strcmp(ca->ty, "unsigned") || !strcmp(ca->ty, "signed")) {
                ValueType ot = rt;
                if (ca->operand->tag == SemExprTag::SIdent)
                    ot = low_.type_of_name(op_, static_cast<const SIdent*>(ca->operand)->name);
                Term t = xexpr(ca->operand, ot);
                int W = low_.scalar_bits(low_.c_scalar(ot));
                return cvt(t, W, ca->ty[0] == 's');
            }
            const char* ct = SemLowerer::java_c(ca->ty);
            Term t = xexpr(ca->operand, rt);
            return cvt(t, low_.scalar_bits(ct), ct[0] != 'u');
        }
        case SemExprTag::SBinOp:
            return xbinop(static_cast<const SBinOp*>(e), rt);
        default:
            // SIndex/SFloat cannot reach a closed integer body (eligibility
            // filtered them); a zero keeps the type sound if one ever does.
            return Term{bvlit(0, 32), 32, true};
        }
    }

    // Execute one statement (closed subset), mirroring lower_stmt's typing.
    bool exec_stmt(const SemStmt* s, std::string* err) {
        switch (s->tag) {
        case SemStmtTag::SAssign: {
            auto* a = static_cast<const SAssign*>(s);
            const char* name = static_cast<const SIdent*>(a->target)->name;
            ValueType rt = low_.type_of_name(op_, name);
            Term v = xexpr(a->value, rt);
            Binding* b = find(name);
            if (!b) {
                *err = std::string("assignment to undeclared name '") + name + "'";
                return false;
            }
            b->t = cvt(v, b->declw, b->declsgn);
            b->assigned = true;
            return true;
        }
        case SemStmtTag::SLocalDecl: {
            auto* d = static_cast<const SLocalDecl*>(s);
            const char* ct = SemLowerer::java_c(d->ty);
            ValueType ivt = ValueType::TyI32;
            if (!strcmp(d->ty, "long") || !strcmp(d->ty, "ulong")) ivt = ValueType::TyI64;
            int W = low_.scalar_bits(ct);
            bool sg = ct[0] != 'u';
            Term t = d->init.has_value() ? xexpr(*d->init, ivt) : Term{bvlit(0, W), W, sg};
            Binding* b = find(d->name);
            if (b) { b->t = cvt(t, W, sg); b->assigned = true; }
            else   env_.push_back({d->name, cvt(t, W, sg), true, W, sg, false});
            return true;
        }
        case SemStmtTag::SIf: {
            auto* i = static_cast<const SIf*>(s);
            Term c = xexpr(i->cond, ValueType::TyI32);
            std::string cb = tobool(c);
            std::vector<Binding> saved = env_;
            if (!exec_stmt(i->then_, err)) return false;
            std::vector<Binding> then_env = env_;
            env_ = saved;
            if (i->else_.has_value() && !exec_stmt(*i->else_, err)) return false;
            // Merge: a binding differing between the arms becomes an ite; it
            // is definitely-assigned only when BOTH arms assigned it (or it
            // already was). Branch-local declarations die with their arm.
            for (auto& b : env_) {
                const Binding* tb = nullptr;
                for (const auto& t : then_env)
                    if (t.name == b.name) { tb = &t; break; }
                if (!tb) continue;
                if (tb->t.s != b.t.s)
                    b.t = Term{"(ite " + cb + " " + tb->t.s + " " + b.t.s + ")",
                               b.declw, b.declsgn};
                b.assigned = b.assigned && tb->assigned;
            }
            return true;
        }
        case SemStmtTag::SBlock: {
            for (auto* st : static_cast<const SBlock*>(s)->stmts)
                if (!exec_stmt(st, err)) return false;
            return true;
        }
        default:
            *err = "statement outside the closed subset";
            return false;
        }
    }

private:
    struct Binding {
        std::string name;
        Term t;
        bool assigned;
        int  declw;
        bool declsgn;
        bool is_input;
    };

    void scalar_of(ValueType ty, int* w, bool* sgn) const {
        const char* sc = low_.c_scalar(ty);
        *w = low_.scalar_bits(sc);
        *sgn = sc[0] != 'u';
    }

    void declare(const char* name, ValueType ty) {
        int w; bool s; scalar_of(ty, &w, &s);
        env_.push_back({name, Term{name, w, s}, true, w, s, true});
    }

    Binding* find(const char* name) {
        for (auto& b : env_) if (b.name == name) return &b;
        return nullptr;
    }
    const Binding* find(const char* name) const {
        for (const auto& b : env_) if (b.name == name) return &b;
        return nullptr;
    }

    Term xbinop(const SBinOp* b, ValueType rt) {
        const char* bop = b->op;
        if (!strcmp(bop, "&&") || !strcmp(bop, "||")) {
            Term l = xexpr(b->left, rt), r = xexpr(b->right, rt);
            const char* fn = bop[0] == '&' ? "and" : "or";
            return boolterm(std::string("(") + fn + " " + tobool(l) + " " + tobool(r) + ")");
        }
        if (!strcmp(bop, "==") || !strcmp(bop, "!=") ||
            !strcmp(bop, "<") || !strcmp(bop, "<=") ||
            !strcmp(bop, ">") || !strcmp(bop, ">=")) {
            Term l = xexpr(b->left, rt), r = xexpr(b->right, rt);
            usual(l, r);
            std::string c;
            if      (!strcmp(bop, "==")) c = "(= " + l.s + " " + r.s + ")";
            else if (!strcmp(bop, "!=")) c = "(not (= " + l.s + " " + r.s + "))";
            else {
                const char* fn =
                    !strcmp(bop, "<")  ? (l.sgn ? "bvslt" : "bvult") :
                    !strcmp(bop, "<=") ? (l.sgn ? "bvsle" : "bvule") :
                    !strcmp(bop, ">")  ? (l.sgn ? "bvsgt" : "bvugt") :
                                         (l.sgn ? "bvsge" : "bvuge");
                c = std::string("(") + fn + " " + l.s + " " + r.s + ")";
            }
            return boolterm(c);
        }
        // Shifts mask their count with the THREADED type's width — exactly
        // the `& (W-1)` the C emission writes (lower_expr's shm).
        int shm = low_.scalar_bits(low_.c_scalar(rt)) - 1;
        if (!strcmp(bop, ">>>")) {
            // ((uW)left) >> (count & shm): the value converts to rt's
            // unsigned type, so the shift is logical at rt's width.
            int W = shm + 1;
            Term l = cvt(xexpr(b->left, rt), W, false);
            Term lp = promote(l);
            Term cnt = promote(xexpr(b->right, rt));
            cnt = cvt(cnt, lp.w, cnt.sgn);
            std::string masked = "(bvand " + cnt.s + " " + bvlit((uint64_t)shm, lp.w) + ")";
            return Term{"(bvlshr " + lp.s + " " + masked + ")", lp.w, lp.sgn};
        }
        if (!strcmp(bop, "<<") || !strcmp(bop, ">>")) {
            Term l = promote(xexpr(b->left, rt));
            Term cnt = promote(xexpr(b->right, rt));
            cnt = cvt(cnt, l.w, cnt.sgn);
            std::string masked = "(bvand " + cnt.s + " " + bvlit((uint64_t)shm, l.w) + ")";
            const char* fn = bop[0] == '<' ? "bvshl" : (l.sgn ? "bvashr" : "bvlshr");
            return Term{std::string("(") + fn + " " + l.s + " " + masked + ")", l.w, l.sgn};
        }
        Term l = xexpr(b->left, rt), r = xexpr(b->right, rt);
        usual(l, r);
        if (!strcmp(bop, "/") || !strcmp(bop, "%")) {
            // The C-defined domain: divisor nonzero, and for SIGNED division
            // not (min / -1). Collected as an undefined-condition; the
            // totality obligation asserts their disjunction under the guards.
            std::string u = "(= " + r.s + " " + bvlit(0, r.w) + ")";
            if (l.sgn)
                u = "(or " + u + " (and (= " + l.s + " " + bvlit(1ULL << (l.w - 1), l.w) +
                    ") (= " + r.s + " " + bvlit(~0ULL, r.w) + ")))";
            undef_.push_back(u);
            const char* fn = bop[0] == '/' ? (l.sgn ? "bvsdiv" : "bvudiv")
                                           : (l.sgn ? "bvsrem" : "bvurem");
            return Term{std::string("(") + fn + " " + l.s + " " + r.s + ")", l.w, l.sgn};
        }
        const char* fn =
            !strcmp(bop, "+") ? "bvadd" : !strcmp(bop, "-") ? "bvsub" :
            !strcmp(bop, "*") ? "bvmul" : !strcmp(bop, "&") ? "bvand" :
            !strcmp(bop, "|") ? "bvor"  : "bvxor";
        return Term{std::string("(") + fn + " " + l.s + " " + r.s + ")", l.w, l.sgn};
    }

    const SemLowerer& low_;
    const Opcode*     op_;
    std::vector<Binding>     env_;
    std::vector<std::string> undef_;
};

// ── VC-layer eligibility beyond RefEmitter's (loops, intrinsics) ────────────

bool expr_calls_unmodeled(const SemExpr* e) {
    if (!e) return false;
    switch (e->tag) {
    case SemExprTag::SCall: {
        auto* c = static_cast<const SCall*>(e);
        if (!modeled_call(c->name)) return true;
        for (auto* a : c->args) if (expr_calls_unmodeled(a)) return true;
        return false;
    }
    case SemExprTag::SBinOp: {
        auto* b = static_cast<const SBinOp*>(e);
        return expr_calls_unmodeled(b->left) || expr_calls_unmodeled(b->right);
    }
    case SemExprTag::SUnary:
        return expr_calls_unmodeled(static_cast<const SUnary*>(e)->operand);
    case SemExprTag::STernary: {
        auto* t = static_cast<const STernary*>(e);
        return expr_calls_unmodeled(t->cond) || expr_calls_unmodeled(t->then_) ||
               expr_calls_unmodeled(t->else_);
    }
    case SemExprTag::SCast:
        return expr_calls_unmodeled(static_cast<const SCast*>(e)->operand);
    default: return false;
    }
}

// Returns "loop in body", "intrinsic not modeled", or nullptr.
const char* vc_open_reason(const SemStmt* s) {
    if (!s) return nullptr;
    switch (s->tag) {
    case SemStmtTag::SWhile:
    case SemStmtTag::SFor:
        return "loop in body";
    case SemStmtTag::SAssign: {
        auto* a = static_cast<const SAssign*>(s);
        return expr_calls_unmodeled(a->value) ? "intrinsic not modeled" : nullptr;
    }
    case SemStmtTag::SLocalDecl: {
        auto* d = static_cast<const SLocalDecl*>(s);
        return (d->init.has_value() && expr_calls_unmodeled(*d->init))
                   ? "intrinsic not modeled" : nullptr;
    }
    case SemStmtTag::SIf: {
        auto* i = static_cast<const SIf*>(s);
        if (expr_calls_unmodeled(i->cond)) return "intrinsic not modeled";
        if (const char* r = vc_open_reason(i->then_)) return r;
        return i->else_.has_value() ? vc_open_reason(*i->else_) : nullptr;
    }
    case SemStmtTag::SBlock: {
        for (auto* st : static_cast<const SBlock*>(s)->stmts)
            if (const char* r = vc_open_reason(st)) return r;
        return nullptr;
    }
    default: return nullptr;
    }
}

// Parse an `edge:` condition string (quotes preserved by the frontend) with
// the ONE grammar, by wrapping it as a guard on a probe opcode — no second
// expression parser to drift from opgen.peg.
SemExpr* parse_cond_string(const char* quoted) {
    std::string t(quoted);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        t = t.substr(1, t.size() - 2);
    std::string src = "_vc_probe 0x00 ( -- )\n    error: (. " + t + " .) -> X\n";
    Parser p;
    p.init(src.data(), (int)src.size());
    if (!p.parse() || !p.ast || p.ast->opcodes.empty()) return nullptr;
    auto* o = p.ast->opcodes[0];
    if (o->errors.empty() || !o->errors[0]->condition.has_value()) return nullptr;
    return *o->errors[0]->condition;
}

} // namespace

// ── The emitter ─────────────────────────────────────────────

int VcEmitter::emit_all(const char* out_dir) {
    std::string mpath = std::string(out_dir) + "/" + prefix_ + "_vc_manifest.txt";
    FILE* mf = fopen(mpath.c_str(), "w");
    if (!mf) { perror(mpath.c_str()); return 1; }

    fprintf(mf,
"# opgen VC manifest — %s. One row per opcode classification.\n"
"# OB <id> <mnemonic> <kind> <expect:unsat|sat> <file>\n"
"# UNVERIFIABLE <mnemonic> <reason>\n"
"# CLOSED <mnemonic>   (closed body, no obligations to discharge)\n",
            prefix_.c_str());

    SemLowerer low(mod_, nullptr, SemLowerer::Mode::Interp, prefix_);
    int rc = 0, seq = 0;

    for (auto* op : mod_->opcodes) {
        if (const char* r = RefEmitter::open_reason(low, op)) {
            fprintf(mf, "UNVERIFIABLE %s %s\n", op->mnemonic, r);
            continue;
        }
        const char* vr = nullptr;
        for (auto* s : op->sem_body)
            if ((vr = vc_open_reason(s))) break;
        if (vr) {
            fprintf(mf, "UNVERIFIABLE %s %s\n", op->mnemonic, vr);
            continue;
        }

        // Guards translate in the INITIAL environment (they run before the
        // body); then the body executes symbolically, collecting the partial-
        // operator side conditions along the way.
        VcLower vl(low, op);
        std::vector<std::string> guards;
        for (auto* e : op->errors) {
            if (!e->condition.has_value()) continue;
            Term g = vl.xexpr(*e->condition, SemLowerer::cond_type(op, *e->condition));
            guards.push_back(tobool(g));
        }
        std::string err;
        if (!vl.exec_body(&err)) {
            fprintf(stderr, "opgen: %s: %s\n", op->mnemonic, err.c_str());
            rc = 1;
            continue;
        }

        int emitted = 0;
        auto obfile = [&](const char* kind, const char* expect,
                          const std::string& body_smt) {
            char id[32];
            snprintf(id, sizeof id, "%s_%04d", prefix_.c_str(), seq++);
            std::string fname = std::string(id) + "_" + op->mnemonic + "_" + kind + ".smt2";
            std::string path = std::string(out_dir) + "/" + fname;
            FILE* f = fopen(path.c_str(), "w");
            if (!f) { perror(path.c_str()); rc = 1; return; }
            fprintf(f, "(set-logic QF_BV)\n; %s %s — generated by opgen\n%s%s"
                       "(check-sat)\n(get-model)\n",
                    op->mnemonic, kind, vl.decls().c_str(), body_smt.c_str());
            fclose(f);
            fprintf(mf, "OB %s %s %s %s %s\n", id, op->mnemonic, kind, expect,
                    fname.c_str());
            emitted++;
        };

        // totality: guards false, yet some partial operator leaves its
        // C-defined domain. (Guard evaluation order and `&&`/`||` short-
        // circuit protect earlier-guarded states; assuming the full guard
        // conjunction mirrors that protection.)
        if (!vl.undefs().empty()) {
            std::string b;
            for (const auto& g : guards) b += "(assert (not " + g + "))\n";
            b += "(assert (or";
            for (const auto& u : vl.undefs()) b += " " + u;
            b += "))\n";
            obfile("totality", "unsat", b);
        }

        // guard-sat: each guard can fire under the guards before it.
        for (size_t i = 0; i < guards.size(); i++) {
            std::string b;
            for (size_t j = 0; j < i; j++) b += "(assert (not " + guards[j] + "))\n";
            b += "(assert " + guards[i] + ")\n";
            obfile("guard-sat", "sat", b);
        }

        // edge: guards false ∧ P (over inputs) ∧ ¬Q (over outputs).
        for (auto* ec : op->edges) {
            SemExpr* P = parse_cond_string(ec->condition);
            SemExpr* Q = parse_cond_string(ec->assignment);
            if (!P || !Q) {
                fprintf(stderr, "opgen: %s: edge condition does not parse: %s -> %s\n",
                        op->mnemonic, ec->condition, ec->assignment);
                rc = 1;
                continue;
            }
            // P reads the input values: translate in a fresh (initial)
            // environment; Q reads the outputs: translate in the final one.
            VcLower init(low, op);
            Term pt = init.xexpr(P, SemLowerer::cond_type(op, P));
            Term qt = vl.xexpr(Q, SemLowerer::cond_type(op, Q));
            std::string b;
            for (const auto& g : guards) b += "(assert (not " + g + "))\n";
            b += "(assert " + tobool(pt) + ")\n";
            b += "(assert (not " + tobool(qt) + "))\n";
            obfile("edge", "unsat", b);
        }

        if (!emitted) fprintf(mf, "CLOSED %s\n", op->mnemonic);
    }

    fclose(mf);
    return rc;
}

} // namespace opgen
