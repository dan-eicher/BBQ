// calc_stencils.c — Minimal calculator stencils for jitterator proof-of-life

#include <stdint.h>

#define STENCIL __attribute__((preserve_none))
#define TAIL    __attribute__((musttail))

// Hole declarations
extern void STENCIL _HOLE_cont(int64_t *slots);
extern uint64_t _HOLE_dst;
extern uint64_t _HOLE_lhs;
extern uint64_t _HOLE_rhs;
extern uint64_t _HOLE_value;

// add: slots[dst] = slots[lhs] + slots[rhs]
void STENCIL add(int64_t *slots) {
    slots[_HOLE_dst] = slots[_HOLE_lhs] + slots[_HOLE_rhs];
    TAIL return _HOLE_cont(slots);
}

// sub: slots[dst] = slots[lhs] - slots[rhs]
void STENCIL sub(int64_t *slots) {
    slots[_HOLE_dst] = slots[_HOLE_lhs] - slots[_HOLE_rhs];
    TAIL return _HOLE_cont(slots);
}

// load_const: slots[dst] = value
void STENCIL load_const(int64_t *slots) {
    slots[_HOLE_dst] = (int64_t)_HOLE_value;
    TAIL return _HOLE_cont(slots);
}

// mul: slots[dst] = slots[lhs] * slots[rhs]
void STENCIL mul(int64_t *slots) {
    slots[_HOLE_dst] = slots[_HOLE_lhs] * slots[_HOLE_rhs];
    TAIL return _HOLE_cont(slots);
}

// neg: slots[dst] = -slots[src]
extern uint64_t _HOLE_src;
void STENCIL neg(int64_t *slots) {
    slots[_HOLE_dst] = -slots[_HOLE_src];
    TAIL return _HOLE_cont(slots);
}

// halt: return to caller
void STENCIL halt(int64_t *slots) {
    return;
}

// entry: transition from C calling convention to preserve_none
// No TAIL here — crossing calling conventions requires a call, not a tail call.
void entry(int64_t *slots) {
    return ((void STENCIL (*)(int64_t*))_HOLE_cont)(slots);
}
