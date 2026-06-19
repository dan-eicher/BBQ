#pragma once
//
// CompilerCtx — the compiler context: the single home for the shared, accumulated
// state the backend phases consume. Before this, bbqc threaded four loose locals
// (the AST, Sema, the compiled IR, a name mapper) into every render call; this
// consolidates them so a phase takes one `const CompilerCtx&` and pulls what it
// needs — and never recomputes what an earlier phase already resolved.
//
// Three sources live here at once, each read for what it holds cleanly:
//   - ast  : declarative SHAPE (struct fields/order, unions, inline structs).
//            ddcg discards shape when it lowers AST->IR, so consume the AST
//            directly for layout — do not reconstruct it from the IR.
//   - sema : RESOLVED FACTS (types, cross-rule relationships, endianness).
//            The home for Sema's attached side-data as it grows.
//   - ir   : the executable kont graph (BEHAVIOR) — the reader/writer source.
//            Null until compile_grammar runs; the context accumulates.
//
// Language-neutral on purpose: the per-language name mapper (CTypeMapper for C,
// the C++ mapper for C++) is a backend detail, not part of the context. This is
// the model the C++ backend copies. NB: `bbq_ctx_t` is the unrelated RUNTIME
// reader cursor, not this.
//
#include "BBQ_AST.h"
#include "Sema.h"
#include "Names.h"

#include <string>

// The kont IR (the BEHAVIOR half of the contract) is referenced here only as an
// opaque pointer, so the shared context — and the SHAPE consumers that include it
// (RenderTypes / the C++ type+parser generators) — never pull in the CEK machine
// headers. Only the reader consumes the IR; default_le() lives in CompilerCtx.cpp
// next to that include. This keeps the tool structurally decoupled from the CEK.
namespace bbq::cek { struct CompiledGrammar; }

namespace bbq::render {

struct CompilerCtx {
    BBQ::Grammar*              ast  = nullptr;  // declarative shape
    bbqgen::Sema*              sema = nullptr;  // resolved facts (+ attached side-data)
    bbq::cek::CompiledGrammar* ir  = nullptr;  // executable kont IR (set after compile)
    std::string               prefix;          // optional type/name prefix

    // Snake-cased name prefix applied to every rule's type/function names.
    std::string type_prefix() const {
        return prefix.empty() ? "" : bbqgen::to_snake_case(prefix);
    }
    // Grammar default endianness (for Native-endian primitives). Defined in
    // CompilerCtx.cpp because it dereferences the kont IR — keep it out of the
    // header so shape-only consumers don't need the CEK machine definition.
    bool default_le() const;
};

}  // namespace bbq::render
