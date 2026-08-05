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
# source so calc_jit.cpp can include them directly; CMake regens them
# whenever the .asdl changes.
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
set(CALC_VM_GEN
    ${CALC_VM_DIR}/gen_interp.c   ${CALC_VM_DIR}/gen_interp.h
    ${CALC_VM_DIR}/runtime_api.h  ${CALC_VM_DIR}/opcodes.h
    ${CALC_VM_DIR}/opcode_tables.c ${CALC_VM_DIR}/calc_stencils.c
    ${CALC_VM_DIR}/calc_jit_meta.h ${CALC_VM_DIR}/calc_jit_symbols.h
    ${CALC_VM_DIR}/calc_valtype.h  ${CALC_VM_DIR}/calc_type_meta.h)
add_custom_command(
    OUTPUT ${CALC_VM_GEN}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CALC_VM_DIR}
    COMMAND opgen -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.def -prefix calc -o ${CALC_VM_DIR}
    DEPENDS opgen
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.def
            ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_backend.h
    COMMENT "opgen: calc.def -> two-tier calc VM"
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
    ${CALC_AST_HEADER}
    ${CALC_IR_HEADER}
    ${CALC_COMPILE_H}
    ${CMAKE_CURRENT_BINARY_DIR}/CalcMatcher.h
    ${CALC_VM_DIR}/opcodes.h
)
# CALC_EQSAT is deliberately NOT defined. The rewrite pass is written and its
# generated matcher builds, but the seam it would run at receives IR whose
# binop operands are already LoadLocals — the DDCG has assigned destinations
# by then — so an e-graph built there cannot see a constant and no axiom can
# fire. Turning this on requires the binding structure the DDCG records, which
# calc does not yet record. The e2e's pinned sequences assert the pre-rewrite
# form; they are what must change when the pass can actually run.
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
# the calc frontend (calc_runner.h / calc_lower.h / CalcIR.h).
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

# Tier 0 — the threaded interpreter object (clang: musttail).
set(CALC_INTERP_O ${CALC_VM_DIR}/gen_interp.o)
add_custom_command(
    OUTPUT ${CALC_INTERP_O}
    COMMAND ${CLANG_C_COMPILER} -std=c11 -O2 ${CALC_VM_INCS}
            -c ${CALC_VM_DIR}/gen_interp.c -o ${CALC_INTERP_O}
    DEPENDS ${CALC_VM_GEN}
    COMMENT "clang: calc interpreter (gen_interp.c)"
    VERBATIM
)

# Tier 1 — the copy-and-patch stencils → jitterator → stencil table.
set(CALC_VM_STENCILS_O ${CALC_VM_DIR}/calc_stencils.o)
add_custom_command(
    OUTPUT ${CALC_VM_STENCILS_O}
    COMMAND ${CLANG_C_COMPILER} -std=c11 -O2 -fno-stack-protector
            -fno-asynchronous-unwind-tables ${CALC_VM_INCS}
            -c ${CALC_VM_DIR}/calc_stencils.c -o ${CALC_VM_STENCILS_O}
    DEPENDS ${CALC_VM_GEN}
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

# Tier 0 e2e — real parsed programs (calc_compile) → cg_jump lowering
# → the generated interpreter.
set(CALC_VM_E2E ${CALC_VM_DIR}/calc_vm_e2e)
add_custom_command(
    OUTPUT ${CALC_VM_E2E}
    COMMAND ${CLANG_CXX_COMPILER} -std=c++17 ${CALC_VM_INCS}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_e2e_test.cpp
            $<TARGET_FILE:calc_runner> ${CALC_INTERP_O}
            ${CALC_VM_DIR}/opcode_tables.c -lm -o ${CALC_VM_E2E}
    DEPENDS ${CALC_INTERP_O} calc_runner ${CALC_REWRITE_H}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_e2e_test.cpp
    COMMENT "clang: calc VM e2e (parse -> ddcg -> IR -> bytecode -> interp)"
    VERBATIM
)
add_custom_target(calc_vm_e2e_build ALL DEPENDS ${CALC_VM_E2E})
add_test(NAME calc_vm_e2e COMMAND ${CALC_VM_E2E})

# The falsifier target belongs here — same sources, the rewrite pass compiled
# out, so the pinned sequences must revert. It is absent while CALC_EQSAT is
# never defined, because a second target building the identical binary proves
# nothing. It comes back with the knob.

# Tier 0 == Tier 1 — the same bytecode on interp and JIT must agree.
set(CALC_VM_JIT ${CALC_VM_DIR}/calc_vm_jit)
add_custom_command(
    OUTPUT ${CALC_VM_JIT}
    COMMAND ${CLANG_C_COMPILER} -std=c11 -O2 ${CALC_VM_INCS}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_vm_test.c
            ${CALC_INTERP_O} -lm -o ${CALC_VM_JIT}
    DEPENDS ${CALC_INTERP_O} ${CALC_VM_STENCIL_TABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/test/calc_vm_test.c
    COMMENT "clang: calc VM interp==JIT harness"
    VERBATIM
)
add_custom_target(calc_vm_jit_build ALL DEPENDS ${CALC_VM_JIT})
add_test(NAME calc_vm_jit COMMAND ${CALC_VM_JIT})
