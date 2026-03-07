// calc.cpp — Proof-of-life: JIT-compile and execute "slots[2] = slots[0] + slots[1]"

#include "calc_stencil_table.h"
#include "../runtime/jitterator.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>

// ── copy_and_patch runtime ─────────────────────────────────────
//
// Emits a stencil into the code buffer with a constant pool appended
// for data holes (RIP-relative loads). Returns the offset of the
// stencil's code in the buffer.
//
// values[i] corresponds to hole_index i. Interpretation depends on
// patch type:
//   PATCH_REL_BRANCH: value is a code address (patched later via backpatch)
//   PATCH_REL_DATA:   value is a uint64_t stored in the constant pool

static size_t emit_stencil(jitterator::CodeBuffer &buf,
                           const StencilDef &stencil,
                           const uint64_t *values)
{
    size_t code_base = buf.size();

    // Copy code bytes
    buf.emit_bytes(stencil.code, stencil.code_size);

    // Allocate constant pool for data holes (8 bytes each, right after code)
    // We need to map each data hole_index to its pool offset.
    // Pool layout: one uint64_t per unique data hole, in hole_index order.
    size_t pool_base = buf.size();
    uint8_t zeros[8] = {};
    // Track which hole indices are data holes and their pool slot
    int16_t pool_slot[64] = {};  // hole_index → pool slot (-1 if not data)
    memset(pool_slot, -1, sizeof(pool_slot));
    uint16_t next_slot = 0;

    for (uint32_t i = 0; i < stencil.patch_count; i++) {
        auto &p = stencil.patches[i];
        if (p.type == PATCH_REL_DATA && pool_slot[p.hole_index] < 0) {
            pool_slot[p.hole_index] = next_slot++;
            buf.emit_bytes(zeros, 8);  // reserve space
        }
    }

    // Write values into constant pool entries
    for (uint16_t hi = 0; hi < stencil.hole_count; hi++) {
        if (pool_slot[hi] >= 0) {
            size_t entry_off = pool_base + pool_slot[hi] * 8;
            uint64_t val = values ? values[hi] : 0;
            buf.patch_64(entry_off, val);
        }
    }

    // Patch relocations
    for (uint32_t i = 0; i < stencil.patch_count; i++) {
        auto &p = stencil.patches[i];
        size_t patch_addr = code_base + p.offset;

        switch (p.type) {
        case PATCH_REL_BRANCH: {
            // Branch target: value is an absolute code address.
            // Displacement = target - (absolute_patch_addr + 4)
            // If target is 0, leave as placeholder for backpatching.
            uint64_t target = values ? values[p.hole_index] : 0;
            if (target != 0) {
                uintptr_t patch_abs = (uintptr_t)buf.data() + patch_addr;
                int32_t disp = (int32_t)(target - (patch_abs + 4));
                buf.patch_32(patch_addr, disp);
            }
            break;
        }
        case PATCH_REL_DATA: {
            // Data load: displacement points to constant pool entry
            size_t entry_off = pool_base + pool_slot[p.hole_index] * 8;
            int32_t disp = (int32_t)(entry_off - (patch_addr + 4));
            buf.patch_32(patch_addr, disp);
            break;
        }
        case PATCH_ABS64: {
            uint64_t val = values ? values[p.hole_index] : 0;
            buf.patch_64(patch_addr, val);
            break;
        }
        case PATCH_ABS32S: {
            uint64_t val = values ? values[p.hole_index] : 0;
            buf.patch_32(patch_addr, (int32_t)val);
            break;
        }
        }
    }

    return code_base;
}

// Backpatch a specific hole in an already-emitted stencil
static void backpatch(jitterator::CodeBuffer &buf,
                      size_t stencil_base,
                      const StencilDef &stencil,
                      uint16_t hole_index,
                      uint64_t target)
{
    for (uint32_t i = 0; i < stencil.patch_count; i++) {
        auto &p = stencil.patches[i];
        if (p.hole_index != hole_index) continue;
        if (p.type != PATCH_REL_BRANCH) continue;

        size_t patch_addr = stencil_base + p.offset;
        uintptr_t patch_abs = (uintptr_t)buf.data() + patch_addr;
        int32_t disp = (int32_t)(target - (patch_abs + 4));
        buf.patch_32(patch_addr, disp);
    }
}

// Find hole index by name
static uint16_t find_hole(const StencilDef &stencil, const char *name) {
    for (uint16_t i = 0; i < stencil.hole_count; i++) {
        if (strcmp(stencil.hole_names[i], name) == 0) return i;
    }
    fprintf(stderr, "Hole not found: %s\n", name);
    abort();
}

// ── Main: compile and run slots[2] = slots[0] + slots[1] ──────

int main() {
    jitterator::CodeBuffer buf;

    // Look up hole indices
    uint16_t entry_cont = find_hole(stencil_table[STENCIL_ENTRY], "_HOLE_cont");
    uint16_t add_lhs    = find_hole(stencil_table[STENCIL_ADD], "_HOLE_lhs");
    uint16_t add_rhs    = find_hole(stencil_table[STENCIL_ADD], "_HOLE_rhs");
    uint16_t add_dst    = find_hole(stencil_table[STENCIL_ADD], "_HOLE_dst");
    uint16_t add_cont   = find_hole(stencil_table[STENCIL_ADD], "_HOLE_cont");

    // Pass 1: emit stencils with data holes filled, branch holes as 0
    uint64_t entry_vals[1] = {0};  // cont = backpatch later
    size_t entry_off = emit_stencil(buf, stencil_table[STENCIL_ENTRY], entry_vals);

    uint64_t add_vals[4] = {};
    add_vals[add_lhs] = 0;  // slot 0
    add_vals[add_rhs] = 1;  // slot 1
    add_vals[add_dst] = 2;  // slot 2
    // add_vals[add_cont] = 0;  // backpatch later
    size_t add_off = emit_stencil(buf, stencil_table[STENCIL_ADD], add_vals);

    size_t halt_off = emit_stencil(buf, stencil_table[STENCIL_HALT], nullptr);

    // Pass 2: backpatch continuations
    // entry → add code (not constant pool — the code starts at add_off)
    uintptr_t base = (uintptr_t)buf.data();
    backpatch(buf, entry_off, stencil_table[STENCIL_ENTRY],
              entry_cont, base + add_off);
    backpatch(buf, add_off, stencil_table[STENCIL_ADD],
              add_cont, base + halt_off);

    // Finalize
    auto *fn = (void(*)(int64_t*))buf.finalize();

    // Run: 10 + 20 = ?
    int64_t slots[3] = {10, 20, 0};
    fn(slots);

    printf("slots[0]=%ld, slots[1]=%ld, slots[2]=%ld\n",
           slots[0], slots[1], slots[2]);
    assert(slots[2] == 30);
    printf("OK: 10 + 20 = %ld\n", slots[2]);

    // Run again: 100 + -50 = ?
    slots[0] = 100;
    slots[1] = -50;
    slots[2] = 0;

    // Need to re-finalize since we called finalize already? No — buf was
    // finalized, fn pointer is still valid.
    fn(slots);
    printf("slots[0]=%ld, slots[1]=%ld, slots[2]=%ld\n",
           slots[0], slots[1], slots[2]);
    assert(slots[2] == 50);
    printf("OK: 100 + (-50) = %ld\n", slots[2]);

    return 0;
}
