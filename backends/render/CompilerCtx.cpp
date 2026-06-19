#include "CompilerCtx.h"
#include "CompiledGrammar.h"  // bbq::cek::CompiledGrammar — the kont IR (behavior)

namespace bbq::render {

// Dereferences the kont IR, so it lives here (with the CEK include) rather than
// in the header — only the reader links this TU; the shape generators don't.
bool CompilerCtx::default_le() const {
    return ir ? ir->default_little_endian
              : (sema && sema->default_endian() != BBQ::Endianness::Big);
}

}  // namespace bbq::render
