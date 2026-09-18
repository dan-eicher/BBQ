/*
 * calc_vm_host.c — the one translation unit that embeds the calc VM.
 *
 * Everything an opgen VM needs from its host, in one place: the dispatcher and
 * trap sink that gen_interp.c declares and tail-calls, the reset that puts a VM
 * back to a known state, and the two tiers. See calc_vm_host.h for why this is C.
 */
#include "calc_host.h"          /* the seam: calc_next, calc_trap, calc_reset, tier 0 */
#include "calc_jit_driver.h"    /* tier 1: stamping jitterator's stencils */
#include "calc_vm_host.h"

s4 calc_run_interp(vm_t* vm, const u1* code, size_t len) {
    return calc_interp_run(vm, code, len);
}

s4 calc_run_jit(vm_t* vm, const u1* code, size_t len) {
    return calc_jit_run(vm, code, len);
}

void calc_set_sidetable(const opgen_st_entry_t* st) {
    calc_g_sidetable = st;
}
