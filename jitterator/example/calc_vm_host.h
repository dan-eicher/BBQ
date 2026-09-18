/*
 * calc_vm_host.h — running calc bytecode, from either language.
 *
 * The VM host is one C translation unit (calc_vm_host.c): it fills opgen's seam
 * (the dispatcher and the trap sink), owns the reset, and drives both tiers. It
 * has to be C, and only C, because opgen's generated calc_jit_symbols.h takes the
 * address of libm functions as `(void*)ceil` — unambiguous in C, and in C++ an
 * overload set with no target type to resolve against. That is why the JIT driver
 * lived inside the C test for as long as it did: nothing C++ could include it.
 *
 * So the C++ leg links this instead of including it, and both tiers are reachable
 * from both test languages.
 */
#ifndef CALC_VM_HOST_H
#define CALC_VM_HOST_H

#include "runtime_api.h"     /* vm_t, s4, u1 */

#ifdef __cplusplus
extern "C" {
#endif

/* Tier 0 — the opgen-generated threaded interpreter. */
s4 calc_run_interp(vm_t* vm, const u1* code, size_t len);

/* Tier 1 — jitterator's stencils, stamped into a buffer and called. */
s4 calc_run_jit(vm_t* vm, const u1* code, size_t len);

/* The side table a br_table program branches through. opgen emits the entry TYPE
 * and the walk over it; filling the entries is a validator's job, which the calc
 * does not have — a caller that needs one supplies it. */
void calc_set_sidetable(const opgen_st_entry_t* st);

#ifdef __cplusplus
}
#endif

#endif /* CALC_VM_HOST_H */
