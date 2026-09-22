# Jitterator — Copy-and-Patch stencil extractor

# The ELF64 reader is generated from extract/elf64.bbq by bbqc's render C++
# backend — the cpp-zcow (view) parser: builds a zero-copy capture index, read
# through header-only handles. Regenerated whenever the grammar or templates
# change, so it can't go stale.
set(ELF64_TYPES  ${CMAKE_CURRENT_BINARY_DIR}/elf64Types.h)
set(ELF64_READER ${CMAKE_CURRENT_BINARY_DIR}/elf64Reader.h)
add_custom_command(
    OUTPUT ${ELF64_TYPES} ${ELF64_READER}
    COMMAND bbqc -backend cpp
                 -prefix elf64 -namespace elf
                 -templates ${CMAKE_SOURCE_DIR}/backends/render/templates
                 -o ${CMAKE_CURRENT_BINARY_DIR}
                 ${CMAKE_CURRENT_SOURCE_DIR}/extract/elf64.bbq
    DEPENDS bbqc
            ${CMAKE_CURRENT_SOURCE_DIR}/extract/elf64.bbq
            ${CMAKE_SOURCE_DIR}/backends/render/templates/types_cpp.inja
            ${CMAKE_SOURCE_DIR}/backends/render/templates/reader_view_cpp.inja
    COMMENT "bbqc: elf64.bbq -> elf64Types.h + elf64Reader.h (cpp-zcow view parser)"
    VERBATIM
)

add_executable(jitterator
    src/jitterator.cpp
    ${CMAKE_SOURCE_DIR}/backends/cpp/runtime/CaptureDecode.cpp  # shared offset->value decoders
    ${ELF64_TYPES} ${ELF64_READER}
)
# A C++ parser consumer depends ONLY on the neutral index runtime — never on the
# CEK machine (backends/cek). That this builds without backends/cek is the proof.
target_include_directories(jitterator PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}                 # generated elf64Types.h / elf64Parser.h
    ${CMAKE_SOURCE_DIR}/backends/cpp/runtime     # bbq_node.h + the index runtime
)

# --- jitterator's own tests ---
# The extractor reads object files, so its tests build real ones with clang and run
# the real binary: what is under test is exactly how clang's output is read. Until
# these existed it was covered only through the calc VM end to end — one object,
# built one way, with two relocation types in it.
find_program(CLANG_FOR_TESTS NAMES clang)
if(CLANG_FOR_TESTS)
    add_executable(jitterator_tests test/jitterator_test.cpp)
    target_link_libraries(jitterator_tests PRIVATE GTest::GTest GTest::Main)
    target_compile_definitions(jitterator_tests PRIVATE
        JITTERATOR_BIN="$<TARGET_FILE:jitterator>"
        CLANG_C="${CLANG_FOR_TESTS}")
    add_dependencies(jitterator_tests jitterator)
    add_test(NAME jitterator_tests COMMAND jitterator_tests)
    set_tests_properties(jitterator_tests PROPERTIES TIMEOUT 300)
endif()

# The code buffer's own tests. No clang and no extractor needed — the buffer is
# header-only C and these stamp bytes by hand, which is the point: the two-phase
# contract (emit, seal, then patch displacements) is what every driver rests on
# and it had nothing checking it.
add_executable(codebuf_tests test/codebuf_test.cpp)
target_include_directories(codebuf_tests PRIVATE runtime)
target_link_libraries(codebuf_tests PRIVATE GTest::GTest GTest::Main)
add_test(NAME codebuf_tests COMMAND codebuf_tests)

# --- Calculator (full toolchain demo: pegc + asdl + ddcg + burgc + opgen) ---
# The calc lowers parsed programs through pegc → ddcg → CPS/ANF IR →
# burgc (cg_jump) → opgen-generated two-tier VM. clang is needed for the
# generated VM (preserve_none + musttail); find it once here.
find_program(CLANG_C_COMPILER NAMES clang)
if(NOT CLANG_C_COMPILER)
    message(FATAL_ERROR "clang required for the opgen-way calc VM "
                        "(preserve_none + musttail attributes are clang-specific)")
endif()

# ASDL JSON sidecars (consumed by ddcgc + burgc) + asdl-generated
# C++ headers (CalcAST.h, CalcIR.h). The headers are checked into
# source so the calc frontend can include them directly; CMake regens
# them whenever the .asdl changes.
set(CALC_IR_JSON ${CMAKE_CURRENT_BINARY_DIR}/calc_ir.json)
set(CALC_IR_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/CalcIR.h)
add_custom_command(
    OUTPUT ${CALC_IR_JSON} ${CALC_IR_HEADER}
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc_ir.asdl
                 --json ${CALC_IR_JSON}
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc_ir.asdl
                 -t ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/templates/calc_ast.inja
                 -o ${CALC_IR_HEADER}
    DEPENDS asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc_ir.asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/templates/calc_ast.inja
    COMMENT "asdl: calc_ir.asdl -> calc_ir.json + CalcIR.h"
    VERBATIM
)
set(CALC_AST_JSON ${CMAKE_CURRENT_BINARY_DIR}/calc.json)
set(CALC_AST_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/CalcAST.h)
add_custom_command(
    OUTPUT ${CALC_AST_JSON} ${CALC_AST_HEADER}
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.asdl
                 --json ${CALC_AST_JSON}
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.asdl
                 -t ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/templates/calc_ast.inja
                 -o ${CALC_AST_HEADER}
    DEPENDS asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/templates/calc_ast.inja
    COMMENT "asdl: calc.asdl -> calc.json + CalcAST.h"
    VERBATIM
)

# ddcgc generates calc_compile.h from calc.ddcg + the two ASDL JSONs.
# Replaces the hand-written `Compiler` class that used to live in
# example/compiler.h. Output is `class calc::Compiler` with inline
# method bodies.
set(CALC_COMPILE_H ${CMAKE_CURRENT_BINARY_DIR}/calc_compile.h)
set(DDCGC_LIB_DIR ${CMAKE_SOURCE_DIR}/ddcgc/lib)
add_custom_command(
    OUTPUT ${CALC_COMPILE_H}
    COMMAND ddcgc -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.ddcg
                  -ast ${CALC_AST_JSON}
                  -ir  ${CALC_IR_JSON}
                  -L   ${DDCGC_LIB_DIR}
                  -o   ${CALC_COMPILE_H}
    DEPENDS ddcgc
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.ddcg
            ${DDCGC_LIB_DIR}/dybvig.ddcg
            ${CALC_AST_JSON}
            ${CALC_IR_JSON}
    COMMENT "Running ddcgc on calc.ddcg"
    VERBATIM
)

# End-to-end pipeline check: run burgc completeness analysis on
# calc.burg with the ASDL JSON. Demonstrates the asdl→burgc flow.
# calc is slot-based, so the schema falls back to heuristic for
# multi-arity terminals; we don't pass --strict here for that
# reason. AST-shaped IRs (yoctojc/sir.asdl) get precise filtering.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/CalcMatcher.h
    COMMAND burgc -i ${CMAKE_CURRENT_SOURCE_DIR}/example/calc.burg
                  -asdl ${CALC_IR_JSON}
                  -frames ${CMAKE_SOURCE_DIR}/burgc/frames
                  -o ${CMAKE_CURRENT_BINARY_DIR}/CalcMatcher.h
    DEPENDS burgc ${CMAKE_CURRENT_SOURCE_DIR}/example/calc.burg ${CALC_IR_JSON}
    COMMENT "burgc: generating CalcMatcher.h from calc.burg"
    VERBATIM
)

# --- Auto-regenerate Parser.h/.cpp from calc.peg ---
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/Parser.h
           ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/Parser.cpp
    COMMAND pegc ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.peg
                 -frames ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/frames/
                 -o ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/
                 -prefix ""
    DEPENDS pegc
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.peg
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/frames/Parser.frame
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/frames/ParserImpl.frame
    COMMENT "Generating calc Parser.h/.cpp from calc.peg"
    VERBATIM
)

# opgen generates the calc VM from calc.def. This must precede calc_runner:
# calc_codegen.h (which the BURG matcher drives) includes the generated
# opcodes.h for its OP_* constants, so calc.def is the single declaration of
# the opcode bytes — the rule actions and the VM cannot disagree.
set(CALC_VM_DIR ${CMAKE_CURRENT_BINARY_DIR}/calc_vm)

# Ertl's stack-cache size for the calc's JIT tier. opgen emits the variant family
# for exactly this number and the driver tiles for it, so they cannot differ — a
# driver tiling for a state whose stencil was never generated selects nothing.
# Changing it regenerates the VM, which is why it is a cache variable rather than
# something a test passes: `cmake -B build -DCALC_TIER2_N=0` builds the tier-1-only
# engine, and the sweep test in example/test runs several sizes in one go.
#
# The calc is this repository's integration test, so the default is NOT 0: a
# default of tier-1-only would mean the variant family, the spill/fill tables and
# the tiling that uses them were generated and then never executed here.
set(CALC_TIER2_N "4" CACHE STRING "calc JIT stack-cache size (0 = tier-1 only)")
set(CALC_TIER2_STAMP ${CALC_VM_DIR}/tier2_n.stamp)
add_custom_command(
    OUTPUT ${CALC_TIER2_STAMP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CALC_VM_DIR}
    COMMAND ${CMAKE_COMMAND} -E echo ${CALC_TIER2_N} > ${CALC_TIER2_STAMP}
    COMMENT "calc: stack-cache size ${CALC_TIER2_N}"
    VERBATIM
)
set(CALC_VM_GEN
    ${CALC_VM_DIR}/gen_interp.c   ${CALC_VM_DIR}/gen_interp.h
    ${CALC_VM_DIR}/runtime_api.h  ${CALC_VM_DIR}/opcodes.h
    ${CALC_VM_DIR}/opcode_tables.c ${CALC_VM_DIR}/calc_stencils.c
    ${CALC_VM_DIR}/calc_jit_meta.h ${CALC_VM_DIR}/calc_jit_symbols.h
    ${CALC_VM_DIR}/calc_valtype.h  ${CALC_VM_DIR}/calc_type_meta.h
    ${CALC_VM_DIR}/calc_sigtab.h)
add_custom_command(
    OUTPUT ${CALC_VM_GEN}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CALC_VM_DIR}
    COMMAND opgen -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.def -prefix calc
                  -tier2 ${CALC_TIER2_N} -o ${CALC_VM_DIR}
    DEPENDS opgen
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.def
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_backend.h
            ${CALC_TIER2_STAMP}
    COMMENT "opgen: calc.def -> two-tier calc VM (stack cache ${CALC_TIER2_N})"
    VERBATIM
)

# calc_runner — shared API for both the calc_jit CLI and the test
# binary. Encapsulates the parse → compile → JIT → execute pipeline so
# callers don't have to deal with the JIT context, slot tracker, or
# BURG matcher directly.
# The rewrite pass: burgc --rewrite turns calc.burg's REWRITE section into a
# saturating matcher over the e-graph. Generated like every other artifact
# here — the example shows the tool being run, not a checked-in output.
# Emitted as a header and included by calc_eqsat.h, the same way CalcMatcher.h
# is included by calc_runner.cpp: the rewriter names the consumer's terminal
# ids and calls its guards, so it compiles inside a translation unit that has
# already declared them. Its own only #include is egraph.h.
set(CALC_REWRITE_H ${CMAKE_CURRENT_BINARY_DIR}/calc_rewrite.h)
add_custom_command(
    OUTPUT ${CALC_REWRITE_H}
    COMMAND $<TARGET_FILE:burgc> -i ${CMAKE_CURRENT_SOURCE_DIR}/example/calc.burg
            --rewrite -name calc -o ${CALC_REWRITE_H}
    DEPENDS burgc ${CMAKE_CURRENT_SOURCE_DIR}/example/calc.burg
    COMMENT "burgc --rewrite: calc.burg REWRITE section -> calc_rewrite.h"
    VERBATIM
)

add_library(calc_runner STATIC
    example/calc_runner.cpp
    example/frontend/Parser.cpp
    ${CALC_REWRITE_H}
    ${CMAKE_SOURCE_DIR}/burgc/runtime/egraph.c
    ${CMAKE_SOURCE_DIR}/crt/bbq_hmap.c
    ${CMAKE_SOURCE_DIR}/crt/bbq_vec.c
    ${CMAKE_SOURCE_DIR}/crt/bbq_alloc.c
    ${CALC_AST_HEADER}
    ${CALC_IR_HEADER}
    ${CALC_COMPILE_H}
    ${CMAKE_CURRENT_BINARY_DIR}/CalcMatcher.h
    ${CALC_VM_DIR}/opcodes.h
)
# The rewrite pass. It works because Figure 8 case 1 leaves a simple binop's
# operands inlined: the e-graph sees `Mul(LoadLocal, LoadConst)` and a rule can
# read the constant off the node. Under the old uniform-spill lowering both
# operands were LoadLocals and no axiom could fire.
target_compile_definitions(calc_runner PUBLIC CALC_EQSAT)
target_include_directories(calc_runner PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/example
    ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_SOURCE_DIR}/pegc/runtime
    ${CMAKE_SOURCE_DIR}/opgen/runtime                  # bbq_emit.h (generic emitter)
    ${CMAKE_CURRENT_BINARY_DIR}                        # calc_compile.h, CalcMatcher.h
    ${CALC_VM_DIR}                                     # opcodes.h (OP_* from calc.def)
    ${CMAKE_SOURCE_DIR}/backends/c/runtime             # bbq_runtime.h, via opcodes.h
    ${CMAKE_SOURCE_DIR}/crt                            # bbq_read.h, via bbq_runtime.h
    ${CMAKE_SOURCE_DIR}/burgc/runtime                  # egraph.h (the rewrite pass)
)

find_package(GTest REQUIRED)

# ──────────────────────────────────────────────────────────────────
# opgen-way calc: a real two-tier VM (threaded interpreter + copy-and-
# patch JIT) generated by opgen from one calc.def — "WASM lite". Real
# parsed calc programs run the whole toolchain end to end:
#   pegc → ddcg → CPS/ANF IR → cg_jump lowering → opgen 2-tier VM.
# Both tiers run the SAME generated opcode bodies, so interp == JIT by
# construction. clang is required (preserve_none + musttail).
# ──────────────────────────────────────────────────────────────────
find_program(CLANG_CXX_COMPILER NAMES clang++)
if(NOT CLANG_CXX_COMPILER)
    message(FATAL_ERROR "clang++ required for the opgen-way calc VM (musttail)")
endif()

# CALC_VM_DIR / CALC_VM_GEN and the opgen custom command are declared above
# calc_runner, which needs the generated opcodes.h.

# Shared include flags: the substrate (calc_backend.h), the bbq_ctx
# cursor (bbq_runtime.h), the generated VM headers, the JIT runtime,
# the calc frontend (calc_runner.h / CalcIR.h) and the VM host
# (calc_vm_host.h / calc_host.h / calc_jit_driver.h).
# These binaries are linked by raw clang commands, so the sanitizer flags the
# rest of the build gets from add_link_options do not reach them — and half their
# objects (calc_runner, the host) DO carry the instrumentation, which is what the
# missing __ubsan_* references at link were. The stencils stay uninstrumented on
# purpose: their bytes are copied into JIT memory, and instrumented code cannot be
# relocated by a copy-and-patch patcher.
set(CALC_VM_LINK_SAN "")
if(BBQ_SANITIZE)
    set(CALC_VM_LINK_SAN -fsanitize=address,undefined)
endif()

set(CALC_VM_INCS
    -I${CALC_VM_DIR}
    -I${CMAKE_CURRENT_SOURCE_DIR}/example
    -I${CMAKE_CURRENT_SOURCE_DIR}/example/frontend
    -I${CMAKE_SOURCE_DIR}/backends/c/runtime
    -I${CMAKE_SOURCE_DIR}/crt                          # bbq_vec.h — the reader's growable parse stacks
    -I${CMAKE_SOURCE_DIR}/opgen/runtime
    -I${CMAKE_CURRENT_SOURCE_DIR}/runtime
    -I${CMAKE_SOURCE_DIR}/pegc/runtime
    -I${CMAKE_CURRENT_BINARY_DIR})

# The calc VM compiles against BBQ's C runtime headers and embeds bbq_ctx_t BY
# VALUE (calc_backend.h: `bbq_ctx_t code` — the bytecode cursor IS a bbq cursor),
# so a field added to that struct changes this object's layout. These objects are
# built by custom commands, which track only what DEPENDS lists: without the
# headers here a runtime change leaves a stale object that links fine against
# freshly compiled translation units and segfaults at run time.
file(GLOB CALC_VM_RUNTIME_HDRS
     ${CMAKE_SOURCE_DIR}/backends/c/runtime/*.h
     ${CMAKE_SOURCE_DIR}/crt/*.h
     ${CMAKE_SOURCE_DIR}/opgen/runtime/*.h
     ${CMAKE_SOURCE_DIR}/pegc/runtime/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.h)

# Tier 0 — the threaded interpreter object (clang: musttail).
set(CALC_INTERP_O ${CALC_VM_DIR}/gen_interp.o)
add_custom_command(
    OUTPUT ${CALC_INTERP_O}
    COMMAND ${CLANG_C_COMPILER} -std=c11 -O2 ${CALC_VM_INCS}
            -c ${CALC_VM_DIR}/gen_interp.c -o ${CALC_INTERP_O}
    DEPENDS ${CALC_VM_GEN} ${CALC_VM_RUNTIME_HDRS}
    COMMENT "clang: calc interpreter (gen_interp.c)"
    VERBATIM
)

# The opcode name table is C — compile it as C once and link the object, so
# the C++ test links below don't compile a .c input in C++ mode (clang 19
# warns -Wdeprecated for that, and the C99 designators inside warn too).
set(CALC_OPTAB_O ${CALC_VM_DIR}/opcode_tables.o)
add_custom_command(
    OUTPUT ${CALC_OPTAB_O}
    COMMAND ${CLANG_C_COMPILER} -std=c11 -O2 ${CALC_VM_INCS}
            -c ${CALC_VM_DIR}/opcode_tables.c -o ${CALC_OPTAB_O}
    DEPENDS ${CALC_VM_GEN} ${CALC_VM_RUNTIME_HDRS}
    COMMENT "clang: calc opcode tables (opcode_tables.c)"
    VERBATIM
)

# Tier 1 — the copy-and-patch stencils → jitterator → stencil table.
set(CALC_VM_STENCILS_O ${CALC_VM_DIR}/calc_stencils.o)
add_custom_command(
    OUTPUT ${CALC_VM_STENCILS_O}
    # -fno-pic -mcmodel=small is not a preference, it is the stencil contract: PIC
    # turns `&_HOLE_x` into a GOT-relative load, and a copied stencil has no GOT;
    # the large model turns it into a 64-bit PC-relative field the patcher cannot
    # write. jitterator refuses either, so this says it rather than relying on the
    # toolchain's default (many distributions default to PIE).
    COMMAND ${CLANG_C_COMPILER} -std=c11 -O2 -fno-stack-protector
            -fno-pic -mcmodel=small
            -fno-asynchronous-unwind-tables ${CALC_VM_INCS}
            -c ${CALC_VM_DIR}/calc_stencils.c -o ${CALC_VM_STENCILS_O}
    DEPENDS ${CALC_VM_GEN} ${CALC_VM_RUNTIME_HDRS}
    COMMENT "clang: calc VM stencils (calc_stencils.c)"
    VERBATIM
)
set(CALC_VM_STENCIL_TABLE ${CALC_VM_DIR}/calc_stencil_table.h)
add_custom_command(
    OUTPUT ${CALC_VM_STENCIL_TABLE}
    COMMAND jitterator ${CALC_VM_STENCILS_O} -o ${CALC_VM_STENCIL_TABLE}
    DEPENDS jitterator ${CALC_VM_STENCILS_O}
    COMMENT "jitterator: calc VM stencils -> calc_stencil_table.h"
    VERBATIM
)

# The VM host — the embedder's side of opgen's seam (the dispatcher and the trap
# sink gen_interp.c tail-calls), the reset, and both tier drivers. Compiled as C
# because opgen's calc_jit_symbols.h takes `(void*)ceil`, which C++ reads as an
# unresolved overload set; linking it is what lets the C++ e2e leg reach the JIT
# at all. Every test links this, so the seam is written once instead of copied
# into each of them — which is how the two copies had come to differ.
set(CALC_VM_HOST_O ${CALC_VM_DIR}/calc_vm_host.o)
add_custom_command(
    OUTPUT ${CALC_VM_HOST_O}
    COMMAND ${CLANG_C_COMPILER} -std=c11 -O2 ${CALC_VM_INCS}
            -c ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_vm_host.c -o ${CALC_VM_HOST_O}
    DEPENDS ${CALC_VM_GEN} ${CALC_VM_RUNTIME_HDRS} ${CALC_VM_STENCIL_TABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_vm_host.c
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_vm_host.h
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_host.h
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_jit_driver.h
    COMMENT "clang: calc VM host (opgen seam + both tiers)"
    VERBATIM
)

# Tier 0 e2e — real parsed programs (calc_compile) → cg_jump lowering
# → the generated interpreter.
set(CALC_VM_E2E ${CALC_VM_DIR}/calc_vm_e2e)
add_custom_command(
    OUTPUT ${CALC_VM_E2E}
    COMMAND ${CLANG_CXX_COMPILER} -std=c++17 -DCALC_EQSAT ${CALC_VM_LINK_SAN} ${CALC_VM_INCS}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_e2e_test.cpp
            $<TARGET_FILE:calc_runner> ${CALC_INTERP_O}
            ${CALC_OPTAB_O} ${CALC_VM_HOST_O} -lm -o ${CALC_VM_E2E}
    DEPENDS ${CALC_INTERP_O} ${CALC_OPTAB_O} calc_runner ${CALC_REWRITE_H} ${CALC_VM_RUNTIME_HDRS}
            ${CALC_VM_HOST_O}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_e2e_test.cpp
    COMMENT "clang: calc VM e2e (parse -> ddcg -> IR -> bytecode -> interp AND jit)"
    VERBATIM
)
add_custom_target(calc_vm_e2e_build ALL DEPENDS ${CALC_VM_E2E})
add_test(NAME calc_vm_e2e COMMAND ${CALC_VM_E2E})

# The falsifier: same sources, same test, the rewrite pass compiled OUT, so
# the pinned sequences must revert to their pre-rewrite form. Without it, pin
# (2) cannot tell "the rules fired" from "the tiler was always going to emit
# that", and the pass could be a no-op unnoticed.
set(CALC_VM_E2E_NOREWRITE ${CALC_VM_DIR}/calc_vm_e2e_norewrite)
add_custom_command(
    OUTPUT ${CALC_VM_E2E_NOREWRITE}
    COMMAND ${CLANG_CXX_COMPILER} -std=c++17 ${CALC_VM_LINK_SAN} ${CALC_VM_INCS}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_e2e_test.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_runner.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/Parser.cpp
            ${CALC_INTERP_O} ${CALC_OPTAB_O} ${CALC_VM_HOST_O} -lm
            -o ${CALC_VM_E2E_NOREWRITE}
    DEPENDS ${CALC_INTERP_O} ${CALC_OPTAB_O} calc_runner ${CALC_REWRITE_H} ${CALC_VM_RUNTIME_HDRS}
            ${CALC_VM_HOST_O}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_e2e_test.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_runner.cpp
    COMMENT "clang: calc e2e with the rewrite pass compiled out (falsifier)"
    VERBATIM
)
add_custom_target(calc_vm_e2e_norewrite_build ALL DEPENDS ${CALC_VM_E2E_NOREWRITE})
add_test(NAME calc_vm_e2e_norewrite COMMAND ${CALC_VM_E2E_NOREWRITE})

# Tier 0 == Tier 1 — the same bytecode on interp and JIT must agree.
set(CALC_VM_JIT ${CALC_VM_DIR}/calc_vm_jit)
add_custom_command(
    OUTPUT ${CALC_VM_JIT}
    COMMAND ${CLANG_C_COMPILER} -std=c11 -O2 ${CALC_VM_INCS}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_vm_test.c
            ${CALC_INTERP_O} ${CALC_VM_HOST_O} -lm -o ${CALC_VM_JIT}
    DEPENDS ${CALC_INTERP_O} ${CALC_VM_STENCIL_TABLE} ${CALC_VM_HOST_O}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_vm_test.c
    COMMENT "clang: calc VM interp==JIT harness"
    VERBATIM
)
add_custom_target(calc_vm_jit_build ALL DEPENDS ${CALC_VM_JIT})
add_test(NAME calc_vm_jit COMMAND ${CALC_VM_JIT})

# Tier 2 — the stack cache. The two harnesses above compare the tiers' ANSWERS,
# which is exactly what a cache does not change, so neither can tell a tiled run
# from one that quietly fell back to state 0. This one asks about the tiling: the
# published tables, the driver's state contract, every program at every cache
# depth, and the counts that say the cache was used at all.
#
# It compiles calc_vm_host.c's includes itself instead of linking the object,
# because the cache, the state walk and the statistics are file-scope to whichever
# translation unit includes calc_jit_driver.h — a test that linked the object
# would be inspecting its own second copy of them.
set(CALC_VM_TIER2 ${CALC_VM_DIR}/calc_vm_tier2)
add_custom_command(
    OUTPUT ${CALC_VM_TIER2}
    COMMAND ${CLANG_C_COMPILER} -std=c11 -O2 ${CALC_VM_INCS}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_tier2_test.c
            ${CALC_INTERP_O} -lm -o ${CALC_VM_TIER2}
    DEPENDS ${CALC_INTERP_O} ${CALC_VM_STENCIL_TABLE} ${CALC_VM_RUNTIME_HDRS}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_tier2_test.c
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_host.h
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_jit_driver.h
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_natives.h
    COMMENT "clang: calc tier-2 stack-cache harness"
    VERBATIM
)
add_custom_target(calc_vm_tier2_build ALL DEPENDS ${CALC_VM_TIER2})
add_test(NAME calc_vm_tier2 COMMAND ${CALC_VM_TIER2})
