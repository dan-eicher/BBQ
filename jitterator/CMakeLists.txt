# Jitterator — Copy-and-Patch stencil extractor

add_executable(jitterator
    src/jitterator.cpp
    extract/elf64Parser.cpp
)
target_include_directories(jitterator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extract
    ${CMAKE_SOURCE_DIR}/backends/cpp/runtime  # BBQContext.h
)

# --- Calculator proof-of-life example ---
add_executable(calc_example
    example/calc.cpp
)
target_include_directories(calc_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/example
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
)

# --- Calculator JIT (full toolchain demo: pegc + asdl + burgc + jitterator) ---

# ASDL JSON sidecar for calc_ir.asdl, consumed by burgc -asdl.
set(CALC_IR_JSON ${CMAKE_CURRENT_BINARY_DIR}/calc_ir.json)
add_custom_command(
    OUTPUT ${CALC_IR_JSON}
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc_ir.asdl
                 --json ${CALC_IR_JSON}
    DEPENDS asdl ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc_ir.asdl
    COMMENT "asdl --json: calc_ir.asdl -> calc_ir.json"
    VERBATIM
)

# End-to-end pipeline check: run burgc completeness analysis on
# calc.burg with the ASDL JSON. Demonstrates the asdl→burgc flow.
# calc is slot-based, so the schema falls back to heuristic for
# multi-arity terminals; we don't pass --strict here for that
# reason. AST-shaped IRs (yoctojc/sir.asdl) get precise filtering.
add_custom_target(calc_burg_check
    COMMAND burgc -i ${CMAKE_CURRENT_SOURCE_DIR}/example/calc.burg
                  -asdl ${CALC_IR_JSON}
                  -frames ${CMAKE_SOURCE_DIR}/burgc/frames
                  -o ${CMAKE_CURRENT_BINARY_DIR}/CalcMatcher.gen.h
    DEPENDS burgc ${CMAKE_CURRENT_SOURCE_DIR}/example/calc.burg ${CALC_IR_JSON}
    COMMENT "burgc analysis: calc.burg with calc_ir.json"
    VERBATIM
)

add_executable(calc_jit
    example/calc_jit.cpp
    example/frontend/Parser.cpp
)
target_include_directories(calc_jit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/example
    ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_SOURCE_DIR}/pegc/runtime
)
add_dependencies(calc_jit calc_burg_check)
