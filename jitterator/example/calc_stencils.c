// calc_stencils.c — Minimal calculator stencils for jitterator proof-of-life

#include <stdint.h>

// Hole declarations
extern void __attribute__((preserve_none)) _HOLE_cont(int64_t *slots);
extern uint64_t _HOLE_dst;
extern uint64_t _HOLE_lhs;
extern uint64_t _HOLE_rhs;
extern uint64_t _HOLE_value;

// add: slots[dst] = slots[lhs] + slots[rhs]
void __attribute__((preserve_none))
add(int64_t *slots) {
    slots[_HOLE_dst] = slots[_HOLE_lhs] + slots[_HOLE_rhs];
    return _HOLE_cont(slots);
}

// sub: slots[dst] = slots[lhs] - slots[rhs]
void __attribute__((preserve_none))
sub(int64_t *slots) {
    slots[_HOLE_dst] = slots[_HOLE_lhs] - slots[_HOLE_rhs];
    return _HOLE_cont(slots);
}

// load_const: slots[dst] = value
void __attribute__((preserve_none))
load_const(int64_t *slots) {
    slots[_HOLE_dst] = (int64_t)_HOLE_value;
    return _HOLE_cont(slots);
}

// exit: return to caller
void __attribute__((preserve_none))
halt(int64_t *slots) {
    return;
}

// entry: transition from C calling convention to preserve_none
void entry(int64_t *slots) {
    return ((void __attribute__((preserve_none))(*)(int64_t*))_HOLE_cont)(slots);
}
