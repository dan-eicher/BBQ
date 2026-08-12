#ifndef OPGEN_SIGEMIT_H
#define OPGEN_SIGEMIT_H

#include "opgen_ast.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace opgen {

// The STORAGE-CLASS signature vocabulary: what each stack slot of each opcode
// needs to be held in once it leaves the type-agnostic operand slot and lives in
// a register. The operand stack is one uniform slot per value, so an in-memory
// value has no class to speak of; a register-resident one must pick one.
//
// Three of the classes are not final. An ADDR slot's width is the addrtype the
// addressed memory/table DECLARES (spec §2.3.11), a POLY slot's class is the
// type of the local/global/field/element/operand the instruction names, and
// neither is knowable from the opcode alone. Resolving them is what turns a
// declared signature into the burg terminals it can become.
enum class SClass : unsigned char {
    I32 = 0, I64, F32, F64, V128, Ref,
    Stk,        // the variadic operand group: one signature slot, no tree child
    Addr,       // non-final: i32 | i64, from the addressed entry's addrtype
    Poly,       // non-final: the declared local/global/field/element/operand type
    Count
};
constexpr unsigned SCLASS_FINAL = 7;   // I32..Stk need no resolution

// Can a value of this class ride a cache register? A managed reference cannot:
// it belongs on the operand stack, where the collector looks for roots and,
// because the collector moves objects, where it rewrites them — a register has
// no address to rewrite. v128 needs two slots and does not have them yet.
//
// THE answer, for the variant family and for anything that reasons about cache
// states. Two readings of it put rules in a grammar for registers no stencil
// ever fills.
// `ref` is the only class that never enters a slot: a managed reference belongs
// on the operand stack, where the collector looks for roots and, because it
// evacuates, where it rewrites them. A register has no address to rewrite.
//
// v128 DOES, across two slots — measured at 111 -> 47 bytes, and better than a
// native vector-class slot at 51, because the stencils are built
// -fno-vectorize/-fno-slp-vectorize so the lane arithmetic is already in GPRs.
// It is why a cache state counts SLOTS and not items.
inline bool sclass_cacheable(SClass c) {
    return c == SClass::I32 || c == SClass::I64 ||
           c == SClass::F32 || c == SClass::F64 ||
           c == SClass::V128;
}

// Slots a class occupies. Two for a v128, one for everything else.
inline int sclass_width(SClass c) { return c == SClass::V128 ? 2 : 1; }

// The spelling a class contributes to a transition stencil's symbol, or null
// when it does not cache. One place, so the emitted stencil and the table that
// names it cannot drift.
inline const char* sclass_stencil_name(SClass c) {
    switch (c) {
    case SClass::I32: return "I32";
    case SClass::I64: return "I64";
    case SClass::F32: return "F32";
    case SClass::F64: return "F64";
    case SClass::V128: return "V128";
    default:          return nullptr;
    }
}

// What a node stands for. Almost every node is an instruction, but a region does
// not begin with an empty stack: whatever the previous region left behind is
// sitting on the memory stack, and the first instruction that consumes one needs
// a child to consume. That leaf is a node with no instruction behind it, and it
// cannot borrow `()->(i32)` from i32.const — the stitcher would stamp a constant.
enum class SigKind : unsigned char {
    Op = 0,      // an opcode's storage-class signature
    Carried,     // a value carried across a cut, already in the canonical state
};

// One signature — a parameter class vector and a result class vector. Final
// signatures (no Addr/Poly slot) are the burg terminals; the rest exist only as
// the declared form an opcode carries and the resolution list it expands to.
struct Sig {
    SigKind kind = SigKind::Op;
    std::vector<SClass> params, results;
    bool operator<(const Sig& o) const {
        if (kind != o.kind) return kind < o.kind;
        if (params != o.params) return params < o.params;
        return results < o.results;
    }
    bool is_final() const;
    int  nkids() const;          // burg arity: the params that ARE tree children
    std::string name() const;    // "Sig_i32_i32_to_i32" — TERM + ASDL constructor
};

// Emits the tier-2 signature vocabulary: the signature table + opcode map
// (jav_sigtab.h), the tree IR schema burgc reads for coverage
// (jav_ttree.asdl), and the tiling grammar's terminals and memory-form rules
// (jav_tile.burg). One walk over Module.opcodes feeds all three, so they cannot
// disagree about an opcode's arity unless the spec changes under all of them.
class SigEmitter {
public:
    SigEmitter(const Module* mod, std::string prefix);

    void emit_sigtab_h(FILE* o);     // <prefix>_sigtab.h
    void emit_ttree_asdl(FILE* o);   // <prefix>_ttree.asdl
    // The tiling grammar is NOT emitted here. A rule's cost is the size of the
    // code it stamps, and that is measured downstream of this tool — so the
    // consumer generates the grammar from the stencil table it ends up with.

    // The class a value type is HELD in. Public because it is the mapping, not an
    // implementation detail: anything asking "can this cache" or "which
    // transition stencil" has to reach a class the same way this walk does.
    static SClass classify_final(ValueType ty, const char* mnemonic, const char* param);

private:
    // One addrtype ATOM: a module entry whose declared addrtype an ADDR slot
    // reads, named by the predicate the spec wrote, the identifier it indexes
    // by, and the operand that identifier is decoded from. The two can differ:
    // a memarg binds `memidx` out of its own flag bit rather than carrying an
    // operand of its own (vmemit.cpp:389).
    struct Atom { std::string pred, ident; int operand; };

    // Everything one opcode contributes: its declared signature, the atoms its
    // ADDR slots read, and the per-slot atom masks (a slot is i64 when EVERY
    // atom it names is — memory.copy's length operand reads both memories).
    struct OpInfo {
        const Opcode* op = nullptr;
        Sig           decl;
        std::vector<Atom> atoms;
        std::vector<unsigned> param_atoms;   // per param: bitmask of atoms, 0 if not ADDR
        std::vector<int>  poly_group;        // per slot (params then results): tie group, -1 if not POLY
        int               npoly_groups = 0;
        int               variadic_param = -1;   // param index of the variadic group, or -1
        std::vector<int>  resolved;          // signature ids this opcode can become
    };

    const Module* mod_;
    std::string   prefix_, uprefix_;

    std::vector<Sig>   sigs_;        // id -> signature
    std::map<Sig, int> sig_ids_;
    std::vector<OpInfo> ops_;
    std::vector<int>    final_ids_;    // the burg terminal set, in id order
    std::vector<int>    carried_ids_;  // class -> its canonical-state leaf

    void build();
    int  intern(const Sig& s);
    OpInfo describe(const Opcode* op) const;
    void resolve(OpInfo& oi);
    static void collect_atoms(const SemExpr* e, std::vector<Atom>& out,
                              const Opcode* op);
    static unsigned atom_mask(const SemExpr* e, const std::vector<Atom>& atoms,
                              const Opcode* op);
    void put_guard(FILE* o, const char* what) const;
};

} // namespace opgen

#endif
